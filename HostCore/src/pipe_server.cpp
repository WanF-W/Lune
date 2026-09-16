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
    bool Transfer(HANDLE pipe, void* buffer, DWORD length, DWORD& transferred,
                  bool write, std::mutex& handleMutex, const std::atomic<bool>& stopped,
                  PipeServer::Error& error, const std::function<bool()>& cancel = {})
    {
        const auto fail = [&](const char* api, DWORD code) {
            error = {PipeServer::Stage::Complete, api, code, false};
            return false;
        };
        win::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return fail("CreateEventW", GetLastError());
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.Get();
        transferred = 0;
        BOOL completed;
        DWORD code = ERROR_SUCCESS;
        {
            // 停止检查与提交必须和 CancelIoEx 使用同一把锁，避免取消后又提交 I/O。
            std::lock_guard<std::mutex> lock(handleMutex);
            if (stopped.load()) return fail(nullptr, ERROR_OPERATION_ABORTED);
            completed = write ? WriteFile(pipe, buffer, length, &transferred, &overlapped)
                              : ReadFile(pipe, buffer, length, &transferred, &overlapped);
            if (!completed) code = GetLastError();
        }
        if (!completed)
        {
            if (code != ERROR_IO_PENDING) return fail(write ? "WriteFile" : "ReadFile", code);
            DWORD wait;
            for (;;) {
                wait = WaitForSingleObject(event.Get(), cancel ? 50 : INFINITE);
                if (wait != WAIT_TIMEOUT) break;
                bool cancelled = false;
                try { cancelled = cancel && cancel(); }
                catch (...) { cancelled = true; }
                if (cancelled) {
                    CancelIoEx(pipe, &overlapped);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                    return fail(nullptr, ERROR_OPERATION_ABORTED);
                }
            }
            if (wait != WAIT_OBJECT_0) {
                code = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
                CancelIoEx(pipe, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                return fail("WaitForSingleObject", code);
            }
            if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE))
                return fail("GetOverlappedResult", GetLastError());
        }
        return transferred > 0 || fail(write ? "WriteFile" : "ReadFile", ERROR_BROKEN_PIPE);
    }

    bool WritePipeOverlapped(HANDLE pipe, const void* buffer, DWORD length,
                            std::mutex& handleMutex, const std::atomic<bool>& stopped,
                            PipeServer::Error& error, const std::function<bool()>& cancel)
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
            DWORD written = 0;
            if (!Transfer(pipe, const_cast<uint8_t*>(static_cast<const uint8_t*>(buffer)) + totalWritten,
                          length - totalWritten, written, true, handleMutex, stopped, error, cancel)) return false;
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

PipeServer::Error PipeServer::GetError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
}

bool PipeServer::Fail(Stage stage, const char* api, DWORD code, bool timeout)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    // 保留最初失败，取消和清理不能覆盖真实原因。
    if (m_error.code == ERROR_SUCCESS) m_error = {stage, api, code, timeout};
    return false;
}

bool PipeServer::Start(const wchar_t* pipeName)
{
    Stop();
    { std::lock_guard<std::mutex> lock(m_errorMutex); m_error = {}; }
    if (!pipeName || !*pipeName) return Fail(Stage::Connect, nullptr, ERROR_INVALID_PARAMETER);
    if (!m_disconnectEvent) {
        m_disconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_disconnectEvent) return Fail(Stage::Connect, "CreateEventW", GetLastError());
    }
    std::lock_guard<std::mutex> lock(m_handleMutex);
    m_connect = {};
    m_connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_connect.hEvent) return Fail(Stage::Connect, "CreateEventW", GetLastError());
    m_pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, nullptr);
    if (m_pipe == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        CloseHandle(m_connect.hEvent);
        m_connect = {};
        return Fail(Stage::Connect, "CreateNamedPipeW", code);
    }
    m_stopFlag.store(false);
    ResetEvent(m_disconnectEvent);
    // 在注入前提交；OVERLAPPED 与事件由当前会话持有。
    if (ConnectNamedPipe(m_pipe, &m_connect)) {
        m_connectionState = ConnectionState::Connected;
        return true;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_IO_PENDING) {
        m_connectionState = ConnectionState::Pending;
        return true;
    }
    if (code == ERROR_PIPE_CONNECTED) {
        m_connectionState = ConnectionState::Connected;
        return true;
    }
    CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
    CloseHandle(m_connect.hEvent);
    m_connect = {};
    m_stopFlag.store(true);
    SetEvent(m_disconnectEvent);
    return Fail(Stage::Connect, "ConnectNamedPipe", code);
}

