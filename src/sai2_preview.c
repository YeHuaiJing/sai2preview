/* SPDX-License-Identifier: MIT */

/*
 * sai2_preview.c - PaintTool SAI 2预览图提取的C实现。
 *
 * 格式说明，已用真实的2480x3508 .sai2文件核对
 *
 *   偏移0        为char[16] "SAI-CANVAS-TYPE0"
 *   偏移16       为u8 flags0
 *   偏移17       为u8 canvas_background_flags（低3位，为0表示透明预览）
 *   偏移18       为u8 flags2
 *   偏移19       为u8 flags3
 *   偏移20       为u32画布宽度
 *   偏移24       为u32画布高度
 *   偏移28       为u32打印分辨率
 *   偏移32       为u32块表项数量
 *   偏移36       为u32选中的图层
 *   偏移40       为u64未知
 *   偏移48       为u64未知
 *   偏移56       为u32背景色（BGRA字节）
 *   偏移60       为char[4]图层效果（"norm"、"vivid"等）
 *   偏移64       起为块表项，每项16字节，依次是u32 tag、u32图层ID、u64偏移
 *
 * "thum"块
 *   u32缩略图宽度、u32缩略图高度、char[4] "jssf"、
 *   u16 jpeg宽度、u16 jpeg高度、u16通道数、
 *   u8亮度量化表[64]、[u8色度量化表[64]]、
 *   然后是ceil(高度/8)行，每行由u16字节数和熵编码字节组成
 *
 * "intg"块
 *   char[4] "dpcm"、u32 tile_size[tiles_x*tiles_y]、
 *   每个tile有u16校验和以及若干行bit-RLE delta数据、
 *   每个tile行末尾有u16校验和
 *
 * 部分JSSF/DPCM解析代码改编自Wunkolo/libsai（MIT许可证）。
 * 详见THIRD_PARTY_NOTICES.md和LICENSES/libsai-MIT.txt。
 */

#include "sai2_preview.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAI2_HEADER_SIZE 64u
#define SAI2_ENTRY_SIZE 16u

/* ------------------------------------------------------------------------- */
/* 平台相关的辅助函数                                                        */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef int64_t sai_off_t;

/* Windows上的fopen()按ANSI代码页打开文件，这里先把UTF-8路径转成UTF-16，
 * 让含中文或非ASCII字符的路径也能用。 */
static FILE *sai_fopen(const char *path, const char *mode) {
    wchar_t *wide;
    int length;
    FILE *file;

    length = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (length <= 0)
        return NULL;

    wide = (wchar_t *)malloc((size_t)length * sizeof(wchar_t));
    if (!wide)
        return NULL;

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, length) <= 0) {
        free(wide);
        return NULL;
    }

    file = _wfopen(wide, mode[0] == 'w' ? L"wb" : L"rb");
    free(wide);
    return file;
}

#define sai_fseek _fseeki64
#define sai_ftell _ftelli64
#else
typedef long sai_off_t;
#define sai_fopen fopen
#define sai_fseek fseek
#define sai_ftell ftell
#endif

/* ------------------------------------------------------------------------- */
/* 小端读取函数                                                              */
/* ------------------------------------------------------------------------- */

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

/* ------------------------------------------------------------------------- */
/* 块表                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct chunk_ref {
    uint64_t offset;
    uint64_t size;
    uint32_t layer_id;
} chunk_ref;

/*
 * 按4字节标签定位块。
 * 找到返回0，没找到返回1，出错返回负的sai2_status值。
 */
