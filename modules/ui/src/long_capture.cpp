#include <picolet/ui/long_capture.h>

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace picolet {
namespace {
constexpr wchar_t kPanelClass[] = L"Picolet.LongCapture";
constexpr wchar_t kMaskClass[] = L"Picolet.LongCaptureMask";
constexpr UINT kInput = WM_APP + 20;
constexpr UINT kPause = WM_APP + 21;
constexpr UINT kControl = WM_APP + 22;
constexpr UINT kPrepareCapture = WM_APP + 23;
constexpr UINT kRestoreCapture = WM_APP + 24;
constexpr ULONG_PTR kAutomaticInput = 0x5049434f;
enum Controls { DirectionDown = 6001, DirectionUp, Automatic, FinishCapture, CancelCapture };

void Fill(HDC dc, RECT area, COLORREF color) noexcept {
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

}

bool LongCaptureSession::Register(HINSTANCE instance, HICON icon) noexcept {
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.hInstance = instance;
    definition.hIcon = icon;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.lpfnWndProc = PanelProc;
    definition.lpszClassName = kPanelClass;
    if (!RegisterClassExW(&definition)) return false;
    definition.lpfnWndProc = MaskProc;
    definition.lpszClassName = kMaskClass;
    return RegisterClassExW(&definition) != 0;
}

HRESULT LongCaptureSession::Open(HINSTANCE instance, HWND owner, RECT area, const Image& seed,
    Image&& annotatedSeed, Hotkey cancelKey) noexcept {
    Close();
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromRect(&area, MONITOR_DEFAULTTONULL), &info) ||
        area.left < info.rcMonitor.left || area.top < info.rcMonitor.top || area.right > info.rcMonitor.right ||
        area.bottom > info.rcMonitor.bottom || seed.width() != area.right - area.left || seed.height() != area.bottom - area.top)
        return E_INVALIDARG;
    const POINT center{(area.left + area.right) / 2, (area.top + area.bottom) / 2};
    target_ = GetAncestor(WindowFromPoint(center), GA_ROOT);
    if (!target_ || target_ == GetDesktopWindow() || GetWindowThreadProcessId(target_, nullptr) == GetCurrentThreadId() ||
        !GetWindowRect(target_, &targetRect_)) return E_INVALIDARG;
    for (const POINT corner : {POINT{area.left, area.top}, POINT{area.right - 1, area.top},
                              POINT{area.left, area.bottom - 1}, POINT{area.right - 1, area.bottom - 1}})
        if (GetAncestor(WindowFromPoint(corner), GA_ROOT) != target_) return E_INVALIDARG;
    owner_ = owner;
    area_ = area;
    monitor_ = info.rcMonitor;
    cancelKey_ = cancelKey;
    automatic_ = upward_ = pendingAuto_ = pending_ = blocked_ = finishing_ = false;
    still_ = 0;
    commandMotion_ = commandProcessed_ = 0;
    std::fill(std::begin(held_), std::end(held_), false);
    targetDpi_ = GetDpiForWindow(target_);
    UINT yDpi = 0;
    if (FAILED(GetDpiForMonitor(MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpi_, &yDpi)) || !dpi_)
        dpi_ = targetDpi_ ? targetDpi_ : USER_DEFAULT_SCREEN_DPI;
    const int width = Dip(238, dpi_), height = std::min<LONG>(Dip(590, dpi_), info.rcWork.bottom - info.rcWork.top - Dip(16, dpi_));
    const int gap = Dip(10, dpi_);
    LONG left = area.right + gap;
    if (left + width > info.rcWork.right) left = area.left - gap - width;
    if (left < info.rcWork.left) left = std::max(info.rcWork.left, info.rcWork.right - width - gap);
    const LONG top = std::clamp(area.top, info.rcWork.top + gap, std::max(info.rcWork.top + gap, info.rcWork.bottom - height - gap));
    mask_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kMaskClass, L"Picolet 长截图范围", WS_POPUP, monitor_.left, monitor_.top,
        monitor_.right - monitor_.left, monitor_.bottom - monitor_.top, owner, nullptr, instance, this);
    if (!mask_) { Close(); return E_FAIL; }
    SetLayeredWindowAttributes(mask_, 0, 125, LWA_ALPHA);
    const HRGN region = CreateRectRgn(0, 0, monitor_.right - monitor_.left, monitor_.bottom - monitor_.top);
    const HRGN hole = CreateRectRgn(area.left - monitor_.left, area.top - monitor_.top, area.right - monitor_.left, area.bottom - monitor_.top);
    if (!region || !hole) {
        if (region) DeleteObject(region);
        if (hole) DeleteObject(hole);
        Close(); return E_OUTOFMEMORY;
    }
    CombineRgn(region, region, hole, RGN_DIFF);
    DeleteObject(hole);
    if (!SetWindowRgn(mask_, region, FALSE)) { DeleteObject(region); Close(); return E_FAIL; }
    panel_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kPanelClass,
        L"Picolet 长截图", WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, left, top, width, height,
        owner, nullptr, instance, this);
    if (!panel_) { Close(); return E_FAIL; }
    font_.reset(CreateUiFont(dpi_));
    const auto button = [&](const wchar_t* label, int id, DWORD style) {
        const HWND control = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            0, 0, 1, 1, panel_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (control && font_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_.get()), FALSE);
        return control;
    };
    directionDown_ = button(L"向下", DirectionDown, BS_RADIOBUTTON);
    directionUp_ = button(L"向上", DirectionUp, BS_RADIOBUTTON);
    autoButton_ = button(L"自动滚动", Automatic, BS_PUSHBUTTON);
    finishButton_ = button(L"完成", FinishCapture, BS_PUSHBUTTON);
    cancelButton_ = button(L"取消", CancelCapture, BS_PUSHBUTTON);
    if (!directionDown_ || !directionUp_ || !autoButton_ || !finishButton_ || !cancelButton_) { Close(); return E_FAIL; }
    SendMessageW(directionDown_, BM_SETCHECK, BST_CHECKED, 0);
    Layout();
    excluded_ = SetWindowDisplayAffinity(panel_, 0x00000011) != FALSE;
    active_ = this;
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, instance, 0);
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, instance, 0);
    if (!mouseHook_ || !keyboardHook_ || !SetTimer(panel_, 1, 33, nullptr)) { Close(); return E_FAIL; }
    SetForegroundWindow(target_);
    ShowWindow(mask_, SW_SHOWNOACTIVATE);
    ShowWindow(panel_, SW_SHOWNOACTIVATE);
    NativeScrollCaptureOptions options;
    options.area = area_; options.targetRect = targetRect_; options.target = target_; options.dpi = targetDpi_;
    options.coordinator = panel_; options.prepareMessage = kPrepareCapture; options.restoreMessage = kRestoreCapture;
    RECT panelRect{}, overlap{};
    GetWindowRect(panel_, &panelRect); InflateRect(&panelRect, Dip(16, dpi_), Dip(16, dpi_));
    options.hideBeforeCapture = !excluded_ && IntersectRect(&overlap, &panelRect, &area_);
    const SIZE preview{std::max(1L, previewRect_.right - previewRect_.left), std::max(1L, previewRect_.bottom - previewRect_.top)};
    CapturePipelineLimits limits;
    if (options.hideBeforeCapture) limits.framesPerSecond = 30;
    const auto hr = pipeline_.Start(seed, std::move(annotatedSeed), MakeNativeScrollSource(options), preview, limits);
    if (FAILED(hr)) { Close(); return hr; }
    snapshot_ = pipeline_.Snapshot();
    Status(L"在选区内滚动鼠标或触摸板，可连续向上、向下截取。");
    UpdatePreview();
    return S_OK;
}

