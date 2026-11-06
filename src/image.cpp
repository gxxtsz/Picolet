#include "image.h"

#include "win32.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace picolet {
namespace {

constexpr std::uint32_t kOpaque = 0xff000000u;

BOOL CALLBACK CollectMonitor(HMONITOR, HDC, LPRECT rect, LPARAM data) noexcept {
    auto& monitors = *reinterpret_cast<std::vector<RECT>*>(data);
    if (!IsRectEmpty(rect)) {
        try {
            monitors.push_back(*rect);
        } catch (...) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }
    return TRUE;
}

}  // namespace

Image::Image(Image&& other) noexcept {
    *this = std::move(other);
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        pixels_ = std::move(other.pixels_);
    }
    return *this;
}

HRESULT Image::Resize(int width, int height) noexcept {
    if (width <= 0 || height <= 0) {
        return E_INVALIDARG;
    }
    const auto count = static_cast<std::uint64_t>(width) * height;
    // WIC 的缓冲区字节数为 UINT，先验证再转换或分配，避免大尺寸溢出。
    if (count > std::numeric_limits<UINT>::max() / sizeof(std::uint32_t)) {
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    }
    try {
        std::vector<std::uint32_t> buffer(static_cast<std::size_t>(count), kOpaque);
        pixels_.swap(buffer);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    width_ = width;
    height_ = height;
    return S_OK;
}

BITMAPINFO Image::BitmapInfo(bool topDown) const noexcept {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = topDown ? -height_ : height_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = byte_size();
    return info;
}

HRESULT CaptureDesktop(DesktopFrame& result) noexcept {
    std::vector<RECT> monitors;
    SetLastError(ERROR_SUCCESS);
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors))) {
        return LastErrorResult();
    }
    if (monitors.empty()) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    DesktopFrame frame;
    for (const auto& monitor : monitors) {
        UnionRect(&frame.bounds, &frame.bounds, &monitor);
    }
    const auto width = frame.bounds.right - frame.bounds.left;
    const auto height = frame.bounds.bottom - frame.bounds.top;
    const HRESULT allocated = frame.image.Resize(width, height);
    if (FAILED(allocated)) {
        return allocated;
    }

    // 使用同一个屏幕 DC 和与它兼容的内存 DC。来源保留绝对坐标，目标减去虚拟桌面原点。
    const auto releaseScreen = [](HDC dc) noexcept { ReleaseDC(nullptr, dc); };
    const UniqueHandle<HDC, releaseScreen> screen(GetDC(nullptr));
    if (!screen) {
        return LastErrorResult();
    }
    const MemoryDc memory(CreateCompatibleDC(screen.get()));
    if (!memory) {
        return LastErrorResult();
    }
    const auto info = frame.image.BitmapInfo();
    void* bits = nullptr;
    const BitmapHandle bitmap(CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bitmap || !bits) {
        return LastErrorResult();
    }
    auto* source = static_cast<std::uint32_t*>(bits);
    std::fill_n(source, frame.image.pixels().size(), 0u);

    const HGDIOBJ original = SelectObject(memory.get(), bitmap.get());
    if (!original || original == HGDI_ERROR) {
        return LastErrorResult();
    }
    HRESULT captured = S_OK;
    for (const auto& monitor : monitors) {
        SetLastError(ERROR_SUCCESS);
        if (!BitBlt(memory.get(), monitor.left - frame.bounds.left, monitor.top - frame.bounds.top,
                    monitor.right - monitor.left, monitor.bottom - monitor.top,
                    screen.get(), monitor.left, monitor.top, SRCCOPY | CAPTUREBLT)) {
            captured = LastErrorResult();
            break;
        }
    }
    // BitBlt 可能批量提交。读取 DIB 前必须等待 GDI 完成，并恢复原有对象后再释放位图。
    if (!GdiFlush() && SUCCEEDED(captured)) {
        captured = E_FAIL;
    }
    SelectObject(memory.get(), original);
    if (FAILED(captured)) {
        return captured;
    }

    // GDI 没有定义第四个字节。显式设为不透明，避免 PNG 显示为透明或半透明。
    const auto pixels = frame.image.pixels();
    std::transform(source, source + pixels.size(), pixels.begin(),
                   [](std::uint32_t pixel) { return pixel | kOpaque; });
    result = std::move(frame);
    return S_OK;
}

