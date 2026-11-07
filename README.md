# sai2preview

从PaintTool SAI 2（`.sai2`）画布文件中提取缩略图/预览图的C库。

## 编译

```bat
cmake -S . -B build
cmake --build build --config Release
```

Windows（MinGW）产物：

- `build/libsai2preview.a`：静态库
- `build/sai2export.exe`：示例程序

Linux下`sai2export`不带扩展名。

## 安装/打包

```bat
cmake --install build --prefix dist
```

安装内容包括`bin/sai2export.exe`、`lib/libsai2preview.a`、`include/sai2_preview.h`
以及许可证文件。

## 运行

在项目根目录执行：

```bat
.\build\sai2export.exe
```

会读取`test/hello.sai2`，把预览图写到`test/img.jpg`。

## 效果

`test/hello.sai2`导出到`test/thumb.jpg`的预览图。

![hello.sai2的预览图](test/thumb.jpg)

## 接口

```c
#include <sai2_preview.h>

sai2_status rc = sai2_thumbnail_export("test/hello.sai2", "test/img.jpg");
if (rc != SAI2_OK)
    fprintf(stderr, "%s\n", sai2_status_string(rc));
```

| 函数 | 说明 |
| --- | --- |
| `sai2_thumbnail_export` | 读取`.sai2`并直接写出缩略图 |
| `sai2_thumbnail_extract` | 从内存数据提取缩略图 |
| `sai2_thumbnail_write` | 将缩略图写入JPEG或BMP |
| `sai2_thumbnail_free` | 释放缩略图 |
| `sai2_get_info` | 读取画布尺寸、块数量等信息 |
| `sai2_extract_jpeg` | 仅提取`thum`，返回JPEG |
| `sai2_extract_pixels` | 仅解码`intg`，返回BGRA |
| `sai2_status_string` | 将错误码转为字符串 |

输出格式由预览块决定：有`thum`为JPEG，只有`intg`为BGRA（写盘为BMP）。
所有返回的buffer都由`malloc`分配，需用对应的`sai2_*_free`释放。

## 链接方式

```bash
cc app.c -Iinclude build/libsai2preview.a
```

或在CMake中：

```cmake
add_subdirectory(path/to/sai2preview)
target_link_libraries(app PRIVATE sai2preview)
```

## License

本项目采用MIT License，Copyright (c) 2026 YeHuaiJing。

部分JSSF/DPCM解析代码改编自Wunkolo/libsai，发布二进制时请同时附带
`LICENSE`、`THIRD_PARTY_NOTICES.md`和`LICENSES/libsai-MIT.txt`。
