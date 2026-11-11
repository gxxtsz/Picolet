#include <picolet/capture/scroll_tracker.h>

#include <algorithm>

namespace picolet {

HRESULT ScrollFrameTracker::Start(const Image& seed, StitchLimits limits, std::size_t retries) noexcept {
    ScrollFrameTracker next;
    const auto hr = next.stitcher_.Start(seed, limits);
    if (FAILED(hr)) return hr;
    next.retryFrames_ = std::min<std::size_t>(3, retries);
    *this = std::move(next);
    return S_OK;
}

void ScrollFrameTracker::Reject(CapturedScrollFrame&& frame, std::vector<Image>& retired) {
    if (!retryFrames_) { retired.push_back(std::move(frame.image)); return; }
    retry_.push_back(std::move(frame));
    while (retry_.size() > retryFrames_) {
        retired.push_back(std::move(retry_.front().image));
        retry_.pop_front();
    }
}

HRESULT ScrollFrameTracker::Apply(Pending& pending, ScrollTrackingUpdate& update) noexcept {
    StitchUpdate change;
    const auto hr = stitcher_.Commit(pending.frame.image, pending.candidate, change);
    if (FAILED(hr)) return hr;
    if (change.status == StitchStatus::Extended || change.status == StitchStatus::Revisited) {
        update.stitch = change;
        update.motionTick = pending.frame.desktopTick;
        ++motion_;
    }
    update.motion = motion_;
    return S_OK;
}

HRESULT ScrollFrameTracker::Push(CapturedScrollFrame&& frame, ScrollTrackingUpdate& update,
    std::vector<Image>& retired, bool fullSearch) noexcept {
    update = {};
    update.sequence = frame.sequence; update.desktopTick = frame.desktopTick; update.motion = motion_;
    update.stitch.viewport = stitcher_.viewport();
    try {
        retired.reserve(retired.size() + 10);
        if (pending_) {
            if (stitcher_.Confirm(pending_->frame.image, pending_->candidate, frame.image, frame.direction)) {
                const auto hr = Apply(*pending_, update);
                if (FAILED(hr)) return hr;
                retired.push_back(std::move(pending_->frame.image));
            } else Reject(std::move(pending_->frame), retired);
            pending_.reset();
        }
        // 新的已确认参考帧可能消除旧候选的歧义；重试必须再次通过与当前画面的真实像素一致性检查。
        for (auto it = retry_.begin(); it != retry_.end();) {
            StitchCandidate candidate;
            const auto hr = stitcher_.Analyze(it->image, it->direction, candidate, false);
            if (FAILED(hr)) return hr;
            if ((candidate.status == StitchStatus::Extended || candidate.status == StitchStatus::Revisited) &&
                stitcher_.Confirm(it->image, candidate, frame.image, frame.direction)) {
                Pending recovered{std::move(*it), std::move(candidate)};
                const auto applied = Apply(recovered, update);
                if (FAILED(applied)) return applied;
                retired.push_back(std::move(recovered.frame.image));
                it = retry_.erase(it);
            } else ++it;
        }
        StitchCandidate candidate;
        const auto hr = stitcher_.Analyze(frame.image, frame.direction, candidate, fullSearch);
        if (FAILED(hr)) return hr;
        if (candidate.status == StitchStatus::Limit) {
            update.state = ScrollTrackingState::Limit;
            update.stitch.status = StitchStatus::Limit;
            retired.push_back(std::move(frame.image));
            return S_OK;
        }
        if (candidate.status == StitchStatus::Unmatched) {
            if (!failureSince_) failureSince_ = frame.timestamp ? frame.timestamp : 1;
            update.state = frame.timestamp >= failureSince_ + 500 ? ScrollTrackingState::Gap : ScrollTrackingState::Recovering;
            if (update.stitch.status == StitchStatus::Unchanged) {
                update.stitch.status = StitchStatus::Unmatched;
                update.stitch.failure = candidate.failure;
            }
            Reject(std::move(frame), retired);
        } else {
            failureSince_ = 0;
            if (candidate.status == StitchStatus::Unchanged || candidate.status == StitchStatus::Calibrating) {
                if (candidate.status == StitchStatus::Calibrating) update.stitch.status = candidate.status;
                retired.push_back(std::move(frame.image));
            }
            else pending_ = std::make_unique<Pending>(Pending{std::move(frame), std::move(candidate)});
            // 已连接的新帧覆盖了旧失败帧所在的时段，无需把未确认像素混入结果。
            if (!pending_)
                while (!retry_.empty()) { retired.push_back(std::move(retry_.front().image)); retry_.pop_front(); }
        }
        update.stitch.viewport = stitcher_.viewport(); update.motion = motion_;
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

HRESULT ScrollFrameTracker::Flush(ScrollTrackingUpdate& update, std::vector<Image>& retired) noexcept {
    try {
        retired.reserve(retired.size() + 5);
        if (pending_) {
            // 候选已通过与可靠长图的分块验证；结束时允许提交资源或超时之前最后一张明确定位的完整帧。
            const auto hr = Apply(*pending_, update);
            if (FAILED(hr)) return hr;
            retired.push_back(std::move(pending_->frame.image)); pending_.reset();
        }
        while (!retry_.empty()) { retired.push_back(std::move(retry_.front().image)); retry_.pop_front(); }
        update.stitch.viewport = stitcher_.viewport(); update.motion = motion_;
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

void ScrollFrameTracker::Clear() noexcept { *this = ScrollFrameTracker{}; }

}  // namespace picolet
