#pragma once

#include "image.h"

#include <memory>
#include <vector>

namespace picolet {

// 管理本次进程中的所有贴图。每张贴图持有自己的原尺寸合成图，移动和缩放不修改像素。
// 必须在创建主控制窗口的 UI 线程操作；窗口可单独关闭，退出时由管理器统一清理。
class PinWindows {
public:
    PinWindows() noexcept;
    ~PinWindows();

    static bool Register(HINSTANCE instance, HICON icon) noexcept;

    // 把 image 移入新的置顶窗口，初始位置靠近选区；图片超过显示器工作区时等比缩小。
    // 返回失败时不留下窗口。调用方如需重试，可从截图会话重新生成合成图。
    HRESULT Open(HINSTANCE instance, HWND owner, Image&& image, const RECT& selection) noexcept;

    // 销毁所有仍打开的窗口并释放原图。可重复调用。
    void CloseAll() noexcept;

private:
    struct Pin;
    std::vector<std::unique_ptr<Pin>> pins_;
};

}  // namespace picolet
