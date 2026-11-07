// 防闪烁回归：对真实 GDI 像素、窗口脏区域及生产绘制入口做检查，不依赖静态截图判断闪烁。
// 更新区测试窗口位于屏幕外且不激活，不注册热键、不安装输入钩子，不改变用户窗口或剪贴板。
#include "paint_buffer.h"
#include "overlay.h"
#include "long_capture.h"

#include <objbase.h>
#include <gdiplus.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace picolet;
namespace {
void Check(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}

using Region = UniqueHandle<HRGN, DeleteObject>;

// 测试目的 DC，直接读取 DIB 像素以验证提交前后及脏矩形外的画面。
class Surface {
public:
    Surface(int width, int height) : width_(width), height_(height), dc_(CreateCompatibleDC(nullptr)) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap_.reset(CreateDIBSection(dc_.get(), &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
        Check(bool(dc_) && bool(bitmap_) && pixels, "创建测试 DIB 失败");
        pixels_ = static_cast<std::uint32_t*>(pixels);
        old_ = SelectObject(dc_.get(), bitmap_.get());
        Fill(RGB(17, 35, 53));
    }
    ~Surface() { SelectObject(dc_.get(), old_); }
    HDC dc() const { return dc_.get(); }
    RECT bounds() const { return {0, 0, width_, height_}; }
    void Fill(COLORREF color) {
        const RECT area = bounds();
        SetDCBrushColor(dc(), color);
        FillRect(dc(), &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        GdiFlush();
    }
    std::uint32_t Pixel(int x, int y) const { return pixels_[static_cast<std::size_t>(y) * width_ + x] & 0xffffff; }
    void Equals(const Surface& other, const char* message) const {
        GdiFlush();
        Check(width_ == other.width_ && height_ == other.height_, "测试尺寸不一致");
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x) Check(Pixel(x, y) == other.Pixel(x, y), message);
    }
private:
    int width_, height_;
    MemoryDc dc_;
    BitmapHandle bitmap_;
    HGDIOBJ old_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
};

void DrawScene(HDC dc) {
    const RECT background{-100, -100, 300, 300};
    FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 10, 180, 110));
        graphics.FillEllipse(&brush, 19, 17, 42, 29);
        Gdiplus::Pen pen(Gdiplus::Color(255, 180, 60, 40), 3.0f);
        graphics.DrawLine(&pen, 20, 50, 70, 36);
    }
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, 23, 24, L"截图", 2);
}

void TestBuffer() {
    Surface target(90, 75), reference(90, 75);
    DrawScene(reference.dc());
    PaintBuffer buffer;
    Region mask(CreateRectRgn(13, 9, 33, 29)), second(CreateRectRgn(45, 33, 78, 63));
    CombineRgn(mask.get(), mask.get(), second.get(), RGN_OR);
    SelectClipRgn(target.dc(), mask.get());
    const auto untouched = target.Pixel(20, 20);
    HGDIOBJ firstBitmap = nullptr;
    {
        BufferedPaint paint(buffer, target.dc(), {13, 9, 78, 63});
        Check(paint.dc() != target.dc(), "没有启用绘制缓冲");
        firstBitmap = GetCurrentObject(paint.dc(), OBJ_BITMAP);
        DrawScene(paint.dc());
        GdiFlush();
        Check(target.Pixel(20, 20) == untouched, "提交前目的 DC 已经被修改");
    }
    GdiFlush();
    for (int y = 0; y < 75; ++y)
        for (int x = 0; x < 90; ++x)
            Check(target.Pixel(x, y) == (PtInRegion(mask.get(), x, y) ? reference.Pixel(x, y) : untouched),
                  "偏移绘制、GDI+ 图形或复杂裁剪的像素错误");
    Region after(CreateRectRgn(0, 0, 0, 0));
    Check(GetClipRgn(target.dc(), after.get()) == 1 && EqualRgn(mask.get(), after.get()), "目的 DC 裁剪被缓冲改写");
    SelectClipRgn(target.dc(), nullptr);
    {
        BufferedPaint paint(buffer, target.dc(), {20, 10, 35, 25});
        Check(GetCurrentObject(paint.dc(), OBJ_BITMAP) == firstBitmap, "小脏矩形未复用原 DIB");
        DrawScene(paint.dc());
    }
    {
        BufferedPaint paint(buffer, target.dc(), target.bounds());
        Check(GetCurrentObject(paint.dc(), OBJ_BITMAP) != firstBitmap, "较大脏矩形未扩容");
        DrawScene(paint.dc());
    }
    target.Equals(reference, "扩容后的完整绘制有残留像素");
    // 负的逻辑原点仍需映射到缓冲的 (0,0)，不依赖窗口所在屏幕的正负坐标。
    SetViewportOrgEx(target.dc(), 10, 10, nullptr);
    SetViewportOrgEx(reference.dc(), 10, 10, nullptr);
    DrawScene(reference.dc());
    {
        BufferedPaint paint(buffer, target.dc(), {-10, -10, 80, 65});
        DrawScene(paint.dc());
    }
    target.Equals(reference, "负坐标缓冲偏移错误");
    buffer.Reset(); buffer.Reset();
    const auto handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 100; ++i) {
        PaintBuffer local;
        BufferedPaint paint(local, target.dc(), {-10, -10, 80, 65});
        DrawScene(paint.dc());
    }
    GdiFlush();
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == handles, "重复绘制后 GDI 句柄泄漏");
}

