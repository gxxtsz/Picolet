# Picolet

**轻量、原生、随用随截的 Windows 截图工具。**

Picolet 面向日常截图、内容标注和桌面对照场景，提供区域截图、全屏截图、滚动长截图、标注、桌面贴图、剪贴板复制与 PNG 保存。启动后驻留系统托盘，通过快捷键或托盘图标即可开始截图。

## 轻量化设计

- **原生实现，依赖精简**：使用 C++20 和 Win32，直接调用 Windows 系统组件完成截图、绘图与图片编码。
- **单文件运行**：运行单个 `Picolet.exe` 即可使用，无需安装额外运行时。
- **托盘待命，操作直接**：平时驻留托盘，通过快捷键唤起截图，工具栏直接提供标注和输出操作。
- **资源按需使用**：截图或长图编辑结束后释放本次会话的截图缓冲和采集资源；已打开的贴图保留到关闭时释放。

## 运行要求

- Windows 10 / Windows 11，64 位（x64）。
- 普通用户权限即可运行。
- HDR 截图需要 Windows 10 2004 或更高版本，以及支持 Windows 图形采集的设备和驱动。

## 快速开始

1. 将 `Picolet.exe` 放到便于保留的位置，双击运行。程序图标会出现在系统托盘中；如果没有直接显示，可展开任务栏的隐藏图标区域。
2. 按 `F1`，或左键单击托盘图标开始截图。也可以右键单击托盘图标，选择“截图”。
3. 按住鼠标左键拖动，框选需要截取的区域。
4. 按需使用工具栏添加形状、箭头、画笔、文字或马赛克。
5. 选择一种输出方式：按 `Ctrl+C` 复制图片，按 `Ctrl+S` 保存为 PNG，或按 `Ctrl+T` 将图片贴在桌面上。

完成输出后，程序继续驻留托盘。取消本次截图可按 `Esc` 或点击工具栏的“取消”；退出程序请右键单击托盘图标，选择“退出”。

## 用户使用指南

### 选择与调整截图范围

- **区域截图**：进入截图界面后拖动框选，尺寸标签会显示选区的像素宽高，支持跨显示器框选。
- **调整选区**：选择工具栏的“调整选区”，在选区内部拖动可移动范围，拖动边缘或角落可调整大小。
- **重新框选**：点击“重选”重新选择范围，同时清除本次已有的标注。
- **全屏截图**：点击“全屏”或按 `Ctrl+A`，选中鼠标当前所在的显示器。

Picolet 每次截图时检查各屏幕的 HDR 状态。跨屏截图分别处理各屏幕，再按桌面布局拼接。HDR 画面会转换为便于分享的 8 位 sRGB 图片，并保留浅色与高光的层次。滚动长截图使用同样的颜色处理。系统可能在 HDR 采集期间显示提示边框。

### 添加标注

| 工具 | 使用方法 |
| --- | --- |
| 形状 | 选择矩形、椭圆或直线后拖动绘制；矩形和椭圆可选择描边或实心。 |
| 箭头 | 从起点向终点拖动，标记需要关注的位置。 |
| 画笔 | 按住鼠标左键自由绘制。 |
| 文字 | 在选区内点击并输入文字，按 `Ctrl+Enter` 提交；按 `Esc` 取消当前文字输入。 |
| 马赛克 | 在需要遮挡的区域拖动，添加马赛克。 |

可通过工具栏调整标注颜色、粗细或文字字号。按 `Ctrl+Z` 撤销上一项标注，按 `Ctrl+Y` 重做。输入文字时，先提交文字，再使用截图内的输出快捷键。

### 复制、保存与贴图

- **复制**：点击“完成并复制”或按 `Ctrl+C`，将截图复制到剪贴板，随后可粘贴到支持图片的聊天窗口、文档或图片编辑器。
- **保存**：点击“另存为 PNG”或按 `Ctrl+S`，选择文件名和保存位置。取消保存对话框后会返回截图编辑界面。
- **贴图**：点击“贴图”或按 `Ctrl+T`，将截图放到桌面上方，便于对照查看。

### 操作桌面贴图

- 按住鼠标左键拖动贴图，调整位置。
- 将鼠标放到贴图上滚动滚轮，放大或缩小图片。
- 右键单击贴图，关闭该贴图。

可以同时保留多张贴图；退出 Picolet 时会关闭全部贴图。

### 滚动长截图

