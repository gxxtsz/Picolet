# Picolet 截图工具

Picolet 是用 C++20 和 Win32 API 编写的 Windows 托盘截图工具，适用于 Windows 10 22H2 和 Windows 11 x64。运行 `Picolet.exe` 后，在任务栏通知区域找到 Picolet 图标。如果图标折叠了，点击通知区域的上箭头。

| 操作 | 默认快捷键 | 使用方法 |
| --- | --- | --- |
| 选区截图 | `Ctrl+Alt+Shift+S` | 拖拽框选，松开鼠标确认。单击后可以继续框选。按 `Esc` 或右键取消。 |
| 全屏截图 | `Ctrl+Alt+Shift+F` | 捕获所有显示器。屏幕之间未被显示器覆盖的部分填黑。 |
| 复制 | 预览中按 `Ctrl+C` | 将原尺寸图片复制到剪贴板，可粘贴到画图等应用。 |
| 另存为 PNG | 点击预览中的「另存为 PNG」 | 选择保存位置。默认文件名带本地时间，覆盖同名文件前会提示。 |

也可在托盘菜单选择截图方式。截图时会先收起上一次的预览，取消时恢复。截图省略鼠标指针，预览缩放不会改变导出尺寸。

在托盘菜单打开「快捷键设置」，为两项截图各选择一组修饰键和一个主键。保存后立即生效，并对当前 Windows 用户记忆。被其他应用占用的快捷键会在托盘菜单标为「未启用」，对应的菜单项仍可点击。关闭预览后工具继续驻留，使用托盘菜单的「退出」可结束程序。

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

## 实现与验证

捕获使用 GDI `BitBlt`，以物理像素把各屏幕放进同一张 BGRA 位图。选区从冻结帧裁剪，剪贴板使用 `CF_DIB`，PNG 由 WIC 编码后从同目录临时文件移入目标路径。快捷键设置保存在当前用户的 `HKEY_CURRENT_USER\Software\Picolet\Screenshot` 下。

自动测试用固定色块核对负坐标裁剪、PNG 的尺寸和像素、中文路径与同名覆盖。交互测试可在左右排列的双屏上从右向左拖拽跨屏，核对选区尺寸与预览，并检查取消恢复、画图粘贴及快捷键冲突。
