#pragma once

#include <windows.h>

#include <array>
#include <optional>

namespace picolet {

enum class CaptureMode : std::size_t { Area, Desktop };

struct Hotkey {
    UINT modifiers = 0;
    UINT key = 0;
    bool operator==(const Hotkey&) const noexcept = default;
};

using HotkeyPair = std::array<Hotkey, 2>;

inline constexpr std::array<UINT, 4> kModifiers{
    MOD_CONTROL | MOD_ALT,
    MOD_CONTROL | MOD_SHIFT,
    MOD_ALT | MOD_SHIFT,
    MOD_CONTROL | MOD_ALT | MOD_SHIFT,
};

const wchar_t* ModifierText(UINT modifiers) noexcept;
void FormatKey(UINT key, wchar_t (&text)[4]) noexcept;

// 注册由托盘窗口接收。编号与组合绑定，因此交换两项快捷键时可以复用原注册。
class Hotkeys {
public:
    // 加载当前用户的设置。某一项注册失败时继续启用另一项，并保留菜单入口。
    void Start(HWND owner) noexcept;
    void Stop() noexcept;

    // 在保留旧注册的同时尝试新组合。全部注册且设置写入成功后才切换。
    // failedIndex 小于 2 时指出占用的那一项，否则表示校验或存储失败。
    HRESULT Apply(const HotkeyPair& requested, std::size_t& failedIndex) noexcept;

    const HotkeyPair& bindings() const noexcept { return bindings_; }
    bool enabled(std::size_t index) const noexcept { return ids_[index] != 0; }
    std::optional<CaptureMode> Match(WPARAM id, LPARAM keyData) const noexcept;

private:
    HWND owner_ = nullptr;
    HotkeyPair bindings_{};
    std::array<int, 2> ids_{};
};

}  // namespace picolet