1. 在需要截取内容的窗口中框选区域，点击“长截图”或按 `Ctrl+L`。
2. **手动滚动**：将鼠标放在选区内，使用滚轮或触摸板向下或向上滚动，程序连续采集并拼接内容。
3. **自动滚动**：在长截图面板选择“向下”或“向上”，点击“自动滚动”。需要停止时点击“暂停滚动”；鼠标移出选区或原窗口失去焦点时也会暂停。
4. 点击“完成”，进入长图编辑窗口。可以使用“适应宽度”或“100%”检查图片，继续添加标注。
5. 编辑完成后，复制图片、保存为 PNG 或创建贴图。

原窗口处于前台时，也可按空格键开始或暂停自动滚动，按 `Enter` 完成采集。采集阶段点击“取消”或使用取消快捷键（默认 `Esc`），会返回原截图，保留原选区和标注；进入长图编辑窗口后取消则结束本次截图。

使用长截图时请留意：

- 选区至少为 **32 × 64 像素**，并完整位于**同一显示器的同一窗口内**。
- 采集期间保持原窗口的位置和大小不变。
- 如果提示当前画面无法连接已截内容，缓慢反向滚回有重叠的位置，再继续滚动。
- 达到长图大小上限时，点击“完成”处理已保留的结果。

## 默认快捷键

| 操作 | 默认快捷键 | 生效范围 |
| --- | --- | --- |
| 开始截图 | `F1` | 全局，Picolet 运行时可用 |
| 全屏 | `Ctrl+A` | 普通截图；在长图编辑中为“适应宽度” |
| 复制图片 | `Ctrl+C` | 截图或长图编辑 |
| 另存为 PNG | `Ctrl+S` | 截图或长图编辑 |
| 桌面贴图 | `Ctrl+T` | 截图或长图编辑 |
| 撤销标注 | `Ctrl+Z` | 截图或长图编辑 |
| 重做标注 | `Ctrl+Y` | 截图或长图编辑 |
| 取消 | `Esc` | 截图、长截图采集或长图编辑 |
| 长截图 | `Ctrl+L` | 普通截图，需先选定区域 |

以上为默认绑定，修改后以设置界面显示的按键为准。输入文字时，`Esc` 只取消当前文字输入。

## 设置

右键单击托盘图标，选择“设置…”。

### 自定义快捷键

1. 点击要修改的快捷键按钮，进入录制状态。
2. 按下并释放所需的按键或组合键。
3. 点击“保存”使修改生效。

点击某项旁边的“清除”可以取消该项快捷键绑定，仍可用鼠标执行对应操作；点击“恢复快捷键默认”可以恢复默认绑定。清除和恢复操作同样需要点击“保存”才会生效。

“开始截图”是全局快捷键，其余快捷键在截图界面内使用。如果提示按键被占用、重复或由 Windows 保留，请更换按键后重新保存。

### 开机自启动

在“通用”中勾选“开机自启动”，点击“保存”。此设置作用于当前用户，登录后程序会驻留托盘；取消勾选并保存即可关闭自启动。

开启自启动后请保留程序所在位置。如果移动了 `Picolet.exe`，从新位置运行程序，打开设置并在保持勾选的情况下再次保存，以更新启动路径。

## 常见问题

### 启动后没有主窗口？

Picolet 启动后驻留系统托盘。展开任务栏的隐藏图标区域，找到 Picolet 图标，点击图标或按 `F1` 即可开始截图。重复启动时，程序会提示已在托盘运行。

### 按 F1 没有反应？

先通过托盘图标尝试截图，再打开“设置…”检查“开始截图”的绑定是否已清除或被其他程序占用。可以换一个组合键并保存，托盘入口也可以继续使用。

### 已开启自启动，登录后却没有运行？

确认 `Picolet.exe` 仍位于原来的位置，并检查 Windows 设置或任务管理器的“启动应用”中是否禁用了 Picolet。若已禁用，需要在 Windows 中重新启用；若移动过程序，按上面的步骤更新启动路径。

## 源码构建

构建环境必须为 Windows x64，源码使用 C++20。支持以下两套工具链：

- **MSVC** 需要 Visual Studio 2022 或对应生成工具、“使用 C++ 的桌面开发”工作负载，以及 Windows SDK 10.0.19041.0 或更高版本。VS2022 生成器需要 CMake 3.21 或更高版本。
- **MinGW-w64** 需要支持 C++20 并包含 Windows 图形采集头文件的 x64 工具链、Ninja 和 CMake 3.20 或更高版本。将 `g++`、`windres` 和 `ninja` 所在目录加入当前终端的 `PATH`。

### 使用统一预设

在项目根目录执行配置、构建和测试；三步使用同一预设名称：

```powershell
cmake --preset msvc-release
cmake --build --preset msvc-release --parallel
ctest --preset msvc-release
```

