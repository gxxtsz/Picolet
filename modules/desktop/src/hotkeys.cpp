#include <picolet/desktop/hotkeys.h>

#include <picolet/win32.h>

#include <cwchar>

namespace picolet {
namespace {

constexpr wchar_t kRegistryKey[] = L"Software\\Picolet\\Screenshot";
constexpr wchar_t kRegistryValue[] = L"Shortcuts";
constexpr wchar_t kLegacyValue[] = L"Hotkeys";
constexpr WORD kBindingVersion = 3;
constexpr auto kV1Count = ShortcutIndex(Shortcut::Redo);
constexpr auto kV2Count = ShortcutIndex(Shortcut::LongCapture);
constexpr ShortcutBindings kDefaults{{{0, VK_F1}, {MOD_CONTROL, 'A'}, {MOD_CONTROL, 'C'},
    {MOD_CONTROL, 'S'}, {MOD_CONTROL, 'T'}, {MOD_CONTROL, 'Z'}, {0, VK_ESCAPE}, {MOD_CONTROL, 'Y'}, {MOD_CONTROL, 'L'}}};

Hotkey Decode(WORD word) noexcept { return {HIBYTE(word), LOBYTE(word)}; }

bool IsLegacyArea(Hotkey binding) noexcept {
    const auto mods = binding.modifiers;
    const auto key = binding.key;
    return (mods == (MOD_CONTROL | MOD_ALT) || mods == (MOD_CONTROL | MOD_SHIFT) ||
            mods == (MOD_ALT | MOD_SHIFT) || mods == (MOD_CONTROL | MOD_ALT | MOD_SHIFT)) &&
           ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || (key >= VK_F1 && key <= VK_F11));
}

ShortcutBindings LoadBindings() noexcept {
    ShortcutWords words{};
    DWORD size = static_cast<DWORD>(sizeof(words));
    const bool hasWords = RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryValue, RRF_RT_REG_BINARY,
                                       nullptr, words.data(), &size) == ERROR_SUCCESS &&
                          size <= sizeof(words) && size % sizeof(WORD) == 0;
    const auto sizeWords = size / sizeof(WORD);
    DWORD legacy = 0;
    size = sizeof(legacy);
    const bool hasLegacy = RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, kLegacyValue, RRF_RT_REG_DWORD,
                                       nullptr, &legacy, &size) == ERROR_SUCCESS && size == sizeof(legacy);
    return DecodeBindings(hasWords ? std::span<const WORD>(words.data(), sizeWords) : std::span<const WORD>{},
                          hasLegacy ? std::optional<DWORD>(legacy) : std::nullopt);
}

bool SaveBindings(const ShortcutBindings& bindings) noexcept {
    HKEY raw = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &raw, nullptr)
        != ERROR_SUCCESS) return false;
    const UniqueHandle<HKEY, RegCloseKey> key(raw);
    const auto words = EncodeBindings(bindings);
    return RegSetValueExW(key.get(), kRegistryValue, 0, REG_BINARY, reinterpret_cast<const BYTE*>(words.data()),
                          static_cast<DWORD>(sizeof(words))) == ERROR_SUCCESS;
}

}  // namespace

const wchar_t* ShortcutName(Shortcut command) noexcept {
    constexpr const wchar_t* names[]{L"开始截图", L"全屏", L"复制", L"另存为 PNG", L"贴图", L"撤销", L"取消", L"重做", L"长截图"};
    const auto index = ShortcutIndex(command);
    return index < kShortcutCount ? names[index] : L"";
}

ShortcutBindings DefaultShortcuts() noexcept { return kDefaults; }

bool IsMainKey(UINT key) noexcept {
    if ((key < VK_BACK && key != VK_CANCEL) || key >= 0xff) return false;
    switch (key) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_PACKET: case VK_PROCESSKEY:
        return false;
    default:
        return true;
    }
}

