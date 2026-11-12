#include "hdr_capture.h"

#include "hdr_color.h"

#include <d3d11_4.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <winstring.h>

#include <algorithm>

#if __has_include(<windows.graphics.directx.direct3d11.interop.h>)
#include <windows.graphics.directx.direct3d11.interop.h>
#else
// MinGW-w64 的导入库提供该函数，部分版本需要补齐互操作声明。
STDAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice* device, IInspectable** result);
namespace Windows::Graphics::DirectX::Direct3D11 {

// 保留 SDK 接口的外部链接。具体实现来自系统 DLL，MinGW 优化构建必须通过 COM 虚表调用。
struct IDirect3DDxgiInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** result) = 0;
};

}  // namespace Windows::Graphics::DirectX::Direct3D11
#endif

namespace picolet {
namespace {

namespace directx = ABI::Windows::Graphics::DirectX;
namespace runtime_d3d = ABI::Windows::Graphics::DirectX::Direct3D11;

constexpr DWORD kFirstFrameTimeoutMs = 500;
constexpr int kFramePoolSize = 2;
constexpr auto kFloatFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
constexpr GUID kDxgiInterfaceAccessId{0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};

template <class Interface, std::size_t Length>
HRESULT GetFactory(const wchar_t (&name)[Length], ComPtr<Interface>& factory) noexcept {
    HSTRING_HEADER storage{};
    HSTRING className = nullptr;
    const auto hr = WindowsCreateStringReference(name, static_cast<UINT32>(Length - 1), &storage, &className);
    if (FAILED(hr)) return hr;
    return RoGetActivationFactory(className, IID_PPV_ARGS(factory.put()));
}

void CloseRuntimeObject(IUnknown* object) noexcept {
    if (!object) return;
    ComPtr<ABI::Windows::Foundation::IClosable> closable;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(closable.put())))) closable->Close();
}

}  // namespace

HdrCapture::RuntimeApartment::~RuntimeApartment() {
    if (initialized) RoUninitialize();
}

HRESULT HdrCapture::RuntimeApartment::Initialize() noexcept {
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier{};
    const auto current = CoGetApartmentType(&type, &qualifier);
    if (FAILED(current) && current != CO_E_NOTINITIALIZED) return current;
    // UI 线程已有 STA，后台采集线程通常还未初始化。沿用调用线程的单元，避免更改已有的 COM 模式。
    const auto mode = type == APTTYPE_STA || type == APTTYPE_MAINSTA ? RO_INIT_SINGLETHREADED : RO_INIT_MULTITHREADED;
    const auto hr = RoInitialize(mode);
    initialized = SUCCEEDED(hr);
    return hr;
}

HdrCapture::~HdrCapture() {
    StopSession();
}

HRESULT HdrCapture::Open(const DisplayCaptureProfile& display, const RECT& region, bool includeCursor) noexcept {
    includeCursor_ = includeCursor;
    auto hr = apartment_.Initialize();
    if (FAILED(hr)) return hr;

    ComPtr<graphics_capture::IGraphicsCaptureSessionStatics> support;
    hr = GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureSession", support);
    if (FAILED(hr)) return hr;
    boolean supported = false;
    hr = support->IsSupported(&supported);
    if (FAILED(hr)) return hr;
    if (!supported) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    ComPtr<IGraphicsCaptureItemInterop> interop;
    hr = GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", interop);
    if (FAILED(hr)) return hr;
    hr = interop->CreateForMonitor(display.monitor, IID_PPV_ARGS(item_.put()));
    if (FAILED(hr)) return hr;
    hr = item_->get_Size(&size_);
    if (FAILED(hr)) return hr;
    if (size_.Width != display.bounds.right - display.bounds.left ||
        size_.Height != display.bounds.bottom - display.bounds.top) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    crop_ = {static_cast<UINT>(region.left - display.bounds.left),
             static_cast<UINT>(region.top - display.bounds.top), 0,
             static_cast<UINT>(region.right - display.bounds.left),
             static_cast<UINT>(region.bottom - display.bounds.top), 1};
    whiteLevel_ = display.sdrWhiteLevel;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           nullptr, 0, D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    if (FAILED(hr)) return hr;
    // 自由线程帧池可在后台使用同一设备。保护立即上下文，避免系统提交与读回命令交错访问。
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(multithread.put())))) {
        multithread->SetMultithreadProtected(TRUE);
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    if (FAILED(hr)) return hr;
    ComPtr<IInspectable> wrappedDevice;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), wrappedDevice.put());
    if (FAILED(hr)) return hr;
    hr = wrappedDevice->QueryInterface(IID_PPV_ARGS(runtimeDevice_.put()));
    if (FAILED(hr)) return hr;
    hr = GetFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", poolFactory_);
    if (FAILED(hr)) return hr;

    // 帧池由系统按全屏尺寸分配；读回纹理只保存选区，避免滚动时反复回读整屏或重新分配。
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = crop_.right - crop_.left;
    texture.Height = crop_.bottom - crop_.top;
    texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
    texture.Format = kFloatFormat;
    texture.Usage = D3D11_USAGE_STAGING;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return device_->CreateTexture2D(&texture, nullptr, readback_.put());
}

