#include "export.h"
#include "hotkeys.h"
#include "overlay.h"
#include "preview.h"
#include "resources.h"
#include "win32.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <array>
#include <cwchar>
#include <iterator>
#include <utility>

namespace picolet {
namespace {

constexpr wchar_t kTitle[] = L"Picolet";
constexpr wchar_t kControllerClass[] = L"Picolet.Tray";
constexpr wchar_t kInstanceName[] = L"Local\\Picolet.Screenshot.Instance";
constexpr UINT kTrayIcon = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kCaptureRequest = WM_APP + 4;
enum TrayCommand : UINT { TrayArea = 2001, TrayDesktop, TraySettings, TrayExit };

struct ShortcutControls {
    int modifiers;
    int key;
};
constexpr std::array<ShortcutControls, 2> kShortcutControls{{
    {IDC_AREA_MODIFIERS, IDC_AREA_KEY},
    {IDC_DESKTOP_MODIFIERS, IDC_DESKTOP_KEY},
}};

enum class Operation { Idle, PendingCapture, Selecting, EditingHotkeys, Saving, Exiting };

void ShowError(HWND owner, const wchar_t* message) noexcept {
    MessageBoxW(owner, message, kTitle, MB_OK | MB_ICONERROR);
}

bool AddComboItem(HWND dialog, int control, const wchar_t* label, UINT value, bool selected) noexcept {
    const LRESULT item = SendDlgItemMessageW(dialog, control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    if (item < 0 || SendDlgItemMessageW(dialog, control, CB_SETITEMDATA, item, value) == CB_ERR) {
        return false;
    }
    if (selected) {
        SendDlgItemMessageW(dialog, control, CB_SETCURSEL, item, 0);
    }
    return true;
}

UINT SelectedComboValue(HWND dialog, int control) noexcept {
    const LRESULT item = SendDlgItemMessageW(dialog, control, CB_GETCURSEL, 0, 0);
    if (item == CB_ERR) {
        return 0;
    }
    const LRESULT value = SendDlgItemMessageW(dialog, control, CB_GETITEMDATA, item, 0);
    return value == CB_ERR ? 0 : static_cast<UINT>(value);
}

// 隐藏的顶层窗口统一接收托盘、热键与显示变化消息。截图和导出在同一 UI 线程串行执行。
class App {
public:
    explicit App(HINSTANCE instance) noexcept : instance_(instance) {}
    ~App() {
        if (window_) {
            DestroyWindow(window_);
        }
    }

    bool Initialize() noexcept;
    int Run() noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    static INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept;

    NOTIFYICONDATAW TrayData() const noexcept;
    bool AddTrayIcon() noexcept;
    void HandleTrayMessage(WPARAM wparam, LPARAM lparam) noexcept;
    void ShowTrayMenu(POINT anchor) noexcept;
    void ShowSettings() noexcept;
    bool PopulateSettings(HWND dialog) const noexcept;
    void ApplySettings(HWND dialog) noexcept;

    void RequestCapture(CaptureMode mode) noexcept;
    void StartCapture(CaptureMode mode) noexcept;
    void FinishSelection(bool accepted) noexcept;
    void PresentCapture(Image&& image, POINT origin) noexcept;
    void RestoreAfterCapture(const wchar_t* error = nullptr) noexcept;
    void HandlePreviewAction(UINT action) noexcept;

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    HACCEL accelerators_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool trayAdded_ = false;
    bool trayVersion4_ = false;
    bool menuOpen_ = false;
    Operation operation_ = Operation::Idle;
    Hotkeys hotkeys_;
    DesktopFrame frame_;
    SelectionOverlay overlay_;
    PreviewWindow preview_;
    PreviewVisibility previousPreview_ = PreviewVisibility::Hidden;
};

bool App::Initialize() noexcept {
    icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_PICOLET));
    if (!icon_) {
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }
    accelerators_ = LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_PREVIEW_KEYS));
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = WindowProc;
    definition.hInstance = instance_;
    definition.hIcon = icon_;
    definition.lpszClassName = kControllerClass;
    if (!RegisterClassExW(&definition) || !SelectionOverlay::Register(instance_, icon_) ||
        !PreviewWindow::Register(instance_, icon_)) {
        return false;
    }
    // 必须使用隐藏的顶层窗口，消息专用窗口收不到 Explorer 的托盘重建广播。
    if (!CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, kTitle, WS_POPUP,
                         0, 0, 0, 0, nullptr, nullptr, instance_, this) || !AddTrayIcon()) {
        return false;
    }
    hotkeys_.Start(window_);
    return true;
}