static int find_chunk(const uint8_t *data, size_t size, const char tag[4],
                      chunk_ref *out) {
    uint32_t count;
    uint32_t i;
    uint64_t target_offset = 0;
    uint64_t end_offset;
    int found = 0;
    uint32_t layer_id = 0;

    if (!data || !out)
        return SAI2_ERR_INVALID_ARG;
    if (size < SAI2_HEADER_SIZE)
        return SAI2_ERR_TRUNCATED;
    if (memcmp(data, "SAI-CANVAS-TYPE0", 16) != 0)
        return SAI2_ERR_NOT_SAI2;

    count = rd32le(data + 32);
    if ((uint64_t)count > ((uint64_t)size - SAI2_HEADER_SIZE) / SAI2_ENTRY_SIZE)
        return SAI2_ERR_TRUNCATED;

    for (i = 0; i < count; ++i) {
        const uint8_t *entry =
            data + SAI2_HEADER_SIZE + (size_t)i * SAI2_ENTRY_SIZE;
        if (memcmp(entry, tag, 4) == 0 && !found) {
            target_offset = rd64le(entry + 8);
            layer_id = rd32le(entry + 4);
            found = 1;
        }
    }

    if (!found)
        return 1;

    if (target_offset >= size)
        return SAI2_ERR_TRUNCATED;

    /* 块的长度一直延伸到文件中下一个块的偏移，或者文件末尾。 */
    end_offset = size;
    for (i = 0; i < count; ++i) {
        uint64_t offset =
            rd64le(data + SAI2_HEADER_SIZE + (size_t)i * SAI2_ENTRY_SIZE + 8);
        if (offset > target_offset && offset < end_offset)
            end_offset = offset;
    }
    if (end_offset > size)
        end_offset = size;
    if (end_offset <= target_offset)
        return SAI2_ERR_CORRUPT;

    out->offset = target_offset;
    out->size = end_offset - target_offset;
    out->layer_id = layer_id;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 对外信息接口                                                              */
/* ------------------------------------------------------------------------- */

sai2_status sai2_get_info(const void *file_data, size_t file_size,
                          sai2_canvas_info *out_info) {
    const uint8_t *d = (const uint8_t *)file_data;
    uint32_t count;

    if (!d || !out_info)
        return SAI2_ERR_INVALID_ARG;
    if (file_size < SAI2_HEADER_SIZE)
        return SAI2_ERR_TRUNCATED;
    if (memcmp(d, "SAI-CANVAS-TYPE0", 16) != 0)
        return SAI2_ERR_NOT_SAI2;

    count = rd32le(d + 32);
    if ((uint64_t)count >
        ((uint64_t)file_size - SAI2_HEADER_SIZE) / SAI2_ENTRY_SIZE)
        return SAI2_ERR_TRUNCATED;

    out_info->flags0 = d[16];
    out_info->background_flags = d[17];
    out_info->width = rd32le(d + 20);
    out_info->height = rd32le(d + 24);
    out_info->table_count = count;
    out_info->selected_layer = rd32le(d + 36);
    out_info->background_color = rd32le(d + 56);
    out_info->layer_effect = rd32le(d + 60);

    return SAI2_OK;
}

/* ------------------------------------------------------------------------- */
/* 可增长的字节缓冲区                                                        */
/* ------------------------------------------------------------------------- */

typedef struct byte_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} byte_buf;

static int buf_reserve(byte_buf *b, size_t extra) {
    size_t need;
    size_t cap;

    if (extra > SIZE_MAX - b->len)
        return 0;
    need = b->len + extra;
    if (need <= b->cap)
        return 1;

    cap = b->cap ? b->cap : 1024;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }

    {
        uint8_t *p = (uint8_t *)realloc(b->data, cap);
        if (!p)
            return 0;
        b->data = p;
        b->cap = cap;
    }
    return 1;
}

static int buf_put(byte_buf *b, const void *src, size_t n) {
    if (!buf_reserve(b, n))
        return 0;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 1;
}

static int buf_u8(byte_buf *b, uint8_t v) { return buf_put(b, &v, 1); }

static int buf_u16be(byte_buf *b, uint16_t v) {
    uint8_t t[2];
    t[0] = (uint8_t)(v >> 8);
    t[1] = (uint8_t)v;
    return buf_put(b, t, 2);
}

/* ------------------------------------------------------------------------- */
/* JSSF转baseline JPEG                                                       */
/* ------------------------------------------------------------------------- */

/* 标准JPEG Annex K霍夫曼表的载荷数据。 */
static const uint8_t k_huff_dc_luma[29] = {
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};

static const uint8_t k_huff_ac_luma[179] = {
    0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05,
    0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1,
    0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19,
    0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA,
    0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
    0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA};

static const uint8_t k_huff_dc_chroma[29] = {
    0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
    0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};

static const uint8_t k_huff_ac_chroma[179] = {
    0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04,
    0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05,
    0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52,
    0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1,
    0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
    0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2,
    0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5,
    0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
    0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA};

