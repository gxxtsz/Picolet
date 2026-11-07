#include "capture_pipeline.h"

#include <algorithm>
#include <chrono>

namespace picolet {
namespace {
std::atomic<std::uint64_t> nextSession{0};

class NativeScrollSource final : public ScrollCaptureSource {
public:
    explicit NativeScrollSource(NativeScrollCaptureOptions options) noexcept
        : options_(options), permission_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    HRESULT Open(std::uint64_t session, HANDLE stop) noexcept override {
        session_ = session; stop_ = stop;
        if (options_.hideBeforeCapture && !permission_) return E_OUTOFMEMORY;
        return source_.Open(options_.area);
    }
    HRESULT Read(Image& image, CaptureSourceState& state) noexcept override {
        RECT rect{};
        if (!IsWindow(options_.target) || IsIconic(options_.target) || !GetWindowRect(options_.target, &rect) ||
            !EqualRect(&rect, &options_.targetRect) || GetDpiForWindow(options_.target) != options_.dpi) {
            state = CaptureSourceState::Blocked; return S_FALSE;
        }
        if (GetAncestor(GetForegroundWindow(), GA_ROOT) != options_.target) { state = CaptureSourceState::Paused; return S_FALSE; }
        state = CaptureSourceState::Ready;
        std::uint64_t request = 0;
        if (options_.hideBeforeCapture) {
            ResetEvent(permission_.get());
            request = waiting_.fetch_add(1) + 1;
            if (!PostMessageW(options_.coordinator, options_.prepareMessage, static_cast<WPARAM>(session_), static_cast<LPARAM>(request))) return E_ABORT;
            const HANDLE events[]{stop_, permission_.get()};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return E_ABORT;
        }
        const auto hr = source_.Read(image);
        if (options_.hideBeforeCapture)
            PostMessageW(options_.coordinator, options_.restoreMessage, static_cast<WPARAM>(session_), static_cast<LPARAM>(request));
        if (!GetWindowRect(options_.target, &rect) || !EqualRect(&rect, &options_.targetRect) ||
            GetDpiForWindow(options_.target) != options_.dpi) { state = CaptureSourceState::Blocked; return S_FALSE; }
        if (GetAncestor(GetForegroundWindow(), GA_ROOT) != options_.target) { state = CaptureSourceState::Paused; return S_FALSE; }
        return hr;
    }
    void Close() noexcept override { source_.Close(); }
    void AllowCapture(std::uint64_t request) noexcept override {
        if (waiting_.load() == request && permission_) SetEvent(permission_.get());
    }
private:
    NativeScrollCaptureOptions options_;
    RegionCaptureSource source_;
    KernelHandle permission_;
    HANDLE stop_ = nullptr;
    std::atomic<std::uint64_t> waiting_{0};
    std::uint64_t session_ = 0;
};

std::uint64_t FrameHash(const Image& image) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < image.height(); y += std::max(1, image.height() / 48))
        for (int x = 0; x < image.width(); x += std::max(1, image.width() / 48)) {
            hash ^= image.pixels()[static_cast<std::size_t>(y) * image.width() + x];
            hash *= 1099511628211ull;
        }
    return hash;
}
}

std::unique_ptr<ScrollCaptureSource> MakeNativeScrollSource(const NativeScrollCaptureOptions& options) noexcept {
    try { return std::make_unique<NativeScrollSource>(options); } catch (...) { return {}; }
}

std::uint64_t ScrollCapturePipeline::Now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

