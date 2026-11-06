#include "shortcut_dialog.h"

#include "resources.h"

#include <commctrl.h>

#include <cwchar>

namespace picolet {
namespace {

constexpr wchar_t kHint[] = L"点击按键开始录制。清除后该操作仍可用鼠标执行。";
constexpr UINT kModifierKeys[]{VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
                              VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN};

class SettingsDialog {
public:
    explicit SettingsDialog(Hotkeys& hotkeys) noexcept : hotkeys_(hotkeys), draft_(hotkeys.bindings()) {}

    static INT_PTR CALLBACK Proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
        if (msg == WM_INITDIALOG) {
            auto* self = reinterpret_cast<SettingsDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
            self->dialog_ = dialog;
            for (std::size_t i = 0; i < kShortcutCount; ++i) {
                if (!SetWindowSubclass(GetDlgItem(dialog, IDC_SHORTCUT_FIRST + static_cast<int>(i)),
                                       RecordButton, 1, reinterpret_cast<DWORD_PTR>(self))) {
                    EndDialog(dialog, -1);
                    return TRUE;
                }
            }
            self->Refresh();
            return TRUE;
        }
        auto* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
        return self ? self->Handle(msg, wparam, lparam) : FALSE;
    }

private:
    // 前台 Raw Input 是实际录制来源。保留按钮的消息入口，以接收辅助输入设备发来的普通键消息。
    static LRESULT CALLBACK RecordButton(HWND window, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR data) noexcept {
        auto& self = *reinterpret_cast<SettingsDialog*>(data);
        if (self.recording_ != kShortcutCount && window == self.Button(self.recording_)) {
            switch (msg) {
            case WM_GETDLGCODE:
                return DLGC_WANTALLKEYS;
            case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
                self.ReadKey(static_cast<UINT>(wp), (HIWORD(lp) & 0xff), (HIWORD(lp) & KF_EXTENDED) != 0,
                             msg == WM_KEYUP || msg == WM_SYSKEYUP);
                return 0;
            case WM_CHAR: case WM_SYSCHAR:
                return 0;
            default:
                break;
            }
        }
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, RecordButton, id);
        return DefSubclassProc(window, msg, wp, lp);
    }

    HWND Button(std::size_t index) const noexcept { return GetDlgItem(dialog_, IDC_SHORTCUT_FIRST + static_cast<int>(index)); }

    void Status(const wchar_t* text) const noexcept { SetDlgItemTextW(dialog_, IDC_SHORTCUT_STATUS, text); }

    void ShowBinding(std::size_t index, Hotkey binding) const noexcept {
        wchar_t label[96]{};
        wchar_t escaped[192]{};
        FormatHotkey(binding, label);
        std::size_t count = 0;
        for (const wchar_t* p = label; *p && count + 2 < std::size(escaped); ++p) {
            if (*p == L'&') escaped[count++] = L'&';
            escaped[count++] = *p;
        }
        SetWindowTextW(Button(index), escaped);
    }

    void Refresh() const noexcept {
        for (std::size_t i = 0; i < draft_.size(); ++i) {
            ShowBinding(i, draft_[i]);
            EnableWindow(GetDlgItem(dialog_, IDC_CLEAR_FIRST + static_cast<int>(i)), draft_[i].key != 0);
        }
    }

