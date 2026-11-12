#include <picolet/capture/gif.h>
#include <picolet/win32.h>

#include <shlwapi.h>
#include <wincodec.h>

#include <limits>
#include <new>

namespace picolet {
namespace {

// 与 SDK 的 GUID_MetadataFormatAPE 相同。MinGW 的导入库没有导出此 GUID。
constexpr GUID kGifApplicationExtension{0x2e043dc2, 0xc967, 0x4e05, {0x87, 0x5e, 0x61, 0x8b, 0xf6, 0x7e, 0x85, 0xc3}};

HRESULT SetInfiniteLoop(IWICImagingFactory& factory, IWICBitmapEncoder& encoder) noexcept {
    ComPtr<IWICMetadataQueryWriter> metadata, extension;
    HRESULT status = encoder.GetMetadataQueryWriter(metadata.put());
    if (FAILED(status)) return status;
    status = factory.CreateQueryWriter(kGifApplicationExtension, nullptr, extension.put());
    if (FAILED(status)) return status;

    BYTE application[]{'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
    // 子块长度、循环标记、循环次数的低/高字节和结束标记。次数为零表示无限循环。
    BYTE loop[]{3, 1, 0, 0, 0};
    PROPVARIANT value{};
    value.vt = VT_VECTOR | VT_UI1;
    value.caub = {static_cast<ULONG>(sizeof(application)), application};
    status = extension->SetMetadataByName(L"/Application", &value);
    if (FAILED(status)) return status;
    value.caub = {static_cast<ULONG>(sizeof(loop)), loop};
    status = extension->SetMetadataByName(L"/Data", &value);
    if (FAILED(status)) return status;

    value.vt = VT_UNKNOWN;
    value.punkVal = extension.get();
    return metadata->SetMetadataByName(L"/appext", &value);
}

HRESULT StreamSize(IStream& stream, std::uint64_t& size) noexcept {
    STATSTG info{};
    const auto status = stream.Stat(&info, STATFLAG_NONAME);
    if (SUCCEEDED(status)) size = info.cbSize.QuadPart;
    return status;
}

}  // namespace

struct GifEncoder::State {
    HRESULT WriteFrame(const Image& image, WORD delay) noexcept;

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    UINT width = 0, height = 0;
    std::uint64_t bytes = 0;
    HRESULT status = S_OK;
};

HRESULT GifEncoder::State::WriteFrame(const Image& image, WORD delay) noexcept {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICPalette> palette;
    ComPtr<IWICFormatConverter> indexed;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICMetadataQueryWriter> metadata;

    // GIF 每帧最多 256 色。按当前画面计算本地色表，使不同帧都可使用完整的颜色预算。
    const auto* pixels = reinterpret_cast<const BYTE*>(image.pixels().data());
    HRESULT hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                                image.stride(), image.byte_size(), const_cast<BYTE*>(pixels), bitmap.put());
    if (FAILED(hr)) return hr;
    hr = factory->CreatePalette(palette.put());
    if (FAILED(hr)) return hr;
    hr = palette->InitializeFromBitmap(bitmap.get(), 256, FALSE);
    if (FAILED(hr)) return hr;
    hr = factory->CreateFormatConverter(indexed.put());
    if (FAILED(hr)) return hr;
    hr = indexed->Initialize(bitmap.get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone,
                             palette.get(), 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    hr = encoder->CreateNewFrame(frame.put(), nullptr);
    if (FAILED(hr)) return hr;
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return hr;
    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return hr;
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return hr;
    if (!IsEqualGUID(format, GUID_WICPixelFormat8bppIndexed)) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    hr = frame->SetPalette(palette.get());
    if (FAILED(hr)) return hr;
    hr = frame->GetMetadataQueryWriter(metadata.put());
    if (FAILED(hr)) return hr;
    PROPVARIANT value{};
    value.vt = VT_UI2;
    value.uiVal = delay;
    hr = metadata->SetMetadataByName(L"/grctlext/Delay", &value);
    if (FAILED(hr)) return hr;
    hr = frame->WriteSource(indexed.get(), nullptr);
    if (FAILED(hr)) return hr;
    return frame->Commit();
}

GifEncoder::GifEncoder() noexcept = default;
GifEncoder::~GifEncoder() = default;

HRESULT GifEncoder::Open(int width, int height) noexcept {
    state_.reset();
    constexpr auto maxDimension = std::numeric_limits<WORD>::max();
    if (width <= 0 || height <= 0 || width > maxDimension || height > maxDimension) return E_INVALIDARG;
    std::unique_ptr<State> state(new (std::nothrow) State);
    if (!state) return E_OUTOFMEMORY;
    state->width = static_cast<UINT>(width);
    state->height = static_cast<UINT>(height);
    HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(state->factory.put()));
    if (FAILED(status)) return status;
    // GIF 编码器会连续写入短数据块。使用 Shell 内存流扩充缓冲，避免逐块重分配 HGLOBAL。
    *state->stream.put() = SHCreateMemStream(nullptr, 0);
    if (!state->stream.get()) return E_OUTOFMEMORY;
    status = state->factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, state->encoder.put());
    if (FAILED(status)) return status;
    status = state->encoder->Initialize(state->stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(status)) return status;
    status = SetInfiniteLoop(*state->factory.get(), *state->encoder.get());
    if (SUCCEEDED(status)) state_ = std::move(state);
    return status;
}

HRESULT GifEncoder::AppendFrame(const Image& image, UINT delayCentiseconds) noexcept {
    if (!state_) return E_UNEXPECTED;
    if (FAILED(state_->status)) return state_->status;
    if (image.empty() || static_cast<UINT>(image.width()) != state_->width ||
        static_cast<UINT>(image.height()) != state_->height || delayCentiseconds == 0 ||
        delayCentiseconds > std::numeric_limits<WORD>::max()) return E_INVALIDARG;

    state_->status = state_->WriteFrame(image, static_cast<WORD>(delayCentiseconds));
    if (SUCCEEDED(state_->status)) state_->status = StreamSize(*state_->stream.get(), state_->bytes);
    return state_->status;
}

std::uint64_t GifEncoder::encoded_size() const noexcept {
    return state_ ? state_->bytes : 0;
}

HRESULT GifEncoder::Finish(std::vector<std::uint8_t>& output) noexcept {
    if (!state_) return E_UNEXPECTED;
    if (FAILED(state_->status)) return state_->status;
    if (!state_->bytes) return E_UNEXPECTED;
    auto state = std::move(state_);
    HRESULT status = state->encoder->Commit();
    if (FAILED(status)) return status;
    std::uint64_t size = 0;
    status = StreamSize(*state->stream.get(), size);
    if (FAILED(status)) return status;
    if (!size || size > std::numeric_limits<ULONG>::max()) return STG_E_READFAULT;
    status = state->stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr);
    if (FAILED(status)) return status;

    try {
        std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
        ULONG read = 0;
        status = state->stream->Read(result.data(), static_cast<ULONG>(size), &read);
        if (FAILED(status)) return status;
        if (read != size) return STG_E_READFAULT;
        output.swap(result);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

}  // namespace picolet