HRESULT ScrollCapturePipeline::Start(const Image& seed, Image&& annotated, std::unique_ptr<ScrollCaptureSource> source,
    SIZE preview, CapturePipelineLimits limits) noexcept {
    Cancel();
    if (!source || seed.empty() || preview.cx <= 0 || preview.cy <= 0 || !limits.maxFrames || !limits.cacheBytes ||
        (!annotated.empty() && (annotated.width() != seed.width() || annotated.height() != seed.height()))) return E_INVALIDARG;
    limits.framesPerSecond = std::clamp(limits.framesPerSecond, 1, 120);
    limits.maxFrames = std::min<std::size_t>(12, limits.maxFrames);
    limits.cacheBytes = std::min<std::uint64_t>(128ull * 1024 * 1024, limits.cacheBytes);
    capacity_ = std::max<std::size_t>(1, std::min<std::uint64_t>(limits.maxFrames, limits.cacheBytes / seed.byte_size()));
    const auto hr = tracker_.Start(seed, limits.image, std::min<std::size_t>(3, capacity_ - 1));
    if (FAILED(hr)) return hr;
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_) return LastErrorResult();
    limits_ = limits; previewSize_ = preview; frameSize_ = {seed.width(), seed.height()};
    source_ = std::move(source); annotatedSeed_ = std::move(annotated);
    session_ = ++nextSession;
    snapshot_ = {}; snapshot_.session = session_; snapshot_.width = seed.width(); snapshot_.height = seed.height();
    snapshot_.viewport = tracker_.image().viewport();
    queuedBytes_ = lastPreview_ = 0;
    producerDone_ = materialize_ = false;
    cancel_ = finish_ = stopCapture_ = false;
    direction_ = ScrollDirection::Unknown; directionTime_ = 0;
    resultError_ = E_PENDING;
    try {
        pool_.reserve(capacity_ + 3);
        processThread_ = std::thread([this] { ProcessLoop(); });
        captureThread_ = std::thread([this] { CaptureLoop(); });
        return S_OK;
    } catch (...) { Cancel(); return E_OUTOFMEMORY; }
}

void ScrollCapturePipeline::RequestFinish() noexcept {
    finish_ = true;
    {
        std::lock_guard lock(mutex_);
        if (producerDone_) { materialize_ = true; snapshot_.finished = false; }
    }
    finalizeReady_.notify_all();
}

void ScrollCapturePipeline::StopCapture() noexcept {
    stopCapture_ = true;
    if (stopEvent_) SetEvent(stopEvent_.get());
    buffersReady_.notify_all();
}

void ScrollCapturePipeline::Cancel() noexcept {
    cancel_ = true;
    if (stopEvent_) SetEvent(stopEvent_.get());
    framesReady_.notify_all(); buffersReady_.notify_all(); finalizeReady_.notify_all();
    if (captureThread_.joinable()) captureThread_.join();
    if (processThread_.joinable()) processThread_.join();
    source_.reset(); stopEvent_.reset();
    tracker_.Clear(); annotatedSeed_ = {};
    {
        std::lock_guard lock(mutex_);
        queue_.clear(); pool_.clear(); snapshot_ = {}; result_.reset();
        queuedBytes_ = 0; producerDone_ = true; resultError_ = E_PENDING;
    }
    session_ = 0;
}

void ScrollCapturePipeline::SetDirection(ScrollDirection direction) noexcept { direction_ = direction; directionTime_ = Now(); }
CapturePipelineSnapshot ScrollCapturePipeline::Snapshot() const noexcept { std::lock_guard lock(mutex_); return snapshot_; }

HRESULT ScrollCapturePipeline::Result(Image& image, SeedPlacement& placement) const noexcept {
    std::shared_ptr<const Image> result;
    SeedPlacement mapping;
    {
        std::lock_guard lock(mutex_);
        if (!snapshot_.finished) return E_PENDING;
        if (FAILED(resultError_)) return resultError_;
        result = result_; mapping = placement_;
    }
    if (!result) return E_UNEXPECTED;
    const auto hr = CopyImageRect(*result, {0, 0, result->width(), result->height()}, image);
    if (SUCCEEDED(hr)) placement = mapping;
    return hr;
}

void ScrollCapturePipeline::AllowCapture(std::uint64_t session, std::uint64_t request) noexcept {
    if (session == session_ && source_ && !cancel_) source_->AllowCapture(request);
}

void ScrollCapturePipeline::Recycle(Image&& image) noexcept {
    if (image.empty()) return;
    try {
        std::lock_guard lock(mutex_);
        if (!cancel_) pool_.push_back(std::move(image));
    } catch (...) {}
    buffersReady_.notify_one();
}

