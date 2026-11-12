#pragma once

#include <picolet/capture/image.h>
#include <picolet/win32.h>

#include <d3d11.h>
// 部分 MinGW 头文件把 boolean 和 BYTE 映射为同一类型，导致 IReference 特化重复。
// 采集只使用 WinRT 接口的标量布尔值，可以跳过未使用的装箱布尔特化。
#if defined(__MINGW32__) && !defined(____FIReference_1_boolean_INTERFACE_DEFINED__)
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <windows.graphics.capture.h>

namespace picolet {

namespace graphics_capture = ABI::Windows::Graphics::Capture;

// 同一线程上持有 HDR 显示器的捕获对象、选区读回纹理与可复用帧池。
class HdrCapture {
public:
    ~HdrCapture();

    // 将物理选区换算为显示器纹理坐标并准备读回资源，首次 Read 时才启动系统会话。
    // includeCursor 固定本次会话及后续重建的帧池是否包含系统指针，默认供截图使用无指针画面。
    HRESULT Open(const DisplayCaptureProfile& display, const RECT& region, bool includeCursor = false) noexcept;
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
    bool includeCursor_ = false;
};

}  // namespace picolet
