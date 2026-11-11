#include <picolet/capture/scroll_stitcher.h>
#include <picolet/capture/capture_edit.h>
#include <picolet/desktop/hotkeys.h>
#include <picolet/desktop/image_view.h>
#include <picolet/capture/png_export.h>
#include <picolet/win32.h>
#include <wincodec.h>
#include <chrono>
#include <filesystem>
#include <cwchar>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace picolet;

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

std::uint32_t Pattern(int x, int y) {
    auto value = static_cast<std::uint32_t>(x * 713 + y * 9187 + 1234);
    value ^= value >> 13;
    value *= 0x85ebca6b;
    value ^= value >> 16;
    return 0xff000000 | (value & 0xffffff);
}

Image View(int position, int top = 0, int bottom = 0, int width = 96, int height = 160) {
    Image image;
    Check(image.Resize(width, height) == S_OK, "创建滚动帧失败");
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int row = y < top ? -1000 + y : y >= height - bottom ? 100000 + y : position + y - top;
            image.pixels()[static_cast<std::size_t>(y) * width + x] = Pattern(x, row);
        }
    return image;
}

void Verify(const Image& image, int minimum, int maximum, int top, int bottom, int seedHeight = 160) {
    Check(image.height() == maximum - minimum + top + bottom, "长图高度不正确");
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const int row = y < top ? -1000 + y : y >= image.height() - bottom
                ? 100000 + seedHeight - image.height() + y : minimum + y - top;
            Check(image.pixels()[static_cast<std::size_t>(y) * image.width() + x] == Pattern(x, row),
                  "拼接出现重复、缺失或像素变更");
        }
}

void TestDirections(int top, int bottom) {
    ScrollStitcher stitcher;
    const auto seed = View(0, top, bottom);
    Check(stitcher.Start(seed) == S_OK, "初始化拼接失败");
    StitchUpdate update;
    Check(stitcher.Push(seed, ScrollDirection::Unknown, update) == S_OK && update.status == StitchStatus::Unchanged,
          "重复首帧产生了新内容");
    int minimum = 0, maximum = 160 - top - bottom;
    int previous = 0;
    for (int position : {37, 76, 31, -24, -63, 0, 76, 103}) {
        const auto frame = View(position, top, bottom);
        const auto expected = position < minimum || position + 160 - top - bottom > maximum
            ? StitchStatus::Extended : StitchStatus::Revisited;
        const auto hr = stitcher.Push(frame, position > previous ? ScrollDirection::Down : ScrollDirection::Up, update);
        if (hr != S_OK || update.status != expected || update.displacement != position - previous)
            std::cerr << "固定头尾=" << top << ',' << bottom << " 位置=" << position << " 上次=" << previous
                      << " 状态=" << int(update.status) << " 位移=" << update.displacement << '\n';
        Check(hr == S_OK &&
              update.status == expected && update.displacement == position - previous, "双向位移或重复范围识别错误");
        minimum = std::min(minimum, position);
        maximum = std::max(maximum, position + 160 - top - bottom);
        previous = position;
        Image result;
        Check(stitcher.Finish(result) == S_OK, "合成长图失败");
        Verify(result, minimum, maximum, top, bottom);
    }
    const auto layout = stitcher.placement();
    Check(layout.fixedTop == top && layout.fixedBottom == bottom && layout.seedBodyTop == top - minimum,
          "固定头尾或首帧位置映射错误");
    Image preview;
    Check(stitcher.Preview(stitcher.width(), stitcher.height(), preview) == S_OK, "生成预览失败");
    Verify(preview, minimum, maximum, top, bottom);
}

