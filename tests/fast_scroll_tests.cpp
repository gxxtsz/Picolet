#include "capture_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace picolet;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

std::uint32_t Pattern(int x, int y) {
    auto value = static_cast<std::uint32_t>(x) * 713u + static_cast<std::uint32_t>(y) * 9187u + 1234;
    value ^= value >> 13; value *= 0x85ebca6bu; value ^= value >> 16;
    return 0xff000000 | (value & 0xffffff);
}

void FillFrame(Image& image, int position, int width = 320, int height = 480, int top = 40, int bottom = 30) {
    if (image.width() != width || image.height() != height) Check(image.Resize(width, height) == S_OK, "创建快速滚动帧失败");
    for (int y = 0; y < height; ++y) {
        const int row = y < top ? -1000 + y : y >= height - bottom ? 100000 + y : position + y - top;
        for (int x = 0; x < width; ++x) image.pixels()[static_cast<std::size_t>(y) * width + x] = Pattern(x, row);
    }
}

CapturedScrollFrame Frame(int position, std::uint64_t sequence, std::uint64_t timestamp, ScrollDirection hint = ScrollDirection::Unknown) {
    CapturedScrollFrame frame;
    frame.session = 1; frame.sequence = sequence; frame.timestamp = timestamp; frame.desktopTick = timestamp;
    frame.direction = hint; FillFrame(frame.image, position);
    return frame;
}

void Verify(const Image& image, int minimum, int maximum, int seedHeight = 480, int top = 40, int bottom = 30) {
    Check(image.height() == top + maximum - minimum + bottom, "快速滚动结果高度错误");
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const int row = y < top ? -1000 + y : y >= image.height() - bottom
                ? 100000 + seedHeight - image.height() + y : minimum + y - top;
            Check(image.pixels()[static_cast<std::size_t>(y) * image.width() + x] == Pattern(x, row),
                  "快速滚动结果出现缺行、重复或错误像素");
        }
}

void TestContinuousMotion() {
    Image seed; FillFrame(seed, 0);
    ScrollFrameTracker tracker;
    Check(tracker.Start(seed) == S_OK, "启动连续追踪失败");
    const int positions[]{7, 19, 38, 65, 103, 160, 245, 360, 492, 650, 550, 421, 282, 130, -20, -210, -430, -610, -790, -700, -400, -100, 100};
    std::vector<Image> retired;
    ScrollTrackingUpdate update;
    std::uint64_t sequence = 0;
    int previous = 0;
    for (const int position : positions) {
        ++sequence;
        auto frame = Frame(position, sequence, 1000 + sequence * 16, position > previous ? ScrollDirection::Down : ScrollDirection::Up);
        Check(tracker.Push(std::move(frame), update, retired) == S_OK, "连续运动处理失败");
        Check(update.state == ScrollTrackingState::Capturing, "不停顿滚动意外丢失追踪");
        previous = position; retired.clear();
    }
    Check(tracker.Flush(update, retired) == S_OK, "确认最后运动帧失败");
    Image result;
    Check(tracker.image().Finish(result) == S_OK, "输出连续滚动结果失败");
    Verify(result, -790, 650 + 410);
}

void TestBadFramesAndRecovery() {
    Image seed; FillFrame(seed, 0);
    ScrollFrameTracker tracker;
    Check(tracker.Start(seed) == S_OK, "初始化恢复追踪失败");
    std::vector<Image> retired;
    ScrollTrackingUpdate update;
    auto push = [&](int position, std::uint64_t seq, std::uint64_t time) {
        auto frame = Frame(position, seq, time);
        const auto hr = tracker.Push(std::move(frame), update, retired);
        retired.clear(); Check(hr == S_OK, "恢复过程中处理帧失败");
    };
    push(80, 1, 1000); push(160, 2, 1016);
    auto torn = Frame(220, 3, 1032);
    for (int y = 160; y < 300; ++y)
        for (int x = 0; x < torn.image.width(); ++x)
            torn.image.pixels()[static_cast<std::size_t>(y) * torn.image.width() + x] = Pattern(x, y + 90000);
    Check(tracker.Push(std::move(torn), update, retired) == S_OK && update.state == ScrollTrackingState::Recovering,
          "局部撕裂没有进入短暂恢复");
    retired.clear();
    push(240, 4, 1048); push(310, 5, 1064); push(5000, 6, 1080);
    Check(update.state == ScrollTrackingState::Recovering, "首个跳跃帧立即报警");
    push(5000, 7, 1600);
    Check(update.state == ScrollTrackingState::Gap, "持续无重叠时未报告缺口");
    push(290, 8, 1620); push(360, 9, 1636); push(410, 10, 1652);
    Check(update.state == ScrollTrackingState::Capturing, "回到重叠范围后未恢复");
    Check(tracker.Flush(update, retired) == S_OK, "恢复后结束失败");
    Image result; Check(tracker.image().Finish(result) == S_OK, "恢复后输出失败");
    Verify(result, 0, 410 + 410);
}

