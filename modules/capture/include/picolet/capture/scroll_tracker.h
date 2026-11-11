#pragma once

#include <picolet/capture/scroll_stitcher.h>
#include <deque>

namespace picolet {

// 帧的时间为单调毫秒，序号在一次会话内递增。像素由持有此对象的线程独占，转移后不得再写。
struct CapturedScrollFrame {
    std::uint64_t session = 0, sequence = 0, timestamp = 0, desktopTick = 0;
    ScrollDirection direction = ScrollDirection::Unknown;
    bool gapBefore = false;
    Image image;
};

enum class ScrollTrackingState { Capturing, Recovering, Gap, Limit };
struct ScrollTrackingUpdate {
    ScrollTrackingState state = ScrollTrackingState::Capturing;
    StitchUpdate stitch;
    std::uint64_t sequence = 0, desktopTick = 0, motion = 0;
    // 本次更新中最后一次确认位移的画面采集时刻，与 GetTickCount64 使用同一时钟。
    std::uint64_t motionTick = 0;
};

// 单线程追踪器：可靠候选先缓存一帧，得到下一张画面的运动补偿确认后再提交新增条带。
// 暂时不能定位的帧最多保留三张，后续参考位置更新后重试；任何未连接的画面都不会直接拼入。
class ScrollFrameTracker {
public:
    // 初始化首帧及暂存上限；仅复制像素，不访问窗口。失败不改变已有会话。
    HRESULT Start(const Image& seed, StitchLimits limits = {}, std::size_t retryFrames = 3) noexcept;
    // 按采集顺序接收帧；retired 接收已处理的像素缓冲，调用方可回收到缓冲池。
    // allowFullSearch 用于低频恢复，常规连续帧只使用行指纹快速路径。
    HRESULT Push(CapturedScrollFrame&& frame, ScrollTrackingUpdate& update, std::vector<Image>& retired,
                 bool allowFullSearch = false) noexcept;
    // 结束时确认最后可靠候选，无法定位的尾帧保留为缺口状态，不猜测新增内容。
    HRESULT Flush(ScrollTrackingUpdate& update, std::vector<Image>& retired) noexcept;
    const ScrollStitcher& image() const noexcept { return stitcher_; }
    void Clear() noexcept;
private:
    struct Pending { CapturedScrollFrame frame; StitchCandidate candidate; };
    void Reject(CapturedScrollFrame&& frame, std::vector<Image>& retired);
    HRESULT Apply(Pending& pending, ScrollTrackingUpdate& update) noexcept;
    ScrollStitcher stitcher_;
    std::unique_ptr<Pending> pending_;
    std::deque<CapturedScrollFrame> retry_;
    std::size_t retryFrames_ = 3;
    std::uint64_t failureSince_ = 0, motion_ = 0;
};

}  // namespace picolet
