#pragma once

#include <picolet/capture/capture_pipeline.h>
#include <picolet/desktop/hotkeys.h>
#include <picolet/desktop/paint_buffer.h>
#include <picolet/win32.h>

namespace picolet {

inline constexpr UINT kScrollAction = WM_APP + 5;
enum class ScrollAction : WPARAM { Finish, Cancel };

// 采集期间，遮罩标记选区，透明输入窗口承接选区内的鼠标移动，原窗口保持前台以便滚动。
// 低级输入钩子只在本会话存续期间安装，拦截会改动原窗口的输入，并投递采集命令。关闭时先卸载再销毁窗口。
// 后台取帧与拼接由 ScrollCapturePipeline 完成。浮窗读取只读快照刷新预览，并将完成或取消通知应用。
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
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK MouseHook(int, WPARAM, LPARAM) noexcept;
    static LRESULT CALLBACK KeyboardHook(int, WPARAM, LPARAM) noexcept;
    // 返回 true 表示截获该事件。鼠标保护选区内的输入窗口与原窗口，键盘只保护当前前台原窗口。
    bool FilterMouse(WPARAM message, const MSLLHOOKSTRUCT& event, HWND hitWindow, UINT modifiers) noexcept;
    bool FilterKey(WPARAM message, Hotkey key, bool targetForeground) noexcept;
    LRESULT Handle(UINT, WPARAM, LPARAM) noexcept;
    void Tick() noexcept;
    // 手动滚轮输入会接管自动滚动，并在新画面尚未确认时提供最新方向。
    void HandleManualWheel(SHORT delta) noexcept;
    // 输入窗口可见时放在浮窗下方，让覆盖选区的浮窗按钮也能被点击。
    void ShowInputWindow(bool visible) noexcept;
    // 将纵向滚轮发送到原窗口中光标位置对应的控件，保留触摸板产生的细粒度滚动量。
    bool ForwardWheel(POINT point, SHORT delta) noexcept;
    // Chromium 在处理滚轮时重新命中屏幕坐标。同步派发期间只开放滚轮坐标处的一个像素，派发结束后恢复输入区。
    HRESULT DispatchChromiumWheel(POINT point, SHORT delta) noexcept;
    void ToggleAutomatic() noexcept;
    // 从手动滚轮与已确认画面中选取时间最近的方向，也支持只产生画面位移的触摸板。
    ScrollDirection RecentDirection() const noexcept;
    // 等待当前滚轮指令的采集反馈，初次方向未知且连续无位移时尝试向上，已确定方向后到边界即暂停。
    void CheckAutomaticResponse(ULONGLONG now) noexcept;
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
    HWND owner_ = nullptr, panel_ = nullptr, mask_ = nullptr, inputWindow_ = nullptr, target_ = nullptr;
    HWND autoButton_ = nullptr, finishButton_ = nullptr, cancelButton_ = nullptr;
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
    ULONGLONG lastWheelAt_ = 0;
    ScrollDirection wheelDirection_ = ScrollDirection::Unknown;
    // 未知时先向下试，确认方向或改试向上后保持本轮方向，直到用户暂停。
    ScrollDirection autoDirection_ = ScrollDirection::Unknown;
    std::uint64_t commandMotion_ = 0, commandProcessed_ = 0;
    int still_ = 0;
    bool automatic_ = false, pendingAuto_ = false, pending_ = false;
    bool blocked_ = false, excluded_ = false;
    bool finishing_ = false;
    bool chromiumTarget_ = false;
    // 记住已拦截的按下事件，鼠标移出或前台切换后仍成对拦截同一次按压的抬起事件。
    bool held_[256]{};
    wchar_t status_[180]{};
};

}  // namespace picolet
