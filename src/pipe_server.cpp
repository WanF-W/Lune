// ============================================================
// pipe_server.cpp — EXE 端命名管道服务实现
// ============================================================
// 管道使用重叠 I/O：后台线程只负责读取和分发，主线程负责发送命令和
// 消费响应。停止顺序是“设置停止标志 -> CancelIoEx -> 等待读取线程 ->
// 关闭句柄”，这是本模块最重要的生命周期约束。
// ============================================================
#include "pipe_server.h"

#include "win_handle.h"

#include <chrono>

namespace
{
    bool ReadPipeOverlapped(HANDLE pipe, void* buffer, DWORD length, DWORD& bytesRead)
    {
        // 读取辅助函数只接受一个有效的非空缓冲区；调用方负责按帧循环读取。
        if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || buffer == nullptr || length == 0)
        {
            return false;
        }

        win::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;

        // 每次 I/O 使用独立事件，事件的生命周期覆盖整个 GetOverlappedResult。
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.Get();
        bytesRead = 0;

        // 重叠 ReadFile 可能立即完成，也可能返回 ERROR_IO_PENDING。
        BOOL completed = ReadFile(pipe, buffer, length, &bytesRead, &overlapped);
        if (!completed)
        {
            // 非 pending 错误表示管道断开或 I/O 无法启动。
            if (GetLastError() != ERROR_IO_PENDING) return false;

            // 事件被信号后，才允许查询并使用这次异步读取的结果。
            if (WaitForSingleObject(event.Get(), INFINITE) != WAIT_OBJECT_0) return false;
            completed = GetOverlappedResult(pipe, &overlapped, &bytesRead, FALSE);
        }

        return completed != FALSE && bytesRead > 0;
    }

    bool WritePipeOverlapped(HANDLE pipe, const void* buffer, DWORD length)
    {
        // 写入函数同样只处理一个连续缓冲区，调用方负责组织协议帧。
        if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || buffer == nullptr || length == 0)
        {
            return false;
        }

        // 命名管道写入可能只完成一部分，必须循环直到整个帧片段写完。
        DWORD totalWritten = 0;
        while (totalWritten < length)
        {
            win::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event) return false;

            OVERLAPPED overlapped{};
            overlapped.hEvent = event.Get();

            // 从尚未写入的位置继续写，支持管道只接受部分数据的情况。
            DWORD written = 0;
            BOOL completed = WriteFile(
                pipe,
                static_cast<const uint8_t*>(buffer) + totalWritten,
                length - totalWritten,
                &written,
                &overlapped);

            if (!completed)
            {
                // 只有 pending 才需要等待事件；其他错误直接终止本次发送。
                if (GetLastError() != ERROR_IO_PENDING) return false;
                if (WaitForSingleObject(event.Get(), INFINITE) != WAIT_OBJECT_0) return false;
                completed = GetOverlappedResult(pipe, &overlapped, &written, FALSE);
            }

            // 写入 0 字节会导致循环无法前进，因此必须视为失败。
            if (!completed || written == 0) return false;
            totalWritten += written;
        }

        return true;
    }

    bool IsKnownMessageType(uint8_t type)
    {
        // 拒绝未知类型，避免后续代码把未定义负载当作合法控制消息处理。
        switch (type)
        {
        case protocol::MSG_HELLO:
        case protocol::MSG_READY:
        case protocol::MSG_LOG:
        case protocol::MSG_ERROR:
        case protocol::MSG_OK:
        case protocol::MSG_CMD:
        case protocol::MSG_FILE:
        case protocol::MSG_EXIT:
            return true;
        default:
            return false;
        }
    }

    uint32_t DecodePayloadLength(const uint8_t* header)
    {
        // 协议长度固定为小端序，不能直接把非对齐字节数组转换成整数。
        return static_cast<uint32_t>(header[1])
             | (static_cast<uint32_t>(header[2]) << 8)
             | (static_cast<uint32_t>(header[3]) << 16)
             | (static_cast<uint32_t>(header[4]) << 24);
    }
}

