#pragma once

#include "image.h"

#include <string>
#include <vector>

namespace picolet {

// 拖动边和角使用同一组位标志；例如左上角同时设置 EdgeLeft 和 EdgeTop。
enum SelectionEdge : unsigned { EdgeNone = 0, EdgeLeft = 1, EdgeTop = 2, EdgeRight = 4, EdgeBottom = 8 };

// 输入和输出均为桌面物理像素的半开矩形。移动保持尺寸，碰到虚拟桌面边缘时停止。
RECT MoveSelection(RECT selection, POINT delta, const RECT& bounds) noexcept;

// 只移动指定边缘；对边保持不动，宽和高最小为一个像素。越界时裁到桌面边界。
RECT ResizeSelection(RECT selection, unsigned edges, POINT delta, const RECT& bounds) noexcept;

// 按鼠标位置选取单台显示器的完整物理范围（含任务栏），并裁到冻结帧的边界。
RECT SelectMonitor(POINT cursor, const RECT& bounds) noexcept;

enum class EditTool { Select, Rectangle, Ellipse, Line, Arrow, Pen, Text, Mosaic };

// 一笔已经提交或仍在编辑的标注。坐标和线宽使用物理像素；clip 是创建时的选区，
// 防止移走或放大选区后，在原来不可绘制的地方露出笔画。filled 只用于矩形和椭圆，
// 为 true 时使用 color 填满形状。直线使用 first、last 和当时的线宽及颜色。
struct Annotation {
    EditTool tool = EditTool::Select;
    RECT clip{};
    POINT first{};
    POINT last{};
    COLORREF color = RGB(239, 68, 68);
    int width = 4;
    int textSize = 24;
    bool filled = false;
    std::vector<POINT> path;
    std::wstring text;
};

// 在已有像素之上绘制尚未提交的标注；origin 为设备上下文 (0, 0) 对应的桌面坐标。
// 马赛克草稿只显示边界，松手提交后才修改实际像素。
void DrawAnnotationDraft(HDC dc, const Annotation& mark, POINT origin) noexcept;

// 保存当前会话的标注顺序和合成像素。冻结帧始终由调用方持有；所有方法在同一 UI 线程调用。
class AnnotationHistory {
public:
    // 提交一笔并重建合成图。成功后才丢弃可重做的标注，失败时保留原游标和图像。
    HRESULT Commit(const DesktopFrame& frame, Annotation&& mark) noexcept;

    // 沿标注顺序后退或前进一笔。没有可操作的标注时返回 S_FALSE，合成失败时保留当前画面。
    HRESULT Undo(const DesktopFrame& frame) noexcept;
    HRESULT Redo(const DesktopFrame& frame) noexcept;

    // 清除已应用和已撤销的标注，释放合成图。用于重新框选和结束会话。
    void Clear() noexcept;

    bool empty() const noexcept { return applied_ == 0; }
    bool can_redo() const noexcept { return applied_ < marks_.size(); }

    // 已合成的局部画面；遮罩可按桌面坐标叠绘到原图上，缩放或移动选区时不重新合成。
    const DesktopFrame& patch() const noexcept { return patch_; }

    // 从冻结帧及同一块合成像素提取原尺寸图片。成功才替换 result，不含任何界面元素。
    HRESULT Render(const DesktopFrame& frame, const RECT& selection, Image& result) const noexcept;

private:
    // 按前 count 笔合成。追加新标注时，把候选笔画叠在此前缀上，重做分支留待提交后清理。
    HRESULT Rebuild(const DesktopFrame& frame, std::size_t count, const Annotation* pending = nullptr) noexcept;

    std::vector<Annotation> marks_;
    std::size_t applied_ = 0;
    DesktopFrame patch_;
};

// 创建文字标注所用的字体。返回值由调用方用 DeleteObject 或 FontHandle 释放。
HFONT CreateAnnotationFont(int pixelSize) noexcept;

}  // namespace picolet
