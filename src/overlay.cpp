#include "overlay.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>

namespace picolet {
namespace {

constexpr wchar_t kOverlayClass[] = L"Picolet.SelectionOverlay";
constexpr int kTextLimit = 2048;
constexpr COLORREF kAccent = RGB(62, 230, 193);
constexpr COLORREF kGripBorder = RGB(20, 29, 43);

void Fill(HDC dc, const RECT& rect, COLORREF color) noexcept {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Border(HDC dc, const RECT& rect, COLORREF color) noexcept {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

bool SamePoint(POINT a, POINT b) noexcept { return a.x == b.x && a.y == b.y; }

// 生成 32×32 原生光标，中心为点击热点。掩码从每行的高位开始，留空处显示底图，
// 十字中心线显示白色，外扩一像素显示黑色。返回句柄由遮罩对象持有并在进程退出时释放。
HCURSOR CreateCaptureCursor(HINSTANCE instance) noexcept {
    constexpr int size = 32;
    constexpr int center = size / 2;
    constexpr int arm = 8;
    std::array<BYTE, size * size / 8> andMask{};
    std::array<BYTE, size * size / 8> xorMask{};
    andMask.fill(0xff);
    for (int y = center - arm - 1; y <= center + arm + 1; ++y) {
        for (int x = center - arm - 1; x <= center + arm + 1; ++x) {
            const int dx = std::abs(x - center);
            const int dy = std::abs(y - center);
            if (dx > 1 && dy > 1) continue;
            const auto index = static_cast<std::size_t>(y * (size / 8) + x / 8);
            const auto bit = static_cast<BYTE>(0x80 >> (x % 8));
            andMask[index] &= static_cast<BYTE>(~bit);
            if ((dx == 0 && dy <= arm) || (dy == 0 && dx <= arm)) xorMask[index] |= bit;
        }
    }
    return CreateCursor(instance, center, center, size, size, andMask.data(), xorMask.data());
}

// 读取指定显示器的界面 DPI。遮罩跨屏时，窗口 DPI 只用作查询失败的回退值。
UINT ScreenDpi(HMONITOR monitor, HWND window) noexcept {
    UINT x = 0, y = 0;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) || !x) x = GetDpiForWindow(window);
    return x ? x : USER_DEFAULT_SCREEN_DPI;
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

HRESULT SelectionOverlay::Open(HINSTANCE instance, HWND owner, const DesktopFrame& frame, POINT anchor,
                               const ShortcutBindings& shortcuts) noexcept {
    const auto& image = frame.image;
    const HRESULT allocated = dark_.Resize(image.width(), image.height());
    if (FAILED(allocated)) return allocated;
    // 遮罩显示冻帧的暗色副本，选区显示未缩放的原图和标注合成块。
    std::ranges::transform(image.pixels(), dark_.pixels().begin(), [](std::uint32_t pixel) {
        return 0xff000000u | ((pixel >> 1) & 0x007f7f7fu);
    });
    frame_ = &frame;
    editorMode_ = suspended_ = false;
    owner_ = owner;
    shortcuts_ = shortcuts;
    anchor_ = anchor;
    selection_ = {};
    tool_ = EditTool::Select;
    drag_ = Drag::None;
    status_ = nullptr;
    activated_ = invalidated_ = pending_ = busy_ = closing_ = composingText_ = paletteOpen_ = false;
    if (!crosshair_) crosshair_.reset(CreateCaptureCursor(instance));

    const auto& bounds = frame.bounds;
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"Picolet 截图",
                                        WS_POPUP | WS_CLIPCHILDREN, bounds.left, bounds.top,
                                        image.width(), image.height(), owner, nullptr, instance, this);
    if (!window || !toolbar_.Create(window, instance, shortcuts)) {
        const HRESULT error = LastErrorResult();
        Close();
        return error;
    }
    LayoutPanel();
    if (!SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE)) {
        const HRESULT error = LastErrorResult();
        Close();
        return error;
    }
    SetForegroundWindow(window_);
    UpdateWindow(window_);
    RefreshCursor();
    return S_OK;
}

void SelectionOverlay::Cancel() noexcept {
    if (!window_ || closing_) return;
    invalidated_ = true;
    if (!busy_) PostAction(OverlayAction::Cancel);
}

void SelectionOverlay::Close() noexcept {
    closing_ = true;
    drag_ = Drag::None;
    toolbar_.Close();
    if (window_) {
        if (GetCapture() == window_) ReleaseCapture();
        DestroyWindow(window_);
    }
    textEdit_ = nullptr;
    textFont_.reset();
    frame_ = nullptr;
    dark_ = {};
    history_.Clear();
    draft_ = {};
    fitButton_ = actualButton_ = nullptr;
    overviewImage_ = {};
    editorFont_.reset();
    editorFontDpi_ = 0;
    suspended_ = false;
    paintBuffer_.Reset();
}

void SelectionOverlay::Suspend() noexcept {
    suspended_ = true;
    pending_ = false;
    toolbar_.Hide();
    ShowWindow(window_, SW_HIDE);
}

void SelectionOverlay::Resume(const wchar_t* status) noexcept {
    suspended_ = false;
    pending_ = busy_ = false;
    ShowWindow(window_, SW_SHOW);
    Retry(status);
    InvalidateRect(window_, nullptr, FALSE);
}

HRESULT SelectionOverlay::CaptureSeed(Image& result) const noexcept {
    return frame_ ? CropImage(*frame_, selection_, result) : E_UNEXPECTED;
}

HRESULT SelectionOverlay::MapHistory(const SeedPlacement& placement, const DesktopFrame& target,
                                      AnnotationHistory& result) const noexcept {
    return frame_ ? history_.MapToLongImage(*frame_, selection_, placement, target, result) : E_UNEXPECTED;
}

void SelectionOverlay::BeginOutput() noexcept {
    busy_ = true;
    paletteOpen_ = false;
    toolbar_.Hide();
}

void SelectionOverlay::Retry(const wchar_t* status) noexcept {
    busy_ = pending_ = false;
    status_ = status;
    LayoutPanel();
    SetForegroundWindow(window_);
    SetFocus(window_);
}

HRESULT SelectionOverlay::Render(Image& result) const noexcept {
    return frame_ ? history_.Render(*frame_, selection_, result) : E_UNEXPECTED;
}

bool SelectionOverlay::HandleKey(const MSG& message) noexcept {
    if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd)) ||
        message.hwnd == textEdit_ || busy_ || pending_)
        return false;
    const bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    const bool snapshotUp = (message.message == WM_KEYUP || message.message == WM_SYSKEYUP) && message.wParam == VK_SNAPSHOT;
    if (!down && !snapshotUp) return false;
    if (down && message.wParam == VK_SNAPSHOT) return false;
    const auto mods = CurrentModifiers();
    const bool repeat = down && (HIWORD(message.lParam) & KF_REPEAT);
    if (down && !mods && toolbar_.IsSlider(message.hwnd)) {
        switch (message.wParam) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            return false;
        }
    }
    if (!textEdit_) {
        if (const auto shortcut = MatchLocalShortcut(shortcuts_, {mods, static_cast<UINT>(message.wParam)})) {
            if (!repeat && (drag_ == Drag::None || *shortcut == Shortcut::Cancel)) Command(*shortcut);
            return true;
        }
    } else if (down && ((message.wParam == VK_ESCAPE && !mods) ||
                        (message.wParam == VK_RETURN && mods == MOD_CONTROL))) {
        if (!repeat && !composingText_) FinishText(message.wParam == VK_RETURN);
        return true;
    }
    if (down && message.wParam == VK_TAB && !(mods & ~MOD_SHIFT) && drag_ == Drag::None) {
        const HWND current = message.hwnd == window_ ? nullptr : message.hwnd;
        if (const HWND next = GetNextDlgTabItem(window_, current, (mods & MOD_SHIFT) != 0)) SetFocus(next);
        return true;
    }
    if (down && message.wParam == VK_RETURN && !mods && (toolbar_.IsButton(message.hwnd) ||
        (editorMode_ && (message.hwnd == fitButton_ || message.hwnd == actualButton_)))) {
        if (!repeat) SendMessageW(message.hwnd, BM_CLICK, 0, 0);
        return true;
    }
    if (editorMode_ && down && !mods && message.hwnd == window_) {
        switch (message.wParam) {
        case VK_UP: ScrollEditor(SB_VERT, SB_LINEUP); return true;
        case VK_DOWN: ScrollEditor(SB_VERT, SB_LINEDOWN); return true;
        case VK_LEFT: ScrollEditor(SB_HORZ, SB_LINEUP); return true;
        case VK_RIGHT: ScrollEditor(SB_HORZ, SB_LINEDOWN); return true;
        case VK_PRIOR: ScrollEditor(SB_VERT, SB_PAGEUP); return true;
        case VK_NEXT: ScrollEditor(SB_VERT, SB_PAGEDOWN); return true;
        case VK_HOME: ScrollEditor(SB_VERT, SB_TOP); return true;
        case VK_END: ScrollEditor(SB_VERT, SB_BOTTOM); return true;
        }
    }
    return false;
}

