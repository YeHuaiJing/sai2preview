/* SPDX-License-Identifier: MIT */

/*
 * main.c - 把test/hello.sai2的预览图导出来。
 *
 * sai2_thumbnail_export()会读取.sai2文件，挑选可用的最佳预览
 * （优先"thum"的JPEG，否则"intg"的BGRA），然后写到文件。
 */

#include "sai2_preview.h"

#include <stdio.h>

int main(void)
{
    sai2_status rc = sai2_thumbnail_export("test/hello.sai2", "test/img.jpg");

    if (rc != SAI2_OK) {
        fprintf(stderr, "export failed: %s\n", sai2_status_string(rc));
        return 1;
    }

    puts("wrote test/img.jpg");
    return 0;
}
