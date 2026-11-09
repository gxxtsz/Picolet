#include <picolet/system/startup.h>

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace picolet {

HRESULT CurrentExecutablePath(std::wstring& path) noexcept {
    try {
        std::wstring buffer(256, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!length) return LastErrorResult();
            if (length < buffer.size()) {
                buffer.resize(length);
                path = std::move(buffer);
                return S_OK;
            }
            if (buffer.size() >= 32768) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            buffer.resize((std::min)(buffer.size() * 2, std::size_t{32768}));
        }
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT BuildStartupCommand(std::wstring_view executable, std::wstring& command) noexcept {
    const bool drive = executable.size() >= 3 &&
        ((executable[0] >= L'A' && executable[0] <= L'Z') || (executable[0] >= L'a' && executable[0] <= L'z')) &&
        executable[1] == L':' && (executable[2] == L'\\' || executable[2] == L'/');
    const bool unc = executable.size() > 2 && executable[0] == L'\\' && executable[1] == L'\\';
    if ((!drive && !unc) || executable.find(L'"') != executable.npos || executable.find(L'\0') != executable.npos)
        return E_INVALIDARG;
    constexpr std::wstring_view suffix = L"\" --autostart";
    if (executable.size() > 260 - 1 - suffix.size()) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    try {
        std::wstring built = L"\"";
        built.append(executable);
        built.append(suffix);
        command = std::move(built);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

bool IsAutostartCommandLine(const wchar_t* commandLine) noexcept {
    if (!commandLine) return false;
    int count = 0;
    auto** arguments = CommandLineToArgvW(commandLine, &count);
    if (!arguments) return false;
    bool found = false;
    for (int i = 1; i < count; ++i) {
        if (std::wstring_view(arguments[i]) == L"--autostart") { found = true; break; }
    }
    LocalFree(arguments);
    return found;
}

HRESULT StartupRegistration::Read(StartupValue& value) const noexcept {
    HKEY raw = nullptr;
    LSTATUS status = RegOpenKeyExW(root_, subkey_, 0, KEY_QUERY_VALUE, &raw);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) { value = {}; return S_OK; }
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    const UniqueHandle<HKEY, RegCloseKey> key(raw);
    try {
        StartupValue read;
        DWORD size = 0;
        status = RegQueryValueExW(key.get(), kStartupValue, nullptr, &read.type, nullptr, &size);
        if (status == ERROR_FILE_NOT_FOUND) { value = {}; return S_OK; }
        if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
        // 两次查询之间允许外部更新值；限制重试次数，避免设置窗口被持续写入者阻塞。
        for (int attempt = 0; attempt < 3; ++attempt) {
            read.bytes.resize(size);
            BYTE dummy = 0;
            status = RegQueryValueExW(key.get(), kStartupValue, nullptr, &read.type,
                                     size ? read.bytes.data() : &dummy, &size);
            if (status == ERROR_FILE_NOT_FOUND) { value = {}; return S_OK; }
            if (status == ERROR_MORE_DATA) continue;
            if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
            read.present = true;
            read.bytes.resize(size);
            value = std::move(read);
            return S_OK;
        }
        return HRESULT_FROM_WIN32(ERROR_RETRY);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT StartupRegistration::Restore(const StartupValue& value) const noexcept {
    if (value.bytes.size() > (std::numeric_limits<DWORD>::max)()) return E_INVALIDARG;
    HKEY raw = nullptr;
    const LSTATUS opened = value.present
        ? RegCreateKeyExW(root_, subkey_, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &raw, nullptr)
        : RegOpenKeyExW(root_, subkey_, 0, KEY_SET_VALUE, &raw);
    if (!value.present && (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND)) return S_OK;
    if (opened != ERROR_SUCCESS) return HRESULT_FROM_WIN32(opened);
    const UniqueHandle<HKEY, RegCloseKey> key(raw);
    const LSTATUS status = value.present
        ? RegSetValueExW(key.get(), kStartupValue, 0, value.type, value.bytes.data(), static_cast<DWORD>(value.bytes.size()))
        : RegDeleteValueW(key.get(), kStartupValue);
    return !value.present && status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT StartupRegistration::SetEnabled(bool enabled, std::wstring_view executable, StartupValue* previous) const noexcept {
    try {
        StartupValue desired;
        if (enabled) {
            std::wstring command;
            const HRESULT built = BuildStartupCommand(executable, command);
            if (FAILED(built)) return built;
            desired.present = true;
            desired.type = REG_SZ;
            desired.bytes.resize((command.size() + 1) * sizeof(wchar_t));
            std::memcpy(desired.bytes.data(), command.c_str(), desired.bytes.size());
        }
        StartupValue before;
        const HRESULT read = Read(before);
        if (FAILED(read)) return read;
        const bool unchanged = before == desired;
        if (previous) *previous = std::move(before);
        return unchanged ? S_FALSE : Restore(desired);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT TrayStartupRetry::Begin(HWND window, ULONGLONG now) noexcept {
    Stop();
    if (!window || !IsWindow(window)) return E_INVALIDARG;
    SetLastError(ERROR_SUCCESS);
    const UINT_PTR timer = SetTimer(window, kTimer, 1000, nullptr);
    if (!timer) return LastErrorResult();
    window_ = window;
    timer_ = timer;
    started_ = now;
    return S_OK;
}

void TrayStartupRetry::Stop() noexcept {
    if (timer_) KillTimer(window_, timer_);
    timer_ = 0;
    window_ = nullptr;
}

}  // namespace picolet