void PipeServer::RequestStop() noexcept
{
    // 先发布状态，让等待线程和发送线程尽快停止继续工作。
    {
        // 与 ReceiveFrame 的谓词检查同步，避免无限等待丢失断线唤醒。
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_stopFlag.store(true);
        m_connected.store(false);
    }

    // 不关闭句柄，只取消挂起 I/O；关闭动作必须等所有使用者退出后进行。
    std::lock_guard<std::mutex> lock(m_handleMutex);
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        // 取消连接、读取和写入；句柄由 Stop 在所有线程退出后关闭。
        CancelIoEx(m_pipe, nullptr);
    }

    if (m_disconnectEvent) SetEvent(m_disconnectEvent);
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

    ClosePipe();

    {
        // 队列中的旧响应属于旧连接，重新 Start 时不能交给新连接消费。
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_frameQueue.clear();
    }

    m_connected.store(false);
    if (m_disconnectEvent != nullptr) SetEvent(m_disconnectEvent);
    m_queueCv.notify_all();
}

// 调用方持有写锁和句柄锁，读取线程已经退出或正执行最后的清理。
void PipeServer::ClosePipe()
{
    if (m_pipe != INVALID_HANDLE_VALUE) {
        if (m_connectionState == ConnectionState::Pending) {
            CancelIoEx(m_pipe, &m_connect);
            DWORD ignored = 0;
            GetOverlappedResult(m_pipe, &m_connect, &ignored, TRUE);
        }
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    if (m_connect.hEvent) CloseHandle(m_connect.hEvent);
    m_connect = {};
    m_connectionState = ConnectionState::Idle;
}

bool PipeServer::WaitForClient(int timeoutMs)
{
    if (timeoutMs < -1) return Fail(Stage::Wait, nullptr, ERROR_INVALID_PARAMETER);
    if (m_pipe == INVALID_HANDLE_VALUE) return Fail(Stage::Wait, nullptr, ERROR_INVALID_HANDLE);
    if (m_readerThread.joinable()) return Fail(Stage::Wait, nullptr, ERROR_INVALID_STATE);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    while (m_connectionState == ConnectionState::Pending) {
        if (m_stopFlag.load() || (m_cancelCallback && m_cancelCallback())) {
            Fail(Stage::Wait, nullptr, ERROR_OPERATION_ABORTED);
            Stop();
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const DWORD waitMs = timeoutMs < 0 ? 50 : static_cast<DWORD>(remaining <= 0 ? 0 : (remaining < 50 ? remaining : 50));
        const DWORD result = WaitForSingleObject(m_connect.hEvent, waitMs);
        if (result == WAIT_OBJECT_0) {
            DWORD ignored = 0;
            if (!GetOverlappedResult(m_pipe, &m_connect, &ignored, FALSE)) {
                const DWORD code = GetLastError();
                Fail(Stage::Complete, "GetOverlappedResult", code);
                Stop();
                return false;
            }
            m_connectionState = ConnectionState::Connected;
            break;
        }
        if (result == WAIT_TIMEOUT) {
            if (timeoutMs < 0 || std::chrono::steady_clock::now() < deadline) continue;
            Fail(Stage::Wait, "WaitForSingleObject", WAIT_TIMEOUT, true);
        } else {
            const DWORD code = result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
            Fail(Stage::Wait, "WaitForSingleObject", code);
        }
        Stop();
        return false;
    }
    if (m_stopFlag.load() || (m_cancelCallback && m_cancelCallback())) {
        Fail(Stage::Wait, nullptr, ERROR_OPERATION_ABORTED);
        Stop();
        return false;
    }
    m_connected.store(true);
    m_readerThread = std::thread([this] {
        try { ReaderLoop(); }
        catch (...) {
            Fail(Stage::Complete, nullptr, ERROR_NOT_ENOUGH_MEMORY);
            RequestStop();
            { std::lock_guard<std::mutex> writeLock(m_writeMutex);
              std::lock_guard<std::mutex> handleLock(m_handleMutex); ClosePipe(); }
            if (m_disconnectCallback) { try { m_disconnectCallback(); } catch (...) {} }
        }
    });
    return true;
}

// ============================================================
// 后台读取线程
// ============================================================
void PipeServer::ReaderLoop()
{
    Error readError;
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
            if (!Transfer(
                    pipe,
                    header + totalRead,
                    static_cast<DWORD>(protocol::HEADER_SIZE) - totalRead,
                    chunk, false, m_handleMutex, m_stopFlag, readError))
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
        if (!IsKnownMessageType(type) || length > protocol::MAX_PAYLOAD) {
            readError = {Stage::Complete, nullptr, ERROR_INVALID_DATA, false};
            break;
        }

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
                if (!Transfer(pipe, frame.payload.data() + totalRead, length - totalRead, chunk, false, m_handleMutex, m_stopFlag, readError))
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

        if (queueOverflow) {
            readError = {Stage::Complete, nullptr, ERROR_BUFFER_OVERFLOW, false};
            break;
        }
        // 唤醒正在等待 OK/ERROR/EXIT 的主线程。
        m_queueCv.notify_one();
    }

    if (readError.code != ERROR_SUCCESS)
        Fail(readError.stage, readError.api, readError.code);
    else if (!m_stopFlag.load()) Fail(Stage::Complete, nullptr, ERROR_GEN_FAILURE);
    RequestStop();
    {
        std::lock_guard<std::mutex> writeLock(m_writeMutex);
        std::lock_guard<std::mutex> handleLock(m_handleMutex);
        ClosePipe();
    }
    // 已完整接收的终态响应仍允许消费；Stop 清除剩余旧会话队列。

    if (m_disconnectEvent != nullptr) SetEvent(m_disconnectEvent);
    if (m_disconnectCallback) {
        try { m_disconnectCallback(); } catch (...) {}
    }
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

    Error writeError;
    // 先写帧头，再写负载；两次写入仍受同一把锁保护。
    if (!WritePipeOverlapped(pipe, header, static_cast<DWORD>(protocol::HEADER_SIZE), m_handleMutex, m_stopFlag, writeError, m_cancelCallback)
        || (len > 0 && !WritePipeOverlapped(pipe, data, len, m_handleMutex, m_stopFlag, writeError, m_cancelCallback)))
    {
        // 发送失败意味着连接状态不再可靠，唤醒等待者并交给 Stop 回收。
        Fail(writeError.stage, writeError.api, writeError.code);
        RequestStop();
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
    out = Frame{};
    const Stage stage = expectedType == protocol::MSG_HELLO ? Stage::Hello : Stage::Ready;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        if (m_cancelCallback && m_cancelCallback()) {
            Fail(stage, nullptr, ERROR_OPERATION_ABORTED);
            RequestStop();
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const int wait = timeoutMs < 0 ? 50 : static_cast<int>(remaining <= 0 ? 0 : (remaining < 50 ? remaining : 50));
        const auto status = ReceiveFrame(out, wait);
        if (status == ReceiveStatus::Timeout) {
            if (timeoutMs < 0 || std::chrono::steady_clock::now() < deadline) continue;
            return Fail(stage, nullptr, ERROR_TIMEOUT, true);
        }
        if (status == ReceiveStatus::Disconnected)
            return Fail(stage, nullptr, ERROR_BROKEN_PIPE);
        if (out.type == expectedType) return true;
        if (out.type == protocol::MSG_ERROR) return false;
        return Fail(stage, nullptr, out.type == protocol::MSG_EXIT ? ERROR_BROKEN_PIPE : ERROR_INVALID_DATA);
    }
}
