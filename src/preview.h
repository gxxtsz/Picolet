#pragma once

#include "image.h"
#include "win32.h"

namespace picolet {

inline constexpr UINT kPreviewAction = WM_APP + 3;

enum class PreviewVisibility { Hidden, Normal, Minimized };

// 预览持有原图。改变窗口尺寸只重新计算显示矩形，复制和保存仍读取原始像素。
class PreviewWindow {
public:
    static bool Register(HINSTANCE instance, HICON icon) noexcept;
    HRESULT Present(HINSTANCE instance, HWND controller, Image&& image, POINT origin) noexcept;
    PreviewVisibility HideForCapture() noexcept;
    void Restore(PreviewVisibility visibility) noexcept;
    void Close() noexcept;
    void SetStatus(const wchar_t* text) noexcept;

    const Image& image() const noexcept { return image_; }
    HWND window() const noexcept { return window_; }
    bool OwnsMessage(HWND target) const noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    bool CreateControls() noexcept;
    void RefreshFont() noexcept;
    void Layout() noexcept;
    void Paint() noexcept;

    HWND window_ = nullptr;
    HWND controller_ = nullptr;
    HWND copy_ = nullptr;
    HWND save_ = nullptr;
    HWND close_ = nullptr;
    HWND dimensions_ = nullptr;
    HWND status_ = nullptr;
    Image image_;
    FontHandle font_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    RECT canvas_{};
};

}  // namespace picolet
