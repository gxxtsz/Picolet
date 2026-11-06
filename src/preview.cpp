#include "preview.h"

#include "resources.h"

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <utility>

namespace picolet {
namespace {

constexpr wchar_t kPreviewClass[] = L"Picolet.Preview";
constexpr wchar_t kPreviewTitle[] = L"Picolet 截图预览";
constexpr UINT kKeepPlacement = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER;

RECT InitialWindowRect(POINT origin) noexcept {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST), &monitor)) {
        monitor.rcWork = {0, 0, 1080, 720};
    }
    const RECT& work = monitor.rcWork;
    const LONG workWidth = work.right - work.left;
    const LONG workHeight = work.bottom - work.top;
    const LONG width = std::min(workWidth * 4 / 5, 1120L);
    const LONG height = std::min(workHeight * 4 / 5, 760L);
    const LONG left = work.left + (workWidth - width) / 2;
    const LONG top = work.top + (workHeight - height) / 2;
    return {left, top, left + width, top + height};
}

}  // namespace

bool PreviewWindow::Register(HINSTANCE instance, HICON icon) noexcept {
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = WindowProc;
    definition.hInstance = instance;
    definition.hIcon = icon;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.lpszClassName = kPreviewClass;
    return RegisterClassExW(&definition) != 0;
}

HRESULT PreviewWindow::Present(HINSTANCE instance, HWND controller, Image&& image, POINT origin) noexcept {
    if (!window_) {
        controller_ = controller;
        const RECT bounds = InitialWindowRect(origin);
        if (!CreateWindowExW(WS_EX_CONTROLPARENT, kPreviewClass, kPreviewTitle,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, bounds.left, bounds.top,
                             bounds.right - bounds.left, bounds.bottom - bounds.top,
                             nullptr, nullptr, instance, this)) {
            return LastErrorResult();
        }
    }
    image_ = std::move(image);
    wchar_t dimensions[48]{};
    std::swprintf(dimensions, std::size(dimensions), L"%d × %d 像素", image_.width(), image_.height());
    SetWindowTextW(dimensions_, dimensions);
    SetStatus(L"");
    InvalidateRect(window_, nullptr, FALSE);
    Restore(PreviewVisibility::Normal);
    return S_OK;
}

PreviewVisibility PreviewWindow::HideForCapture() noexcept {
    if (!window_ || !IsWindowVisible(window_)) {
        return PreviewVisibility::Hidden;
    }
    const auto visibility = IsIconic(window_) ? PreviewVisibility::Minimized : PreviewVisibility::Normal;
    // 托盘程序可能带隐藏启动标志。直接更改可见性，避免首次 ShowWindow 采用启动参数。
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, kKeepPlacement | SWP_HIDEWINDOW);
    return visibility;
}

void PreviewWindow::Restore(PreviewVisibility visibility) noexcept {
    if (!window_ || visibility == PreviewVisibility::Hidden) {
        return;
    }
    if (visibility == PreviewVisibility::Minimized) {
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, kKeepPlacement | SWP_SHOWWINDOW);
        return;
    }
    if (IsIconic(window_)) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(window_, &placement)) {
            placement.showCmd = (placement.flags & WPF_RESTORETOMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            SetWindowPlacement(window_, &placement);
        }
    }
    SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (SetForegroundWindow(window_)) {
        SetFocus(copy_);
    }
}

void PreviewWindow::Close() noexcept {
    if (window_) {
        DestroyWindow(window_);
    }
}

void PreviewWindow::SetStatus(const wchar_t* text) noexcept {
    if (status_) {
        SetWindowTextW(status_, text);
    }
}

bool PreviewWindow::OwnsMessage(HWND target) const noexcept {
    return window_ && (target == window_ || IsChild(window_, target));
}

LRESULT CALLBACK PreviewWindow::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<PreviewWindow*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->window_ = nullptr;
        self->copy_ = self->save_ = self->close_ = self->dimensions_ = self->status_ = nullptr;
        self->font_.reset();
        self->image_ = Image{};
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return self->Handle(message, wparam, lparam);
}

