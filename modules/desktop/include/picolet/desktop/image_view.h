#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace picolet {

// 编辑视口只管理几何关系，不持有图像或窗口。图片坐标始终是原始像素，滚动量是客户区像素。
// 控件布局后设置 canvas 和 image，再调用 Clamp；缩放不会改写标注的原始线宽或字号。
struct ImageViewport {
    RECT canvas{};
    SIZE image{};
    double zoom = 1.0, scrollX = 0, scrollY = 0;

    // 图片小于画布时居中；大于画布时允许滚动，返回其原点在客户区的位置。
    POINT Origin() const noexcept {
        return {canvas.left + static_cast<LONG>(std::max(0.0, (canvas.right - canvas.left - image.cx * zoom) / 2) - scrollX),
                canvas.top + static_cast<LONG>(std::max(0.0, (canvas.bottom - canvas.top - image.cy * zoom) / 2) - scrollY)};
    }
    // 限制滚动范围；调整窗口大小、缩放或直接设置滚动量后均须调用。
    void Clamp() noexcept {
        scrollX = std::clamp(scrollX, 0.0, std::max(0.0, image.cx * zoom - (canvas.right - canvas.left)));
        scrollY = std::clamp(scrollY, 0.0, std::max(0.0, image.cy * zoom - (canvas.bottom - canvas.top)));
    }
    // 客户区鼠标位置转图片像素，不裁剪，调用方可区分图片外侧和图片内部。
    POINT ToImage(POINT point) const noexcept {
        const auto origin = Origin();
        return {static_cast<LONG>(std::floor((point.x - origin.x) / zoom)),
                static_cast<LONG>(std::floor((point.y - origin.y) / zoom))};
    }
    // 半开图片矩形映射到客户区；外侧取整以包含像素边缘，适合重绘和裁剪。
    RECT ToClient(RECT rect) const noexcept {
        const auto origin = Origin();
        return {origin.x + static_cast<LONG>(std::floor(rect.left * zoom)),
                origin.y + static_cast<LONG>(std::floor(rect.top * zoom)),
                origin.x + static_cast<LONG>(std::ceil(rect.right * zoom)),
                origin.y + static_cast<LONG>(std::ceil(rect.bottom * zoom))};
    }
    // 当前可见的原图矩形；用于只传输可见行，避免 GDI 绘制超高图片时溢出。
    RECT Visible() const noexcept {
        const auto first = ToImage({canvas.left, canvas.top});
        const auto last = ToImage({canvas.right, canvas.bottom});
        return {std::clamp(first.x, 0L, image.cx), std::clamp(first.y, 0L, image.cy),
                std::clamp(last.x + 1, 0L, image.cx), std::clamp(last.y + 1, 0L, image.cy)};
    }
    // 以客户区 anchor 对应的图片点为缩放锚点；触及滚动边缘时服从可见范围约束。
    void ZoomAt(double value, POINT anchor) noexcept {
        const auto oldOrigin = Origin();
        const double x = (anchor.x - oldOrigin.x) / zoom, y = (anchor.y - oldOrigin.y) / zoom;
        zoom = std::clamp(value, 0.05, 4.0);
        scrollX = scrollY = 0;
        const auto origin = Origin();
        scrollX = origin.x + x * zoom - anchor.x;
        scrollY = origin.y + y * zoom - anchor.y;
        Clamp();
    }
    // 适应宽度最多放大到 100%，并回到图片顶部。
    void FitWidth() noexcept {
        zoom = image.cx ? std::clamp(double(canvas.right - canvas.left) / image.cx, 0.05, 1.0) : 1.0;
        scrollX = scrollY = 0;
    }
};

}  // namespace picolet