LRESULT CALLBACK SelectionOverlay::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<SelectionOverlay*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<SelectionOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->window_ = nullptr;
        self = nullptr;
    }
    return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SelectionOverlay::Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_DRAWITEM:
        if (toolbar_.DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_NOTIFY:
        if (const auto result = toolbar_.DrawSlider(*reinterpret_cast<const NMHDR*>(lparam))) return *result;
        break;
    case WM_HSCROLL:
        if (toolbar_.IsSlider(reinterpret_cast<HWND>(lparam))) {
            if (!busy_ && !pending_) AdjustSize();
            return 0;
        }
        if (editorMode_ && !lparam) { ScrollEditor(SB_HORZ, LOWORD(wparam)); return 0; }
        break;
    case WM_VSCROLL:
        if (editorMode_) { ScrollEditor(SB_VERT, LOWORD(wparam)); return 0; }
        break;
    case WM_SIZE:
        if (editorMode_ && !composingText_) {
            if (textEdit_) FinishText(true);
            LayoutEditor();
            InvalidateRect(window_, nullptr, FALSE);
        }
        break;
    case WM_GETMINMAXINFO:
        if (editorMode_) {
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lparam);
            limits.ptMinTrackSize = {Dip(560, dpi_), Dip(360, dpi_)};
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (editorMode_ && !busy_ && drag_ == Drag::None && ChangeView()) {
            const auto before = viewport_;
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL)
                viewport_.ZoomAt(viewport_.zoom * std::pow(1.2, double(delta) / WHEEL_DELTA), ClientCursor());
            else if (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) viewport_.scrollX -= double(delta) / WHEEL_DELTA * Dip(90, dpi_);
            else viewport_.scrollY -= double(delta) / WHEEL_DELTA * Dip(90, dpi_);
            viewport_.Clamp();
            RefreshEditorView(before);
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
        if (textEdit_ && reinterpret_cast<HWND>(lparam) == textEdit_) {
            // 让白色及亮色标注在输入时仍然可读，草稿最终只导出文字本身。
            const auto color = draft_.color;
            const bool light = GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114 >= 155000;
            const auto dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, color);
            SetBkColor(dc, light ? RGB(0, 0, 0) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(light ? BLACK_BRUSH : WHITE_BRUSH));
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            activated_ = true;
            if (!busy_ && !closing_) SetFocus(textEdit_ ? textEdit_ : window_);
        } else if (activated_ && !busy_ && !closing_ && !suspended_ && !editorMode_) {
            Cancel();
        }
        return 0;
    case WM_SETCURSOR:
        // 只接管客户区的画布及工具栏光标。边框、标题栏和滚动条交给系统，
        // 避免长图编辑窗口的原生缩放光标被画布光标覆盖。
        if (LOWORD(lparam) != HTCLIENT) break;
        if (toolbar_.IsButton(reinterpret_cast<HWND>(wparam)) || toolbar_.IsSlider(reinterpret_cast<HWND>(wparam))) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (reinterpret_cast<HWND>(wparam) == window_ && frame_) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                SetCanvasCursor(cursor);
                return TRUE;
            }
        }
        break;
    case WM_COMMAND:
        if (editorMode_ && (reinterpret_cast<HWND>(lparam) == fitButton_ || reinterpret_cast<HWND>(lparam) == actualButton_)) {
            if (!busy_ && !pending_ && ChangeView()) {
                const auto before = viewport_;
                if (reinterpret_cast<HWND>(lparam) == fitButton_) viewport_.FitWidth();
                else viewport_.ZoomAt(1.0, {viewport_.canvas.left, viewport_.canvas.top});
                RefreshEditorView(before);
            }
            return 0;
        }
        if (const auto button = toolbar_.Clicked(wparam, lparam)) {
            if (!pending_ && !busy_) PressButton(*button);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (!busy_ && !pending_ && drag_ == Drag::None) {
            const auto point = MessagePoint();
            const POINT client = editorMode_ ? ClientCursor() : POINT{point.x - frame_->bounds.left, point.y - frame_->bounds.top};
            if (editorMode_ && PtInRect(&overview_, client)) {
                if (ChangeView()) {
                    const auto before = viewport_;
                    viewport_.scrollY = double(client.y - overview_.top) / std::max(1L, overview_.bottom - overview_.top) *
                        frame_->image.height() * viewport_.zoom - (viewport_.canvas.bottom - viewport_.canvas.top) / 2;
                    viewport_.Clamp();
                    RefreshEditorView(before);
                }
                return 0;
            }
            if (editorMode_ && !PtInRect(&viewport_.canvas, client)) return 0;
            if (!toolbar_.Contains(client)) {
                const bool hadPalette = std::exchange(paletteOpen_, false);
                if (FinishText(true)) BeginDrag(point);
                if (hadPalette) LayoutPanel();
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (drag_ != Drag::None) UpdateDrag(MessagePoint());
        return 0;
    case WM_LBUTTONUP:
        if (drag_ != Drag::None) EndDrag(MessagePoint());
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        if (drag_ != Drag::None) AbortDrag();
        return 0;
    case WM_RBUTTONUP:
        if (!busy_) Cancel();
        return 0;
    case WM_DISPLAYCHANGE:
        if (suspended_) return 0;
        if (editorMode_) { LayoutEditor(); InvalidateRect(window_, nullptr, FALSE); return 0; }
        Cancel();
        return 0;
    case WM_CLOSE:
        Cancel();
        return 0;
    case WM_DPICHANGED:
        if (editorMode_) {
            dpi_ = HIWORD(wparam);
            const auto& suggested = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window_, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                         suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutEditor();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (activated_ && !closing_ && !suspended_) Cancel();
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void SelectionOverlay::Command(Shortcut shortcut) noexcept {
    switch (shortcut) {
    case Shortcut::Fullscreen:
        if (editorMode_) {
            if (ChangeView()) { const auto before = viewport_; viewport_.FitWidth(); RefreshEditorView(before); }
        } else SelectFullscreen();
        break;
    case Shortcut::LongCapture:
        if (!editorMode_ && FinishText(true)) PostAction(OverlayAction::LongCapture);
        break;
    case Shortcut::Copy: PostAction(OverlayAction::Copy); break;
    case Shortcut::Save: PostAction(OverlayAction::Save); break;
    case Shortcut::Pin: PostAction(OverlayAction::Pin); break;
    case Shortcut::Undo:
    case Shortcut::Redo: {
        const bool undo = shortcut == Shortcut::Undo;
        const auto result = undo ? history_.Undo(*frame_) : history_.Redo(*frame_);
        status_ = FAILED(result) ? (undo ? L"无法撤销，请重试。" : L"无法重做，请重试。") : nullptr;
        RepaintSelection(selection_, result == S_OK);
        LayoutPanel();
        break;
    }
    case Shortcut::Cancel: Cancel(); break;
    default: break;
    }
}

void SelectionOverlay::PressButton(ToolbarButton button) noexcept {
    using Button = ToolbarButton;
    if (button == Button::Cancel) {
        Cancel();
        return;
    }
    // 修改样式时保留正在输入的文字。切换工具或输出时才提交草稿。
    if (button == Button::Color) {
        paletteOpen_ = !paletteOpen_;
        LayoutPanel();
        return;
    }
    if (button >= Button::Red && button <= Button::Black) {
        style_.color = static_cast<std::size_t>(button) - static_cast<std::size_t>(Button::Red);
        paletteOpen_ = false;
        UpdateTextStyle();
        LayoutPanel();
        if (textEdit_) SetFocus(textEdit_);
        return;
    }
    if (button == Button::Outline || button == Button::Solid) {
        style_.filled = button == Button::Solid;
        paletteOpen_ = false;
        LayoutPanel();
        return;
    }
    if (!FinishText(button != Button::Reselect)) return;
    paletteOpen_ = false;
    status_ = nullptr;
    switch (button) {
    case Button::Select: tool_ = EditTool::Select; break;
    case Button::Shapes: tool_ = shape_; break;
    case Button::Rectangle: tool_ = shape_ = EditTool::Rectangle; break;
    case Button::Ellipse: tool_ = shape_ = EditTool::Ellipse; break;
    case Button::Line: tool_ = shape_ = EditTool::Line; break;
    case Button::Arrow: tool_ = EditTool::Arrow; break;
    case Button::Pen: tool_ = EditTool::Pen; break;
    case Button::Text: tool_ = EditTool::Text; break;
    case Button::Mosaic: tool_ = EditTool::Mosaic; break;
    case Button::Reselect: {
        const RECT previous = selection_;
        selection_ = {};
        tool_ = EditTool::Select;
        history_.Clear();
        GetCursorPos(&anchor_);
        RepaintSelection(previous);
        break;
    }
    case Button::Fullscreen: Command(Shortcut::Fullscreen); return;
    case Button::LongCapture: Command(Shortcut::LongCapture); return;
    case Button::Undo: Command(Shortcut::Undo); return;
    case Button::Redo: Command(Shortcut::Redo); return;
    case Button::Copy: Command(Shortcut::Copy); return;
    case Button::Save: Command(Shortcut::Save); return;
    case Button::Pin: Command(Shortcut::Pin); return;
    default: return;
    }
    LayoutPanel();
    RepaintSelection(selection_, false);
}

void SelectionOverlay::AdjustSize() noexcept {
    const auto value = toolbar_.SliderValue();
    if (tool_ == EditTool::Text) {
        style_.textSize = std::clamp(value, ToolbarStyle::kMinTextSize, ToolbarStyle::kMaxTextSize);
        UpdateTextStyle();
        toolbar_.SetValue(style_.textSize);
    } else {
        style_.width = std::clamp(value, ToolbarStyle::kMinWidth, ToolbarStyle::kMaxWidth);
        toolbar_.SetValue(style_.width);
    }
}

void SelectionOverlay::PostAction(OverlayAction action) noexcept {
    if (pending_ || busy_ || closing_ || (action != OverlayAction::Cancel && IsRectEmpty(&selection_))) return;
    pending_ = true;
    // 让按钮和窗口过程先返回，再由控制窗口销毁遮罩或打开系统对话框。
    if (!PostMessageW(owner_, kOverlayAction, static_cast<WPARAM>(action), reinterpret_cast<LPARAM>(window_))) {
        pending_ = false;
        if (action == OverlayAction::Cancel) SendMessageW(owner_, kOverlayAction, static_cast<WPARAM>(action), reinterpret_cast<LPARAM>(window_));
        else Status(L"无法执行截图操作，请重试。");
    }
}

void SelectionOverlay::SelectFullscreen() noexcept {
    POINT cursor = anchor_;
    GetCursorPos(&cursor);
    const RECT selected = SelectMonitor(cursor, frame_->bounds);
    if (IsRectEmpty(&selected)) {
        Status(L"无法确定当前显示器。");
        return;
    }
    const auto previous = selection_;
    selection_ = selected;
    tool_ = EditTool::Select;
    paletteOpen_ = false;
    status_ = nullptr;
    RepaintSelection(previous);
    LayoutPanel();
}

POINT SelectionOverlay::MessagePoint() const noexcept {
    if (editorMode_) return viewport_.ToImage(ClientCursor());
    const auto& bounds = frame_->bounds;
    if (bounds.left >= -32768 && bounds.top >= -32768 && bounds.right <= 32767 && bounds.bottom <= 32767) {
        const DWORD position = GetMessagePos();
        return {GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    }
    POINT point{};
    GetCursorPos(&point);
    return point;
}

void SelectionOverlay::SetCanvasCursor(POINT point) const noexcept {
    if (editorMode_) {
        const auto client = ClientCursor();
        LPCWSTR cursor = !PtInRect(&viewport_.canvas, client) || busy_ ? IDC_ARROW :
            tool_ == EditTool::Select ? IDC_HAND : tool_ == EditTool::Text ? IDC_IBEAM : IDC_CROSS;
        SetCursor(cursor == IDC_CROSS && crosshair_ ? crosshair_.get() : LoadCursorW(nullptr, cursor));
        return;
    }
    LPCWSTR shape = IDC_CROSS;
    const POINT client{point.x - frame_->bounds.left, point.y - frame_->bounds.top};
    if (drag_ == Drag::None && (busy_ || toolbar_.Contains(client))) {
        shape = IDC_ARROW;
    } else if (drag_ == Drag::Move) {
        shape = IDC_SIZEALL;
    } else if (drag_ == Drag::Resize || (drag_ == Drag::None && tool_ == EditTool::Select)) {
        const unsigned edges = drag_ == Drag::Resize ? edges_ : HitEdges(point);
        if (edges == (EdgeLeft | EdgeTop) || edges == (EdgeRight | EdgeBottom)) shape = IDC_SIZENWSE;
        else if (edges == (EdgeLeft | EdgeBottom) || edges == (EdgeRight | EdgeTop)) shape = IDC_SIZENESW;
        else if (edges & (EdgeLeft | EdgeRight)) shape = IDC_SIZEWE;
        else if (edges & (EdgeTop | EdgeBottom)) shape = IDC_SIZENS;
        else if (PtInRect(&selection_, point)) shape = IDC_SIZEALL;
    } else if (tool_ == EditTool::Text && PtInRect(&selection_, point)) {
        shape = IDC_IBEAM;
    }
    SetCursor(shape == IDC_CROSS && crosshair_ ? crosshair_.get() : LoadCursorW(nullptr, shape));
}

void SelectionOverlay::RefreshCursor() const noexcept {
    POINT point{};
    if (!window_ || !GetCursorPos(&point) || WindowFromPoint(point) != window_) return;
    POINT client = point;
    RECT bounds{};
    // WindowFromPoint 也可能命中本窗口的非客户区；主动同步时同样须保留系统边框光标。
    if (!ScreenToClient(window_, &client) || !GetClientRect(window_, &bounds) || !PtInRect(&bounds, client)) return;
    SetCanvasCursor(point);
}

unsigned SelectionOverlay::HitEdges(POINT point) const noexcept {
    if (IsRectEmpty(&selection_)) return EdgeNone;
    const LONG grip = Dip(6, dpi_);
    RECT hit = selection_;
    InflateRect(&hit, grip, grip);
    if (!PtInRect(&hit, point)) return EdgeNone;
    const LONG left = std::abs(point.x - selection_.left);
    const LONG right = std::abs(point.x - (selection_.right - 1));
    const LONG top = std::abs(point.y - selection_.top);
    const LONG bottom = std::abs(point.y - (selection_.bottom - 1));
    unsigned edges = EdgeNone;
    if (std::min(left, right) <= grip) edges |= left <= right ? EdgeLeft : EdgeRight;
    if (std::min(top, bottom) <= grip) edges |= top <= bottom ? EdgeTop : EdgeBottom;
    return edges;
}

Annotation SelectionOverlay::MakeMark(POINT point) const noexcept {
    Annotation mark;
    mark.tool = tool_;
    mark.first = mark.last = point;
    mark.clip = selection_;
    mark.color = kToolbarColors[style_.color];
    mark.width = style_.width;
    mark.textSize = style_.textSize;
    mark.filled = style_.filled && (tool_ == EditTool::Rectangle || tool_ == EditTool::Ellipse);
    return mark;
}

void SelectionOverlay::BeginDrag(POINT point) noexcept {
    start_ = point;
    original_ = selection_;
    if (editorMode_ && tool_ == EditTool::Select) {
        drag_ = Drag::Pan;
        panStart_ = ClientCursor();
        panX_ = viewport_.scrollX;
        panY_ = viewport_.scrollY;
    } else if (IsRectEmpty(&selection_)) {
        drag_ = Drag::Create;
    } else if (tool_ == EditTool::Select) {
        edges_ = HitEdges(point);
        if (edges_ != EdgeNone) drag_ = Drag::Resize;
        else if (PtInRect(&selection_, point)) drag_ = Drag::Move;
    } else if (PtInRect(&selection_, point)) {
        if (tool_ == EditTool::Text) {
            BeginText(point);
            return;
        }
        draft_ = MakeMark(point);
        drag_ = Drag::Mark;
    }
    if (drag_ != Drag::None) {
        const bool hadStatus = status_ != nullptr;
        status_ = nullptr;
        if (editorMode_ && hadStatus) LayoutEditor();
        SetFocus(window_);
        if (!editorMode_) toolbar_.Hide();
        SetCapture(window_);
        if (GetCapture() != window_) {
            AbortDrag();
            return;
        }
        SetCanvasCursor(point);
        if (!editorMode_ && drag_ != Drag::Mark && !IsRectEmpty(&selection_)) {
            toolbar_.LayoutDimension(DimensionScreen(), ToClient(selection_));
            RepaintSelection(selection_);
        }
    }
}

void SelectionOverlay::UpdateDrag(POINT point) noexcept {
    const RECT previous = selection_;
    const POINT delta{point.x - start_.x, point.y - start_.y};
    switch (drag_) {
    case Drag::Pan: {
        const auto before = viewport_;
        const auto cursor = ClientCursor();
        viewport_.scrollX = panX_ - (cursor.x - panStart_.x);
        viewport_.scrollY = panY_ - (cursor.y - panStart_.y);
        viewport_.Clamp();
        RefreshEditorView(before);
        return;
    }
    case Drag::Create: selection_ = SelectionRect(start_, point, frame_->bounds); break;
    case Drag::Move: selection_ = MoveSelection(original_, delta, frame_->bounds); break;
    case Drag::Resize: selection_ = ResizeSelection(original_, edges_, delta, frame_->bounds); break;
    case Drag::Mark:
        point.x = std::clamp(point.x, selection_.left, selection_.right - 1);
        point.y = std::clamp(point.y, selection_.top, selection_.bottom - 1);
        if (SamePoint(point, draft_.last)) return;
        if (draft_.tool == EditTool::Mosaic) {
            InvalidateMosaicDraft();
            draft_.last = point;
            InvalidateMosaicDraft();
            return;
        }
        if (draft_.tool == EditTool::Pen) {
            try {
                if (draft_.path.empty()) draft_.path.push_back(draft_.first);
                draft_.path.push_back(point);
            } catch (...) {
                AbortDrag();
                Status(L"无法继续绘制，请重试。");
                return;
            }
        }
        draft_.last = point;
        RepaintSelection(previous, false);
        return;
    default: return;
    }
    if (!EqualRect(&selection_, &previous)) {
        toolbar_.LayoutDimension(DimensionScreen(), ToClient(selection_));
        RepaintSelection(previous);
    }
}

void SelectionOverlay::EndDrag(POINT point) noexcept {
    UpdateDrag(point);
    const auto finished = std::exchange(drag_, Drag::None);
    if (GetCapture() == window_) ReleaseCapture();
    if (finished == Drag::Pan) { RefreshCursor(); return; }
    bool contentChanged = false;
    if (finished == Drag::Mark && (draft_.tool == EditTool::Pen || !SamePoint(draft_.first, draft_.last))) {
        const auto result = history_.Commit(*frame_, std::move(draft_));
        contentChanged = result == S_OK;
        if (FAILED(result)) status_ = L"无法应用标注，请重试。";
    }
    draft_ = {};
    LayoutPanel();
    RepaintSelection(selection_, contentChanged);
    RefreshCursor();
}

void SelectionOverlay::AbortDrag() noexcept {
    const RECT previous = selection_;
    const auto aborted = std::exchange(drag_, Drag::None);
    selection_ = original_;
    draft_ = {};
    if (GetCapture() == window_) ReleaseCapture();
    if (aborted == Drag::Pan) { RefreshCursor(); return; }
    LayoutPanel();
    RepaintSelection(previous, false);
    RefreshCursor();
}

void SelectionOverlay::BeginText(POINT point) noexcept {
    const LONG width = std::min<LONG>(selection_.right - selection_.left, Dip(220, dpi_));
    const LONG height = std::min<LONG>(selection_.bottom - selection_.top, Dip(100, dpi_));
    point.x = std::min(point.x, selection_.right - width);
    point.y = std::min(point.y, selection_.bottom - height);
    draft_ = MakeMark(point);
    textFont_.reset(CreateAnnotationFont(editorMode_ ? std::max(1, static_cast<int>(std::lround(draft_.textSize * viewport_.zoom))) : draft_.textSize));
    RECT box = ToClient({point.x, point.y, selection_.right, point.y + height});
    if (editorMode_) {
        box.left = std::max(box.left, viewport_.canvas.left);
        box.top = std::max(box.top, viewport_.canvas.top);
        box.right = std::min(box.right, viewport_.canvas.right);
        box.bottom = std::min(viewport_.canvas.bottom, std::max(box.bottom, box.top + Dip(70, dpi_)));
        draft_.clip.right = std::min(selection_.right, viewport_.ToImage({box.right, box.top}).x);
    }
    textEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                                box.left, box.top, box.right - box.left, box.bottom - box.top,
                                window_, nullptr, nullptr, nullptr);
    if (!textEdit_ || !SetWindowSubclass(textEdit_, TextProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
        FinishText(false);
        textFont_.reset();
        Status(L"无法打开文字输入框，请重试。");
        return;
    }
    composingText_ = false;
    if (textFont_) SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont_.get()), TRUE);
    SendMessageW(textEdit_, EM_SETLIMITTEXT, kTextLimit, 0);
    SetWindowPos(textEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Status(L"Ctrl+Enter 提交文字，Esc 取消文字");
    SetFocus(textEdit_);
}

void SelectionOverlay::UpdateTextStyle() noexcept {
    if (!textEdit_) return;
    draft_.color = kToolbarColors[style_.color];
    if (style_.textSize != draft_.textSize) {
        if (const HFONT font = CreateAnnotationFont(editorMode_ ? std::max(1, static_cast<int>(std::lround(style_.textSize * viewport_.zoom))) : style_.textSize)) {
            SendMessageW(textEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            textFont_.reset(font);
            draft_.textSize = style_.textSize;
        }
    }
    InvalidateRect(textEdit_, nullptr, TRUE);
}

bool SelectionOverlay::FinishText(bool commit) noexcept {
    if (!textEdit_) return true;
    if (commit) {
        wchar_t text[kTextLimit + 1]{};
        const int length = GetWindowTextW(textEdit_, text, static_cast<int>(std::size(text)));
        if (length) {
            Annotation mark = MakeMark(draft_.first);
            mark.clip = draft_.clip;
            mark.textSize = draft_.textSize;
            try {
                mark.text.assign(text, static_cast<std::size_t>(length));
            } catch (...) {
                Status(L"无法读取文字，请重试。");
                return false;
            }
            if (FAILED(history_.Commit(*frame_, std::move(mark)))) {
                Status(L"无法应用文字，请重试。");
                return false;
            }
        }
    }
    DestroyWindow(std::exchange(textEdit_, nullptr));
    textFont_.reset();
    draft_ = {};
    composingText_ = false;
    paletteOpen_ = false;
    status_ = nullptr;
    SetFocus(window_);
    LayoutPanel();
    RepaintSelection(selection_);
    return true;
}

LRESULT CALLBACK SelectionOverlay::TextProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                           UINT_PTR id, DWORD_PTR data) noexcept {
    auto& self = *reinterpret_cast<SelectionOverlay*>(data);
    if (message == WM_IME_STARTCOMPOSITION) self.composingText_ = true;
    if (message == WM_IME_ENDCOMPOSITION) self.composingText_ = false;
    if (message == WM_KEYDOWN && !self.composingText_) {
        if (wparam == VK_ESCAPE) {
            self.FinishText(false);
            return 0;
        }
        if (wparam == VK_RETURN && CurrentModifiers() == MOD_CONTROL) {
            self.FinishText(true);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, TextProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

RECT SelectionOverlay::ToClient(RECT rect) const noexcept {
    if (editorMode_) return viewport_.ToClient(rect);
    if (!IsRectEmpty(&rect)) OffsetRect(&rect, -frame_->bounds.left, -frame_->bounds.top);
    return rect;
}

ToolbarArea SelectionOverlay::DimensionScreen() const noexcept {
    const HMONITOR monitor = MonitorFromPoint({selection_.left, selection_.top}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT bounds{};
    if (!GetMonitorInfoW(monitor, &info) || !IntersectRect(&bounds, &info.rcMonitor, &frame_->bounds)) {
        bounds = frame_->bounds;
    }
    return {ToClient(bounds), ScreenDpi(monitor, window_)};
}

void SelectionOverlay::LayoutPanel() noexcept {
    if (editorMode_) { LayoutEditor(); return; }
    if (closing_ || busy_ || drag_ != Drag::None || !window_) return;
    const bool selected = !IsRectEmpty(&selection_);
    const HMONITOR monitor = selected ? MonitorFromRect(&selection_, MONITOR_DEFAULTTONEAREST)
                                      : MonitorFromPoint(anchor_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{};
    if (!GetMonitorInfoW(monitor, &info) || !IntersectRect(&work, &info.rcWork, &frame_->bounds)) work = frame_->bounds;
    dpi_ = ScreenDpi(monitor, window_);
    const ToolbarView view{tool_, style_, !history_.empty(), history_.can_redo() && !textEdit_, paletteOpen_};
    const ToolbarArea area{ToClient(work), dpi_};
    toolbar_.Layout(area, selected ? DimensionScreen() : area, ToClient(selection_), view, status_);
}

void SelectionOverlay::Status(const wchar_t* text) noexcept {
    status_ = text;
    LayoutPanel();
}

void SelectionOverlay::RepaintSelection(const RECT& previous, bool contentChanged) noexcept {
    if (editorMode_) {
        if (contentChanged) {
            overviewImage_ = {};
            if (!IsRectEmpty(&overview_)) InvalidateRect(window_, &overview_, FALSE);
        }
        InvalidateRect(window_, &viewport_.canvas, FALSE);
        return;
    }
    RECT changed{};
    if (UnionRect(&changed, &previous, &selection_)) {
        changed = ToClient(changed);
        InflateRect(&changed, Dip(8, dpi_), Dip(8, dpi_));
        InvalidateRect(window_, &changed, FALSE);
    }
}

RECT SelectionOverlay::MosaicDraftArea() const noexcept {
    return SelectionRect(draft_.first, draft_.last, draft_.clip);
}

void SelectionOverlay::InvalidateMosaicDraft() const noexcept {
    const RECT area = ToClient(MosaicDraftArea());
    if (!IsRectEmpty(&area)) InvalidateRect(window_, &area, FALSE);
}

void SelectionOverlay::PaintDraft(HDC dc) const noexcept {
    if (draft_.tool == EditTool::Mosaic) {
        const RECT area = MosaicDraftArea();
        const RECT client = ToClient(area);
        if (IsRectEmpty(&client) || !RectVisible(dc, &client)) return;
        Image preview;
        if (history_.RenderMosaicDraft(*frame_, area, preview) == S_OK) {
            DrawImage(dc, preview, client);
            return;
        }
    }
    if (editorMode_) {
        const int saved = SaveDC(dc);
        const auto origin = viewport_.Origin();
        SetGraphicsMode(dc, GM_ADVANCED);
        const XFORM transform{static_cast<FLOAT>(viewport_.zoom), 0, 0, static_cast<FLOAT>(viewport_.zoom),
                              static_cast<FLOAT>(origin.x), static_cast<FLOAT>(origin.y)};
        SetWorldTransform(dc, &transform);
        DrawAnnotationDraft(dc, draft_, {0, 0});
        if (saved) RestoreDC(dc, saved);
    } else DrawAnnotationDraft(dc, draft_, {frame_->bounds.left, frame_->bounds.top});
}

void SelectionOverlay::Paint() noexcept {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window_, &paint);
    if (!target || !frame_) {
        EndPaint(window_, &paint);
        return;
    }
    {
        // 底图及工具栏最后只提交一次，避免屏幕先看到灰色/暗色背景再看到标签。
        BufferedPaint drawing(paintBuffer_, target, paint.rcPaint);
        if (editorMode_) PaintEditor(drawing.dc());
        else PaintSelection(drawing.dc());
    }
    EndPaint(window_, &paint);
}

void SelectionOverlay::PaintSelection(HDC dc) const noexcept {
    const RECT canvas{0, 0, dark_.width(), dark_.height()};
    const RECT selected = ToClient(selection_);
    const bool hasSelection = !IsRectEmpty(&selected);
    if (const int saved = SaveDC(dc)) {
        if (hasSelection) ExcludeClipRect(dc, selected.left, selected.top, selected.right, selected.bottom);
        DrawImage(dc, dark_, canvas);
        RestoreDC(dc, saved);
    }
    if (hasSelection) {
        if (const int saved = SaveDC(dc)) {
            IntersectClipRect(dc, selected.left, selected.top, selected.right, selected.bottom);
            DrawImage(dc, frame_->image, canvas);
            const auto& patch = history_.patch();
            DrawImage(dc, patch.image, ToClient(patch.bounds));
            if (drag_ == Drag::Mark) PaintDraft(dc);
            RestoreDC(dc, saved);
        }
        Border(dc, selected, kAccent);
        if (tool_ == EditTool::Select && drag_ == Drag::None && !busy_) {
            const int radius = Dip(3, dpi_);
            const LONG midX = selected.left + (selected.right - selected.left) / 2;
            const LONG midY = selected.top + (selected.bottom - selected.top) / 2;
            const POINT handles[]{{selected.left, selected.top}, {midX, selected.top}, {selected.right - 1, selected.top},
                {selected.left, midY}, {selected.right - 1, midY}, {selected.left, selected.bottom - 1},
                {midX, selected.bottom - 1}, {selected.right - 1, selected.bottom - 1}};
            for (const auto point : handles) {
                const RECT grip{point.x - radius, point.y - radius, point.x + radius + 1, point.y + radius + 1};
                Fill(dc, grip, kAccent);
                Border(dc, grip, kGripBorder);
            }
        }
    }
    toolbar_.Paint(dc);
}

}  // namespace picolet
