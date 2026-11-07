#include "toolbar.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cwchar>
#include <initializer_list>

namespace picolet {
namespace {

using Button = ToolbarButton;
using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;

constexpr int kControlBase = 4000;
constexpr auto Index(Button button) noexcept { return static_cast<std::size_t>(button); }
constexpr bool IsSwatch(Button button) noexcept { return button >= Button::Red && button <= Button::Black; }
constexpr bool IsShape(EditTool tool) noexcept {
    return tool == EditTool::Rectangle || tool == EditTool::Ellipse || tool == EditTool::Line;
}

struct ControlSpec {
    const wchar_t* name;
    Shortcut shortcut = Shortcut::Count;
};

constexpr std::array<ControlSpec, Index(Button::Count)> kControls{{
    {L"调整选区"}, {L"重选"}, {L"全屏", Shortcut::Fullscreen},
    {L"长截图", Shortcut::LongCapture},
    {L"形状"}, {L"箭头"}, {L"画笔"}, {L"文字"}, {L"马赛克"},
    {L"撤销", Shortcut::Undo}, {L"重做", Shortcut::Redo},
    {L"贴图", Shortcut::Pin}, {L"另存为 PNG", Shortcut::Save}, {L"取消", Shortcut::Cancel},
    {L"完成并复制", Shortcut::Copy}, {L"矩形"}, {L"椭圆"}, {L"直线"}, {L"描边"}, {L"实心"},
    {L"粗细或字号"}, {L"标注颜色"}, {L"红色"}, {L"黄色"}, {L"绿色"}, {L"蓝色"}, {L"白色"}, {L"黑色"},
}};

constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(223, 225, 229);
constexpr COLORREF kInk = RGB(22, 24, 27);
constexpr COLORREF kMuted = RGB(96, 101, 111);
constexpr COLORREF kRed = RGB(216, 47, 48);
constexpr COLORREF kGreen = RGB(0, 139, 74);

Color Opaque(COLORREF color) noexcept {
    return Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

RectF Box(RECT rect) noexcept {
    return {static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top)};
}

void Smooth(Graphics& graphics) noexcept {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

void Rounded(Graphics& graphics, const RectF& rect, float radius, COLORREF color,
             const COLORREF* outline = nullptr) noexcept {
    if (rect.Width <= 0.0f || rect.Height <= 0.0f) return;
    const float d = std::min({2.0f * radius, rect.Width, rect.Height});
    const float right = rect.X + rect.Width - d;
    const float bottom = rect.Y + rect.Height - d;
    GraphicsPath path;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(right, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(right, bottom, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, bottom, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    const SolidBrush fill(Opaque(color));
    graphics.FillPath(&fill, &path);
    if (outline) {
        const Pen border(Opaque(*outline), 1.0f);
        graphics.DrawPath(&border, &path);
    }
}

// 图标均在 24×24 的逻辑画布内定义。实际尺寸随显示器 DPI 缩放，线端保持圆润。
void Glyph(Graphics& g, Button button, RECT rect, UINT dpi, COLORREF ink, COLORREF swatch) noexcept {
    const float w = static_cast<float>(rect.right - rect.left);
    const float h = static_cast<float>(rect.bottom - rect.top);
    const float extent = std::min({static_cast<float>(Dip(24, dpi)), w - 2.0f, h - 2.0f});
    if (extent <= 0) return;
    const auto saved = g.Save();
    g.TranslateTransform(rect.left + (w - extent) / 2.0f, rect.top + (h - extent) / 2.0f);
    g.ScaleTransform(extent / 24.0f, extent / 24.0f);
    Pen pen(Opaque(ink), 1.8f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const SolidBrush fill(Opaque(ink));
    const auto line = [&](std::initializer_list<PointF> points) {
        g.DrawLines(&pen, points.begin(), static_cast<int>(points.size()));
    };
    switch (button) {
    case Button::Select:
        line({{4, 12}, {20, 12}});
        line({{12, 4}, {12, 20}});
        line({{9, 7}, {12, 4}, {15, 7}});
        line({{9, 17}, {12, 20}, {15, 17}});
        line({{7, 9}, {4, 12}, {7, 15}});
        line({{17, 9}, {20, 12}, {17, 15}});
        break;
    case Button::Reselect:
        line({{5, 3}, {5, 18}, {21, 18}});
        line({{3, 6}, {18, 6}, {18, 21}});
        break;
    case Button::Fullscreen:
        line({{4, 9}, {4, 4}, {9, 4}});
        line({{15, 4}, {20, 4}, {20, 9}});
        line({{20, 15}, {20, 20}, {15, 20}});
        line({{9, 20}, {4, 20}, {4, 15}});
        break;
    case Button::LongCapture:
        line({{7, 3}, {4, 3}, {4, 21}, {7, 21}});
        line({{17, 3}, {20, 3}, {20, 21}, {17, 21}});
        line({{12, 4}, {12, 20}});
        line({{9, 7}, {12, 4}, {15, 7}});
        line({{9, 17}, {12, 20}, {15, 17}});
        break;
    case Button::Shapes:
        line({{6, 16}, {4, 16}, {4, 4}, {16, 4}, {16, 6}});
        g.DrawEllipse(&pen, 9, 9, 12, 12);
        break;
    case Button::Arrow:
        line({{4, 20}, {20, 4}, {12, 4}});
        line({{20, 4}, {20, 12}});
        break;
    case Button::Pen:
        line({{4, 19}, {5, 14}, {16, 3}, {21, 8}, {10, 19}, {4, 19}});
        line({{13, 6}, {18, 11}});
        line({{4, 22}, {20, 22}});
        break;
    case Button::Text:
        line({{4, 20}, {12, 4}, {20, 20}});
        line({{7, 14}, {17, 14}});
        break;
    case Button::Mosaic:
        g.DrawRectangle(&pen, 4, 4, 16, 16);
        g.FillRectangle(&fill, 4, 4, 8, 8);
        g.FillRectangle(&fill, 12, 12, 8, 8);
        break;
    case Button::Undo:
    case Button::Redo:
        if (button == Button::Redo) {
            g.TranslateTransform(24.0f, 0.0f);
            g.ScaleTransform(-1.0f, 1.0f);
        }
        line({{9, 4}, {4, 9}, {9, 14}});
        g.DrawBezier(&pen, 4, 9, 26, 6, 24, 23, 12, 20);
        break;
    case Button::Pin:
        line({{7, 4}, {17, 4}, {15, 10}, {19, 14}, {5, 14}, {9, 10}, {7, 4}});
        line({{12, 14}, {12, 22}});
        break;
    case Button::Save:
        line({{12, 3}, {12, 15}});
        line({{7, 11}, {12, 16}, {17, 11}});
        line({{4, 16}, {4, 21}, {20, 21}, {20, 16}});
        break;
    case Button::Cancel:
        line({{5, 5}, {19, 19}});
        line({{19, 5}, {5, 19}});
        break;
    case Button::Copy:
        line({{3, 12}, {9, 18}, {21, 6}});
        break;
    case Button::Rectangle:
        g.DrawRectangle(&pen, 4, 5, 16, 14);
        break;
    case Button::Ellipse:
        g.DrawEllipse(&pen, 4, 4, 16, 16);
        break;
    case Button::Line:
        line({{4, 20}, {20, 4}});
        break;
    case Button::Outline:
        g.DrawRectangle(&pen, 7, 7, 10, 10);
        break;
    case Button::Solid:
        g.FillRectangle(&fill, 6, 6, 12, 12);
        break;
    case Button::Color:
        Rounded(g, {0, 5, 13, 14}, 2.5f, swatch, &kBorder);
        line({{17, 10}, {20, 13}, {23, 10}});
        break;
    default:
        break;
    }
    g.Restore(saved);
}

bool Active(Button button, const ToolbarView& view) noexcept {
    switch (button) {
    case Button::Select: return view.tool == EditTool::Select;
    case Button::Shapes: return IsShape(view.tool);
    case Button::Rectangle: return view.tool == EditTool::Rectangle;
    case Button::Ellipse: return view.tool == EditTool::Ellipse;
    case Button::Line: return view.tool == EditTool::Line;
    case Button::Arrow: return view.tool == EditTool::Arrow;
    case Button::Pen: return view.tool == EditTool::Pen;
    case Button::Text: return view.tool == EditTool::Text;
    case Button::Mosaic: return view.tool == EditTool::Mosaic;
    case Button::Outline: return !view.style.filled;
    case Button::Solid: return view.style.filled;
    case Button::Color: return view.paletteOpen;
    default: return IsSwatch(button) && Index(button) - Index(Button::Red) == view.style.color;
    }
}

Button ToolButton(EditTool tool) noexcept {
    switch (tool) {
    case EditTool::Arrow: return Button::Arrow;
    case EditTool::Pen: return Button::Pen;
    case EditTool::Text: return Button::Text;
    default: return Button::Shapes;
    }
}

// 把常用控件先按组放入一行。组本身比屏幕更宽时，逐个控件折行以保持可点。
struct Rows {
    LONG limit;
    LONG height;
    LONG gap;
    LONG x = 0, y = 0, width = 0;

    void Group(LONG nextWidth, LONG separation, RECT& divider) noexcept {
        if (!x) return;
        if (x + separation + nextWidth > limit) {
            x = 0;
            y += height + gap;
            return;
        }
        const LONG middle = x + separation / 2;
        divider = {middle, y + height / 5, middle + 1, y + height * 4 / 5};
        x += separation - gap;
    }

    void Add(RECT& box, LONG itemWidth) noexcept {
        itemWidth = std::min(itemWidth, limit);
        if (x && x + gap + itemWidth > limit) {
            x = 0;
            y += height + gap;
        }
        if (x) x += gap;
        box = {x, y, x + itemWidth, y + height};
        x += itemWidth;
        width = std::max(width, x);
    }

    LONG Height() const noexcept { return width ? y + height : 0; }
};

RECT Place(const RECT& work, LONG left, LONG top, LONG width, LONG height) noexcept {
    width = std::min(width, work.right - work.left);
    height = std::min(height, work.bottom - work.top);
    left = std::clamp(left, work.left, work.right - width);
    top = std::clamp(top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}

void Offset(RECT& rect, LONG x, LONG y) noexcept {
    if (!IsRectEmpty(&rect)) OffsetRect(&rect, x, y);
}

LONG TextWidth(HWND window, HFONT font, const wchar_t* text) noexcept {
    HDC dc = GetDC(window);
    if (!dc) return 0;
    const auto previous = font ? SelectObject(dc, font) : nullptr;
    SIZE measured{};
    GetTextExtentPoint32W(dc, text, lstrlenW(text), &measured);
    if (previous) SelectObject(dc, previous);
    ReleaseDC(window, dc);
    return measured.cx;
}

void Caption(HDC dc, RECT rect, const wchar_t* value, COLORREF color, UINT alignment) noexcept {
    if (IsRectEmpty(&rect)) return;
    SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &rect, alignment | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

}  // namespace

bool CaptureToolbar::Create(HWND parent, HINSTANCE instance, const ShortcutBindings& shortcuts) noexcept {
    parent_ = parent;
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                               parent, nullptr, instance, nullptr);
    if (!tooltip_) return false;
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        const auto button = static_cast<Button>(i);
        const auto& spec = kControls[i];
        const bool slider = button == Button::Size;
        const auto id = reinterpret_cast<HMENU>(kControlBase + i);
        controls_[i] = CreateWindowExW(0, slider ? TRACKBAR_CLASSW : L"BUTTON", spec.name,
            WS_CHILD | WS_TABSTOP | (slider ? TBS_NOTICKS : BS_OWNERDRAW), 0, 0, 0, 0, parent, id, instance, nullptr);
        if (!controls_[i]) return false;
        if (slider) {
            SendMessageW(controls_[i], TBM_SETLINESIZE, 0, 1);
        } else if (!SetWindowSubclass(controls_[i], ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
            return false;
        }
        wchar_t key[96]{};
        if (spec.shortcut != Shortcut::Count) {
            const auto binding = shortcuts[ShortcutIndex(spec.shortcut)];
            if (binding.key) FormatHotkey(binding, key);
        }
        auto& label = tooltipText_[i];
        if (*key) std::swprintf(label.data(), label.size(), L"%ls（%ls）", spec.name, key);
        else lstrcpynW(label.data(), spec.name, static_cast<int>(label.size()));
        TOOLINFOW tip{};
        tip.cbSize = sizeof(tip);
        tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tip.hwnd = parent;
        tip.uId = reinterpret_cast<UINT_PTR>(controls_[i]);
        tip.lpszText = label.data();
        if (!SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip))) return false;
    }
    const Hotkey fullscreen = shortcuts[ShortcutIndex(Shortcut::Fullscreen)];
    if (fullscreen.key) {
        wchar_t key[96]{};
        FormatHotkey(fullscreen, key);
        std::swprintf(hintText_, std::size(hintText_), L"拖拽选择区域　·　全屏 %ls", key);
    } else {
        lstrcpynW(hintText_, L"拖拽选择区域", static_cast<int>(std::size(hintText_)));
    }
    // 开始提示与尺寸标签分别使用不同的文本，重选后仍能显示配置中的全屏键。
    return true;
}

void CaptureToolbar::Close() noexcept {
    if (tooltip_) DestroyWindow(std::exchange(tooltip_, nullptr));
    for (auto& control : controls_) if (control) DestroyWindow(std::exchange(control, nullptr));
    hovered_ = parent_ = nullptr;
    font_.reset();
    dimensionFont_.reset();
    main_ = options_ = palette_ = dimension_ = status_ = sizeLabel_ = valueLabel_ = {};
    separators_.fill({});
}

void CaptureToolbar::InvalidateSurfaces() const noexcept {
    for (const auto& rect : {main_, options_, palette_, dimension_, status_}) {
        if (!IsRectEmpty(&rect)) InvalidateRect(parent_, &rect, FALSE);
    }
}

void CaptureToolbar::Hide() noexcept {
    InvalidateSurfaces();
    SendMessageW(tooltip_, TTM_POP, 0, 0);
    for (const auto control : controls_) ShowWindow(control, SW_HIDE);
    hovered_ = nullptr;
    main_ = options_ = palette_ = dimension_ = status_ = sizeLabel_ = valueLabel_ = {};
    separators_.fill({});
}

bool CaptureToolbar::OverlapsBars(const RECT& rect) const noexcept {
    RECT overlap{};
    return IntersectRect(&overlap, &rect, &main_) || IntersectRect(&overlap, &rect, &options_) ||
           IntersectRect(&overlap, &rect, &palette_);
}

void CaptureToolbar::LayoutDimension(const ToolbarArea& screen, const RECT& selection) noexcept {
    if (!IsRectEmpty(&dimension_)) InvalidateRect(parent_, &dimension_, FALSE);
    dimension_ = {};
    if (IsRectEmpty(&selection) || IsRectEmpty(&screen.bounds)) return;
    const UINT dpi = screen.dpi;
    if (dimensionDpi_ != dpi || !dimensionFont_) {
        dimensionDpi_ = dpi;
        dimensionFont_.reset(CreateUiFont(dpi));
    }
    std::swprintf(dimensionText_, std::size(dimensionText_), L"%ld × %ld 像素",
                  selection.right - selection.left, selection.bottom - selection.top);
    const LONG width = TextWidth(parent_, dimensionFont_.get(), dimensionText_) + Dip(16, dpi);
    const LONG height = Dip(24, dpi);
    const LONG gap = Dip(6, dpi);
    const LONG above = selection.top - gap - height;
    dimension_ = Place(screen.bounds, selection.left, above, width, height);
    if (above < screen.bounds.top || OverlapsBars(dimension_)) {
        const LONG inset = std::min(gap, selection.bottom - selection.top - 1);
        dimension_ = Place(screen.bounds, selection.left, selection.top + inset, width, height);
    }
    InvalidateRect(parent_, &dimension_, FALSE);
}

void CaptureToolbar::Layout(const ToolbarArea& workArea, const ToolbarArea& labelScreen, const RECT& selection,
                            const ToolbarView& view, const wchar_t* status) noexcept {
    if (IsRectEmpty(&workArea.bounds)) {
        Hide();
        return;
    }
    InvalidateSurfaces();
    SendMessageW(tooltip_, TTM_POP, 0, 0);
    const UINT dpi = workArea.dpi;
    if (dpi_ != dpi || !font_) {
        dpi_ = dpi;
        font_.reset(CreateUiFont(dpi_));
    }
    view_ = view;
    main_ = options_ = palette_ = dimension_ = status_ = sizeLabel_ = valueLabel_ = {};
    separators_.fill({});
    lstrcpynW(statusText_, status ? status : L"", static_cast<int>(std::size(statusText_)));

    RECT work = workArea.bounds;
    InflateRect(&work, -std::min<LONG>(Dip(8, dpi), (work.right - work.left - 1) / 2),
                -std::min<LONG>(Dip(8, dpi), (work.bottom - work.top - 1) / 2));
    const LONG available = work.right - work.left;
    const LONG pad = std::min<LONG>(Dip(5, dpi), (available - 1) / 2);
    const LONG gap = Dip(6, dpi);
    const LONG inner = available - 2 * pad;
    const LONG spacing = Dip(2, dpi);
    const LONG dividerGap = Dip(13, dpi);
    const LONG mainButton = Dip(36, dpi);
    const LONG optionButton = Dip(32, dpi);
    const bool selected = !IsRectEmpty(&selection);
    std::array<RECT, kCount> boxes{};
    Rows main{inner, mainButton, spacing};
    const auto addGroup = [&](Button first, Button last, std::size_t group) {
        const auto count = static_cast<LONG>(Index(last) - Index(first) + 1);
        if (group) main.Group(count * mainButton + (count - 1) * spacing, dividerGap, separators_[group - 1]);
        for (auto i = Index(first); i <= Index(last); ++i) main.Add(boxes[i], mainButton);
    };
    if (selected) {
        if (view.document) main.Add(boxes[Index(Button::Select)], mainButton);
        else addGroup(Button::Select, Button::LongCapture, 0);
        addGroup(Button::Shapes, Button::Mosaic, 1);
        addGroup(Button::Undo, Button::Redo, 2);
        addGroup(Button::Pin, Button::Copy, 3);
    } else {
        main.Add(boxes[Index(Button::Fullscreen)], mainButton);
        main.Add(boxes[Index(Button::Cancel)], mainButton);
    }

    Rows options{inner, optionButton, spacing};
    const bool shapes = IsShape(view.tool);
    const bool fillControls = shapes && view.tool != EditTool::Line;
    const bool hasOptions = selected && (shapes || view.tool == EditTool::Arrow ||
                                        view.tool == EditTool::Pen || view.tool == EditTool::Text);
    const bool hasSize = hasOptions && (!fillControls || !view.style.filled);
    if (hasOptions) {
        if (shapes) {
            for (auto i = Index(Button::Rectangle); i <= Index(Button::Line); ++i) options.Add(boxes[i], optionButton);
        }
        if (fillControls) {
            options.Group(2 * optionButton + spacing, dividerGap, separators_[3]);
            options.Add(boxes[Index(Button::Outline)], optionButton);
            options.Add(boxes[Index(Button::Solid)], optionButton);
        }
        if (hasSize) {
            const LONG sizeWidth = Dip(34, dpi);
            const LONG valueWidth = Dip(25, dpi);
            const LONG sliderWidth = std::clamp<LONG>(inner - sizeWidth - valueWidth - 2 * spacing,
                                                      Dip(50, dpi), Dip(122, dpi));
            options.Group(sizeWidth + sliderWidth + valueWidth + spacing * 2, dividerGap, separators_[4]);
            options.Add(sizeLabel_, sizeWidth);
            options.Add(boxes[Index(Button::Size)], sliderWidth);
            options.Add(valueLabel_, valueWidth);
        }
        options.Add(boxes[Index(Button::Color)], Dip(42, dpi));
    }

    Rows palette{inner, Dip(26, dpi), spacing};
    if (hasOptions && view.paletteOpen) {
        for (auto i = Index(Button::Red); i <= Index(Button::Black); ++i) palette.Add(boxes[i], palette.height);
    }
    const LONG tagHeight = Dip(24, dpi);
    const LONG mainHeight = main.Height() + 2 * pad;
    const LONG mainWidth = main.width + 2 * pad;
    const LONG optionHeight = hasOptions ? options.Height() + 2 * pad : 0;
    const LONG paletteHeight = palette.width ? palette.Height() + 2 * pad : 0;
    const LONG blockHeight = mainHeight + (optionHeight ? gap + optionHeight : 0) +
                             (paletteHeight ? gap + paletteHeight : 0);
    const LONG below = work.bottom - selection.bottom - gap;
    const LONG above = selection.top - work.top - gap;
    const bool flipAbove = !view.document && selected && below < blockHeight && above >= blockHeight;
    const bool putBelow = !flipAbove;
    LONG desiredTop = work.top + tagHeight + gap;
    if (selected) {
        if (flipAbove) desiredTop = selection.top - gap - blockHeight;
        else if (below >= blockHeight) desiredTop = selection.bottom + gap;
        else desiredTop = selection.bottom - gap - blockHeight;
    }
    const LONG blockTop = view.document ? work.top + gap :
        std::clamp(desiredTop, work.top, std::max(work.top, work.bottom - blockHeight));
    const LONG mainX = view.document ? work.left : selected ? selection.right - mainWidth : (work.left + work.right - mainWidth) / 2;
    main_ = Place(work, mainX, putBelow ? blockTop : blockTop + blockHeight - mainHeight, mainWidth, mainHeight);
    if (hasOptions) {
        const auto& activeBox = boxes[Index(ToolButton(view.tool))];
        options_ = Place(work, main_.left + activeBox.left,
                         putBelow ? main_.bottom + gap : main_.top - gap - optionHeight,
                         options.width + 2 * pad, optionHeight);
        if (paletteHeight) {
            const auto& colorBox = boxes[Index(Button::Color)];
            palette_ = Place(work, options_.left + pad + colorBox.right - palette.width - 2 * pad,
                             putBelow ? options_.bottom + gap : options_.top - gap - paletteHeight,
                             palette.width + 2 * pad, paletteHeight);
        }
    }
    for (std::size_t i = 0; i < separators_.size(); ++i) {
        const auto& area = i < 3 ? main_ : options_;
        Offset(separators_[i], area.left + pad, area.top + pad);
    }
    Offset(sizeLabel_, options_.left + pad, options_.top + pad);
    Offset(valueLabel_, options_.left + pad, options_.top + pad);

    const auto placeTag = [&](LONG width, LONG left, LONG top) {
        RECT tag = Place(work, left, top, width, tagHeight);
        if (OverlapsBars(tag)) {
            tag = Place(work, left, selected ? selection.top + gap : main_.bottom + gap, width, tagHeight);
        }
        return tag;
    };
    if (selected) {
        LayoutDimension(labelScreen, selection);
        if (view.document) {
            std::swprintf(dimensionText_, std::size(dimensionText_), L"%ld × %ld 像素", view.imageSize.cx, view.imageSize.cy);
            dimension_ = {work.left + gap, blockTop + blockHeight + gap,
                          work.left + Dip(230, dpi), blockTop + blockHeight + gap + tagHeight};
        }
    } else {
        dimensionFont_.reset();
        dimensionDpi_ = dpi;
        lstrcpynW(dimensionText_, hintText_, static_cast<int>(std::size(dimensionText_)));
        const LONG width = TextWidth(parent_, font_.get(), dimensionText_) + Dip(16, dpi);
        dimension_ = placeTag(width, (work.left + work.right - width) / 2, main_.top - gap - tagHeight);
    }
    if (*statusText_) {
        const LONG width = TextWidth(parent_, font_.get(), statusText_) + Dip(16, dpi);
        if (dimension_.right + gap + width <= work.right)
            status_ = placeTag(width, dimension_.right + gap, dimension_.top);
        else
            status_ = placeTag(width, dimension_.left, dimension_.bottom + gap);
    }

    EnableWindow(controls_[Index(Button::Undo)], view.canUndo);
    SetWindowTextW(controls_[Index(Button::Select)], view.document ? L"移动视图" : L"调整选区");
    lstrcpynW(tooltipText_[Index(Button::Select)].data(), view.document ? L"移动视图" : L"调整选区", 128);
    EnableWindow(controls_[Index(Button::Redo)], view.canRedo);
    if (hasSize) {
        const bool text = view.tool == EditTool::Text;
        const HWND slider = controls_[Index(Button::Size)];
        value_ = text ? view.style.textSize : view.style.width;
        SetWindowTextW(slider, text ? L"字号" : L"粗细");
        const int minimum = text ? ToolbarStyle::kMinTextSize : ToolbarStyle::kMinWidth;
        const int maximum = text ? ToolbarStyle::kMaxTextSize : ToolbarStyle::kMaxWidth;
        SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(minimum, maximum));
        SendMessageW(slider, TBM_SETPOS, TRUE, value_);
    }
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        RECT box = boxes[i];
        if (IsRectEmpty(&box)) {
            ShowWindow(controls_[i], SW_HIDE);
            continue;
        }
        const auto& area = i < Index(Button::Rectangle) ? main_ : i < Index(Button::Red) ? options_ : palette_;
        OffsetRect(&box, area.left + pad, area.top + pad);
        if (!IntersectRect(&box, &box, &area)) {
            ShowWindow(controls_[i], SW_HIDE);
            continue;
        }
        SetWindowPos(controls_[i], nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        InvalidateRect(controls_[i], nullptr, FALSE);
    }
    InvalidateSurfaces();
}

int CaptureToolbar::Bottom() const noexcept {
    return std::max({main_.bottom, options_.bottom, palette_.bottom, dimension_.bottom, status_.bottom});
}

bool CaptureToolbar::Contains(POINT client) const noexcept {
    return PtInRect(&main_, client) || PtInRect(&options_, client) || PtInRect(&palette_, client) ||
           PtInRect(&dimension_, client) || PtInRect(&status_, client);
}

std::optional<ToolbarButton> CaptureToolbar::Clicked(WPARAM command, LPARAM sender) const noexcept {
    const auto id = LOWORD(command);
    if (HIWORD(command) != BN_CLICKED || id < kControlBase || id >= kControlBase + kCount) return std::nullopt;
    const auto index = id - kControlBase;
    const auto button = static_cast<Button>(index);
    if (button != Button::Size && sender && controls_[index] == reinterpret_cast<HWND>(sender)) return button;
    return std::nullopt;
}

bool CaptureToolbar::IsButton(HWND window) const noexcept {
    return window && !IsSlider(window) && std::ranges::find(controls_, window) != controls_.end();
}

bool CaptureToolbar::IsSlider(HWND window) const noexcept {
    return window && window == controls_[Index(Button::Size)];
}

int CaptureToolbar::SliderValue() const noexcept {
    return static_cast<int>(SendMessageW(controls_[Index(Button::Size)], TBM_GETPOS, 0, 0));
}

void CaptureToolbar::SetValue(int value) noexcept {
    value_ = value;
    if (!IsRectEmpty(&valueLabel_)) InvalidateRect(parent_, &valueLabel_, FALSE);
}

LRESULT CALLBACK CaptureToolbar::ButtonProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                           UINT_PTR id, DWORD_PTR data) noexcept {
    auto& self = *reinterpret_cast<CaptureToolbar*>(data);
    if (message == WM_MOUSEMOVE && self.hovered_ != window) {
        const HWND previous = std::exchange(self.hovered_, window);
        if (previous) InvalidateRect(previous, nullptr, FALSE);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE && self.hovered_ == window) {
        self.hovered_ = nullptr;
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ButtonProc, id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void CaptureToolbar::Paint(HDC dc) const noexcept {
    if (IsRectEmpty(&main_) && IsRectEmpty(&dimension_)) return;
    {
        Graphics graphics(dc);
        Smooth(graphics);
        for (const auto* rect : {&main_, &options_, &palette_, &status_, &dimension_}) {
            if (IsRectEmpty(rect)) continue;
            auto box = Box(*rect);
            box.Inflate(-0.5f, -0.5f);
            const UINT dpi = rect == &dimension_ ? dimensionDpi_ : dpi_;
            Rounded(graphics, box, static_cast<float>(Dip(10, dpi)), kWhite, &kBorder);
        }
        const SolidBrush divider(Opaque(kBorder));
        for (const auto& rect : separators_) {
            if (!IsRectEmpty(&rect)) graphics.FillRectangle(&divider, Box(rect));
        }
    }
    const auto previous = font_ ? SelectObject(dc, font_.get()) : nullptr;
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    if (!IsRectEmpty(&dimension_)) {
        const auto previousLabel = dimensionFont_ ? SelectObject(dc, dimensionFont_.get()) : nullptr;
        RECT text = dimension_;
        InflateRect(&text, -Dip(8, dimensionDpi_), 0);
        Caption(dc, text, dimensionText_, kInk, DT_CENTER);
        if (previousLabel) SelectObject(dc, previousLabel);
    }
    RECT statusText = status_;
    InflateRect(&statusText, -Dip(8, dpi_), 0);
    Caption(dc, statusText, statusText_, kInk, DT_LEFT);
    Caption(dc, sizeLabel_, view_.tool == EditTool::Text ? L"字号" : L"粗细", kMuted, DT_CENTER);
    wchar_t number[12]{};
    std::swprintf(number, std::size(number), L"%d", value_);
    Caption(dc, valueLabel_, number, kInk, DT_CENTER);
    SetBkMode(dc, oldMode);
    if (previous) SelectObject(dc, previous);
}

bool CaptureToolbar::DrawButton(const DRAWITEMSTRUCT& item) const noexcept {
    if (item.CtlID < kControlBase || item.CtlID >= kControlBase + kCount || !IsButton(item.hwndItem)) return false;
    const auto button = static_cast<Button>(item.CtlID - kControlBase);
    const bool enabled = !(item.itemState & ODS_DISABLED);
    const bool active = Active(button, view_);
    const bool pressed = enabled && (item.itemState & ODS_SELECTED);
    const bool hovered = enabled && item.hwndItem == hovered_;
    const COLORREF ink = !enabled ? RGB(163, 167, 175) : button == Button::Copy ? kGreen :
                         button == Button::Cancel ? kRed : kInk;
    FillRect(item.hDC, &item.rcItem, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    {
        Graphics graphics(item.hDC);
        Smooth(graphics);
        if (active || pressed || hovered) {
            auto background = Box(item.rcItem);
            background.Inflate(-1.0f, -1.0f);
            const COLORREF shade = pressed ? RGB(219, 222, 228) :
                                   active ? RGB(231, 233, 237) : RGB(242, 243, 245);
            Rounded(graphics, background, static_cast<float>(Dip(6, dpi_)), shade);
        }
        if (IsSwatch(button)) {
            auto swatch = Box(item.rcItem);
            swatch.Inflate(-static_cast<float>(Dip(5, dpi_)), -static_cast<float>(Dip(5, dpi_)));
            Rounded(graphics, swatch, static_cast<float>(Dip(3, dpi_)),
                    kToolbarColors[Index(button) - Index(Button::Red)], &kBorder);
        } else {
            Glyph(graphics, button, item.rcItem, dpi_, ink, kToolbarColors[view_.style.color]);
        }
    }
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -Dip(2, dpi_), -Dip(2, dpi_));
        DrawFocusRect(item.hDC, &focus);
    }
    return true;
}

std::optional<LRESULT> CaptureToolbar::DrawSlider(const NMHDR& item) const noexcept {
    if (!IsSlider(item.hwndFrom) || item.code != NM_CUSTOMDRAW) return std::nullopt;
    const auto& draw = reinterpret_cast<const NMCUSTOMDRAW&>(item);
    if (draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    RECT client{}, channel{}, thumb{};
    GetClientRect(item.hwndFrom, &client);
    SendMessageW(item.hwndFrom, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(item.hwndFrom, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    FillRect(draw.hdc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    {
        Graphics graphics(draw.hdc);
        Smooth(graphics);
        const auto track = Box(channel);
        const auto grip = Box(thumb);
        const float center = static_cast<float>(client.bottom) / 2.0f;
        const auto height = static_cast<float>(Dip(4, dpi_));
        Rounded(graphics, {track.X, center - height / 2.0f, track.Width, height},
                height / 2.0f, RGB(218, 221, 226));
        const auto size = std::min({grip.Width, grip.Height, static_cast<float>(Dip(15, dpi_))});
        const auto x = grip.X + (grip.Width - size) / 2.0f;
        constexpr COLORREF thumbBorder = RGB(193, 198, 207);
        Rounded(graphics, {x, center - size / 2.0f, size, size}, size / 2.0f, kWhite, &thumbBorder);
    }
    if (GetFocus() == item.hwndFrom) {
        InflateRect(&client, -1, -1);
        DrawFocusRect(draw.hdc, &client);
    }
    return CDRF_SKIPDEFAULT;
}

}  // namespace picolet
