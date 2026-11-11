#pragma once

#include <picolet/capture/image.h>

namespace picolet {

// 将完整图像编码为 PNG 并写入 path。调用线程须已初始化 COM，path 和 image 在返回前保持有效。
// 编码在目标目录的临时文件中完成，关闭编码流后再提交到目标路径，避免中途失败截断已有文件。
// replaceExisting 只在调用方已授权覆盖时设为 true。成功返回 S_OK，失败返回对应的 HRESULT。
HRESULT SavePngFile(const wchar_t* path, const Image& image, bool replaceExisting) noexcept;

}  // namespace picolet
