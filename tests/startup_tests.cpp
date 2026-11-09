// 测试进程将 HKCU 重定向到临时子键。真实 Run 项及用户快捷键配置始终不参与写入。
#include <picolet/resources/resources.h>
#include <picolet/system/settings.h>
#include <picolet/ui/shortcut_dialog.h>

#include <commctrl.h>
#include <shellapi.h>
#include <sddl.h>

#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace picolet;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 先持有真实 HKCU 的独立句柄，再覆盖预定义句柄；析构先恢复映射，再删除本测试自己的整棵子树。
class RegistrySandbox {
public:
    RegistrySandbox() {
        HKEY raw = nullptr;
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, L"", 0, KEY_ALL_ACCESS, &raw) == ERROR_SUCCESS, "无法打开测试注册表根键");
        original_.reset(raw);
        Check(SUCCEEDED(StartupRegistration(original_.get()).Read(realBefore_)), "无法记录真实启动项");
        path_ = L"Software\\Picolet_Test_Startup_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        Check(RegCreateKeyExW(original_.get(), path_.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &raw, nullptr)
              == ERROR_SUCCESS, "无法创建临时注册表子键");
        root_.reset(raw);
        if (RegOverridePredefKey(HKEY_CURRENT_USER, root_.get()) != ERROR_SUCCESS) {
            root_.reset();
            RegDeleteTreeW(original_.get(), path_.c_str());
            throw std::runtime_error("无法隔离测试进程的 HKCU");
        }
        overridden_ = true;
    }

    ~RegistrySandbox() {
        if (overridden_) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        root_.reset();
        RegDeleteTreeW(original_.get(), path_.c_str());
    }

    HKEY root() const noexcept { return root_.get(); }
    void CheckRealUnchanged() const {
        StartupValue after;
        Check(SUCCEEDED(StartupRegistration(original_.get()).Read(after)) && after == realBefore_, "测试改变了真实启动项");
    }

private:
    UniqueHandle<HKEY, RegCloseKey> original_;
    UniqueHandle<HKEY, RegCloseKey> root_;
    std::wstring path_;
    StartupValue realBefore_;
    bool overridden_ = false;
};

StartupValue ReadValue(const StartupRegistration& registration) {
    StartupValue value;
    Check(SUCCEEDED(registration.Read(value)), "启动项读取失败");
    return value;
}

std::wstring Command(const StartupValue& value) {
    Check(value.present && value.type == REG_SZ && value.bytes.size() >= sizeof(wchar_t) &&
          value.bytes.size() % sizeof(wchar_t) == 0, "启动命令不是 REG_SZ");
    std::wstring command(value.bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(command.data(), value.bytes.data(), value.bytes.size());
    Check(command.back() == L'\0', "启动命令没有终止符");
    command.pop_back();
    return command;
}

void TestCommand() {
    std::wstring command;
    Check(BuildStartupCommand(L"C:\\中文 目录\\Picolet.exe", command) == S_OK &&
          command == L"\"C:\\中文 目录\\Picolet.exe\" --autostart", "中文及空格路径转义错误");
    Check(IsAutostartCommandLine(command.c_str()), "生成的启动命令未识别自启动参数");
    Check(BuildStartupCommand(L"\\\\服务器\\共享目录\\Picolet.exe", command) == S_OK, "UNC 路径被拒绝");
    Check(!IsAutostartCommandLine(L"\"C:\\--autostart\\Picolet.exe\""), "把程序路径当成自启动参数");
    Check(!IsAutostartCommandLine(L"Picolet.exe --autostart-extra --AUTOSTART") &&
          !IsAutostartCommandLine(L"Picolet.exe \"prefix --autostart\""), "自启动参数不是精确匹配");
    Check(IsAutostartCommandLine(L"Picolet.exe \"--autostart\"") && !IsAutostartCommandLine(nullptr), "引号参数或空命令行处理错误");
    const std::wstring unchanged = command;
    for (const auto* invalid : {L"", L"Picolet.exe", L"C:Picolet.exe", L"C:\\bad\"path.exe"})
        Check(BuildStartupCommand(invalid, command) == E_INVALIDARG && command == unchanged, "非法路径改写了输出");
    const std::wstring embedded(L"C:\\bad\0path.exe", 15);
    Check(BuildStartupCommand(embedded, command) == E_INVALIDARG, "未拒绝内嵌零路径");
    std::wstring boundary = L"C:\\";
    boundary.append(260 - std::wstring_view(L"\"\" --autostart").size() - boundary.size(), L'x');
    Check(BuildStartupCommand(boundary, command) == S_OK && command.size() == 260, "260 字符边界被错误拒绝");
    const auto maximum = command;
    boundary.push_back(L'x');
    Check(BuildStartupCommand(boundary, command) == HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) &&
          command == maximum, "超长命令被截断或未报告错误");
    std::wstring current;
    Check(CurrentExecutablePath(current) == S_OK && !current.empty(), "无法获取实际程序路径");
}