sai2_status sai2_extract_jpeg(const void *file_data, size_t file_size,
                              sai2_image *out_image) {
    const uint8_t *d = (const uint8_t *)file_data;
    chunk_ref chunk;
    const uint8_t *p;
    size_t left;
    uint16_t jw, jh, channels;
    const uint8_t *luma_quant;
    const uint8_t *chroma_quant = NULL;
    uint32_t rows;
    uint32_t r;
    size_t scan_total = 0;
    byte_buf out = {0, 0, 0};
    int rc;

    if (!d || !out_image)
        return SAI2_ERR_INVALID_ARG;
    memset(out_image, 0, sizeof(*out_image));

    rc = find_chunk(d, file_size, "thum", &chunk);
    if (rc == 1)
        return SAI2_ERR_NO_THUMBNAIL;
    if (rc < 0)
        return (sai2_status)rc;

    p = d + chunk.offset;
    left = (size_t)chunk.size;

    /* u32宽度、u32高度、"jssf"（画布预览尺寸） */
    if (left < 8)
        return SAI2_ERR_TRUNCATED;
    p += 8;
    left -= 8;
    if (memcmp(p, "jssf", 4) != 0)
        return SAI2_ERR_CORRUPT;
    p += 4;
    left -= 4;

    /* u16宽度、u16高度、u16通道数 */
    if (left < 6)
        return SAI2_ERR_TRUNCATED;
    jw = rd16le(p);
    jh = rd16le(p + 2);
    channels = rd16le(p + 4);
    p += 6;
    left -= 6;

    if (jw == 0 || jh == 0 || channels == 0 || channels > 4)
        return SAI2_ERR_UNSUPPORTED;

    if (left < 64)
        return SAI2_ERR_TRUNCATED;
    luma_quant = p;
    p += 64;
    left -= 64;
    if (channels > 1) {
        if (left < 64)
            return SAI2_ERR_TRUNCATED;
        chroma_quant = p;
        p += 64;
        left -= 64;
    }

    /* 校验每8行像素一段的扫描表，并统计熵编码字节总数。 */
    rows = (jh + 7u) / 8u;
    {
        const uint8_t *s = p;
        size_t sl = left;
        for (r = 0; r < rows; ++r) {
            uint16_t n;
            if (sl < 2)
                return SAI2_ERR_TRUNCATED;
            n = rd16le(s);
            s += 2;
            sl -= 2;
            if (n > sl)
                return SAI2_ERR_TRUNCATED;
            s += n;
            sl -= n;
            scan_total += n;
        }
    }
    if (scan_total > SIZE_MAX - 4096)
        return SAI2_ERR_NOMEM;

    /* SOI标记 */
    if (!buf_u16be(&out, 0xFFD8))
        goto oom;

    /* DQT量化表 */
    {
        uint16_t body = (uint16_t)(65 + (channels > 1 ? 65 : 0));
        if (!buf_u16be(&out, 0xFFDB) || !buf_u16be(&out, (uint16_t)(2 + body)))
            goto oom;
        if (!buf_u8(&out, 0x00) || !buf_put(&out, luma_quant, 64))
            goto oom;
        if (channels > 1) {
            if (!buf_u8(&out, 0x01) || !buf_put(&out, chroma_quant, 64))
                goto oom;
        }
    }

    /* DHT霍夫曼表 */
    {
        uint16_t body =
            (uint16_t)(sizeof(k_huff_dc_luma) + sizeof(k_huff_ac_luma) +
                       (channels > 1 ? sizeof(k_huff_dc_chroma) +
                                           sizeof(k_huff_ac_chroma)
                                     : 0));
        if (!buf_u16be(&out, 0xFFC4) || !buf_u16be(&out, (uint16_t)(2 + body)))
            goto oom;
        if (!buf_put(&out, k_huff_dc_luma, sizeof(k_huff_dc_luma)) ||
            !buf_put(&out, k_huff_ac_luma, sizeof(k_huff_ac_luma)))
            goto oom;
        if (channels > 1) {
            if (!buf_put(&out, k_huff_dc_chroma, sizeof(k_huff_dc_chroma)) ||
                !buf_put(&out, k_huff_ac_chroma, sizeof(k_huff_ac_chroma)))
                goto oom;
        }
    }

    /* SOF0帧头 */
    {
        uint16_t i;
        if (!buf_u16be(&out, 0xFFC0) ||
            !buf_u16be(&out, (uint16_t)(8 + channels * 3)) ||
            !buf_u8(&out, 8) || !buf_u16be(&out, jh) || !buf_u16be(&out, jw) ||
            !buf_u8(&out, (uint8_t)channels))
            goto oom;
        for (i = 0; i < channels; ++i) {
            if (!buf_u8(&out, (uint8_t)(i + 1)) || !buf_u8(&out, 0x11) ||
                !buf_u8(&out, (uint8_t)(i != 0 ? 1 : 0)))
                goto oom;
        }
    }

    /* DRI，每个8像素高的MCU行使用一个重启间隔 */
    if (!buf_u16be(&out, 0xFFDD) || !buf_u16be(&out, 4) ||
        !buf_u16be(&out, (uint16_t)((jw + 7u) / 8u)))
        goto oom;

    /* SOS扫描头 */
    {
        uint16_t i;
        if (!buf_u16be(&out, 0xFFDA) ||
            !buf_u16be(&out, (uint16_t)(6 + channels * 2)) ||
            !buf_u8(&out, (uint8_t)channels))
            goto oom;
        for (i = 0; i < channels; ++i) {
            if (!buf_u8(&out, (uint8_t)(i + 1)) ||
                !buf_u8(&out, (uint8_t)(i == 0 ? 0x00 : 0x11)))
                goto oom;
        }
        if (!buf_u8(&out, 0x00) || !buf_u8(&out, 0x3F) || !buf_u8(&out, 0x00))
            goto oom;
    }

    /* 熵编码扫描数据，每个重启间隔对应一行MCU。 */
    {
        const uint8_t *s = p;
        size_t sl = left;
        for (r = 0; r < rows; ++r) {
            uint16_t n = rd16le(s);
            s += 2;
            sl -= 2;
            if (!buf_put(&out, s, n))
                goto oom;
            s += n;
            sl -= n;
            if (r + 1 < rows) {
                if (!buf_u16be(&out, (uint16_t)(0xFFD0 | (r & 7u))))
                    goto oom;
            }
        }
    }

    /* EOI结束标记 */
    if (!buf_u16be(&out, 0xFFD9))
        goto oom;

    out_image->data = out.data;
    out_image->size = out.len;
    out_image->width = jw;
    out_image->height = jh;
    return SAI2_OK;

oom:
    free(out.data);
    return SAI2_ERR_NOMEM;
}