int App::Run() noexcept {
    MSG message{};
    BOOL next;
    while ((next = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (preview_.OwnsMessage(message.hwnd) &&
            ((accelerators_ && TranslateAcceleratorW(preview_.window(), accelerators_, &message)) ||
             IsDialogMessageW(preview_.window(), &message))) {
            continue;
        }
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

LRESULT App::Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (taskbarCreated_ && message == taskbarCreated_) {
        AddTrayIcon();
        return 0;
    }
    switch (message) {
    case kTrayMessage:
        HandleTrayMessage(wparam, lparam);
        return 0;
    case WM_HOTKEY:
        if (const auto mode = hotkeys_.Match(wparam, lparam)) {
            RequestCapture(*mode);
        }
        return 0;
    case kCaptureRequest:
        if (operation_ == Operation::PendingCapture) {
            StartCapture(static_cast<CaptureMode>(wparam));
        }
        return 0;
    case kOverlayFinished:
        if (operation_ == Operation::Selecting) {
            FinishSelection(wparam != 0);
        }
        return 0;
    case kPreviewAction:
        HandlePreviewAction(static_cast<UINT>(wparam));
        return 0;
    case WM_DISPLAYCHANGE:
        if (operation_ == Operation::Selecting) {
            overlay_.Cancel();
        }
        return 0;
    case WM_ENDSESSION:
        if (wparam) {
            DestroyWindow(window_);
        }
        return 0;
    case WM_DESTROY:
        operation_ = Operation::Exiting;
        overlay_.Close();
        preview_.Close();
        hotkeys_.Stop();
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
        data.uVersion = NOTIFYICON_VERSION_4;
        trayVersion4_ = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
    }
    return trayAdded_;
}

void App::HandleTrayMessage(WPARAM wparam, LPARAM lparam) noexcept {
    const UINT event = trayVersion4_ ? LOWORD(lparam) : static_cast<UINT>(lparam);
    if (event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT ||
        (!trayVersion4_ && (event == WM_RBUTTONUP || event == WM_LBUTTONUP))) {
        POINT anchor{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
        if (!trayVersion4_ || (anchor.x == -1 && anchor.y == -1)) {
            GetCursorPos(&anchor);
        }
        ShowTrayMenu(anchor);
    }
}

void App::ShowTrayMenu(POINT anchor) noexcept {
    if (operation_ != Operation::Idle || menuOpen_) {
        return;
    }
    const MenuHandle menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    constexpr std::array<const wchar_t*, 2> names{L"选区截图", L"全屏截图"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& hotkey = hotkeys_.bindings()[index];
        wchar_t key[4]{};
        wchar_t label[96]{};
        FormatKey(hotkey.key, key);
        std::swprintf(label, std::size(label), L"%ls\t%ls+%ls%ls", names[index], ModifierText(hotkey.modifiers),
                      key, hotkeys_.enabled(index) ? L"" : L"（未启用）");
        AppendMenuW(menu.get(), MF_STRING, TrayArea + index, label);
    }
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, TraySettings, L"快捷键设置…");
    AppendMenuW(menu.get(), MF_STRING, TrayExit, L"退出");

    menuOpen_ = true;
    SetForegroundWindow(window_);
    const UINT alignment = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command = TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | alignment,
                                          anchor.x, anchor.y, window_, nullptr);
    // 先结束菜单消息循环，再异步取帧，防止托盘菜单出现在截图里。
    PostMessageW(window_, WM_NULL, 0, 0);
    menuOpen_ = false;
    switch (command) {
    case TrayArea: RequestCapture(CaptureMode::Area); break;
    case TrayDesktop: RequestCapture(CaptureMode::Desktop); break;
    case TraySettings: ShowSettings(); break;
    case TrayExit: DestroyWindow(window_); break;
    }
}

void App::ShowSettings() noexcept {
    operation_ = Operation::EditingHotkeys;
    const HWND owner = IsWindowVisible(preview_.window()) && !IsIconic(preview_.window())
                           ? preview_.window() : window_;
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_HOTKEYS), owner, SettingsProc,
                        reinterpret_cast<LPARAM>(this)) == -1) {
        ShowError(owner, L"无法打开快捷键设置。");
    }
    operation_ = Operation::Idle;
}