void TestRegistration(HKEY root) {
    const StartupRegistration registration;
    Check(!ReadValue(registration).present, "新环境不应启用自启动");
    Check(registration.SetEnabled(false, L"ignored") == S_FALSE, "关闭不存在的启动项不幂等");
    HKEY raw = nullptr;
    Check(RegOpenKeyExW(root, kStartupKey, 0, KEY_QUERY_VALUE, &raw) == ERROR_FILE_NOT_FOUND, "只读查询创建了 Run 键");
    StartupValue before;
    Check(registration.SetEnabled(true, L"C:\\中文 空格\\Picolet.exe", &before) == S_OK && !before.present,
          "未保存原本不存在的启动项状态");
    const auto enabled = ReadValue(registration);
    Check(Command(enabled) == L"\"C:\\中文 空格\\Picolet.exe\" --autostart", "注册表启动命令错误");
    Check(RegOpenKeyExW(root, kStartupKey, 0, KEY_NOTIFY | KEY_SET_VALUE | KEY_QUERY_VALUE, &raw) == ERROR_SUCCESS,
          "无法打开测试 Run 键");
    const UniqueHandle<HKEY, RegCloseKey> key(raw);
    const DWORD unrelated = 123;
    Check(RegSetValueExW(key.get(), L"其他程序", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&unrelated), sizeof(unrelated))
          == ERROR_SUCCESS, "无法创建无关测试项");
    const KernelHandle notification(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(notification && RegNotifyChangeKeyValue(key.get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET, notification.get(), TRUE)
          == ERROR_SUCCESS, "无法监听重复写入");
    Check(registration.SetEnabled(true, L"C:\\中文 空格\\Picolet.exe") == S_FALSE &&
          WaitForSingleObject(notification.get(), 0) == WAIT_TIMEOUT, "未变化的启动项仍被写入");
    Check(registration.SetEnabled(true, L"D:\\新目录\\Picolet.exe", &before) == S_OK && before == enabled,
          "显式保存没有更新移动后的程序位置");
    Check(registration.Restore(before) == S_OK && ReadValue(registration) == enabled, "没有精确恢复旧路径");
    Check(registration.SetEnabled(false, {}) == S_OK && !ReadValue(registration).present &&
          registration.SetEnabled(false, {}) == S_FALSE, "关闭启动项失败");
    DWORD queried = 0, size = sizeof(queried);
    Check(RegQueryValueExW(key.get(), L"其他程序", nullptr, nullptr, reinterpret_cast<BYTE*>(&queried), &size)
          == ERROR_SUCCESS && queried == unrelated, "关闭自启动影响了其他程序");
    const StartupValue rawValue{true, REG_EXPAND_SZ, {0x01, 0x03, 0x00, 0x7f, 0xff}};
    Check(registration.Restore(rawValue) == S_OK && ReadValue(registration) == rawValue, "恢复改变了原始类型或字节");
    const StartupRegistration invalid(nullptr, L"Run");
    StartupValue output = rawValue;
    Check(invalid.Read(output) == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) && output == rawValue,
          "读取失败丢失错误码或覆盖了输出");
    Check(invalid.Restore(rawValue) == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE), "写入失败丢失系统错误码");
    Check(registration.Restore({}) == S_OK, "无法清理测试启动项");
}

