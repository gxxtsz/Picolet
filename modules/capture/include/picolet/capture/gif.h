#pragma once

#include <picolet/capture/image.h>

#include <memory>

namespace picolet {

// 逐帧生成无限循环的 GIF。Open、AppendFrame、Finish 与析构必须在同一个已初始化 COM 的线程执行。
// 编码器拥有自己的输出流，仅在一次 AppendFrame 调用期间读取传入图像，不接管帧缓冲区。
class GifEncoder {
public:
    GifEncoder() noexcept;
    ~GifEncoder();
    GifEncoder(const GifEncoder&) = delete;
    GifEncoder& operator=(const GifEncoder&) = delete;

    // 开始新文件并丢弃此前的编码状态。宽高为原图像素，必须在 GIF 的 16 位尺寸范围内。
    HRESULT Open(int width, int height) noexcept;
    // 追加同尺寸的不透明 BGRA 帧。delayCentiseconds 为百分之一秒，范围为 1 到 65535。
    // 参数错误允许重新提交该帧，WIC 写入失败后须重新 Open。
    HRESULT AppendFrame(const Image& image, UINT delayCentiseconds) noexcept;
    // 返回已提交帧在流中占用的字节数，供调用方在完整帧边界检查大小上限。
    std::uint64_t encoded_size() const noexcept;
    // 提交至少一帧后才能完成。成功时移交完整 GIF 字节并释放编码状态，失败时保留 output。
    HRESULT Finish(std::vector<std::uint8_t>& output) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace picolet
