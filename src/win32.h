#pragma once

#include <windows.h>

#include <utility>

namespace picolet {

// 所有权专用于以空指针表示无效值的句柄。交给系统时调用 release，避免重复释放。
template <typename Handle, auto Close>
class UniqueHandle {
public:
    explicit UniqueHandle(Handle handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    Handle get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    Handle release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(Handle handle = nullptr) noexcept {
        if (handle_) {
            Close(handle_);
        }
        handle_ = handle;
    }

private:
    Handle handle_;
};

using KernelHandle = UniqueHandle<HANDLE, CloseHandle>;
using GlobalMemory = UniqueHandle<HGLOBAL, GlobalFree>;
using MemoryDc = UniqueHandle<HDC, DeleteDC>;
using BitmapHandle = UniqueHandle<HBITMAP, DeleteObject>;
using FontHandle = UniqueHandle<HFONT, DeleteObject>;
using BrushHandle = UniqueHandle<HBRUSH, DeleteObject>;
using MenuHandle = UniqueHandle<HMENU, DestroyMenu>;

// 让 COM 输出参数直接写入受管指针。put 会先释放此前持有的引用。
template <typename Interface>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    Interface* get() const noexcept { return value_; }
    Interface* operator->() const noexcept { return value_; }
    Interface** put() noexcept {
        reset();
        return &value_;
    }

private:
    void reset() noexcept {
        if (auto* old = std::exchange(value_, nullptr)) {
            old->Release();
        }
    }

    Interface* value_ = nullptr;
};

// 有些 Win32 API 失败时不写入错误码，仍须向调用者返回失败的 HRESULT。
inline HRESULT LastErrorResult() noexcept {
    const DWORD error = GetLastError();
    return error != ERROR_SUCCESS ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

inline int Dip(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

// 采用系统消息字体，让界面在不同显示器上保持原生控件的字形和尺寸。
inline HFONT CreateUiFont(UINT dpi) noexcept {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return nullptr;
}

}  // namespace picolet
