#include <picolet/scroll/scroll_stitcher.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace picolet {

using ScrollRow = std::array<BYTE, 96>;
struct ScrollFeatures {
    struct Positions { std::array<int, 8> rows{}; int count = 0; };
    std::vector<ScrollRow> rows;
    std::unordered_map<std::uint64_t, Positions> index;
};

namespace {
double Difference(const ScrollRow& first, const ScrollRow& second) noexcept {
    std::array<int, 4> largest{};
    int total = 0;
    for (int x = 0; x < 32; ++x) {
        int error = 0;
        for (int c = 0; c < 3; ++c) error += std::abs(int(first[x * 3 + c]) - second[x * 3 + c]);
        total += error;
        if (error > largest[0]) {
            largest[0] = error;
            for (int i = 0; i < 3 && largest[i] > largest[i + 1]; ++i) std::swap(largest[i], largest[i + 1]);
        }
    }
    int outliers = 0;
    for (const int error : largest) outliers += error;
    return (total - outliers * 0.7) / (29.2 * 3);
}

bool Informative(const ScrollRow& row) noexcept {
    const auto [low, high] = std::minmax_element(row.begin(), row.end());
    return *high - *low >= 8;
}

int Luminance(const ScrollRow& row, int x) noexcept {
    return (row[x * 3] * 29 + row[x * 3 + 1] * 150 + row[x * 3 + 2] * 77) >> 8;
}

std::uint64_t Fingerprint(const ScrollRow& row, int kind) noexcept {
    std::uint64_t hash = 1469598103934665603ull ^ static_cast<unsigned>(kind);
    for (int x = kind == 2 ? 3 : 0; x < 32; x += kind == 2 ? 4 : 1)
        for (int c = 0; c < 3; ++c) {
            hash ^= kind == 0 ? row[x * 3 + c] : row[x * 3 + c] >> 4;
            hash *= 1099511628211ull;
        }
    return hash;
}

int PixelError(std::uint32_t a, std::uint32_t b) noexcept {
    return (std::abs(int(a & 255) - int(b & 255)) +
        std::abs(int((a >> 8) & 255) - int((b >> 8) & 255)) +
        std::abs(int((a >> 16) & 255) - int((b >> 16) & 255))) / 3;
}

// 以纵向四带和横向四带独立校验实际像素。允许小面积动态内容，但拒绝不同区域发生不同位移。
template <typename ReferenceRow>
bool CheckPixels(const Image& image, int first, int last, ReferenceRow reference, double& quality) noexcept {
    if (last - first < 24) return false;
    std::array<int, 4> vertical{}, verticalGood{}, horizontal{}, horizontalGood{};
    int total = 0, good = 0;
    int foreground = 0, foregroundGood = 0;
    const int step = std::max(1, (last - first) / 96);
    for (int y = first; y < last; y += step) {
        const auto* row = reference(y);
        if (!row) return false;
        const auto* current = image.pixels().data() + static_cast<std::size_t>(y) * image.width();
        const int band = std::min(3, (y - first) * 4 / (last - first));
        for (int column = 0; column < 32; ++column) {
            const int x = 2 + (image.width() - 5) * column / 31;
            const bool matches = PixelError(current[x], row[x]) <= 12;
            ++total; good += matches;
            // 文字页面的大部分像素是背景；额外核对高对比内容，防止浅色背景掩盖字形错位。
            // 低对比卡片底色不算文字锚点，小面积动画仍可由一致性比例容忍。
            if (PixelError(current[x], current[2]) >= 24 || PixelError(row[x], row[2]) >= 24) {
                ++foreground; foregroundGood += matches;
            }
            ++vertical[band]; verticalGood[band] += matches;
            ++horizontal[column / 8]; horizontalGood[column / 8] += matches;
        }
    }
    quality = total ? double(good) / total : 0;
    const double required = last - first < image.height() / 4 ? 0.96 : 0.90;
    if (quality < required) return false;
    if (foreground >= 16 && foregroundGood < foreground * (required > 0.9 ? 0.93 : 0.85)) return false;
    int reliableColumns = 0;
    for (int band = 0; band < 4; ++band) {
        if (vertical[band] && verticalGood[band] < vertical[band] * 0.84) return false;
        reliableColumns += horizontal[band] && horizontalGood[band] >= horizontal[band] * 0.86;
    }
    return reliableColumns >= 3;
}

bool SameFeatures(const ScrollFeatures& a, const ScrollFeatures& b) noexcept {
    if (a.rows.size() != b.rows.size()) return false;
    std::size_t changed = 0;
    // 重复正文可能仅行号不同，不能用较大的平均色差把整行位移误认为静止。
    for (std::size_t y = 0; y < a.rows.size(); ++y) changed += Difference(a.rows[y], b.rows[y]) > 0.15;
    return changed <= std::max<std::size_t>(1, a.rows.size() / 100);
}
}

