#include <picolet/desktop/export.h>

#include <picolet/capture/png_export.h>
#include <picolet/win32.h>

#include <objbase.h>
#include <shobjidl.h>

#include <cstring>
#include <cwchar>
#include <iterator>

namespace picolet {

HRESULT CopyImageToClipboard(HWND owner, const Image& image) noexcept {
    if (!owner || image.empty()) {
        return E_INVALIDARG;
    }
    const auto info = image.BitmapInfo(false);
    const auto headerSize = sizeof(info.bmiHeader);
    GlobalMemory dib(GlobalAlloc(GMEM_MOVEABLE, headerSize + image.byte_size()));
    if (!dib) {
        return E_OUTOFMEMORY;
    }
    auto* destination = static_cast<BYTE*>(GlobalLock(dib.get()));
    if (!destination) {
        return LastErrorResult();
    }

    // CF_DIB 不包含磁盘 BMP 文件头。发布从下到上的未压缩 DIB，兼容常见粘贴目标。
    std::memcpy(destination, &info.bmiHeader, headerSize);
    destination += headerSize;
    const auto* pixels = reinterpret_cast<const BYTE*>(image.pixels().data());
    const auto stride = static_cast<std::size_t>(image.stride());
    for (int row = image.height() - 1; row >= 0; --row) {
        std::memcpy(destination, pixels + static_cast<std::size_t>(row) * stride, stride);
        destination += stride;
    }
    GlobalUnlock(dib.get());

    // 先准备好数据再打开剪贴板，以减少对其他程序的占用时间。
    if (!OpenClipboard(owner)) {
        return LastErrorResult();
    }
    HRESULT status = S_OK;
    if (EmptyClipboard() && SetClipboardData(CF_DIB, dib.get())) {
        dib.release();  // 成功后全局内存归剪贴板所有。
    } else {
        status = LastErrorResult();
    }
    CloseClipboard();
    return status;
}

HRESULT SavePngWithDialog(HWND owner, const Image& image) noexcept {
    ComPtr<IFileSaveDialog> dialog;
    HRESULT status = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(dialog.put()));
    if (FAILED(status)) return status;
    FILEOPENDIALOGOPTIONS options{};
    status = dialog->GetOptions(&options);
    if (FAILED(status)) return status;
    // 由应用记录覆盖授权。若确认后其他程序才创建目标，移动操作仍会拒绝静默覆盖。
    options = (options | FOS_FORCEFILESYSTEM | FOS_STRICTFILETYPES | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR)
              & ~static_cast<DWORD>(FOS_OVERWRITEPROMPT);
    status = dialog->SetOptions(options);
    if (FAILED(status)) return status;
    const COMDLG_FILTERSPEC format{L"PNG 图片 (*.png)", L"*.png"};
    status = dialog->SetFileTypes(1, &format);
    if (FAILED(status)) return status;
    status = dialog->SetDefaultExtension(L"png");
    if (FAILED(status)) return status;
    status = dialog->SetTitle(L"另存为 PNG");
    if (FAILED(status)) return status;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[64]{};
    std::swprintf(name, std::size(name), L"Picolet_%04u-%02u-%02u_%02u-%02u-%02u.png",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                  static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
    status = dialog->SetFileName(name);
    if (FAILED(status)) return status;

    for (;;) {
        status = dialog->Show(owner);
        if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return S_FALSE;
        if (FAILED(status)) return status;

        ComPtr<IShellItem> item;
        status = dialog->GetResult(item.put());
        if (FAILED(status)) return status;
        PWSTR rawPath = nullptr;
        status = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
        const UniqueHandle<PWSTR, CoTaskMemFree> path(rawPath);
        if (FAILED(status)) return status;
        const DWORD attributes = GetFileAttributesW(path.get());
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
        if (!exists) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                return HRESULT_FROM_WIN32(error);
            }
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        } else if (MessageBoxW(owner, L"同名文件已存在。要替换它吗？", L"确认覆盖",
                               MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            continue;
        }
        // 桌面层负责取得覆盖授权，再交给 capture 完成编码和文件提交。
        // 选择路径时文件不存在，则编码期间新出现的同名文件也会由写入接口保护。
        return SavePngFile(path.get(), image, exists);
    }
}

}  // namespace picolet