| 预设 | 工具链与配置 | 应用输出 |
| --- | --- | --- |
| `msvc-debug` | MSVC，Debug | `build/msvc-debug/Debug/Picolet.exe` |
| `msvc-release` | MSVC，Release | `build/msvc-release/Release/Picolet.exe` |
| `mingw-debug` | MinGW-w64，Debug | `build/mingw-debug/Picolet.exe` |
| `mingw-release` | MinGW-w64，Release | `build/mingw-release/Picolet.exe` |

四组预设均启用测试，并使用独立构建目录。测试预设串行执行，以免多个窗口测试争用桌面焦点。修改编译器或生成器时应使用新的构建目录。

应用仍可单文件运行；MSVC 使用静态 CRT，MinGW 静态链接自身运行库。无需复制构建得到的模块静态库，Windows 系统 DLL 由操作系统提供。

### 仅构建应用或指定模块

只构建应用，或只构建滚动模块及其依赖：

```powershell
cmake --build --preset msvc-release --target Picolet --parallel
cmake --build --preset mingw-debug --target picolet_scroll --parallel
```

构建命令使用真实目标名，如 `picolet_scroll`；`Picolet::scroll` 是 CMake 脚本中用于声明链接依赖的别名。

发布时如需完全关闭测试目标，可使用独立目录。原有手动构建入口仍然有效：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
cmake --build build --config Release --target Picolet
```

上述命令输出 `build/Release/Picolet.exe`。也可在配置预设时指定 `-DBUILD_TESTING=OFF`；该构建目录随后不应再运行测试预设。

### 个人工具路径

共享的 `CMakePresets.json` 不记录个人安装目录。临时覆盖 Ninja 路径可使用：

```powershell
cmake --preset mingw-debug -DCMAKE_MAKE_PROGRAM="C:/工具目录/ninja.exe"
```

长期使用的路径放入已被 Git 忽略的 `CMakeUserPresets.json`。例如，复制以下内容并替换工具路径：

```json
{
  "version": 2,
  "configurePresets": [
    {
      "name": "local-mingw-debug",
      "displayName": "本机 MinGW 调试版",
      "inherits": "mingw-debug",
      "cacheVariables": {
        "CMAKE_CXX_COMPILER": "C:/工具目录/mingw64/bin/g++.exe",
        "CMAKE_RC_COMPILER": "C:/工具目录/mingw64/bin/windres.exe",
        "CMAKE_MAKE_PROGRAM": "C:/工具目录/ninja.exe"
      }
    }
  ],
  "buildPresets": [
    { "name": "local-mingw-debug", "configurePreset": "local-mingw-debug" }
  ],
  "testPresets": [
    {
      "name": "local-mingw-debug",
      "inherits": "mingw-debug",
      "configurePreset": "local-mingw-debug"
    }
  ]
}
```

个人配置继承共享规则，输出到 `build/local-mingw-debug`。工具链自身需要的 DLL 目录仍须能被当前进程找到。

## 模块与依赖管理

项目采用单仓库、顶层统一配置、模块独立编译的组织方式，当前模块仅在仓库内部复用，不提供安装或外部包导出。

```text
Picolet/
├── CMakeLists.txt          # 项目版本、平台检查、模块注册和测试开关
├── CMakePresets.json       # 团队共享的配置、构建和测试入口
├── cmake/                 # 目标编译策略和 Unicode 入口配置
├── modules/
│   ├── win32/             # Win32 基础封装，仅含头文件
│   ├── image/             # 图像、屏幕采集和图片输出
│   ├── scroll/            # 滚动拼接、帧跟踪、采集流水线
│   ├── edit/              # 选区、标注和撤销重做
│   ├── system/            # 快捷键、自启动和设置提交
│   └── ui/                # 遮罩、编辑器、浮窗、工具栏和贴图
├── app/                   # 入口、托盘和功能协调
├── resources/             # 图标、资源编号、对话框和版本模板
└── tests/                 # 自动测试及交互夹具
```

各业务模块包含自己的 `CMakeLists.txt`、`include/picolet/模块名/` 和 `src/`。公开头文件统一使用 `<picolet/模块名/头文件名.h>` 引用；实现目录不向其他模块公开。Win32 基础模块只有公开头文件。

| 目标别名 | 公开模块依赖 | 职责 |
| --- | --- | --- |
| `Picolet::win32` | 无 | 句柄、COM、字体等基础封装，以及公共 Windows 编译定义 |
| `Picolet::image` | `win32` | 像素存储、桌面及区域采集、绘制、PNG 和剪贴板输出 |
| `Picolet::scroll` | `image`、`win32`、`Threads::Threads` | 拼接、帧跟踪、后台采集和诊断快照 |
| `Picolet::edit` | `image`、`scroll`、`win32` | 选区调整、标注历史、长图标注迁移 |
| `Picolet::system` | `win32` | 快捷键、自启动、设置提交和回滚 |
| `Picolet::ui` | `image`、`scroll`、`edit`、`system`、`win32` | 截图和长图界面、工具栏、贴图与设置窗口 |
| `Picolet::resources` | 无 | 资源编号和当前构建目录生成的版本头 |

`win32` 和 `resources` 是接口目标，其余业务模块是静态库。应用与测试链接这些库，生产界面不在各个测试中重复编译。公开类定义需要的视口、绘制缓冲、工具栏头文件属于 UI 接口；贴图阴影保留为内部实现，仅向现有白盒测试开放私有包含路径。

`edit` 使用滚动模块的 `SeedPlacement` 迁移标注，因此保留 `edit → scroll` 依赖。普通遮罩和长图编辑共同实现 `SelectionOverlay`，统一归属 UI 模块。模块化不改变这些类型、函数签名和线程约定。

### 新模块接入约定

1. 在 `modules` 下建立模块目录，把公开声明放入 `include/picolet/模块名/`，实现与内部头文件放入 `src/`，并为公开接口补充中文说明。
2. 用 `add_library(picolet_模块名 STATIC ...)` 显式列出源码，建立 `Picolet::模块名` 别名，并调用 `picolet_configure_target()`。仅有头文件的模块使用 `INTERFACE` 目标，无需调用该函数。
3. 通过 `target_include_directories()` 公开本模块的 `include` 目录；通过 `target_link_libraries()` 声明直接依赖。公开头文件需要的依赖使用 `PUBLIC`，实现细节使用 `PRIVATE`，仅提供给使用方的要求使用 `INTERFACE`。避免循环依赖和全局包含路径。
4. 在顶层按依赖顺序调用 `add_subdirectory()`；应用和测试通过链接目标使用模块，不复制生产源码列表。

公共编译策略位于 `cmake/PicoletOptions.cmake`：C++20、禁用语言扩展、私有警告选项及静态运行库保持一致。Windows 宏与内联基础函数需要的系统库由 `Picolet::win32` 传播。使用 `wmain` 或 `wWinMain` 的可执行目标另外调用 `picolet_use_unicode_entry()`，普通 `main` 测试不调用。

新增需要图标、清单或设置对话框的可执行目标时，调用 `picolet_add_resources()`。资源直接嵌入最终可执行文件，不能只放进静态库。该函数同时处理各目标目录中的资源依赖、MinGW 资源编码和 MSVC 清单选项。

项目版本只在顶层 `project(Picolet VERSION ...)` 修改。版本头与清单生成在当前构建目录的 `generated` 下，不写回源码，也不在不同工具链之间共享。

## 开发测试

完整构建预设后，运行同名测试预设即可执行现有 5 项自动测试：`picolet.image`、`picolet.scroll`、`picolet.fast_scroll`、`picolet.startup` 和 `picolet.paint`。部分测试覆盖多个模块，名称保持兼容。

```powershell
cmake --build --preset mingw-debug --parallel
ctest --preset mingw-debug
ctest --preset mingw-debug -R '^picolet\.fast_scroll$'
```

`picolet.startup` 使用独立注册表沙箱，并传入当前配置的应用路径验证重复启动；只构建 `picolet_startup_tests` 也会先构建应用。该测试保留 30 秒超时。

默认构建还提供以下交互夹具，运行时选择同一构建目录中的程序：

- `picolet_settings_fixture`：在隔离注册表中检查设置界面和保存流程。
- `picolet_scroll_fixture`：提供文档、聊天和网页样式的滚动内容。
- `picolet_capture_fixture`：配合滚动夹具验证实际采集、编辑、标注和贴图流程。

`picolet_native_scroll_probe` 不参加默认构建和 CTest；需要先显式构建，再配合滚动夹具运行：

```powershell
cmake --build --preset mingw-debug --target picolet_native_scroll_probe
```

原生探针会在其可执行文件旁的 `native-capture` 目录输出采集结果。修改构建、资源或模块接口后，应分别验证 MSVC 和 MinGW 的 Debug、Release 构建，并运行对应测试预设。

## 许可证

本项目采用 [MIT 许可证](LICENSE)，版权所有 © 2026 gxxtsz。

允许商业使用、修改和再分发。复制或分发本软件或其重要部分时，须保留版权声明和许可声明。本软件按“原样”提供，不提供任何明示或暗示担保；完整条款以许可证正文为准。
