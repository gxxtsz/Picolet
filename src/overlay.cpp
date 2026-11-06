#include "overlay.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace picolet {
namespace {

constexpr wchar_t kOverlayClass[] = L"Picolet.SelectionOverlay";

POINT MousePoint() noexcept {
    // 读取当前鼠标消息的屏幕位置，避免拖动很快时使用到下一条消息的位置。
    const auto position = GetMessagePos();
    return {GET_X_LPARAM(position), GET_Y_LPARAM(position)};
}

}  // namespace

bool SelectionOverlay::Register(HINSTANCE instance, HICON icon) noexcept {
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = WindowProc;
    definition.hInstance = instance;
    definition.hIcon = icon;
    definition.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    definition.lpszClassName = kOverlayClass;
    return RegisterClassExW(&definition) != 0;
}

HRESULT SelectionOverlay::Open(HINSTANCE instance, HWND owner, const DesktopFrame& frame, POINT anchor) noexcept {
    const auto& image = frame.image;
    const HRESULT allocated = dark_.Resize(image.width(), image.height());
    if (FAILED(allocated)) {
        return allocated;
    }
    // 只在进入框选时生成一次暗色副本。拖动时用裁剪区显示选区内的原图。
    std::ranges::transform(image.pixels(), dark_.pixels().begin(), [](std::uint32_t pixel) {
        return 0xff000000u | ((pixel >> 1) & 0x007f7f7fu);
    });
    frame_ = &frame;
    owner_ = owner;
    anchor_ = last_ = anchor;
    selection_ = {};
    activated_ = dragging_ = finished_ = false;

    const auto& bounds = frame.bounds;
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"Picolet 选区截图",
                                        WS_POPUP, bounds.left, bounds.top, image.width(), image.height(),
                                        owner, nullptr, instance, this);
    if (!window) {
        const HRESULT error = LastErrorResult();
        Close();
        return error;
    }
    dpi_ = GetDpiForWindow(window_);
    font_.reset(CreateUiFont(dpi_));
    if (!SetWindowPos(window_, HWND_TOPMOST, bounds.left, bounds.top, image.width(), image.height(), SWP_SHOWWINDOW)) {
        const HRESULT error = LastErrorResult();
        Close();
        return error;
    }
    SetForegroundWindow(window_);
    UpdateWindow(window_);
    return S_OK;
}

void SelectionOverlay::Cancel() noexcept {
    Finish(false);
}

void SelectionOverlay::Close() noexcept {
    finished_ = true;
    dragging_ = false;
    if (window_) {
        if (GetCapture() == window_) {
            ReleaseCapture();
        }
        DestroyWindow(window_);
    }
    font_.reset();
    frame_ = nullptr;
    dark_ = Image{};
}

LRESULT CALLBACK SelectionOverlay::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<SelectionOverlay*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<SelectionOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->window_ = nullptr;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return self->Handle(message, wparam, lparam);
}

