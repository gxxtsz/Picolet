#pragma once

#include <picolet/system/hotkeys.h>
#include <picolet/system/startup.h>

namespace picolet {

// 独立保留启动项写入、快捷键提交及启动项恢复的结果，界面不可把回滚失败误报为“原设置已保留”。
struct SettingsCommitResult {
    HRESULT startup = S_OK;
    ShortcutResult shortcut;
    HRESULT rollback = S_OK;
    bool succeeded() const noexcept {
        return SUCCEEDED(startup) && shortcut.error == ShortcutError::None && SUCCEEDED(rollback);
    }
};

// 同一界面线程内协调两类设置：先验证整组快捷键，再改启动项，最后提交变更的快捷键。
// enabled 为空表示启动项未变、读取失败或不可编辑，必须跳过该设置；executable 仅在开启时使用。
// applyShortcuts 必须符合 Hotkeys::Apply 的约定：失败不改变已保存快捷键，且不抛异常。
// 启动项真正改变后才需要回滚；原始类型、原始字节及不存在状态全部恢复。回滚错误单独交付界面。
// 使用回调把快捷键原有提交逻辑保留在其所有者内，也让测试可以模拟写入失败而不修改真实快捷键。
template <typename ApplyShortcuts>
SettingsCommitResult CommitSettings(const StartupRegistration& registration, std::optional<bool> enabled,
                                    std::wstring_view executable, const ShortcutBindings& current,
                                    const ShortcutBindings& requested, ApplyShortcuts&& applyShortcuts) noexcept {
    SettingsCommitResult result;
    result.shortcut = ValidateBindings(requested);
    if (result.shortcut.error != ShortcutError::None) return result;
    StartupValue before;
    if (enabled) {
        result.startup = registration.SetEnabled(*enabled, executable, &before);
        if (FAILED(result.startup)) return result;
    }
    if (requested != current) result.shortcut = applyShortcuts(requested);
    if (result.shortcut.error != ShortcutError::None && enabled && result.startup == S_OK)
        result.rollback = registration.Restore(before);
    return result;
}

}  // namespace picolet
