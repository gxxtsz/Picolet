#include "export.h"
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
        TestPng();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    CoUninitialize();
    return status;
}
