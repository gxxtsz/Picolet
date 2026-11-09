#include <picolet/image/export.h>
#include <picolet/system/hotkeys.h>
#include <picolet/ui/overlay.h>
#include <picolet/ui/long_capture.h>
#include <picolet/ui/pins.h>
#include <picolet/resources/resources.h>
#include <picolet/ui/shortcut_dialog.h>
#include <picolet/system/startup.h>
#include <picolet/win32/win32.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cwchar>

namespace picolet {
namespace {

constexpr wchar_t kTitle[] = L"Picolet";
constexpr wchar_t kControllerClass[] = L"Picolet.Tray";
constexpr wchar_t kInstanceName[] = L"Local\\Picolet.Screenshot.Instance";
constexpr UINT kTrayIcon = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kCaptureRequest = WM_APP + 4;
enum TrayCommand : UINT { TrayCapture = 2001, TraySettings, TrayExit };
enum class Operation { Idle, PendingCapture, Selecting, Scrolling, EditingLong, EditingSettings, Output, Exiting };

void ShowError(HWND owner, const wchar_t* message) noexcept {
    MessageBoxW(owner, message, kTitle, MB_OK | MB_ICONERROR);
}

// 隐藏的顶层窗口统一接收托盘、热键与显示变化。冻结帧、会话和输出在同一线程串行管理。
class App {
public:
    explicit App(HINSTANCE instance, bool autostart) noexcept : instance_(instance), autostart_(autostart) {}
    ~App() { if (window_) DestroyWindow(window_); }

    bool Initialize() noexcept;
    int Run() noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept;

    NOTIFYICONDATAW TrayData() const noexcept;
    bool AddTrayIcon() noexcept;
    void HandleTrayMessage(WPARAM wparam, LPARAM lparam) noexcept;
    void ShowTrayMenu(POINT anchor) noexcept;
    void ShowSettings() noexcept;

    void RequestCapture() noexcept;
    void StartCapture() noexcept;
    void HandleOverlayAction(OverlayAction action) noexcept;
    // 长截图保留原遮罩直到新编辑器成功打开；取消或创建失败均可恢复原截图。
    void StartLongCapture() noexcept;
    void HandleScrollAction(ScrollAction action) noexcept;
    SelectionOverlay& ActiveEditor() noexcept { return longEditing_ ? longEditor_ : overlay_; }
    void EndCapture(const wchar_t* error = nullptr) noexcept;

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool trayAdded_ = false;
    bool trayVersion4_ = false;
    bool autostart_ = false;
    bool hotkeysStarted_ = false;
    TrayStartupRetry trayRetry_;
    bool menuOpen_ = false;
    Operation operation_ = Operation::Idle;
    Hotkeys hotkeys_;
    DesktopFrame frame_;
    SelectionOverlay overlay_;
    SelectionOverlay longEditor_;
    LongCaptureSession scrolling_;
    DesktopFrame longFrame_;
    RECT longAnchor_{};
    bool longEditing_ = false;
    PinWindows pins_;
};

bool App::Initialize() noexcept {
    icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_PICOLET));
    if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = WindowProc;
    definition.hInstance = instance_;
    definition.hIcon = icon_;
    definition.lpszClassName = kControllerClass;
    if (!RegisterClassExW(&definition) || !SelectionOverlay::Register(instance_, icon_) ||
        !PinWindows::Register(instance_, icon_) || !LongCaptureSession::Register(instance_, icon_)) return false;
    // 使用隐藏的顶层窗口。消息专用窗口收不到 Explorer 的托盘重建广播。
    if (!CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, kTitle, WS_POPUP,
                         0, 0, 0, 0, nullptr, nullptr, instance_, this)) return false;
    if (AddTrayIcon()) return true;
    // 登录启动时 Explorer 可能仍在初始化：保留消息循环异步等待，不阻塞界面线程。
    return autostart_ && SUCCEEDED(trayRetry_.Begin(window_, GetTickCount64()));
}

