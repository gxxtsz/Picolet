// 原生交互验证夹具：文档、聊天和网页卡片共用可滚动正文与固定头尾。
// 连续模式仅在夹具为前台时推进，截图框选阶段自动停住，恢复实时采集后继续运动。
#include "capture_edit.h"
#include "win32.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

using namespace picolet;
namespace {
Image page;
FontHandle font;
HWND toggle = nullptr;
int position = 0, mode = 0, direction = 1, tick = 0;
bool running = false;
constexpr int kPageHeight = 40000;

void Fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color); FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Prepare(int width) {
    if (width <= 0 || FAILED(page.Resize(width, kPageHeight))) return;
    const MemoryDc dc(CreateCompatibleDC(nullptr));
    void* bits = nullptr; const auto info = page.BitmapInfo();
    const BitmapHandle bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bitmap || !bits) return;
    const auto old = SelectObject(dc.get(), bitmap.get());
    Fill(dc.get(), {0, 0, width, kPageHeight}, RGB(255, 255, 255));
    const auto oldFont = font ? SelectObject(dc.get(), font.get()) : nullptr;
    SetBkMode(dc.get(), TRANSPARENT); SetTextColor(dc.get(), RGB(25, 45, 62));
    for (int row = 0, y = 12; y < kPageHeight - 90; ++row, y += mode == 0 ? 38 : 118) {
        const int left = mode == 1 && row % 2 ? 100 : 18;
        if (mode) Fill(dc.get(), {left, y, width - 24 - row % 3 * 18, y + 101},
                       mode == 1 ? RGB(226, 246, 235) : RGB(240, 244, 250));
        wchar_t text[180]{};
        std::swprintf(text, std::size(text), L"第 %04d 段　内容编号 %08d　快速滚动截图验证 %05d", row, row * 7919, row * 313);
        TextOutW(dc.get(), left + 8, y + 6, text, static_cast<int>(std::wcslen(text)));
        if (mode) {
            std::swprintf(text, std::size(text), L"原生测试内容：%d。请核对文字、段落顺序和固定区域。", row + 1);
            TextOutW(dc.get(), left + 8, y + 49, text, static_cast<int>(std::wcslen(text)));
        }
    }
    GdiFlush(); if (oldFont) SelectObject(dc.get(), oldFont); SelectObject(dc.get(), old);
    const auto* source = static_cast<const std::uint32_t*>(bits);
    std::transform(source, source + page.pixels().size(), page.pixels().begin(), [](auto value) { return value | 0xff000000; });
}

void Scroll(HWND window, int delta) {
    RECT client{}; GetClientRect(window, &client);
    position = std::clamp(position + delta, 0, std::max<int>(0, kPageHeight - (client.bottom - 142)));
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMax = kPageHeight - 1; info.nPage = std::max(1L, client.bottom - 142); info.nPos = position;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        font.reset(CreateAnnotationFont(20));
        const wchar_t* labels[]{L"文档", L"聊天", L"网页", L"连续滚动"};
        for (int i = 0; i < 4; ++i) {
            const auto button = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                16 + i * 132, 10, 120, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7000 + i)), nullptr, nullptr);
            if (font) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font.get()), FALSE);
            if (i == 3) toggle = button;
        }
        SetTimer(window, 1, 16, nullptr);
        return 0;
    }
    case WM_SIZE: {
        RECT client{}; GetClientRect(window, &client); Prepare(client.right); Scroll(window, 0); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) >= 7000 && LOWORD(wp) < 7003) {
            mode = LOWORD(wp) - 7000; position = 0;
            RECT client{}; GetClientRect(window, &client); Prepare(client.right); Scroll(window, 0);
        } else if (LOWORD(wp) == 7003) {
            running = !running; SetWindowTextW(toggle, running ? L"停止连续滚动" : L"连续滚动");
        }
        return 0;
    case WM_MOUSEWHEEL: Scroll(window, -GET_WHEEL_DELTA_WPARAM(wp) * 90 / WHEEL_DELTA); return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL; GetScrollInfo(window, SB_VERT, &info);
        switch (LOWORD(wp)) {
        case SB_LINEUP: Scroll(window, -35); break;
        case SB_LINEDOWN: Scroll(window, 35); break;
        case SB_PAGEUP: Scroll(window, -static_cast<int>(info.nPage)); break;
        case SB_PAGEDOWN: Scroll(window, info.nPage); break;
        case SB_THUMBTRACK: Scroll(window, info.nTrackPos - position); break;
        }
        return 0;
    }
    case WM_TIMER:
        if (running && GetForegroundWindow() == window) {
            const int step = 12 + static_cast<int>(18 * (1 + std::sin(++tick * 0.08)));
            if (position > 12000) direction = -1;
            if (position < 100) direction = 1;
            Scroll(window, step * direction);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        Fill(dc, client, RGB(18, 63, 78));
        if (!page.empty() && client.bottom > 142) {
            const int visible = std::min<int>(client.bottom - 142, page.height() - position);
            DrawImageSection(dc, page, {0, position, page.width(), position + visible}, {0, 84, client.right, 84 + visible});
        }
        const auto old = font ? SelectObject(dc, font.get()) : nullptr;
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT title{18, 45, client.right - 18, 79};
        DrawTextW(dc, L"固定顶部 · 正文可以快速滚动；框选时可包含本栏", -1, &title, DT_SINGLELINE | DT_VCENTER);
        RECT footer{18, client.bottom - 50, client.right - 18, client.bottom - 8};
        DrawTextW(dc, L"固定底部 · 模拟文档状态栏或聊天输入区，仅保留一次", -1, &footer, DT_SINGLELINE | DT_VCENTER);
        if (old) SelectObject(dc, old);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW definition{}; definition.lpfnWndProc = Proc; definition.hInstance = instance;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW); definition.lpszClassName = L"Picolet.ScrollFixture";
    if (!RegisterClassW(&definition)) return 1;
    const auto window = CreateWindowExW(0, definition.lpszClassName, L"Picolet 快速滚动验证", WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        120, 70, 980, 850, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
