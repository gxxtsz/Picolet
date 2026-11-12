#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <string>

namespace picolet {

// 管理一次 GIF 复制的临时文件。Prepare 与 Copy 可分别在工作线程和 UI 线程执行，调用方须串行移交。
// 析构删除未发布的文件。发布后文件随剪贴板引用保留，后续由 CleanupGifClipboardFiles 清理。
class GifClipboardFile {
public:
    GifClipboardFile() = default;
    ~GifClipboardFile();
    GifClipboardFile(const GifClipboardFile&) = delete;
    GifClipboardFile& operator=(const GifClipboardFile&) = delete;

    // 将完整 GIF 写入专用临时目录。失败会移除本次部分写入的文件，可用原始字节重试。
    HRESULT Prepare(std::span<const std::uint8_t> gif) noexcept;
    // owner 必须是调用线程的有效窗口，gif 须与 Prepare 的内容一致。
    // CF_HDROP 发布成功时返回 S_OK，同时尽量提供 image/gif。剪贴板打不开时返回 ERROR_BUSY 对应的 HRESULT。
    HRESULT Copy(HWND owner, std::span<const std::uint8_t> gif) noexcept;

private:
    std::wstring path_;
    bool published_ = false;
};

// 清理专用临时目录中超过 24 小时且未被当前 CF_HDROP 引用的 Picolet GIF。
// 剪贴板被占用或无法读取文件列表时跳过本次清理。可在应用启动和成功复制后调用。
void CleanupGifClipboardFiles() noexcept;

}  // namespace picolet