std::shared_ptr<const ScrollFeatures> ScrollStitcher::Describe(const Image& image) {
    auto features = std::make_shared<ScrollFeatures>();
    features->rows.resize(image.height());
    features->index.reserve(static_cast<std::size_t>(image.height()) * 3);
    const int inset = image.width() >= 64 ? 8 : 1;
    for (int y = 0; y < image.height(); ++y) {
        auto& row = features->rows[y];
        for (int x = 0; x < 32; ++x) {
            const int px = inset + (image.width() - 2 * inset - 1) * x / 31;
            const auto pixel = image.pixels()[static_cast<std::size_t>(y) * image.width() + px];
            row[x * 3] = static_cast<BYTE>(pixel);
            row[x * 3 + 1] = static_cast<BYTE>(pixel >> 8);
            row[x * 3 + 2] = static_cast<BYTE>(pixel >> 16);
        }
        if (!Informative(row)) continue;
        for (int kind = 0; kind < 3; ++kind) {
            auto& bucket = features->index[Fingerprint(row, kind)];
            if (bucket.count < 8) bucket.rows[bucket.count] = y;
            bucket.count = std::min(9, bucket.count + 1);
        }
    }
    return features;
}

HRESULT ScrollStitcher::Start(const Image& seed, StitchLimits limits) noexcept {
    if (seed.width() < 32 || seed.height() < 64 || limits.maxHeight < seed.height() || limits.maxBytes < seed.byte_size())
        return E_INVALIDARG;
    ScrollStitcher next;
    next.limits_ = limits;
    auto hr = CopyImageRect(seed, {0, 0, seed.width(), seed.height()}, next.seed_);
    if (FAILED(hr)) return hr;
    Strip initial{0, {}};
    hr = CopyImageRect(seed, {0, 0, seed.width(), seed.height()}, initial.image);
    if (FAILED(hr)) return hr;
    try {
        next.previous_ = Describe(seed);
        next.recent_.reserve(6);
        next.strips_.push_back(std::move(initial));
        next.order_.push_back(0);
    } catch (...) { return E_OUTOFMEMORY; }
    next.maximum_ = seed.height();
    next.revision_ = revision_ + 1;
    *this = std::move(next);
    return S_OK;
}

