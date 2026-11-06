#pragma once

#include "capture_edit.h"
#include "hotkeys.h"
#include "toolbar.h"
#include "win32.h"

namespace picolet {

inline constexpr UINT kOverlayAction = WM_APP + 2;
enum class OverlayAction : WPARAM { Cancel, Copy, Save, Pin };

// 置顶遮罩和选区工具栏。调用方保有冻结帧，在关闭遮罩之前不能修改或释放它。
// 选区和标注使用桌面物理坐标，工具栏与尺寸标签各自按所在屏幕的 DPI 布局。
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
    enum class Drag { None, Create, Move, Resize, Mark };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    static LRESULT CALLBACK TextProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR id, DWORD_PTR data) noexcept;
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept;

    void Command(Shortcut shortcut) noexcept;
    void PressButton(ToolbarButton button) noexcept;
    void AdjustSize() noexcept;
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
    void UpdateTextStyle() noexcept;

    // 根据画布命中位置或已捕获的拖动操作设置光标。point 使用桌面物理坐标。
    void SetCanvasCursor(POINT point) const noexcept;
    // 窗口显示或拖动结束时同步光标，仅在鼠标实际指向本画布时生效。
    void RefreshCursor() const noexcept;

    RECT ToClient(RECT rect) const noexcept;
    POINT MessagePoint() const noexcept;
    // 取得选区左上角所在显示器的客户区物理范围与 DPI，供尺寸标签单独布局。
    ToolbarArea DimensionScreen() const noexcept;
    void LayoutPanel() noexcept;
    void Status(const wchar_t* text) noexcept;
    void RepaintSelection(const RECT& previous) noexcept;

    // 马赛克草稿只影响原选区内的矩形。每次改变终点前后各标记一次，旧预览由底图重绘清除。
    RECT MosaicDraftArea() const noexcept;
    void InvalidateMosaicDraft() const noexcept;
    // 在已裁到选区的 DC 上叠绘草稿，马赛克只在草稿区域需要重绘时计算像素。
    void PaintDraft(HDC dc) const noexcept;
    void Paint() noexcept;

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HWND textEdit_ = nullptr;
    CaptureToolbar toolbar_;
    const DesktopFrame* frame_ = nullptr;
    ShortcutBindings shortcuts_{};
    Image dark_;
    AnnotationHistory history_;
    Annotation draft_;
    FontHandle textFont_;
    UniqueHandle<HCURSOR, DestroyCursor> crosshair_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    RECT selection_{};
    RECT original_{};
    POINT anchor_{};
    POINT start_{};
    unsigned edges_ = EdgeNone;
    EditTool tool_ = EditTool::Select;
    EditTool shape_ = EditTool::Rectangle;
    Drag drag_ = Drag::None;
    ToolbarStyle style_;
    const wchar_t* status_ = nullptr;
    bool activated_ = false;
    bool invalidated_ = false;
    bool pending_ = false;
    bool busy_ = false;
    bool closing_ = false;
    bool composingText_ = false;
    bool paletteOpen_ = false;
};

}  // namespace picolet
