#include <picolet/image/image.h>
#include <picolet/win32/win32.h>

#include "hdr_capture.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace picolet {
namespace {

constexpr std::uint32_t kOpaque = 0xff000000u;
constexpr auto kHdrColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

bool ContainsRect(const RECT& bounds, const RECT& rect) noexcept {
    return !IsRectEmpty(&rect) && rect.left >= bounds.left && rect.top >= bounds.top &&
           rect.right <= bounds.right && rect.bottom <= bounds.bottom;
}

bool SameDisplay(const DisplayCaptureProfile& first, const DisplayCaptureProfile& second) noexcept {
    return first.monitor == second.monitor && EqualRect(&first.bounds, &second.bounds) &&
           first.sdrWhiteLevel == second.sdrWhiteLevel;
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT rect, LPARAM data) noexcept {
    auto& displays = *reinterpret_cast<std::vector<DisplayCaptureProfile>*>(data);
    if (!IsRectEmpty(rect)) {
        try {
            displays.push_back({monitor, *rect});
        } catch (...) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }
    return TRUE;
}

// 用同一次活动路径快照匹配各 HDR 屏。显示拓扑可能在查询长度与读取之间发生变化，需要有限重试。
HRESULT ReadActiveDisplayPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths) noexcept {
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        auto error = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        if (pathCount == 0 || modeCount == 0) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        try {
            paths.resize(pathCount);
            modes.resize(modeCount);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        error = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (error == ERROR_INSUFFICIENT_BUFFER) continue;
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        paths.resize(pathCount);
        return S_OK;
    }
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

HRESULT ReadSdrWhite(const std::vector<DISPLAYCONFIG_PATH_INFO>& paths, const wchar_t* deviceName,
                     UINT32& value) noexcept {
    HRESULT result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                         path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            CompareStringOrdinal(source.viewGdiDeviceName, -1, deviceName, -1, TRUE) != CSTR_EQUAL) continue;

        // GDI 名称标识源，参考白属于目标。复制屏幕时尝试同一源的活动目标，直到取得有效值。
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white),
                        path.targetInfo.adapterId, path.targetInfo.id};
        const auto error = DisplayConfigGetDeviceInfo(&white.header);
        if (error == ERROR_SUCCESS && white.SDRWhiteLevel != 0) {
            value = white.SDRWhiteLevel;
            return S_OK;
        }
        result = HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
    }
    return result;
}

// DXGI 输出与 GDI 物理屏幕通过 HMONITOR 对齐。仅在遇到已启用 HDR 的输出时读取参考白。
HRESULT DescribeDisplays(std::span<DisplayCaptureProfile> displays) noexcept {
    ComPtr<IDXGIFactory1> factory;
    auto hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (hr == DXGI_ERROR_UNSUPPORTED) return S_OK;
    if (FAILED(hr)) return hr;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    bool havePaths = false;
    for (UINT adapterIndex = 0; ; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapterIndex, adapter.put());
        if (hr == DXGI_ERROR_NOT_FOUND) return S_OK;
        if (FAILED(hr)) return hr;
        for (UINT outputIndex = 0; ; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIndex, output.put());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) return hr;
            DXGI_OUTPUT_DESC description{};
            hr = output->GetDesc(&description);
            if (FAILED(hr)) return hr;
            const auto found = std::find_if(displays.begin(), displays.end(), [&](const auto& display) {
                return display.monitor == description.Monitor;
            });
            if (found == displays.end() || !description.AttachedToDesktop || found->sdrWhiteLevel != 0) continue;

            ComPtr<IDXGIOutput6> advanced;
            hr = output->QueryInterface(IID_PPV_ARGS(advanced.put()));
            if (hr == E_NOINTERFACE) continue;
            if (FAILED(hr)) return hr;
            DXGI_OUTPUT_DESC1 color{};
            hr = advanced->GetDesc1(&color);
            if (FAILED(hr)) return hr;
            if (color.ColorSpace != kHdrColorSpace) continue;
            if (!havePaths) {
                hr = ReadActiveDisplayPaths(paths);
                if (FAILED(hr)) return hr;
                havePaths = true;
            }
            hr = ReadSdrWhite(paths, color.DeviceName, found->sdrWhiteLevel);
            if (FAILED(hr)) return hr;
        }
    }
}