HRESULT HdrCapture::StartSession() noexcept {
    auto hr = poolFactory_->CreateFreeThreaded(runtimeDevice_.get(), directx::DirectXPixelFormat_R16G16B16A16Float,
                                                kFramePoolSize, size_, pool_.put());
    if (FAILED(hr)) return hr;
    hr = pool_->CreateCaptureSession(item_.get(), session_.put());
    if (FAILED(hr)) return hr;
    ComPtr<graphics_capture::IGraphicsCaptureSession2> cursor;
    hr = session_->QueryInterface(IID_PPV_ARGS(cursor.put()));
    if (hr == E_NOINTERFACE) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (FAILED(hr)) return hr;
    hr = cursor->put_IsCursorCaptureEnabled(includeCursor_);
    if (FAILED(hr)) return hr;
    return session_->StartCapture();
}

void HdrCapture::StopSession() noexcept {
    CloseRuntimeObject(session_.get());
    session_.reset();
    CloseRuntimeObject(pool_.get());
    pool_.reset();
    firstFrame_ = true;
}

HRESULT HdrCapture::Read(std::span<std::uint32_t> destination, std::size_t pitch,
                         HANDLE stopEvent, bool freshSession) noexcept {
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return E_ABORT;
    if (freshSession) StopSession();
    auto hr = session_.get() ? S_OK : StartSession();
    if (SUCCEEDED(hr)) hr = ReadFrame(destination, pitch, stopEvent);
    // 短暂隐藏面板时，必须在恢复面板之前结束会话，下一次读取从新的隐藏画面开始。
    if (freshSession || FAILED(hr)) StopSession();
    return hr;
}

HRESULT HdrCapture::ReadFrame(std::span<std::uint32_t> destination, std::size_t pitch,
                              HANDLE stopEvent) noexcept {
    const auto deadline = GetTickCount64() + kFirstFrameTimeoutMs;
    ComPtr<graphics_capture::IDirect3D11CaptureFrame> frame;
    while (true) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return E_ABORT;
        const auto hr = pool_->TryGetNextFrame(frame.put());
        if (FAILED(hr)) return hr;
        if (frame.get()) break;
        if (!firstFrame_) return S_FALSE;

        // 自由线程帧池不依赖 UI 消息循环。首帧等待有上限，并能由调用方的停止事件提前唤醒。
        const auto now = GetTickCount64();
        if (now >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        const auto delay = static_cast<DWORD>(std::min<ULONGLONG>(2, deadline - now));
        if (!stopEvent) {
            Sleep(delay);
        } else {
            const auto wait = WaitForSingleObject(stopEvent, delay);
            if (wait == WAIT_OBJECT_0) return E_ABORT;
            if (wait != WAIT_TIMEOUT) return LastErrorResult();
        }
    }

    // 两帧池里如有更新的一帧，释放旧帧并采用更新的内容，避免滚动预览滞后。
    ComPtr<graphics_capture::IDirect3D11CaptureFrame> newer;
    auto hr = pool_->TryGetNextFrame(newer.put());
    if (SUCCEEDED(hr)) {
        auto* latest = newer.get() ? newer.get() : frame.get();
        hr = CopyFrame(*latest, destination, pitch);
        if (SUCCEEDED(hr)) firstFrame_ = false;
    }
    CloseRuntimeObject(newer.get());
    CloseRuntimeObject(frame.get());
    return hr;
}

HRESULT HdrCapture::CopyFrame(graphics_capture::IDirect3D11CaptureFrame& frame,
                              std::span<std::uint32_t> destination, std::size_t pitch) noexcept {
    ABI::Windows::Graphics::SizeInt32 size{};
    auto hr = frame.get_ContentSize(&size);
    if (FAILED(hr)) return hr;
    if (size.Width != size_.Width || size.Height != size_.Height) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    ComPtr<runtime_d3d::IDirect3DSurface> surface;
    hr = frame.get_Surface(surface.put());
    if (FAILED(hr)) return hr;
    ComPtr<IDirect3DDxgiInterfaceAccess> access;
    hr = surface->QueryInterface(kDxgiInterfaceAccessId, reinterpret_cast<void**>(access.put()));
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11Texture2D> texture;
    hr = access->GetInterface(IID_PPV_ARGS(texture.put()));
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != kFloatFormat || description.Width < crop_.right || description.Height < crop_.bottom) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    context_->CopySubresourceRegion(readback_.get(), 0, 0, 0, 0, texture.get(), 0, &crop_);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(readback_.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    const auto height = crop_.bottom - crop_.top;
    const auto* bytes = static_cast<const std::byte*>(mapped.pData);
    hr = ConvertHdrPixels({bytes, static_cast<std::size_t>(mapped.RowPitch) * height}, mapped.RowPitch,
                          whiteLevel_, destination, pitch, static_cast<int>(crop_.right - crop_.left),
                          static_cast<int>(height));
    context_->Unmap(readback_.get(), 0);
    return hr;
}

}  // namespace picolet