void TestCommit(HKEY root) {
    const StartupRegistration registration;
    auto current = DefaultShortcuts();
    current[0] = {};
    auto requested = current;
    requested[ShortcutIndex(Shortcut::Copy)] = {};
    int calls = 0;
    const auto success = [&](const ShortcutBindings& bindings) noexcept {
        ++calls;
        return bindings == requested ? ShortcutResult{} : ShortcutResult{ShortcutError::Storage};
    };
    auto result = CommitSettings(registration, false, {}, current, current, success);
    Check(result.succeeded() && calls == 0, "无变更保存仍提交快捷键");
    auto duplicate = requested;
    duplicate[ShortcutIndex(Shortcut::Save)] = duplicate[ShortcutIndex(Shortcut::Pin)];
    result = CommitSettings(registration, true, L"C:\\Picolet.exe", current, duplicate, success);
    Check(result.shortcut.error == ShortcutError::Duplicate && !ReadValue(registration).present && calls == 0,
          "快捷键验证失败前写入了自启动");
    result = CommitSettings(registration, true, L"relative.exe", current, requested, success);
    Check(FAILED(result.startup) && calls == 0, "自启动写入失败后仍提交了快捷键");
    const auto failing = [&](const ShortcutBindings&) noexcept {
        ++calls;
        return ShortcutResult{ShortcutError::Storage};
    };
    result = CommitSettings(registration, true, L"C:\\Picolet.exe", current, requested, failing);
    Check(!result.succeeded() && SUCCEEDED(result.rollback) && !ReadValue(registration).present,
          "快捷键失败后没有恢复原本不存在的启动项");
    const StartupValue original{true, REG_BINARY, {0x31, 0x00, 0x72}};
    Check(registration.Restore(original) == S_OK, "无法准备恢复测试");
    result = CommitSettings(registration, false, {}, current, requested, failing);
    Check(SUCCEEDED(result.rollback) && ReadValue(registration) == original, "回滚没有精确恢复原始字节");
    result = CommitSettings(registration, true, L"C:\\Picolet.exe", current, requested, success);
    Check(result.succeeded() && Command(ReadValue(registration)) == L"\"C:\\Picolet.exe\" --autostart",
          "同时提交两类设置失败");
    result = CommitSettings(StartupRegistration(nullptr, L"Run"), std::nullopt, {}, current, requested, success);
    Check(result.succeeded(), "不可编辑的自启动阻碍了快捷键保存");
    HKEY raw = nullptr;
    Check(RegCreateKeyExW(root, L"回滚失败", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &raw, nullptr) == ERROR_SUCCESS,
          "无法创建回滚失败测试键");
    UniqueHandle<HKEY, RegCloseKey> transient(raw);
    const StartupRegistration broken(transient.get(), L"Run");
    Check(broken.Restore(original) == S_OK, "无法准备回滚失败快照");
    result = CommitSettings(broken, true, L"C:\\Picolet.exe", current, requested,
        [&](const ShortcutBindings&) noexcept {
            // 刻意使借用的测试句柄失效，模拟提交后存储不可用；不用真实键或修改系统 ACL。
            transient.reset();
            return ShortcutResult{ShortcutError::Storage};
        });
    Check(FAILED(result.rollback) && !result.succeeded(), "回滚失败被误报为保存成功");
    Check(registration.Restore({}) == S_OK, "无法清理提交测试");
}

