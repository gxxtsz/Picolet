#include <picolet/capture/png_export.h>

#include <picolet/win32.h>

#include <objbase.h>
#include <wincodec.h>

#include <filesystem>
#include <iterator>
#include <new>

namespace picolet {
namespace {

HRESULT EncodePng(const wchar_t* path, const Image& image) noexcept {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory.put()));
    if (FAILED(status)) return status;
    status = factory->CreateStream(stream.put());
    if (FAILED(status)) return status;
    status = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (FAILED(status)) return status;
    status = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    if (FAILED(status)) return status;
    status = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(status)) return status;
    status = encoder->CreateNewFrame(frame.put(), nullptr);
    if (FAILED(status)) return status;
    status = frame->Initialize(nullptr);
    if (FAILED(status)) return status;
    status = frame->SetSize(static_cast<UINT>(image.width()), static_cast<UINT>(image.height()));
    if (FAILED(status)) return status;

    // SetPixelFormat 可能回传替代格式。确认编码器接受 BGRA 后，才能传入现有行缓冲区。
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    status = frame->SetPixelFormat(&format);
    if (FAILED(status)) return status;
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    const auto* bytes = reinterpret_cast<const BYTE*>(image.pixels().data());
    // WritePixels 的缓冲区参数在接口中没有 const，但它只读取调用方传入的像素。
    status = frame->WritePixels(static_cast<UINT>(image.height()), image.stride(), image.byte_size(),
                                const_cast<BYTE*>(bytes));
    if (FAILED(status)) return status;
    status = frame->Commit();
    return FAILED(status) ? status : encoder->Commit();
}

struct TemporaryFile {
    const wchar_t* path;
    ~TemporaryFile() {
        if (path) {
            DeleteFileW(path);
        }
    }
};

}  // namespace

HRESULT SavePngFile(const wchar_t* path, const Image& image, bool replaceExisting) noexcept {
    if (!path || !*path || image.empty()) {
        return E_INVALIDARG;
    }
    try {
        GUID id{};
        const HRESULT created = CoCreateGuid(&id);
        if (FAILED(created)) {
            return created;
        }
        wchar_t token[39]{};
        if (StringFromGUID2(id, token, static_cast<int>(std::size(token))) == 0) {
            return E_FAIL;
        }
        // 在目标目录独占创建临时文件，关闭预留句柄后交给 WIC。
        // 清理对象在提交成功前持有临时路径，编码或移动失败时会尝试删除中间文件。
        const std::filesystem::path target(path);
        const auto temporary = target.parent_path() / (std::wstring(L".picolet-") + token + L".tmp");
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return LastErrorResult();
        }
        const auto cleanupPath = temporary.c_str();
        TemporaryFile cleanup{cleanupPath};
        CloseHandle(file);

        // 在关闭 WIC 流后再移动同目录文件，编码失败不会截断已有目标。
        const HRESULT encoded = EncodePng(cleanupPath, image);
        if (FAILED(encoded)) {
            return encoded;
        }
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0u);
        if (!MoveFileExW(cleanupPath, path, flags)) {
            return LastErrorResult();
        }
        cleanup.path = nullptr;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

}  // namespace picolet
