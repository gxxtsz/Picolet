#pragma once

#include "hotkeys.h"
#include "startup.h"

namespace picolet {

// 打开通用及快捷键设置对话框。调用前暂停全局键，返回后由调用方恢复注册。
// 只有点击保存才写入；快捷键提交失败时恢复原启动项，恢复失败会显示实际状态并保持对话框。
// 取消会丢弃未保存草稿。registration 及其借用根键必须活到模态对话框返回；测试可传隔离注册表。
// 返回 -1 表示无法打开对话框，其余值为结束对话框的控件编号。
INT_PTR ShowSettingsDialog(HINSTANCE instance, HWND owner, Hotkeys& hotkeys,
                          const StartupRegistration& registration = StartupRegistration{}) noexcept;

}  // namespace picolet
