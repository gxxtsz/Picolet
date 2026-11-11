#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace picolet {

// 把选区内的线性 scRGB 半精度 RGBA 转为不透明的 8 位 sRGB BGRA。
// 输入行距以字节计，输出行距以像素计。两端可有行填充，转换只写选区内像素。
HRESULT ConvertHdrPixels(std::span<const std::byte> source, std::size_t sourcePitch,
                         UINT32 sdrWhiteLevel, std::span<std::uint32_t> destination,
                         std::size_t destinationPitch, int width, int height) noexcept;

}  // namespace picolet
