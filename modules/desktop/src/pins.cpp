#include <picolet/desktop/pins.h>

#include "pin_shadow.h"
#include <picolet/win32.h>

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace picolet {
namespace {

constexpr wchar_t kPinClass[] = L"Picolet.Pin";

RECT WorkArea(HMONITOR monitor) noexcept {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

// 把图片主体的一部分留在目标工作区，确保移到屏幕边缘后仍能抓取。
// imageBounds 使用桌面物理坐标，四周的阴影不计入可抓取的尺寸。
RECT KeepReachable(RECT imageBounds, const RECT& work, HWND window) noexcept {
    if (IsRectEmpty(&work)) return imageBounds;
    const LONG width = imageBounds.right - imageBounds.left;
    const LONG height = imageBounds.bottom - imageBounds.top;
    const UINT dpi = GetDpiForWindow(window);
    const LONG grip = Dip(36, dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
    const LONG gripX = std::min({width, work.right - work.left, grip});
    const LONG gripY = std::min({height, work.bottom - work.top, grip});
    const LONG left = std::clamp(imageBounds.left, work.left + gripX - width, work.right - gripX);
    const LONG top = std::clamp(imageBounds.top, work.top + gripY - height, work.bottom - gripY);
    return {left, top, left + width, top + height};
}

}  // namespace

struct PinWindows::Pin {
    HWND window = nullptr;
    Image image;
    RECT dragRect{};
    POINT dragStart{};
    double scale = 1.0;
    double minScale = 0.1;
    bool dragging = false;

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<Pin*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Pin*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && message == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->window = nullptr;
            self->image = {};
            self = nullptr;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // 按新的图片主体矩形合成窗口。imageBounds 是桌面物理坐标，四周另加固定宽度的阴影。
    // 先完成离屏绘制，再一次性提交位置、尺寸和像素。提交失败时调用方保留原有缩放倍率。
    HRESULT Present(const RECT& imageBounds) const noexcept {
        const LONG width = imageBounds.right - imageBounds.left;
        const LONG height = imageBounds.bottom - imageBounds.top;
        SIZE size{width + 2 * kPinShadowInset, height + 2 * kPinShadowInset};
        const auto count = static_cast<std::uint64_t>(size.cx) * size.cy;
        if (count > std::numeric_limits<DWORD>::max() / sizeof(std::uint32_t)) {
            return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        }

        const MemoryDc dc(CreateCompatibleDC(nullptr));
        if (!dc) return E_FAIL;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size.cx;
        info.bmiHeader.biHeight = -size.cy;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        info.bmiHeader.biSizeImage = static_cast<DWORD>(count * sizeof(std::uint32_t));
        void* bits = nullptr;
        SetLastError(ERROR_SUCCESS);
        const BitmapHandle bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
        if (!bitmap || !bits) return LastErrorResult();
        const HGDIOBJ original = SelectObject(dc.get(), bitmap.get());
        if (!original || original == HGDI_ERROR) return E_FAIL;

        const RECT destination{kPinShadowInset, kPinShadowInset, kPinShadowInset + width, kPinShadowInset + height};
        HRESULT result = E_FAIL;
        if (SetStretchBltMode(dc.get(), HALFTONE) && SetBrushOrgEx(dc.get(), 0, 0, nullptr) &&
            DrawImage(dc.get(), image, destination) && GdiFlush()) {
            ApplyPinShadow({static_cast<std::uint32_t*>(bits), static_cast<std::size_t>(count)}, size);
            POINT position{imageBounds.left - kPinShadowInset, imageBounds.top - kPinShadowInset};
            POINT source{};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            SetLastError(ERROR_SUCCESS);
            result = UpdateLayeredWindow(window, nullptr, &position, &size, dc.get(), &source, 0, &blend, ULW_ALPHA)
                ? S_OK : LastErrorResult();
        }
        SelectObject(dc.get(), original);
        return result;
    }

    // bounds 含阴影，移动时只修改窗口位置，复用系统保存的分层位图。
    void Move(RECT bounds, HMONITOR monitor) const noexcept {
        InflateRect(&bounds, -kPinShadowInset, -kPinShadowInset);
        const RECT visible = KeepReachable(bounds, WorkArea(monitor), window);
        SetWindowPos(window, HWND_TOPMOST, visible.left - kPinShadowInset, visible.top - kPinShadowInset,
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // 调整图片显示倍率，以光标下的图像位置为锚。指向可见阴影时，取最近的图片边缘。
    // 分层窗口的全透明像素不命中，滚轮消息即使由焦点窗口收到也不会触发缩放。
    void Zoom(int wheel) noexcept {
        if (dragging || !wheel) return;
        RECT previous{};
        POINT cursor{};
        if (!GetWindowRect(window, &previous) || !GetCursorPos(&cursor) || WindowFromPoint(cursor) != window) return;
        const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        // 限制离屏位图尺寸，保留小图的最小显示倍率。
        const double ceiling = std::max(minScale, std::min({4.0,
            static_cast<double>(work.right - work.left) * 4.0 / image.width(),
            static_cast<double>(work.bottom - work.top) * 4.0 / image.height()}));
        const double next = std::clamp(scale * std::pow(1.2, static_cast<double>(wheel) / WHEEL_DELTA), minScale, ceiling);
        if (scale == next) return;
        const LONG width = std::max(1L, std::lround(image.width() * next));
        const LONG height = std::max(1L, std::lround(image.height() * next));
        InflateRect(&previous, -kPinShadowInset, -kPinShadowInset);
        const LONG previousWidth = previous.right - previous.left;
        const LONG previousHeight = previous.bottom - previous.top;
        if (width == previousWidth && height == previousHeight) {
            scale = next;
            return;
        }
        const POINT anchor{std::clamp(cursor.x, previous.left, previous.right),
                           std::clamp(cursor.y, previous.top, previous.bottom)};
        const LONG x = anchor.x - std::lround(static_cast<double>(anchor.x - previous.left) * width / previousWidth);
        const LONG y = anchor.y - std::lround(static_cast<double>(anchor.y - previous.top) * height / previousHeight);
        const RECT target = KeepReachable({x, y, x + width, y + height}, work, window);
        if (SUCCEEDED(Present(target))) {
            scale = next;
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch (message) {
        case WM_LBUTTONDOWN:
            if (GetWindowRect(window, &dragRect) && GetCursorPos(&dragStart)) {
                SetCapture(window);
                dragging = GetCapture() == window;
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging) {
                POINT cursor{};
                if (GetCursorPos(&cursor)) {
                    RECT rect = dragRect;
                    OffsetRect(&rect, cursor.x - dragStart.x, cursor.y - dragStart.y);
                    Move(rect, MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
                }
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CANCELMODE:
            dragging = false;
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            dragging = false;
            return 0;
        case WM_MOUSEWHEEL:
            Zoom(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        case WM_DPICHANGED:
            // 图片尺寸和阴影宽度都使用物理像素，换屏时沿用当前分层位图尺寸。
            return 0;
        case WM_DISPLAYCHANGE: {
            RECT rect{};
            if (GetWindowRect(window, &rect)) Move(rect, MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
            return 0;
        }
        case WM_RBUTTONUP:
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

PinWindows::PinWindows() noexcept = default;
PinWindows::~PinWindows() { CloseAll(); }

bool PinWindows::Register(HINSTANCE instance, HICON icon) noexcept {
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = Pin::Proc;
    definition.hInstance = instance;
    definition.hIcon = icon;
    definition.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    definition.lpszClassName = kPinClass;
    return RegisterClassExW(&definition) != 0;
}

HRESULT PinWindows::Open(HINSTANCE instance, HWND owner, Image&& image, const RECT& selection) noexcept {
    if (image.empty() || IsRectEmpty(&selection)) return E_INVALIDARG;
    std::erase_if(pins_, [](const auto& pin) { return !pin->window; });
    try {
        pins_.push_back(std::make_unique<Pin>());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    auto& pin = *pins_.back();
    const RECT work = WorkArea(MonitorFromRect(&selection, MONITOR_DEFAULTTONEAREST));
    RECT available = work;
    InflateRect(&available, -kPinShadowInset, -kPinShadowInset);
    const RECT fitted = FitImage(image, available);
    const LONG width = fitted.right - fitted.left;
    const LONG height = fitted.bottom - fitted.top;
    if (width <= 0 || height <= 0) {
        pins_.pop_back();
        return E_FAIL;
    }
    pin.scale = std::min(static_cast<double>(width) / image.width(), static_cast<double>(height) / image.height());
    pin.minScale = std::min(0.1, pin.scale);
    pin.image = std::move(image);
    const LONG x = std::clamp(selection.left, available.left, available.right - width);
    const LONG y = std::clamp(selection.top, available.top, available.bottom - height);
    SetLastError(ERROR_SUCCESS);
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kPinClass, L"Picolet 贴图",
                                        WS_POPUP, x - kPinShadowInset, y - kPinShadowInset,
                                        width + 2 * kPinShadowInset, height + 2 * kPinShadowInset,
                                        owner, nullptr, instance, &pin);
    HRESULT result = window ? pin.Present({x, y, x + width, y + height}) : LastErrorResult();
    if (SUCCEEDED(result)) {
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
            result = LastErrorResult();
        }
    }
    if (FAILED(result)) {
        if (window) DestroyWindow(window);
        pins_.pop_back();
    }
    return result;
}

void PinWindows::CloseAll() noexcept {
    for (const auto& pin : pins_) if (pin->window) DestroyWindow(pin->window);
    pins_.clear();
}

}  // namespace picolet
