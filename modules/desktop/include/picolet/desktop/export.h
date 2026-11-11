#pragma once

#include <picolet/capture/image.h>

namespace picolet {

// 将原始尺寸的图片以 DIB 格式写入系统剪贴板。owner 必须是有效窗口，并在调用期间保持有效。
// 成功返回 S_OK，系统接管 DIB 内存；关闭窗口或退出程序后仍可粘贴。
HRESULT CopyImageToClipboard(HWND owner, const Image& image) noexcept;

// 打开系统保存对话框，由用户选路径并确认覆盖，再交由 capture 编码和落盘。
// 调用线程须已初始化 COM，owner 和 image 在模态对话框及保存过程结束前保持有效。
// 保存成功返回 S_OK，用户取消返回 S_FALSE；取消或失败时调用方可以恢复原编辑会话。
HRESULT SavePngWithDialog(HWND owner, const Image& image) noexcept;

}  // namespace picolet