// 仅更改临时键的 DACL，保留查询权限、拒绝新句柄写入；原先借用的管理句柄用于恢复权限。
// 构造阶段只准备描述符，Deny 才改变权限，便于在两类设置提交之间模拟真实 ERROR_ACCESS_DENIED。
class WriteDenial {
public:
    explicit WriteDenial(HKEY key) : key_(key) {
        DWORD size = 0;
        Check(RegGetKeySecurity(key_, DACL_SECURITY_INFORMATION, nullptr, &size) == ERROR_INSUFFICIENT_BUFFER,
              "无法读取测试键权限大小");
        original_.resize(size);
        Check(RegGetKeySecurity(key_, DACL_SECURITY_INFORMATION, original_.data(), &size) == ERROR_SUCCESS,
              "无法记录测试键权限");
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        Check(GetSecurityDescriptorControl(original_.data(), &control, &revision), "无法读取权限继承标志");
        restoreFlags_ = DACL_SECURITY_INFORMATION | ((control & SE_DACL_PROTECTED)
            ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x0002;;;WD)(A;;KA;;;WD)",
              SDDL_REVISION_1, &descriptor, nullptr), "无法创建拒绝写入的测试权限");
        denied_.reset(descriptor);
    }
    ~WriteDenial() { Restore(); }
    HRESULT Deny() noexcept {
        const LSTATUS status = RegSetKeySecurity(key_, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                                denied_.get());
        if (status == ERROR_SUCCESS) active_ = true;
        return HRESULT_FROM_WIN32(status);
    }
    HRESULT Restore() noexcept {
        if (!active_) return S_OK;
        const LSTATUS status = RegSetKeySecurity(key_, restoreFlags_, original_.data());
        if (status == ERROR_SUCCESS) active_ = false;
        return HRESULT_FROM_WIN32(status);
    }

private:
    HKEY key_;
    std::vector<BYTE> original_;
    UniqueHandle<HLOCAL, LocalFree> denied_;
    SECURITY_INFORMATION restoreFlags_ = 0;
    bool active_ = false;
};

void TestPermissionFailures(HKEY root) {
    const StartupRegistration registration(root, L"权限拒绝\\Run");
    const StartupValue original{true, REG_BINARY, {0x71, 0x51}};
    Check(registration.Restore(original) == S_OK, "无法准备权限失败测试");
    HKEY raw = nullptr;
    Check(RegOpenKeyExW(root, L"权限拒绝\\Run", 0, KEY_ALL_ACCESS, &raw) == ERROR_SUCCESS, "无法管理临时键权限");
    const UniqueHandle<HKEY, RegCloseKey> key(raw);
    WriteDenial denial(key.get());
    Check(denial.Deny() == S_OK && ReadValue(registration) == original, "拒绝写入时丢失查询权限");
    auto current = DefaultShortcuts(), requested = current;
    requested[ShortcutIndex(Shortcut::Copy)] = {};
    int calls = 0;
    auto result = CommitSettings(registration, true, L"C:\\Picolet.exe", current, requested,
        [&](const ShortcutBindings&) noexcept { ++calls; return ShortcutResult{}; });
    Check(result.startup == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && calls == 0 &&
          ReadValue(registration) == original, "启动项写入被拒绝后仍提交快捷键或改变了原值");
    Check(denial.Restore() == S_OK, "无法恢复测试键权限");
    HRESULT denied = E_UNEXPECTED;
    result = CommitSettings(registration, true, L"C:\\Picolet.exe", current, requested,
        [&](const ShortcutBindings&) noexcept {
            denied = denial.Deny();
            return ShortcutResult{ShortcutError::Storage};
        });
    Check(denied == S_OK && result.rollback == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) &&
          !result.succeeded() && ReadValue(registration).type == REG_SZ,
          "回滚被拒绝后没有交付错误或无法查询实际状态");
    Check(denial.Restore() == S_OK && registration.Restore(original) == S_OK, "无法清理权限测试");
}