int App::Run() noexcept {
    MSG message{};
    BOOL next;
    while ((next = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if ((operation_ == Operation::Selecting || operation_ == Operation::EditingLong) && ActiveEditor().HandleKey(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return next == -1 ? 1 : static_cast<int>(message.wParam);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self && message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->window_ = nullptr;
        self = nullptr;
    }
    return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT App::Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (taskbarCreated_ && message == taskbarCreated_) {
        if (trayRetry_.waiting()) {
            if (trayRetry_.Attempt(GetTickCount64(), [this]() noexcept { return AddTrayIcon(); }) ==
                TrayStartupRetry::Result::TimedOut) DestroyWindow(window_);
        } else AddTrayIcon();
        return 0;
    }
    switch (message) {
    case WM_TIMER:
        if (wparam == TrayStartupRetry::kTimer && trayRetry_.waiting()) {
            if (trayRetry_.Attempt(GetTickCount64(), [this]() noexcept { return AddTrayIcon(); }) ==
                TrayStartupRetry::Result::TimedOut) DestroyWindow(window_);
            return 0;
        }
        break;
    case kTrayMessage:
        HandleTrayMessage(wparam, lparam);
        return 0;
    case WM_HOTKEY:
        if (hotkeys_.Match(wparam, lparam)) RequestCapture();
        return 0;
    case kCaptureRequest:
        if (operation_ == Operation::PendingCapture) StartCapture();
        return 0;
    case kOverlayAction:
        if ((operation_ == Operation::Selecting || operation_ == Operation::EditingLong) &&
            reinterpret_cast<HWND>(lparam) == ActiveEditor().window()) HandleOverlayAction(static_cast<OverlayAction>(wparam));
        return 0;
    case kScrollAction:
        if (operation_ == Operation::Scrolling && reinterpret_cast<HWND>(lparam) == scrolling_.window())
            HandleScrollAction(static_cast<ScrollAction>(wparam));
        return 0;
    case WM_DISPLAYCHANGE:
        if (operation_ == Operation::Scrolling) scrolling_.StopForDisplayChange();
        else if (!longEditing_ && (operation_ == Operation::Selecting || operation_ == Operation::Output)) overlay_.Cancel();
        return 0;
    case WM_ENDSESSION:
        if (wparam) DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        operation_ = Operation::Exiting;
        trayRetry_.Stop();
        hotkeys_.Stop();
        overlay_.Close();
        longEditor_.Close();
        scrolling_.Close();
        pins_.CloseAll();
        if (trayAdded_) {
            auto data = TrayData();
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

NOTIFYICONDATAW App::TrayData() const noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIcon;
    return data;
}

bool App::AddTrayIcon() noexcept {
    auto data = TrayData();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon_;
    lstrcpynW(data.szTip, L"Picolet 截图工具", static_cast<int>(std::size(data.szTip)));
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (trayAdded_) {
        trayRetry_.Stop();
        data.uVersion = NOTIFYICON_VERSION_4;
        trayVersion4_ = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        // 重试成功及 Explorer 重建均走此处，但快捷键配置只加载一次。
        if (!hotkeysStarted_) { hotkeys_.Start(window_); hotkeysStarted_ = true; }
    }
    return trayAdded_;
}

void App::HandleTrayMessage(WPARAM wparam, LPARAM lparam) noexcept {
    const UINT event = trayVersion4_ ? LOWORD(lparam) : static_cast<UINT>(lparam);
    if ((trayVersion4_ && (event == NIN_SELECT || event == NIN_KEYSELECT)) || (!trayVersion4_ && event == WM_LBUTTONUP)) {
        RequestCapture();
        return;
    }
    if ((trayVersion4_ && event == WM_CONTEXTMENU) || (!trayVersion4_ && event == WM_RBUTTONUP)) {
        POINT anchor{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
        if (!trayVersion4_ || (anchor.x == -1 && anchor.y == -1)) GetCursorPos(&anchor);
        ShowTrayMenu(anchor);
    }
}

void App::ShowTrayMenu(POINT anchor) noexcept {
    if (operation_ != Operation::Idle || menuOpen_) return;
    const MenuHandle menu(CreatePopupMenu());
    if (!menu) return;
    wchar_t key[96]{};
    wchar_t escaped[192]{};
    wchar_t label[220]{};
    const auto binding = hotkeys_.bindings()[ShortcutIndex(Shortcut::Capture)];
    FormatHotkey(binding, key);
    std::size_t n = 0;
    for (const wchar_t* p = key; *p && n + 2 < std::size(escaped); ++p) {
        if (*p == L'&') escaped[n++] = L'&';
        escaped[n++] = *p;
    }
    std::swprintf(label, std::size(label), L"截图\t%ls%ls", escaped,
                  binding.key && !hotkeys_.enabled() ? L"（未启用）" : L"");
    AppendMenuW(menu.get(), MF_STRING, TrayCapture, label);
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, TraySettings, L"设置…");
    AppendMenuW(menu.get(), MF_STRING, TrayExit, L"退出");

    menuOpen_ = true;
    hotkeys_.Pause();
    SetForegroundWindow(window_);
    const UINT alignment = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command = TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | alignment,
                                          anchor.x, anchor.y, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);
    menuOpen_ = false;
    switch (command) {
    case TrayCapture: RequestCapture(); break;
    case TraySettings: ShowSettings(); break;
    case TrayExit: DestroyWindow(window_); break;
    default: hotkeys_.Resume(); break;
    }
}

void App::ShowSettings() noexcept {
    operation_ = Operation::EditingSettings;
    hotkeys_.Pause();
    if (ShowSettingsDialog(instance_, window_, hotkeys_) == -1) {
        ShowError(window_, L"无法打开设置。");
    }
    if (operation_ == Operation::Exiting) return;
    operation_ = Operation::Idle;
    hotkeys_.Resume();
}

void App::RequestCapture() noexcept {
    if (operation_ != Operation::Idle || menuOpen_) return;
    hotkeys_.Pause();
    operation_ = Operation::PendingCapture;
    // 托盘菜单先退出自身消息循环，下一条消息再取帧，避免把菜单截进去。
    if (!PostMessageW(window_, kCaptureRequest, 0, 0)) EndCapture(L"无法启动截图，请重试。");
}

void App::StartCapture() noexcept {
    POINT origin{};
    GetCursorPos(&origin);
    // 已有贴图和其他桌面窗口一起取帧；任何新遮罩都在抓取完成后创建。
    DwmFlush();
    if (FAILED(CaptureDesktop(frame_))) {
        EndCapture(L"无法读取桌面画面，请重试。");
        return;
    }
    operation_ = Operation::Selecting;
    if (FAILED(overlay_.Open(instance_, window_, frame_, origin, hotkeys_.bindings()))) {
        EndCapture(L"无法打开截图界面，请重试。");
    }
}

void App::HandleOverlayAction(OverlayAction action) noexcept {
    if (action == OverlayAction::LongCapture) { if (!longEditing_) StartLongCapture(); return; }
    auto& editor = ActiveEditor();
    if (action == OverlayAction::Cancel || editor.invalidated()) {
        EndCapture();
        return;
    }
    operation_ = Operation::Output;
    editor.BeginOutput();
    Image image;
    const auto composed = editor.Render(image);
    if (FAILED(composed)) {
        operation_ = longEditing_ ? Operation::EditingLong : Operation::Selecting;
        editor.Retry(L"无法生成图片，请重试。");
        return;
    }
    HRESULT status = E_UNEXPECTED;
    const wchar_t* error = L"无法输出截图，请重试。";
    switch (action) {
    case OverlayAction::Copy:
        status = CopyImageToClipboard(editor.window(), image);
        error = L"无法复制图片，请稍后重试。";
        break;
    case OverlayAction::Save:
        status = SavePngWithDialog(editor.window(), image);
        error = L"保存失败，请检查保存位置后重试。";
        break;
    case OverlayAction::Pin:
        status = pins_.Open(instance_, window_, std::move(image), longEditing_ ? longAnchor_ : editor.selection());
        error = L"无法打开贴图，请重试。";
        break;
    default:
        break;
    }
    if (operation_ == Operation::Exiting) return;
    if (status == S_OK || editor.invalidated()) {
        EndCapture();
    } else {
        operation_ = longEditing_ ? Operation::EditingLong : Operation::Selecting;
        editor.Retry(FAILED(status) ? error : nullptr);
    }
}

void App::StartLongCapture() noexcept {
    Image seed, annotated;
    if (FAILED(overlay_.CaptureSeed(seed)) || FAILED(overlay_.Render(annotated))) {
        overlay_.Retry(L"无法准备长截图，请重试。");
        return;
    }
    longAnchor_ = overlay_.selection();
    operation_ = Operation::Scrolling;
    overlay_.Suspend();
    DwmFlush();
    const auto hr = scrolling_.Open(instance_, window_, longAnchor_, seed, std::move(annotated),
                                    hotkeys_.bindings()[ShortcutIndex(Shortcut::Cancel)]);
    if (FAILED(hr)) {
        scrolling_.Close();
        operation_ = Operation::Selecting;
        overlay_.Resume(L"长截图需至少 32×64 像素，并完整位于同一显示器的同一窗口中；请调整范围后重试。");
    }
}

void App::HandleScrollAction(ScrollAction action) noexcept {
    if (action == ScrollAction::Cancel) {
        scrolling_.Close();
        operation_ = Operation::Selecting;
        overlay_.Resume();
        return;
    }
    DesktopFrame document;
    SeedPlacement placement;
    auto hr = scrolling_.Finish(document.image, placement);
    if (FAILED(hr)) { scrolling_.Retry(L"无法合成长图，已截内容已保留，请重试。"); return; }
    document.bounds = {0, 0, document.image.width(), document.image.height()};
    AnnotationHistory history;
    hr = overlay_.MapHistory(placement, document, history);
    if (FAILED(hr)) { scrolling_.Retry(L"无法迁移标注，已截内容已保留，请重试。"); return; }
    longFrame_ = std::move(document);
    hr = longEditor_.OpenEditor(instance_, window_, longFrame_, longAnchor_, hotkeys_.bindings(), std::move(history));
    if (FAILED(hr)) {
        longEditor_.Close();
        longFrame_ = {};
        scrolling_.Retry(L"无法打开长图编辑窗口，请重试。");
        return;
    }
    scrolling_.Close();
    overlay_.Close();
    frame_ = {};
    longEditing_ = true;
    operation_ = Operation::EditingLong;
}

void App::EndCapture(const wchar_t* error) noexcept {
    overlay_.Close();
    longEditor_.Close();
    scrolling_.Close();
    frame_ = {};
    longFrame_ = {};
    longEditing_ = false;
    if (error) ShowError(window_, error);
    if (operation_ == Operation::Exiting) return;
    operation_ = Operation::Idle;
    hotkeys_.Resume();
}

}  // namespace
}  // namespace picolet

// Unicode 图形程序入口。互斥体保持到进程退出，所有 COM 接口都在反初始化前释放。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace picolet;
    const bool autostart = IsAutostartCommandLine(GetCommandLineW());
    const KernelHandle instanceLock(CreateMutexW(nullptr, FALSE, kInstanceName));
    const DWORD lockStatus = GetLastError();
    if (!instanceLock) {
        ShowError(nullptr, L"无法启动 Picolet。");
        return 1;
    }
    if (lockStatus == ERROR_ALREADY_EXISTS) {
        if (!autostart) MessageBoxW(nullptr, L"Picolet 已在系统托盘运行。", kTitle, MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        ShowError(nullptr, L"无法初始化截图工具所需的系统组件。");
        return 1;
    }
    const Gdiplus::GdiplusStartupInput graphicsInput;
    ULONG_PTR graphicsToken = 0;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&controls) ||
        Gdiplus::GdiplusStartup(&graphicsToken, &graphicsInput, nullptr) != Gdiplus::Ok) {
        ShowError(nullptr, L"无法初始化截图工具栏所需的系统组件。");
        CoUninitialize();
        return 1;
    }
    int result = 1;
    {
        App app(instance, autostart);
        if (app.Initialize()) result = app.Run();
        else if (!autostart) ShowError(nullptr, L"无法创建系统托盘入口。请确认 Windows 任务栏已启动。");
    }
    Gdiplus::GdiplusShutdown(graphicsToken);
    CoUninitialize();
    return result;
}
