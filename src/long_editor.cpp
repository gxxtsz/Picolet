#include "overlay.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace picolet {
namespace {
void FillEditor(HDC dc, const RECT& rect, COLORREF color) noexcept {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
}

HRESULT SelectionOverlay::OpenEditor(HINSTANCE instance, HWND owner, const DesktopFrame& frame, RECT anchor,
    const ShortcutBindings& shortcuts, AnnotationHistory&& history) noexcept {
    if (frame.image.empty() || frame.bounds.left || frame.bounds.top) return E_INVALIDARG;
    editorMode_ = true;
    suspended_ = false;
    owner_ = owner;
    frame_ = &frame;
    shortcuts_ = shortcuts;
    selection_ = frame.bounds;
    tool_ = EditTool::Select;
    drag_ = Drag::None;
    activated_ = invalidated_ = pending_ = busy_ = closing_ = composingText_ = paletteOpen_ = false;
    status_ = nullptr;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return E_FAIL;
    const RECT work = monitor.rcWork;
    dpi_ = USER_DEFAULT_SCREEN_DPI;
    const int width = std::min<LONG>(work.right - work.left, 1200);
    const int height = std::min<LONG>(work.bottom - work.top, 850);
    const HWND window = CreateWindowExW(WS_EX_APPWINDOW, L"Picolet.SelectionOverlay", L"Picolet 长图编辑",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_HSCROLL | WS_VSCROLL,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, owner, nullptr, instance, this);
    if (!window || !toolbar_.Create(window, instance, shortcuts)) { Close(); return E_FAIL; }
    dpi_ = GetDpiForWindow(window);
    fitButton_ = CreateWindowExW(0, L"BUTTON", L"适应宽度", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 1, 1, window, nullptr, instance, nullptr);
    actualButton_ = CreateWindowExW(0, L"BUTTON", L"100%", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 1, 1, window, nullptr, instance, nullptr);
    if (!fitButton_ || !actualButton_) { Close(); return E_FAIL; }
    history_ = std::move(history);
    viewport_ = {};
    viewport_.image = {frame.image.width(), frame.image.height()};
    LayoutEditor();
    viewport_.FitWidth();
    UpdateScrollbars();
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);
    return S_OK;
}

POINT SelectionOverlay::ClientCursor() const noexcept {
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(window_, &point);
    return point;
}

bool SelectionOverlay::ChangeView() noexcept {
    if (composingText_) return false;
    return FinishText(true);
}

