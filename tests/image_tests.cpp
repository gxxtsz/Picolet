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
    Check(history.Undo(frame) == S_OK && history.empty() && SUCCEEDED(history.Render(frame, frame.bounds, composed)) &&
          std::ranges::equal(composed.pixels(), frame.image.pixels()), "撤销所有标注没有恢复原图");
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
        TestShortcuts();
        TestPng();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    CoUninitialize();
    return status;
}
