#pragma once

#include "image.h"
#include "win32.h"

namespace picolet {

inline constexpr UINT kOverlayFinished = WM_APP + 2;

// 使用冻结帧作为置顶遮罩的背景。原图由控制窗口持有，直到选择完成或取消。
class SelectionOverlay {
public:
    static bool Register(HINSTANCE instance, HICON icon) noexcept;
    HRESULT Open(HINSTANCE instance, HWND owner, const DesktopFrame& frame, POINT anchor) noexcept;
    void Cancel() noexcept;
    void Close() noexcept;
    const RECT& selection() const noexcept { return selection_; }
    POINT last_point() const noexcept { return last_; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    void Finish(bool accepted) noexcept;
    void UpdateSelection(POINT point) noexcept;
    void Paint() noexcept;
    RECT ClientSelection() const noexcept;
    RECT LabelRect() const noexcept;

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    const DesktopFrame* frame_ = nullptr;
    Image dark_;
    FontHandle font_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    POINT anchor_{};
    POINT start_{};
    POINT last_{};
    RECT selection_{};
    bool dragging_ = false;
    bool finished_ = true;
    bool activated_ = false;
};

}  // namespace picolet
