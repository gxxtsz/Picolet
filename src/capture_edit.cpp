#include "capture_edit.h"

#include "win32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace picolet {
namespace {

using PenHandle = UniqueHandle<HPEN, DeleteObject>;

constexpr int kMosaicBlock = 12;
constexpr std::uint32_t kOpaque = 0xff000000u;

LONG ShiftEdge(LONG start, LONG delta, LONG low, LONG high) noexcept {
    return static_cast<LONG>(std::clamp<std::int64_t>(static_cast<std::int64_t>(start) + delta, low, high));
}

RECT DragBounds(POINT first, POINT last) noexcept {
    return {std::min(first.x, last.x), std::min(first.y, last.y),
            std::max(first.x, last.x) + 1, std::max(first.y, last.y) + 1};
}

RECT InkBounds(const Annotation& mark) noexcept {
    RECT rect = DragBounds(mark.first, mark.last);
    if (mark.tool == EditTool::Text) {
        rect = {mark.first.x, mark.first.y, mark.clip.right, mark.clip.bottom};
    } else if (mark.tool != EditTool::Mosaic) {
        for (const auto point : mark.path) {
            rect.left = std::min(rect.left, point.x);
            rect.top = std::min(rect.top, point.y);
            rect.right = std::max(rect.right, point.x + 1);
            rect.bottom = std::max(rect.bottom, point.y + 1);
        }
        const int pad = mark.tool == EditTool::Arrow ? 2 * std::max(12, 4 * mark.width) + mark.width : mark.width + 2;
        InflateRect(&rect, pad, pad);
    }
    IntersectRect(&rect, &rect, &mark.clip);
    return rect;
}

bool DrawMark(HDC dc, const Annotation& mark, POINT origin) noexcept {
    const int saved = SaveDC(dc);
    if (!saved) return false;
    OffsetViewportOrgEx(dc, -origin.x, -origin.y, nullptr);
    const int clip = IntersectClipRect(dc, mark.clip.left, mark.clip.top, mark.clip.right, mark.clip.bottom);
    bool ok = clip != ERROR;
    if (clip != ERROR && clip != NULLREGION) {
        if (mark.tool == EditTool::Text) {
            const FontHandle font(CreateAnnotationFont(mark.textSize));
            if (font && SelectObject(dc, font.get())) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, mark.color);
                RECT rect{mark.first.x, mark.first.y, mark.clip.right, mark.clip.bottom};
                ok = DrawTextW(dc, mark.text.c_str(), static_cast<int>(mark.text.size()), &rect,
                               DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL) != 0;
            } else {
                ok = false;
            }
            RestoreDC(dc, saved);
            return ok;
        }
        if (mark.tool == EditTool::Pen && mark.path.size() <= 1) {
            const BrushHandle dot(CreateSolidBrush(mark.color));
            if (dot && SelectObject(dc, dot.get()) && SelectObject(dc, GetStockObject(NULL_PEN))) {
                const LONG x = mark.first.x - mark.width / 2;
                const LONG y = mark.first.y - mark.width / 2;
                ok = Ellipse(dc, x, y, x + mark.width + 1, y + mark.width + 1) != FALSE;
            } else {
                ok = false;
            }
            RestoreDC(dc, saved);
            return ok;
        }

        const LOGBRUSH style{BS_SOLID, mark.color, 0};
        const PenHandle pen(mark.tool == EditTool::Mosaic
            ? CreatePen(PS_DOT, 1, RGB(245, 245, 245))
            : ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, mark.width, &style, 0, nullptr));
        if (pen && SelectObject(dc, pen.get()) && SelectObject(dc, GetStockObject(NULL_BRUSH))) {
            const RECT rect = DragBounds(mark.first, mark.last);
            switch (mark.tool) {
            case EditTool::Rectangle:
            case EditTool::Mosaic:
                ok = Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom) != FALSE;
                break;
            case EditTool::Ellipse:
                ok = Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom) != FALSE;
                break;
            case EditTool::Pen:
                ok = Polyline(dc, mark.path.data(), static_cast<int>(mark.path.size())) != FALSE;
                break;
            case EditTool::Arrow: {
                const double dx = static_cast<double>(mark.last.x) - mark.first.x;
                const double dy = static_cast<double>(mark.last.y) - mark.first.y;
                const double length = std::hypot(dx, dy);
                if (length > 0.0) {
                    const double head = std::min(length * 0.45, static_cast<double>(std::max(12, 4 * mark.width)));
                    const auto endpoint = [&](double side) -> POINT {
                        return {static_cast<LONG>(std::lround(mark.last.x - (dx + side * dy) * head / length)),
                                static_cast<LONG>(std::lround(mark.last.y - (dy - side * dx) * head / length))};
                    };
                    const POINT shaft[]{mark.first, mark.last};
                    const POINT tip[]{endpoint(0.5), mark.last, endpoint(-0.5)};
                    ok = Polyline(dc, shaft, 2) && Polyline(dc, tip, 3);
                }
                break;
            }
            default:
                break;
            }
        } else {
            ok = false;
        }
        RestoreDC(dc, saved);
        return ok;
    }
    RestoreDC(dc, saved);
    return ok;
}

