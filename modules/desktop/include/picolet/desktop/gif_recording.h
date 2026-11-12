#pragma once

#include <windows.h>

#include <memory>

namespace picolet {

struct DisplayCaptureProfile;

// Pending 表示仍由会话持有窗口或输出，其他值表示调用方可关闭会话并处理结果。
// DisplayChanged 表示冻结画面与当前显示配置不再一致，须重新框选。
enum class GifRecordingResult { Pending, Copied, Cancelled, Failed, DisplayChanged };

// 一次单屏 GIF 录制及复制会话。公开方法和析构均在所属窗口的 UI 线程执行。
// 采集和编码资源由会话的工作线程创建和释放。调用方须保持 owner 有效，直至 Close 返回。
class GifRecordingSession {
public:
    GifRecordingSession() noexcept;
    ~GifRecordingSession();
    GifRecordingSession(const GifRecordingSession&) = delete;
    GifRecordingSession& operator=(const GifRecordingSession&) = delete;

    // 在程序初始化时注册非激活录制浮窗，失败时返回 false。
    static bool Register(HINSTANCE instance, HICON icon) noexcept;
    // 调用前须隐藏截图遮罩并刷新桌面。area 是同一显示器内的半开物理坐标矩形。
    // seed 是冻结截图的显示器配置，由会话复制后交给后台核对 HDR 模式与参考白。
    // S_FALSE 表示显示器已经变化，E_INVALIDARG 表示选区无效。S_OK 表示开始异步取帧。
    HRESULT Open(HINSTANCE instance, HWND owner, RECT area, const DisplayCaptureProfile& seed) noexcept;
    // 每 100 毫秒在 UI 线程调用，更新浮窗并在编码完成后尝试复制。收到终态后应调用 Close。
    GifRecordingResult Tick() noexcept;
    // 中断等待并加入工作线程，再关闭浮窗和释放本次输出。可重复调用，关闭过程不发布剪贴板。
    void Close() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace picolet