void TestFailures() {
    ScrollStitcher stitcher;
    auto seed = View(0);
    Check(stitcher.Start(seed, {256 * 1024, 210}) == S_OK, "初始化有限长图失败");
    StitchUpdate update;
    auto frame = View(37);
    Check(stitcher.Push(frame, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Extended,
          "第一次追加失败");
    Image before, after;
    Check(stitcher.Finish(before) == S_OK, "读取长图失败");
    frame = View(80);
    Check(stitcher.Push(frame, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Limit,
          "未执行长图高度限制");
    frame = View(10000);
    Check(stitcher.Push(frame, ScrollDirection::Unknown, update) == S_OK && update.status == StitchStatus::Unmatched,
          "无重叠内容被错误拼接");
    Check(stitcher.Finish(after) == S_OK && std::ranges::equal(before.pixels(), after.pixels()),
          "错误或超限改变了已截像素");
    frame = View(42);
    Check(stitcher.Push(frame, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Extended,
          "错误后无法回到可靠位置继续");
    Image wrong;
    Check(wrong.Resize(32, 64) == S_OK && stitcher.Push(wrong, ScrollDirection::Down, update) == E_INVALIDARG,
          "接受了不一致的帧尺寸");
    Image blank;
    Check(blank.Resize(96, 160) == S_OK && stitcher.Start(blank) == S_OK, "创建空白帧失败");
    Check(stitcher.Push(blank, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Unchanged,
          "空白页面产生了重复内容");
    frame = View(0);
    Check(stitcher.Push(frame, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Unmatched,
          "缺少纹理时猜测了位移");
}

void TestMappedHistory() {
    constexpr int top = 19, bottom = 17;
    DesktopFrame source;
    source.bounds = {-80, -50, 16, 110};
    source.image = View(0, top, bottom);
    AnnotationHistory history;
    Annotation shape;
    shape.tool = EditTool::Rectangle;
    shape.clip = source.bounds;
    shape.first = {-75, -46};
    shape.last = {8, 106};
    shape.filled = true;
    shape.color = RGB(250, 70, 90);
    Check(history.Commit(source, std::move(shape)) == S_OK, "创建跨段标注失败");
    Annotation mosaic;
    mosaic.tool = EditTool::Mosaic;
    mosaic.clip = source.bounds;
    mosaic.first = {-70, -44};
    mosaic.last = {9, 105};
    Check(history.Commit(source, std::move(mosaic)) == S_OK, "创建跨段马赛克失败");
    Check(history.Undo(source) == S_OK, "准备重做分支失败");
    ScrollStitcher stitcher;
    Check(stitcher.Start(source.image) == S_OK, "创建标注迁移长图失败");
    StitchUpdate update;
    auto frame = View(-41, top, bottom);
    Check(stitcher.Push(frame, ScrollDirection::Up, update) == S_OK && update.status == StitchStatus::Extended,
          "向上补图失败");
    frame = View(12, top, bottom);
    Check(stitcher.Push(frame, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Extended,
          "向下补图失败");
    DesktopFrame target;
    Check(stitcher.Finish(target.image) == S_OK, "合成目标长图失败");
    target.bounds = {0, 0, target.image.width(), target.image.height()};
    AnnotationHistory mapped;
    const auto layout = stitcher.placement();
    Check(history.MapToLongImage(source, source.bounds, layout, target, mapped) == S_OK && mapped.can_redo(),
          "映射丢失重做分支");
    const auto verify = [&] {
        Image old, image;
        Check(history.Render(source, source.bounds, old) == S_OK && mapped.Render(target, target.bounds, image) == S_OK,
              "渲染迁移标注失败");
        for (int y = 0; y < old.height(); ++y) {
            const int destination = y < top ? y : y >= old.height() - bottom
                ? target.image.height() - old.height() + y : layout.seedBodyTop + y - top;
            for (int x = 0; x < old.width(); ++x)
                Check(old.pixels()[static_cast<std::size_t>(y) * old.width() + x] ==
                      image.pixels()[static_cast<std::size_t>(destination) * old.width() + x], "标注或马赛克迁移像素改变");
        }
        Image preview;
        Check(stitcher.Preview(target.image.width(), target.image.height(), preview, &old) == S_OK &&
              std::ranges::equal(preview.pixels(), image.pixels()), "预览标注与最终迁移的像素不一致");
    };
    verify();
    Check(history.Redo(source) == S_OK && mapped.Redo(target) == S_OK, "迁移后无法整笔重做");
    verify();
    Check(history.Undo(source) == S_OK && mapped.Undo(target) == S_OK &&
          history.Undo(source) == S_OK && mapped.Undo(target) == S_OK, "跨区标注无法整笔撤销");
    verify();
}

void TestLimitsAndNoise() {
    const auto seed = View(0);
    ScrollStitcher stitcher;
    Check(stitcher.Start(seed, {96ull * 190 * 4, 65535}) == S_OK, "创建字节上限会话失败");
    auto next = View(40);
    StitchUpdate update;
    Check(stitcher.Push(next, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Limit,
          "未执行像素字节上限");
    Check(stitcher.Start(seed) == S_OK, "重置噪声会话失败");
    next = View(37);
    for (int y = 8; y < 11; ++y)
        for (int x = 20; x < 23; ++x) next.pixels()[static_cast<std::size_t>(y) * next.width() + x] = 0xffff0000;
    Check(stitcher.Push(next, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Extended &&
          update.displacement == 37, "小面积动画阻止了可靠匹配");
    Image result;
    Check(stitcher.Finish(result) == S_OK, "噪声会话输出失败");
    Verify(result, 0, 197, 0, 0);
    Image periodic, shifted;
    Check(periodic.Resize(96, 160) == S_OK && shifted.Resize(96, 160) == S_OK, "创建周期纹理失败");
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 96; ++x) {
            periodic.pixels()[static_cast<std::size_t>(y) * 96 + x] = Pattern(x, y % 20);
            shifted.pixels()[static_cast<std::size_t>(y) * 96 + x] = Pattern(x, (y + 7) % 20);
        }
    Check(stitcher.Start(periodic) == S_OK && stitcher.Push(shifted, ScrollDirection::Down, update) == S_OK &&
          update.status == StitchStatus::Unmatched, "周期纹理存在歧义时猜测了位移");
    Check(stitcher.Finish(result) == S_OK && std::ranges::equal(result.pixels(), periodic.pixels()), "歧义帧改变了可靠结果");
    Image tinyImage;
    Check(tinyImage.Resize(16, 16) == S_OK && stitcher.Start(tinyImage) == E_INVALIDARG && stitcher.Finish(result) == S_OK &&
          std::ranges::equal(result.pixels(), periodic.pixels()), "初始化失败破坏了已有会话");
}

void TestLongCanvas() {
    DesktopFrame frame;
    Check(frame.image.Resize(64, 40000) == S_OK, "创建超高标注画布失败");
    frame.bounds = {0, 0, 64, 40000};
    AnnotationHistory history;
    Annotation mark;
    mark.tool = EditTool::Rectangle;
    mark.clip = frame.bounds;
    mark.first = {3, 4};
    mark.last = {50, 12};
    mark.filled = true;
    mark.color = RGB(240, 50, 70);
    Check(history.Commit(frame, Annotation(mark)) == S_OK, "创建顶部标注失败");
    mark.first.y = 35000;
    mark.last.y = 35010;
    Check(history.Commit(frame, std::move(mark)) == S_OK, "创建底部标注失败");
    Image composed;
    Check(history.Render(frame, frame.bounds, composed) == S_OK && composed.pixels()[35001ull * 64 + 10] == 0xfff03246,
          "超高画布底部标注坐标被截断");
    Check(history.Undo(frame) == S_OK && history.Redo(frame) == S_OK && history.Render(frame, frame.bounds, composed) == S_OK,
          "超高画布撤销重做失败");
    const MemoryDc dc(CreateCompatibleDC(nullptr));
    Image visible;
    Check(visible.Resize(48, 20) == S_OK, "创建可见条带失败");
    auto info = visible.BitmapInfo();
    void* bits = nullptr;
    const BitmapHandle bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    Check(bitmap && bits, "创建可见条带 DIB 失败");
    const auto old = SelectObject(dc.get(), bitmap.get());
    Check(DrawImageSection(dc.get(), composed, {3, 34996, 51, 35016}, {0, 0, 48, 20}) && GdiFlush(),
          "绘制超高图片的可见条带失败");
    SelectObject(dc.get(), old);
    const auto* pixels = static_cast<const std::uint32_t*>(bits);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 48; ++x)
            Check((pixels[y * 48 + x] | 0xff000000) == composed.pixels()[static_cast<std::size_t>(34996 + y) * 64 + 3 + x],
                  "可见条带的源坐标或像素错误");
}

void TestShortcutUpgrade() {
    auto defaults = DefaultShortcuts();
    std::array<WORD, 9> old{};
    old[0] = 2;
    for (std::size_t i = 0; i < 8; ++i)
        old[i + 1] = MAKEWORD(defaults[i].key, defaults[i].modifiers);
    old[ShortcutIndex(Shortcut::Copy) + 1] = 0;
    auto bindings = DecodeBindings(old, {});
    Check(bindings[ShortcutIndex(Shortcut::Copy)] == Hotkey{} &&
          bindings[ShortcutIndex(Shortcut::LongCapture)] == Hotkey{MOD_CONTROL, 'L'}, "升级没有保留清除项或补上长截图快捷键");
    old[ShortcutIndex(Shortcut::Save) + 1] = MAKEWORD('L', MOD_CONTROL);
    bindings = DecodeBindings(old, {});
    Check(bindings[ShortcutIndex(Shortcut::LongCapture)] == Hotkey{} &&
          bindings[ShortcutIndex(Shortcut::Save)] == Hotkey{MOD_CONTROL, 'L'}, "长截图覆盖了旧快捷键");
    Check(DecodeBindings(EncodeBindings(bindings), {}) == bindings, "新版本显式清除的长截图按键未保留");
}

void TestViewport() {
    ImageViewport view;
    view.canvas = {20, 140, 820, 740};
    view.image = {1600, 60000};
    view.FitWidth();
    Check(view.zoom == 0.5 && view.ToImage({20, 140}).x == 0, "适应宽度坐标错误");
    view.scrollY = 12000;
    const POINT anchor{420, 440};
    const auto old = view.ToImage(anchor);
    view.ZoomAt(2.0, anchor);
    const auto next = view.ToImage(anchor);
    Check(std::abs(old.x - next.x) <= 1 && std::abs(old.y - next.y) <= 1, "缩放没有保持鼠标锚点");
    view.scrollY = 999999;
    view.Clamp();
    Check(view.Visible().bottom == 60000 && view.scrollY > 65535, "高倍长图滚动被截为十六位");
    const auto box = view.ToClient({400, 1000, 500, 1100});
    const auto point = view.ToImage({box.left, box.top});
    Check(point.x == 400 && point.y == 1000, "图片与客户区坐标往返错误");
}

void TestLargeFrame() {
    const auto seed = View(0, 70, 60, 900, 1000);
    const auto next = View(83, 70, 60, 900, 1000);
    ScrollStitcher stitcher;
    Check(stitcher.Start(seed) == S_OK, "创建大帧失败");
    StitchUpdate update;
    const auto start = std::chrono::steady_clock::now();
    Check(stitcher.Push(next, ScrollDirection::Down, update) == S_OK && update.status == StitchStatus::Extended &&
          update.displacement == 83, "大帧匹配失败");
    std::cout << "大帧匹配耗时：" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " 毫秒\n";
}

Image Paper(int mode) {
    Image image;
    Check(image.Resize(780, 4200) == S_OK, "创建文字页面失败");
    const MemoryDc dc(CreateCompatibleDC(nullptr));
    void* bits = nullptr;
    const auto info = image.BitmapInfo();
    const BitmapHandle bitmap(CreateDIBSection(dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    Check(bitmap && bits, "创建页面 DIB 失败");
    const auto old = SelectObject(dc.get(), bitmap.get());
    RECT bounds{0, 0, image.width(), image.height()};
    FillRect(dc.get(), &bounds, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const FontHandle font(CreateAnnotationFont(20));
    const auto oldFont = SelectObject(dc.get(), font.get());
    SetBkMode(dc.get(), TRANSPARENT);
    SetTextColor(dc.get(), RGB(30, 42, 55));
    for (int row = 0, y = 15; y < image.height() - 70; ++row, y += mode == 0 ? 36 : 112) {
        const int left = mode == 1 ? (row % 2 ? 90 : 20) : 20;
        if (mode) {
            const BrushHandle brush(CreateSolidBrush(mode == 1 ? RGB(224, 245, 232) : RGB(241, 244, 250)));
            RECT card{left, y, 750 - (row % 3) * 30, y + 97};
            FillRect(dc.get(), &card, brush.get());
        }
        wchar_t text[180]{};
        std::swprintf(text, std::size(text), L"第 %04d 行　内容校验 %08d　中文截图与滚动定位 %05d", row, row * 7919, row * 313);
        TextOutW(dc.get(), left + 10, y + (mode ? 6 : 0), text, static_cast<int>(std::wcslen(text)));
        if (mode) {
            std::swprintf(text, std::size(text), L"网页、聊天记录、文档的第 %d 段内容，需要保持原始像素。", row + 1);
            TextOutW(dc.get(), left + 10, y + 42, text, static_cast<int>(std::wcslen(text)));
        }
    }
    Check(GdiFlush() != FALSE, "刷新页面绘制失败");
    SelectObject(dc.get(), oldFont);
    SelectObject(dc.get(), old);
    const auto* pixels = static_cast<const std::uint32_t*>(bits);
    std::transform(pixels, pixels + image.pixels().size(), image.pixels().begin(), [](auto p) { return p | 0xff000000; });
    return image;
}

Image PaperFrame(const Image& paper, int position, int top, int bottom) {
    Image frame;
    Check(frame.Resize(paper.width(), 720) == S_OK, "创建纸张视口失败");
    for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x) {
            const auto value = y < top ? 0xff123f4eu : y >= frame.height() - bottom ? 0xffd8e4eeu :
                paper.pixels()[static_cast<std::size_t>(200 + position + y - top) * paper.width() + x];
            frame.pixels()[static_cast<std::size_t>(y) * frame.width() + x] = value;
        }
    return frame;
}

void TestTextPages() {
    for (int mode = 0; mode < 3; ++mode) {
        const auto paper = Paper(mode);
        constexpr int top = 70, bottom = 58, body = 720 - top - bottom;
        const auto seed = PaperFrame(paper, 0, top, bottom);
        ScrollStitcher stitcher;
        Check(stitcher.Start(seed) == S_OK, "初始化文字页面失败");
        StitchUpdate update;
        int previous = 0;
        for (int position : {36, 108, 73, 149, 89, -32, -81, 18}) {
            const auto frame = PaperFrame(paper, position, top, bottom);
            const auto hr = stitcher.Push(frame, position > previous ? ScrollDirection::Down : ScrollDirection::Up, update);
            if (hr != S_OK || update.displacement != position - previous)
                std::cerr << "页面类型=" << mode << " 位置=" << position << " 位移=" << update.displacement << " 状态=" << int(update.status) << '\n';
            Check(hr == S_OK && update.displacement == position - previous, "文字页面未正确识别位移");
            previous = position;
        }
        const auto layout = stitcher.placement();
        if (layout.fixedTop != top || layout.fixedBottom != bottom)
            std::cerr << "页面类型=" << mode << " 识别头尾=" << layout.fixedTop << ',' << layout.fixedBottom << '\n';
        Check(layout.fixedTop == top && layout.fixedBottom == bottom, "正文空白被误认为固定头尾");
        Image result;
        Check(stitcher.Finish(result) == S_OK && result.height() == 720 + 149 + 81, "文字长图尺寸错误");
        for (int y = 0; y < result.height(); ++y)
            for (int x = 0; x < result.width(); ++x) {
                const auto expected = y < top ? 0xff123f4eu : y >= result.height() - bottom ? 0xffd8e4eeu :
                    paper.pixels()[static_cast<std::size_t>(200 - 81 + y - top) * paper.width() + x];
                Check(result.pixels()[static_cast<std::size_t>(y) * result.width() + x] == expected, "文字长图缺行或重复了固定区域");
            }
        Check(stitcher.viewport().bottom <= top + body + 18 + 81, "文字视口定位错误");
    }
}

void TestCardStartPhases() {
    // 卡片在视口边缘只露出背景或重复字形时，首个小位移仍须准确区分正文与固定栏。
    for (int mode : {1, 2}) {
        const auto paper = Paper(mode);
        for (int phase : {0, 17, 43, 71, 95}) {
            ScrollStitcher stitcher;
            const auto seed = PaperFrame(paper, phase, 70, 58);
            Check(stitcher.Start(seed) == S_OK, "初始化卡片起始位置失败");
            StitchUpdate update;
            for (int delta : {3, 13, 21, 38, 63, 93, 131, 103, 79, 31, -7}) {
                const auto frame = PaperFrame(paper, phase + delta, 70, 58);
                Check(stitcher.Push(frame, ScrollDirection::Unknown, update) == S_OK && update.status != StitchStatus::Unmatched,
                      "卡片初始小位移丢失定位");
                if (delta < 24 && update.status == StitchStatus::Calibrating) {
                    Image unchanged;
                    Check(stitcher.Finish(unchanged) == S_OK && std::ranges::equal(seed.pixels(), unchanged.pixels()),
                          "校准期间改变了原始像素");
                    continue;
                }
                const auto placement = stitcher.placement();
                if (placement.fixedTop != 70 || placement.fixedBottom != 58)
                    std::cerr << "卡片类型=" << mode << " 初始=" << phase << " 位移=" << delta << " 实际=" << update.displacement
                              << " 状态=" << int(update.status) << " 固定="
                              << placement.fixedTop << ',' << placement.fixedBottom << '\n';
                Check(placement.fixedTop == 70 && placement.fixedBottom == 58, "新露出的卡片正文被误识别为固定区");
            }
            Image result; Check(stitcher.Finish(result) == S_OK && result.height() == 858, "卡片往返结果高度错误");
            for (int y = 70; y < result.height() - 58; ++y)
                for (int x = 0; x < result.width(); ++x)
                    Check(result.pixels()[static_cast<std::size_t>(y) * result.width() + x] ==
                          paper.pixels()[static_cast<std::size_t>(200 + phase - 7 + y - 70) * paper.width() + x],
                          "卡片初始小位移产生错误接缝");
        }
    }
}

void TestLongPng() {
    Image image;
    Check(image.Resize(64, 33001) == S_OK, "创建超高 PNG 失败");
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) image.pixels()[static_cast<std::size_t>(y) * image.width() + x] = Pattern(x, y);
    GUID id{};
    wchar_t token[40]{};
    Check(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, token, 40), "创建 PNG 测试路径失败");
    const auto path = std::filesystem::temp_directory_path() / (std::wstring(L"Picolet-长图-") + token + L".png");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { DeleteFileW(path.c_str()); } } cleanup{path};
    Check(SavePngFile(path.c_str(), image, false) == S_OK, "保存超高中文路径 PNG 失败");
    ComPtr<IWICImagingFactory> factory;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))),
          "创建 PNG 解码器失败");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put())),
          "打开超高 PNG 失败");
    ComPtr<IWICBitmapFrameDecode> decoded;
    Check(SUCCEEDED(decoder->GetFrame(0, decoded.put())), "读取超高 PNG 帧失败");
    UINT width = 0, height = 0;
    Check(SUCCEEDED(decoded->GetSize(&width, &height)) && width == 64 && height == 33001, "PNG 高度被截断");
    ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(factory->CreateFormatConverter(converter.put())) &&
          SUCCEEDED(converter->Initialize(decoded.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)),
          "转换超高 PNG 失败");
    Image roundtrip;
    Check(roundtrip.Resize(64, 33001) == S_OK && SUCCEEDED(converter->CopyPixels(nullptr, roundtrip.stride(), roundtrip.byte_size(),
          reinterpret_cast<BYTE*>(roundtrip.pixels().data()))) && std::ranges::equal(image.pixels(), roundtrip.pixels()), "超高 PNG 像素改变");
}
}  // namespace

int main() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int status = 0;
    try {
        TestDirections(0, 0);
        TestDirections(19, 0);
        TestDirections(0, 17);
        TestDirections(19, 17);
        TestFailures();
        TestLimitsAndNoise();
        TestMappedHistory();
        TestShortcutUpgrade();
        TestViewport();
        TestLargeFrame();
        TestTextPages();
        TestCardStartPhases();
        TestLongPng();
        TestLongCanvas();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    CoUninitialize();
    return status;
}