    void BeginRecording(std::size_t index) noexcept {
        if (recording_ == index) {
            StopRecording(false);
            return;
        }
        StopRecording(false);
        // 不使用后台监听或键盘钩子。只在对话框前台录制时接收键盘，并让 Enter、Esc、Tab 也可录制。
        const RAWINPUTDEVICE device{0x01, 0x06, RIDEV_NOLEGACY, dialog_};
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
            Status(L"无法开始按键录制，请重试。");
            return;
        }
        recording_ = index;
        candidate_ = {};
        mainReleased_ = false;
        for (std::size_t i = 0; i < modifiers_.size(); ++i) modifiers_[i] = GetAsyncKeyState(kModifierKeys[i]) < 0;
        SetWindowTextW(Button(index), L"请按键…");
        SetFocus(Button(index));
        EnableWindow(GetDlgItem(dialog_, IDOK), FALSE);
        Status(L"按下并松开所需按键。再次点击该条目可停止录制。");
    }

    void StopRecording(bool keep, bool refresh = true) noexcept {
        if (recording_ == kShortcutCount) return;
        const RAWINPUTDEVICE device{0x01, 0x06, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&device, 1, sizeof(device));
        if (keep && candidate_.key) draft_[recording_] = candidate_;
        recording_ = kShortcutCount;
        if (refresh) {
            EnableWindow(GetDlgItem(dialog_, IDOK), TRUE);
            Refresh();
            Status(keep ? L"已录制。点击保存后生效。" : kHint);
        }
    }

    UINT Modifiers() const noexcept {
        return ((modifiers_[0] || modifiers_[1]) ? MOD_CONTROL : 0) |
               ((modifiers_[2] || modifiers_[3]) ? MOD_ALT : 0) |
               ((modifiers_[4] || modifiers_[5]) ? MOD_SHIFT : 0) |
               ((modifiers_[6] || modifiers_[7]) ? MOD_WIN : 0);
    }

    void ReadKey(UINT key, UINT scan, bool extended, bool up) noexcept {
        if (recording_ == kShortcutCount) return;
        if (key == VK_CONTROL) key = extended ? VK_RCONTROL : VK_LCONTROL;
        if (key == VK_MENU) key = extended ? VK_RMENU : VK_LMENU;
        if (key == VK_SHIFT) {
            // 部分键盘在扩展按键前发送额外的 Shift 扫描码，它并不表示用户按了 Shift。
            if (extended) return;
            key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
            if (key != VK_LSHIFT && key != VK_RSHIFT) key = VK_LSHIFT;
        }
        for (std::size_t i = 0; i < modifiers_.size(); ++i) {
            if (key == kModifierKeys[i]) {
                modifiers_[i] = !up;
                if (candidate_.key && mainReleased_ && !Modifiers()) StopRecording(true);
                return;
            }
        }
        if (!IsMainKey(key)) return;
        // Print Screen 在部分键盘布局只报告释放；Pause 则可能没有释放事件。
        if (!candidate_.key && (!up || key == VK_SNAPSHOT)) {
            candidate_ = {Modifiers(), key};
            mainReleased_ = up || key == VK_PAUSE;
            ShowBinding(recording_, candidate_);
        }
        if (up && candidate_.key == key) mainReleased_ = true;
        if (candidate_.key && mainReleased_ && !Modifiers()) StopRecording(true);
    }

    void ReadRawInput(HRAWINPUT handle) noexcept {
        if (recording_ == kShortcutCount) return;
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
            raw.header.dwType != RIM_TYPEKEYBOARD) return;
        const auto& key = raw.data.keyboard;
        if (key.VKey >= 0xff || key.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE ||
            ((key.Flags & RI_KEY_E1) && key.VKey == VK_CONTROL)) return;
        ReadKey(key.VKey, key.MakeCode, (key.Flags & RI_KEY_E0) != 0, (key.Flags & RI_KEY_BREAK) != 0);
    }

    void Save() noexcept {
        if (recording_ != kShortcutCount) return;
        const auto result = hotkeys_.Apply(draft_);
        if (result.error == ShortcutError::None) {
            EndDialog(dialog_, IDOK);
            return;
        }
        const wchar_t* reason = L"无法使用该按键。";
        switch (result.error) {
        case ShortcutError::Duplicate: reason = L"与另一条截图内快捷键重复。"; break;
        case ShortcutError::Reserved: reason = L"该键由 Windows 保留，请换一个。"; break;
        case ShortcutError::Occupied: reason = L"该键已被占用，请换一个。"; break;
        case ShortcutError::Registration: reason = L"Windows 无法注册该键，请换一个。"; break;
        case ShortcutError::Storage: Status(L"无法保存快捷键设置，原设置已保留。"); return;
        default: break;
        }
        wchar_t message[120]{};
        std::swprintf(message, std::size(message), L"%ls：%ls", ShortcutName(result.slot), reason);
        Status(message);
    }

    INT_PTR Handle(UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
        switch (msg) {
        case WM_INPUT:
            ReadRawInput(reinterpret_cast<HRAWINPUT>(lparam));
            DefWindowProcW(dialog_, msg, wparam, lparam);
            return TRUE;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) StopRecording(false);
            return FALSE;
        case WM_DESTROY:
            StopRecording(false, false);
            return FALSE;
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            if (id >= IDC_SHORTCUT_FIRST && id < IDC_SHORTCUT_FIRST + static_cast<int>(kShortcutCount)) {
                BeginRecording(static_cast<std::size_t>(id - IDC_SHORTCUT_FIRST));
            } else if (id >= IDC_CLEAR_FIRST && id < IDC_CLEAR_FIRST + static_cast<int>(kShortcutCount)) {
                StopRecording(false);
                draft_[id - IDC_CLEAR_FIRST] = {};
                Refresh();
                Status(L"已清除。点击保存后生效。");
            } else if (id == IDC_SHORTCUT_RESTORE) {
                StopRecording(false);
                draft_ = DefaultShortcuts();
                Refresh();
                Status(L"已恢复默认值。点击保存后生效。");
            } else if (id == IDOK) {
                Save();
            } else if (id == IDCANCEL) {
                StopRecording(false);
                EndDialog(dialog_, IDCANCEL);
            } else {
                return FALSE;
            }
            return TRUE;
        }
        default:
            return FALSE;
        }
    }

    Hotkeys& hotkeys_;
    ShortcutBindings draft_;
    HWND dialog_ = nullptr;
    std::size_t recording_ = kShortcutCount;
    std::array<bool, std::size(kModifierKeys)> modifiers_{};
    Hotkey candidate_{};
    bool mainReleased_ = false;
};

}  // namespace

INT_PTR ShowShortcutDialog(HINSTANCE instance, HWND owner, Hotkeys& hotkeys) noexcept {
    SettingsDialog settings(hotkeys);
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_HOTKEYS), owner, SettingsDialog::Proc,
                           reinterpret_cast<LPARAM>(&settings));
}

}  // namespace picolet
