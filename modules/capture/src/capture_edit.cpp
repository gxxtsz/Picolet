#include <picolet/capture/capture_edit.h>

#include <picolet/win32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

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

bool SolidShape(const Annotation& mark) noexcept {
    return mark.filled && (mark.tool == EditTool::Rectangle || mark.tool == EditTool::Ellipse);
}

RECT InkBounds(const Annotation& mark) noexcept {
    if (mark.tool == EditTool::Pixels) {
        RECT bounds{};
        for (const auto& fragment : mark.fragments) UnionRect(&bounds, &bounds, &fragment.bounds);
        return bounds;
    }
    RECT rect = DragBounds(mark.first, mark.last);
    if (mark.tool == EditTool::Text) {
        rect = {mark.first.x, mark.first.y, mark.clip.right, mark.clip.bottom};
    } else if (mark.tool != EditTool::Mosaic && !SolidShape(mark)) {
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

        if (SolidShape(mark)) {
            const BrushHandle fill(CreateSolidBrush(mark.color));
            const RECT rect = DragBounds(mark.first, mark.last);
            ok = fill && SelectObject(dc, fill.get()) && SelectObject(dc, GetStockObject(NULL_PEN));
            if (ok) {
                ok = mark.tool == EditTool::Rectangle
                    ? Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom) != FALSE
                    : Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom) != FALSE;
            }
            RestoreDC(dc, saved);
            return ok;
        }

        const LOGBRUSH style{BS_SOLID, mark.color, 0};
        const PenHandle pen(mark.tool == EditTool::Mosaic
            ? CreatePen(PS_DOT, 1, RGB(245, 245, 245))
            : ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, mark.width, &style, 0, nullptr));
        if (pen && SelectObject(dc, pen.get()) && SelectObject(dc, GetStockObject(NULL_BRUSH))) {
            if (mark.tool == EditTool::Mosaic) SetBkMode(dc, TRANSPARENT);
            const RECT rect = DragBounds(mark.first, mark.last);
            switch (mark.tool) {
            case EditTool::Rectangle:
            case EditTool::Mosaic:
                ok = Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom) != FALSE;
                break;
            case EditTool::Ellipse:
                ok = Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom) != FALSE;
                break;
            case EditTool::Line: {
                const POINT points[]{mark.first, mark.last};
                ok = Polyline(dc, points, 2) != FALSE;
                break;
            }
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
    if (applied_ == marks_.max_size()) return E_OUTOFMEMORY;
    try {
        // 在修改合成图之前准备空间。撤销过时，原有尾部已经为新的一笔预留了容量。
        if (applied_ == marks_.capacity()) {
            const auto remaining = marks_.max_size() - applied_;
            marks_.reserve(applied_ + std::min(remaining, std::max<std::size_t>(1, applied_)));
        }
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    const auto status = Rebuild(frame, applied_, &mark);
    if (SUCCEEDED(status)) {
        static_assert(std::is_nothrow_move_constructible_v<Annotation>);
        marks_.resize(applied_);
        marks_.push_back(std::move(mark));
        ++applied_;
    }
    return status;
}

HRESULT AnnotationHistory::Undo(const DesktopFrame& frame) noexcept {
    if (empty()) return S_FALSE;
    const auto status = Rebuild(frame, applied_ - 1);
    if (SUCCEEDED(status)) --applied_;
    return status;
}

HRESULT AnnotationHistory::Redo(const DesktopFrame& frame) noexcept {
    if (!can_redo()) return S_FALSE;
    const auto status = Rebuild(frame, applied_ + 1);
    if (SUCCEEDED(status)) ++applied_;
    return status;
}

void AnnotationHistory::Clear() noexcept {
    marks_.clear();
    applied_ = 0;
    patch_ = {};
}