HRESULT DescribeRegion(const RECT& region, DisplayCaptureProfile& result) noexcept {
    if (IsRectEmpty(&region)) return E_INVALIDARG;
    const HMONITOR handle = MonitorFromRect(&region, MONITOR_DEFAULTTONULL);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!handle || !GetMonitorInfoW(handle, &info) || !ContainsRect(info.rcMonitor, region)) return E_INVALIDARG;
    DisplayCaptureProfile profile{handle, info.rcMonitor};
    const auto hr = DescribeDisplays({&profile, 1});
    if (SUCCEEDED(hr)) result = profile;
    return hr;
}

HRESULT CaptureSdrDisplays(DesktopFrame& frame) noexcept {
    // 多个 SDR 屏共用一个桌面大小的 DIB。没有采集内容的像素先清零，供混合桌面后续填入 HDR 画面。
    const auto releaseScreen = [](HDC dc) noexcept { ReleaseDC(nullptr, dc); };
    const UniqueHandle<HDC, releaseScreen> screen(GetDC(nullptr));
    if (!screen) return LastErrorResult();
    const MemoryDc memory(CreateCompatibleDC(screen.get()));
    if (!memory) return LastErrorResult();
    const auto info = frame.image.BitmapInfo();
    void* bits = nullptr;
    const BitmapHandle bitmap(CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bitmap || !bits) return LastErrorResult();
    auto* source = static_cast<std::uint32_t*>(bits);
    const auto pixels = frame.image.pixels();
    std::fill_n(source, pixels.size(), 0u);

    const HGDIOBJ original = SelectObject(memory.get(), bitmap.get());
    if (!original || original == HGDI_ERROR) return LastErrorResult();
    HRESULT captured = S_OK;
    for (const auto& display : frame.displays) {
        if (display.sdrWhiteLevel != 0) continue;
        const RECT& rect = display.bounds;
        SetLastError(ERROR_SUCCESS);
        if (!BitBlt(memory.get(), rect.left - frame.bounds.left, rect.top - frame.bounds.top,
                    rect.right - rect.left, rect.bottom - rect.top,
                    screen.get(), rect.left, rect.top, SRCCOPY | CAPTUREBLT)) {
            captured = LastErrorResult();
            break;
        }
    }
    // 等待批量提交的 GDI 绘制完成后才读取 DIB，并在位图销毁前恢复 DC 的原始选中对象。
    if (!GdiFlush() && SUCCEEDED(captured)) captured = E_FAIL;
    SelectObject(memory.get(), original);
    if (FAILED(captured)) return captured;
    std::transform(source, source + pixels.size(), pixels.begin(),
                   [](std::uint32_t pixel) { return pixel | kOpaque; });
    return S_OK;
}

}  // namespace

const DisplayCaptureProfile* DesktopFrame::FindDisplay(const RECT& screenRect) const noexcept {
    const auto found = std::find_if(displays.begin(), displays.end(), [&](const auto& display) {
        return ContainsRect(display.bounds, screenRect);
    });
    return found == displays.end() ? nullptr : &*found;
}

HRESULT CaptureDesktop(DesktopFrame& result) noexcept {
    DesktopFrame frame;
    SetLastError(ERROR_SUCCESS);
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&frame.displays))) {
        return LastErrorResult();
    }
    if (frame.displays.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    auto hr = DescribeDisplays(frame.displays);
    if (FAILED(hr)) return hr;
    for (const auto& display : frame.displays) UnionRect(&frame.bounds, &frame.bounds, &display.bounds);
    hr = frame.image.Resize(frame.bounds.right - frame.bounds.left, frame.bounds.bottom - frame.bounds.top);
    if (FAILED(hr)) return hr;

    if (std::any_of(frame.displays.begin(), frame.displays.end(), [](const auto& display) {
            return display.sdrWhiteLevel == 0;
        })) {
        hr = CaptureSdrDisplays(frame);
        if (FAILED(hr)) return hr;
    }
    // 转换结果直接写入虚拟桌面对应的行和列，整屏采集逐屏释放会话，所有屏成功后再移交结果。
    const auto width = static_cast<std::size_t>(frame.image.width());
    for (const auto& display : frame.displays) {
        if (display.sdrWhiteLevel == 0) continue;
        HdrCapture capture;
        hr = capture.Open(display, display.bounds);
        if (FAILED(hr)) return hr;
        const auto offset = static_cast<std::size_t>(display.bounds.top - frame.bounds.top) * width +
                            static_cast<std::size_t>(display.bounds.left - frame.bounds.left);
        hr = capture.Read(frame.image.pixels().subspan(offset), width);
        if (FAILED(hr)) return hr;
    }
    result = std::move(frame);
    return S_OK;
}