// ============================================================
// 生命周期管理
// ============================================================
PipeServer::PipeServer()
    : m_disconnectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

PipeServer::~PipeServer()
{
    // 析构必须覆盖所有退出路径，即使调用方忘记显式 Stop 也不能遗留线程。
    Stop();
    if (m_disconnectEvent != nullptr)
    {
        CloseHandle(m_disconnectEvent);
        m_disconnectEvent = nullptr;
    }
}

bool PipeServer::Start(const wchar_t* pipeName)
{
    // Start 可安全重复调用；先完整回收上一次连接，避免旧线程和旧队列串线。
    Stop();
    if (pipeName == nullptr || *pipeName == L'\0' || m_disconnectEvent == nullptr) return false;

    HANDLE pipe = CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        65536,
        65536,
        0,
        nullptr);

    // CreateNamedPipeW 失败时没有可供后续清理的句柄。
    if (pipe == INVALID_HANDLE_VALUE) return false;

    {
        std::lock_guard<std::mutex> lock(m_handleMutex);
        m_pipe = pipe;
    }

    // 新管道尚未连接；清除停止标志后才能进入等待连接阶段。
    m_connected.store(false);
    m_stopFlag.store(false);
    ResetEvent(m_disconnectEvent);
    return true;
}

void PipeServer::RequestStop() noexcept
{
    // 先发布状态，让等待线程和发送线程尽快停止继续工作。
    m_stopFlag.store(true);
    m_connected.store(false);

    // 不关闭句柄，只取消挂起 I/O；关闭动作必须等所有使用者退出后进行。
    std::lock_guard<std::mutex> lock(m_handleMutex);
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        // 取消连接、读取和写入；句柄由 Stop 在所有线程退出后关闭。
        CancelIoEx(m_pipe, nullptr);
    }

    m_queueCv.notify_all();
}

void PipeServer::Stop()
{
    // RequestStop 负责发出取消信号，Stop 负责等待并完成实际资源回收。
    RequestStop();

    if (m_readerThread.joinable())
    {
        // ReaderLoop 可能正在等待 ReadFile；RequestStop 已通过 CancelIoEx 唤醒它。
        m_readerThread.join();
    }

    // RequestStop 已经取消了挂起 I/O，此时再等待发送线程释放写锁。
    std::lock_guard<std::mutex> writeLock(m_writeMutex);
    std::lock_guard<std::mutex> handleLock(m_handleMutex);

    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        // 现在没有线程再使用管道，关闭句柄不会使 OVERLAPPED 结构悬空。
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    {
        // 队列中的旧响应属于旧连接，重新 Start 时不能交给新连接消费。
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_frameQueue.clear();
    }

    m_connected.store(false);
    if (m_disconnectEvent != nullptr) SetEvent(m_disconnectEvent);
    m_queueCv.notify_all();
}

// ============================================================
// 客户端连接
// ============================================================
bool PipeServer::WaitForClient(int timeoutMs)
{
    // 连接等待只在 Start 创建的单个管道实例上进行。
    if (timeoutMs < -1) return false;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(m_handleMutex);
        pipe = m_pipe;
    }
    if (pipe == INVALID_HANDLE_VALUE || m_stopFlag.load()) return false;

    // ConnectNamedPipe 的重叠操作必须绑定一个仍然有效的事件对象。
    win::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.Get();

    BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    if (!connected)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
        {
            // 客户端可能在 ConnectNamedPipe 返回前完成连接，这仍然算成功。
            connected = TRUE;
        }
        else if (error == ERROR_IO_PENDING)
        {
            // pending 表示等待客户端连接，等待时间由调用方传入。
            const DWORD waitMs = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);
            const DWORD waitResult = WaitForSingleObject(event.Get(), waitMs);

            if (waitResult == WAIT_OBJECT_0)
            {
                // 事件触发后确认连接操作确实成功，而不是被取消或异常结束。
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
            }
            else
            {
                // 超时或等待失败都取消连接，并等待取消完成后再离开函数。
                // 取消后必须等待 overlapped 完成，才能安全释放 event 和复用管道句柄。
                RequestStop();
                WaitForSingleObject(event.Get(), INFINITE);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
                return false;
            }
        }
        else
        {
            // 其他 ConnectNamedPipe 错误不再尝试复用这条连接。
            connected = FALSE;
        }
    }

    if (!connected || m_stopFlag.load()) return false;

    // 连接完成后再启动唯一的后台读取线程，避免线程读到未连接的句柄。
    m_connected.store(true);
    m_stopFlag.store(false);
    m_readerThread = std::thread([this] { ReaderLoop(); });
    return true;
}

