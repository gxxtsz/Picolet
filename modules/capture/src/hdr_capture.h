#pragma once

#include <picolet/capture/image.h>
#include <picolet/win32.h>

#include <d3d11.h>
#include <windows.graphics.capture.h>

namespace picolet {

namespace graphics_capture = ABI::Windows::Graphics::Capture;

// 同一线程上持有 HDR 显示器的捕获对象、选区读回纹理与可复用帧池。
class HdrCapture {
public:
    ~HdrCapture();

    // 将物理选区换算为显示器纹理坐标并准备读回资源，首次 Read 时才启动系统会话。
    HRESULT Open(const DisplayCaptureProfile& display, const RECT& region) noexcept;
    // 直接填充目标图像中的选区。freshSession 每次创建新帧池，取帧后立即关闭会话。
    HRESULT Read(std::span<std::uint32_t> destination, std::size_t pitch,
                 HANDLE stopEvent = nullptr, bool freshSession = false) noexcept;

private:
    HRESULT StartSession() noexcept;
    void StopSession() noexcept;
    HRESULT ReadFrame(std::span<std::uint32_t> destination, std::size_t pitch, HANDLE stopEvent) noexcept;
    HRESULT CopyFrame(graphics_capture::IDirect3D11CaptureFrame& frame,
                      std::span<std::uint32_t> destination, std::size_t pitch) noexcept;

    // 声明顺序保证 COM 引用全部释放后，才撤销本采集器对所属 WinRT 线程单元的初始化。
    struct RuntimeApartment {
        ~RuntimeApartment();
        HRESULT Initialize() noexcept;
        bool initialized = false;
    } apartment_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> readback_;
    ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> runtimeDevice_;
    ComPtr<graphics_capture::IGraphicsCaptureItem> item_;
    ComPtr<graphics_capture::IDirect3D11CaptureFramePoolStatics2> poolFactory_;
    ComPtr<graphics_capture::IDirect3D11CaptureFramePool> pool_;
    ComPtr<graphics_capture::IGraphicsCaptureSession> session_;
    ABI::Windows::Graphics::SizeInt32 size_{};
    D3D11_BOX crop_{};
    UINT32 whiteLevel_ = 0;
    bool firstFrame_ = true;
};

}  // namespace picolet
