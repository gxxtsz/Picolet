#pragma once

#include <picolet/scroll/capture_pipeline.h>
#include <picolet/system/hotkeys.h>
#include <picolet/win32/win32.h>
#include <picolet/ui/paint_buffer.h>

namespace picolet {

inline constexpr UINT kScrollAction = WM_APP + 5;
enum class ScrollAction : WPARAM { Finish, Cancel };

// 采集中的浮窗与镂空遮罩，所有状态只在应用 UI 线程访问。源窗口保持前台以接收真实滚轮。
// 低级输入钩子只在本会话存续期间安装，回调只投递消息，关闭时先卸载再销毁窗口。
class LongCaptureSession {
public:
    ~LongCaptureSession() { Close(); }
    static bool Register(HINSTANCE instance, HICON icon) noexcept;

    // 调用前必须挂起普通截图遮罩。area 为同屏、同窗口的物理选区，seed 不含标注；
    // annotatedSeed 仅供预览，成功时转移所有权。seedDisplay 在启动后台采集前复制，用于复核颜色配置。
    HRESULT Open(HINSTANCE instance, HWND owner, RECT area, const Image& seed,
                 Image&& annotatedSeed, Hotkey cancelKey, const DisplayCaptureProfile* seedDisplay = nullptr) noexcept;
    // 停止定时器、输入监听和自动滚动，唤醒等待并加入两个后台线程后释放窗口及像素；可重复调用。
    void Close() noexcept;
    // 将已可靠拼接的像素生成完整图片，placement 同时给出原标注迁移所需的三段映射。
    HRESULT Finish(Image& image, SeedPlacement& placement) const noexcept;
    // 编辑器创建或合成失败后保留已截内容，解除完成按钮的等待状态。
    void Retry(const wchar_t* message) noexcept;
    // 显示配置发生变化时停止取帧，保留原结果供完成或取消。
    void StopForDisplayChange() noexcept;
    HWND window() const noexcept { return panel_; }
    // 返回只读诊断快照，便于交互夹具检查采样速率、积压和定位失败原因，不改变会话。
    CapturePipelineSnapshot Snapshot() const noexcept { return pipeline_.Snapshot(); }

private:
    // 绘制回归测试使用固定快照验证脏区域，不启动采集线程或输入钩子。
    friend struct PaintTestAccess;
    static LRESULT CALLBACK PanelProc(HWND, UINT, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK MaskProc(HWND, UINT, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK MouseHook(int, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK KeyboardHook(int, WPARAM, LPARAM) noexcept;
    LRESULT Handle(UINT, WPARAM, LPARAM) noexcept;
    void Tick() noexcept;
    void ToggleAutomatic() noexcept;
    void Pause(const wchar_t* status) noexcept;
    void PostAction(ScrollAction action) noexcept;
    void PublishAction(ScrollAction action) noexcept;
    void Layout() noexcept;
    void Paint() noexcept;
    // 完整绘制指定 DC 中被裁剪的浮窗区域，由 Paint 负责缓冲和最终提交。
    void PaintContents(HDC dc) const noexcept;
    void UpdatePreview() noexcept;
    // 接收只读快照并按可见变化刷新；版本及采集统计独立更新，不引起界面重绘。
    void UpdateSnapshot(const CapturePipelineSnapshot& next) noexcept;
    // 以下矩形均为浮窗客户区物理坐标，绘制与失效标记共用，避免旧文字/定位框残留。
    RECT DimensionRect() const noexcept;
    RECT StatusRect() const noexcept;
    RECT PreviewPosition(const CapturePipelineSnapshot& snapshot) const noexcept;
    void Status(const wchar_t* text) noexcept;
    bool TargetForeground() const noexcept;

    inline static LongCaptureSession* active_ = nullptr;
    HWND owner_ = nullptr, panel_ = nullptr, mask_ = nullptr, target_ = nullptr;
    HWND directionUp_ = nullptr, directionDown_ = nullptr, autoButton_ = nullptr;
    HWND finishButton_ = nullptr, cancelButton_ = nullptr;
    HHOOK mouseHook_ = nullptr, keyboardHook_ = nullptr;
    FontHandle font_;
    PaintBuffer paintBuffer_;
    RECT area_{}, targetRect_{}, monitor_{}, previewRect_{};
    ScrollCapturePipeline pipeline_;
    CapturePipelineSnapshot snapshot_;
    Hotkey cancelKey_{};
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT targetDpi_ = USER_DEFAULT_SCREEN_DPI;
    ULONGLONG lastAuto_ = 0, commandAt_ = 0;
    std::uint64_t commandMotion_ = 0, commandProcessed_ = 0;
    int still_ = 0;
    bool automatic_ = false, upward_ = false, pendingAuto_ = false, pending_ = false;
    bool blocked_ = false, excluded_ = false;
    bool finishing_ = false;
    bool held_[256]{};
    wchar_t status_[180]{};
};

}  // namespace picolet
