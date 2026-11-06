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

    // 把 image 移入新的置顶窗口，初始位置靠近选区，优先让图片与原选区对齐。
    // 窗口四边各预留 12 个物理像素绘制渐变阴影。空间不足时等比缩小显示区域，保留原图像素。
    // 分层位图提交成功后才显示窗口。创建失败时返回 HRESULT 并清理窗口，截图会话可重新生成合成图后重试。
    HRESULT Open(HINSTANCE instance, HWND owner, Image&& image, const RECT& selection) noexcept;

    // 销毁所有仍打开的窗口并释放原图。可重复调用。
    void CloseAll() noexcept;

private:
    struct Pin;
    std::vector<std::unique_ptr<Pin>> pins_;
};

}  // namespace picolet