/* ------------------------------------------------------------------------- */
/* DPCM（delta加bit-RLE）解码器                                             */
/* ------------------------------------------------------------------------- */

typedef struct byte_span {
    const uint8_t *p;
    size_t n;
} byte_span;

static int span_u16(byte_span *s, uint16_t *v) {
    if (s->n < 2)
        return 0;
    *v = rd16le(s->p);
    s->p += 2;
    s->n -= 2;
    return 1;
}

static uint32_t ctz64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctzll(v);
#else
    uint32_t n = 0;
    while ((v & 1u) == 0) {
        v >>= 1;
        ++n;
    }
    return n;
#endif
}

/*
 * 解包一个通道的16位delta数据流。
 *
 * 位流按低位优先读取，规则来自SAI编码器
 *   连续读出值为0的位，记个数为n
 *   再读1位b，得到opcode = 2*n + b
 *   opcode为0时，输出一个字面量0
 *   opcode为1到14，记为k，再读k+1位，最高位是符号，
 *   低k位是幅值后缀，值为正负((1<<k | suffix) - 1)
 *   opcode为15时，读7位，输出(count + 8)个0
 *
 * 返回消耗的字节数。末尾不足一字节的位会被丢弃，因为SAI写入时
 * 每个通道都从字节边界开始。
 */