// ============================================================
// 后台读取线程
// ============================================================
void PipeServer::ReaderLoop()
{
    // ReaderLoop 是管道唯一的读者；主线程只通过队列取得非日志帧。
    while (!m_stopFlag.load())
    {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(m_handleMutex);
            pipe = m_pipe;
        }
        if (pipe == INVALID_HANDLE_VALUE) break;

        // 先完整读取 5 字节帧头，再根据长度分配负载缓冲区。
        uint8_t header[protocol::HEADER_SIZE]{};
        DWORD totalRead = 0;
        bool ok = true;

        while (totalRead < protocol::HEADER_SIZE)
        {
            // 字节模式管道可能拆分一次 WriteFile，因此读取必须允许部分完成。
            DWORD chunk = 0;
            if (!ReadPipeOverlapped(
                    pipe,
                    header + totalRead,
                    static_cast<DWORD>(protocol::HEADER_SIZE) - totalRead,
                    chunk))
            {
                ok = false;
                break;
            }
            totalRead += chunk;
        }
        if (!ok) break;

        // 帧头读取完成后，先校验类型和长度，再接触负载内存。
        const uint8_t type = header[0];
        const uint32_t length = DecodePayloadLength(header);
        if (!IsKnownMessageType(type) || length > protocol::MAX_PAYLOAD) break;

        Frame frame;
        frame.type = type;
        if (length > 0)
        {
            // 负载大小已通过 MAX_PAYLOAD 校验，vector 分配不会受协议长度攻击。
            frame.payload.resize(length);
            totalRead = 0;
            while (totalRead < length)
            {
                DWORD chunk = 0;
                if (!ReadPipeOverlapped(pipe, frame.payload.data() + totalRead, length - totalRead, chunk))
                {
                    ok = false;
                    break;
                }
                totalRead += chunk;
            }
            if (!ok) break;
        }

        if (type == protocol::MSG_LOG)
        {
            // 日志不应堵塞命令响应队列，直接交给 UI 回调处理。
            if (m_logCallback)
            {
                const std::string text(frame.payload.begin(), frame.payload.end());
                try
                {
                    m_logCallback(text.c_str());
                }
                catch (...)
                {
                    // 回调属于 UI 层，不能让异常穿过后台线程边界。
                    break;
                }
            }
            continue;
        }

        bool queueOverflow = false;
        {
            // 控制帧进入有上限的队列；异常发送方不能无限消耗本地内存。
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_frameQueue.size() >= MAX_QUEUED_FRAMES)
            {
                queueOverflow = true;
            }
            else
            {
                m_frameQueue.push_back(std::move(frame));
            }
        }

        if (queueOverflow) break;
        // 唤醒正在等待 OK/ERROR/EXIT 的主线程。
        m_queueCv.notify_one();
    }

    // 所有读取失败、停止或回调异常最终都把连接标记为断开。
    m_connected.store(false);
    if (m_disconnectEvent != nullptr) SetEvent(m_disconnectEvent);
    m_queueCv.notify_all();
}

