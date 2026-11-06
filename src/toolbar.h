#pragma once

#include "capture_edit.h"
#include "hotkeys.h"
#include "win32.h"

#include <commctrl.h>

#include <array>
#include <optional>

namespace picolet {

// 控件顺序即 Tab 顺序。主栏、参数栏和色板中的按钮都由同一个遮罩窗口接收命令。
enum class ToolbarButton : std::size_t {
    Select, Reselect, Fullscreen, Shapes, Arrow, Pen, Text, Mosaic, Undo, Redo, Pin, Save, Cancel, Copy,
    Rectangle, Ellipse, Line, Outline, Solid, Size, Color, Red, Yellow, Green, Blue, White, Black, Count
};

inline constexpr std::array<COLORREF, 6> kToolbarColors{
    RGB(239, 68, 68), RGB(250, 204, 21), RGB(74, 222, 128),
    RGB(59, 130, 246), RGB(255, 255, 255), RGB(15, 23, 42)
};

// 线宽和字号使用导出图片的物理像素；工具栏本身的缩放不会改变这些值。
struct ToolbarStyle {
    static constexpr int kMinWidth = 1;
    static constexpr int kMaxWidth = 24;
    static constexpr int kMinTextSize = 12;
    static constexpr int kMaxTextSize = 72;

    int width = 4;
    int textSize = 24;
    std::size_t color = 0;
    bool filled = false;
};

// 布局所需的会话状态。由遮罩管理选区、历史和标注草稿，工具栏只负责显示与输入。
struct ToolbarView {
    EditTool tool = EditTool::Select;
    ToolbarStyle style;
    bool canUndo = false;
    bool canRedo = false;
    bool paletteOpen = false;
};

// 截图遮罩内的原生子控件与自绘界面。矩形、鼠标坐标和绘图 DC 均使用遮罩客户区的物理像素。
// 必须在所属 UI 线程上使用，Create 后由遮罩转发 WM_DRAWITEM、WM_NOTIFY 和 WM_HSCROLL。
class CaptureToolbar {
public:
    // 创建隐藏控件和提示窗。快捷键显示文本会复制到本对象，不依赖调用方的存储期。
    bool Create(HWND parent, HINSTANCE instance, const ShortcutBindings& shortcuts) noexcept;
    void Close() noexcept;

    // 在工作区内布置两级工具栏和独立标签。空选区时仅显示全屏、取消和开始提示。
    // status 是可选的即时提示；本方法会复制文本。Hide 在拖动或打开系统对话框时隐藏所有控件。
    void Layout(const RECT& work, const RECT& selection, UINT dpi, const ToolbarView& view,
                const wchar_t* status) noexcept;
    void Hide() noexcept;
    bool Contains(POINT client) const noexcept;

    // 判断消息是否由本工具栏的控件发出。滑块值由原生控件处理，SetValue 仅同步数值标签，
    // 使拖动期间不会重排参数栏。
    std::optional<ToolbarButton> Clicked(WPARAM command, LPARAM sender) const noexcept;
    bool IsButton(HWND window) const noexcept;
    bool IsSlider(HWND window) const noexcept;
    int SliderValue() const noexcept;
    void SetValue(int value) noexcept;

    // 绘制底板、标签和各控件。返回空值表示消息应交给窗口的默认处理。
    void Paint(HDC dc) const noexcept;
    bool DrawButton(const DRAWITEMSTRUCT& item) const noexcept;
    std::optional<LRESULT> DrawSlider(const NMHDR& item) const noexcept;

private:
    static constexpr auto kCount = static_cast<std::size_t>(ToolbarButton::Count);
    static LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR data) noexcept;
    void InvalidateSurfaces() const noexcept;

    HWND parent_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND hovered_ = nullptr;
    std::array<HWND, kCount> controls_{};
    std::array<std::array<wchar_t, 128>, kCount> tooltipText_{};
    FontHandle font_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    ToolbarView view_;
    RECT main_{}, options_{}, palette_{}, dimension_{}, status_{}, sizeLabel_{}, valueLabel_{};
    std::array<RECT, 5> separators_{};
    wchar_t hintText_[128]{};
    wchar_t dimensionText_[128]{};
    wchar_t statusText_[128]{};
    int value_ = 4;
};

}  // namespace picolet
