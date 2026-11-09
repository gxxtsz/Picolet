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

// 捕获单个显示器内的桌面物理矩形，不缩放、不包含鼠标指针。调用方须先排除采集界面。
// 矩形必须完整位于同一显示器内；失败时不改变 result，像素的 Alpha 始终补为不透明。
HRESULT CaptureRegion(const RECT& screenRect, Image& result) noexcept;

// 可复用的同屏区域采集器。Open、Read、Close 和析构必须在同一线程执行；DC 与 DIB 不跨线程共享。
// 内部 DIB 在 Open 时分配一次，Read 只抓取并复制到调用方拥有的缓冲，适合连续高频采集。
class RegionCaptureSource {
public:
    RegionCaptureSource() = default;
    ~RegionCaptureSource() { Close(); }
    RegionCaptureSource(const RegionCaptureSource&) = delete;
    RegionCaptureSource& operator=(const RegionCaptureSource&) = delete;

    // 绑定同一显示器内的完整物理矩形。失败释放本次创建的资源，调用方可重新 Open。
    HRESULT Open(const RECT& screenRect) noexcept;
    // 已有输出缓冲尺寸一致时直接复用；GDI 刷新完成后才读取像素，成功时 Alpha 为不透明。
    HRESULT Read(Image& buffer) noexcept;
    // 在所属线程恢复原位图并释放所有 GDI 资源，可重复调用。
    void Close() noexcept;
private:
    RECT rect_{};
    HDC screen_ = nullptr, memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    const std::uint32_t* bits_ = nullptr;
    DWORD thread_ = 0;
};

// 复制图片内的半开矩形；要求矩形完整有效。失败保留 result，可用于截取长图的独立条带。
HRESULT CopyImageRect(const Image& image, const RECT& rect, Image& result) noexcept;

// 只绘制图片的指定源矩形，避免长图在 GDI 中产生超大目标传输。源、目标均为半开矩形。
bool DrawImageSection(HDC dc, const Image& image, const RECT& source, const RECT& destination) noexcept;

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
