#include "hdr_color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace picolet {
namespace {

constexpr std::size_t kHalfInfinity = 0x7c00;
constexpr std::size_t kSrgbSteps = 65535;

// FP16 值和 sRGB 编码的查找表只在首次使用时生成，滚动取帧无需逐通道计算幂函数。
const auto& HalfValues() noexcept {
    static const auto values = [] {
        std::array<float, kHalfInfinity> result{};
        for (std::size_t bits = 0; bits < result.size(); ++bits) {
            const int exponent = static_cast<int>(bits >> 10);
            const auto fraction = static_cast<float>(bits & 0x3ffu);
            result[bits] = exponent == 0 ? std::ldexp(fraction, -24)
                                         : std::ldexp(1024.0f + fraction, exponent - 25);
        }
        return result;
    }();
    return values;
}

const auto& SrgbValues() noexcept {
    static const auto values = [] {
        std::array<std::uint8_t, kSrgbSteps + 1> result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            const float linear = static_cast<float>(i) / static_cast<float>(kSrgbSteps);
            const float srgb = linear <= 0.0031308f ? 12.92f * linear
                                                    : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            result[i] = static_cast<std::uint8_t>(srgb * 255.0f + 0.5f);
        }
        return result;
    }();
    return values;
}

float ToneMapScale(float peak) noexcept {
    if (peak <= 0.75f) return 1.0f;
    // 将参考白放在 0.94，腾出 SDR 高光空间。按最大通道计算共同缩放比，避免单通道截断引起偏色。
    const float mapped = peak <= 1.0f ? 0.75f + 0.76f * (peak - 0.75f) : 1.0f - 0.06f / peak;
    return mapped / peak;
}

}  // namespace

HRESULT ConvertHdrPixels(std::span<const std::byte> source, std::size_t sourcePitch,
                         UINT32 sdrWhiteLevel, std::span<std::uint32_t> destination,
                         std::size_t destinationPitch, int width, int height) noexcept {
    if (width <= 0 || height <= 0 || sdrWhiteLevel == 0) return E_INVALIDARG;
    const auto columns = static_cast<std::size_t>(width);
    const auto precedingRows = static_cast<std::size_t>(height - 1);
    const auto rowBytes = columns * 4 * sizeof(std::uint16_t);
    if (sourcePitch < rowBytes || source.size() < rowBytes ||
        precedingRows > (source.size() - rowBytes) / sourcePitch ||
        destinationPitch < columns || destination.size() < columns ||
        precedingRows > (destination.size() - columns) / destinationPitch) {
        return E_INVALIDARG;
    }

    // Windows 把 SDR 参考白表示为 80 尼特的千分比。先抵消桌面合成时的增益，再压缩 HDR 高光。
    const float referenceScale = 1000.0f / static_cast<float>(sdrWhiteLevel);
    const auto& halves = HalfValues();
    const auto& srgb = SrgbValues();
    const auto decode = [&](std::uint16_t bits) {
        // 负通道在 sRGB 色域外，非有限值无法显示，均归零后再计算颜色比例。
        return bits < halves.size() ? halves[bits] * referenceScale : 0.0f;
    };
    const auto encode = [&](float value) -> std::uint32_t {
        const auto index = static_cast<std::size_t>(std::min(value, 1.0f) * kSrgbSteps + 0.5f);
        return srgb[index];
    };
    for (std::size_t y = 0; y <= precedingRows; ++y) {
        const auto* input = source.data() + y * sourcePitch;
        auto* output = destination.data() + y * destinationPitch;
        for (std::size_t x = 0; x < columns; ++x) {
            std::uint16_t rgba[4];
            std::memcpy(rgba, input + x * sizeof(rgba), sizeof(rgba));
            const float red = decode(rgba[0]), green = decode(rgba[1]), blue = decode(rgba[2]);
            const float scale = ToneMapScale(std::max(red, std::max(green, blue)));
            output[x] = 0xff000000u | (encode(red * scale) << 16) |
                        (encode(green * scale) << 8) | encode(blue * scale);
        }
    }
    return S_OK;
}

}  // namespace picolet
