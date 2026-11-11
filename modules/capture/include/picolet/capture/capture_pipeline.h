#pragma once

#include <picolet/capture/scroll_tracker.h>
#include <picolet/win32.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace picolet {

// Ready 表示可采集，Paused 表示暂时失焦，Blocked 表示目标窗口或显示配置已失效。
enum class CaptureSourceState { Ready, Paused, Blocked };

// 原生和测试来源共用此接口。Open、Read、Close 均由采集线程调用；析构时已完成 Close。
// Open 返回 S_FALSE 表示首帧配置已失效。Read 返回 S_FALSE 时由 state 区分暂无更新、失焦和失效。
class ScrollCaptureSource {
public:
    virtual ~ScrollCaptureSource() = default;
    virtual HRESULT Open(std::uint64_t session, HANDLE stopEvent) noexcept = 0;
    virtual HRESULT Read(Image& buffer, CaptureSourceState& state) noexcept = 0;
    virtual void Close() noexcept = 0;
    // 仅原生浮窗隐藏回退需要：界面线程按同会话的请求编号允许一次取帧，默认无需处理。
    virtual void AllowCapture(std::uint64_t) noexcept {}
};

struct NativeScrollCaptureOptions {
    // area 为单显示器上的物理像素选区；targetRect、dpi 是会话开始时的目标几何快照。
    RECT area{}, targetRect{};
    HWND target = nullptr, coordinator = nullptr;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    // 普通截图提供的首帧配置按值传入后台，保证首帧和后续帧使用同一显示器与参考白。
    std::optional<DisplayCaptureProfile> seedDisplay;
    // 采集排除失败且浮窗相交时，后台携会话编号、请求编号投递这两个 UI 消息。
    // prepare 隐藏并刷新桌面后调用 AllowCapture，restore 在取帧后恢复浮窗。
    UINT prepareMessage = 0, restoreMessage = 0;
    bool hideBeforeCapture = false;
};

// 创建原生来源只分配常规对象，所有 GDI 与 HDR 资源均在采集线程打开和释放。
std::unique_ptr<ScrollCaptureSource> MakeNativeScrollSource(const NativeScrollCaptureOptions& options) noexcept;

struct CapturePipelineLimits {
    // 队列及可回收缓存的预算；另有必要的采集与确认工作缓冲，不改变最终图片大小上限。
    std::size_t maxFrames = 12;
    std::uint64_t cacheBytes = 128ull * 1024 * 1024;
    int framesPerSecond = 60;
    StitchLimits image;
};

struct CapturePipelineStats {
    // captured 计真实成功取帧，processed 计定位帧；间断只记录跳过采样，不覆盖已排队画面。
    std::uint64_t captured = 0, processed = 0, samplingGaps = 0, motion = 0;
    std::size_t queued = 0, peakQueued = 0;
    // 首末采集时间使用单调毫秒，可由 (captured-1)*1000/(last-first) 计算实际平均帧率。
    std::uint64_t firstCaptureTime = 0, lastCaptureTime = 0;
    // 最近一次成功取帧、定位及缩略图生成各自的耗时，单位微秒，互不混入 UI 绘制时间。
    std::uint64_t captureMicroseconds = 0;
    std::uint64_t matchMicroseconds = 0, previewMicroseconds = 0;
};

// 预览是不可变共享图片，界面持有快照时后台不会覆盖其缓冲。编号用于拒绝过期会话消息。
struct CapturePipelineSnapshot {
    std::uint64_t session = 0, revision = 0, lastFrameTick = 0;
    ScrollTrackingState tracking = ScrollTrackingState::Capturing;
    StitchStatus stitchStatus = StitchStatus::Unchanged;
    CaptureSourceState source = CaptureSourceState::Ready;
    StitchFailure failure = StitchFailure::None;
    HRESULT error = S_OK;
    int width = 0, height = 0;
    RECT viewport{};
    bool finished = false;
    std::shared_ptr<const Image> preview;
    CapturePipelineStats stats;
};

// 后台采集与拼接管线。调用方在 UI 线程使用公开接口；采集、追踪、预览各在所属线程串行执行。
// 队列同时限制帧数和额外缓存字节，正常输入不会被接管、重放或自动回滚。
class ScrollCapturePipeline {
public:
    ~ScrollCapturePipeline() { Cancel(); }
    // 复制首帧后启动两个线程；source 所有权转移给管线，失败后可再次 Start。
    HRESULT Start(const Image& seed, Image&& annotatedSeed, std::unique_ptr<ScrollCaptureSource> source,
                  SIZE previewSize, CapturePipelineLimits limits = {}) noexcept;
    // 完成请求使采集线程补取最终画面并停止，拼接线程排空队列后生成可重复读取的结果。
    void RequestFinish() noexcept;
    // 因窗口或显示变化停止来源，保留已采集队列，允许完成已有结果。
    void StopCapture() noexcept;
    // 取消会话并唤醒所有等待，等待线程释放采集资源后再释放像素及来源对象，可重复调用。
    void Cancel() noexcept;
    // 方向仅为短时提示；触摸板惯性阶段即使没有滚轮消息，也持续由实际画面判断运动。
    void SetDirection(ScrollDirection direction) noexcept;
    // 只复制少量元数据和共享指针，调用期间不做匹配或缩略图绘制。
    CapturePipelineSnapshot Snapshot() const noexcept;
    // 完成后复制不可变结果，支持创建编辑器失败后重试；E_PENDING 表示仍在排空队列。
    HRESULT Result(Image& image, SeedPlacement& placement) const noexcept;
    // 响应同一会话的浮窗隐藏回退请求，错误编号和过期会话直接忽略。
    void AllowCapture(std::uint64_t session, std::uint64_t request) noexcept;
    std::uint64_t session() const noexcept { return session_; }

private:
    void CaptureLoop() noexcept;
    void ProcessLoop() noexcept;
    void Recycle(Image&& image) noexcept;
    void Publish(const ScrollTrackingUpdate& update, bool forcePreview) noexcept;
    void Materialize() noexcept;
    static std::uint64_t Now() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable framesReady_, buffersReady_, finalizeReady_;
    std::deque<CapturedScrollFrame> queue_;
    std::vector<Image> pool_;
    std::unique_ptr<ScrollCaptureSource> source_;
    ScrollFrameTracker tracker_;
    Image annotatedSeed_;
    SIZE previewSize_{};
    SIZE frameSize_{};
    CapturePipelineLimits limits_;
    CapturePipelineSnapshot snapshot_;
    std::shared_ptr<const Image> result_;
    SeedPlacement placement_;
    std::thread captureThread_, processThread_;
    KernelHandle stopEvent_;
    std::atomic<bool> cancel_{false}, finish_{false}, stopCapture_{false};
    std::atomic<ScrollDirection> direction_{ScrollDirection::Unknown};
    std::atomic<std::uint64_t> directionTime_{0};
    std::uint64_t session_ = 0, lastPreview_ = 0, queuedBytes_ = 0;
    std::size_t capacity_ = 1;
    bool producerDone_ = false, materialize_ = false;
    HRESULT resultError_ = E_PENDING;
};

}  // namespace picolet
