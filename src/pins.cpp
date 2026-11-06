#include "pins.h"

#include "win32.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace picolet {
namespace {

constexpr wchar_t kPinClass[] = L"Picolet.Pin";
constexpr std::array<COLORREF, 2> kFrameColors{RGB(38, 38, 38), RGB(245, 245, 245)};
// FrameRect 每道边占一个物理像素。窗口比实际图片四周各多出这段距离。
constexpr LONG kFrameInset = static_cast<LONG>(kFrameColors.size());

RECT WorkArea(HMONITOR monitor) noexcept {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

RECT KeepReachable(RECT window, const RECT& work, UINT dpi) noexcept {
    if (IsRectEmpty(&work)) return window;
    const LONG width = window.right - window.left;
    const LONG height = window.bottom - window.top;
    const LONG gripX = std::min({width, work.right - work.left, static_cast<LONG>(Dip(36, dpi))});
    const LONG gripY = std::min({height, work.bottom - work.top, static_cast<LONG>(Dip(36, dpi))});
    const LONG left = std::clamp(window.left, work.left + gripX - width, work.right - gripX);
    const LONG top = std::clamp(window.top, work.top + gripY - height, work.bottom - gripY);
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

    // rect 是含边框的窗口范围，按指定屏幕限制位置以保留可抓取区域。
    bool Place(RECT rect, HMONITOR monitor) const noexcept {
        const auto dpi = GetDpiForWindow(window);
        rect = KeepReachable(rect, WorkArea(monitor), dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
        return SetWindowPos(window, HWND_TOPMOST, rect.left, rect.top, rect.right - rect.left,
                            rect.bottom - rect.top, SWP_NOACTIVATE) != FALSE;
    }

    // 按滚轮倍率调整内框中的图片。光标落在边框上时，固定离光标最近的图片边缘。
    void Zoom(int wheel) noexcept {
        if (dragging || !wheel) return;
        RECT previous{};
        POINT cursor{};
        if (!GetWindowRect(window, &previous) || !GetCursorPos(&cursor) || !PtInRect(&previous, cursor)) return;
        const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        const RECT work = WorkArea(monitor);
        // 放大仅改变窗口大小。保留光标下的图像位置，同时限制过大的窗口尺寸。
        const double ceiling = std::max(minScale, std::min({4.0,
            static_cast<double>(work.right - work.left) * 4.0 / image.width(),
            static_cast<double>(work.bottom - work.top) * 4.0 / image.height()}));
        const double next = std::clamp(scale * std::pow(1.2, static_cast<double>(wheel) / WHEEL_DELTA), minScale, ceiling);
        if (scale == next) return;
        const LONG width = std::max(1L, std::lround(image.width() * next));
        const LONG height = std::max(1L, std::lround(image.height() * next));
        InflateRect(&previous, -kFrameInset, -kFrameInset);
        const POINT anchor{std::clamp(cursor.x, previous.left, previous.right),
                           std::clamp(cursor.y, previous.top, previous.bottom)};
        const LONG x = anchor.x - std::lround(static_cast<double>(anchor.x - previous.left) * width /
                                             (previous.right - previous.left));
        const LONG y = anchor.y - std::lround(static_cast<double>(anchor.y - previous.top) * height /
                                             (previous.bottom - previous.top));
        RECT outer{x, y, x + width, y + height};
        InflateRect(&outer, kFrameInset, kFrameInset);
        if (Place(outer, monitor)) {
            scale = next;
            InvalidateRect(window, nullptr, FALSE);
        }
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            if (dc) {
                RECT client{};
                GetClientRect(window, &client);
                const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
                for (const COLORREF color : kFrameColors) {
                    SetDCBrushColor(dc, color);
                    FrameRect(dc, &client, brush);
                    InflateRect(&client, -1, -1);
                }
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                DrawImage(dc, image, client);
            }
            EndPaint(window, &paint);
            return 0;
        }
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
                    Place(rect, MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
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
            // 贴图使用物理像素。换屏时不接受按 DPI 改变图像比例的建议尺寸。
            return 0;
        case WM_DISPLAYCHANGE: {
            RECT rect{};
            if (GetWindowRect(window, &rect)) Place(rect, MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
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
    InflateRect(&available, -kFrameInset, -kFrameInset);
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
    const LONG outerWidth = width + 2 * kFrameInset;
    const LONG outerHeight = height + 2 * kFrameInset;
    const LONG x = std::clamp(selection.left - kFrameInset, work.left, work.right - outerWidth);
    const LONG y = std::clamp(selection.top - kFrameInset, work.top, work.bottom - outerHeight);
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPinClass, L"Picolet 贴图",
                                        WS_POPUP, x, y, outerWidth, outerHeight, owner, nullptr, instance, &pin);
    if (!window || !SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        const auto error = LastErrorResult();
        if (window) DestroyWindow(window);
        pins_.pop_back();
        return error;
    }
    return S_OK;
}

void PinWindows::CloseAll() noexcept {
    for (const auto& pin : pins_) if (pin->window) DestroyWindow(pin->window);
    pins_.clear();
}

}  // namespace picolet
