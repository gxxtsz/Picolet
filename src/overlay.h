#pragma once

#include "capture_edit.h"
#include "hotkeys.h"
#include "toolbar.h"
#include "win32.h"
#include "image_view.h"
#include "paint_buffer.h"

namespace picolet {

inline constexpr UINT kOverlayAction = WM_APP + 2;
enum class OverlayAction : WPARAM { Cancel, Copy, Save, Pin, LongCapture };

// 置顶遮罩和选区工具栏。调用方保有冻结帧，在关闭遮罩之前不能修改或释放它。
// 选区和标注使用桌面物理坐标，工具栏与尺寸标签各自按所在屏幕的 DPI 布局。
class SelectionOverlay {
public:
    static bool Register(HINSTANCE instance, HICON icon) noexcept;

    // 在冻结帧上开始一次会话，并复制本次会话的局部快捷键。anchor 是初始鼠标位置。
    HRESULT Open(HINSTANCE instance, HWND owner, const DesktopFrame& frame, POINT anchor,
                 const ShortcutBindings& shortcuts) noexcept;

    // 在普通可调整窗口中编辑完整长图。frame 由应用持有，history 成功打开后才转移到本对象。
    // anchor 为原桌面选区，仅用于选择窗口所在显示器和贴图位置；图片坐标从 (0,0) 开始。
    HRESULT OpenEditor(HINSTANCE instance, HWND owner, const DesktopFrame& frame, RECT anchor,
                       const ShortcutBindings& shortcuts, AnnotationHistory&& history) noexcept;

    // 挂起普通遮罩，保留选区及历史。恢复时清除待处理标记，显示指定的中文状态。
    void Suspend() noexcept;
    void Resume(const wchar_t* status = nullptr) noexcept;

    // 读取不含标注的首帧，以及完成采集后迁移原历史。失败均保留原会话。
    HRESULT CaptureSeed(Image& result) const noexcept;
    HRESULT MapHistory(const SeedPlacement& placement, const DesktopFrame& target, AnnotationHistory& result) const noexcept;

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
    // 仅仓库内的绘制回归测试读取窗口脏区域和缓存状态，不向应用调用方增加接口。
    friend struct PaintTestAccess;
    enum class Drag { None, Create, Move, Resize, Mark, Pan };

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
    // 窗口显示或拖动结束时同步光标，仅在鼠标命中本窗口客户区时生效；保留系统边框光标。
    void RefreshCursor() const noexcept;

    RECT ToClient(RECT rect) const noexcept;
    POINT MessagePoint() const noexcept;
    // 取得选区左上角所在显示器的客户区物理范围与 DPI，供尺寸标签单独布局。
    ToolbarArea DimensionScreen() const noexcept;
    void LayoutPanel() noexcept;
    void Status(const wchar_t* text) noexcept;
    // contentChanged 只在已提交的像素变化时为 true；草稿或工具切换不重建长图缩略图。
    void RepaintSelection(const RECT& previous, bool contentChanged = true) noexcept;

    // 马赛克草稿只影响原选区内的矩形。每次改变终点前后各标记一次，旧预览由底图重绘清除。
    RECT MosaicDraftArea() const noexcept;
    void InvalidateMosaicDraft() const noexcept;
    // 在已裁到选区的 DC 上叠绘草稿，马赛克只在草稿区域需要重绘时计算像素。
    void PaintDraft(HDC dc) const noexcept;
    void Paint() noexcept;
    // 在指定 DC 上绘制普通截图；底图、标注及标签由外层统一缓冲后提交。
    void PaintSelection(HDC dc) const noexcept;

    // 长图编辑专用布局、可见行绘制、缩放和 32 位滚动条同步；普通截图仍使用原有桌面坐标。
    void LayoutEditor() noexcept;
    void PaintEditor(HDC dc) noexcept;
    void DrawDocumentImage(HDC dc, const Image& image, RECT bounds) const noexcept;
    void UpdateScrollbars() noexcept;
    void ScrollEditor(int bar, UINT command) noexcept;
    bool ChangeView() noexcept;
    POINT ClientCursor() const noexcept;
    // before 为本次视图操作前的快照；仅刷新画布、定位框新旧范围及变化的缩放文字。
    // 此方法同步滚动条，但不重排工具栏、不重建缩略图；位置无变化时不触发刷新。
    void RefreshEditorView(const ImageViewport& before) noexcept;
    // 将原图可见范围映射为缩略图客户区定位框；空缩略图返回空矩形。
    RECT OverviewPosition(const ImageViewport& view) const noexcept;
    // 缩放文字在客户区中的物理像素范围，绘制及局部刷新使用同一矩形。
    RECT ZoomLabel() const noexcept;

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HWND textEdit_ = nullptr;
    PaintBuffer paintBuffer_;
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
    bool editorMode_ = false;
    bool suspended_ = false;
    ImageViewport viewport_;
    RECT overview_{};
    Image overviewImage_;
    HWND fitButton_ = nullptr, actualButton_ = nullptr;
    FontHandle editorFont_;
    UINT editorFontDpi_ = 0;
    POINT panStart_{};
    double panX_ = 0, panY_ = 0;
};

}  // namespace picolet