static sai2_status unpack_delta_rle16(const uint8_t *compressed,
                                      size_t compressed_size, int16_t *out,
                                      uint32_t pixel_count,
                                      uint8_t out_channels, uint8_t in_channels,
                                      size_t *consumed) {
    byte_span s;
    uint64_t mask = 0;
    uint32_t remaining = 0;
    uint8_t channel;

    s.p = compressed;
    s.n = compressed_size;

    for (channel = 0; channel < in_channels; ++channel) {
        uint32_t written = 0;

        while (written < pixel_count) {
            uint32_t first;
            uint64_t next;
            uint32_t opcode;

            /* 向缓冲区补充最多32位的小端数据。 */
            while (remaining < 32 && s.n > 0) {
                uint64_t word = 0;
                uint32_t got = 0;

                if (s.n >= 4) {
                    word = rd32le(s.p);
                    s.p += 4;
                    s.n -= 4;
                    got = 32;
                } else if (s.n >= 2) {
                    word = rd16le(s.p);
                    s.p += 2;
                    s.n -= 2;
                    got = 16;
                } else {
                    word = s.p[0];
                    s.p += 1;
                    s.n -= 1;
                    got = 8;
                }
                mask |= word << remaining;
                remaining += got;
            }

            if (mask == 0)
                return SAI2_ERR_CORRUPT;

            first = ctz64(mask);
            if (first >= 32)
                return SAI2_ERR_CORRUPT;
            next = mask >> (first + 1u);
            opcode = (first * 2u) | (uint32_t)(next & 1u);
            if (opcode > 0x0F || remaining < 2u + first)
                return SAI2_ERR_CORRUPT;
            remaining -= 2u + first;
            mask = next >> 1u;

            if (opcode == 0) {
                out[(size_t)written * out_channels + channel] = 0;
                written += 1;
            } else if (opcode <= 0x0E) {
                int32_t active;
                uint32_t value_bits;
                uint64_t value_mask;
                uint64_t value;
                int32_t value_mag;
                int16_t result;

                if (remaining < opcode + 1u)
                    return SAI2_ERR_CORRUPT;

                active = (mask & ((uint64_t)1 << opcode)) ? 1 : 0;
                value_bits = opcode;
                value_mask = ((uint64_t)1 << opcode) - 1u;
                value = mask & value_mask;
                value_mag =
                    (int32_t)((((uint64_t)1 << value_bits) | value) - 1u);
                result = (int16_t)(active ? -value_mag : value_mag);

                remaining -= opcode + 1u;
                mask >>= opcode + 1u;

                out[(size_t)written * out_channels + channel] = result;
                written += 1;
            } else {
                uint32_t zeros;
                uint32_t i;

                if (remaining < 7)
                    return SAI2_ERR_CORRUPT;

                zeros = (uint32_t)(mask & 0x7Fu) + 8u;
                remaining -= 7u;
                mask >>= 7u;

                if (zeros > pixel_count - written)
                    return SAI2_ERR_CORRUPT;

                for (i = 0; i < zeros; ++i)
                    out[(size_t)(written + i) * out_channels + channel] = 0;
                written += zeros;
            }
        }
    }

    /* 流里没有的通道（alpha）补0。 */
    if (in_channels < out_channels) {
        uint32_t i;
        for (channel = in_channels; channel < out_channels; ++channel)
            for (i = 0; i < pixel_count; ++i)
                out[(size_t)i * out_channels + channel] = 0;
    }

    {
        size_t total_read = compressed_size - s.n;
        size_t remaining_bytes = remaining / 8u;
        if (remaining_bytes > total_read)
            return SAI2_ERR_CORRUPT;
        *consumed = total_read - remaining_bytes;
    }
    return SAI2_OK;
}

/*
 * 用上一行已解码的像素作为预测值，把16位delta还原成一行8位BGRA，
 * 算术规则与SAI编码器一致。
 */
static void delta_unpack_row(uint8_t *dest, const uint8_t *previous_row,
                             const int16_t *delta, uint32_t count) {
    uint16_t sum[4] = {0, 0, 0, 0};
    uint16_t prev16[4] = {0, 0, 0, 0};
    uint32_t i;
    uint8_t c;

    for (i = 0; i < count; ++i) {
        uint16_t cur_prev[4];

        for (c = 0; c < 4; ++c)
            cur_prev[c] = previous_row[(size_t)i * 4u + c];

        for (c = 0; c < 4; ++c) {
            uint32_t a = (uint32_t)sum[c] + cur_prev[c];
            uint16_t a16 = (uint16_t)a; /* 回绕加法 */
            uint16_t b;
            uint16_t cc;
            uint16_t d;
            uint16_t e;

            b = (a16 < prev16[c]) ? 0 : (uint16_t)(a16 - prev16[c]);

            if ((uint32_t)b + 0xFF00u > 0xFFFFu)
                cc = 0xFFFFu;
            else
                cc = (uint16_t)((uint32_t)b + 0xFF00u);

            d = (cc < 0xFF00u) ? 0 : (uint16_t)(cc - 0xFF00u);
            e = (uint16_t)(d + (uint16_t)delta[(size_t)i * 4u + c]);

            /* B、G、R、A字节顺序。 */
            dest[(size_t)i * 4u + c] = (uint8_t)(e > 255u ? 255u : e);
            sum[c] = e;
            prev16[c] = cur_prev[c];
        }
    }
}