LRESULT PreviewWindow::Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(window_);
        if (!CreateControls()) {
            return -1;
        }
        RefreshFont();
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto& bounds = *reinterpret_cast<MINMAXINFO*>(lparam);
        const UINT dpi = GetDpiForWindow(window_);
        bounds.ptMinTrackSize = {Dip(460, dpi ? dpi : dpi_), Dip(280, dpi ? dpi : dpi_)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        RefreshFont();
        const auto& suggested = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window_, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                     suggested.bottom - suggested.top, SWP_NOACTIVATE | SWP_NOZORDER);
        Layout();
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            Layout();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_PREVIEW_COPY:
        case ID_PREVIEW_SAVE:
            SendMessageW(controller_, kPreviewAction, LOWORD(wparam), 0);
            return 0;
        case IDCANCEL:
        case ID_PREVIEW_CLOSE:
            Close();
            return 0;
        }
        break;
    case WM_CLOSE:
        Close();
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

bool PreviewWindow::CreateControls() noexcept {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
    const auto control = [&](const wchar_t* kind, const wchar_t* label, DWORD style, INT_PTR id) {
        return CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(id), instance, nullptr);
    };
    copy_ = control(L"BUTTON", L"复制", BS_PUSHBUTTON | WS_TABSTOP, ID_PREVIEW_COPY);
    save_ = control(L"BUTTON", L"另存为 PNG…", BS_PUSHBUTTON | WS_TABSTOP, ID_PREVIEW_SAVE);
    close_ = control(L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP, ID_PREVIEW_CLOSE);
    dimensions_ = control(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    status_ = control(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    return copy_ && save_ && close_ && dimensions_ && status_;
}

void PreviewWindow::RefreshFont() noexcept {
    FontHandle replacement(CreateUiFont(dpi_));
    const HGDIOBJ font = replacement ? replacement.get() : GetStockObject(DEFAULT_GUI_FONT);
    for (const HWND control : {copy_, save_, close_, dimensions_, status_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
    // 控件不拥有字体。所有控件已换到新字体后，才释放旧字体。
    font_.reset(replacement.release());
}

void PreviewWindow::Layout() noexcept {
    if (!status_) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const int margin = Dip(12, dpi_);
    const int gap = Dip(8, dpi_);
    const int buttonHeight = Dip(32, dpi_);
    const int rowY = client.bottom - Dip(62, dpi_);
    canvas_ = {margin, margin, client.right - margin, rowY - gap};

    int x = client.right - margin;
    x -= Dip(70, dpi_);
    MoveWindow(close_, x, rowY, Dip(70, dpi_), buttonHeight, TRUE);
    x -= Dip(112, dpi_) + gap;
    MoveWindow(save_, x, rowY, Dip(112, dpi_), buttonHeight, TRUE);
    x -= Dip(70, dpi_) + gap;
    MoveWindow(copy_, x, rowY, Dip(70, dpi_), buttonHeight, TRUE);
    MoveWindow(dimensions_, margin, rowY, std::max(0, x - margin - gap), buttonHeight, TRUE);
    MoveWindow(status_, margin, rowY + buttonHeight + Dip(2, dpi_),
               std::max(0L, client.right - 2 * margin), Dip(18, dpi_), TRUE);
    InvalidateRect(window_, nullptr, FALSE);
}

void PreviewWindow::Paint() noexcept {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    if (dc) {
        FillRect(dc, &paint.rcPaint, GetSysColorBrush(COLOR_BTNFACE));
        const BrushHandle matte(CreateSolidBrush(RGB(22, 30, 42)));
        if (matte) {
            FillRect(dc, &canvas_, matte.get());
        }
        RECT inner = canvas_;
        InflateRect(&inner, -Dip(8, dpi_), -Dip(8, dpi_));
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        DrawImage(dc, image_, FitImage(image_, inner));
    }
    EndPaint(window_, &paint);
}

}  // namespace picolet
