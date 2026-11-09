#pragma once

#include <picolet/image/image.h>

namespace picolet {

// 将完整尺寸的图像立即写入剪贴板。成功后数据由系统保管，退出程序后仍可粘贴。
HRESULT CopyImageToClipboard(HWND owner, const Image& image) noexcept;

// 在目标目录编码临时文件，再移动到目标路径。仅在调用方已确认覆盖时替换同名文件。
// 必须在已初始化 COM 的线程调用。
HRESULT SavePngFile(const wchar_t* path, const Image& image, bool replaceExisting) noexcept;

// 以系统对话框选择 PNG 保存路径。owner 在对话框关闭前必须有效。
// S_FALSE 表示用户取消，失败时调用方可保留当前会话的选区和标注以便重试。
HRESULT SavePngWithDialog(HWND owner, const Image& image) noexcept;

}  // namespace picolet