sai2_status sai2_extract_pixels(const void *file_data, size_t file_size,
                                sai2_pixels *out_pixels) {
    const uint8_t *d = (const uint8_t *)file_data;
    sai2_canvas_info info;
    chunk_ref chunk;
    byte_span body;
    uint32_t tiles_x, tiles_y, tiles_count;
    uint32_t *tile_sizes = NULL;
    uint8_t *image = NULL;
    uint32_t channels;
    size_t image_size;
    uint32_t ty;
    int rc;

    if (!d || !out_pixels)
        return SAI2_ERR_INVALID_ARG;
    memset(out_pixels, 0, sizeof(*out_pixels));

    rc = sai2_get_info(d, file_size, &info);
    if (rc != SAI2_OK)
        return (sai2_status)rc;

    rc = find_chunk(d, file_size, "intg", &chunk);
    if (rc == 1)
        return SAI2_ERR_NO_THUMBNAIL;
    if (rc < 0)
        return (sai2_status)rc;

    if (chunk.size < 4 || memcmp(d + chunk.offset, "dpcm", 4) != 0)
        return SAI2_ERR_CORRUPT;

    body.p = d + chunk.offset + 4;
    body.n = (size_t)chunk.size - 4;

    if (info.width == 0 || info.height == 0)
        return SAI2_ERR_CORRUPT;
    if (info.width > (1u << 20) || info.height > (1u << 20))
        return SAI2_ERR_UNSUPPORTED;

    tiles_x = (info.width + 255u) / 256u;
    tiles_y = (info.height + 255u) / 256u;
    if (tiles_x == 0 || tiles_y == 0)
        return SAI2_ERR_CORRUPT;
    if ((uint64_t)tiles_x * tiles_y > (uint64_t)1 << 20)
        return SAI2_ERR_UNSUPPORTED;
    tiles_count = tiles_x * tiles_y;

    if ((uint64_t)body.n < (uint64_t)tiles_count * 4u)
        return SAI2_ERR_TRUNCATED;

    tile_sizes = (uint32_t *)malloc((size_t)tiles_count * sizeof(uint32_t));
    if (!tile_sizes)
        return SAI2_ERR_NOMEM;

    {
        uint32_t i;
        for (i = 0; i < tiles_count; ++i) {
            tile_sizes[i] = rd32le(body.p);
            body.p += 4;
            body.n -= 4;
        }
    }

    if ((uint64_t)info.width * info.height > SIZE_MAX / 4u) {
        free(tile_sizes);
        return SAI2_ERR_NOMEM;
    }
    image_size = (size_t)info.width * info.height * 4u;
    image = (uint8_t *)calloc(1, image_size);
    if (!image) {
        free(tile_sizes);
        return SAI2_ERR_NOMEM;
    }

    channels = ((info.background_flags & 7u) == 0) ? 4u : 3u;

    for (ty = 0; ty < tiles_y; ++ty) {
        uint32_t tile_beg_y = ty * 256u;
        uint32_t tile_end_y = tile_beg_y + 256u;
        uint32_t tile_size_y;
        uint32_t tx;

        if (tile_end_y > info.height)
            tile_end_y = info.height;
        tile_size_y = tile_end_y - tile_beg_y;

        {
            uint8_t composite_row[256 * 4];
            memset(composite_row, 0, sizeof(composite_row));

            for (tx = 0; tx < tiles_x; ++tx) {
                uint32_t tile_size = tile_sizes[(size_t)ty * tiles_x + tx];
                byte_span tile;
                uint16_t checksum;
                uint32_t tile_beg_x = tx * 256u;
                uint32_t tile_end_x = tile_beg_x + 256u;
                uint32_t tile_size_x;
                const uint8_t *previous_row = composite_row;
                uint32_t row;

                if (tile_end_x > info.width)
                    tile_end_x = info.width;
                tile_size_x = tile_end_x - tile_beg_x;

                if (tile_size > body.n) {
                    rc = SAI2_ERR_TRUNCATED;
                    goto fail;
                }
                tile.p = body.p;
                tile.n = tile_size;

                if (!span_u16(&tile, &checksum)) {
                    rc = SAI2_ERR_TRUNCATED;
                    goto fail;
                }
                (void)checksum;

                for (row = 0; row < tile_size_y; ++row) {
                    int16_t row_delta[256 * 4];
                    size_t consumed = 0;
                    uint8_t *dest_row =
                        image +
                        ((size_t)(tile_beg_y + row) * info.width + tile_beg_x) *
                            4u;

                    rc = unpack_delta_rle16(tile.p, tile.n, row_delta,
                                            tile_size_x, 4, (uint8_t)channels,
                                            &consumed);
                    if (rc != SAI2_OK)
                        goto fail;

                    delta_unpack_row(dest_row, previous_row, row_delta,
                                     tile_size_x);
                    previous_row = dest_row;

                    tile.p += consumed;
                    tile.n -= consumed;
                }

                body.p += tile_size;
                body.n -= tile_size;
            }
        }

        /* Per tile-row checksum (high byte == tiles_x). */
        {
            uint16_t checksum;
            if (!span_u16(&body, &checksum)) {
                rc = SAI2_ERR_TRUNCATED;
                goto fail;
            }
            (void)checksum;
        }
    }

    if (channels == 3) {
        size_t i;
        for (i = 0; i < (size_t)info.width * info.height; ++i)
            image[i * 4u + 3u] = 0xFFu;
    }

    free(tile_sizes);
    out_pixels->data = image;
    out_pixels->size = image_size;
    out_pixels->width = info.width;
    out_pixels->height = info.height;
    return SAI2_OK;

fail:
    free(tile_sizes);
    free(image);
    return (sai2_status)rc;
}

