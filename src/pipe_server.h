// ============================================================
// pipe_server.h — EXE 端命名管道服务器声明
// ============================================================
// 本模块负责创建管道、等待 DLL 连接、发送协议帧并接收响应。
// 后台读取线程处理重叠读；主线程通过队列消费 OK/ERROR/EXIT，日志直接回调 UI。
// 停止顺序必须是取消 I/O、等待线程退出、最后关闭句柄。
// 仅针对 Windows x64。
// ============================================================
#pragma once
#include "protocol.h"

// Windows API
#include <windows.h>

// 标准库
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>


// ============================================================
// PipeServer — 命名管道服务器
// ============================================================
class PipeServer
{
public:
    // ============================================================
    // 帧结构
    // ============================================================
    // 从管道读取的一个完整帧
    struct Frame
    {
        uint8_t              type = 0;     // 消息类型（protocol::MessageType）
        std::vector<uint8_t> payload;      // 负载数据（可能为空）
    };

    // 构造 / 析构
    PipeServer();
    ~PipeServer();

    // 生命周期

    // 创建单个双工字节模式命名管道，并以 FILE_FLAG_OVERLAPPED 打开，
    // 使后台读取线程的挂起读不会阻塞主线程的写操作。
    // pipeName 为完整管道名称；返回 true 表示创建成功。
    bool Start(const wchar_t* pipeName);

    // 停止服务器：关闭管道、退出后台读取线程并清空响应队列。
    void Stop();

    // 请求停止但不等待后台线程，供控制台控制处理函数使用。
    // 真正的资源回收仍由主线程调用 Stop 完成。
    void RequestStop() noexcept;

    // 等待 DLL 客户端连接；timeoutMs 为毫秒数，-1 表示无限等待。
    // 连接成功后启动后台读取线程，持续读取 DLL 发来的帧。
    bool WaitForClient(int timeoutMs);

    // 状态查询
    bool IsConnected() const { return m_connected.load(); }

    // 连接断开或服务器停止时置位。控制台输入层等待这个事件，
    // 这样游戏先退出时 Lune 不会永远停在 ReadConsoleW。
    HANDLE GetDisconnectEventHandle() const { return m_disconnectEvent; }

    // 设置日志回调。DLL 发来的 MSG_LOG 帧由后台读取线程实时转发，
    // 因此不需要等待用户输入命令；必须在 WaitForClient 之前设置。
    void SetLogCallback(std::function<void(const char*)> cb) { m_logCallback = std::move(cb); }

    // 向 DLL 发送一个帧；data 可为 nullptr，但仅在 len 为 0 时有效。
    // 返回 true 表示完整帧已写入管道。
    bool SendFrame(uint8_t type, const void* data, uint32_t len);

    // 从后台读取线程填充的响应队列中接收一帧（OK / ERROR / EXIT）。
    // timeoutMs：-1 无限等待，0 立即返回，>0 等待指定毫秒；日志帧不入队。
    enum class ReceiveStatus
    {
        Received,
        Timeout,
        Disconnected,
    };

    ReceiveStatus ReceiveFrame(Frame& out, int timeoutMs = -1);

    bool RecvFrame(Frame& out, int timeoutMs = -1)
    {
        return ReceiveFrame(out, timeoutMs) == ReceiveStatus::Received;
    }

    // 非阻塞接收一个帧，等价于 RecvFrame(out, 0)。
    bool PollFrame(Frame& out) { return RecvFrame(out, 0); }

    // 等待特定类型的帧；会丢弃非匹配帧，直到收到目标类型或超时。
    // 仅用于握手阶段（HELLO / READY），REPL 阶段应使用 RecvFrame 逐个处理。
    bool WaitForFrame(uint8_t expectedType, Frame& out, int timeoutMs);

private:
    // 后台读取线程主循环
    void ReaderLoop();

    // 成员变量
    HANDLE m_pipe = INVALID_HANDLE_VALUE;   // 管道句柄（FILE_FLAG_OVERLAPPED）
    mutable std::mutex m_handleMutex;       // 保护句柄关闭与取消操作

    std::atomic<bool> m_connected{ false }; // 连接状态（原子操作）
    std::atomic<bool> m_stopFlag{ false };  // 停止标志
    std::mutex m_writeMutex;                // 写操作互斥锁

    std::thread m_readerThread;                       // 后台读取线程
    std::function<void(const char*)> m_logCallback;   // 日志实时输出回调

    HANDLE m_disconnectEvent = nullptr;               // 连接断开通知事件

    std::deque<Frame> m_frameQueue;         // 响应帧队列（OK/ERROR/EXIT）
    std::mutex m_queueMutex;                // 队列互斥锁
    std::condition_variable m_queueCv;      // 队列条件变量

    static constexpr size_t MAX_QUEUED_FRAMES = 256;
};