void LongCaptureSession::Close() noexcept {
    if (active_ == this) active_ = nullptr;
    if (mouseHook_) UnhookWindowsHookEx(std::exchange(mouseHook_, nullptr));
    if (keyboardHook_) UnhookWindowsHookEx(std::exchange(keyboardHook_, nullptr));
    automatic_ = pendingAuto_ = false;
    pipeline_.Cancel();
    if (panel_) {
        KillTimer(panel_, 1);
        DestroyWindow(std::exchange(panel_, nullptr));
    }
    if (mask_) DestroyWindow(std::exchange(mask_, nullptr));
    target_ = owner_ = nullptr;
    directionDown_ = directionUp_ = autoButton_ = finishButton_ = cancelButton_ = nullptr;
    font_.reset();
    snapshot_ = {};
    paintBuffer_.Reset();
}

HRESULT LongCaptureSession::Finish(Image& image, SeedPlacement& placement) const noexcept {
    return pipeline_.Result(image, placement);
}

void LongCaptureSession::Retry(const wchar_t* message) noexcept {
    pending_ = false;
    Pause(message);
}

void LongCaptureSession::StopForDisplayChange() noexcept {
    blocked_ = true;
    pipeline_.StopCapture();
    Pause(L"显示配置已变化，请完成当前长图或取消后重新框选。");
    if (mask_) ShowWindow(mask_, SW_HIDE);
    RECT rect{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (panel_ && GetWindowRect(panel_, &rect) &&
        GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const int width = std::min(rect.right - rect.left, monitor.rcWork.right - monitor.rcWork.left);
        const int height = std::min(rect.bottom - rect.top, monitor.rcWork.bottom - monitor.rcWork.top);
        const LONG left = std::clamp(rect.left, monitor.rcWork.left, monitor.rcWork.right - width);
        const LONG top = std::clamp(rect.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
        SetWindowPos(panel_, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Layout();
        UpdatePreview();
    }
}

bool LongCaptureSession::TargetForeground() const noexcept {
    return GetAncestor(GetForegroundWindow(), GA_ROOT) == target_;
}

LRESULT CALLBACK LongCaptureSession::PanelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<LongCaptureSession*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->panel_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<LongCaptureSession*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCDESTROY) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); return DefWindowProcW(window, message, wparam, lparam); }
    return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK LongCaptureSession::MaskProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    const auto* self = reinterpret_cast<LongCaptureSession*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && self) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        Fill(dc, client, RGB(0, 0, 0));
        RECT border = self->area_;
        OffsetRect(&border, -self->monitor_.left, -self->monitor_.top);
        InflateRect(&border, 2, 2);
        SetDCBrushColor(dc, RGB(62, 230, 193));
        FrameRect(dc, &border, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK LongCaptureSession::MouseHook(int code, WPARAM message, LPARAM data) noexcept {
    auto* self = active_;
    if (code == HC_ACTION && self && !self->pending_) {
        const auto& event = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if (event.dwExtraInfo != kAutomaticInput) {
            if (message == WM_MOUSEWHEEL && PtInRect(&self->area_, event.pt) && self->TargetForeground())
                PostMessageW(self->panel_, kInput, static_cast<WPARAM>(static_cast<SHORT>(HIWORD(event.mouseData))), 0);
            else if (self->automatic_ && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                     (message == WM_MOUSEMOVE && !PtInRect(&self->area_, event.pt))))
                PostMessageW(self->panel_, kPause, 0, 0);
            if (message == WM_RBUTTONUP && PtInRect(&self->area_, event.pt) && self->TargetForeground()) {
                PostMessageW(self->panel_, kControl, static_cast<WPARAM>(ScrollAction::Cancel), 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

LRESULT CALLBACK LongCaptureSession::KeyboardHook(int code, WPARAM message, LPARAM data) noexcept {
    auto* self = active_;
    if (code == HC_ACTION && self) {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const UINT key = event.vkCode;
        if (key < 256) {
            const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (up && self->held_[key]) { self->held_[key] = false; return 1; }
            if (!up && self->TargetForeground() && !self->pending_) {
                const auto modifiers = CurrentModifiers();
                const bool cancel = self->cancelKey_.key && self->cancelKey_ == Hotkey{modifiers, key};
                const bool finish = !modifiers && key == VK_RETURN;
                const bool toggle = !modifiers && key == VK_SPACE;
                if (cancel || finish || toggle) {
                    if (!self->held_[key]) PostMessageW(self->panel_, toggle && !cancel ? WM_COMMAND : kControl,
                        toggle && !cancel ? static_cast<WPARAM>(Automatic) : static_cast<WPARAM>(cancel ? ScrollAction::Cancel : ScrollAction::Finish), 0);
                    self->held_[key] = true;
                    return 1;
                }
                if (self->automatic_) PostMessageW(self->panel_, kPause, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

LRESULT LongCaptureSession::Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case kPrepareCapture:
        if (static_cast<std::uint64_t>(wparam) == pipeline_.session()) {
            if (!pending_ && !blocked_) { ShowWindow(panel_, SW_HIDE); DwmFlush(); }
            pipeline_.AllowCapture(static_cast<std::uint64_t>(wparam), static_cast<std::uint64_t>(lparam));
        }
        return 0;
    case kRestoreCapture:
        if (static_cast<std::uint64_t>(wparam) == pipeline_.session() && !pending_)
            ShowWindow(panel_, SW_SHOWNOACTIVATE);
        return 0;
    case WM_TIMER: Tick(); return 0;
    case WM_CLOSE: PostAction(ScrollAction::Cancel); return 0;
    case kControl: PostAction(static_cast<ScrollAction>(wparam)); return 0;
    case kPause:
        if (!finishing_) Pause(L"自动滚动已暂停，可手动滚动或继续。");
        return 0;
    case kInput:
        if (finishing_) return 0;
        if (automatic_) Pause(L"已切换为手动滚动。");
        pipeline_.SetDirection(static_cast<SHORT>(wparam) > 0 ? ScrollDirection::Up : ScrollDirection::Down);
        return 0;
    case WM_COMMAND:
        if (pending_ || (finishing_ && LOWORD(wparam) != CancelCapture)) return 0;
        switch (LOWORD(wparam)) {
        case DirectionDown: case DirectionUp:
            Pause(L"方向已切换，可点击自动滚动。");
            upward_ = LOWORD(wparam) == DirectionUp;
            SendMessageW(directionUp_, BM_SETCHECK, upward_ ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(directionDown_, BM_SETCHECK, upward_ ? BST_UNCHECKED : BST_CHECKED, 0);
            break;
        case Automatic: ToggleAutomatic(); break;
        case FinishCapture: PostAction(ScrollAction::Finish); break;
        case CancelCapture: PostAction(ScrollAction::Cancel); break;
        }
        return 0;
    }
    return DefWindowProcW(panel_, message, wparam, lparam);
}

void LongCaptureSession::ToggleAutomatic() noexcept {
    if (automatic_) { Pause(L"已暂停，可手动滚动或继续。"); return; }
    if (blocked_) return;
    SetForegroundWindow(target_);
    if (!TargetForeground()) { Pause(L"请先激活原窗口，再开始自动滚动。"); return; }
    SetCursorPos((area_.left + area_.right) / 2, (area_.top + area_.bottom) / 2);
    automatic_ = true;
    still_ = 0;
    lastAuto_ = GetTickCount64();
    SetWindowTextW(autoButton_, L"暂停滚动");
    Status(L"正在自动滚动；鼠标移出选区即暂停。");
}

void LongCaptureSession::Pause(const wchar_t* status) noexcept {
    const bool wasAutomatic = automatic_;
    automatic_ = pendingAuto_ = false;
    if (autoButton_ && wasAutomatic) SetWindowTextW(autoButton_, L"自动滚动");
    Status(status);
}

void LongCaptureSession::Tick() noexcept {
    if (pending_) return;
    const auto fresh = pipeline_.Snapshot();
    if (!fresh.session || fresh.session != pipeline_.session()) return;
    const auto oldMotion = snapshot_.stats.motion;
    const auto oldTracking = snapshot_.tracking;
    const auto oldStitch = snapshot_.stitchStatus;
    UpdateSnapshot(fresh);
    if (finishing_) {
        if (snapshot_.finished) PublishAction(ScrollAction::Finish);
        return;
    }
    if (blocked_) return;
    RECT rect{};
    if (!IsWindow(target_) || IsIconic(target_) || !GetWindowRect(target_, &rect) || !EqualRect(&rect, &targetRect_) ||
        GetDpiForWindow(target_) != targetDpi_ || snapshot_.source == CaptureSourceState::Blocked) {
        blocked_ = true; pipeline_.StopCapture();
        Pause(L"原窗口位置或尺寸已变化，请完成当前长图或重新框选。");
        return;
    }
    if (FAILED(snapshot_.error)) {
        blocked_ = true; pipeline_.StopCapture();
        Pause(L"采集遇到资源或图像错误，已保留可靠结果，可完成或取消。");
        return;
    }
    if (snapshot_.tracking == ScrollTrackingState::Limit) {
        blocked_ = true; Pause(L"已达到长图大小上限，请点击完成。"); return;
    }
    if (!TargetForeground()) {
        if (automatic_) Pause(L"原窗口失去焦点，自动滚动已暂停。");
        return;
    }
    POINT cursor{}; GetCursorPos(&cursor);
    if (automatic_ && !PtInRect(&area_, cursor)) Pause(L"鼠标已移出选区，自动滚动已暂停。");
    if (snapshot_.tracking == ScrollTrackingState::Gap) {
        Pause(L"当前画面无法连接已截内容，请缓慢反向滚回重叠位置补截。");
        return;
    }
    if (snapshot_.tracking == ScrollTrackingState::Recovering) Status(L"正在连续跟踪，尝试从中间帧恢复定位…");
    else if (snapshot_.stitchStatus == StitchStatus::Calibrating) Status(L"正在识别固定区域，请继续滚动。");
    else if (oldTracking != ScrollTrackingState::Capturing || oldStitch == StitchStatus::Calibrating || snapshot_.stats.motion != oldMotion) {
        Status(automatic_ ? L"正在自动滚动并连续拼接，可随时暂停。" : L"已拼接，可继续快速向上或向下滚动。");
    }
    const auto now = GetTickCount64();
    if (pendingAuto_ && snapshot_.stats.processed > commandProcessed_ && snapshot_.lastFrameTick >= commandAt_ + 180) {
        if (snapshot_.tracking == ScrollTrackingState::Capturing) {
            still_ = snapshot_.stats.motion == commandMotion_ ? still_ + 1 : 0;
            pendingAuto_ = false; lastAuto_ = now;
            if (still_ >= 3) Pause(L"内容未继续滚动，可能已到边界；可反向继续或完成。");
        } else if (now - commandAt_ > 1800) Pause(L"仍在尝试恢复定位，已暂停自动滚动。");
    }
    if (automatic_ && !pendingAuto_ && now - lastAuto_ >= 400) {
        if (CurrentModifiers()) { Pause(L"检测到按键操作，自动滚动已暂停。"); return; }
        INPUT input{};
        input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(upward_ ? WHEEL_DELTA : -WHEEL_DELTA);
        input.mi.dwExtraInfo = kAutomaticInput;
        if (SendInput(1, &input, sizeof(input)) != 1) { Pause(L"无法发送滚动，请手动滚动原窗口。"); return; }
        pipeline_.SetDirection(upward_ ? ScrollDirection::Up : ScrollDirection::Down);
        pendingAuto_ = true; commandAt_ = now;
        commandMotion_ = snapshot_.stats.motion; commandProcessed_ = snapshot_.stats.processed;
    }
}
void LongCaptureSession::PostAction(ScrollAction action) noexcept {
    if (pending_) return;
    if (action == ScrollAction::Finish) {
        if (finishing_) return;
        Pause(L"正在处理最后画面，请稍候…");
        finishing_ = true;
        pipeline_.RequestFinish();
        return;
    }
    PublishAction(action);
}
void LongCaptureSession::PublishAction(ScrollAction action) noexcept {
    if (pending_) return;
    Pause(status_);
    finishing_ = false;
    pending_ = true;
    if (!PostMessageW(owner_, kScrollAction, static_cast<WPARAM>(action), reinterpret_cast<LPARAM>(panel_))) {
        pending_ = false;
        Status(L"无法结束采集，请重试。");
    }
}

void LongCaptureSession::Status(const wchar_t* text) noexcept {
    wchar_t next[180]{};
    lstrcpynW(next, text ? text : L"", static_cast<int>(std::size(next)));
    if (!lstrcmpW(next, status_)) return;
    lstrcpynW(status_, next, static_cast<int>(std::size(status_)));
    if (panel_) { const auto area = StatusRect(); InvalidateRect(panel_, &area, FALSE); }
}

void LongCaptureSession::Layout() noexcept {
    RECT client{};
    GetClientRect(panel_, &client);
    const int gap = Dip(12, dpi_), height = Dip(29, dpi_);
    const int bottom = client.bottom - gap;
    const auto place = [&](HWND control, int x, int y, int width) {
        SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const int inner = client.right - gap * 2, half = (inner - gap) / 2;
    place(finishButton_, gap, bottom - height, half);
    place(cancelButton_, gap * 2 + half, bottom - height, half);
    place(autoButton_, gap, bottom - height * 2 - gap, inner);
    place(directionDown_, gap, bottom - height * 3 - gap * 2, half);
    place(directionUp_, gap * 2 + half, bottom - height * 3 - gap * 2, half);
    previewRect_ = {gap, Dip(58, dpi_), client.right - gap, bottom - height * 3 - gap * 3 - Dip(62, dpi_)};
    InvalidateRect(panel_, nullptr, FALSE);
}

void LongCaptureSession::UpdatePreview() noexcept {
    UpdateSnapshot(pipeline_.Snapshot());
}

RECT LongCaptureSession::DimensionRect() const noexcept {
    RECT client{}; GetClientRect(panel_, &client);
    return {Dip(12, dpi_), Dip(32, dpi_), client.right - Dip(12, dpi_), Dip(52, dpi_)};
}

RECT LongCaptureSession::StatusRect() const noexcept {
    RECT client{}; GetClientRect(panel_, &client);
    return {Dip(12, dpi_), previewRect_.bottom + Dip(7, dpi_), client.right - Dip(12, dpi_), previewRect_.bottom + Dip(63, dpi_)};
}

RECT LongCaptureSession::PreviewPosition(const CapturePipelineSnapshot& snapshot) const noexcept {
    if (!snapshot.preview || snapshot.preview->empty() || snapshot.height <= 0 || IsRectEmpty(&previewRect_)) return {};
    const RECT fitted = FitImage(*snapshot.preview, previewRect_);
    const auto view = snapshot.viewport;
    RECT position{fitted.left, fitted.top + MulDiv(view.top, fitted.bottom - fitted.top, snapshot.height),
                  fitted.right, fitted.top + MulDiv(view.bottom, fitted.bottom - fitted.top, snapshot.height)};
    position.bottom = std::min(fitted.bottom, std::max(position.top + 2, position.bottom));
    return position;
}

void LongCaptureSession::UpdateSnapshot(const CapturePipelineSnapshot& next) noexcept {
    if (panel_) {
        if (next.width != snapshot_.width || next.height != snapshot_.height) {
            const auto label = DimensionRect();
            InvalidateRect(panel_, &label, FALSE);
        }
        if (next.preview != snapshot_.preview) {
            InvalidateRect(panel_, &previewRect_, FALSE);
        } else {
            const auto previous = PreviewPosition(snapshot_), current = PreviewPosition(next);
            if (!EqualRect(&previous, &current)) {
                if (!IsRectEmpty(&previous)) InvalidateRect(panel_, &previous, FALSE);
                if (!IsRectEmpty(&current)) InvalidateRect(panel_, &current, FALSE);
            }
        }
    }
    snapshot_ = next;
}
void LongCaptureSession::Paint() noexcept {
    PAINTSTRUCT paint{};
    const auto target = BeginPaint(panel_, &paint);
    if (target) {
        BufferedPaint drawing(paintBuffer_, target, paint.rcPaint);
        PaintContents(drawing.dc());
    }
    EndPaint(panel_, &paint);
}

void LongCaptureSession::PaintContents(HDC dc) const noexcept {
    RECT client{};
    GetClientRect(panel_, &client);
    Fill(dc, client, RGB(255, 255, 255));
    const auto old = font_ ? SelectObject(dc, font_.get()) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(24, 30, 40));
    RECT title{Dip(12, dpi_), Dip(9, dpi_), client.right - Dip(12, dpi_), Dip(30, dpi_)};
    DrawTextW(dc, L"长截图", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    wchar_t dimension[80]{};
    std::swprintf(dimension, std::size(dimension), L"%d × %d 像素", snapshot_.width, snapshot_.height);
    RECT label = DimensionRect();
    SetTextColor(dc, RGB(95, 101, 112));
    DrawTextW(dc, dimension, -1, &label, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    Fill(dc, previewRect_, RGB(240, 242, 245));
    if (snapshot_.preview && !snapshot_.preview->empty() && snapshot_.height > 0) {
        const RECT fitted = FitImage(*snapshot_.preview, previewRect_);
        DrawImage(dc, *snapshot_.preview, fitted);
        const auto position = PreviewPosition(snapshot_);
        SetDCBrushColor(dc, RGB(0, 139, 110));
        FrameRect(dc, &position, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    RECT status = StatusRect();
    DrawTextW(dc, status_, -1, &status, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    if (old) SelectObject(dc, old);
}

}  // namespace picolet