HRESULT CaptureRegion(const RECT& rect, Image& result) noexcept {
    RegionCaptureSource source;
    const auto hr = source.Open(rect);
    return hr == S_OK ? source.Read(result) : hr;
}

RegionCaptureSource::RegionCaptureSource() noexcept = default;

RegionCaptureSource::~RegionCaptureSource() {
    Close();
}

HRESULT RegionCaptureSource::Open(const RECT& rect, const DisplayCaptureProfile* seed, HANDLE stopEvent) noexcept {
    Close();
    DisplayCaptureProfile profile;
    const auto described = DescribeRegion(rect, profile);
    if (seed && described == E_INVALIDARG) return S_FALSE;
    if (FAILED(described)) return described;
    if (seed && !SameDisplay(*seed, profile)) return S_FALSE;

    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const auto bytes = static_cast<std::uint64_t>(width) * height * sizeof(std::uint32_t);
    if (bytes > std::numeric_limits<UINT>::max()) return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    display_ = profile;
    thread_ = GetCurrentThreadId();
    rect_ = rect;
    stopEvent_ = stopEvent;
    if (display_.sdrWhiteLevel != 0) {
        hdr_.reset(new (std::nothrow) HdrCapture);
        const auto hr = hdr_ ? hdr_->Open(display_, rect_) : E_OUTOFMEMORY;
        if (FAILED(hr)) Close();
        return hr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(bytes);
    SetLastError(ERROR_SUCCESS);
    screen_ = GetDC(nullptr);
    if (screen_) memory_ = CreateCompatibleDC(screen_);
    void* bits = nullptr;
    if (memory_) bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap_ && bits) previous_ = SelectObject(memory_, bitmap_);
    if (!previous_ || previous_ == HGDI_ERROR) {
        previous_ = nullptr;
        const auto error = LastErrorResult();
        Close();
        return error;
    }
    bits_ = static_cast<const std::uint32_t*>(bits);
    return S_OK;
}

HRESULT RegionCaptureSource::Read(Image& buffer, bool freshSession) noexcept {
    if (GetCurrentThreadId() != thread_) return E_UNEXPECTED;
    const int width = rect_.right - rect_.left, height = rect_.bottom - rect_.top;
    Image replacement;
    auto* output = &buffer;
    if (buffer.width() != width || buffer.height() != height) {
        const auto hr = replacement.Resize(width, height);
        if (FAILED(hr)) return hr;
        output = &replacement;
    }
    if (hdr_) {
        const auto hr = hdr_->Read(output->pixels(), width, stopEvent_, freshSession);
        if (hr != S_OK) return hr;
    } else {
        SetLastError(ERROR_SUCCESS);
        if (!BitBlt(memory_, 0, 0, width, height, screen_, rect_.left, rect_.top, SRCCOPY | CAPTUREBLT) || !GdiFlush()) {
            return LastErrorResult();
        }
        std::transform(bits_, bits_ + output->pixels().size(), output->pixels().begin(),
                       [](std::uint32_t value) { return value | kOpaque; });
    }
    if (output == &replacement) buffer = std::move(replacement);
    return S_OK;
}

HRESULT RegionCaptureSource::VerifyDisplay() const noexcept {
    if (GetCurrentThreadId() != thread_) return E_UNEXPECTED;
    DisplayCaptureProfile current;
    const auto hr = DescribeRegion(rect_, current);
    if (hr == E_INVALIDARG) return S_FALSE;
    if (FAILED(hr)) return hr;
    return SameDisplay(display_, current) ? S_OK : S_FALSE;
}

void RegionCaptureSource::Close() noexcept {
    hdr_.reset();
    if (memory_) {
        GdiFlush();
        if (previous_) SelectObject(memory_, previous_);
    }
    if (bitmap_) DeleteObject(bitmap_);
    if (memory_) DeleteDC(memory_);
    if (screen_) ReleaseDC(nullptr, screen_);
    bits_ = nullptr;
    previous_ = nullptr;
    bitmap_ = nullptr;
    memory_ = screen_ = nullptr;
    stopEvent_ = nullptr;
    thread_ = 0;
}

}  // namespace picolet
