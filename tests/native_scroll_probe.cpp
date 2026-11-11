// 在已打开的受控滚动夹具上记录真实桌面帧及匹配结果，用于排查显示缩放和颜色差异。
// 本程序只读取夹具的客户区，不改变窗口、焦点或输入；输出写入可执行文件旁的验证目录。
#include <picolet/edit/capture_edit.h>
#include <picolet/image/export.h>
#include <picolet/scroll/capture_pipeline.h>
#include <objbase.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>

using namespace picolet;
class ProbeSource final : public ScrollCaptureSource {
public:
    ProbeSource(std::unique_ptr<ScrollCaptureSource> source, std::filesystem::path path)
        : source_(std::move(source)), path_(std::move(path)) {}
    HRESULT Open(std::uint64_t session, HANDLE stop) noexcept override {
        com_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return source_->Open(session, stop);
    }
    HRESULT Read(Image& image, CaptureSourceState& state) noexcept override {
        const auto hr = source_->Read(image, state);
        if (hr == S_OK && count_ < 3) {
            CopyImageRect(image, {0, 0, image.width(), image.height()}, frames_[count_++]);
        }
        return hr;
    }
    void Close() noexcept override {
        source_->Close();
        for (int i = 0; i < count_; ++i)
            SavePngFile((path_ / (L"pipe-" + std::to_wstring(i) + L".png")).c_str(), frames_[i], true);
        if (SUCCEEDED(com_)) CoUninitialize();
    }
private:
    std::unique_ptr<ScrollCaptureSource> source_;
    std::filesystem::path path_;
    Image frames_[3]; int count_ = 0; HRESULT com_ = E_FAIL;
};

int main(int argc, char**) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    const auto window = FindWindowW(L"Picolet.ScrollFixture", nullptr);
    if (!window) { std::cerr << "请先打开滚动夹具。\n"; CoUninitialize(); return 1; }
    RECT rect{}; GetClientRect(window, &rect);
    POINT origin{}; ClientToScreen(window, &origin);
    rect = {origin.x + 2, origin.y + 44, origin.x + rect.right - 2, origin.y + rect.bottom - 2};
    wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH);
    const auto directory = std::filesystem::path(module).parent_path() / L"native-capture";
    std::filesystem::create_directories(directory);
    Image seed, image, fresh;
    const auto hr = CaptureRegion(rect, seed);
    if (FAILED(hr)) { std::cerr << "无法采集首帧。\n"; CoUninitialize(); return 1; }
    SavePngFile((directory / L"seed.png").c_str(), seed, true);
    if (argc > 1) {
        NativeScrollCaptureOptions options; options.area = rect; options.target = window;
        GetWindowRect(window, &options.targetRect); options.dpi = GetDpiForWindow(window);
        ScrollCapturePipeline pipeline;
        const auto started = pipeline.Start(seed, Image{}, std::make_unique<ProbeSource>(MakeNativeScrollSource(options), directory), {180, 300});
        if (FAILED(started)) return 1;
        for (int i = 0; i < 6; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const auto snapshot = pipeline.Snapshot();
            const auto elapsed = snapshot.stats.lastCaptureTime - snapshot.stats.firstCaptureTime;
            std::cout << "后台 采集=" << snapshot.stats.captured << " 处理=" << snapshot.stats.processed << " 运动=" << snapshot.stats.motion
                      << " 高=" << snapshot.height << " 原因=" << int(snapshot.failure) << " 跟踪=" << int(snapshot.tracking)
                      << " 频率=" << (elapsed ? (snapshot.stats.captured - 1) * 1000.0 / elapsed : 0)
                      << " 积压峰值=" << snapshot.stats.peakQueued << " 采样间断=" << snapshot.stats.samplingGaps
                      << " 采集微秒=" << snapshot.stats.captureMicroseconds << " 匹配微秒=" << snapshot.stats.matchMicroseconds
                      << " 预览微秒=" << snapshot.stats.previewMicroseconds << '\n';
        }
        pipeline.StopCapture();
        while (!pipeline.Snapshot().finished) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Image result; SeedPlacement placement; pipeline.Result(result, placement);
        SavePngFile((directory / L"pipe-result.png").c_str(), result, true);
        pipeline.Cancel(); CoUninitialize(); return 0;
    }
    ScrollStitcher stitcher; stitcher.Start(seed);
    RegionCaptureSource source;
    if (source.Open(rect) != S_OK) return 1;
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(17));
        const auto captured = source.Read(image);
        if (captured == S_FALSE) continue;
        if (FAILED(captured)) break;
        StitchCandidate candidate;
        stitcher.Analyze(image, ScrollDirection::Unknown, candidate);
        if (i < 5 || candidate.status != StitchStatus::Unmatched)
            std::cout << "帧=" << i << " 状态=" << int(candidate.status) << " 位移=" << candidate.displacement
                      << " 位置=" << candidate.position << " 重叠=" << candidate.overlap << " 可信度=" << candidate.confidence
                      << " 原因=" << int(candidate.failure) << " 固定头尾=" << candidate.fixedTop << ',' << candidate.fixedBottom << '\n';
        if (i < 3) {
            SavePngFile((directory / (L"cached-" + std::to_wstring(i) + L".png")).c_str(), image, true);
            CaptureRegion(rect, fresh);
            SavePngFile((directory / (L"fresh-" + std::to_wstring(i) + L".png")).c_str(), fresh, true);
        }
        StitchUpdate update; stitcher.Commit(image, candidate, update);
    }
    Image result; stitcher.Finish(result);
    SavePngFile((directory / L"result.png").c_str(), result, true);
    std::cout << "输出=" << result.width() << 'x' << result.height() << '\n';
    source.Close(); CoUninitialize(); return 0;
}
