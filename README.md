# Picolet 截图工具

Picolet 是用 C++20 和 Win32 API 编写的 Windows 托盘截图工具，适用于 Windows 10 22H2 和 Windows 11 x64。运行 `Picolet.exe` 后，在任务栏通知区域找到 Picolet 图标。如果图标折叠了，点击通知区域的上箭头。

单击托盘图标或按 `F1` 开始截图。拖拽框选，松开后可以拖动选区内部，或拉动边和角调整。点「全屏」选中鼠标所在的显示器，包含任务栏；手动框选仍可跨屏。点「重选」可清空选区和标注，在同一冻结画面上重新框选。

| 操作 | 默认快捷键 | 结果 |
| --- | --- | --- |
| 开始截图 | `F1`（全局） | 冻结桌面并打开截图界面。 |
| 全屏 | `Ctrl+A` | 选中鼠标所在的显示器。 |
| 复制 | `Ctrl+C` | 将原尺寸截图复制到剪贴板。 |
| 另存为 PNG | `Ctrl+S` | 选择保存位置；覆盖同名文件前会提示。 |
| 贴图 | `Ctrl+T` | 将截图作为独立图片窗口置顶显示。 |
| 撤销 | `Ctrl+Z` | 移除最后一笔标注。 |
| 取消 | `Esc` 或鼠标右键 | 关闭本次截图。 |

在选区旁的工具栏可画矩形、椭圆、箭头、画笔和马赛克，也可输入文字。色块选择颜色，点击线宽或字号按钮切换大小；文字输入时按 `Ctrl+Enter` 提交，按 `Esc` 放弃本次文字。点「选区」返回移动或调整大小。标注留在原画面的位置，调整选区只改变导出的范围。

复制、保存或贴图成功后结束本次截图；取消保存可回到原来的选区继续编辑。截图不包含鼠标、边框或工具栏。贴图可同时打开多张，左键拖动，滚轮缩放，右键关闭。已有贴图如果显示在新选区内，会和其他桌面内容一起被截取。托盘菜单的「退出」会关闭程序和所有贴图。

右键托盘图标打开「快捷键设置」。点击对应按键条目直接录入，支持单键及 `Ctrl`、`Alt`、`Shift`、`Win` 组合，可清除或恢复默认。同一组不能重复，全局键可与截图内键相同。保存后立即生效。系统保留或已被占用的全局键无法注册，托盘仍能开始截图；系统拦截的按键也可能无法用于截图内命令。升级后，已保存过的旧选区键会成为统一截图键，旧全屏键停止使用。

## 构建

需要 CMake 3.20 或以上，以及支持 C++20 的 Windows x64 工具链。下面的命令在 PowerShell 中运行。

使用 Visual Studio 2022 的 C++ 生成工具和 Windows SDK。

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
ctest --test-dir build-msvc -C Release --output-on-failure
```

可执行文件位于 `build-msvc\Release\Picolet.exe`。

使用 MinGW-w64 时，确保 `g++`、`windres` 和 `mingw32-make` 都在 `PATH` 中。

```powershell
cmake -S . -B build-mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
ctest --test-dir build-mingw --output-on-failure
```

可执行文件位于 `build-mingw\Picolet.exe`。两种构建的可执行文件都只依赖 Windows 自带的 DLL，可以直接运行。

如果使用 CLion 2026.2 自带的 MinGW，遇到默认 C 运行库的静态链接错误，可在 CMake 配置中加上 `-DCMAKE_CXX_FLAGS=-mcrtdll=ucrtbase`。

## 实现与验证

捕获使用 GDI `BitBlt`，以物理像素把各屏幕放进同一张 BGRA 位图，屏幕间空隙填黑。标注按提交顺序合成，遮罩和输出从同一份标注像素裁剪。剪贴板使用 `CF_DIB`，PNG 由 WIC 编码后从同目录临时文件移入目标路径。快捷键设置作为整组保存在当前用户的 `HKEY_CURRENT_USER\Software\Picolet\Screenshot` 下。

自动测试覆盖负坐标裁剪和调整、标注合成和马赛克撤销、快捷键迁移和冲突、PNG 像素、中文路径与同名覆盖。交互验证可在缩放比例不同的双屏上测试单屏全选、跨屏框选和工具栏位置，并检查画图粘贴、取消保存后重试、贴图的移动和缩放以及快捷键录入。
