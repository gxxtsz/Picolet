#include "capture_edit.h"
#include "export.h"
#include "hotkeys.h"
#include "win32.h"

#include <wincodec.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace picolet;

namespace {

void Check(bool passed, const char* message) {
    if (!passed) {
        throw std::runtime_error(message);
    }
}

void TestCrop() {
    DesktopFrame frame;
    frame.bounds = {-3, -2, 1, 1};
    Check(SUCCEEDED(frame.image.Resize(4, 3)), "创建测试帧失败");
    constexpr std::array<std::uint32_t, 12> colors{
        0xff112233, 0xff445566, 0xff778899, 0xffaabbcc,
        0xff102030, 0xff405060, 0xff708090, 0xffa0b0c0,
        0xff123456, 0xff234567, 0xff345678, 0xff456789,
    };
    std::ranges::copy(colors, frame.image.pixels().begin());

    // 从主屏坐标向左上反向拖动，终点和起点的像素都必须出现在输出里。
    const RECT selection = SelectionRect({0, 0}, {-2, -1}, frame.bounds);
    const RECT expected{-2, -1, 1, 1};
    Check(EqualRect(&selection, &expected) != FALSE, "反向拖拽得到错误的物理坐标");
    Image cropped;
    Check(SUCCEEDED(CropImage(frame, selection, cropped)), "裁剪负坐标选区失败");
    Check(cropped.width() == 3 && cropped.height() == 2, "裁剪尺寸错误");
    const std::array expectedColors{colors[5], colors[6], colors[7], colors[9], colors[10], colors[11]};
    Check(std::ranges::equal(cropped.pixels(), expectedColors), "裁剪漏像素、取错位置或行方向错误");

    const RECT clipped = SelectionRect({-50, -50}, {50, 50}, frame.bounds);
    Check(EqualRect(&clipped, &frame.bounds) != FALSE, "越界拖动未裁到桌面边缘");
    const RECT click = SelectionRect({-2, -2}, {-2, -2}, frame.bounds);
    Check(IsRectEmpty(&click) != FALSE, "单击应保留框选状态");
}

void TestSelectionAdjustments() {
    const RECT desktop{-1920, -240, 2560, 1440};
    const RECT selection{-180, -140, 220, 60};
    const RECT moved = MoveSelection(selection, {-4000, 1600}, desktop);
    const RECT expectedMove{-1920, 1240, -1520, 1440};
    Check(EqualRect(&moved, &expectedMove), "移动越界后未保持选区尺寸");

    const RECT expanded = ResizeSelection(selection, EdgeLeft | EdgeTop, {-4000, -4000}, desktop);
    const RECT expectedExpand{-1920, -240, 220, 60};
    Check(EqualRect(&expanded, &expectedExpand), "左上角缩放移动了对边或超出屏幕");

    const RECT reduced = ResizeSelection(selection, EdgeRight | EdgeBottom, {-4000, -4000}, desktop);
    const RECT expectedReduce{-180, -140, -179, -139};
    Check(EqualRect(&reduced, &expectedReduce), "缩放越过对边后没有保留最小的单像素选区");
}

void TestAnnotations() {
    DesktopFrame frame;
    frame.bounds = {-12, -6, 12, 6};
    Check(SUCCEEDED(frame.image.Resize(24, 12)), "创建标注测试帧失败");
    for (int y = 0; y < frame.image.height(); ++y) {
        for (int x = 0; x < frame.image.width(); ++x) {
            frame.image.pixels()[static_cast<std::size_t>(y) * 24 + x] = (x % 2) ? 0xffffffff : 0xff000000;
        }
    }

    // 先画笔再打码，比较马赛克和未遮挡区域，随后撤销并移动选区检查标注是否留在原处。
    AnnotationHistory history;
    Annotation stroke;
    stroke.tool = EditTool::Pen;
    stroke.clip = frame.bounds;
    stroke.first = {-10, 0};
    stroke.last = {5, 0};
    stroke.path = {stroke.first, stroke.last};
    stroke.color = RGB(255, 0, 0);
    Check(history.Commit(frame, std::move(stroke)) == S_OK, "无法提交画笔标注");
    Image annotated;
    Check(SUCCEEDED(history.Render(frame, frame.bounds, annotated)), "合成标注图失败");
    Check(annotated.pixels()[6 * 24 + 4] == 0xffff0000, "画笔颜色、坐标或透明度错误");

    const RECT tile{-12, -6, 0, 6};
    Annotation mosaic;
    mosaic.tool = EditTool::Mosaic;
    mosaic.clip = frame.bounds;
    mosaic.first = {tile.left, tile.top};
    mosaic.last = {tile.right - 1, tile.bottom - 1};
    Check(history.Commit(frame, std::move(mosaic)) == S_OK, "无法提交马赛克标注");
    Image composed;
    Check(SUCCEEDED(history.Render(frame, frame.bounds, composed)), "合成马赛克图失败");
    std::uint32_t red = 0, green = 0, blue = 0;
    for (std::size_t y = 0; y < 12; ++y) {
        for (std::size_t x = 0; x < 12; ++x) {
            const auto pixel = annotated.pixels()[y * 24 + x];
            red += (pixel >> 16) & 0xff;
            green += (pixel >> 8) & 0xff;
            blue += pixel & 0xff;
        }
    }
    const auto average = 0xff000000u | ((red / 144) << 16) | ((green / 144) << 8) | (blue / 144);
    for (std::size_t y = 0; y < 12; ++y) {
        for (std::size_t x = 0; x < 24; ++x) {
            const auto offset = y * 24 + x;
            Check(composed.pixels()[offset] == (x < 12 ? average : annotated.pixels()[offset]),
                  "马赛克没有处理已合成的像素或改变了范围外的内容");
        }
    }
    Check(SUCCEEDED(history.Render(frame, {-4, -2, 2, 2}, composed)) && composed.pixels()[0] == average,
          "改变选区后重算了马赛克块或移动了标注");

    Check(history.Undo(frame) == S_OK && SUCCEEDED(history.Render(frame, frame.bounds, composed)) &&
          std::ranges::equal(composed.pixels(), annotated.pixels()), "撤销马赛克没有恢复上一笔");
    Check(history.Redo(frame) == S_OK && SUCCEEDED(history.Render(frame, frame.bounds, composed)) &&
          composed.pixels()[6 * 24 + 4] == average && !history.can_redo(), "重做马赛克没有按原顺序合成");
    Check(history.Undo(frame) == S_OK, "重做后无法再次撤销");
    Check(history.Undo(frame) == S_OK && history.empty() && SUCCEEDED(history.Render(frame, frame.bounds, composed)) &&
          std::ranges::equal(composed.pixels(), frame.image.pixels()), "撤销所有标注没有恢复原图");
}

void TestMosaicDraft() {
    DesktopFrame frame;
    frame.bounds = {-20, -14, 8, 10};
    Check(SUCCEEDED(frame.image.Resize(28, 24)), "创建马赛克预览测试帧失败");
    for (std::size_t i = 0; i < frame.image.pixels().size(); ++i) {
        frame.image.pixels()[i] = i % 2 ? 0xff00ff00u : 0xff0000ffu;
    }

    // 用红色标注区分底图和合成图，同时准备一笔可重做的标注。
    AnnotationHistory history;
    Annotation box;
    box.tool = EditTool::Rectangle;
    box.clip = frame.bounds;
    box.filled = true;
    box.color = RGB(255, 0, 0);
    box.first = {-16, -10};
    box.last = {-10, -4};
    Check(history.Commit(frame, Annotation(box)) == S_OK, "创建预览下方的标注失败");
    box.first = {1, 5};
    box.last = {3, 7};
    Check(history.Commit(frame, std::move(box)) == S_OK && history.Undo(frame) == S_OK && history.can_redo(),
          "创建预览时的重做分支失败");
    Image before;
    Check(history.Render(frame, frame.bounds, before) == S_OK, "读取预览前的图像失败");

    Annotation mosaic;
    mosaic.tool = EditTool::Mosaic;
    mosaic.clip = {-24, -18, 7, 9};
    mosaic.first = {-1, 4};
    mosaic.last = {-22, -16};
    const RECT area = SelectionRect(mosaic.first, mosaic.last, mosaic.clip);
    Image preview;
    Check(history.RenderMosaicDraft(frame, area, preview) == S_OK && preview.width() == 20 && preview.height() == 19,
          "反向拖动的马赛克预览未按冻结帧边界裁剪");
    Check((preview.pixels()[0] & 0x00ff0000u) != 0, "马赛克预览没有使用已提交的标注像素");

    const RECT smaller = SelectionRect(mosaic.first, {-8, -3}, mosaic.clip);
    Image shrunk;
    Check(history.RenderMosaicDraft(frame, smaller, shrunk) == S_OK && shrunk.width() == 8 && shrunk.height() == 8 &&
          std::ranges::all_of(shrunk.pixels(), [](auto pixel) { return pixel == 0xff007f7fu; }),
          "缩小草稿时没有按新的边界重新取样");
    Check(history.RenderMosaicDraft(frame, {}, shrunk) == S_FALSE && shrunk.width() == 8,
          "空草稿不应覆盖上次生成的图像");

    Image after;
    Check(history.Render(frame, frame.bounds, after) == S_OK && std::ranges::equal(before.pixels(), after.pixels()),
          "移动马赛克草稿改变了已提交的图像");
    Check(history.Redo(frame) == S_OK && history.Undo(frame) == S_OK, "马赛克草稿改变了撤销或重做记录");
    Check(history.Commit(frame, std::move(mosaic)) == S_OK && !history.can_redo() &&
          history.Render(frame, area, after) == S_OK && std::ranges::equal(preview.pixels(), after.pixels()),
          "马赛克预览与提交后的边缘块像素不一致");
}

void TestShapeAnnotations() {
    DesktopFrame frame;
    frame.bounds = {-16, -14, 16, 16};
    Check(SUCCEEDED(frame.image.Resize(32, 30)), "创建形状测试帧失败");
    constexpr auto base = 0xff303030u;
    std::ranges::fill(frame.image.pixels(), base);
    AnnotationHistory history;
    const auto make = [&](EditTool tool, POINT first, POINT last, COLORREF color, bool filled = false) {
        Annotation mark;
        mark.tool = tool;
        mark.clip = frame.bounds;
        mark.first = first;
        mark.last = last;
        mark.color = color;
        mark.width = 3;
        mark.filled = filled;
        return mark;
    };
    const auto pixel = [&](const Image& image, LONG x, LONG y) {
        const auto row = static_cast<std::size_t>(y - frame.bounds.top);
        return image.pixels()[row * image.width() + (x - frame.bounds.left)];
    };
    auto outline = make(EditTool::Rectangle, {-12, -10}, {2, 4}, RGB(255, 0, 0));
    Check(history.Commit(frame, std::move(outline)) == S_OK, "无法绘制描边矩形");
    Image outlined;
    Check(history.Render(frame, frame.bounds, outlined) == S_OK &&
          pixel(outlined, -5, -10) == 0xffff0000 && pixel(outlined, -5, -3) == base,
          "描边矩形边缘颜色或内部像素错误");

    auto solid = make(EditTool::Rectangle, {-12, -10}, {2, 4}, RGB(0, 255, 0), true);
    solid.clip.right = -3;
    Check(history.Commit(frame, std::move(solid)) == S_OK, "无法绘制实心矩形");
    Check(history.Commit(frame, make(EditTool::Ellipse, {5, -10}, {14, 4}, RGB(0, 0, 255), true)) == S_OK,
          "无法绘制实心椭圆");
    Check(history.Commit(frame, make(EditTool::Line, {10, 10}, {-10, 10}, RGB(255, 255, 255))) == S_OK,
          "无法绘制反向直线");
    Image all;
    Check(history.Render(frame, frame.bounds, all) == S_OK &&
          pixel(all, -5, -3) == 0xff00ff00 && pixel(all, -3, -3) == base &&
          pixel(all, 9, -3) == 0xff0000ff && pixel(all, 5, -10) == base &&
          pixel(all, 0, 10) == 0xffffffff && pixel(all, 0, 7) == base,
          "实心形状、选区裁剪或直线的导出像素错误");

    // 失败的提交和重做不能损坏已撤销的分支。成功的新标注则应替换分支。
    Check(history.Undo(frame) == S_OK && history.can_redo(), "撤销后未保留可重做的直线");
    DesktopFrame broken;
    broken.bounds = frame.bounds;
    Check(FAILED(history.Redo(broken)) && history.can_redo(), "重做失败后丢失历史");
    Check(FAILED(history.Commit(broken, make(EditTool::Line, {-4, 5}, {4, 5}, RGB(255, 0, 0)))) &&
          history.can_redo(), "提交失败后丢失重做分支");
    Image replayed;
    Check(history.Redo(frame) == S_OK && history.Render(frame, frame.bounds, replayed) == S_OK &&
          std::ranges::equal(all.pixels(), replayed.pixels()), "重做后的标注像素不一致");
    Check(history.Undo(frame) == S_OK &&
          history.Commit(frame, make(EditTool::Line, {-4, 5}, {4, 5}, RGB(255, 0, 0))) == S_OK &&
          !history.can_redo() && history.Redo(frame) == S_FALSE, "新标注没有清除重做分支");
    Check(history.Undo(frame) == S_OK && history.can_redo(), "清空历史前未生成重做分支");
    history.Clear();
    Check(history.empty() && !history.can_redo(), "重选后仍然保留了标注历史");
}

void TestShortcuts() {
    const auto defaults = DefaultShortcuts();
    Check(defaults[0] == Hotkey{0, VK_F1}, "全新设置的全局截图键不是 F1");
    constexpr DWORD old = MAKELONG(MAKEWORD('G', MOD_CONTROL | MOD_SHIFT), MAKEWORD('F', MOD_CONTROL | MOD_ALT));
    auto migrated = DecodeBindings({}, old);
    Check(migrated[0] == Hotkey{MOD_CONTROL | MOD_SHIFT, 'G'} &&
          migrated[ShortcutIndex(Shortcut::Fullscreen)] == defaults[ShortcutIndex(Shortcut::Fullscreen)],
          "旧选区键未迁移，或错误迁移了旧全屏键");

    const Hotkey custom{MOD_WIN | MOD_SHIFT, VK_OEM_PLUS};
    migrated[0] = custom;
    migrated[ShortcutIndex(Shortcut::Copy)] = custom;
    migrated[ShortcutIndex(Shortcut::Undo)] = {};
    const auto saved = EncodeBindings(migrated);
    Check(DecodeBindings(saved, old) == migrated && ValidateBindings(migrated).error == ShortcutError::None,
          "未能还原清除项或拒绝了跨作用域复用的按键");
    Check(MatchLocalShortcut(migrated, custom) == Shortcut::Copy &&
          !MatchLocalShortcut(migrated, {MOD_WIN, VK_OEM_PLUS}), "局部快捷键未按作用域和完整修饰键匹配");

    migrated[ShortcutIndex(Shortcut::Save)] = custom;
    const auto duplicate = ValidateBindings(migrated);
    Check(duplicate.error == ShortcutError::Duplicate && duplicate.slot == Shortcut::Save, "未发现同组快捷键冲突");
    Check(DecodeBindings(EncodeBindings(migrated), {}) == defaults, "损坏的设置没有回退到默认值");
    migrated[0] = {0, VK_F12};
    Check(ValidateBindings(migrated).error == ShortcutError::Reserved, "全局快捷键接受了系统保留的 F12");

    // 用旧版的实际字段顺序验证升级，包含显式清除和跨作用域复用。
    std::array<WORD, 8> v1{{1, MAKEWORD('Y', MOD_CONTROL), MAKEWORD('A', MOD_CONTROL), 0,
                           MAKEWORD('S', MOD_CONTROL), MAKEWORD('T', MOD_CONTROL),
                           MAKEWORD('Z', MOD_CONTROL), VK_ESCAPE}};
    const Hotkey redo{MOD_CONTROL, 'Y'};
    auto upgraded = DecodeBindings(v1, old);
    Check(upgraded[0] == redo && upgraded[ShortcutIndex(Shortcut::Copy)] == Hotkey{} &&
          MatchLocalShortcut(upgraded, redo) == Shortcut::Redo, "旧版绑定迁移或默认重做键错误");
    v1[ShortcutIndex(Shortcut::Copy) + 1] = MAKEWORD('Y', MOD_CONTROL);
    upgraded = DecodeBindings(v1, old);
    Check(upgraded[ShortcutIndex(Shortcut::Redo)] == Hotkey{} && MatchLocalShortcut(upgraded, redo) == Shortcut::Copy &&
          DecodeBindings(EncodeBindings(upgraded), old) == upgraded, "重做默认键覆盖旧配置或恢复了已清除的绑定");
    upgraded[ShortcutIndex(Shortcut::Redo)] = redo;
    const auto redoConflict = ValidateBindings(upgraded);
    Check(redoConflict.error == ShortcutError::Duplicate && redoConflict.slot == Shortcut::Redo,
          "没有报告自定义重做快捷键冲突");
}

// 通过系统解码器读取实际写出的文件，检测像素通道、透明度、行方向和尺寸的错误。
std::array<std::uint32_t, 4> ReadPng(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(factory.put()))), "无法创建 WIC 解码组件");
    Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad, decoder.put())),
          "无法解码已保存的 PNG");
    GUID container{};
    Check(SUCCEEDED(decoder->GetContainerFormat(&container)) && IsEqualGUID(container, GUID_ContainerFormatPng),
          "写出的文件不是 PNG");
    Check(SUCCEEDED(decoder->GetFrame(0, frame.put())), "无法读取 PNG 图像帧");
    UINT width{}, height{};
    Check(SUCCEEDED(frame->GetSize(&width, &height)) && width == 2 && height == 2, "PNG 尺寸错误");
    ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(factory->CreateFormatConverter(converter.put())) &&
              SUCCEEDED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)),
          "无法将 PNG 转回 BGRA");
    std::array<std::uint32_t, 4> pixels{};
    Check(SUCCEEDED(converter->CopyPixels(nullptr, 8, 16, reinterpret_cast<BYTE*>(pixels.data()))),
          "无法读取 PNG 像素");
    return pixels;
}