ScrollStitcher::Match ScrollStitcher::Locate(const ScrollFeatures& previous, const ScrollFeatures& next,
    int top, int bottom, ScrollDirection direction, bool fullSearch) {
    const int length = bottom - top, minimumOverlap = std::max(24, (length * 8 + 99) / 100);
    if (length <= minimumOverlap) return {};
    const int range = length - minimumOverlap;
    std::vector<int> votes(static_cast<std::size_t>(range) * 2 + 1);
    for (int y = top; y < bottom; ++y) {
        if (!Informative(next.rows[y])) continue;
        for (int kind = 0; kind < 3; ++kind) {
            const auto found = previous.index.find(Fingerprint(next.rows[y], kind));
            if (found == previous.index.end() || found->second.count > 8) continue;
            for (int i = 0; i < found->second.count; ++i) {
                const int row = found->second.rows[i], delta = row - y;
                if (row >= top && row < bottom && std::abs(delta) <= range)
                    votes[delta + range] += kind == 0 ? 4 : kind == 1 ? 2 : 1;
            }
        }
    }
    std::array<std::pair<int, int>, 12> candidates{};
    candidates.fill({0, 0});
    for (int delta = -range; delta <= range; ++delta)
        if (votes[delta + range] > candidates.back().first) {
            candidates.back() = {votes[delta + range], delta};
            std::sort(candidates.begin(), candidates.end(), std::greater<>());
        }
    const auto score = [&](int delta, bool coarse = false) {
        const int first = std::max(top, top - delta), last = std::min(bottom, bottom - delta);
        const int step = std::max(1, (last - first) / 96);
        double sum = 0;
        int active = 0, good = 0;
        for (int y = first; y < last; y += step) {
            const auto& a = previous.rows[y + delta];
            const auto& b = next.rows[y];
            if (!Informative(a) && !Informative(b) && !(y + 1 < last && Difference(b, next.rows[y + 1]) > 3)) continue;
            double error = Difference(a, b);
            if (coarse)
                for (int offset = -2; offset <= 2; ++offset) {
                    const int row = y + delta + offset;
                    if (row >= top && row < bottom) error = std::min(error, Difference(previous.rows[row], b));
                }
            sum += std::min(45.0, error);
            ++active; good += error <= 7;
        }
        if (active < std::min(8, std::max(4, (last - first) / step / 4)) || good < active * 0.78) return 1e9;
        return sum / active;
    };
    // 每个位移只计分一次，第二候选用于判断重复文本的歧义，滚轮方向不能强行解除歧义。
    std::vector<std::pair<double, int>> scores;
    scores.reserve(128);
    const auto consider = [&](int delta) {
        if (std::abs(delta) > range || std::any_of(scores.begin(), scores.end(),
            [delta](const auto& item) { return item.second == delta; })) return;
        scores.emplace_back(score(delta), delta);
    };
    for (const auto& candidate : candidates)
        if (candidate.first >= 4)
            for (int delta = candidate.second - 1; delta <= candidate.second + 1; ++delta) consider(delta);
    const auto select = [&] {
        Match match;
        for (const auto& [error, delta] : scores) {
            const bool preferred = (direction == ScrollDirection::Down && delta > 0) || (direction == ScrollDirection::Up && delta < 0);
            const bool oldPreferred = (direction == ScrollDirection::Down && match.delta > 0) || (direction == ScrollDirection::Up && match.delta < 0);
            if (error < match.error || (error == match.error && preferred && !oldPreferred)) {
                match.error = error; match.delta = delta;
            }
        }
        for (const auto& [error, delta] : scores)
            if (std::abs(delta - match.delta) > 2) match.alternative = std::min(match.alternative, error);
        match.overlap = length - std::abs(match.delta);
        match.valid = match.error <= 6 && ((match.error < 0.3 && match.alternative > match.error + 0.025) ||
            match.alternative >= match.error + 0.8 || match.alternative > 8);
        if (match.overlap < length / 4) match.valid &= match.error <= 2;
        return match;
    };
    auto match = select();
    if (match.valid || !fullSearch) return match;
    std::array<std::pair<double, int>, 8> fallback{};
    fallback.fill({1e9, 0});
    for (int delta = -range; delta <= range; delta += 4) {
        const double error = score(delta, true);
        if (error < fallback.back().first) {
            fallback.back() = {error, delta}; std::sort(fallback.begin(), fallback.end());
        }
    }
    for (int delta = -8; delta <= 8; ++delta) consider(delta);
    for (const auto& candidate : fallback)
        for (int delta = candidate.second - 3; delta <= candidate.second + 3; ++delta) consider(delta);
    return select();
}

