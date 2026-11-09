// 直接复用生产采集浮窗及编辑器的交互夹具，配合 scroll_fixture 验证实际 GDI 与线程生命周期。
// 测试入口不注册全局热键，也不读写用户快捷键设置，可独立启动和重复验证。
#include <picolet/ui/long_capture.h>
#include <picolet/ui/overlay.h>
#include <picolet/ui/pins.h>
#include <picolet/image/export.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <cwchar>
#include <filesystem>
#include <utility>
#include <algorithm>

namespace picolet {
// 仅供本交互夹具观察生产绘制状态，不向应用添加诊断接口或计数开销。
struct PaintTestAccess {
    static RECT Toolbar(const SelectionOverlay& overlay) noexcept { return overlay.toolbar_.main_; }
    static bool Cached(const SelectionOverlay& overlay) noexcept { return !overlay.overviewImage_.empty(); }
};
}

using namespace picolet;
namespace {
LongCaptureSession capture;
SelectionOverlay editor;
PinWindows pins;
DesktopFrame document;
HINSTANCE instance;
RECT anchor{};
wchar_t status[180] = L"请先打开原生滚动验证页，再点击开始。";
bool documentMode = true;
unsigned long long paints = 0, toolbarPaints = 0, thumbnailBuilds = 0;

// 在生产窗口绘制之前读取真实更新区，绘制之后只读检查缓存；不注入输入、不改变绘制时序。
LRESULT CALLBACK ObservePaint(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ObservePaint, id);
    if (message != WM_PAINT) return DefSubclassProc(window, message, wp, lp);
    const auto region = CreateRectRgn(0, 0, 0, 0);
    const auto toolbar = PaintTestAccess::Toolbar(editor);
    if (region) {
        GetUpdateRgn(window, region, FALSE);
        if (RectInRegion(region, &toolbar)) ++toolbarPaints;
        DeleteObject(region);
    }
    ++paints;
    const bool cached = PaintTestAccess::Cached(editor);
    const auto result = DefSubclassProc(window, message, wp, lp);
    if (!cached && PaintTestAccess::Cached(editor)) ++thumbnailBuilds;
    return result;
}

void ObserveEditor() {
    paints = toolbarPaints = thumbnailBuilds = 0;
    SetWindowSubclass(editor.window(), ObservePaint, 2, 0);
}

