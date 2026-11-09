#pragma once

#include <picolet/win32/win32.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace picolet {

class BufferedPaint;

// 界面线程专用的可复用绘制缓冲，不保存业务图片，也不跨线程共享 GDI 句柄。
// 容量按客户区脏矩形扩容；位图始终以物理像素存储，不随长图的原始高度增长。
// 必须先结束使用它的 BufferedPaint，再 Reset 或销毁本对象。
class PaintBuffer {
public:
    PaintBuffer() = default;
    ~PaintBuffer() { Reset(); }
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;

    // 释放内存 DC 和 DIB。先恢复原位图，保证不删除仍被 DC 选中的 GDI 对象。
    // 允许重复调用；窗口关闭时主动调用，避免缓存一直保留到应用退出。
    void Reset() noexcept {
        if (memory_ && original_) SelectObject(memory_.get(), original_);
        bitmap_.reset();
        memory_.reset();
        original_ = nullptr;
        capacity_ = {};
    }

private:
    friend class BufferedPaint;

    // 为本次脏区域预留像素容量，成功后 DC 已选中新 DIB；失败保留上一块有效位图。
    // 原位图仅在首次选入 DIB 时记录，扩容先选入候选再释放旧图，避免选中对象泄漏。
    bool Reserve(HDC target, LONG width, LONG height) noexcept {
        if (active_ || width <= 0 || height <= 0) return false;
        if (bitmap_ && width <= capacity_.cx && height <= capacity_.cy) return true;
        width = std::max(width, capacity_.cx);
        height = std::max(height, capacity_.cy);
        const auto bytes = static_cast<std::uint64_t>(width) * height * 4;
        if (bytes > std::numeric_limits<DWORD>::max()) return false;
        if (!memory_) memory_.reset(CreateCompatibleDC(target));
        if (!memory_) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        const auto candidate = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!candidate) return false;
        const auto previous = SelectObject(memory_.get(), candidate);
        if (!previous || previous == HGDI_ERROR) { DeleteObject(candidate); return false; }
        if (!original_) original_ = previous;
        bitmap_.reset(candidate);
        capacity_ = {width, height};
        return true;
    }

    MemoryDc memory_;
    BitmapHandle bitmap_;
    HGDIOBJ original_ = nullptr;
    SIZE capacity_{};
    bool active_ = false;
};

// 一次完整的离屏绘制。bounds 与所有绘图命令均使用目标 DC 的客户区物理坐标，
// 即使脏矩形不从 (0,0) 开始，也无需修改现有绘制代码的图片、文字或标注坐标。
// 调用方必须覆盖 bounds 内所有待提交像素；析构时一次 BitBlt 提交，目标 DC 原有的
// 复杂裁剪及 WS_CLIPCHILDREN 仍然生效。对象必须在 EndPaint/释放目标 DC 之前析构。
class BufferedPaint {
public:
    BufferedPaint(PaintBuffer& buffer, HDC target, RECT bounds) noexcept
        : buffer_(buffer), target_(target), bounds_(bounds) {
        RECT clip{};
        if (!target || GetClipBox(target, &clip) == ERROR || !IntersectRect(&bounds_, &bounds, &clip)) return;
        const auto width = static_cast<std::int64_t>(bounds_.right) - bounds_.left;
        const auto height = static_cast<std::int64_t>(bounds_.bottom) - bounds_.top;
        if (width > std::numeric_limits<LONG>::max() || height > std::numeric_limits<LONG>::max() ||
            bounds_.left == std::numeric_limits<LONG>::min() || bounds_.top == std::numeric_limits<LONG>::min() ||
            !buffer.Reserve(target, static_cast<LONG>(width), static_cast<LONG>(height))) return;
        const auto dc = buffer.memory_.get();
        const int saved = SaveDC(dc);
        if (!saved) return;
        if (!SetViewportOrgEx(dc, -bounds_.left, -bounds_.top, nullptr) ||
            IntersectClipRect(dc, bounds_.left, bounds_.top, bounds_.right, bounds_.bottom) == ERROR) {
            RestoreDC(dc, saved);
            return;
        }
        saved_ = saved;
        buffer.active_ = true;
    }

    ~BufferedPaint() {
        if (!saved_) return;
        const auto dc = buffer_.memory_.get();
        BitBlt(target_, bounds_.left, bounds_.top, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top,
               dc, bounds_.left, bounds_.top, SRCCOPY);
        RestoreDC(dc, saved_);
        buffer_.active_ = false;
    }
    BufferedPaint(const BufferedPaint&) = delete;
    BufferedPaint& operator=(const BufferedPaint&) = delete;

    // 取得本次绘图 DC。分配、裁剪设置失败或缓冲正在使用时直接返回原 DC，
    // 调用方沿用相同绘图流程完成画面，不清空编辑内容，也不提交未初始化的缓冲。
    HDC dc() const noexcept { return saved_ ? buffer_.memory_.get() : target_; }

private:
    PaintBuffer& buffer_;
    HDC target_;
    RECT bounds_;
    int saved_ = 0;
};

}  // namespace picolet