void TestLowOverlapAndVersions() {
    Image seed, next; FillFrame(seed, 0); FillFrame(next, 369);
    ScrollStitcher stitcher;
    Check(stitcher.Start(seed) == S_OK, "初始化低重叠失败");
    StitchCandidate candidate;
    Check(stitcher.Analyze(next, ScrollDirection::Down, candidate, false) == S_OK && candidate.status == StitchStatus::Extended &&
          candidate.overlap == 41 && candidate.confidence > 0.95, "约百分之十重叠未被准确定位");
    StitchUpdate update;
    Check(stitcher.Commit(next, candidate, update) == S_OK, "低重叠提交失败");
    Check(stitcher.Commit(next, candidate, update) == HRESULT_FROM_WIN32(ERROR_RETRY), "过期候选被重复提交");
    Image result; Check(stitcher.Finish(result) == S_OK, "低重叠输出失败");
    Verify(result, 0, 410 + 369);
    FillFrame(next, 739);
    Check(stitcher.Push(next, ScrollDirection::Up, update) == S_OK && update.status == StitchStatus::Extended && update.displacement == 370,
          "过期滚轮提示覆盖了实际画面方向");
}

void TestRepeatedTextMotion() {
    // 正文大部分相同，仅数字不同，整行高度的快速位移不能被平均色差误判为静止。
    Image paper; Check(paper.Resize(780, 3600) == S_OK, "创建重复文字参考图失败");
    MemoryDc dc(CreateCompatibleDC(nullptr));
    void* bits = nullptr; const auto info = paper.BitmapInfo();
    BitmapHandle bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    Check(bitmap && bits, "创建重复文字 DIB 失败");
    const auto old = SelectObject(dc.get(), bitmap.get());
    RECT bounds{0, 0, paper.width(), paper.height()};
    FillRect(dc.get(), &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    FontHandle font(CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI"));
    const auto oldFont = SelectObject(dc.get(), font.get());
    SetBkMode(dc.get(), TRANSPARENT); SetTextColor(dc.get(), RGB(25, 45, 62));
    for (int row = 0, y = 12; y < paper.height() - 38; ++row, y += 38) {
        wchar_t text[160]{};
        std::swprintf(text, std::size(text), L"第 %04d 段　内容编号 %08d　快速滚动截图验证 %05d", row, row * 7919, row * 313);
        TextOutW(dc.get(), 26, y + 6, text, static_cast<int>(std::wcslen(text)));
    }
    Check(GdiFlush() != FALSE, "刷新重复文字失败");
    const auto* pixels = static_cast<const std::uint32_t*>(bits);
    std::transform(pixels, pixels + paper.pixels().size(), paper.pixels().begin(), [](auto value) { return value | 0xff000000; });
    SelectObject(dc.get(), oldFont); SelectObject(dc.get(), old);
    const auto view = [&](int position) {
        Image image; Check(image.Resize(paper.width(), 720) == S_OK, "创建文字运动帧失败");
        for (int y = 0; y < image.height(); ++y) {
            auto* row = image.pixels().data() + static_cast<std::size_t>(y) * image.width();
            if (y < 40 || y >= 664) std::fill_n(row, image.width(), 0xff123f4eu);
            else std::copy_n(paper.pixels().data() + static_cast<std::size_t>(300 + position + y - 40) * paper.width(), image.width(), row);
        }
        return image;
    };
    ScrollFrameTracker tracker; const auto seed = view(0);
    Check(tracker.Start(seed) == S_OK, "启动文字运动追踪失败");
    std::vector<Image> retired; ScrollTrackingUpdate update;
    std::uint64_t sequence = 0;
    for (int position : {38, 114, 228, 380, 570, 798, 680, 456, 266, 114, -76, -190, -114, 38}) {
        CapturedScrollFrame frame; frame.image = view(position); frame.sequence = ++sequence; frame.timestamp = sequence * 16;
        Check(tracker.Push(std::move(frame), update, retired) == S_OK && update.state == ScrollTrackingState::Capturing,
              "重复文字的连续或反向滚动丢失追踪");
        retired.clear();
    }
    Check(tracker.Flush(update, retired) == S_OK, "确认最后文字帧失败");
    Image result; Check(tracker.image().Finish(result) == S_OK, "生成文字长图失败");
    const auto placement = tracker.image().placement();
    if (placement.fixedTop != 40 || placement.fixedBottom != 56 || result.height() != 1708)
        std::cerr << "重复文字：运动=" << update.motion << " 高=" << result.height() << " 固定=" << placement.fixedTop << ',' << placement.fixedBottom << '\n';
    Check(placement.fixedTop == 40 && placement.fixedBottom == 56 && result.height() == 1708,
          "文字帧被误判为静止或固定区域错误");
    for (int y = 0; y < result.height(); ++y)
        for (int x = 0; x < result.width(); ++x) {
            const auto expected = y < 40 || y >= result.height() - 56 ? 0xff123f4eu
                : paper.pixels()[static_cast<std::size_t>(300 - 190 + y - 40) * paper.width() + x];
            Check(result.pixels()[static_cast<std::size_t>(y) * result.width() + x] == expected, "文字长图出现缺行或重复接缝");
        }
}

void TestSparseRepaint() {
    // 大片浅色背景中只有窄小文字；文字局部重绘不能被大量相同背景掩盖。
    const auto sparse = [](int position, bool repaint) {
        Image image; Check(image.Resize(320, 480) == S_OK, "创建稀疏内容帧失败");
        for (int y = 0; y < image.height(); ++y) {
            auto* row = image.pixels().data() + static_cast<std::size_t>(y) * image.width();
            std::fill_n(row, image.width(), y < 40 || y >= 450 ? 0xff123f4eu : 0xfff4f6f8u);
            if (y < 40 || y >= 450) continue;
            const int logical = position + y - 40;
            row[8] = Pattern(0, logical);
            row[12] = ((logical % 7 < 3) != repaint) ? 0xff182536u : 0xfff4f6f8u;
        }
        return image;
    };
    const auto seed = sparse(0, false);
    ScrollStitcher stitcher; Check(stitcher.Start(seed) == S_OK, "初始化稀疏内容失败");
    StitchUpdate update;
    const auto bad = sparse(80, true);
    Check(stitcher.Push(bad, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Unmatched,
          "背景掩盖了文字重绘或撕裂");
    Image result; Check(stitcher.Finish(result) == S_OK && std::ranges::equal(seed.pixels(), result.pixels()),
                        "未确认文字被拼入结果");
    const auto good = sparse(80, false);
    Check(stitcher.Push(good, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Extended &&
          update.displacement == 80, "正常稀疏文字不能恢复定位");
}

struct ScriptControl {
    std::atomic<DWORD> opened{0}, read{0}, closed{0};
    std::atomic<bool> paused{false}, blocked{false};
    std::atomic<int> calls{0}, allowed{0};
};

class ScriptSource final : public ScrollCaptureSource {
public:
    ScriptSource(std::shared_ptr<ScriptControl> control, std::vector<int> positions, int width = 320, int height = 480)
        : control_(std::move(control)), positions_(std::move(positions)), width_(width), height_(height) {}
    HRESULT Open(std::uint64_t, HANDLE) noexcept override { control_->opened = GetCurrentThreadId(); return S_OK; }
    HRESULT Read(Image& image, CaptureSourceState& state) noexcept override {
        control_->read = GetCurrentThreadId();
        if (control_->blocked) { state = CaptureSourceState::Blocked; return S_FALSE; }
        if (control_->paused) { state = CaptureSourceState::Paused; return S_FALSE; }
        try {
            const int index = control_->calls.fetch_add(1);
            const int position = positions_.empty() ? index * 100000 : positions_[std::min<std::size_t>(index, positions_.size() - 1)];
            FillFrame(image, position, width_, height_);
            state = CaptureSourceState::Ready; return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    void Close() noexcept override { control_->closed = GetCurrentThreadId(); }
    void AllowCapture(std::uint64_t) noexcept override { ++control_->allowed; }
private:
    std::shared_ptr<ScriptControl> control_;
    std::vector<int> positions_;
    int width_, height_;
};

template <typename Predicate>
CapturePipelineSnapshot WaitFor(ScrollCapturePipeline& pipeline, Predicate predicate, int timeout = 5000) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(timeout)) {
        auto snapshot = pipeline.Snapshot();
        if (predicate(snapshot)) return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("等待后台管线超时");
}

void TestPipeline() {
    Image seed; FillFrame(seed, 0);
    ScrollCapturePipeline pipeline;
    const auto control = std::make_shared<ScriptControl>();
    const std::vector<int> positions{0, 12, 31, 61, 95, 140, 190, 250, 310, 350, 300, 230, 150, 50, -40, -100, -130};
    Check(pipeline.Start(seed, Image{}, std::make_unique<ScriptSource>(control, positions), {180, 320}) == S_OK, "启动异步管线失败");
    const auto session = pipeline.session();
    WaitFor(pipeline, [&](const auto& snapshot) { return snapshot.stats.captured >= positions.size() + 3 && snapshot.stats.motion > 0; });
    pipeline.RequestFinish();
    const auto snapshot = WaitFor(pipeline, [](const auto& value) { return value.finished; });
    Check(SUCCEEDED(snapshot.error) && snapshot.stats.peakQueued <= 12 && snapshot.preview, "后台状态或预览错误");
    Image result; SeedPlacement placement;
    Check(pipeline.Result(result, placement) == S_OK, "异步结果未准备好");
    Verify(result, -130, 350 + 410);
    Image retry; Check(pipeline.Result(retry, placement) == S_OK && std::ranges::equal(result.pixels(), retry.pixels()), "结束重试改变了像素");
    Check(control->opened != GetCurrentThreadId() && control->opened == control->read && control->read == control->closed,
          "来源资源未在同一后台线程创建读取释放");
    pipeline.AllowCapture(session - 1, 1);
    Check(control->allowed == 0, "过期会话消息被接受");
    pipeline.Cancel();
    Check(pipeline.Snapshot().session == 0, "取消后残留会话状态");
    for (int iteration = 0; iteration < 5; ++iteration) {
        const auto next = std::make_shared<ScriptControl>();
        next->paused = true;
        Check(pipeline.Start(seed, Image{}, std::make_unique<ScriptSource>(next, positions), {120, 200}) == S_OK && pipeline.session() > session,
              "重复启动未生成新会话");
        WaitFor(pipeline, [](const auto& value) { return value.source == CaptureSourceState::Paused; });
        if (iteration % 2) {
            next->blocked = true;
            WaitFor(pipeline, [](const auto& value) { return value.finished; });
        }
        pipeline.Cancel();
        Check(next->opened == next->closed, "取消时未释放来源");
    }
}

void TestBacklog() {
    Image seed; FillFrame(seed, 0, 64, 8000);
    const auto control = std::make_shared<ScriptControl>();
    ScrollCapturePipeline pipeline;
    CapturePipelineLimits limits; limits.maxFrames = 1; limits.cacheBytes = seed.byte_size();
    Check(pipeline.Start(seed, Image{}, std::make_unique<ScriptSource>(control, std::vector<int>{}, 64, 8000), {80, 200}, limits) == S_OK,
          "启动积压验证失败");
    const auto snapshot = WaitFor(pipeline, [](const auto& value) { return value.stats.samplingGaps > 0; }, 8000);
    Check(snapshot.stats.peakQueued <= 1, "队列超出帧数上限或覆盖旧帧");
    pipeline.StopCapture();
    WaitFor(pipeline, [](const auto& value) { return value.finished; }, 8000);
    Image result; SeedPlacement placement;
    Check(pipeline.Result(result, placement) == S_OK && std::ranges::equal(seed.pixels(), result.pixels()), "积压缺口被错误拼入");
    pipeline.Cancel();
}

void TestPipelineLimit() {
    Image seed; FillFrame(seed, 0);
    ScrollCapturePipeline pipeline;
    const auto control = std::make_shared<ScriptControl>();
    CapturePipelineLimits limits; limits.image.maxHeight = seed.height() + 20;
    Check(pipeline.Start(seed, Image{}, std::make_unique<ScriptSource>(control, std::vector<int>{80, 0, 0}), {120, 200}, limits) == S_OK,
          "启动上限管线失败");
    const auto snapshot = WaitFor(pipeline, [](const auto& value) { return value.finished; });
    Check(snapshot.tracking == ScrollTrackingState::Limit && control->opened == control->closed,
          "队列中的后续重复帧覆盖了大小上限状态");
    Image result; SeedPlacement placement;
    Check(pipeline.Result(result, placement) == S_OK && std::ranges::equal(seed.pixels(), result.pixels()), "达到上限后可靠像素改变");
    pipeline.Cancel();
}

void Benchmark() {
    Image seed, frame; FillFrame(seed, 0, 900, 1000, 70, 60);
    ScrollStitcher stitcher; Check(stitcher.Start(seed) == S_OK, "启动性能基准失败");
    std::vector<double> durations;
    StitchUpdate update;
    int position = 0;
    for (int i = 0; i < 120; ++i) {
        position += i % 3 == 0 ? 83 : 47;
        FillFrame(frame, position, 900, 1000, 70, 60);
        const auto start = std::chrono::steady_clock::now();
        Check(stitcher.Push(frame, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Extended, "基准匹配错误");
        durations.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(durations.begin(), durations.end());
    const double p95 = durations[static_cast<std::size_t>(durations.size() * 0.95) - 1];
    std::cout << "900×1000 连续帧基准：P50=" << durations[durations.size() / 2] << " ms，P95=" << p95
              << " ms，最大=" << durations.back() << " ms\n";
    Check(p95 <= 16, "同机 Release 匹配 P95 超过 16 毫秒");
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--benchmark") Benchmark();
        else {
            TestContinuousMotion(); TestBadFramesAndRecovery(); TestLowOverlapAndVersions(); TestRepeatedTextMotion();
            TestSparseRepaint(); TestPipeline(); TestBacklog(); TestPipelineLimit();
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