UINT CurrentModifiers() noexcept {
    return ((GetKeyState(VK_CONTROL) < 0) ? MOD_CONTROL : 0) |
           ((GetKeyState(VK_MENU) < 0) ? MOD_ALT : 0) |
           ((GetKeyState(VK_SHIFT) < 0) ? MOD_SHIFT : 0) |
           ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
}

void FormatHotkey(Hotkey hotkey, wchar_t (&label)[96]) noexcept {
    if (!hotkey.key) {
        lstrcpynW(label, L"未设置", 96);
        return;
    }
    wchar_t key[64]{};
    if ((hotkey.key >= 'A' && hotkey.key <= 'Z') || (hotkey.key >= '0' && hotkey.key <= '9')) {
        key[0] = static_cast<wchar_t>(hotkey.key);
    } else if (hotkey.key >= VK_F1 && hotkey.key <= VK_F24) {
        std::swprintf(key, std::size(key), L"F%u", hotkey.key - VK_F1 + 1);
    } else if (hotkey.key >= VK_NUMPAD0 && hotkey.key <= VK_NUMPAD9) {
        std::swprintf(key, std::size(key), L"小键盘 %u", hotkey.key - VK_NUMPAD0);
    } else {
        const wchar_t* name = nullptr;
        switch (hotkey.key) {
        case VK_ESCAPE: name = L"Esc"; break;
        case VK_RETURN: name = L"Enter"; break;
        case VK_SNAPSHOT: name = L"Print Screen"; break;
        case VK_SPACE: name = L"空格"; break;
        case VK_BACK: name = L"退格"; break;
        case VK_DELETE: name = L"Delete"; break;
        default: break;
        }
        if (name) {
            lstrcpynW(key, name, static_cast<int>(std::size(key)));
        } else {
            const UINT scan = MapVirtualKeyW(hotkey.key, MAPVK_VK_TO_VSC_EX);
            const LONG param = static_cast<LONG>(((scan & 0xff) << 16) | (scan & 0xff00 ? (1u << 24) : 0));
            if (!scan || !GetKeyNameTextW(param, key, static_cast<int>(std::size(key)))) {
                std::swprintf(key, std::size(key), L"按键 0x%02X", hotkey.key);
            }
        }
    }
    std::swprintf(label, std::size(label), L"%ls%ls%ls%ls%ls",
                  (hotkey.modifiers & MOD_CONTROL) ? L"Ctrl+" : L"",
                  (hotkey.modifiers & MOD_ALT) ? L"Alt+" : L"",
                  (hotkey.modifiers & MOD_SHIFT) ? L"Shift+" : L"",
                  (hotkey.modifiers & MOD_WIN) ? L"Win+" : L"", key);
}

ShortcutResult ValidateBindings(const ShortcutBindings& bindings) noexcept {
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const Hotkey binding = bindings[i];
        const auto command = static_cast<Shortcut>(i);
        if (binding.key ? (!IsMainKey(binding.key) || (binding.modifiers & ~kShortcutModifiers)) : binding.modifiers) {
            return {ShortcutError::InvalidKey, command};
        }
        if (command == Shortcut::Capture && binding.key == VK_F12) return {ShortcutError::Reserved, command};
        if (!binding.key) continue;
        for (std::size_t previous = 1; previous < i; ++previous) {
            if (binding == bindings[previous]) return {ShortcutError::Duplicate, command};
        }
    }
    return {};
}

std::optional<Shortcut> MatchLocalShortcut(const ShortcutBindings& bindings, Hotkey received) noexcept {
    if (received.key) {
        for (std::size_t i = 1; i < bindings.size(); ++i) {
            if (bindings[i] == received) return static_cast<Shortcut>(i);
        }
    }
    return std::nullopt;
}

ShortcutWords EncodeBindings(const ShortcutBindings& bindings) noexcept {
    ShortcutWords words{};
    words[0] = kBindingVersion;
    for (std::size_t i = 0; i < bindings.size(); ++i) words[i + 1] = MAKEWORD(bindings[i].key, bindings[i].modifiers);
    return words;
}

