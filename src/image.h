#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <vector>

namespace picolet {

// 图像按从上到下的顺序存放，无行填充。每个像素为不透明 BGRA 四字节。
class Image {
public:
    Image() = default;
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    // 验证尺寸可以被 GDI 和 WIC 接收。分配失败时保留原有图像。
    HRESULT Resize(int width, int height) noexcept;
    bool empty() const noexcept { return pixels_.empty(); }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    UINT stride() const noexcept { return static_cast<UINT>(width_) * sizeof(std::uint32_t); }
    UINT byte_size() const noexcept { return static_cast<UINT>(pixels_.size() * sizeof(std::uint32_t)); }
    std::span<std::uint32_t> pixels() noexcept { return pixels_; }
    std::span<const std::uint32_t> pixels() const noexcept { return pixels_; }
    BITMAPINFO BitmapInfo(bool topDown = true) const noexcept;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint32_t> pixels_;
};

struct DesktopFrame {
    // 桌面物理坐标的外接矩形。左上角可以在主屏原点的左侧或上方。
    RECT bounds{};
    Image image;
};

// 在显示任何选区窗口之前获取各显示器的内容，空隙保留为黑色。
HRESULT CaptureDesktop(DesktopFrame& result) noexcept;

// 两个鼠标端点都包含在选区内，返回裁到桌面范围后的半开矩形。
// 鼠标起点与终点相同表示单击，返回空矩形以便重新选择。
RECT SelectionRect(POINT first, POINT last, const RECT& bounds) noexcept;

// 接收带符号的桌面坐标，输出独立、未缩放的选区图像。
HRESULT CropImage(const DesktopFrame& frame, const RECT& screenRect, Image& result) noexcept;

// 在指定矩形中等比居中摆放图片，最多按原始尺寸显示。用于贴图初始摆放，不修改原图。
RECT FitImage(const Image& image, const RECT& canvas) noexcept;

// 把完整的自顶向下位图缩放到 destination。调用方可通过设备上下文的裁剪区限制绘制范围。
// 成功传输像素时返回 true，图像或目标为空、GDI 传输失败时返回 false。
// 调用方直接访问 DIB 前须同步 GDI，用于分层合成时须自行补齐目标像素的 Alpha。
bool DrawImage(HDC dc, const Image& image, const RECT& destination) noexcept;

}  // namespace picolet