// ============================================================
// 帧发送
// ============================================================
bool PipeServer::SendFrame(uint8_t type, const void* data, uint32_t len)
{
    // 帧长度和空指针在进入锁前校验，避免无效调用阻塞正常发送。
    if (len > protocol::MAX_PAYLOAD || (len > 0 && data == nullptr)) return false;

    // 同一管道上的帧必须串行写入，否则头和负载可能与另一条命令交错。
    std::lock_guard<std::mutex> writeLock(m_writeMutex);
    if (!m_connected.load() || m_stopFlag.load()) return false;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> handleLock(m_handleMutex);
        pipe = m_pipe;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    // 按协议手动编码小端帧头，保持与 DLL 的同步实现一致。
    uint8_t header[protocol::HEADER_SIZE]{};
    header[0] = type;
    header[1] = static_cast<uint8_t>(len & 0xFF);
    header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[4] = static_cast<uint8_t>((len >> 24) & 0xFF);

    // 先写帧头，再写负载；两次写入仍受同一把锁保护。
    if (!WritePipeOverlapped(pipe, header, static_cast<DWORD>(protocol::HEADER_SIZE))
        || (len > 0 && !WritePipeOverlapped(pipe, data, len)))
    {
        // 发送失败意味着连接状态不再可靠，唤醒等待者并交给 Stop 回收。
        m_connected.store(false);
        m_stopFlag.store(true);
        m_queueCv.notify_all();
        return false;
    }

    return true;
}

// ============================================================
// 帧接收
// ============================================================
PipeServer::ReceiveStatus PipeServer::ReceiveFrame(Frame& out, int timeoutMs)
{
    // 负数只有 -1 表示无限等待，其他负数属于调用错误。
    if (timeoutMs < -1) return ReceiveStatus::Timeout;

    std::unique_lock<std::mutex> lock(m_queueMutex);
    // 有响应、连接断开或显式停止时都应唤醒等待者。
    const auto ready = [this] {
        return !m_frameQueue.empty() || !m_connected.load() || m_stopFlag.load();
    };

    bool signaled = true;
    if (timeoutMs < 0)
    {
        // -1 使用条件变量无限等待，直到出现响应或连接状态变化。
        m_queueCv.wait(lock, ready);
    }
    else if (timeoutMs > 0)
    {
        // 有限等待返回 false 时，调用方可以准确报告命令超时。
        signaled = m_queueCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
    }
    else
    {
        signaled = ready();
    }

    if (!m_frameQueue.empty())
    {
        // 队列优先于连接状态，避免连接刚断开时丢失已经完整收到的响应。
        out = std::move(m_frameQueue.front());
        m_frameQueue.pop_front();
        return ReceiveStatus::Received;
    }

    if (!signaled) return ReceiveStatus::Timeout;
    // 没有响应但状态已变化，只能向上层报告断线/停止。
    return ReceiveStatus::Disconnected;
}

// ============================================================
// 握手帧等待
// ============================================================
bool PipeServer::WaitForFrame(uint8_t expectedType, Frame& out, int timeoutMs)
{
    // 握手帧必须严格按阶段接收；非预期控制帧不会被静默吞掉。
    out = Frame{};
    const auto start = std::chrono::steady_clock::now();

    while (!m_stopFlag.load())
    {
        int remaining = timeoutMs;
        if (timeoutMs > 0)
        {
            // 每次循环用剩余时间，避免收到无关帧后重新获得完整超时。
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            remaining = timeoutMs - static_cast<int>(elapsed);
            if (remaining <= 0) return false;
        }

        const ReceiveStatus status = ReceiveFrame(out, remaining);
        if (status != ReceiveStatus::Received) return false;

        // 预期帧成功返回给握手调用方。
        if (out.type == expectedType) return true;

        // 握手期间的错误和退出都是终止条件，不能被当成 expectedType。
        if (out.type == protocol::MSG_ERROR || out.type == protocol::MSG_EXIT) return false;

        // 不接受握手帧乱序，避免静默丢帧后进入错误状态。
        return false;
    }

    return false;
}