/* ------------------------------------------------------------------------- */
/* 高层缩略图接口                                                            */
/* ------------------------------------------------------------------------- */

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static sai2_status write_bmp_file(const sai2_thumbnail *thumb,
                                  const char *path) {
    FILE *f;
    uint8_t header[54];
    uint32_t image_size;

    if (!thumb->data || thumb->width == 0 || thumb->height == 0)
        return SAI2_ERR_INVALID_ARG;
    if ((uint64_t)thumb->width * thumb->height > UINT32_MAX / 4u)
        return SAI2_ERR_UNSUPPORTED;

    image_size = thumb->width * thumb->height * 4u;
    if (thumb->size < image_size)
        return SAI2_ERR_CORRUPT;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    write_le32(header + 2, 54u + image_size);
    write_le32(header + 10, 54u);
    write_le32(header + 14, 40u); /* BITMAPINFOHEADER */
    write_le32(header + 18, thumb->width);
    /* 高度取负值，表示行按从上到下存储。 */
    write_le32(header + 22, (uint32_t)(-(int32_t)thumb->height));
    write_le16(header + 26, 1u);  /* 平面数 */
    write_le16(header + 28, 32u); /* 每像素位数 */
    write_le32(header + 30, 0u);  /* BI_RGB，不压缩 */
    write_le32(header + 34, image_size);
    write_le32(header + 38, 2835u); /* 72 DPI */
    write_le32(header + 42, 2835u);

    f = sai_fopen(path, "wb");
    if (!f)
        return SAI2_ERR_IO;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(thumb->data, 1, image_size, f) != image_size) {
        fclose(f);
        return SAI2_ERR_IO;
    }
    fclose(f);
    return SAI2_OK;
}

sai2_status sai2_thumbnail_extract(const void *file_data, size_t file_size,
                                   sai2_thumbnail *out_thumbnail) {
    sai2_image jpeg;
    sai2_pixels pixels;
    sai2_status rc;

    if (!file_data || !out_thumbnail)
        return SAI2_ERR_INVALID_ARG;

    memset(out_thumbnail, 0, sizeof(*out_thumbnail));

    /* 优先走快速路径，取SAI保存的小尺寸有损预览。 */
    rc = sai2_extract_jpeg(file_data, file_size, &jpeg);
    if (rc == SAI2_OK) {
        out_thumbnail->format = SAI2_THUMBNAIL_JPEG;
        out_thumbnail->data = jpeg.data;
        out_thumbnail->size = jpeg.size;
        out_thumbnail->width = jpeg.width;
        out_thumbnail->height = jpeg.height;
        return SAI2_OK;
    }

    /* 回退路径，解码无损的整幅画布预览。 */
    rc = sai2_extract_pixels(file_data, file_size, &pixels);
    if (rc == SAI2_OK) {
        out_thumbnail->format = SAI2_THUMBNAIL_BGRA;
        out_thumbnail->data = pixels.data;
        out_thumbnail->size = pixels.size;
        out_thumbnail->width = pixels.width;
        out_thumbnail->height = pixels.height;
        return SAI2_OK;
    }

    return rc;
}