struct TempDirectory {
    std::filesystem::path path;
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void TestPng() {
    GUID unique{};
    wchar_t token[39]{};
    Check(SUCCEEDED(CoCreateGuid(&unique)) && StringFromGUID2(unique, token, static_cast<int>(std::size(token))) > 0,
          "无法生成测试目录名");
    const auto directory = std::filesystem::current_path() / (std::wstring(L"Picolet 测试 ") + token);
    Check(std::filesystem::create_directory(directory), "无法创建中文测试目录");
    const TempDirectory cleanup{directory};
    const auto file = directory / L"四角色块.png";

    Image image;
    Check(SUCCEEDED(image.Resize(2, 2)), "无法分配 PNG 测试图像");
    const std::array<std::uint32_t, 4> first{0xffff0000, 0xff00ff00, 0xff0000ff, 0xffefab34};
    std::ranges::copy(first, image.pixels().begin());
    Check(SUCCEEDED(SavePngFile(file.c_str(), image, false)), "无法保存到中文路径");
    Check(ReadPng(file) == first, "PNG 往返后的像素不一致");

    image.pixels()[0] = 0xff675423;
    Check(FAILED(SavePngFile(file.c_str(), image, false)), "未确认时覆盖了同名文件");
    Check(ReadPng(file) == first, "拒绝覆盖时改变了已有文件");
    Check(SUCCEEDED(SavePngFile(file.c_str(), image, true)), "确认后无法覆盖文件");
    Check(std::ranges::equal(ReadPng(file), image.pixels()), "覆盖后没有写入新像素");
    Check(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1,
          "保存后遗留了临时文件");
}

}  // namespace

int main() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "无法初始化 COM 测试环境\n";
        return 1;
    }
    int status = 0;
    try {
        TestCrop();
        TestSelectionAdjustments();
        TestAnnotations();
        TestMosaicDraft();
        TestShapeAnnotations();
        TestShortcuts();
        TestPng();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    CoUninitialize();
    return status;
}