LRESULT SelectionOverlay::Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            activated_ = true;
            SetFocus(window_);
        } else if (activated_) {
            Cancel();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!finished_ && !dragging_) {
            start_ = MousePoint();
            dragging_ = true;
            SetCapture(window_);
            if (GetCapture() != window_) {
                Cancel();
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_) {
            UpdateSelection(MousePoint());
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            UpdateSelection(MousePoint());
            if (!IsRectEmpty(&selection_)) {
                Finish(true);
            } else {
                dragging_ = false;
                ReleaseCapture();
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging_) {
            Cancel();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam == VK_ESCAPE) {
            Cancel();
            return 0;
        }
        break;
    case WM_RBUTTONDOWN:
        return 0;
    case WM_RBUTTONUP:
    case WM_CANCELMODE:
    case WM_DISPLAYCHANGE:
    case WM_CLOSE:
        Cancel();
        return 0;
    case WM_DPICHANGED:
        if (IsWindowVisible(window_)) {
            Cancel();
        }
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void SelectionOverlay::Finish(bool accepted) noexcept {
    if (finished_ || !window_) {
        return;
    }
    // 释放鼠标或隐藏窗口会同步触发其他消息。先标记结束，防止确认被误判成取消。
    finished_ = true;
    dragging_ = false;
    if (GetCapture() == window_) {
        ReleaseCapture();
    }
    SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                 SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    if (!PostMessageW(owner_, kOverlayFinished, accepted, 0)) {
        SendMessageW(owner_, kOverlayFinished, accepted, 0);
    }
}

RECT SelectionOverlay::ClientSelection() const noexcept {
    RECT result = selection_;
    if (!IsRectEmpty(&result)) {
        OffsetRect(&result, -frame_->bounds.left, -frame_->bounds.top);
    }
    return result;
}

RECT SelectionOverlay::LabelRect() const noexcept {
    const bool hint = IsRectEmpty(&selection_) != FALSE;
    const POINT point = hint ? anchor_ : last_;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor)) {
        monitor.rcMonitor = frame_->bounds;
    }
    const RECT& screen = monitor.rcMonitor;
    const LONG width = std::min<LONG>(Dip(hint ? 260 : 144, dpi_), screen.right - screen.left);
    const LONG height = std::min<LONG>(Dip(30, dpi_), screen.bottom - screen.top);
    const int gap = Dip(12, dpi_);
    const LONG x = hint ? screen.left + (screen.right - screen.left - width) / 2
                        : std::clamp(point.x + gap, screen.left, screen.right - width);
    const LONG y = hint ? std::min(screen.top + gap, screen.bottom - height)
                        : std::clamp(point.y + (point.y + gap + height <= screen.bottom ? gap : -gap - height),
                                     screen.top, screen.bottom - height);
    RECT label{x, y, x + width, y + height};
    OffsetRect(&label, -frame_->bounds.left, -frame_->bounds.top);
    return label;
}

void SelectionOverlay::UpdateSelection(POINT point) noexcept {
    RECT changed = LabelRect();
    const RECT oldSelection = ClientSelection();
    UnionRect(&changed, &changed, &oldSelection);
    last_ = point;
    selection_ = SelectionRect(start_, point, frame_->bounds);
    const RECT newSelection = ClientSelection();
    const RECT newLabel = LabelRect();
    UnionRect(&changed, &changed, &newSelection);
    UnionRect(&changed, &changed, &newLabel);
    InvalidateRect(window_, &changed, FALSE);
}

void SelectionOverlay::Paint() noexcept {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    if (!dc || !frame_) {
        EndPaint(window_, &paint);
        return;
    }
    const RECT canvas{0, 0, dark_.width(), dark_.height()};
    const RECT selected = ClientSelection();
    const bool hasSelection = IsRectEmpty(&selected) == FALSE;
    const int saved = SaveDC(dc);
    if (saved && hasSelection) {
        ExcludeClipRect(dc, selected.left, selected.top, selected.right, selected.bottom);
    }
    DrawImage(dc, dark_, canvas);
    if (saved) {
        RestoreDC(dc, saved);
    }
    if (hasSelection) {
        if (const int clip = SaveDC(dc)) {
            IntersectClipRect(dc, selected.left, selected.top, selected.right, selected.bottom);
            DrawImage(dc, frame_->image, canvas);
            RestoreDC(dc, clip);
        }
        const BrushHandle border(CreateSolidBrush(RGB(62, 230, 193)));
        if (border) {
            FrameRect(dc, &selected, border.get());
        }
    }

    RECT label = LabelRect();
    const BrushHandle background(CreateSolidBrush(RGB(20, 30, 44)));
    if (background) {
        FillRect(dc, &label, background.get());
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(247, 250, 252));
    const HGDIOBJ previous = font_ ? SelectObject(dc, font_.get()) : nullptr;
    wchar_t size[48]{};
    std::swprintf(size, std::size(size), L"%ld × %ld 像素", selected.right - selected.left,
                  selected.bottom - selected.top);
    DrawTextW(dc, hasSelection ? size : L"拖拽选择区域  ·  Esc 或右键取消", -1, &label,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previous) {
        SelectObject(dc, previous);
    }
    EndPaint(window_, &paint);
}

}  // namespace picolet