HRESULT AnnotationHistory::Rebuild(const DesktopFrame& frame, std::size_t count, const Annotation* pending) noexcept {
    const auto total = count + (pending ? 1 : 0);
    if (total == 0) {
        patch_ = {};
        return S_OK;
    }
    const auto markAt = [&](std::size_t i) -> const Annotation& {
        return i < count ? marks_[i] : *pending;
    };
    DesktopFrame patch;
    for (std::size_t i = 0; i < total; ++i) {
        const RECT ink = InkBounds(markAt(i));
        UnionRect(&patch.bounds, &patch.bounds, &ink);
    }
    IntersectRect(&patch.bounds, &patch.bounds, &frame.bounds);
    if (IsRectEmpty(&patch.bounds)) {
        patch_ = {};
        return S_OK;
    }
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
    for (std::size_t i = 0; ok && i < total; ++i) {
        const auto& mark = markAt(i);
        if (mark.tool == EditTool::Pixels) {
            ok = GdiFlush() != FALSE;
            for (const auto& fragment : mark.fragments) {
                RECT overlap{};
                if (!IntersectRect(&overlap, &fragment.bounds, &patch.bounds)) continue;
                const int width = fragment.bounds.right - fragment.bounds.left;
                for (LONG y = overlap.top; y < overlap.bottom; ++y) {
                    for (LONG x = overlap.left; x < overlap.right; ++x) {
                        const auto index = static_cast<std::size_t>(y - fragment.bounds.top) * width + x - fragment.bounds.left;
                        if (fragment.mask[index]) pixels[static_cast<std::size_t>(y - patch.bounds.top) * patch.image.width() +
                                                        x - patch.bounds.left] = fragment.pixels[index];
                    }
                }
            }
        } else if (mark.tool == EditTool::Mosaic) {
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

HRESULT AnnotationHistory::RenderMosaicDraft(const DesktopFrame& frame, const RECT& area, Image& result) const noexcept {
    RECT crop{};
    if (!IntersectRect(&crop, &area, &frame.bounds)) return S_FALSE;
    const auto status = Render(frame, crop, result);
    if (FAILED(status)) return status;
    Mosaic(result.pixels().data(), crop, crop);
    return S_OK;
}

HRESULT AnnotationHistory::MapToLongImage(const DesktopFrame& source, const RECT& sourceRect,
    const SeedPlacement& placement, const DesktopFrame& target, AnnotationHistory& result) const noexcept {
    const int width = sourceRect.right - sourceRect.left, height = sourceRect.bottom - sourceRect.top;
    if (target.bounds.left || target.bounds.top || target.bounds.right != target.image.width() ||
        target.bounds.bottom != target.image.height() || width != target.image.width() || height != placement.seedHeight ||
        placement.resultHeight != target.image.height() || placement.fixedTop < 0 || placement.fixedBottom < 0 ||
        placement.fixedTop + placement.fixedBottom >= height || placement.seedBodyTop < placement.fixedTop ||
        placement.seedBodyTop + height - placement.fixedTop - placement.fixedBottom > placement.resultHeight - placement.fixedBottom)
        return E_INVALIDARG;
    Image before;
    auto hr = CropImage(source, sourceRect, before);
    if (FAILED(hr) || before.width() != width || before.height() != height) return FAILED(hr) ? hr : E_INVALIDARG;
    AnnotationHistory mapped, prefix;
    try {
        mapped.marks_.reserve(marks_.size());
        const std::array<int, 4> edges{0, placement.fixedTop, height - placement.fixedBottom, height};
        const std::array<int, 3> destinations{0, placement.seedBodyTop, placement.resultHeight - placement.fixedBottom};
        for (const auto& original : marks_) {
            // prefix 始终在原冻结帧中重放，像素差异因此保留了原有裁剪、文字换行和马赛克分块。
            hr = prefix.Commit(source, Annotation(original));
            if (FAILED(hr)) return hr;
            Image after;
            hr = prefix.Render(source, sourceRect, after);
            if (FAILED(hr)) return hr;
            Annotation mark;
            mark.tool = EditTool::Pixels;
            mark.clip = target.bounds;
            RECT ink = InkBounds(original), visible{};
            if (IntersectRect(&visible, &ink, &sourceRect)) {
                OffsetRect(&visible, -sourceRect.left, -sourceRect.top);
                for (int band = 0; band < 3; ++band) {
                    RECT area{visible.left, std::max(visible.top, static_cast<LONG>(edges[band])),
                              visible.right, std::min(visible.bottom, static_cast<LONG>(edges[band + 1]))};
                    if (IsRectEmpty(&area)) continue;
                    AnnotationPixels fragment;
                    fragment.bounds = area;
                    OffsetRect(&fragment.bounds, 0, destinations[band] - edges[band]);
                    const auto count = static_cast<std::size_t>(area.right - area.left) * (area.bottom - area.top);
                    fragment.pixels.reserve(count);
                    fragment.mask.reserve(count);
                    bool changed = false;
                    for (LONG y = area.top; y < area.bottom; ++y)
                        for (LONG x = area.left; x < area.right; ++x) {
                            const auto index = static_cast<std::size_t>(y) * width + x;
                            const bool different = before.pixels()[index] != after.pixels()[index];
                            fragment.pixels.push_back(after.pixels()[index]);
                            fragment.mask.push_back(static_cast<BYTE>(different));
                            changed |= different;
                        }
                    if (changed) mark.fragments.push_back(std::move(fragment));
                }
            }
            mapped.marks_.push_back(std::move(mark));
            before = std::move(after);
        }
        mapped.applied_ = applied_;
        hr = mapped.Rebuild(target, applied_);
        if (FAILED(hr)) return hr;
        result = std::move(mapped);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

}  // namespace picolet
