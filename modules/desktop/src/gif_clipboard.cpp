#include <picolet/desktop/gif_clipboard.h>
#include <picolet/win32.h>

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <new>
#include <string_view>
#include <vector>

namespace picolet {
namespace {

constexpr wchar_t kFilePrefix[] = L"Picolet_";
constexpr wchar_t kGifFormat[] = L"image/gif";

std::filesystem::path ClipboardDirectory() {
    return std::filesystem::absolute(std::filesystem::temp_directory_path()) / L"Picolet-GifClipboard";
}

bool IsClipboardDirectory(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsClipboardFile(std::wstring_view name) noexcept {
    constexpr std::size_t guidLength = 38;
    constexpr std::size_t suffixLength = 4;
    if (!name.starts_with(kFilePrefix) || !name.ends_with(L".gif") ||
        name.size() != std::size(kFilePrefix) - 1 + 20 + guidLength + suffixLength) return false;
    const auto token = name.substr(name.size() - guidLength - suffixLength, guidLength);
    if (token.front() != L'{' || token.back() != L'}') return false;
    wchar_t value[guidLength + 1]{};
    std::ranges::copy(token, value);
    GUID guid{};
    return SUCCEEDED(CLSIDFromString(value, &guid));
}

struct CloseClipboardOnExit {
    ~CloseClipboardOnExit() { CloseClipboard(); }
};

HGLOBAL FileDrop(const std::wstring& path) noexcept {
    // DROPFILES 后紧跟以双零结尾的绝对路径列表。预先清零可覆盖列表结束符和结构体填充。
    const auto size = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    GlobalMemory memory(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size));
    if (!memory) return nullptr;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory.get()));
    if (!drop) return nullptr;
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(drop + 1, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory.get());
    return memory.release();
}

HGLOBAL ClipboardBytes(const void* bytes, std::size_t size) noexcept {
    GlobalMemory memory(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size));
    if (!memory) return nullptr;
    void* destination = GlobalLock(memory.get());
    if (!destination) return nullptr;
    std::memcpy(destination, bytes, size);
    GlobalUnlock(memory.get());
    return memory.release();
}

}  // namespace

GifClipboardFile::~GifClipboardFile() {
    if (!published_ && !path_.empty()) DeleteFileW(path_.c_str());
}

HRESULT GifClipboardFile::Prepare(std::span<const std::uint8_t> gif) noexcept {
    if (gif.empty() || gif.size() > MAXDWORD) return E_INVALIDARG;
    if (!path_.empty()) return E_UNEXPECTED;
    try {
        const auto directory = ClipboardDirectory();
        std::filesystem::create_directory(directory);
        if (!IsClipboardDirectory(directory)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);

        GUID id{};
        const HRESULT created = CoCreateGuid(&id);
        if (FAILED(created)) return created;
        wchar_t token[39]{};
        if (!StringFromGUID2(id, token, static_cast<int>(std::size(token)))) return E_FAIL;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t name[96]{};
        if (std::swprintf(name, std::size(name), L"%ls%04u-%02u-%02u_%02u-%02u-%02u_%ls.gif", kFilePrefix,
                          static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                          static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                          static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond), token) < 0) return E_FAIL;
        std::wstring path = (directory / name).native();
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return LastErrorResult();
        const auto size = static_cast<DWORD>(gif.size());
        DWORD written = 0;
        HRESULT status = S_OK;
        if (!WriteFile(file, gif.data(), size, &written, nullptr)) status = LastErrorResult();
        else if (written != size) status = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        CloseHandle(file);
        if (FAILED(status)) {
            DeleteFileW(path.c_str());
            return status;
        }
        path_.swap(path);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT GifClipboardFile::Copy(HWND owner, std::span<const std::uint8_t> gif) noexcept {
    if (!owner || path_.empty() || gif.empty()) return E_INVALIDARG;
    GlobalMemory drop(FileDrop(path_));
    if (!drop) return E_OUTOFMEMORY;
    const UINT gifFormat = RegisterClipboardFormatW(kGifFormat);
    GlobalMemory bytes(gifFormat ? ClipboardBytes(gif.data(), gif.size()) : nullptr);
    const UINT effectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    const DWORD preferredEffect = DROPEFFECT_COPY;
    GlobalMemory effect(effectFormat ? ClipboardBytes(&preferredEffect, sizeof(preferredEffect)) : nullptr);
    // 文件及内存均在清空剪贴板前就绪。常见的占用失败可安全重试。
    if (!OpenClipboard(owner)) return HRESULT_FROM_WIN32(ERROR_BUSY);
    const CloseClipboardOnExit close;
    if (!EmptyClipboard() || !SetClipboardData(CF_HDROP, drop.get())) return LastErrorResult();
    drop.release();
    published_ = true;
    if (effect && SetClipboardData(effectFormat, effect.get())) effect.release();
    if (bytes && SetClipboardData(gifFormat, bytes.get())) bytes.release();
    return S_OK;
}

void CleanupGifClipboardFiles() noexcept {
    try {
        const auto directory = ClipboardDirectory();
        if (!IsClipboardDirectory(directory)) return;
        const auto expires = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        std::vector<std::filesystem::path> expired;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            std::error_code error;
            if (entry.symlink_status(error).type() != std::filesystem::file_type::regular || error ||
                !IsClipboardFile(entry.path().filename().native())) continue;
            const auto written = entry.last_write_time(error);
            if (!error && written < expires) expired.push_back(entry.path());
        }
        if (expired.empty() || !OpenClipboard(nullptr)) return;
        const CloseClipboardOnExit close;
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
            if (!drop) return;
            const UINT count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
            if (!count) return;
            for (UINT index = 0; index < count; ++index) {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                if (!length) return;
                std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                if (DragQueryFileW(drop, index, path.data(), length + 1) != length) return;
                std::erase_if(expired, [&path](const auto& file) {
                    return CompareStringOrdinal(file.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
                });
            }
        }
        // 检查引用与删除使用同一次剪贴板锁，防止在两者之间被另一个应用复制。
        for (const auto& path : expired) DeleteFileW(path.c_str());
    } catch (...) {
        // 临时目录不可用时保留文件，后续启动或复制时仍可清理。
    }
}

}  // namespace picolet