INT_PTR CALLBACK App::SettingsProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_INITDIALOG) {
        auto* self = reinterpret_cast<App*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        if (!self->PopulateSettings(dialog)) {
            EndDialog(dialog, -1);
        }
        return TRUE;
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_COMMAND && self) {
        switch (LOWORD(wparam)) {
        case IDOK:
            self->ApplySettings(dialog);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

bool App::PopulateSettings(HWND dialog) const noexcept {
    for (std::size_t index = 0; index < kShortcutControls.size(); ++index) {
        const auto& controls = kShortcutControls[index];
        const auto& binding = hotkeys_.bindings()[index];
        for (const UINT modifier : kModifiers) {
            if (!AddComboItem(dialog, controls.modifiers, ModifierText(modifier), modifier,
                              modifier == binding.modifiers)) {
                return false;
            }
        }
        constexpr std::array<std::array<UINT, 2>, 3> ranges{{{'A', 'Z'}, {'0', '9'}, {VK_F1, VK_F11}}};
        for (const auto& range : ranges) {
            for (UINT value = range[0]; value <= range[1]; ++value) {
                wchar_t label[4]{};
                FormatKey(value, label);
                if (!AddComboItem(dialog, controls.key, label, value, value == binding.key)) {
                    return false;
                }
            }
        }
    }
    return true;
}

void App::ApplySettings(HWND dialog) noexcept {
    HotkeyPair requested{};
    for (std::size_t index = 0; index < requested.size(); ++index) {
        const auto& controls = kShortcutControls[index];
        requested[index] = {SelectedComboValue(dialog, controls.modifiers), SelectedComboValue(dialog, controls.key)};
    }
    std::size_t failedIndex{};
    const HRESULT status = hotkeys_.Apply(requested, failedIndex);
    if (SUCCEEDED(status)) {
        EndDialog(dialog, IDOK);
        return;
    }
    const wchar_t* error = L"无法保存快捷键设置，请重试。";
    if (status == E_INVALIDARG) {
        error = L"请为选区和全屏选择两个不同的组合。";
    } else if (status == HRESULT_FROM_WIN32(ERROR_HOTKEY_ALREADY_REGISTERED)) {
        error = failedIndex == 0 ? L"选区截图的快捷键已被占用，请换一个组合。"
                                 : L"全屏截图的快捷键已被占用，请换一个组合。";
    } else if (failedIndex < requested.size()) {
        error = failedIndex == 0 ? L"选区截图的快捷键无法注册，请换一个组合。"
                                 : L"全屏截图的快捷键无法注册，请换一个组合。";
    }
    SetDlgItemTextW(dialog, IDC_HOTKEY_STATUS, error);
}

void App::RequestCapture(CaptureMode mode) noexcept {
    if (operation_ != Operation::Idle || menuOpen_) {
        return;
    }
    operation_ = Operation::PendingCapture;
    if (!PostMessageW(window_, kCaptureRequest, static_cast<WPARAM>(mode), 0)) {
        RestoreAfterCapture(L"无法启动截图，请重试。");
    }
}

void App::StartCapture(CaptureMode mode) noexcept {
    POINT origin{};
    GetCursorPos(&origin);
    previousPreview_ = preview_.HideForCapture();
    if (IsWindowVisible(preview_.window()) && !IsIconic(preview_.window())) {
        RestoreAfterCapture(L"无法收起上一次截图的预览，请关闭预览后重试。");
        return;
    }
    // 隐藏旧预览并等待下一轮桌面合成，随后再读取桌面像素。
    DwmFlush();
    if (FAILED(CaptureDesktop(frame_))) {
        RestoreAfterCapture(L"无法读取桌面画面，请重试。");
        return;
    }
    if (mode == CaptureMode::Desktop) {
        PresentCapture(std::move(frame_.image), origin);
        frame_ = {};
        return;
    }
    operation_ = Operation::Selecting;
    if (FAILED(overlay_.Open(instance_, window_, frame_, origin))) {
        frame_ = {};
        RestoreAfterCapture(L"无法打开选区遮罩，请重试。");
    }
}

void App::FinishSelection(bool accepted) noexcept {
    Image image;
    const POINT origin = overlay_.last_point();
    const HRESULT status = accepted ? CropImage(frame_, overlay_.selection(), image) : S_FALSE;
    overlay_.Close();
    frame_ = {};
    if (status == S_OK) {
        PresentCapture(std::move(image), origin);
    } else {
        RestoreAfterCapture(FAILED(status) ? L"无法生成选区图像，请重试。" : nullptr);
    }
}

void App::PresentCapture(Image&& image, POINT origin) noexcept {
    if (FAILED(preview_.Present(instance_, window_, std::move(image), origin))) {
        RestoreAfterCapture(L"无法打开截图预览，请重试。");
        return;
    }
    previousPreview_ = PreviewVisibility::Hidden;
    operation_ = Operation::Idle;
}

void App::RestoreAfterCapture(const wchar_t* error) noexcept {
    preview_.Restore(std::exchange(previousPreview_, PreviewVisibility::Hidden));
    if (error) {
        ShowError(preview_.window(), error);
    }
    operation_ = Operation::Idle;
}

void App::HandlePreviewAction(UINT action) noexcept {
    if (operation_ != Operation::Idle || preview_.image().empty()) {
        return;
    }
    if (action == ID_PREVIEW_COPY) {
        const HRESULT status = CopyImageToClipboard(preview_.window(), preview_.image());
        preview_.SetStatus(SUCCEEDED(status) ? L"已复制到剪贴板。" : L"复制失败，请稍后重试。");
    } else if (action == ID_PREVIEW_SAVE) {
        operation_ = Operation::Saving;
        const HRESULT status = SavePngWithDialog(preview_.window(), preview_.image());
        if (status == S_OK) {
            preview_.SetStatus(L"已保存 PNG。");
        } else if (FAILED(status)) {
            preview_.SetStatus(L"保存失败，请检查保存位置后重试。");
        }
        operation_ = Operation::Idle;
    }
}

}  // namespace
}  // namespace picolet

// Unicode 图形程序入口。互斥体保持到进程退出，所有 COM 接口都在反初始化前释放。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace picolet;
    const KernelHandle instanceLock(CreateMutexW(nullptr, FALSE, kInstanceName));
    const DWORD lockStatus = GetLastError();
    if (!instanceLock) {
        ShowError(nullptr, L"无法启动 Picolet。");
        return 1;
    }
    if (lockStatus == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Picolet 已在系统托盘运行。", kTitle, MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        ShowError(nullptr, L"无法初始化截图工具所需的系统组件。");
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int result = 1;
    {
        App app(instance);
        if (app.Initialize()) {
            result = app.Run();
        } else {
            ShowError(nullptr, L"无法创建系统托盘入口。请确认 Windows 任务栏已启动。");
        }
    }
    CoUninitialize();
    return result;
}