void SelectionOverlay::LayoutEditor() noexcept {
    if (!window_ || !frame_ || !fitButton_ || closing_ || busy_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int sidebar = Dip(164, dpi_), gap = Dip(10, dpi_);
    if (!editorFont_ || editorFontDpi_ != dpi_) {
        editorFont_.reset(CreateUiFont(dpi_));
        editorFontDpi_ = dpi_;
        for (const auto control : {fitButton_, actualButton_})
            if (editorFont_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_.get()), TRUE);
    }
    const ToolbarArea area{{gap, 0, std::max<LONG>(gap + 1, client.right - sidebar), client.bottom}, dpi_};
    const ToolbarView view{tool_, style_, !history_.empty(), history_.can_redo() && !textEdit_, paletteOpen_, true,
                           {frame_->image.width(), frame_->image.height()}};
    toolbar_.Layout(area, area, {0, 0, 1, 1}, view, status_);
    viewport_.canvas = {gap, std::min<LONG>(client.bottom - 1, toolbar_.Bottom() + gap),
                        std::max<LONG>(gap + 1, client.right - sidebar), std::max<LONG>(gap + 1, client.bottom - gap)};
    if (viewport_.canvas.bottom <= viewport_.canvas.top) viewport_.canvas.bottom = viewport_.canvas.top + 1;
    viewport_.Clamp();
    const int sideX = client.right - sidebar + gap;
    SetWindowPos(fitButton_, nullptr, sideX, gap, sidebar - gap * 2, Dip(28, dpi_), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(actualButton_, nullptr, sideX, gap + Dip(36, dpi_), sidebar - gap * 2, Dip(28, dpi_), SWP_NOZORDER | SWP_NOACTIVATE);
    const RECT space{sideX, gap + Dip(100, dpi_), client.right - gap, client.bottom - gap};
    overview_ = FitImage(frame_->image, space);
    overviewImage_ = {};
    if (textEdit_) {
        RECT box = viewport_.ToClient({draft_.first.x, draft_.first.y, draft_.clip.right, draft_.first.y + 100});
        box.right = std::min(box.right, viewport_.canvas.right);
        box.bottom = std::min(viewport_.canvas.bottom, std::max(box.bottom, box.top + Dip(70, dpi_)));
        SetWindowPos(textEdit_, nullptr, box.left, box.top, std::max(1L, box.right - box.left),
                     std::max(1L, box.bottom - box.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    UpdateScrollbars();
}

void SelectionOverlay::UpdateScrollbars() noexcept {
    if (!editorMode_ || !window_ || !frame_) return;
    for (const int bar : {SB_HORZ, SB_VERT}) {
        const bool vertical = bar == SB_VERT;
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        info.nMax = std::max(0, static_cast<int>(std::ceil((vertical ? viewport_.image.cy : viewport_.image.cx) * viewport_.zoom)) - 1);
        info.nPage = static_cast<UINT>(std::max(1L, vertical ? viewport_.canvas.bottom - viewport_.canvas.top : viewport_.canvas.right - viewport_.canvas.left));
        info.nPos = static_cast<int>(vertical ? viewport_.scrollY : viewport_.scrollX);
        SetScrollInfo(window_, bar, &info, TRUE);
    }
}

void SelectionOverlay::ScrollEditor(int bar, UINT command) noexcept {
    if (busy_ || pending_ || drag_ != Drag::None || !ChangeView()) return;
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_ALL;
    GetScrollInfo(window_, bar, &info);
    double& position = bar == SB_VERT ? viewport_.scrollY : viewport_.scrollX;
    switch (command) {
    case SB_LINEUP: position -= Dip(30, dpi_); break;
    case SB_LINEDOWN: position += Dip(30, dpi_); break;
    case SB_PAGEUP: position -= info.nPage * 0.9; break;
    case SB_PAGEDOWN: position += info.nPage * 0.9; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
    case SB_TOP: position = 0; break;
    case SB_BOTTOM: position = info.nMax; break;
    default: return;
    }
    viewport_.Clamp();
    UpdateScrollbars();
    InvalidateRect(window_, nullptr, FALSE);
}

void SelectionOverlay::DrawDocumentImage(HDC dc, const Image& image, RECT bounds) const noexcept {
    RECT visible{}, area = viewport_.Visible();
    if (image.empty() || !IntersectRect(&visible, &area, &bounds)) return;
    RECT source = visible;
    OffsetRect(&source, -bounds.left, -bounds.top);
    DrawImageSection(dc, image, source, viewport_.ToClient(visible));
}

void SelectionOverlay::PaintEditor(HDC dc) noexcept {
    RECT client{};
    GetClientRect(window_, &client);
    FillEditor(dc, client, RGB(240, 242, 245));
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport_.canvas.left, viewport_.canvas.top, viewport_.canvas.right, viewport_.canvas.bottom);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    DrawDocumentImage(dc, frame_->image, frame_->bounds);
    const auto& patch = history_.patch();
    DrawDocumentImage(dc, patch.image, patch.bounds);
    if (drag_ == Drag::Mark) PaintDraft(dc);
    if (saved) RestoreDC(dc, saved);
    if (!IsRectEmpty(&overview_)) {
        if (overviewImage_.empty() && SUCCEEDED(overviewImage_.Resize(overview_.right - overview_.left, overview_.bottom - overview_.top))) {
            for (int y = 0; y < overviewImage_.height(); ++y)
                for (int x = 0; x < overviewImage_.width(); ++x) {
                    const POINT point{static_cast<LONG>(static_cast<std::int64_t>(x) * frame_->image.width() / overviewImage_.width()),
                                      static_cast<LONG>(static_cast<std::int64_t>(y) * frame_->image.height() / overviewImage_.height())};
                    auto pixel = frame_->image.pixels()[static_cast<std::size_t>(point.y) * frame_->image.width() + point.x];
                    if (!patch.image.empty() && PtInRect(&patch.bounds, point))
                        pixel = patch.image.pixels()[static_cast<std::size_t>(point.y - patch.bounds.top) * patch.image.width() + point.x - patch.bounds.left];
                    overviewImage_.pixels()[static_cast<std::size_t>(y) * overviewImage_.width() + x] = pixel;
                }
        }
        DrawImage(dc, overviewImage_, overview_);
        const RECT visible = viewport_.Visible();
        RECT box{overview_.left + MulDiv(visible.left, overview_.right - overview_.left, frame_->image.width()),
                 overview_.top + MulDiv(visible.top, overview_.bottom - overview_.top, frame_->image.height()),
                 overview_.left + MulDiv(visible.right, overview_.right - overview_.left, frame_->image.width()),
                 overview_.top + MulDiv(visible.bottom, overview_.bottom - overview_.top, frame_->image.height())};
        box.bottom = std::max(box.top + 2, box.bottom);
        SetDCBrushColor(dc, RGB(0, 139, 110));
        FrameRect(dc, &box, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    wchar_t label[32]{};
    std::swprintf(label, std::size(label), L"缩放 %.0f%%", viewport_.zoom * 100);
    RECT text{client.right - Dip(154, dpi_), Dip(80, dpi_), client.right - Dip(10, dpi_), Dip(100, dpi_)};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(80, 85, 95));
    const auto oldFont = editorFont_ ? SelectObject(dc, editorFont_.get()) : nullptr;
    DrawTextW(dc, label, -1, &text, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (oldFont) SelectObject(dc, oldFont);
    toolbar_.Paint(dc);
}

}  // namespace picolet
