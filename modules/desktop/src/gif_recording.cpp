#include <picolet/desktop/gif_recording.h>

#include <picolet/capture/gif.h>
#include <picolet/desktop/gif_clipboard.h>
#include <picolet/win32.h>

#include <dwmapi.h>
#include <objbase.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace picolet {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using namespace std::chrono_literals;

constexpr wchar_t kPanelClass[] = L"Picolet.GifRecording";
constexpr UINT kPrepareFrame = WM_APP + 25;
constexpr UINT kRestorePanel = WM_APP + 26;
constexpr auto kFramePeriod = 100ms;
constexpr auto kDisplayCheckPeriod = 250ms;
constexpr auto kDurationLimit = 30s;
constexpr std::uint64_t kSizeThreshold = 32ull * 1024 * 1024;
constexpr int kClipboardAttempts = 5;
enum Control : int { FinishRecording = 6101, CancelRecording };
std::atomic<WPARAM> nextSession{0};

UINT MonitorDpi(HMONITOR monitor) noexcept {
    UINT x = 0, y = 0;
    return SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x ? x : USER_DEFAULT_SCREEN_DPI;
}

RECT PanelBounds(const RECT& area, const RECT& work, UINT dpi) noexcept {
    const LONG width = std::min<LONG>(Dip(282, dpi), work.right - work.left);
    const LONG height = std::min<LONG>(Dip(76, dpi), work.bottom - work.top);
    const LONG gap = Dip(8, dpi);
    const LONG x = std::clamp(area.left, work.left, work.right - width);
    const LONG y = std::clamp(area.top, work.top, work.bottom - height);
    const POINT positions[]{{x, area.bottom + gap}, {x, area.top - gap - height},
                            {area.right + gap, y}, {area.left - gap - width, y}};
    for (const auto& position : positions) {
        const RECT bounds{position.x, position.y, position.x + width, position.y + height};
        if (bounds.left >= work.left && bounds.top >= work.top && bounds.right <= work.right &&
            bounds.bottom <= work.bottom) return bounds;
    }
    const LONG right = std::max(work.left + width, work.right - gap);
    const LONG bottom = std::max(work.top + height, work.bottom - gap);
    return {right - width, bottom - height, right, bottom};
}

struct OtherMonitor {
    HMONITOR excluded;
    HMONITOR found = nullptr;
    RECT work{};
};

BOOL CALLBACK FindOtherMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) noexcept {
    auto& search = *reinterpret_cast<OtherMonitor*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == search.excluded || !GetMonitorInfoW(monitor, &info) || IsRectEmpty(&info.rcWork)) return TRUE;
    search.found = monitor;
    search.work = info.rcWork;
    return FALSE;
}

bool ExcludePanel(HWND panel) noexcept {
    // Windows 10 2004 之前即使返回成功，也可能以黑块代替浮窗，需改用隐藏后取帧。
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    version.dwBuildNumber = 19041;
    const DWORDLONG mask = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    constexpr DWORD excludeFromCapture = 0x00000011;
    return VerifyVersionInfoW(&version, VER_BUILDNUMBER, mask) && SetWindowDisplayAffinity(panel, excludeFromCapture);
}

UINT FrameDelay(TimePoint start, TimePoint frame, TimePoint next) noexcept {
    using Centiseconds = std::chrono::duration<int, std::centi>;
    // 从同一时间原点取整，避免逐帧取整导致总时长偏移。
    const auto ticks = std::chrono::round<Centiseconds>(next - start) - std::chrono::round<Centiseconds>(frame - start);
    return static_cast<UINT>(std::max(1, ticks.count()));
}

}  // namespace

struct GifRecordingSession::State {
    enum class Phase { Preparing, Recording, Finishing, Copying, Retry, Cancelling };