void Mosaic(std::uint32_t* pixels, const RECT& patch, RECT rect) noexcept {
    const LONG stride = patch.right - patch.left;
    for (LONG top = rect.top; top < rect.bottom; top += kMosaicBlock) {
        const LONG bottom = std::min(rect.bottom, top + kMosaicBlock);
        for (LONG left = rect.left; left < rect.right; left += kMosaicBlock) {
            const LONG right = std::min(rect.right, left + kMosaicBlock);
            std::uint32_t blue = 0, green = 0, red = 0;
            for (LONG y = top; y < bottom; ++y) {
                const auto* row = pixels + static_cast<std::size_t>(y - patch.top) * stride;
                for (LONG x = left; x < right; ++x) {
                    const auto pixel = row[x - patch.left];
                    blue += pixel & 0xff;
                    green += (pixel >> 8) & 0xff;
                    red += (pixel >> 16) & 0xff;
                }
            }
            const auto count = static_cast<std::uint32_t>((bottom - top) * (right - left));
            const auto average = kOpaque | ((red / count) << 16) | ((green / count) << 8) | (blue / count);
            for (LONG y = top; y < bottom; ++y) {
                auto* row = pixels + static_cast<std::size_t>(y - patch.top) * stride;
                std::fill(row + (left - patch.left), row + (right - patch.left), average);
            }
        }
    }
}

}  // namespace

RECT MoveSelection(RECT selection, POINT delta, const RECT& bounds) noexcept {
    if (!IntersectRect(&selection, &selection, &bounds)) return {};
    const LONG width = selection.right - selection.left;
    const LONG height = selection.bottom - selection.top;
    const LONG left = ShiftEdge(selection.left, delta.x, bounds.left, bounds.right - width);
    const LONG top = ShiftEdge(selection.top, delta.y, bounds.top, bounds.bottom - height);
    return {left, top, left + width, top + height};
}

RECT ResizeSelection(RECT selection, unsigned edges, POINT delta, const RECT& bounds) noexcept {
    if (!IntersectRect(&selection, &selection, &bounds)) return {};
    if (edges & EdgeLeft) selection.left = ShiftEdge(selection.left, delta.x, bounds.left, selection.right - 1);
    if (edges & EdgeRight) selection.right = ShiftEdge(selection.right, delta.x, selection.left + 1, bounds.right);
    if (edges & EdgeTop) selection.top = ShiftEdge(selection.top, delta.y, bounds.top, selection.bottom - 1);
    if (edges & EdgeBottom) selection.bottom = ShiftEdge(selection.bottom, delta.y, selection.top + 1, bounds.bottom);
    return selection;
}

RECT SelectMonitor(POINT cursor, const RECT& bounds) noexcept {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT rect{};
    if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info)) {
        IntersectRect(&rect, &info.rcMonitor, &bounds);
    }
    return rect;
}

HFONT CreateAnnotationFont(int pixelSize) noexcept {
    return CreateFontW(-pixelSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH, L"Microsoft YaHei UI");
}

void DrawAnnotationDraft(HDC dc, const Annotation& mark, POINT origin) noexcept {
    DrawMark(dc, mark, origin);
}

