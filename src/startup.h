#pragma once

#include "win32.h"

#include <string>
#include <string_view>
#include <vector>

namespace picolet {

inline constexpr wchar_t kStartupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kStartupValue[] = L"Picolet";

// 保留单个注册表值的原始类型和字节，包括没有该值的情况；用于跨设置保存失败后的精确恢复。
// present 只表示本程序的启动项已注册，不代表 Windows 的“启动应用”审批允许执行。
struct StartupValue {
    bool present = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> bytes;
    bool operator==(const StartupValue&) const noexcept = default;
};

// 读取当前进程的完整可执行文件路径，不依赖工作目录。成功才替换 path；不截断长路径。
// 内存分配失败返回 E_OUTOFMEMORY，其余系统错误以 HRESULT_FROM_WIN32 保留。
HRESULT CurrentExecutablePath(std::wstring& path) noexcept;

// 将完整磁盘路径或 UNC 路径构造成带引号的命令行，并追加独立的 --autostart 参数。
// 拒绝空路径、相对路径、双引号、内嵌零及超过 Run 项 260 字符限制的命令；失败不改 command。
HRESULT BuildStartupCommand(std::wstring_view executable, std::wstring& command) noexcept;

// 参数为包含程序名的完整 Windows 命令行；按系统规则拆分后仅比较参数，不在路径或子串中匹配。
// 参数大小写敏感，只有完整的 --autostart 才生效；拆分失败按普通启动处理。
bool IsAutostartCommandLine(const wchar_t* commandLine) noexcept;

// 当前用户启动项的读写器，仅管理 subkey 下名为 Picolet 的一个值，不删除整个键。
// root、subkey 均为借用：调用者负责使句柄和字符串活到本对象最后一次调用之后；本对象不关闭 root。
// 默认定位真实 HKCU Run；测试可传临时根键，确保不读写用户的真实自启动配置。
class StartupRegistration {
public:
    explicit StartupRegistration(HKEY root = HKEY_CURRENT_USER, const wchar_t* subkey = kStartupKey) noexcept
        : root_(root), subkey_(subkey) {}

    // 只申请查询权限；键或值不存在是成功，返回 present=false。失败不改 value。
    // 不展开环境变量、不补终止符，原始类型和字节原样保存，供恢复使用。
    HRESULT Read(StartupValue& value) const noexcept;

    // 精确恢复快照；present=false 时只删除本程序的值，不创建原本不存在的键。
    // 每次调用自行打开并关闭子键；原生注册表错误码通过 HRESULT 返回。
    HRESULT Restore(const StartupValue& value) const noexcept;

    // 开启时写入 REG_SZ（含结尾零）；关闭时忽略 executable。先读原值，再比较，完全相同时不写。
    // 返回 S_OK 表示实际改动、S_FALSE 表示无需改动。previous 非空时，在写入前交付原值快照；
    // 任何失败均不宣称保存成功，调用方只在 S_OK 后需要用 previous 回滚。
    HRESULT SetEnabled(bool enabled, std::wstring_view executable, StartupValue* previous = nullptr) const noexcept;

private:
    HKEY root_;
    const wchar_t* subkey_;
};

// 仅管理登录时等待托盘就绪的窗口计时器，不拥有窗口。所有调用和析构必须在窗口所属线程执行。
// Begin 后每秒产生一次 kTimer 消息，满 30 秒时调用方退出；Stop/析构释放计时器并拒绝迟到消息。
// 应在 WM_DESTROY 内先 Stop，再销毁所借用窗口。now 为 GetTickCount64 的单调毫秒值，可供测试注入。
class TrayStartupRetry {
public:
    enum class Result { Inactive, Waiting, Ready, TimedOut };
    static constexpr UINT_PTR kTimer = 0x5049;
    static constexpr ULONGLONG kTimeoutMs = 30000;
    ~TrayStartupRetry() { Stop(); }
    TrayStartupRetry() = default;
    TrayStartupRetry(const TrayStartupRetry&) = delete;
    TrayStartupRetry& operator=(const TrayStartupRetry&) = delete;

    // 重启前释放旧计时器；成功后 waiting() 为真，创建失败保留系统错误并保持停止状态。
    HRESULT Begin(HWND window, ULONGLONG now) noexcept;
    void Stop() noexcept;
    bool waiting() const noexcept { return timer_ != 0; }
    bool Expired(ULONGLONG now) const noexcept { return waiting() && now - started_ >= kTimeoutMs; }

    // 在 WM_TIMER/TaskbarCreated 中执行一次尝试。超时前才调用注册函数；成功或超时均先停止计时器。
    // 已停止时不再调用回调，允许忽略队列中迟到的计时器消息。回调须在当前线程执行且不抛异常。
    template <typename RegisterTray>
    Result Attempt(ULONGLONG now, RegisterTray&& registerTray) noexcept {
        if (!waiting()) return Result::Inactive;
        if (Expired(now)) { Stop(); return Result::TimedOut; }
        if (registerTray()) { Stop(); return Result::Ready; }
        return Result::Waiting;
    }

private:
    HWND window_ = nullptr;
    UINT_PTR timer_ = 0;
    ULONGLONG started_ = 0;
};

}  // namespace picolet