    ~State();
    static LRESULT CALLBACK PanelProc(HWND, UINT, WPARAM, LPARAM) noexcept;
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    HRESULT Open(HINSTANCE instance, HWND ownerWindow, RECT selection, const DisplayCaptureProfile& seed) noexcept;
    GifRecordingResult Tick() noexcept;
    void Layout() noexcept;
    void BeginFinishing() noexcept;
    void ShowRetry(const wchar_t* message) noexcept;
    void RequestCancel() noexcept;
    void FinishOrRetry() noexcept;
    void SignalStop() noexcept;

    HRESULT LaunchWorker(bool record) noexcept;
    HRESULT Encode(HWND capturePanel) noexcept;
    HRESULT ReadFrame(HWND capturePanel, RegionCaptureSource& source, Image& frame, TimePoint deadline) noexcept;
    // 调用方须持有 mutex。停止保留既有帧，取消丢弃整个输出。
    bool Interrupted() const noexcept { return cancelled || stopAt != TimePoint{}; }

    HWND owner = nullptr, panel = nullptr;
    HWND label = nullptr, finish = nullptr, cancel = nullptr;
    FontHandle font;
    RECT area{};
    DisplayCaptureProfile display{};
    const WPARAM session = ++nextSession;
    bool hideForSample = false;
    Phase phase = Phase::Preparing;
    bool fileReady = false;
    int attempts = 0, shownSeconds = -1;

    std::mutex mutex;
    std::condition_variable wake;
    KernelHandle stopEvent;
    TimePoint started{}, stopAt{};
    bool cancelled = false, finalizing = false, done = false;
    HRESULT permission = E_PENDING, workerStatus = E_PENDING;
    // 工作线程退出并由 UI 加入后，UI 才读取字节或发布文件。
    std::vector<std::uint8_t> gif;
    GifClipboardFile file;
    std::thread worker;
};

GifRecordingSession::State::~State() {
    {
        std::lock_guard lock(mutex);
        cancelled = true;
    }
    SignalStop();
    if (worker.joinable()) worker.join();
    if (panel) DestroyWindow(panel);
}

HRESULT GifRecordingSession::State::Open(HINSTANCE instance, HWND ownerWindow, RECT selection,
                                         const DisplayCaptureProfile& seed) noexcept {
    if (!ownerWindow || IsRectEmpty(&selection) || selection.left < seed.bounds.left || selection.top < seed.bounds.top ||
        selection.right > seed.bounds.right || selection.bottom > seed.bounds.bottom ||
        static_cast<std::int64_t>(selection.right) - selection.left > std::numeric_limits<WORD>::max() ||
        static_cast<std::int64_t>(selection.bottom) - selection.top > std::numeric_limits<WORD>::max()) return E_INVALIDARG;
    const HMONITOR monitor = seed.monitor;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info) || !EqualRect(&info.rcMonitor, &seed.bounds)) return S_FALSE;
    stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent) return LastErrorResult();

    owner = ownerWindow;
    area = selection;
    display = seed;
    RECT bounds = PanelBounds(area, IsRectEmpty(&info.rcWork) ? info.rcMonitor : info.rcWork, MonitorDpi(monitor));
    RECT overlap{};
    if (IntersectRect(&overlap, &bounds, &area)) {
        OtherMonitor search{monitor};
        EnumDisplayMonitors(nullptr, nullptr, FindOtherMonitor, reinterpret_cast<LPARAM>(&search));
        if (search.found) bounds = PanelBounds(area, search.work, MonitorDpi(search.found));
    }

    if (!CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kPanelClass, L"Picolet GIF 录制",
                         WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, bounds.left, bounds.top,
                         bounds.right - bounds.left, bounds.bottom - bounds.top, owner, nullptr, instance, this))
        return LastErrorResult();
    const auto add = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id) noexcept {
        return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, panel,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    label = add(L"STATIC", L"正在准备录制…", SS_NOPREFIX, 0);
    finish = add(L"BUTTON", L"停止并复制", BS_PUSHBUTTON | WS_DISABLED, FinishRecording);
    cancel = add(L"BUTTON", L"取消", BS_PUSHBUTTON, CancelRecording);
    if (!label || !finish || !cancel) return LastErrorResult();
    Layout();
    hideForSample = IntersectRect(&overlap, &bounds, &area) && !ExcludePanel(panel);
    ShowWindow(panel, SW_SHOWNOACTIVATE);
    return LaunchWorker(true);
}

