#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace picolet {

// 分层窗口四周为阴影预留的物理像素，独立于贴图倍率和显示器 DPI。
inline constexpr LONG kPinShadowInset = 12;

// 将自顶向下、无行填充的 32 位位图转为分层窗口所需的预乘 BGRA。
// size 含四周留白，宽高都须大于两倍阴影边距，surface 须覆盖全部像素。
// 中间矩形已由 GDI 绘入图片，仅将其 Alpha 设为 255。四周按到最近图片像素的距离
// 绘制深灰阴影，从内缘的 40% 不透明度以二次曲线降至全透明，角部沿径向过渡。
inline void ApplyPinShadow(std::span<std::uint32_t> surface, SIZE size) noexcept {
    // 相同距离的像素共用预乘颜色。将曲线计算限制在宽度为 kPinShadowInset 的查找表内。
    static const auto shadow = [] {
        constexpr double kEdgeAlpha = 102.0;
        constexpr std::uint32_t kShadowGray = 96;
        std::array<std::array<std::uint32_t, kPinShadowInset + 1>, kPinShadowInset + 1> pixels{};
        for (std::size_t y = 0; y < pixels.size(); ++y) {
            for (std::size_t x = 0; x < pixels[y].size(); ++x) {
                const double distance = std::sqrt(static_cast<double>(x * x + y * y));
                const double remaining = std::clamp((kPinShadowInset - distance) / (kPinShadowInset - 1), 0.0, 1.0);
                const auto alpha = static_cast<std::uint32_t>(std::lround(kEdgeAlpha * remaining * remaining));
                const auto gray = (kShadowGray * alpha + 127u) / 255u;
                pixels[y][x] = (alpha << 24) | (gray * 0x010101u);
            }
        }
        return pixels;
    }();

    const LONG bottom = size.cy - kPinShadowInset;
    for (LONG y = 0; y < size.cy; ++y) {
        const auto row = surface.subspan(static_cast<std::size_t>(y) * size.cx, size.cx);
        const LONG dy = std::max({kPinShadowInset - y, y - bottom + 1, 0L});
        for (LONG x = 0; x < kPinShadowInset; ++x) {
            const auto pixel = shadow[dy][kPinShadowInset - x];
            row[x] = pixel;
            row[size.cx - 1 - x] = pixel;
        }
        const auto center = row.subspan(kPinShadowInset, size.cx - 2 * kPinShadowInset);
        if (dy == 0) {
            std::transform(center.begin(), center.end(), center.begin(),
                           [](std::uint32_t pixel) { return pixel | 0xff000000u; });
        } else {
            std::ranges::fill(center, shadow[dy][0]);
        }
    }
}

}  // namespace picolet
