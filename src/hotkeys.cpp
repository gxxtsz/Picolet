#include "hotkeys.h"

#include "win32.h"

#include <algorithm>
#include <cwchar>

namespace picolet {
namespace {

constexpr wchar_t kRegistryKey[] = L"Software\\Picolet\\Screenshot";
constexpr wchar_t kRegistryValue[] = L"Hotkeys";
constexpr UINT kModifierMask = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr HotkeyPair kDefaults{{{kModifierMask, 'S'}, {kModifierMask, 'F'}}};

bool IsValid(const Hotkey& hotkey) noexcept {
    const auto key = hotkey.key;
    const bool validKey = (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
                          (key >= VK_F1 && key <= VK_F11);
    return validKey && std::ranges::find(kModifiers, hotkey.modifiers) != kModifiers.end();
}

Hotkey Decode(WORD value) noexcept {
    return {HIBYTE(value), LOBYTE(value)};
}

WORD Encode(const Hotkey& hotkey) noexcept {
    return MAKEWORD(hotkey.key, hotkey.modifiers);
}

HotkeyPair LoadBindings() noexcept {
    DWORD data{};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, kRegistryValue, RRF_RT_REG_DWORD,
                     nullptr, &data, &size) == ERROR_SUCCESS && size == sizeof(data)) {
        const HotkeyPair saved{Decode(LOWORD(data)), Decode(HIWORD(data))};
        if (IsValid(saved[0]) && IsValid(saved[1]) && saved[0] != saved[1]) {
            return saved;
        }
    }
    return kDefaults;
}

HRESULT SaveBindings(const HotkeyPair& bindings) noexcept {
    HKEY rawKey = nullptr;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0,
                                           KEY_SET_VALUE, nullptr, &rawKey, nullptr);
    if (created != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(created);
    }
    const UniqueHandle<HKEY, RegCloseKey> key(rawKey);
    // 把两项一起写入一个值，避免进程在两次写入之间退出而留下半套设置。
    const DWORD packed = MAKELONG(Encode(bindings[0]), Encode(bindings[1]));
    return HRESULT_FROM_WIN32(RegSetValueExW(key.get(), kRegistryValue, 0, REG_DWORD,
                                            reinterpret_cast<const BYTE*>(&packed), sizeof(packed)));
}

}  // namespace

const wchar_t* ModifierText(UINT modifiers) noexcept {
    switch (modifiers) {
    case MOD_CONTROL | MOD_ALT: return L"Ctrl+Alt";
    case MOD_CONTROL | MOD_SHIFT: return L"Ctrl+Shift";
    case MOD_ALT | MOD_SHIFT: return L"Alt+Shift";
    default: return L"Ctrl+Alt+Shift";
    }
}

void FormatKey(UINT key, wchar_t (&text)[4]) noexcept {
    if (key >= VK_F1 && key <= VK_F11) {
        std::swprintf(text, 4, L"F%u", key - VK_F1 + 1);
    } else {
        text[0] = static_cast<wchar_t>(key);
        text[1] = L'\0';
    }
}

void Hotkeys::Start(HWND owner) noexcept {
    owner_ = owner;
    bindings_ = LoadBindings();
    for (std::size_t index = 0; index < bindings_.size(); ++index) {
        const auto& hotkey = bindings_[index];
        const int id = static_cast<int>(index) + 1;
        if (RegisterHotKey(owner_, id, hotkey.modifiers | MOD_NOREPEAT, hotkey.key)) {
            ids_[index] = id;
        }
    }
}

void Hotkeys::Stop() noexcept {
    for (const int id : ids_) {
        if (id != 0) {
            UnregisterHotKey(owner_, id);
        }
    }
    ids_.fill(0);
    owner_ = nullptr;
}

HRESULT Hotkeys::Apply(const HotkeyPair& requested, std::size_t& failedIndex) noexcept {
    failedIndex = requested.size();
    if (!IsValid(requested[0]) || !IsValid(requested[1]) || requested[0] == requested[1]) {
        return E_INVALIDARG;
    }

    std::array<int, 2> staged{};
    HRESULT status = S_OK;
    for (std::size_t index = 0; index < requested.size(); ++index) {
        const auto& hotkey = requested[index];
        for (std::size_t previous = 0; previous < bindings_.size(); ++previous) {
            if (hotkey == bindings_[previous]) {
                staged[index] = ids_[previous];
                break;
            }
        }
        if (staged[index] != 0) {
            continue;
        }

        // 旧组合至多两个，新组合至多两个。挑一个尚未使用的注册编号。
        int id = 1;
        while (std::ranges::find(ids_, id) != ids_.end() ||
               std::ranges::find(staged, id) != staged.end()) {
            ++id;
        }
        SetLastError(ERROR_SUCCESS);
        if (!RegisterHotKey(owner_, id, hotkey.modifiers | MOD_NOREPEAT, hotkey.key)) {
            failedIndex = index;
            status = LastErrorResult();
            break;
        }
        staged[index] = id;
    }
    if (SUCCEEDED(status)) {
        status = SaveBindings(requested);
    }
    if (FAILED(status)) {
        for (const int id : staged) {
            if (id != 0 && std::ranges::find(ids_, id) == ids_.end()) {
                UnregisterHotKey(owner_, id);
            }
        }
        return status;
    }

    for (const int id : ids_) {
        if (id != 0 && std::ranges::find(staged, id) == staged.end()) {
            UnregisterHotKey(owner_, id);
        }
    }
    bindings_ = requested;
    ids_ = staged;
    return S_OK;
}

std::optional<CaptureMode> Hotkeys::Match(WPARAM id, LPARAM keyData) const noexcept {
    const Hotkey received{LOWORD(keyData) & kModifierMask, HIWORD(keyData)};
    for (std::size_t index = 0; index < ids_.size(); ++index) {
        // 同时核对组合，丢弃设置切换前已进入队列、编号恰好被复用的消息。
        if (ids_[index] != 0 && static_cast<WPARAM>(ids_[index]) == id && received == bindings_[index]) {
            return static_cast<CaptureMode>(index);
        }
    }
    return std::nullopt;
}

}  // namespace picolet
