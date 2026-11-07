#pragma once

#include <windows.h>

#include <array>
#include <optional>
#include <span>

namespace picolet {

// Capture 属于全局作用域；其余命令只在截图遮罩内生效。新增命令追加到末尾，
// 使设置界面的控件编号和旧版本注册表中的命令索引保持一致。
enum class Shortcut : std::size_t { Capture, Fullscreen, Copy, Save, Pin, Undo, Cancel, Redo, LongCapture, Count };
constexpr auto ShortcutIndex(Shortcut command) noexcept { return static_cast<std::size_t>(command); }
inline constexpr auto kShortcutCount = ShortcutIndex(Shortcut::Count);
inline constexpr UINT kShortcutModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

// 用 Win32 虚拟键表示主键，修饰键不区分左右。两个字段均为零表示已清除绑定。
struct Hotkey {
    UINT modifiers = 0;
    UINT key = 0;
    bool operator==(const Hotkey&) const noexcept = default;
};

using ShortcutBindings = std::array<Hotkey, kShortcutCount>;
using ShortcutWords = std::array<WORD, kShortcutCount + 1>;

enum class ShortcutError { None, InvalidKey, Duplicate, Reserved, Occupied, Registration, Storage };

// 错误针对一条命令时 slot 指出该条目，设置界面可就地显示原因；存储错误针对整组设置。
struct ShortcutResult {
    ShortcutError error = ShortcutError::None;
    Shortcut slot = Shortcut::Capture;
};

const wchar_t* ShortcutName(Shortcut command) noexcept;
ShortcutBindings DefaultShortcuts() noexcept;
bool IsMainKey(UINT key) noexcept;
UINT CurrentModifiers() noexcept;

// 由操作系统取得本地键盘布局的主键名称。没有绑定时显示「未设置」，输出总是以零结尾。
void FormatHotkey(Hotkey hotkey, wchar_t (&label)[96]) noexcept;

// 局部命令之间不能重复；全局键可以与任一局部键相同。未绑定的命令不参与查重。
ShortcutResult ValidateBindings(const ShortcutBindings& bindings) noexcept;
std::optional<Shortcut> MatchLocalShortcut(const ShortcutBindings& bindings, Hotkey received) noexcept;

// 首字为格式版本，其余为按命令顺序排列的键。第 1 版沿用原有绑定，
// 仅在截图内没有其他命令使用 Ctrl+Y 时为重做补上默认键。第 2 版升级时同样按冲突情况
// 为长截图补上 Ctrl+L；显式清除的已有命令保持为空，第 3 版完整保存全部命令。
// 格式无效时尝试旧 DWORD 的低字，只迁移旧选区键，其余命令使用默认值。
ShortcutWords EncodeBindings(const ShortcutBindings& bindings) noexcept;
ShortcutBindings DecodeBindings(std::span<const WORD> words, std::optional<DWORD> legacy) noexcept;

// 只有 Capture 通过 Windows 注册为系统热键。窗口和所有方法必须属于同一消息线程。
class Hotkeys {
public:
    // 从 HKCU 读取设置并尝试注册。即使系统拒绝注册，bindings() 仍返回用户保存的配置。
    void Start(HWND owner) noexcept;
    void Stop() noexcept;

    // 设置和截图期间释放全局键，让窗口中的输入控件及局部快捷键正常接收键盘消息。
    // Resume 失败只影响全局触发，托盘仍可打开截图和设置。
    void Pause() noexcept;
    ShortcutResult Resume() noexcept;

    // 验证全组配置，先尝试注册新全局键，再将整组写为一个值。失败不会改变已生效的配置。
    ShortcutResult Apply(const ShortcutBindings& requested) noexcept;

    const ShortcutBindings& bindings() const noexcept { return bindings_; }
    bool enabled() const noexcept { return activeId_ != 0; }

    // 同时验证注册编号及消息携带的按键，忽略来自已暂停或已替换注册的旧消息。
    bool Match(WPARAM id, LPARAM keyData) const noexcept;

private:
    ShortcutResult Register(Hotkey binding, int& id) noexcept;

    HWND owner_ = nullptr;
    ShortcutBindings bindings_{};
    int activeId_ = 0;
    int nextId_ = 1;
};

}  // namespace picolet