void ScrollCapturePipeline::CaptureLoop() noexcept {
    HRESULT error = S_OK;
    try {
        // 队列预算之外仅保留采集、当前确认和下一帧三个必要工作缓冲；传递所有权时不复制像素。
        for (std::size_t i = 0; i < capacity_ + 3 && !cancel_; ++i) {
            Image image;
            error = image.Resize(frameSize_.cx, frameSize_.cy);
            if (FAILED(error)) break;
            Recycle(std::move(image));
        }
        if (SUCCEEDED(error) && !cancel_) error = source_->Open(session_, stopEvent_.get());
        KernelHandle timer(CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS));
        if (!timer) timer.reset(CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
        if (!timer && SUCCEEDED(error)) error = LastErrorResult();
        const auto period = std::chrono::microseconds(1000000 / limits_.framesPerSecond);
        auto next = std::chrono::steady_clock::now();
        std::uint64_t sequence = 0, finishAt = 0, lastHash = 0;
        bool haveHash = false, gap = false;
        int still = 0;
        while (SUCCEEDED(error) && !cancel_ && !stopCapture_) {
            next = std::max(next + period, std::chrono::steady_clock::now());
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next - std::chrono::steady_clock::now()).count() / 100);
            if (!SetWaitableTimerEx(timer.get(), &due, 0, nullptr, nullptr, nullptr, 0)) { error = LastErrorResult(); break; }
            const HANDLE events[]{stopEvent_.get(), timer.get()};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            if (finish_ && !finishAt) finishAt = Now();
            if (finishAt && Now() - finishAt > 500) break;
            CapturedScrollFrame frame;
            frame.session = session_; frame.sequence = ++sequence;
            {
                std::unique_lock lock(mutex_);
                if (pool_.empty()) buffersReady_.wait_for(lock, std::chrono::milliseconds(2), [this] { return cancel_ || stopCapture_ || !pool_.empty(); });
                if (cancel_ || stopCapture_) break;
                if (pool_.empty()) { ++snapshot_.stats.samplingGaps; gap = true; continue; }
                frame.image = std::move(pool_.back()); pool_.pop_back();
            }
            CaptureSourceState state = CaptureSourceState::Ready;
            const auto captureStarted = std::chrono::steady_clock::now();
            error = source_->Read(frame.image, state);
            const auto captureCost = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - captureStarted).count());
            {
                std::lock_guard lock(mutex_);
                snapshot_.source = state;
            }
            if (error == S_FALSE) {
                Recycle(std::move(frame.image));
                error = S_OK;
                if (state == CaptureSourceState::Blocked || finish_) break;
                continue;
            }
            if (FAILED(error)) { Recycle(std::move(frame.image)); break; }
            frame.timestamp = Now(); frame.desktopTick = GetTickCount64();
            frame.direction = frame.timestamp <= directionTime_.load() + 200 ? direction_.load() : ScrollDirection::Unknown;
            frame.gapBefore = gap;
            const auto hash = FrameHash(frame.image);
            still = haveHash && hash == lastHash ? still + 1 : 0;
            lastHash = hash; haveHash = true;
            bool queued = false;
            {
                std::lock_guard lock(mutex_);
                ++snapshot_.stats.captured;
                if (!snapshot_.stats.firstCaptureTime) snapshot_.stats.firstCaptureTime = frame.timestamp;
                snapshot_.stats.lastCaptureTime = frame.timestamp;
                snapshot_.stats.captureMicroseconds = captureCost;
                // 超大选区的唯一交接帧属于必要工作缓冲，不额外分配大于缓存预算的排队空间。
                if (queue_.size() < capacity_ && (queue_.empty() || queuedBytes_ + frame.image.byte_size() <= limits_.cacheBytes)) {
                    queuedBytes_ += frame.image.byte_size(); queue_.push_back(std::move(frame)); queued = true;
                    snapshot_.stats.queued = queue_.size();
                    snapshot_.stats.peakQueued = std::max(snapshot_.stats.peakQueued, queue_.size());
                } else { ++snapshot_.stats.samplingGaps; }
            }
            if (queued) { gap = false; framesReady_.notify_one(); }
            else { gap = true; Recycle(std::move(frame.image)); }
            if (finishAt && Now() - finishAt >= 180 && still >= 1) break;
        }
    } catch (...) { error = E_OUTOFMEMORY; }
    source_->Close();  // 与 Open/Read 同线程释放 DC 和 DIB。
    {
        std::lock_guard lock(mutex_);
        if (FAILED(error) && !cancel_ && !stopCapture_) snapshot_.error = error;
        producerDone_ = true;
    }
    framesReady_.notify_all();
}