HRESULT AnnotationHistory::Commit(const DesktopFrame& frame, Annotation&& mark) noexcept {
    if (mark.tool == EditTool::Select || mark.width < 1 || mark.width > 64 ||
        mark.textSize < 1 || mark.textSize > 256 || mark.path.size() > (std::numeric_limits<int>::max)() ||
        mark.text.size() > (std::numeric_limits<int>::max)()) {
        return E_INVALIDARG;
    }
    if (mark.tool == EditTool::Text && mark.text.empty()) return S_FALSE;
    const RECT ink = InkBounds(mark);
    RECT visible{};
    if (!IntersectRect(&visible, &ink, &frame.bounds)) return S_FALSE;
    try {
        marks_.push_back(std::move(mark));
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    const auto status = Rebuild(frame, marks_.size());
    if (FAILED(status)) marks_.pop_back();
    return status;
}

HRESULT AnnotationHistory::Undo(const DesktopFrame& frame) noexcept {
    if (marks_.empty()) return S_FALSE;
    const auto status = Rebuild(frame, marks_.size() - 1);
    if (SUCCEEDED(status)) marks_.pop_back();
    return status;
}

void AnnotationHistory::Clear() noexcept {
    marks_.clear();
    patch_ = {};
}

HRESULT AnnotationHistory::Rebuild(const DesktopFrame& frame, std::size_t count) noexcept {
    if (count == 0) {
        patch_ = {};
        return S_OK;
    }
    DesktopFrame patch;
    for (std::size_t i = 0; i < count; ++i) {
        const RECT ink = InkBounds(marks_[i]);
        UnionRect(&patch.bounds, &patch.bounds, &ink);
    }
    IntersectRect(&patch.bounds, &patch.bounds, &frame.bounds);
    const auto cropped = CropImage(frame, patch.bounds, patch.image);
    if (FAILED(cropped)) return cropped;

    const MemoryDc dc(CreateCompatibleDC(nullptr));
    if (!dc) return LastErrorResult();
    const auto info = patch.image.BitmapInfo();
    void* bits = nullptr;
    const BitmapHandle bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bitmap || !bits) return LastErrorResult();
    const HGDIOBJ old = SelectObject(dc.get(), bitmap.get());
    if (!old || old == HGDI_ERROR) return LastErrorResult();
    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::memcpy(pixels, patch.image.pixels().data(), patch.image.byte_size());

    // 马赛克按提交顺序处理已画好的像素。必须先等待 GDI 批处理写入 DIB，再由 CPU 读取。
    bool ok = true;
    const POINT origin{patch.bounds.left, patch.bounds.top};
    for (std::size_t i = 0; ok && i < count; ++i) {
        const auto& mark = marks_[i];
        if (mark.tool == EditTool::Mosaic) {
            RECT rect = InkBounds(mark);
            if (IntersectRect(&rect, &rect, &patch.bounds)) {
                ok = GdiFlush() != FALSE;
                if (ok) Mosaic(pixels, patch.bounds, rect);
            }
        } else {
            ok = DrawMark(dc.get(), mark, origin);
        }
    }
    ok = GdiFlush() && ok;
    SelectObject(dc.get(), old);
    if (!ok) return E_FAIL;
    const auto destination = patch.image.pixels();
    std::transform(pixels, pixels + destination.size(), destination.begin(),
                   [](std::uint32_t pixel) { return pixel | kOpaque; });
    patch_ = std::move(patch);
    return S_OK;
}

HRESULT AnnotationHistory::Render(const DesktopFrame& frame, const RECT& selection, Image& result) const noexcept {
    RECT crop{};
    if (!IntersectRect(&crop, &selection, &frame.bounds)) return E_INVALIDARG;
    Image image;
    const auto status = CropImage(frame, crop, image);
    if (FAILED(status)) return status;
    RECT overlap{};
    if (!patch_.image.empty() && IntersectRect(&overlap, &crop, &patch_.bounds)) {
        const auto bytes = static_cast<std::size_t>(overlap.right - overlap.left) * sizeof(std::uint32_t);
        for (LONG y = overlap.top; y < overlap.bottom; ++y) {
            const auto src = static_cast<std::size_t>(y - patch_.bounds.top) * patch_.image.width() +
                             (overlap.left - patch_.bounds.left);
            const auto dst = static_cast<std::size_t>(y - crop.top) * image.width() + (overlap.left - crop.left);
            std::memcpy(image.pixels().data() + dst, patch_.image.pixels().data() + src, bytes);
        }
    }
    result = std::move(image);
    return S_OK;
}

}  // namespace picolet