HWND TestWindow() {
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Picolet 设置测试",
                           WS_POPUP, -20000, -20000, 40, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void TestTrayRetry() {
    const HWND window = TestWindow();
    Check(window != nullptr, "无法创建计时器测试窗口");
    {
        TrayStartupRetry retry;
        Check(retry.Begin(nullptr, 0) == E_INVALIDARG && !retry.waiting(), "无效窗口启动了重试");
        Check(retry.Begin(window, 100) == S_OK && retry.waiting(), "无法启动托盘重试");
        Check(!retry.Expired(30099) && retry.Expired(30100), "托盘重试超时边界错误");
        retry.Stop();
        Check(!retry.waiting() && !retry.Expired(100000), "停止后仍处理迟到重试");
        Check(!KillTimer(window, TrayStartupRetry::kTimer), "停止后仍遗留窗口计时器");
        Check(retry.Begin(window, 200) == S_OK, "无法反复启动托盘重试");
        Check(retry.Begin(window, 300) == S_OK && !retry.Expired(30299), "重新启动未重置等待期限");
        int attempts = 0;
        const auto delayedTray = [&]() noexcept { return ++attempts == 3; };
        Check(retry.Attempt(1300, delayedTray) == TrayStartupRetry::Result::Waiting &&
              retry.Attempt(2300, delayedTray) == TrayStartupRetry::Result::Waiting &&
              retry.Attempt(3300, delayedTray) == TrayStartupRetry::Result::Ready && !retry.waiting(),
              "托盘延迟就绪后未停止重试");
        Check(!KillTimer(window, TrayStartupRetry::kTimer), "托盘成功注册后仍遗留计时器");
        Check(retry.Attempt(4300, delayedTray) == TrayStartupRetry::Result::Inactive && attempts == 3,
              "迟到消息重复注册托盘");
        Check(retry.Begin(window, 5000) == S_OK &&
              retry.Attempt(35000, delayedTray) == TrayStartupRetry::Result::TimedOut && attempts == 3 && !retry.waiting(),
              "超时后仍尝试注册托盘");
        Check(!KillTimer(window, TrayStartupRetry::kTimer), "托盘超时后仍遗留计时器");
        Check(retry.Begin(window, 40000) == S_OK, "无法启动析构清理测试");
    }
    Check(!KillTimer(window, TrayStartupRetry::kTimer), "析构没有释放托盘重试计时器");
    DestroyWindow(window);
}

std::function<void(HWND)> dialogAction;
std::exception_ptr dialogFailure;
bool dialogVisited = false;

void CALLBACK DialogTimer(HWND dialog, UINT, UINT_PTR id, DWORD) {
    KillTimer(dialog, id);
    try {
        dialogVisited = true;
        dialogAction(dialog);
    } catch (...) {
        dialogFailure = std::current_exception();
        EndDialog(dialog, IDCANCEL);
    }
}

LRESULT CALLBACK DialogHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        const HWND dialog = reinterpret_cast<HWND>(wp);
        if (GetDlgItem(dialog, IDC_STARTUP_ENABLE)) {
            // 测试通过控件消息验证真实模态对话框；显示前移出桌面，不占用用户鼠标或键盘。
            SetWindowPos(dialog, nullptr, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetTimer(dialog, 7, 1, DialogTimer);
            return 1;  // 拒绝测试窗口激活，模态消息循环仍通过自己的计时器执行验证。
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

INT_PTR RunDialog(HWND owner, Hotkeys& hotkeys, std::function<void(HWND)> action,
                  const StartupRegistration& registration = StartupRegistration{}) {
    dialogAction = std::move(action);
    dialogFailure = nullptr;
    dialogVisited = false;
    const HHOOK hook = SetWindowsHookExW(WH_CBT, DialogHook, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "无法安装测试线程的对话框观察器");
    const INT_PTR result = ShowSettingsDialog(GetModuleHandleW(nullptr), owner, hotkeys, registration);
    UnhookWindowsHookEx(hook);
    dialogAction = {};
    if (dialogFailure) std::rethrow_exception(dialogFailure);
    Check(dialogVisited, "模态对话框未执行验证");
    return result;
}

void Click(HWND dialog, int control) { SendDlgItemMessageW(dialog, control, BM_CLICK, 0, 0); }

void SeedBindings(const ShortcutBindings& bindings) {
    HKEY raw = nullptr;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Picolet\\Screenshot", 0, nullptr, 0, KEY_SET_VALUE,
                          nullptr, &raw, nullptr) == ERROR_SUCCESS, "无法准备隔离快捷键");
    const UniqueHandle<HKEY, RegCloseKey> key(raw);
    const auto words = EncodeBindings(bindings);
    Check(RegSetValueExW(key.get(), L"Shortcuts", 0, REG_BINARY, reinterpret_cast<const BYTE*>(words.data()),
                        static_cast<DWORD>(sizeof(words))) == ERROR_SUCCESS, "无法写入隔离快捷键");
}

void TestDialogs(HKEY root) {
    auto initial = DefaultShortcuts();
    initial[0] = {};  // 不注册或暂停用户系统里的任何全局按键。
    SeedBindings(initial);
    const HWND owner = TestWindow();
    Check(owner != nullptr, "无法创建设置窗口所有者");
    Hotkeys hotkeys;
    hotkeys.Start(owner);
    const StartupRegistration registration;
    auto result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Check(IsDlgButtonChecked(dialog, IDC_STARTUP_ENABLE) == BST_UNCHECKED, "首次打开没有默认关闭");
        Click(dialog, IDC_STARTUP_ENABLE);
        Click(dialog, IDC_CLEAR_FIRST + static_cast<int>(ShortcutIndex(Shortcut::Copy)));
        Click(dialog, IDC_SHORTCUT_RESTORE);
        Check(IsDlgButtonChecked(dialog, IDC_STARTUP_ENABLE) == BST_CHECKED, "恢复快捷键默认影响了自启动草稿");
        Check(!ReadValue(registration).present, "点击复选框立即写入了启动项");
        Click(dialog, IDCANCEL);
    });
    Check(result == IDCANCEL && hotkeys.bindings() == initial && !ReadValue(registration).present, "取消未丢弃全部草稿");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Click(dialog, IDC_STARTUP_ENABLE);
        SendMessageW(dialog, WM_CLOSE, 0, 0);
    });
    Check(result == IDCANCEL && !ReadValue(registration).present, "关闭窗口保存了自启动草稿");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Click(dialog, IDC_STARTUP_ENABLE);
        Click(dialog, IDC_CLEAR_FIRST + static_cast<int>(ShortcutIndex(Shortcut::Copy)));
        Click(dialog, IDOK);
    });
    Check(result == IDOK && ReadValue(registration).present && !hotkeys.bindings()[ShortcutIndex(Shortcut::Copy)].key,
          "设置窗口未一起保存两类设置");
    auto saved = hotkeys.bindings();
    Hotkeys reloaded;
    reloaded.Start(owner);
    Check(reloaded.bindings() == saved, "快捷键提交只更新内存，没有保存到隔离注册表");
    reloaded.Stop();
    Check(registration.SetEnabled(true, L"C:\\旧位置\\Picolet.exe") == S_OK, "无法准备程序移动测试");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Check(IsDlgButtonChecked(dialog, IDC_STARTUP_ENABLE) == BST_CHECKED, "移动前的登记未显示勾选");
        Click(dialog, IDOK);
    });
    std::wstring expected;
    Check(CurrentExecutablePath(expected) == S_OK, "无法取得移动后测试路径");
    std::wstring expectedCommand;
    Check(BuildStartupCommand(expected, expectedCommand) == S_OK && Command(ReadValue(registration)) == expectedCommand,
          "保持勾选保存没有更新已移动的程序路径");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Check(IsDlgButtonChecked(dialog, IDC_STARTUP_ENABLE) == BST_CHECKED, "重开没有读取实际启动项");
        Click(dialog, IDOK);
    });
    Check(result == IDOK && hotkeys.bindings() == saved, "无变化的设置保存失败");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Click(dialog, IDC_STARTUP_ENABLE);
        Click(dialog, IDOK);
    });
    Check(result == IDOK && !ReadValue(registration).present, "设置窗口没有关闭自启动");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Check(!IsWindowEnabled(GetDlgItem(dialog, IDC_STARTUP_ENABLE)), "读取失败后自启动开关仍可用");
        Click(dialog, IDC_CLEAR_FIRST + static_cast<int>(ShortcutIndex(Shortcut::Save)));
        Click(dialog, IDOK);
    }, StartupRegistration(nullptr, L"Run"));
    Check(result == IDOK && !hotkeys.bindings()[ShortcutIndex(Shortcut::Save)].key, "读取失败阻碍了快捷键保存");
    HKEY unchangedRaw = nullptr;
    Check(RegCreateKeyExW(root, L"未改动启动项", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &unchangedRaw, nullptr)
          == ERROR_SUCCESS, "无法准备跳过未改动启动项测试");
    UniqueHandle<HKEY, RegCloseKey> unchangedRoot(unchangedRaw);
    const StartupRegistration unchanged(unchangedRoot.get(), L"Run");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        unchangedRoot.reset();
        Click(dialog, IDC_CLEAR_FIRST + static_cast<int>(ShortcutIndex(Shortcut::Pin)));
        Click(dialog, IDOK);
    }, unchanged);
    Check(result == IDOK && !hotkeys.bindings()[ShortcutIndex(Shortcut::Pin)].key,
          "没有改动的启动项仍被访问并阻碍快捷键保存");
    saved = hotkeys.bindings();
    HKEY raw = nullptr;
    Check(RegCreateKeyExW(root, L"对话框写入失败", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &raw, nullptr)
          == ERROR_SUCCESS, "无法准备对话框失败测试");
    UniqueHandle<HKEY, RegCloseKey> transient(raw);
    const StartupRegistration broken(transient.get(), L"Run");
    result = RunDialog(owner, hotkeys, [&](HWND dialog) {
        Click(dialog, IDC_STARTUP_ENABLE);
        Click(dialog, IDC_CLEAR_FIRST + static_cast<int>(ShortcutIndex(Shortcut::Undo)));
        transient.reset();
        Click(dialog, IDOK);
        Check(IsWindow(dialog) && IsDlgButtonChecked(dialog, IDC_STARTUP_ENABLE) == BST_CHECKED &&
              hotkeys.bindings() == saved, "保存失败关闭了设置窗口或丢弃了草稿");
        wchar_t status[240]{};
        GetDlgItemTextW(dialog, IDC_STARTUP_STATUS, status, static_cast<int>(std::size(status)));
        Check(std::wstring_view(status).find(L"0x") != std::wstring_view::npos, "保存失败没有显示系统错误");
        Click(dialog, IDCANCEL);
    }, broken);
    Check(result == IDCANCEL && hotkeys.bindings() == saved, "失败后取消改变了快捷键");
    hotkeys.Stop();
    DestroyWindow(owner);
}