ShortcutBindings DecodeBindings(std::span<const WORD> words, std::optional<DWORD> legacy) noexcept {
    if (words.size() == kShortcutCount + 1 && words.front() == kBindingVersion) {
        ShortcutBindings bindings{};
        for (std::size_t i = 0; i < bindings.size(); ++i) bindings[i] = Decode(words[i + 1]);
        if (ValidateBindings(bindings).error == ShortcutError::None) return bindings;
    }
    const bool version1 = words.size() == kV1Count + 1 && words.front() == 1;
    const bool version2 = words.size() == kV2Count + 1 && words.front() == 2;
    if (version1 || version2) {
        ShortcutBindings bindings{};
        const auto count = version1 ? kV1Count : kV2Count;
        for (std::size_t i = 0; i < count; ++i) bindings[i] = Decode(words[i + 1]);
        if (ValidateBindings(bindings).error == ShortcutError::None) {
            if (version1) {
                const Hotkey redo = kDefaults[ShortcutIndex(Shortcut::Redo)];
                if (!MatchLocalShortcut(bindings, redo)) bindings[ShortcutIndex(Shortcut::Redo)] = redo;
            }
            const Hotkey scroll = kDefaults[ShortcutIndex(Shortcut::LongCapture)];
            if (!MatchLocalShortcut(bindings, scroll)) bindings[ShortcutIndex(Shortcut::LongCapture)] = scroll;
            return bindings;
        }
    }
    ShortcutBindings bindings = kDefaults;
    if (legacy) {
        const auto area = Decode(LOWORD(*legacy));
        if (IsLegacyArea(area)) bindings[0] = area;
    }
    return bindings;
}

void Hotkeys::Start(HWND owner) noexcept {
    owner_ = owner;
    bindings_ = LoadBindings();
    Resume();
}

void Hotkeys::Stop() noexcept {
    Pause();
    owner_ = nullptr;
}

void Hotkeys::Pause() noexcept {
    if (activeId_) UnregisterHotKey(owner_, std::exchange(activeId_, 0));
}

ShortcutResult Hotkeys::Resume() noexcept {
    return activeId_ ? ShortcutResult{} : Register(bindings_[0], activeId_);
}

ShortcutResult Hotkeys::Register(Hotkey binding, int& id) noexcept {
    id = 0;
    if (!binding.key) return {};
    if (!owner_) return {ShortcutError::Registration};
    if (binding.key == VK_F12) return {ShortcutError::Reserved};
    if (nextId_ > 0xbfff) nextId_ = 1;
    if (nextId_ == activeId_) ++nextId_;
    if (nextId_ > 0xbfff) nextId_ = 1;
    const int candidate = nextId_++;
    SetLastError(ERROR_SUCCESS);
    if (!RegisterHotKey(owner_, candidate, binding.modifiers | MOD_NOREPEAT, binding.key)) {
        return {GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED ? ShortcutError::Occupied : ShortcutError::Registration};
    }
    id = candidate;
    return {};
}

ShortcutResult Hotkeys::Apply(const ShortcutBindings& requested) noexcept {
    const auto valid = ValidateBindings(requested);
    if (valid.error != ShortcutError::None) return valid;
    int staged = activeId_;
    if (!staged || requested[0] != bindings_[0]) {
        const auto result = Register(requested[0], staged);
        if (result.error != ShortcutError::None) return result;
    }
    if (!SaveBindings(requested)) {
        if (staged && staged != activeId_) UnregisterHotKey(owner_, staged);
        return {ShortcutError::Storage};
    }
    if (activeId_ != staged) Pause();
    activeId_ = staged;
    bindings_ = requested;
    return {};
}

bool Hotkeys::Match(WPARAM id, LPARAM keyData) const noexcept {
    const Hotkey received{LOWORD(keyData) & kShortcutModifiers, HIWORD(keyData)};
    return activeId_ && id == static_cast<WPARAM>(activeId_) && received == bindings_[0];
}

}  // namespace picolet