sai2_status sai2_thumbnail_write(const sai2_thumbnail *thumb,
                                 const char *path) {
    if (!thumb || !path)
        return SAI2_ERR_INVALID_ARG;

    switch (thumb->format) {
    case SAI2_THUMBNAIL_JPEG: {
        FILE *f;
        if (!thumb->data)
            return SAI2_ERR_INVALID_ARG;
        f = sai_fopen(path, "wb");
        if (!f)
            return SAI2_ERR_IO;
        if (fwrite(thumb->data, 1, thumb->size, f) != thumb->size) {
            fclose(f);
            return SAI2_ERR_IO;
        }
        fclose(f);
        return SAI2_OK;
    }
    case SAI2_THUMBNAIL_BGRA:
        return write_bmp_file(thumb, path);
    default:
        return SAI2_ERR_UNSUPPORTED;
    }
}

static sai2_status read_entire_file(const char *path, uint8_t **out_data,
                                    size_t *out_size) {
    FILE *f;
    sai_off_t size;
    uint8_t *data;

    if (!path || !out_data || !out_size)
        return SAI2_ERR_INVALID_ARG;

    f = sai_fopen(path, "rb");
    if (!f)
        return SAI2_ERR_IO;
    if (sai_fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return SAI2_ERR_IO;
    }
    size = sai_ftell(f);
    if (size <= 0 || sai_fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return SAI2_ERR_IO;
    }
    if ((uint64_t)size > (uint64_t)SIZE_MAX) {
        fclose(f);
        return SAI2_ERR_UNSUPPORTED;
    }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return SAI2_ERR_NOMEM;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return SAI2_ERR_IO;
    }
    fclose(f);

    *out_data = data;
    *out_size = (size_t)size;
    return SAI2_OK;
}

sai2_status sai2_thumbnail_export(const char *sai2_path,
                                  const char *output_path) {
    uint8_t *file_data = NULL;
    size_t file_size = 0;
    sai2_thumbnail thumb;
    sai2_status rc;

    if (!sai2_path || !output_path)
        return SAI2_ERR_INVALID_ARG;

    rc = read_entire_file(sai2_path, &file_data, &file_size);
    if (rc != SAI2_OK)
        return rc;

    rc = sai2_thumbnail_extract(file_data, file_size, &thumb);
    free(file_data);
    if (rc != SAI2_OK)
        return rc;

    rc = sai2_thumbnail_write(&thumb, output_path);
    sai2_thumbnail_free(&thumb);
    return rc;
}

void sai2_thumbnail_free(sai2_thumbnail *thumb) {
    if (!thumb)
        return;
    free(thumb->data);
    thumb->data = NULL;
    thumb->size = 0;
    thumb->width = 0;
    thumb->height = 0;
}

/* ------------------------------------------------------------------------- */
/* 清理与错误信息                                                            */
/* ------------------------------------------------------------------------- */

void sai2_image_free(sai2_image *image) {
    if (!image)
        return;
    free(image->data);
    image->data = NULL;
    image->size = 0;
    image->width = 0;
    image->height = 0;
}

void sai2_pixels_free(sai2_pixels *pixels) {
    if (!pixels)
        return;
    free(pixels->data);
    pixels->data = NULL;
    pixels->size = 0;
    pixels->width = 0;
    pixels->height = 0;
}

const char *sai2_status_string(sai2_status status) {
    switch (status) {
    case SAI2_OK:
        return "ok";
    case SAI2_ERR_INVALID_ARG:
        return "invalid argument";
    case SAI2_ERR_NOT_SAI2:
        return "not a SAI2 canvas";
    case SAI2_ERR_TRUNCATED:
        return "truncated file";
    case SAI2_ERR_NO_THUMBNAIL:
        return "no preview chunk";
    case SAI2_ERR_UNSUPPORTED:
        return "unsupported preview";
    case SAI2_ERR_CORRUPT:
        return "corrupt preview data";
    case SAI2_ERR_NOMEM:
        return "out of memory";
    case SAI2_ERR_IO:
        return "I/O error";
    default:
        return "unknown error";
    }
}
