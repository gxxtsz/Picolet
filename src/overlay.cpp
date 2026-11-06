#include "overlay.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <limits>

namespace picolet {
namespace {

constexpr wchar_t kOverlayClass[] = L"Picolet.SelectionOverlay";
constexpr int kButtonBase = 4000;
constexpr int kTextLimit = 2048;
constexpr int kWidths[]{2, 4, 8};
constexpr int kTextSizes[]{16, 24, 36};
constexpr COLORREF kColors[]{RGB(239, 68, 68), RGB(250, 204, 21), RGB(74, 222, 128),
                             RGB(59, 130, 246), RGB(255, 255, 255), RGB(15, 23, 42)};
constexpr COLORREF kAccent = RGB(62, 230, 193);
constexpr COLORREF kPanel = RGB(20, 29, 43);
constexpr COLORREF kPanelBorder = RGB(61, 77, 96);
constexpr COLORREF kText = RGB(241, 245, 249);

void Fill(HDC dc, const RECT& rect, COLORREF color) noexcept {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Border(HDC dc, const RECT& rect, COLORREF color) noexcept {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

bool SamePoint(POINT a, POINT b) noexcept { return a.x == b.x && a.y == b.y; }

}  // namespace

const std::array<SelectionOverlay::ButtonSpec, SelectionOverlay::BCount> SelectionOverlay::kButtons{{
    {L"选区", 46}, {L"矩形", 46}, {L"椭圆", 46}, {L"箭头", 46}, {L"画笔", 46}, {L"文字", 46}, {L"马赛克", 60},
    {L"红色", 21}, {L"黄色", 21}, {L"绿色", 21}, {L"蓝色", 21}, {L"白色", 21}, {L"黑色", 21}, {L"线宽", 68},
    {L"重选", 46}, {L"全屏", 46}, {L"撤销", 46}, {L"复制", 46}, {L"另存为 PNG", 90}, {L"贴图", 46}, {L"取消", 46},
}};

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
    owner_ = owner;
    shortcuts_ = shortcuts;
    anchor_ = anchor;
    selection_ = panel_ = {};
    tool_ = EditTool::Select;
    drag_ = Drag::None;
    status_ = nullptr;
    activated_ = invalidated_ = pending_ = busy_ = closing_ = composingText_ = false;

    const auto& bounds = frame.bounds;
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"Picolet 截图",
                                        WS_POPUP | WS_CLIPCHILDREN, bounds.left, bounds.top,
                                        image.width(), image.height(), owner, nullptr, instance, this);
    if (!window || !CreateButtons(instance)) {
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
    return S_OK;
}

bool SelectionOverlay::CreateButtons(HINSTANCE instance) noexcept {
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        buttons_[i] = CreateWindowExW(0, L"BUTTON", kButtons[i].label, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kButtonBase + i), instance, nullptr);
        if (!buttons_[i]) return false;
    }
    return true;
}

void SelectionOverlay::Cancel() noexcept {
    if (!window_ || closing_) return;
    invalidated_ = true;
    if (!busy_) PostAction(OverlayAction::Cancel);
}

void SelectionOverlay::Close() noexcept {
    closing_ = true;
    drag_ = Drag::None;
    if (window_) {
        if (GetCapture() == window_) ReleaseCapture();
        DestroyWindow(window_);
    }
    textEdit_ = nullptr;
    buttons_.fill(nullptr);
    textFont_.reset();
    font_.reset();
    frame_ = nullptr;
    dark_ = {};
    history_.Clear();
    draft_ = {};
}