// 分块生成带段号和色块的 1400×42500 测试长图，避免另建同尺寸 GDI 位图。
HRESULT PrepareLongDocument() {
    constexpr int width = 1400, height = 42500, tileHeight = 256;
    auto hr = document.image.Resize(width, height);
    if (FAILED(hr)) return hr;
    Image tile; hr = tile.Resize(width, tileHeight);
    if (FAILED(hr)) return hr;
    MemoryDc dc(CreateCompatibleDC(nullptr));
    void* pixels = nullptr;
    const auto info = tile.BitmapInfo();
    BitmapHandle bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    if (!dc || !bitmap || !pixels) return E_OUTOFMEMORY;
    const auto oldBitmap = SelectObject(dc.get(), bitmap.get());
    FontHandle font(CreateAnnotationFont(28));
    const auto oldFont = font ? SelectObject(dc.get(), font.get()) : nullptr;
    SetBkMode(dc.get(), TRANSPARENT);
    for (int top = 0; top < height; top += tileHeight) {
        const RECT bounds{0, 0, width, tileHeight};
        FillRect(dc.get(), &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        SetDCBrushColor(dc.get(), RGB(225 + (top / tileHeight) % 20, 240, 248));
        RECT card{30, 12, width - 30, 240};
        FillRect(dc.get(), &card, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetTextColor(dc.get(), RGB(25, 45, 62));
        wchar_t text[180]{};
        std::swprintf(text, std::size(text), L"第 %03d 段：长图连续滚动与局部重绘验证，原图行 %d。", top / tileHeight + 1, top);
        TextOutW(dc.get(), 55, 35, text, static_cast<int>(std::wcslen(text)));
        constexpr wchar_t detail[] = L"检查：画面无露底，固定工具栏不刷新，缩略图缓存不重建。";
        TextOutW(dc.get(), 55, 105, detail, static_cast<int>(std::size(detail) - 1));
        GdiFlush();
        const auto count = static_cast<std::size_t>(std::min(tileHeight, height - top)) * width;
        const auto* source = static_cast<const std::uint32_t*>(pixels);
        std::transform(source, source + count, document.image.pixels().begin() + static_cast<std::size_t>(top) * width,
                       [](auto pixel) { return pixel | 0xff000000u; });
    }
    if (oldFont) SelectObject(dc.get(), oldFont);
    SelectObject(dc.get(), oldBitmap);
    document.bounds = {0, 0, width, height};
    return S_OK;
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        CreateWindowExW(0, L"BUTTON", L"开始长截图", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            18, 18, 200, 38, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7100)), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"打开测试长图（1400×42500）", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            18, 66, 360, 38, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7101)), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"普通截图绘制验证", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            18, 114, 360, 38, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7102)), instance, nullptr);
        SetTimer(window, 2, 250, nullptr);
        return 0;
    case WM_TIMER:
        if (capture.window()) {
            const auto snapshot = capture.Snapshot();
            wchar_t title[180]{};
            std::swprintf(title, std::size(title), L"Picolet 长截图 · 采集 %llu / 处理 %llu / 运动 %llu / 原因 %d / 排队 %zu",
                          snapshot.stats.captured, snapshot.stats.processed, snapshot.stats.motion, int(snapshot.failure), snapshot.stats.queued);
            SetWindowTextW(capture.window(), title);
        }
        if (editor.window()) {
            // 窗口截图可能不包含系统光标；只读显示实际光标和窗口尺寸，便于验证边框反馈。
            CURSORINFO cursor{}; cursor.cbSize = sizeof(cursor);
            const wchar_t* shape = L"未知";
            if (GetCursorInfo(&cursor)) {
                for (const auto& entry : {std::pair{IDC_SIZEWE, L"水平缩放"}, std::pair{IDC_SIZENS, L"垂直缩放"},
                     std::pair{IDC_SIZENWSE, L"左上右下缩放"}, std::pair{IDC_SIZENESW, L"右上左下缩放"},
                     std::pair{IDC_HAND, L"手形"}, std::pair{IDC_IBEAM, L"文字"}, std::pair{IDC_CROSS, L"十字"},
                     std::pair{IDC_ARROW, L"箭头"}})
                    if (cursor.hCursor == LoadCursorW(nullptr, entry.first)) { shape = entry.second; break; }
            }
            RECT bounds{}; GetWindowRect(editor.window(), &bounds);
            wchar_t title[240]{};
            std::swprintf(title, std::size(title), L"Picolet %ls · %ls · %ld×%ld · %u DPI · 重绘 %llu / 工具栏 %llu / 缩略图 %llu",
                          documentMode ? L"长图编辑" : L"截图验证", shape, bounds.right - bounds.left, bounds.bottom - bounds.top,
                          GetDpiForWindow(editor.window()), paints, toolbarPaints, thumbnailBuilds);
            wchar_t previous[240]{}; GetWindowTextW(editor.window(), previous, static_cast<int>(std::size(previous)));
            if (lstrcmpW(previous, title)) SetWindowTextW(editor.window(), title);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 7101 || LOWORD(wp) == 7102) {
            ShowWindow(window, SW_HIDE);
            documentMode = LOWORD(wp) == 7101;
            HRESULT hr;
            if (documentMode) {
                hr = PrepareLongDocument();
                anchor = {180, 180, 1580, 1000};
                if (SUCCEEDED(hr)) hr = editor.OpenEditor(instance, window, document, anchor, DefaultShortcuts(), AnnotationHistory{});
            } else {
                DwmFlush();  // 等待测试入口隐藏后再冻结桌面，避免夹具自身残留在测试底图中。
                hr = CaptureDesktop(document);
                if (SUCCEEDED(hr)) hr = editor.Open(instance, window, document, {180, 180}, DefaultShortcuts());
                if (SUCCEEDED(hr)) {
                    const auto overlay = editor.window();
                    SetWindowLongPtrW(overlay, GWL_EXSTYLE, (GetWindowLongPtrW(overlay, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
                }
            }
            if (SUCCEEDED(hr)) ObserveEditor();
            else { document = {}; ShowWindow(window, SW_SHOW); MessageBoxW(window, L"无法打开绘制验证会话。", L"交互验证", MB_OK); }
            return 0;
        }
        if (LOWORD(wp) == 7100) {
            const HWND source = FindWindowW(L"Picolet.ScrollFixture", nullptr);
            if (!source) { MessageBoxW(window, L"请先打开 picolet_scroll_fixture.exe。", L"交互验证", MB_OK); return 0; }
            RECT client{}; GetClientRect(source, &client);
            POINT origin{0, 0}; ClientToScreen(source, &origin);
            // 保留固定说明栏与底部，排除顶部用于切换测试模式的按钮。
            anchor = {origin.x + 2, origin.y + 44, origin.x + client.right - 2, origin.y + client.bottom - 2};
            ShowWindow(window, SW_HIDE); SetForegroundWindow(source); DwmFlush();
            Image seed, annotated;
            auto hr = CaptureRegion(anchor, seed);
            if (SUCCEEDED(hr)) hr = CopyImageRect(seed, {0, 0, seed.width(), seed.height()}, annotated);
            if (SUCCEEDED(hr)) hr = capture.Open(instance, window, anchor, seed, std::move(annotated), {0, VK_ESCAPE});
            if (SUCCEEDED(hr)) {
                // 仅测试入口把生产工具窗暴露为可观察的应用窗口，便于 UI 自动化选择控件。
                const auto panel = capture.window();
                SetWindowLongPtrW(panel, GWL_EXSTYLE, (GetWindowLongPtrW(panel, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
                SetWindowPos(panel, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                RECT panelRect{}, overlap{};
                if (GetWindowRect(panel, &panelRect) && !IntersectRect(&overlap, &panelRect, &anchor)) SetWindowDisplayAffinity(panel, WDA_NONE);
            }
            if (FAILED(hr)) { ShowWindow(window, SW_SHOW); MessageBoxW(window, L"无法开始，请检查验证页是否完整位于同一显示器。", L"交互验证", MB_OK); }
        }
        return 0;
    case kScrollAction:
        if (reinterpret_cast<HWND>(lp) != capture.window()) return 0;
        if (static_cast<ScrollAction>(wp) == ScrollAction::Cancel) {
            capture.Close(); ShowWindow(window, SW_SHOW); SetForegroundWindow(window); return 0;
        } else {
            SeedPlacement placement;
            auto hr = capture.Finish(document.image, placement);
            if (FAILED(hr)) { capture.Retry(L"测试结果尚未生成，请重试。"); return 0; }
            document.bounds = {0, 0, document.image.width(), document.image.height()};
            std::swprintf(status, std::size(status), L"最近完成：%d × %d 像素，固定顶部 %d，底部 %d。", document.image.width(),
                          document.image.height(), placement.fixedTop, placement.fixedBottom);
            documentMode = true;
            hr = editor.OpenEditor(instance, window, document, anchor, DefaultShortcuts(), AnnotationHistory{});
            if (SUCCEEDED(hr)) { capture.Close(); ObserveEditor(); } else capture.Retry(L"无法打开测试编辑器，请重试。");
            return 0;
        }
    case kOverlayAction: {
        if (reinterpret_cast<HWND>(lp) != editor.window()) return 0;
        const auto action = static_cast<OverlayAction>(wp);
        if (action == OverlayAction::LongCapture) {
            editor.Retry(L"此入口仅验证普通截图绘制，请返回测试入口开始长截图。");
            return 0;
        }
        if (action != OverlayAction::Cancel) {
            if (!documentMode) anchor = editor.selection();
            Image image; editor.BeginOutput();
            auto hr = editor.Render(image);
            if (SUCCEEDED(hr)) {
                if (action == OverlayAction::Save) hr = SavePngWithDialog(editor.window(), image);
                else if (action == OverlayAction::Pin) hr = pins.Open(instance, window, std::move(image), anchor);
                else hr = CopyImageToClipboard(editor.window(), image);
            }
            if (hr != S_OK) { editor.Retry(FAILED(hr) ? L"测试输出失败，可重试。" : nullptr); return 0; }
        }
        editor.Close(); document = {};
        ShowWindow(window, SW_SHOW); SetForegroundWindow(window); InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        RECT text{18, 174, 460, 280}; DrawTextW(dc, status, -1, &text, DT_WORDBREAK); EndPaint(window, &paint); return 0;
    }
    case WM_DESTROY:
        capture.Close(); editor.Close(); pins.CloseAll(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE appInstance, HINSTANCE, PWSTR, int) {
    instance = appInstance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    const Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { CoUninitialize(); return 1; }
    const auto icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!LongCaptureSession::Register(instance, icon) || !SelectionOverlay::Register(instance, icon) || !PinWindows::Register(instance, icon)) return 1;
    WNDCLASSW definition{}; definition.lpfnWndProc = Proc; definition.hInstance = instance;
    definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.lpszClassName = L"Picolet.CaptureFixture"; RegisterClassW(&definition);
    const auto window = CreateWindowExW(0, definition.lpszClassName, L"Picolet 长截图测试入口", WS_OVERLAPPEDWINDOW,
        1150, 120, 520, 360, nullptr, nullptr, instance, nullptr);
    ShowWindow(window, SW_SHOW);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (editor.HandleKey(message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    Gdiplus::GdiplusShutdown(token); CoUninitialize(); return 0;
}