void ScrollCapturePipeline::Publish(const ScrollTrackingUpdate& update, bool force) noexcept {
    const auto now = Now();
    std::shared_ptr<Image> preview;
    const auto started = std::chrono::steady_clock::now();
    if (force || now >= lastPreview_ + 100) {
        try {
            const auto& image = tracker_.image();
            int width = previewSize_.cx;
            int height = std::max(1, static_cast<int>(static_cast<std::int64_t>(image.height()) * width / image.width()));
            if (height > previewSize_.cy) {
                height = previewSize_.cy; width = std::max(1, static_cast<int>(static_cast<std::int64_t>(image.width()) * height / image.height()));
            }
            preview = std::make_shared<Image>();
            if (FAILED(image.Preview(width, height, *preview, annotatedSeed_.empty() ? nullptr : &annotatedSeed_))) preview.reset();
            lastPreview_ = now;
        } catch (...) { preview.reset(); }
    }
    std::lock_guard lock(mutex_);
    snapshot_.tracking = update.state; snapshot_.stitchStatus = update.stitch.status; snapshot_.failure = update.stitch.failure;
    snapshot_.height = tracker_.image().height(); snapshot_.viewport = tracker_.image().viewport();
    if (update.desktopTick) snapshot_.lastFrameTick = update.desktopTick;
    snapshot_.stats.motion = update.motion;
    if (preview) {
        snapshot_.preview = std::move(preview);
        snapshot_.stats.previewMicroseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    }
    ++snapshot_.revision;
}

void ScrollCapturePipeline::Materialize() noexcept {
    HRESULT error = S_OK;
    std::shared_ptr<Image> image;
    try {
        image = std::make_shared<Image>();
        error = tracker_.image().Finish(*image);
    } catch (...) { error = E_OUTOFMEMORY; }
    std::lock_guard lock(mutex_);
    if (SUCCEEDED(error)) { result_ = std::move(image); placement_ = tracker_.image().placement(); }
    resultError_ = error; snapshot_.finished = true;
    if (FAILED(error)) snapshot_.error = error;
    ++snapshot_.revision;
}

void ScrollCapturePipeline::ProcessLoop() noexcept {
    ScrollTrackingUpdate update;
    std::uint64_t lastFullSearch = 0;
    std::vector<Image> retired;
    bool stoppedMatching = false, canFlush = true;
    try {
        retired.reserve(16);
        Publish(update, true);
        while (!cancel_) {
            CapturedScrollFrame frame;
            {
                std::unique_lock lock(mutex_);
                framesReady_.wait(lock, [this] { return cancel_ || producerDone_ || !queue_.empty(); });
                if (cancel_) return;
                if (queue_.empty() && producerDone_) break;
                frame = std::move(queue_.front()); queue_.pop_front();
                queuedBytes_ -= frame.image.byte_size(); snapshot_.stats.queued = queue_.size();
            }
            if (frame.session != session_) { Recycle(std::move(frame.image)); continue; }
            // 达到上限或发生资源错误后，只回收已排队像素，避免后续重复帧覆盖终止状态。
            if (stoppedMatching) { Recycle(std::move(frame.image)); continue; }
            const bool fullSearch = frame.timestamp >= lastFullSearch + 200;
            if (fullSearch) lastFullSearch = frame.timestamp;
            const auto started = std::chrono::steady_clock::now();
            const auto hr = tracker_.Push(std::move(frame), update, retired, fullSearch);
            const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
            for (auto& image : retired) Recycle(std::move(image));
            retired.clear();
            {
                std::lock_guard lock(mutex_);
                ++snapshot_.stats.processed; snapshot_.stats.matchMicroseconds = elapsed;
                if (FAILED(hr)) snapshot_.error = hr;
            }
            Publish(update, false);
            if (FAILED(hr) || update.state == ScrollTrackingState::Limit) {
                stoppedMatching = true; canFlush = SUCCEEDED(hr); StopCapture();
            }
        }
        if (cancel_) return;
        const auto hr = canFlush ? tracker_.Flush(update, retired) : S_OK;
        for (auto& image : retired) Recycle(std::move(image));
        retired.clear();
        if (FAILED(hr)) { std::lock_guard lock(mutex_); snapshot_.error = hr; }
        Publish(update, true);
        Materialize();
        // 保留模型支持分配失败后的完成重试，等待时不占用 CPU；取消会唤醒并退出。
        while (!cancel_) {
            std::unique_lock lock(mutex_);
            finalizeReady_.wait(lock, [this] { return cancel_ || materialize_; });
            if (cancel_) return;
            materialize_ = false; lock.unlock();
            Materialize();
        }
    } catch (...) {
        StopCapture();
        {
            std::lock_guard lock(mutex_); snapshot_.error = E_OUTOFMEMORY;
        }
        Materialize();
        while (!cancel_) {
            std::unique_lock lock(mutex_);
            finalizeReady_.wait(lock, [this] { return cancel_ || materialize_; });
            if (cancel_) return;
            materialize_ = false; lock.unlock(); Materialize();
        }
    }
}

}  // namespace picolet