void GifRecordingSession::State::Layout() noexcept {
    RECT client{};
    if (!label || !finish || !cancel || !GetClientRect(panel, &client)) return;
    const UINT actualDpi = GetDpiForWindow(panel);
    const UINT dpi = actualDpi ? actualDpi : USER_DEFAULT_SCREEN_DPI;
    font.reset(CreateUiFont(dpi));
    for (const HWND control : {label, finish, cancel})
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font.get()), TRUE);

    const int gap = Dip(9, dpi);
    const int buttonHeight = Dip(27, dpi);
    const int y = std::max(gap, static_cast<int>(client.bottom) - gap - buttonHeight);
    const int finishWidth = Dip(108, dpi);
    MoveWindow(label, gap, gap, std::max(1, static_cast<int>(client.right) - 2 * gap), Dip(23, dpi), TRUE);
    MoveWindow(finish, gap, y, finishWidth, buttonHeight, TRUE);
    MoveWindow(cancel, 2 * gap + finishWidth, y, Dip(62, dpi), buttonHeight, TRUE);
}

LRESULT CALLBACK GifRecordingSession::State::PanelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<State*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->panel = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->panel = nullptr;
        self = nullptr;
    }
    return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT GifRecordingSession::State::Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DPICHANGED:
        // 保持已计算的物理窗口范围，避免 DPI 更新后浮窗移入采集区域。
        Layout();
        return 0;
    case WM_CLOSE:
        RequestCancel();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) {
            const auto control = reinterpret_cast<HWND>(lparam);
            if (control == finish) FinishOrRetry();
            if (control == cancel) RequestCancel();
        }
        return 0;
    case kPrepareFrame:
        if (wparam == session && (phase == Phase::Preparing || phase == Phase::Recording)) {
            ShowWindow(panel, SW_HIDE);
            const HRESULT status = DwmFlush();
            {
                std::lock_guard lock(mutex);
                permission = status;
            }
            wake.notify_all();
        }
        return 0;
    case kRestorePanel:
        if (wparam == session && phase != Phase::Cancelling) ShowWindow(panel, SW_SHOWNOACTIVATE);
        return 0;
    }
    return DefWindowProcW(panel, message, wparam, lparam);
}

void GifRecordingSession::State::BeginFinishing() noexcept {
    phase = Phase::Finishing;
    EnableWindow(finish, FALSE);
    SetWindowTextW(label, L"正在生成 GIF…");
}

void GifRecordingSession::State::ShowRetry(const wchar_t* message) noexcept {
    phase = Phase::Retry;
    SetWindowTextW(label, message);
    SetWindowTextW(finish, L"重新复制");
    EnableWindow(finish, TRUE);
    ShowWindow(panel, SW_SHOWNOACTIVATE);
}

void GifRecordingSession::State::RequestCancel() noexcept {
    phase = Phase::Cancelling;
    {
        std::lock_guard lock(mutex);
        cancelled = true;
    }
    SignalStop();
    ShowWindow(panel, SW_HIDE);
}

void GifRecordingSession::State::FinishOrRetry() noexcept {
    if (phase == Phase::Recording) {
        BeginFinishing();
        {
            std::lock_guard lock(mutex);
            stopAt = Clock::now();
        }
        SignalStop();
    } else if (phase == Phase::Retry) {
        if (fileReady) {
            phase = Phase::Copying;
            attempts = 0;
            EnableWindow(finish, FALSE);
            SetWindowTextW(label, L"正在复制 GIF…");
        } else {
            BeginFinishing();
            if (FAILED(LaunchWorker(false))) ShowRetry(L"无法准备临时 GIF，请重新复制。");
        }
    }
}

void GifRecordingSession::State::SignalStop() noexcept {
    // 条件变量唤醒采样定时及浮窗握手，停止事件同时中断 HDR 后端的取帧等待。
    if (stopEvent) SetEvent(stopEvent.get());
    wake.notify_all();
}