RECT SelectionRect(POINT first, POINT last, const RECT& bounds) noexcept {
    if (IsRectEmpty(&bounds) || (first.x == last.x && first.y == last.y)) {
        return {};
    }
    // 先把最大端点限制在边缘以内，再加一，确保贴边时也包含最外侧像素。
    const RECT candidate{
        std::max(std::min(first.x, last.x), bounds.left),
        std::max(std::min(first.y, last.y), bounds.top),
        std::min(std::max(first.x, last.x), bounds.right - 1) + 1,
        std::min(std::max(first.y, last.y), bounds.bottom - 1) + 1,
    };
    return IsRectEmpty(&candidate) ? RECT{} : candidate;
}

HRESULT CropImage(const DesktopFrame& frame, const RECT& screenRect, Image& result) noexcept {
    RECT crop{};
    if (frame.image.empty() ||
        static_cast<std::int64_t>(frame.bounds.right) - frame.bounds.left != frame.image.width() ||
        static_cast<std::int64_t>(frame.bounds.bottom) - frame.bounds.top != frame.image.height() ||
        !IntersectRect(&crop, &screenRect, &frame.bounds)) {
        return E_INVALIDARG;
    }

    Image image;
    const HRESULT allocated = image.Resize(crop.right - crop.left, crop.bottom - crop.top);
    if (FAILED(allocated)) {
        return allocated;
    }
    const auto source = frame.image.pixels();
    const auto destination = image.pixels();
    const auto x = static_cast<std::size_t>(crop.left - frame.bounds.left);
    const auto y = static_cast<std::size_t>(crop.top - frame.bounds.top);
    const auto sourceWidth = static_cast<std::size_t>(frame.image.width());
    const auto rowSize = static_cast<std::size_t>(image.width());
    for (std::size_t row = 0; row < static_cast<std::size_t>(image.height()); ++row) {
        std::memcpy(destination.data() + row * rowSize,
                    source.data() + (y + row) * sourceWidth + x, image.stride());
    }
    result = std::move(image);
    return S_OK;
}

RECT FitImage(const Image& image, const RECT& canvas) noexcept {
    if (image.empty() || IsRectEmpty(&canvas)) {
        return {};
    }
    const auto maxWidth = canvas.right - canvas.left;
    const auto maxHeight = canvas.bottom - canvas.top;
    const auto width = static_cast<LONG>(image.width());
    const auto height = static_cast<LONG>(image.height());
    LONG fittedWidth = std::min(width, maxWidth);
    LONG fittedHeight = std::min(height, maxHeight);
    if (static_cast<std::int64_t>(fittedWidth) * height <= static_cast<std::int64_t>(fittedHeight) * width) {
        fittedHeight = std::max(1L, static_cast<LONG>(static_cast<std::int64_t>(height) * fittedWidth / width));
    } else {
        fittedWidth = std::max(1L, static_cast<LONG>(static_cast<std::int64_t>(width) * fittedHeight / height));
    }
    const LONG left = canvas.left + (maxWidth - fittedWidth) / 2;
    const LONG top = canvas.top + (maxHeight - fittedHeight) / 2;
    return {left, top, left + fittedWidth, top + fittedHeight};
}

bool DrawImage(HDC dc, const Image& image, const RECT& destination) noexcept {
    if (image.empty() || IsRectEmpty(&destination)) return false;
    const auto info = image.BitmapInfo();
    const int copied = StretchDIBits(dc, destination.left, destination.top,
                                    destination.right - destination.left, destination.bottom - destination.top,
                                    0, 0, image.width(), image.height(), image.pixels().data(),
                                    &info, DIB_RGB_COLORS, SRCCOPY);
    return copied != 0 && copied != static_cast<int>(GDI_ERROR);
}

}  // namespace picolet
