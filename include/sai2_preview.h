/* SPDX-License-Identifier: MIT */

/*
 * sai2_preview.h - 从PaintTool SAI 2（.sai2）画布文件中提取预览图/缩略图。
 *
 * SAI2画布格式
 *   64字节文件头，以"SAI-CANVAS-TYPE0"开头
 *   一张块表，每项16字节，包含uint32 tag、uint32 layer_id和
 *   uint64绝对文件偏移
 *   块数据存放在这些偏移处
 *
 * 两个块携带预览图
 *   "thum" 有损缩略图，内容为"jssf"数据，本质是去掉容器、
 *          带量化表和每8像素行重启标记的JPEG码流
 *   "intg" 无损缩略图，内容为"dpcm"数据，按完整画布分辨率
 *          存储delta加bit-RLE压缩的BGRA像素
 *
 * 大画布（>= 512像素）通常带"thum" JPEG预览，小画布可能只有"intg"。
 *
 * sai2_preview.c中的代码改编自Wunkolo/libsai（MIT许可证），
 * 负责把JSSF容器还原成标准baseline JPEG。
 * 详见THIRD_PARTY_NOTICES.md和LICENSES/libsai-MIT.txt。
 * 霍夫曼表使用标准JPEG Annex K表。
 *
 * API约定
 *   所有输出缓冲区都由malloc分配，必须用对应的free函数释放
 *   （或者调用sai2_thumbnail_free()）。
 *   所有函数都不修改输入，也没有全局状态，是线程安全的。
 *
 * 快速上手，一次调用即可拿到可用的缩略图
 *
 *   sai2_thumbnail_export("input.sai2", "thumb.jpg"); // 或thumb.bmp
 *
 * 或者，文件内容已经在内存里时
 *
 *   sai2_thumbnail thumb;
 *   if (sai2_thumbnail_extract(file_data, file_size, &thumb) == SAI2_OK) {
 *       sai2_thumbnail_write(&thumb, "thumb.jpg"); // BGRA时用.bmp
 *       sai2_thumbnail_free(&thumb);
 *   }
 */

#ifndef SAI2_PREVIEW_H
#define SAI2_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sai2_status {
    SAI2_OK = 0,
    SAI2_ERR_INVALID_ARG = -1,
    SAI2_ERR_NOT_SAI2 = -2,
    SAI2_ERR_TRUNCATED = -3,
    SAI2_ERR_NO_THUMBNAIL = -4,
    SAI2_ERR_UNSUPPORTED = -5,
    SAI2_ERR_CORRUPT = -6,
    SAI2_ERR_NOMEM = -7,
    SAI2_ERR_IO = -8
} sai2_status;

typedef struct sai2_canvas_info {
    uint32_t width;
    uint32_t height;
    uint32_t table_count;
    uint32_t selected_layer;
    uint32_t background_color; /* 按文件原样存储的0xAARRGGBB（字节序为BGRA） */
    uint32_t layer_effect;     /* 4字符标签，例如"norm" */
    uint8_t background_flags;  /* 低3位，为0表示透明预览 */
    uint8_t flags0;
} sai2_canvas_info;

typedef struct sai2_image {
    uint8_t *data; /* 由malloc分配的标准baseline JPEG码流 */
    size_t size;
    uint32_t width;
    uint32_t height;
} sai2_image;

typedef struct sai2_pixels {
    uint8_t *data; /* 由malloc分配的BGRA像素（字节序B、G、R、A），行从上到下 */
    size_t size;   /* 值为width * height * 4 */
    uint32_t width;
    uint32_t height;
} sai2_pixels;

/* sai2_thumbnail所持数据的格式。 */
typedef enum sai2_thumbnail_format {
    SAI2_THUMBNAIL_JPEG = 1, /* data是完整的baseline JPEG码流 */
    SAI2_THUMBNAIL_BGRA = 2  /* data是width*height*4字节的BGRA数据 */
} sai2_thumbnail_format;

/*
 * 可直接使用的缩略图。只需要文件的“预览图”时，优先用它，
 * 而不是底层的提取函数。
 */
typedef struct sai2_thumbnail {
    sai2_thumbnail_format format;
    uint8_t *data; /* 由malloc分配，用sai2_thumbnail_free()释放 */
    size_t size;
    uint32_t width;
    uint32_t height;
} sai2_thumbnail;

/*
 * 一次调用提取可用的最佳缩略图
 *   先尝试小尺寸的"thum"块，返回JPEG（format为JPEG）
 *   否则解码"intg"块，返回画布原始尺寸的BGRA
 * 成功返回SAI2_OK。
 */
sai2_status sai2_thumbnail_extract(const void *file_data, size_t file_size,
                                   sai2_thumbnail *out_thumbnail);

/*
 * 按thumb->format决定输出格式写盘
 *   JPEG写JPEG文件，BGRA写32位BMP文件
 * 成功返回SAI2_OK。
 */
sai2_status sai2_thumbnail_write(const sai2_thumbnail *thumb, const char *path);

/*
 * 文件到文件的便捷接口
 * 读取sai2_path，提取最佳预览，写到output_path。
 * 有"thum"块时输出JPEG，否则输出BMP。
 * 成功返回SAI2_OK。
 */
sai2_status sai2_thumbnail_export(const char *sai2_path,
                                  const char *output_path);

void sai2_thumbnail_free(sai2_thumbnail *thumb);

/*
 * 解析画布文件头和块表。
 * 成功返回SAI2_OK。
 */
sai2_status sai2_get_info(const void *file_data, size_t file_size,
                          sai2_canvas_info *out_info);

/*
 * 提取"thum"（jssf）预览，并还原成标准JPEG。
 * 返回的JPEG是baseline、8位、带重启标记，可交给任何JPEG解码器
 * （libjpeg、stb_image、Qt、浏览器等）。
 * 文件没有"thum"块时返回SAI2_ERR_NO_THUMBNAIL（小画布可能只有
 * "intg"，此时改用sai2_extract_pixels）。
 */
sai2_status sai2_extract_jpeg(const void *file_data, size_t file_size,
                              sai2_image *out_image);

/*
 * 把"intg"（dpcm）无损预览解码成完整画布分辨率的BGRA像素。
 * sai2_extract_jpeg()返回NO_THUMBNAIL时用它。
 * 数据可能很大，占width * height * 4字节。
 */
sai2_status sai2_extract_pixels(const void *file_data, size_t file_size,
                                sai2_pixels *out_pixels);

void sai2_image_free(sai2_image *image);
void sai2_pixels_free(sai2_pixels *pixels);

const char *sai2_status_string(sai2_status status);

#ifdef __cplusplus
}
#endif

#endif /* SAI2_PREVIEW_H */