const std::uint32_t* ScrollStitcher::BodyRow(int logical) const noexcept {
    const auto found = std::upper_bound(order_.begin(), order_.end(), logical,
        [&](int y, std::size_t index) { return y < strips_[index].top; });
    if (found == order_.begin()) return nullptr;
    const auto& strip = strips_[*std::prev(found)];
    if (logical < strip.top || logical >= strip.top + strip.image.height()) return nullptr;
    return strip.image.pixels().data() + static_cast<std::size_t>(logical - strip.top) * width();
}

bool ScrollStitcher::Validate(const Image& frame, int position, int top, int bottom, double& quality) const noexcept {
    const int body = frame.height() - top - bottom;
    const int maximum = calibrated_ ? maximum_ : body;
    const int first = top + std::max(0, minimum_ - position), last = top + std::min(body, maximum - position);
    return CheckPixels(frame, first, last, [&](int y) -> const std::uint32_t* {
        const int logical = position + y - top;
        return calibrated_ ? BodyRow(logical) : seed_.pixels().data() + static_cast<std::size_t>(logical + top) * width();
    }, quality);
}

HRESULT ScrollStitcher::Analyze(const Image& frame, ScrollDirection direction, StitchCandidate& result, bool fullSearch) const noexcept {
    result = {};
    if (seed_.empty() || frame.width() != width() || frame.height() != seed_.height()) return E_INVALIDARG;
    try {
        StitchCandidate candidate;
        candidate.features_ = Describe(frame);
        candidate.basis_ = previous_;
        candidate.revision_ = revision_;
        candidate.position = position_;
        candidate.fixedTop = top_; candidate.fixedBottom = bottom_;
        const auto& next = *candidate.features_;
        const auto& previous = *previous_;
        if (SameFeatures(previous, next)) {
            candidate.status = StitchStatus::Unchanged; candidate.failure = StitchFailure::None; candidate.confidence = 1;
            result = std::move(candidate); return S_OK;
        }
        int leading = 0, trailing = 0;
        if (!calibrated_) {
            // 固定区的边界使用整行像素确认，避免采样列恰好错过变化的行号，
            // 把相同段落中的一小段正文误收入固定头尾。该扫描仅在首次校准时执行。
            const auto stationary = [&](int y) {
                const auto offset = static_cast<std::size_t>(y) * width();
                return Difference(previous.rows[y], next.rows[y]) <= 0.02 &&
                    std::memcmp(seed_.pixels().data() + offset, frame.pixels().data() + offset, seed_.stride()) == 0;
            };
            while (leading < frame.height() / 3 && stationary(leading)) ++leading;
            while (trailing < frame.height() / 3 && stationary(frame.height() - trailing - 1)) ++trailing;
        }
        auto match = Locate(previous, next, calibrated_ ? top_ : leading,
            frame.height() - (calibrated_ ? bottom_ : trailing), direction, false);
        int referencePosition = position_;
        if (!match.valid && calibrated_) {
            for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
                const auto alternative = Locate(*it->features, next, top_, frame.height() - bottom_, direction, false);
                if (alternative.valid && (!match.valid || alternative.error < match.error)) {
                    match = alternative; referencePosition = it->position;
                }
            }
        }
        if (!match.valid && fullSearch) {
            match = Locate(previous, next, calibrated_ ? top_ : leading,
                frame.height() - (calibrated_ ? bottom_ : trailing), direction, true);
            referencePosition = position_;
        }
        if (!match.valid) {
            candidate.failure = match.error < 1e8 ? StitchFailure::Ambiguous : StitchFailure::NoOverlap;
            result = std::move(candidate); return S_OK;
        }
        if (!calibrated_ && std::abs(match.delta) < 24) {
            // 初始几像素的位移不足以区分正文空白、重复字形与固定栏。
            // 保留首帧，累计到可判断的位移后再一次性追加，运动中无需停顿。
            candidate.status = StitchStatus::Calibrating; candidate.failure = StitchFailure::None;
            candidate.displacement = candidate.position = match.delta; candidate.overlap = match.overlap;
            result = std::move(candidate); return S_OK;
        }
        if (!calibrated_) {
            std::array<int, 32> histogram{};
            for (int y = frame.height() / 3; y < frame.height() * 2 / 3; ++y)
                for (const int column : {0, 1, 30, 31}) ++histogram[Luminance(next.rows[y], column) / 8];
            const int background = static_cast<int>(std::max_element(histogram.begin(), histogram.end()) - histogram.begin()) * 8 + 4;
            const auto evidence = [&](int y) {
                int average = 0;
                for (int x = 0; x < 32; ++x) average += Luminance(next.rows[y], x);
                if (!Informative(next.rows[y]) && std::abs(average / 32 - background) <= 6) return false;
                // 逐行检查，避免偶数行高的重复段落与隔行采样发生相位混叠。
                for (int row = frame.height() / 3; row < frame.height() * 2 / 3; ++row)
                    if (Difference(next.rows[y], next.rows[row]) <= 0.02) return false;
                const int moved = y + match.delta, reverse = y - match.delta;
                // 新露出的正文恰好与旧行相同，且另一侧落进固定栏时，不能据此认作固定区。
                // 已知正文参照吻合会否决证据；差异参照仍允许在边缘空白中，以支持首个小位移。
                bool different = false;
                for (const auto& [row, forward] : {std::pair{moved, true}, std::pair{reverse, false}}) {
                    if (row < 0 || row >= frame.height()) continue;
                    const double error = forward ? Difference(previous.rows[row], next.rows[y])
                                                 : Difference(previous.rows[y], next.rows[row]);
                    if (error <= 0.02 && row >= leading && row < frame.height() - trailing) return false;
                    different |= error > 10;
                }
                return different;
            };
            int headerEdge = -1, footerEdge = -1;
            for (int y = 0; y < leading; ++y) if (evidence(y)) headerEdge = y;
            for (int y = frame.height() - trailing; y < frame.height(); ++y) if (evidence(y)) { footerEdge = y; break; }
            candidate.fixedTop = leading >= 3 && headerEdge >= 0 ? headerEdge + 1 : 0;
            candidate.fixedBottom = trailing >= 3 && footerEdge >= 0 ? frame.height() - footerEdge : 0;
            match = Locate(previous, next, candidate.fixedTop, frame.height() - candidate.fixedBottom, direction, fullSearch);
            if (!match.valid) { result = std::move(candidate); return S_OK; }
        } else {
            int unstable = 0;
            for (int y = 0; y < top_; ++y) unstable += Difference(previous.rows[y], next.rows[y]) > 8;
            for (int y = frame.height() - bottom_; y < frame.height(); ++y) unstable += Difference(previous.rows[y], next.rows[y]) > 8;
            if (unstable > std::max(3, (top_ + bottom_) / 8)) {
                candidate.failure = StitchFailure::FixedChanged; result = std::move(candidate); return S_OK;
            }
        }
        const auto position = static_cast<std::int64_t>(referencePosition) + match.delta;
        if (position < std::numeric_limits<int>::min() / 2 || position > std::numeric_limits<int>::max() / 2) return E_INVALIDARG;
        candidate.position = static_cast<int>(position);
        candidate.displacement = candidate.position - position_;
        double quality = 0;
        if (!Validate(frame, candidate.position, candidate.fixedTop, candidate.fixedBottom, quality)) {
            candidate.failure = StitchFailure::Inconsistent; result = std::move(candidate); return S_OK;
        }
        candidate.overlap = match.overlap;
        candidate.confidence = quality * std::max(0.0, 1 - match.error / 30);
        const int body = frame.height() - candidate.fixedTop - candidate.fixedBottom;
        const int maximum = calibrated_ ? maximum_ : body;
        const bool extends = candidate.position < minimum_ || candidate.position + body > maximum;
        candidate.status = candidate.displacement == 0 ? StitchStatus::Unchanged : extends ? StitchStatus::Extended : StitchStatus::Revisited;
        candidate.failure = StitchFailure::None;
        const auto height = static_cast<std::int64_t>(candidate.fixedTop) + std::max(maximum, candidate.position + body) -
            std::min(minimum_, candidate.position) + candidate.fixedBottom;
        if (height > limits_.maxHeight || static_cast<std::uint64_t>(height) * width() * 4 > limits_.maxBytes)
            candidate.status = StitchStatus::Limit;
        result = std::move(candidate);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

HRESULT ScrollStitcher::Commit(const Image& frame, const StitchCandidate& candidate, StitchUpdate& update) noexcept {
    update = {candidate.status, candidate.displacement, viewport(), candidate.overlap, candidate.confidence, candidate.failure};
    if (candidate.status == StitchStatus::Unmatched || candidate.status == StitchStatus::Limit ||
        candidate.status == StitchStatus::Calibrating) return S_OK;
    if (!candidate.features_ || candidate.basis_ != previous_ || candidate.revision_ != revision_) return HRESULT_FROM_WIN32(ERROR_RETRY);
    if (frame.width() != width() || frame.height() != seed_.height()) return E_INVALIDARG;
    if (candidate.status == StitchStatus::Unchanged) return S_OK;
    try {
        const int body = frame.height() - candidate.fixedTop - candidate.fixedBottom;
        const int maximum = calibrated_ ? maximum_ : body;
        const int minimum = std::min(minimum_, candidate.position), nextMaximum = std::max(maximum, candidate.position + body);
        Image seedBody;
        if (!calibrated_) {
            const auto hr = CopyImageRect(seed_, {0, candidate.fixedTop, width(), seed_.height() - candidate.fixedBottom}, seedBody);
            if (FAILED(hr)) return hr;
        }
        Strip prepend{candidate.position, {}}, append{maximum, {}};
        const bool addTop = candidate.position < minimum_, addBottom = candidate.position + body > maximum;
        if (addTop) {
            const auto hr = CopyImageRect(frame, {0, candidate.fixedTop, width(), candidate.fixedTop + minimum_ - candidate.position}, prepend.image);
            if (FAILED(hr)) return hr;
        }
        if (addBottom) {
            const auto hr = CopyImageRect(frame, {0, candidate.fixedTop + maximum - candidate.position, width(), frame.height() - candidate.fixedBottom}, append.image);
            if (FAILED(hr)) return hr;
        }
        strips_.reserve(strips_.size() + 2); order_.reserve(order_.size() + 2);
        if (!calibrated_) strips_[0].image = std::move(seedBody);
        if (addTop) { order_.insert(order_.begin(), strips_.size()); strips_.push_back(std::move(prepend)); }
        if (addBottom) { order_.push_back(strips_.size()); strips_.push_back(std::move(append)); }
        recent_.push_back({position_, previous_});
        if (recent_.size() > 5) recent_.erase(recent_.begin());
        previous_ = candidate.features_;
        top_ = candidate.fixedTop; bottom_ = candidate.fixedBottom;
        minimum_ = minimum; maximum_ = nextMaximum; position_ = candidate.position;
        calibrated_ = true; ++revision_;
        update.viewport = viewport();
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

bool ScrollStitcher::Confirm(const Image& first, const StitchCandidate& candidate, const Image& next, ScrollDirection direction) const noexcept {
    if (!candidate.features_ || first.width() != next.width() || first.height() != next.height()) return false;
    try {
        const auto features = Describe(next);
        int delta = 0;
        if (!SameFeatures(*candidate.features_, *features)) {
            const auto match = Locate(*candidate.features_, *features, candidate.fixedTop, next.height() - candidate.fixedBottom, direction, false);
            if (!match.valid) return false;
            delta = match.delta;
        }
        const int begin = std::max(candidate.fixedTop, candidate.fixedTop - delta);
        const int end = std::min(next.height() - candidate.fixedBottom, next.height() - candidate.fixedBottom - delta);
        double quality = 0;
        return CheckPixels(next, begin, end, [&](int y) { return first.pixels().data() + static_cast<std::size_t>(y + delta) * first.width(); }, quality);
    } catch (...) { return false; }
}

HRESULT ScrollStitcher::Push(const Image& frame, ScrollDirection direction, StitchUpdate& update) noexcept {
    StitchCandidate candidate;
    const auto hr = Analyze(frame, direction, candidate);
    if (FAILED(hr)) return hr;
    return Commit(frame, candidate, update);
}

SeedPlacement ScrollStitcher::placement() const noexcept { return {top_, bottom_, top_ - minimum_, seed_.height(), height()}; }
RECT ScrollStitcher::viewport() const noexcept {
    return {0, top_ + position_ - minimum_, width(), top_ + position_ - minimum_ + seed_.height() - top_ - bottom_};
}
int ScrollStitcher::SeedRow(int y) const noexcept {
    if (y < top_) return y;
    if (y >= height() - bottom_) return seed_.height() - height() + y;
    const int row = y + minimum_;
    return row >= top_ && row < seed_.height() - bottom_ ? row : -1;
}
const std::uint32_t* ScrollStitcher::OutputRow(int y) const noexcept {
    if (y < top_) return seed_.pixels().data() + static_cast<std::size_t>(y) * width();
    if (y >= height() - bottom_) return seed_.pixels().data() + static_cast<std::size_t>(seed_.height() - height() + y) * width();
    return BodyRow(y - top_ + minimum_);
}

HRESULT ScrollStitcher::Finish(Image& result) const noexcept {
    if (seed_.empty()) return E_UNEXPECTED;
    Image image;
    const auto hr = image.Resize(width(), height());
    if (FAILED(hr)) return hr;
    if (top_) std::memcpy(image.pixels().data(), seed_.pixels().data(), static_cast<std::size_t>(top_) * image.stride());
    for (const auto& strip : strips_)
        std::memcpy(image.pixels().data() + static_cast<std::size_t>(top_ + strip.top - minimum_) * width(),
                    strip.image.pixels().data(), strip.image.byte_size());
    if (bottom_)
        std::memcpy(image.pixels().data() + static_cast<std::size_t>(height() - bottom_) * width(),
                    seed_.pixels().data() + static_cast<std::size_t>(seed_.height() - bottom_) * width(),
                    static_cast<std::size_t>(bottom_) * image.stride());
    result = std::move(image); return S_OK;
}

HRESULT ScrollStitcher::Preview(int width, int height, Image& result, const Image* annotated) const noexcept {
    if (seed_.empty() || width <= 0 || height <= 0 || (annotated && (annotated->width() != this->width() || annotated->height() != seed_.height())))
        return E_INVALIDARG;
    Image preview;
    const auto hr = preview.Resize(width, height);
    if (FAILED(hr)) return hr;
    for (int y = 0; y < height; ++y) {
        const int sourceY = static_cast<int>(static_cast<std::int64_t>(y) * this->height() / height);
        const auto* source = OutputRow(sourceY);
        if (!source) return E_UNEXPECTED;
        const int seedY = SeedRow(sourceY);
        for (int x = 0; x < width; ++x) {
            const int sourceX = static_cast<int>(static_cast<std::int64_t>(x) * this->width() / width);
            auto pixel = source[sourceX];
            if (annotated && seedY >= 0) {
                const auto index = static_cast<std::size_t>(seedY) * this->width() + sourceX;
                if (annotated->pixels()[index] != seed_.pixels()[index]) pixel = annotated->pixels()[index];
            }
            preview.pixels()[static_cast<std::size_t>(y) * width + x] = pixel;
        }
    }
    result = std::move(preview); return S_OK;
}
void ScrollStitcher::Clear() noexcept { *this = ScrollStitcher{}; }

}  // namespace picolet