bool Dirty(HWND window) { return GetUpdateRect(window, nullptr, FALSE) != FALSE; }

// 按真实窗口更新区提交局部画面，再与整帧参考比较，验证所有旧画面都能被正确恢复。
template<class Draw>
void PaintDirty(HWND window, Surface& target, PaintBuffer& buffer, Draw draw) {
    Region region(CreateRectRgn(0, 0, 0, 0));
    Check(GetUpdateRgn(window, region.get(), FALSE) != ERROR, "读取窗口更新区域失败");
    RECT bounds{}; GetRgnBox(region.get(), &bounds);
    SelectClipRgn(target.dc(), region.get());
    { BufferedPaint paint(buffer, target.dc(), bounds); draw(paint.dc()); }
    SelectClipRgn(target.dc(), nullptr);
    ValidateRect(window, nullptr);
}

void CheckNoOverlap(HWND window, RECT rect, const char* message) {
    Region region(CreateRectRgn(0, 0, 0, 0)); GetUpdateRgn(window, region.get(), FALSE);
    Check(!RectInRegion(region.get(), &rect), message);
}
}

namespace picolet {
// 使用生产对象的私有绘制入口构造隐藏测试会话，所有检查均在本测试线程完成。
struct PaintTestAccess {
    static void Clear(CaptureToolbar& toolbar) {
        ValidateRect(toolbar.parent_, nullptr);
        for (const auto control : toolbar.controls_) ValidateRect(control, nullptr);
    }

    static void TestToolbar(HINSTANCE instance) {
        const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"绘制测试", WS_POPUP,
            -20000, -20000, 900, 700, nullptr, nullptr, instance, nullptr);
        CaptureToolbar toolbar;
        Check(window && toolbar.Create(window, instance, DefaultShortcuts()), "创建隐藏工具栏失败");
        const ToolbarArea area{{0, 0, 900, 700}, 96};
        const RECT selection{120, 180, 620, 500};
        ToolbarView view; view.tool = EditTool::Pen;
        toolbar.Layout(area, area, selection, view, nullptr);
        // Windows 不为隐藏窗口保留失效区域，因此用屏幕外的可见窗口验证真实更新区。
        ShowWindow(window, SW_SHOWNOACTIVATE);
        Clear(toolbar);
        toolbar.Layout(area, area, selection, view, nullptr);
        Check(!Dirty(window), "相同布局仍刷新父窗口");
        for (const auto control : toolbar.controls_) Check(!Dirty(control), "相同布局仍刷新子控件");
        toolbar.LayoutDimension(area, selection);
        Check(!Dirty(window), "相同尺寸、位置、DPI 的标签仍刷新");
        const auto previous = toolbar.dimension_;
        const RECT moved{350, 300, 850, 620};
        toolbar.LayoutDimension(area, moved);
        Region dirty(CreateRectRgn(0, 0, 0, 0)); GetUpdateRgn(window, dirty.get(), FALSE);
        if (!RectInRegion(dirty.get(), &previous) || !RectInRegion(dirty.get(), &toolbar.dimension_)) {
            RECT box{}; const auto type = GetRgnBox(dirty.get(), &box);
            std::cerr << "标签更新区类型 " << type << "，旧标签 " << previous.left << ',' << previous.top
                      << "，新标签 " << toolbar.dimension_.left << ',' << toolbar.dimension_.top << '\n';
            Check(false, "标签移动未恢复新旧区域");
        }
        Clear(toolbar);
        toolbar.LayoutDimension({area.bounds, 144}, moved);
        Check(Dirty(window), "标签 DPI 变化没有刷新");
        const auto slider = toolbar.controls_[static_cast<std::size_t>(ToolbarButton::Size)];
        Check(SendMessageW(slider, WM_ERASEBKGND, 0, 0) == 1, "滑块仍提前擦背景");
        toolbar.Hide(); toolbar.Layout(area, area, selection, view, nullptr);
        Check(GetWindowLongPtrW(slider, GWL_STYLE) & WS_VISIBLE, "隐藏后相同布局未恢复控件");
        toolbar.Close(); DestroyWindow(window);
    }