void TestDuplicateProcess(const wchar_t* executable) {
    // 即使用户未运行 Picolet，也用现有实例互斥体模拟已运行状态；子进程应在创建任何界面前退出。
    const KernelHandle instance(CreateMutexW(nullptr, FALSE, L"Local\\Picolet.Screenshot.Instance"));
    Check(instance.get() != nullptr, "无法创建重复启动测试互斥体");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --autostart";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
          "无法创建重复自启动验证进程");
    const KernelHandle child(process.hProcess), thread(process.hThread);
    const DWORD waited = WaitForSingleObject(child.get(), 5000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child.get(), 1); // 只终止由本测试创建、尚未通过单实例检查的子进程。
        WaitForSingleObject(child.get(), 5000);
        throw std::runtime_error("重复自启动没有静默退出");
    }
    DWORD code = 1;
    Check(GetExitCodeProcess(child.get(), &code) && code == 0, "重复自启动返回失败");
}

}  // namespace

int RunStartupVerification(int argc, wchar_t** argv) {
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 1;
    try {
        RegistrySandbox sandbox;
        auto bindings = DefaultShortcuts();
        bindings[0] = {};
        if (argc == 2 && std::wstring_view(argv[1]) == L"--interactive") {
            SeedBindings(bindings);
            Hotkeys hotkeys;
            const HWND owner = TestWindow();
            hotkeys.Start(owner);
            ShowSettingsDialog(GetModuleHandleW(nullptr), nullptr, hotkeys);
            hotkeys.Stop();
            DestroyWindow(owner);
        } else {
            TestCommand();
            TestRegistration(sandbox.root());
            TestCommit(sandbox.root());
            TestPermissionFailures(sandbox.root());
            TestTrayRetry();
            TestDialogs(sandbox.root());
            if (argc == 2) TestDuplicateProcess(argv[1]);
        }
        sandbox.CheckRealUnchanged();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

#ifdef PICOLET_SETTINGS_FIXTURE
// 独立手工夹具复用相同的 HKCU 隔离，允许交互保存而不改变真实启动项或用户快捷键。
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t program[] = L"picolet_settings_fixture.exe";
    wchar_t option[] = L"--interactive";
    wchar_t* arguments[]{program, option};
    return RunStartupVerification(2, arguments);
}
#else
int wmain(int argc, wchar_t** argv) { return RunStartupVerification(argc, argv); }
#endif
