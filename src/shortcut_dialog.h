#pragma once

#include "hotkeys.h"

namespace picolet {

// 打开快捷键设置对话框。调用前暂停全局键，返回后由调用方恢复注册。
// 只有点击保存且全组注册、写入成功才更新 hotkeys；取消会丢弃所有未保存的修改。
// 返回 -1 表示无法打开对话框，其余值为结束对话框的控件编号。
INT_PTR ShowShortcutDialog(HINSTANCE instance, HWND owner, Hotkeys& hotkeys) noexcept;

}  // namespace picolet