    static void Prepare(SelectionOverlay& editor, DesktopFrame& frame, HINSTANCE instance, bool document) {
        editor.frame_ = &frame; editor.editorMode_ = document; editor.selection_ = frame.bounds;
        editor.dpi_ = 96;
        const auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Picolet.SelectionOverlay", L"屏幕外绘制测试",
            WS_POPUP | WS_CLIPCHILDREN | WS_HSCROLL | WS_VSCROLL, -20000, -20000, 1200, 850, nullptr, nullptr, instance, &editor);
        Check(window && editor.toolbar_.Create(window, instance, DefaultShortcuts()), "创建隐藏编辑会话失败");
        if (document) {
            editor.fitButton_ = CreateWindowExW(0, L"BUTTON", L"适应宽度", WS_CHILD, 0, 0, 1, 1, window, nullptr, instance, nullptr);
            editor.actualButton_ = CreateWindowExW(0, L"BUTTON", L"100%", WS_CHILD, 0, 0, 1, 1, window, nullptr, instance, nullptr);
            editor.viewport_.image = {frame.image.width(), frame.image.height()};
            editor.LayoutEditor(); editor.viewport_.FitWidth(); editor.UpdateScrollbars();
        } else {
            Check(SUCCEEDED(editor.dark_.Resize(frame.image.width(), frame.image.height())), "创建暗色底图失败");
            std::ranges::transform(frame.image.pixels(), editor.dark_.pixels().begin(), [](auto pixel) {
                return 0xff000000u | ((pixel >> 1) & 0x007f7f7fu);
            });
        }
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }

    static void TestEditor(HINSTANCE instance) {
        DesktopFrame frame;
        frame.bounds = {0, 0, 1400, 6000}; Check(SUCCEEDED(frame.image.Resize(1400, 6000)), "创建长图失败");
        for (int y = 0; y < 6000; ++y) for (int x = 0; x < 1400; ++x)
            frame.image.pixels()[static_cast<std::size_t>(y) * 1400 + x] = 0xff000000u | ((y * 19 + x * 11) & 0xffffff);
        SelectionOverlay editor; Prepare(editor, frame, instance, true);
        RECT client{}; GetClientRect(editor.window_, &client);
        Surface screen(client.right, client.bottom), reference(client.right, client.bottom);
        editor.PaintEditor(screen.dc());
        Check(!editor.overviewImage_.empty(), "未建立缩略图缓存");
        const auto cached = editor.overviewImage_.pixels().data();
        for (int i = 0; i < 12; ++i) {
            Clear(editor.toolbar_);
            editor.ScrollEditor(SB_VERT, i < 6 ? SB_PAGEDOWN : SB_PAGEUP);
            CheckNoOverlap(editor.window_, editor.toolbar_.main_, "滚动刷新了固定工具栏");
            CheckNoOverlap(editor.window_, editor.ZoomLabel(), "普通滚动刷新了缩放文字");
            Check(!editor.overviewImage_.empty(), "滚动清空了缩略图缓存");
            PaintDirty(editor.window_, screen, editor.paintBuffer_, [&](HDC dc) { editor.PaintEditor(dc); });
            editor.PaintEditor(reference.dc());
            screen.Equals(reference, "双向滚动的局部画面与整帧不一致");
            Check(editor.overviewImage_.pixels().data() == cached, "滚动重新分配了缩略图");
        }
        Clear(editor.toolbar_); editor.LayoutEditor();
        Check(!Dirty(editor.window_) && !editor.overviewImage_.empty(), "无变化布局触发刷新或清空缓存");
        editor.drag_ = SelectionOverlay::Drag::Pan; editor.panStart_ = editor.ClientCursor();
        editor.panX_ = editor.viewport_.scrollX; editor.panY_ = editor.viewport_.scrollY;
        editor.EndDrag({});
        Check(!Dirty(editor.window_) && !editor.overviewImage_.empty(), "平移结束重排了工具栏或清空缓存");
        editor.drag_ = SelectionOverlay::Drag::Pan; editor.original_ = editor.selection_; editor.AbortDrag();
        Check(!Dirty(editor.window_) && !editor.overviewImage_.empty(), "平移取消触发了无关刷新");
        const auto before = editor.viewport_;
        editor.viewport_.ZoomAt(1.25, {400, 400}); editor.RefreshEditorView(before);
        CheckNoOverlap(editor.window_, editor.toolbar_.main_, "缩放刷新了固定工具栏");
        PaintDirty(editor.window_, screen, editor.paintBuffer_, [&](HDC dc) { editor.PaintEditor(dc); });
        editor.PaintEditor(reference.dc()); screen.Equals(reference, "缩放的局部画面错误");
        // 在非整数缩放及非零滚动量下比较真实草稿，覆盖世界变换与缓冲视口偏移的叠加。
        for (const auto tool : {EditTool::Rectangle, EditTool::Mosaic}) {
            editor.draft_ = {};
            editor.draft_.tool = tool; editor.draft_.first = {300, 300}; editor.draft_.last = {500, 500};
            editor.draft_.clip = frame.bounds; editor.draft_.width = 6;
            editor.drag_ = SelectionOverlay::Drag::Mark;
            editor.RepaintSelection(editor.selection_, false);
            PaintDirty(editor.window_, screen, editor.paintBuffer_, [&](HDC dc) { editor.PaintEditor(dc); });
            editor.PaintEditor(reference.dc()); screen.Equals(reference, "缩放后的草稿或马赛克坐标错误");
            editor.drag_ = SelectionOverlay::Drag::None; editor.draft_ = {};
            editor.RepaintSelection(editor.selection_, false);
            PaintDirty(editor.window_, screen, editor.paintBuffer_, [&](HDC dc) { editor.PaintEditor(dc); });
        }
        Clear(editor.toolbar_); editor.RepaintSelection(editor.selection_, false);
        Check(!editor.overviewImage_.empty(), "草稿刷新清空了缩略图");
        Annotation mark; mark.tool = EditTool::Rectangle; mark.filled = true; mark.first = {300, 300};
        mark.last = {500, 500}; mark.clip = frame.bounds;
        Check(SUCCEEDED(editor.history_.Commit(frame, std::move(mark))), "提交绘制回归标注失败");
        editor.RepaintSelection(editor.selection_);
        Check(editor.overviewImage_.empty(), "标注提交没有清除旧缩略图");
        PaintDirty(editor.window_, screen, editor.paintBuffer_, [&](HDC dc) { editor.PaintEditor(dc); });
        editor.PaintEditor(reference.dc()); screen.Equals(reference, "标注后的局部画面错误");
        editor.Command(Shortcut::Undo);
        Check(editor.overviewImage_.empty(), "撤销后没有刷新缩略图");
        editor.PaintEditor(reference.dc()); editor.Command(Shortcut::Redo);
        Check(editor.overviewImage_.empty(), "重做后没有刷新缩略图");
        editor.dpi_ = 144; editor.LayoutEditor();
        PaintDirty(editor.window_, screen, editor.paintBuffer_, [&](HDC dc) { editor.PaintEditor(dc); });
        editor.PaintEditor(reference.dc()); screen.Equals(reference, "DPI 布局变化后的缓冲画面错误");
        editor.Close();
    }

    static void TestSelection(HINSTANCE instance) {
        DesktopFrame frame; frame.bounds = {0, 0, 1200, 850};
        Check(SUCCEEDED(frame.image.Resize(1200, 850)), "创建普通截图测试帧失败");
        for (std::size_t i = 0; i < frame.image.pixels().size(); ++i) frame.image.pixels()[i] = 0xff000000u | ((i * 191) & 0xffffff);
        SelectionOverlay editor; Prepare(editor, frame, instance, false);
        editor.selection_ = {120, 180, 620, 600};
        const ToolbarArea area{{0, 0, 1200, 850}, 96};
        editor.toolbar_.Layout(area, area, editor.selection_, ToolbarView{}, nullptr);
        Surface screen(1200, 850), reference(1200, 850);
        editor.PaintSelection(screen.dc());
        for (const auto selection : {RECT{350, 280, 850, 700}, RECT{250, 0, 900, 600}, RECT{80, 100, 700, 700}}) {
            Clear(editor.toolbar_);
            const auto previous = editor.selection_; editor.selection_ = selection;
            editor.toolbar_.LayoutDimension(area, selection); editor.RepaintSelection(previous, false);
            PaintDirty(editor.window_, screen, editor.paintBuffer_, [&](HDC dc) { editor.PaintSelection(dc); });
            editor.PaintSelection(reference.dc()); screen.Equals(reference, "移动/拉伸选区的标签或底图有残影");
        }
        editor.Close();
    }

    static void TestPanel(HINSTANCE instance) {
        LongCaptureSession panel;
        const auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Picolet.LongCapture", L"屏幕外预览测试", WS_POPUP | WS_CLIPCHILDREN,
            -20000, -20000, 238, 590, nullptr, nullptr, instance, &panel);
        Check(window != nullptr, "创建隐藏预览浮窗失败");
        panel.font_.reset(CreateUiFont(96));
        const auto button = [&] { return CreateWindowExW(0, L"BUTTON", L"自动滚动", WS_CHILD, 0, 0, 1, 1, window, nullptr, instance, nullptr); };
        panel.directionDown_ = button(); panel.directionUp_ = button(); panel.autoButton_ = button();
        panel.finishButton_ = button(); panel.cancelButton_ = button(); panel.Layout();
        ShowWindow(window, SW_SHOWNOACTIVATE);
        auto image = std::make_shared<Image>(); Check(SUCCEEDED(image->Resize(60, 200)), "创建预览失败");
        std::ranges::fill(image->pixels(), 0xffd1e8f2u);
        CapturePipelineSnapshot next; next.width = 1400; next.height = 42500; next.preview = image; next.viewport = {0, 0, 1400, 900};
        panel.UpdateSnapshot(next); panel.Status(L"正在连续跟踪");
        Surface screen(238, 590), reference(238, 590); panel.PaintContents(screen.dc());
        ValidateRect(window, nullptr);
        ++next.revision; ++next.stats.captured; ++next.stats.processed; ++next.lastFrameTick;
        panel.UpdateSnapshot(next); panel.Status(L"正在连续跟踪");
        Check(!Dirty(window), "仅统计或相同提示变化导致浮窗刷新");
        for (const auto top : {10000L, 35000L, 4000L}) {
            next.viewport = {0, top, 1400, top + 900}; panel.UpdateSnapshot(next);
            CheckNoOverlap(window, panel.DimensionRect(), "定位更新刷新了尺寸文字");
            CheckNoOverlap(window, panel.StatusRect(), "定位更新刷新了提示文字");
            PaintDirty(window, screen, panel.paintBuffer_, [&](HDC dc) { panel.PaintContents(dc); });
            panel.PaintContents(reference.dc()); screen.Equals(reference, "预览定位框移动有残影");
        }
        next.height = 43000; panel.UpdateSnapshot(next);
        PaintDirty(window, screen, panel.paintBuffer_, [&](HDC dc) { panel.PaintContents(dc); });
        panel.PaintContents(reference.dc()); screen.Equals(reference, "预览尺寸文字更新有残影");
        panel.Pause(L"暂停"); ValidateRect(window, nullptr); ValidateRect(panel.autoButton_, nullptr);
        panel.Pause(L"暂停"); Check(!Dirty(window) && !Dirty(panel.autoButton_), "重复暂停刷新了相同文字");
        panel.Close();
    }
};
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    ULONG_PTR token = 0; const Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        const auto instance = GetModuleHandleW(nullptr);
        Check(SelectionOverlay::Register(instance, nullptr) && LongCaptureSession::Register(instance, nullptr), "注册绘制测试窗口失败");
        TestBuffer(); PaintTestAccess::TestToolbar(instance); PaintTestAccess::TestEditor(instance);
        PaintTestAccess::TestSelection(instance); PaintTestAccess::TestPanel(instance);
        std::cout << "缓冲、裁剪、局部重绘和缩略图缓存测试通过。\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    Gdiplus::GdiplusShutdown(token);
    return result;
}
