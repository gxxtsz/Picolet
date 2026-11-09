#pragma once

#include <picolet/image/image.h>

#include <array>
#include <memory>
#include <vector>

namespace picolet {

// 方向只用于消除周期性纹理的歧义，不把滚轮刻度当作实际像素位移。
enum class ScrollDirection { Unknown, Up, Down };
// Calibrating 表示首段位移太小，暂不提交固定区映射；继续采集即可，不属于定位失败。
enum class StitchStatus { Unchanged, Extended, Revisited, Unmatched, Limit, Calibrating };
enum class StitchFailure { None, NoOverlap, Ambiguous, Inconsistent, FixedChanged };

// 限制只约束最终不透明 BGRA 图片；分配失败和越界均保留最后一次可靠拼接结果。
struct StitchLimits {
    std::uint64_t maxBytes = 256ull * 1024 * 1024;
    int maxHeight = 65535;
};

// 首帧的三段在最终图片中的位置。正文的 sourceTop 固定为 fixedTop，目标为 seedBodyTop。
// 固定头尾取首帧像素各一次，向上补图只改变正文偏移，固定底部始终位于最终图片末尾。
struct SeedPlacement {
    int fixedTop = 0;
    int fixedBottom = 0;
    int seedBodyTop = 0;
    int seedHeight = 0;
    int resultHeight = 0;
};

struct StitchUpdate {
    StitchStatus status = StitchStatus::Unchanged;
    int displacement = 0;
    RECT viewport{};  // 当前正文在完整长图中的物理像素位置。
    int overlap = 0;
    double confidence = 0;
    StitchFailure failure = StitchFailure::None;
};

struct ScrollFeatures;

// Analyze 生成的只读定位结果。候选只适用于生成它的帧和未推进的拼接版本，Commit 会校验版本。
// 特征通过不可变共享对象复用，避免确认及重试时重复计算。调用方不得修改公开定位字段。
struct StitchCandidate {
    StitchStatus status = StitchStatus::Unmatched;
    StitchFailure failure = StitchFailure::NoOverlap;
    int displacement = 0, position = 0, fixedTop = 0, fixedBottom = 0, overlap = 0;
    double confidence = 0;
private:
    friend class ScrollStitcher;
    std::shared_ptr<const ScrollFeatures> features_, basis_;
    std::uint64_t revision_ = 0;
};

// 不访问窗口或输入设备的纵向拼接器。所有方法应在同一线程调用；匹配和预览只读取原始像素。
// 新内容按独立条带保存，反向经过已有内容不覆盖旧像素，也不重新分配整张长图。
class ScrollStitcher {
public:
    // 复制首帧并重置会话。首帧至少为 32×64；失败不会替换已有会话。
    HRESULT Start(const Image& seed, StitchLimits limits = {}) noexcept;
    // 定位运动中的完整帧而不修改图片。先做指纹投票、位移精修及真实像素分块校验，
    // allowFullSearch 只控制低频恢复搜索；正常路径不遍历全部位移。失败保留已有结果。
    HRESULT Analyze(const Image& frame, ScrollDirection direction, StitchCandidate& candidate,
                    bool allowFullSearch = true) const noexcept;
    // 提交已定位候选；模型已经推进时返回 ERROR_RETRY。所有分配成功后才更新条带和参考帧。
    HRESULT Commit(const Image& frame, const StitchCandidate& candidate, StitchUpdate& update) noexcept;
    // 验证两张运动帧经过相对位移补偿后的一致性，供追踪器确认待提交帧。
    // 不要求速度恒定，允许加速和反向；固定栏和正文分别检查。
    bool Confirm(const Image& first, const StitchCandidate& candidate, const Image& next,
                 ScrollDirection direction) const noexcept;
    // 定位后立即提交，供稳定帧调用及现有测试使用。HRESULT 表示参数或资源错误。
    // 无法定位、达到上限时不推进参考帧，用户反向滚回最后可靠范围后可继续采集。
    HRESULT Push(const Image& frame, ScrollDirection direction, StitchUpdate& update) noexcept;
    // 一次性生成完整原尺寸图片，失败保留调用方 result。
    HRESULT Finish(Image& result) const noexcept;
    // 生成有界缩略图；annotatedSeed 可选，尺寸须与首帧相同，只将首帧的标注差异映射到预览。
    HRESULT Preview(int width, int height, Image& result, const Image* annotatedSeed = nullptr) const noexcept;
    SeedPlacement placement() const noexcept;
    RECT viewport() const noexcept;
    int width() const noexcept { return seed_.width(); }
    int height() const noexcept { return top_ + maximum_ - minimum_ + bottom_; }
    void Clear() noexcept;

private:
    struct Strip { int top; Image image; };
    struct Reference { int position; std::shared_ptr<const ScrollFeatures> features; };
    struct Match { int delta = 0; double error = 1e9; double alternative = 1e9; bool valid = false; int overlap = 0; };

    static std::shared_ptr<const ScrollFeatures> Describe(const Image& image);
    static Match Locate(const ScrollFeatures& previous, const ScrollFeatures& next, int top, int bottom,
                        ScrollDirection direction, bool fullSearch);
    bool Validate(const Image& frame, int position, int top, int bottom, double& quality) const noexcept;
    const std::uint32_t* BodyRow(int logical) const noexcept;
    const std::uint32_t* OutputRow(int y) const noexcept;
    int SeedRow(int outputY) const noexcept;

    Image seed_;
    std::shared_ptr<const ScrollFeatures> previous_;
    std::vector<Reference> recent_;
    std::vector<Strip> strips_;
    std::vector<std::size_t> order_;
    StitchLimits limits_;
    int top_ = 0, bottom_ = 0, minimum_ = 0, maximum_ = 0, position_ = 0;
    bool calibrated_ = false;
    std::uint64_t revision_ = 0;
};

}  // namespace picolet
