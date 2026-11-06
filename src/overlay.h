#pragma once

#include "capture_edit.h"
#include "hotkeys.h"
#include "win32.h"

#include <array>

namespace picolet {

inline constexpr UINT kOverlayAction = WM_APP + 2;
enum class OverlayAction : WPARAM { Cancel, Copy, Save, Pin };

// 置顶遮罩和选区工具栏。调用方保有冻结帧，在关闭遮罩之前不能修改或释放它。
// 选区和标注使用桌面物理坐标；窗口布局单独按工具栏所在屏幕的 DPI 缩放。
class SelectionOverlay {
public:
    static bool Register(HINSTANCE instance, HICON icon) noexcept;

    // 在冻结帧上开始一次会话，并复制本次会话的局部快捷键。anchor 是初始鼠标位置。
    HRESULT Open(HINSTANCE instance, HWND owner, const DesktopFrame& frame, POINT anchor,
                 const ShortcutBindings& shortcuts) noexcept;

    // 从应用消息循环转发属于遮罩或工具栏的键盘消息。返回 true 时无需再分发这条消息。
    // 编辑文字时保留原生输入控件行为，不将输入解释成截图命令。
    bool HandleKey(const MSG& message) noexcept;

    // 取消会话；若正显示系统保存对话框，先标记会话失效，由控制窗口在对话框返回后关闭。
    void Cancel() noexcept;
    void Close() noexcept;

    // 输出期间锁定选区及标注，允许应用把系统保存对话框设为遮罩的子窗口。
    // 取消保存或输出失败后调用 Retry，恢复界面并保留编辑内容。
    void BeginOutput() noexcept;
    void Retry(const wchar_t* status) noexcept;
    HRESULT Render(Image& result) const noexcept;

    bool invalidated() const noexcept { return invalidated_; }
    HWND window() const noexcept { return window_; }
    const RECT& selection() const noexcept { return selection_; }

private:
    enum Button : std::size_t {
        BSelect, BRectangle, BEllipse, BArrow, BPen, BText, BMosaic,
        BRed, BYellow, BGreen, BBlue, BWhite, BBlack, BSize,
        BReselect, BFullscreen, BUndo, BCopy, BSave, BPin, BCancel, BCount
    };
    struct ButtonSpec { const wchar_t* label; int width; };
    static const std::array<ButtonSpec, BCount> kButtons;
    enum class Drag { None, Create, Move, Resize, Mark };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    static LRESULT CALLBACK TextProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR id, DWORD_PTR data) noexcept;
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept;

    bool CreateButtons(HINSTANCE instance) noexcept;
    void Command(Shortcut shortcut) noexcept;
    void PressButton(Button button) noexcept;
    void PostAction(OverlayAction action) noexcept;
    void SelectFullscreen() noexcept;
    void BeginDrag(POINT point) noexcept;
    void UpdateDrag(POINT point) noexcept;
    void EndDrag(POINT point) noexcept;
    void AbortDrag() noexcept;
    unsigned HitEdges(POINT point) const noexcept;
    Annotation MakeMark(POINT point) const noexcept;
    void BeginText(POINT point) noexcept;
    bool FinishText(bool commit) noexcept;

    RECT ToClient(RECT rect) const noexcept;
    POINT MessagePoint() const noexcept;
    void LayoutPanel() noexcept;
    void HidePanel() noexcept;
    void Status(const wchar_t* text) noexcept;
    void RepaintSelection(const RECT& previous) noexcept;
    void Paint() noexcept;
    void PaintButton(const DRAWITEMSTRUCT& item) const noexcept;

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HWND textEdit_ = nullptr;
    std::array<HWND, BCount> buttons_{};
    const DesktopFrame* frame_ = nullptr;
    ShortcutBindings shortcuts_{};
    Image dark_;
    AnnotationHistory history_;
    Annotation draft_;
    FontHandle font_;
    FontHandle textFont_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    RECT panel_{};
    RECT selection_{};
    RECT original_{};
    POINT anchor_{};
    POINT start_{};
    unsigned edges_ = EdgeNone;
    EditTool tool_ = EditTool::Select;
    Drag drag_ = Drag::None;
    std::size_t color_ = 0;
    std::size_t strokeSize_ = 1;
    std::size_t textSize_ = 1;
    const wchar_t* status_ = nullptr;
    bool activated_ = false;
    bool invalidated_ = false;
    bool pending_ = false;
    bool busy_ = false;
    bool closing_ = false;
    bool composingText_ = false;
};

}  // namespace picolet