HRESULT GifRecordingSession::State::LaunchWorker(bool record) noexcept {
    done = false;
    try {
        worker = std::thread([this, capturePanel = panel, record] {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HRESULT status = initialized;
            if (SUCCEEDED(initialized)) {
                status = record ? Encode(capturePanel) : S_OK;
                if (status == S_OK) {
                    {
                        std::lock_guard lock(mutex);
                        if (cancelled) status = E_ABORT;
                    }
                    if (SUCCEEDED(status)) status = file.Prepare(gif);
                }
                CoUninitialize();
            }
            const std::lock_guard lock(mutex);
            workerStatus = status;
            done = true;
        });
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT GifRecordingSession::State::ReadFrame(HWND capturePanel, RegionCaptureSource& source, Image& frame,
                                               TimePoint deadline) noexcept {
    {
        std::lock_guard lock(mutex);
        if (Interrupted()) return E_ABORT;
        permission = E_PENDING;
    }
    if (!hideForSample) return source.Read(frame);

    // 使用可被停止或取消唤醒的等待，关闭窗口时 UI 可以安全加入工作线程。
    if (!PostMessageW(capturePanel, kPrepareFrame, session, 0)) return LastErrorResult();
    HRESULT status = S_FALSE;
    {
        std::unique_lock lock(mutex);
        const bool ready = wake.wait_until(lock, deadline, [this] { return Interrupted() || permission != E_PENDING; });
        if (Interrupted()) status = E_ABORT;
        else if (ready) status = permission;
    }
    // HDR 必须从浮窗隐藏后开启的新帧池取帧，避免复用含浮窗的旧帧。
    if (status == S_OK) status = source.Read(frame, true);
    if (!PostMessageW(capturePanel, kRestorePanel, session, 0)) return LastErrorResult();
    return status;
}

HRESULT GifRecordingSession::State::Encode(HWND capturePanel) noexcept {
    GifEncoder encoder;
    HRESULT status = encoder.Open(area.right - area.left, area.bottom - area.top);
    if (FAILED(status)) return status;
    RegionCaptureSource source;
    status = source.Open(area, &display, stopEvent.get(), true);
    if (status != S_OK) return status;

    Image current, sample;
    status = ReadFrame(capturePanel, source, current, Clock::now() + kDurationLimit);
    if (status != S_OK) return FAILED(status) ? status : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    const auto first = Clock::now();
    {
        std::lock_guard lock(mutex);
        started = first;
    }
    auto frameTime = first;
    auto end = first + kDurationLimit;
    auto next = first + kFramePeriod;
    auto verifyAt = first + kDisplayCheckPeriod;
    bool reachedSize = false;
    for (;;) {
        {
            std::unique_lock lock(mutex);
            if (wake.wait_until(lock, std::min(next, end), [this] { return Interrupted(); })) break;
        }
        const auto tick = Clock::now();
        if (tick >= end) break;
        if (tick >= verifyAt) {
            // HDR 开关或参考白可在屏幕尺寸不变时调整，定期核对冻结时的显示配置。
            status = source.VerifyDisplay();
            if (status != S_OK) return status;
            verifyAt = tick + kDisplayCheckPeriod;
        }
        status = ReadFrame(capturePanel, source, sample, end);
        const auto sampled = Clock::now();
        {
            std::lock_guard lock(mutex);
            if (Interrupted() || sampled >= end) break;
        }
        if (FAILED(status)) return status;
        // 延后一帧提交，才能把实际间隔和连续相同画面的时间计入当前帧。
        // HDR 后端暂无新帧时继续计时，保留上一帧直到画面变化或停止。
        if (status == S_OK && !std::ranges::equal(current.pixels(), sample.pixels())) {
            status = encoder.AppendFrame(current, FrameDelay(first, frameTime, sampled));
            if (FAILED(status)) return status;
            if (encoder.encoded_size() >= kSizeThreshold) {
                reachedSize = true;
                break;
            }
            std::swap(current, sample);
            frameTime = sampled;
        }
        next = std::max(next + kFramePeriod, Clock::now());
    }

    {
        std::lock_guard lock(mutex);
        if (cancelled) return E_ABORT;
        if (stopAt != TimePoint{}) end = std::min(end, stopAt);
        finalizing = true;
    }
    source.Close();
    if (!reachedSize) {
        status = encoder.AppendFrame(current, FrameDelay(first, frameTime, end));
        if (FAILED(status)) return status;
    }
    return encoder.Finish(gif);
}

GifRecordingResult GifRecordingSession::State::Tick() noexcept {
    if (worker.joinable()) {
        TimePoint start;
        bool finished = false, ending = false;
        HRESULT status = E_PENDING;
        {
            std::lock_guard lock(mutex);
            start = started;
            finished = done;
            ending = finalizing;
            status = workerStatus;
        }
        if (!finished) {
            if (phase == Phase::Preparing && start != TimePoint{}) {
                phase = Phase::Recording;
                EnableWindow(finish, TRUE);
            }
            if (phase == Phase::Recording) {
                if (ending) BeginFinishing();
                else {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start);
                    const int seconds = static_cast<int>(std::min(elapsed, kDurationLimit).count());
                    if (seconds != shownSeconds) {
                        shownSeconds = seconds;
                        wchar_t text[48]{};
                        std::swprintf(text, std::size(text), L"正在录制 GIF   %02d:%02d", seconds / 60, seconds % 60);
                        SetWindowTextW(label, text);
                    }
                }
            }
            return GifRecordingResult::Pending;
        }

        worker.join();
        if (phase == Phase::Cancelling) return GifRecordingResult::Cancelled;
        if (status == S_FALSE) return GifRecordingResult::DisplayChanged;
        if (FAILED(status)) {
            if (gif.empty()) return GifRecordingResult::Failed;
            ShowRetry(L"无法准备临时 GIF，请重新复制。");
            return GifRecordingResult::Pending;
        }
        fileReady = true;
        phase = Phase::Copying;
        attempts = 0;
        EnableWindow(finish, FALSE);
        SetWindowTextW(label, L"正在复制 GIF…");
        ShowWindow(panel, SW_SHOWNOACTIVATE);
    }

    if (phase == Phase::Cancelling) return GifRecordingResult::Cancelled;
    if (phase == Phase::Copying) {
        const HRESULT status = file.Copy(owner, gif);
        if (status == S_OK) return GifRecordingResult::Copied;
        if (status != HRESULT_FROM_WIN32(ERROR_BUSY)) ShowRetry(L"无法复制 GIF，请重新复制。");
        else if (++attempts >= kClipboardAttempts) ShowRetry(L"剪贴板正忙，请重新复制。");
    }
    return GifRecordingResult::Pending;
}

GifRecordingSession::GifRecordingSession() noexcept = default;
GifRecordingSession::~GifRecordingSession() = default;

bool GifRecordingSession::Register(HINSTANCE instance, HICON icon) noexcept {
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.hInstance = instance;
    definition.hIcon = icon;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    definition.lpfnWndProc = State::PanelProc;
    definition.lpszClassName = kPanelClass;
    return RegisterClassExW(&definition) != 0;
}

HRESULT GifRecordingSession::Open(HINSTANCE instance, HWND owner, RECT area, const DisplayCaptureProfile& seed) noexcept {
    Close();
    // 部分工具链的条件变量构造可能抛异常，窗口创建失败时也由 State 统一清理。
    try {
        auto state = std::make_unique<State>();
        const auto status = state->Open(instance, owner, area, seed);
        if (status == S_OK) state_ = std::move(state);
        return status;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

GifRecordingResult GifRecordingSession::Tick() noexcept {
    return state_ ? state_->Tick() : GifRecordingResult::Pending;
}

void GifRecordingSession::Close() noexcept {
    state_.reset();
}

}  // namespace picolet