void SelectionOverlay::BeginOutput() noexcept {
    busy_ = true;
    HidePanel();
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
    if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd)) || textEdit_ || busy_ || pending_)
        return false;
    const bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    const bool snapshotUp = (message.message == WM_KEYUP || message.message == WM_SYSKEYUP) && message.wParam == VK_SNAPSHOT;
    if (!down && !snapshotUp) return false;
    if (down && message.wParam == VK_SNAPSHOT) return false;
    const auto mods = CurrentModifiers();
    if (const auto shortcut = MatchLocalShortcut(shortcuts_, {mods, static_cast<UINT>(message.wParam)})) {
        if (!(down && (HIWORD(message.lParam) & KF_REPEAT)) && (drag_ == Drag::None || *shortcut == Shortcut::Cancel))
            Command(*shortcut);
        return true;
    }
    if (down && message.wParam == VK_TAB && !(mods & ~MOD_SHIFT) && drag_ == Drag::None) {
        const HWND current = message.hwnd == window_ ? nullptr : message.hwnd;
        if (const HWND next = GetNextDlgTabItem(window_, current, (mods & MOD_SHIFT) != 0)) SetFocus(next);
        return true;
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
        if (const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            item->CtlID >= kButtonBase && item->CtlID < kButtonBase + BCount) {
            PaintButton(*item);
            return TRUE;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            activated_ = true;
            if (!busy_ && !closing_) SetFocus(textEdit_ ? textEdit_ : window_);
        } else if (activated_ && !busy_ && !closing_) {
            Cancel();
        }
        return 0;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wparam) == window_) {
            POINT cursor{};
            GetCursorPos(&cursor);
            LPCWSTR shape = IDC_CROSS;
            const POINT client{cursor.x - frame_->bounds.left, cursor.y - frame_->bounds.top};
            if (!IsRectEmpty(&panel_) && PtInRect(&panel_, client)) {
                shape = IDC_ARROW;
            } else if (tool_ == EditTool::Select && !IsRectEmpty(&selection_)) {
                const unsigned edges = drag_ == Drag::Resize ? edges_ : HitEdges(cursor);
                if (edges == (EdgeLeft | EdgeTop) || edges == (EdgeRight | EdgeBottom)) shape = IDC_SIZENWSE;
                else if (edges == (EdgeLeft | EdgeBottom) || edges == (EdgeRight | EdgeTop)) shape = IDC_SIZENESW;
                else if (edges & (EdgeLeft | EdgeRight)) shape = IDC_SIZEWE;
                else if (edges & (EdgeTop | EdgeBottom)) shape = IDC_SIZENS;
                else if (drag_ == Drag::Move || PtInRect(&selection_, cursor)) shape = IDC_SIZEALL;
            } else if (tool_ == EditTool::Text && PtInRect(&selection_, cursor)) {
                shape = IDC_IBEAM;
            }
            SetCursor(LoadCursorW(nullptr, shape));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) >= kButtonBase && LOWORD(wparam) < kButtonBase + BCount && HIWORD(wparam) == BN_CLICKED) {
            if (!pending_ && !busy_) PressButton(static_cast<Button>(LOWORD(wparam) - kButtonBase));
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (!busy_ && !pending_ && drag_ == Drag::None && FinishText(true)) {
            const auto point = MessagePoint();
            const POINT client{point.x - frame_->bounds.left, point.y - frame_->bounds.top};
            if (!PtInRect(&panel_, client)) BeginDrag(point);
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
    case WM_CLOSE:
        Cancel();
        return 0;
    case WM_DPICHANGED:
        if (activated_ && !closing_) Cancel();
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void SelectionOverlay::Command(Shortcut shortcut) noexcept {
    switch (shortcut) {
    case Shortcut::Fullscreen: SelectFullscreen(); break;
    case Shortcut::Copy: PostAction(OverlayAction::Copy); break;
    case Shortcut::Save: PostAction(OverlayAction::Save); break;
    case Shortcut::Pin: PostAction(OverlayAction::Pin); break;
    case Shortcut::Undo:
        if (FAILED(history_.Undo(*frame_))) Status(L"无法撤销，请重试。");
        else Status(nullptr);
        RepaintSelection(selection_);
        LayoutPanel();
        break;
    case Shortcut::Cancel: Cancel(); break;
    default: break;
    }
}

void SelectionOverlay::PressButton(Button button) noexcept {
    if (button == BCancel) {
        Cancel();
        return;
    }
    if (!FinishText(true)) return;
    status_ = nullptr;
    if (button <= BMosaic) {
        tool_ = static_cast<EditTool>(button);
    } else if (button >= BRed && button <= BBlack) {
        color_ = button - BRed;
    } else if (button == BSize) {
        auto& size = tool_ == EditTool::Text ? textSize_ : strokeSize_;
        size = (size + 1) % std::size(kWidths);
    } else if (button == BReselect) {
        const RECT previous = selection_;
        selection_ = {};
        tool_ = EditTool::Select;
        history_.Clear();
        GetCursorPos(&anchor_);
        RepaintSelection(previous);
    } else {
        switch (button) {
        case BFullscreen: Command(Shortcut::Fullscreen); return;
        case BUndo: Command(Shortcut::Undo); return;
        case BCopy: Command(Shortcut::Copy); return;
        case BSave: Command(Shortcut::Save); return;
        case BPin: Command(Shortcut::Pin); return;
        default: return;
        }
    }
    LayoutPanel();
    RepaintSelection(selection_);
}

void SelectionOverlay::PostAction(OverlayAction action) noexcept {
    if (pending_ || busy_ || closing_ || (action != OverlayAction::Cancel && IsRectEmpty(&selection_))) return;
    pending_ = true;
    // 让按钮和窗口过程先返回，再由控制窗口销毁遮罩或打开系统对话框。
    if (!PostMessageW(owner_, kOverlayAction, static_cast<WPARAM>(action), 0)) {
        pending_ = false;
        if (action == OverlayAction::Cancel) SendMessageW(owner_, kOverlayAction, static_cast<WPARAM>(action), 0);
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
    status_ = nullptr;
    RepaintSelection(previous);
    LayoutPanel();
}

POINT SelectionOverlay::MessagePoint() const noexcept {
    const auto& bounds = frame_->bounds;
    if (bounds.left >= -32768 && bounds.top >= -32768 && bounds.right <= 32767 && bounds.bottom <= 32767) {
        const DWORD position = GetMessagePos();
        return {GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    }
    POINT point{};
    GetCursorPos(&point);
    return point;
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
    mark.color = kColors[color_];
    mark.width = kWidths[strokeSize_];
    mark.textSize = kTextSizes[textSize_];
    return mark;
}

void SelectionOverlay::BeginDrag(POINT point) noexcept {
    start_ = point;
    original_ = selection_;
    if (IsRectEmpty(&selection_)) {
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
        status_ = nullptr;
        SetFocus(window_);
        HidePanel();
        SetCapture(window_);
        if (GetCapture() != window_) AbortDrag();
    }
}

void SelectionOverlay::UpdateDrag(POINT point) noexcept {
    const RECT previous = selection_;
    const POINT delta{point.x - start_.x, point.y - start_.y};
    switch (drag_) {
    case Drag::Create: selection_ = SelectionRect(start_, point, frame_->bounds); break;
    case Drag::Move: selection_ = MoveSelection(original_, delta, frame_->bounds); break;
    case Drag::Resize: selection_ = ResizeSelection(original_, edges_, delta, frame_->bounds); break;
    case Drag::Mark:
        point.x = std::clamp(point.x, selection_.left, selection_.right - 1);
        point.y = std::clamp(point.y, selection_.top, selection_.bottom - 1);
        if (SamePoint(point, draft_.last)) return;
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
        break;
    default: return;
    }
    RepaintSelection(previous);
}

void SelectionOverlay::EndDrag(POINT point) noexcept {
    UpdateDrag(point);
    const auto finished = std::exchange(drag_, Drag::None);
    if (GetCapture() == window_) ReleaseCapture();
    if (finished == Drag::Mark && (draft_.tool == EditTool::Pen || !SamePoint(draft_.first, draft_.last))) {
        if (FAILED(history_.Commit(*frame_, std::move(draft_)))) status_ = L"无法应用标注，请重试。";
    }
    draft_ = {};
    LayoutPanel();
    RepaintSelection(selection_);
}

void SelectionOverlay::AbortDrag() noexcept {
    const RECT previous = selection_;
    drag_ = Drag::None;
    selection_ = original_;
    draft_ = {};
    if (GetCapture() == window_) ReleaseCapture();
    LayoutPanel();
    RepaintSelection(previous);
}

void SelectionOverlay::BeginText(POINT point) noexcept {
    const LONG width = std::min<LONG>(selection_.right - selection_.left, Dip(220, dpi_));
    const LONG height = std::min<LONG>(selection_.bottom - selection_.top, Dip(100, dpi_));
    point.x = std::min(point.x, selection_.right - width);
    point.y = std::min(point.y, selection_.bottom - height);
    draft_ = MakeMark(point);
    textFont_.reset(CreateAnnotationFont(draft_.textSize));
    const RECT box = ToClient({point.x, point.y, selection_.right, point.y + height});
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

bool SelectionOverlay::FinishText(bool commit) noexcept {
    if (!textEdit_) return true;
    if (commit) {
        wchar_t text[kTextLimit + 1]{};
        const int length = GetWindowTextW(textEdit_, text, static_cast<int>(std::size(text)));
        if (length) {
            Annotation mark = MakeMark(draft_.first);
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
    if (!IsRectEmpty(&rect)) OffsetRect(&rect, -frame_->bounds.left, -frame_->bounds.top);
    return rect;
}

void SelectionOverlay::HidePanel() noexcept {
    if (!IsRectEmpty(&panel_)) InvalidateRect(window_, &panel_, FALSE);
    panel_ = {};
    for (const auto button : buttons_) ShowWindow(button, SW_HIDE);
}

void SelectionOverlay::LayoutPanel() noexcept {
    if (closing_ || busy_ || !window_) return;
    const bool selected = !IsRectEmpty(&selection_);
    const HMONITOR monitor = selected ? MonitorFromRect(&selection_, MONITOR_DEFAULTTONEAREST)
                                      : MonitorFromPoint(anchor_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{};
    if (!GetMonitorInfoW(monitor, &info) || !IntersectRect(&work, &info.rcWork, &frame_->bounds)) work = frame_->bounds;
    UINT dpiX = 0, dpiY = 0;
    // 这里只查询按钮和字号的比例；选区和图像本身始终保持物理像素。
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || !dpiX) dpiX = GetDpiForWindow(window_);
    if (!dpiX) dpiX = USER_DEFAULT_SCREEN_DPI;
    if (dpi_ != dpiX || !font_) {
        dpi_ = dpiX;
        font_.reset(CreateUiFont(dpi_));
    }

    std::array<RECT, BCount> boxes{};
    const LONG pad = Dip(8, dpi_);
    const LONG gap = Dip(4, dpi_);
    const LONG buttonHeight = Dip(28, dpi_);
    const LONG header = Dip(23, dpi_);
    const LONG maxWidth = std::max(1L, work.right - work.left - pad * 4);
    LONG x = 0;
    LONG y = header;
    LONG width = std::min(maxWidth, static_cast<LONG>(Dip(selected ? 306 : 350, dpi_)));
    const auto nextRow = [&] { x = 0; y += buttonHeight + gap; };
    const auto add = [&](Button button) {
        const LONG w = std::min(maxWidth, static_cast<LONG>(Dip(kButtons[button].width, dpi_)));
        if (x && x + w > maxWidth) nextRow();
        boxes[button] = {x, y, x + w, y + buttonHeight};
        width = std::max(width, x + w);
        x += w + gap;
    };
    if (selected) {
        for (std::size_t i = BSelect; i <= BSize; ++i) add(static_cast<Button>(i));
        nextRow();
        add(BReselect);
        add(BFullscreen);
        add(BUndo);
        add(BCopy);
        add(BSave);
        add(BPin);
    } else {
        add(BFullscreen);
    }
    add(BCancel);

    const LONG panelWidth = width + 2 * pad;
    const LONG panelHeight = y + buttonHeight + 2 * pad;
    const LONG minX = work.left + pad;
    const LONG minY = work.top + pad;
    const LONG maxX = std::max(minX, work.right - pad - panelWidth);
    const LONG maxY = std::max(minY, work.bottom - pad - panelHeight);
    LONG left = work.left + (work.right - work.left - panelWidth) / 2;
    LONG top = minY;
    if (selected) {
        left = selection_.right - panelWidth;
        top = selection_.bottom + pad;
        if (top > maxY) {
            const LONG above = selection_.top - panelHeight - pad;
            top = above >= minY ? above : selection_.bottom - panelHeight - pad;
        }
    }
    left = std::clamp(left, minX, maxX);
    top = std::clamp(top, minY, maxY);
    const RECT old = panel_;
    panel_ = ToClient({left, top, left + panelWidth, top + panelHeight});
    wchar_t size[32]{};
    std::swprintf(size, std::size(size), tool_ == EditTool::Text ? L"字号 %d" : L"线宽 %d",
                  tool_ == EditTool::Text ? kTextSizes[textSize_] : kWidths[strokeSize_]);
    SetWindowTextW(buttons_[BSize], size);
    EnableWindow(buttons_[BUndo], !history_.empty());
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const auto& box = boxes[i];
        if (IsRectEmpty(&box)) ShowWindow(buttons_[i], SW_HIDE);
        else {
            SetWindowPos(buttons_[i], nullptr, panel_.left + pad + box.left, panel_.top + pad + box.top,
                         box.right - box.left, box.bottom - box.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
            InvalidateRect(buttons_[i], nullptr, FALSE);
        }
    }
    if (!IsRectEmpty(&old)) InvalidateRect(window_, &old, FALSE);
    InvalidateRect(window_, &panel_, FALSE);
}

void SelectionOverlay::Status(const wchar_t* text) noexcept {
    status_ = text;
    if (!IsRectEmpty(&panel_)) InvalidateRect(window_, &panel_, FALSE);
}

void SelectionOverlay::RepaintSelection(const RECT& previous) noexcept {
    RECT changed{};
    if (UnionRect(&changed, &previous, &selection_)) {
        changed = ToClient(changed);
        InflateRect(&changed, Dip(8, dpi_), Dip(8, dpi_));
        InvalidateRect(window_, &changed, FALSE);
    }
}

void SelectionOverlay::Paint() noexcept {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    if (!dc || !frame_) {
        EndPaint(window_, &paint);
        return;
    }
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
            if (drag_ == Drag::Mark) DrawAnnotationDraft(dc, draft_, {frame_->bounds.left, frame_->bounds.top});
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
                Border(dc, grip, kPanel);
            }
        }
    }
    if (!IsRectEmpty(&panel_)) {
        Fill(dc, panel_, kPanel);
        Border(dc, panel_, kPanelBorder);
        RECT header = panel_;
        InflateRect(&header, -Dip(8, dpi_), -Dip(5, dpi_));
        header.bottom = header.top + Dip(23, dpi_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        const auto old = font_ ? SelectObject(dc, font_.get()) : nullptr;
        wchar_t label[192]{};
        if (hasSelection) {
            std::swprintf(label, std::size(label), L"%ld × %ld 像素%ls%ls", selected.right - selected.left,
                          selected.bottom - selected.top, status_ ? L"   ·   " : L"", status_ ? status_ : L"");
        } else if (status_) {
            lstrcpynW(label, status_, static_cast<int>(std::size(label)));
        } else {
            wchar_t binding[96]{};
            FormatHotkey(shortcuts_[ShortcutIndex(Shortcut::Fullscreen)], binding);
            std::swprintf(label, std::size(label), L"拖拽选择区域   ·   全屏：%ls", binding);
        }
        DrawTextW(dc, label, -1, &header, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
    }
    EndPaint(window_, &paint);
}

void SelectionOverlay::PaintButton(const DRAWITEMSTRUCT& item) const noexcept {
    const auto button = static_cast<Button>(item.CtlID - kButtonBase);
    const bool color = button >= BRed && button <= BBlack;
    const bool active = (button <= BMosaic && static_cast<EditTool>(button) == tool_) ||
                        (color && button - BRed == color_);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    auto rect = item.rcItem;
    Fill(item.hDC, rect, pressed ? RGB(52, 73, 87) : active ? RGB(26, 65, 67) : RGB(33, 45, 61));
    Border(item.hDC, rect, active || button == BCopy ? kAccent : kPanelBorder);
    if (color) {
        InflateRect(&rect, -Dip(4, dpi_), -Dip(6, dpi_));
        Fill(item.hDC, rect, kColors[button - BRed]);
    } else {
        wchar_t label[36]{};
        GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
        const auto old = font_ ? SelectObject(item.hDC, font_.get()) : nullptr;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, disabled ? RGB(140, 153, 171) : kText);
        DrawTextW(item.hDC, label, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        if (old) SelectObject(item.hDC, old);
    }
    if (item.itemState & ODS_FOCUS) {
        rect = item.rcItem;
        InflateRect(&rect, -2, -2);
        DrawFocusRect(item.hDC, &rect);
    }
}

}  // namespace picolet
