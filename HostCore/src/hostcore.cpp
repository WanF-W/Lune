// ============================================================
// hostcore.cpp — HostCore 会话控制实现
// ============================================================
// 本文件实现 HostCore 对外 C ABI，并将前端请求串联为一次完整会话：
// 目标定位、DLL 查找、管道创建、DLL 注入、协议握手和命令执行。
// UI、控制台输出和具体后端运行时逻辑不属于本模块。
// ============================================================
#include "hostcore.h"

// HostCore 内部模块
#include "backend_profile.h"
#include "injector.h"
#include "pipe_server.h"
#include "win_handle.h"

// 标准库
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ============================================================
// 会话对象
// ============================================================
// HC_Session 对外只暴露此对象的地址；成员和生命周期均由 HostCore 私有管理。
struct HC_SessionImpl {
    const BackendProfile* backend;       // 当前后端配置
    std::wstring processName, dllPath;   // 目标进程名和 DLL 路径
    DWORD pid;                           // 目标进程 PID
    HC_EventCallback callback;           // 前端事件回调
    HC_CancelCallback cancel;            // 前端取消回调
    void* context;                       // 回调上下文
    Injector injector;                   // 当前会话的 DLL 注入器
    PipeServer pipe;                     // 当前会话的命名管道服务端
    bool started = false;                // 是否已经启动过；会话只能启动一次
    bool ready = false;                  // 后端是否已完成 READY 握手

    // 从 C ABI 参数复制会话所需的字符串和回调，不保留调用方临时对象的引用。
    explicit HC_SessionImpl(const HC_SessionOptions& o)
        : backend(FindBackend(static_cast<wchar_t>(o.backend))),
          processName(o.processName ? o.processName : L""),
          dllPath(o.dllPath ? o.dllPath : L""), pid(o.pid),
          callback(o.eventCallback), cancel(o.cancelCallback), context(o.context) {}

    // 将内部阶段或后端消息转换为前端事件。
    // 文本指针只在回调期间有效；回调异常不得穿过 HostCore 边界。
    void emit(uint32_t kind, const wchar_t* wide = nullptr,
              const char* text = nullptr, uint32_t length = 0,
              int32_t category = 4, int32_t line = -1) noexcept {
        if (!callback) return;
        HC_Event e{kind, pid, wide, text, length, category, line, backend->protocolVersion};
        try { callback(context, &e); } catch (...) { /* Never unwind into transport. */ }
    }
    // 等待循环定期检查取消；前端无需接触管道句柄。
    bool cancelled() { return cancel && cancel(context) != 0; }

    void pipeError(uint32_t kind, uint32_t stageOverride = HC_STAGE_NONE) {
        const auto error = pipe.GetError();
        const uint32_t stage = stageOverride ? stageOverride : static_cast<uint32_t>(error.stage) + HC_STAGE_CONNECT;
        static const char* names[] = {"", "Connect", "Wait", "Complete", "Hello", "Ready"};
        wchar_t* systemText = nullptr;
        const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error.code, 0,
            reinterpret_cast<wchar_t*>(&systemText), 0, nullptr);
        const std::unique_ptr<wchar_t, decltype(&LocalFree)> messageOwner(systemText, &LocalFree);
        std::wstring description;
        if (count && systemText) description.assign(systemText, count);
        if (description.empty()) description = L"System message unavailable (Win32 " + std::to_wstring(error.code) + L")";
        while (!description.empty() && (description.back() == L'\r' || description.back() == L'\n')) description.pop_back();
        HC_Event e{};
        e.kind = kind; e.pid = pid; e.category = 4; e.line = -1;
        e.expectedVersion = backend->protocolVersion;
        e.errorStage = stage; e.errorStageName = names[stage];
        e.win32Error = error.code; e.errorApi = error.api; e.timedOut = error.timeout ? 1 : 0;
        e.errorDescription = description.c_str();
        if (callback) { try { callback(context, &e); } catch (...) {} }
    }

    // 发送一个失败事件并返回统一失败结果。
    HC_Result fail(uint32_t kind, const wchar_t* wide = nullptr) {
        emit(kind, wide); return HC_FAILURE;
    }

    // 解码后端 MSG_ERROR，并转换为统一的 HostCore 后端错误事件。
    void error(const PipeServer::Frame& frame) {
        protocol::ErrorPayloadView view;
        if (protocol::DecodeErrorPayload(frame.payload.data(),
                static_cast<uint32_t>(frame.payload.size()), view))
            emit(HC_BACKEND_ERROR, nullptr, view.message, view.messageLength,
                 static_cast<int32_t>(view.category), view.line);
        else
            emit(HC_BACKEND_ERROR, nullptr,
                 reinterpret_cast<const char*>(frame.payload.data()),
                 static_cast<uint32_t>(frame.payload.size()));
    }
    // 停止当前会话的通信和共享内存；PipeServer 负责线程与管道回收。
    void stop() { pipe.Stop(); injector.CloseSharedMemory(); ready = false; }
};

namespace {
// ============================================================
// 会话内部辅助函数
// ============================================================
// 判断路径是否存在且确实为普通文件。
bool isFile(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 获取当前 Host 进程的完整可执行文件路径，用于解析默认 DLL 目录。
std::wstring executablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!n) return {};
        if (n + 1 < buffer.size()) return std::wstring(buffer.data(), n);
        buffer.resize(buffer.size() * 2);
    }
}

// ============================================================
// 会话启动
// ============================================================
// 执行一次完整的 HostCore 启动流程；调用方负责捕获异常并清理会话。
HC_Result start(HC_SessionImpl& s) {
    // 会话是一次性的；启动失败后由调用方销毁并重新创建会话对象。
    if (s.started) return HC_INVALID_ARGUMENT;
    s.started = true;

    // 先验证目标进程仍可打开，再进入 DLL 查找和管道创建阶段。
    s.emit(HC_LOCATING_TARGET);
    if (s.pid) {
        win::UniqueHandle process(Injector::OpenTargetProcess(s.pid));
        if (!process) return s.fail(HC_TARGET_ERROR);
    } else {
        s.pid = Injector::FindProcessByName(s.processName);
        if (!s.pid) return s.fail(HC_TARGET_ERROR, s.processName.c_str());
    }
    // PID 模式和进程名模式统一转换为已确认的目标 PID。
    const auto name = Injector::GetProcessName(s.pid);
    s.emit(HC_TARGET_FOUND, name.empty() ? L"<unknown>" : name.c_str());

    // 显式 DLL 路径优先；未指定时从当前 Host 可执行文件目录解析默认 DLL。
    s.emit(HC_LOCATING_DLL, s.backend->dllName);
    if (!s.dllPath.empty()) {
        if (!isFile(s.dllPath)) return s.fail(HC_DLL_ERROR, s.dllPath.c_str());
    } else {
        const auto exe = executablePath();
        const auto separator = exe.find_last_of(L"\\/");
        if (separator == std::wstring::npos) return s.fail(HC_DIRECTORY_ERROR);
        s.dllPath = exe.substr(0, separator + 1) + s.backend->dllName;
        if (!isFile(s.dllPath)) return s.fail(HC_DEFAULT_DLL_ERROR, s.backend->dllName);
    }
    s.emit(HC_DLL_FOUND, s.dllPath.c_str());

    // 管道名称和共享内存名称必须使用同一目标 PID 与后端配置。
    wchar_t pipeName[128]{};
    if (swprintf_s(pipeName, 128, L"%s%lu", s.backend->pipePrefix, s.pid) < 0)
        return s.fail(HC_PIPE_NAME_ERROR);
    // 管道日志通过事件回调转交前端；回调本身不由 HostCore 保存文本。
    s.pipe.SetLogCallback([&s](const char* text) {
        s.emit(HC_LOG, nullptr, text, static_cast<uint32_t>(std::strlen(text)));
    });

    // 断线事件用于唤醒前端并结束当前会话。
    s.pipe.SetDisconnectCallback([&s] { s.emit(HC_DISCONNECTED); });
    s.pipe.SetCancelCallback([&s] { return s.cancelled(); });
    s.emit(HC_CREATING_PIPE);
    // 先创建管道，再注入 DLL；Start 的具体连接等待策略由 PipeServer 管理。
    if (!s.pipe.Start(pipeName)) { s.pipeError(HC_PIPE_ERROR); return HC_FAILURE; }
    if (s.cancelled()) return HC_FAILURE;

    // Injector 同时创建共享内存，将完整管道名称传给目标进程内的 DLL。
    s.emit(HC_INJECTING);
    if (!s.injector.Inject(s.pid, s.dllPath, pipeName, s.backend->sharedMemoryPrefix))
        return s.fail(HC_INJECTION_ERROR);
    if (s.cancelled()) return HC_FAILURE;
    s.emit(HC_INJECTED);
    // LoadLibraryW 返回只代表 DLL 已加载；此处继续等待 DLL 工作线程连接管道。
    s.emit(HC_CONNECTING);
    if (!s.pipe.WaitForClient(protocol::HANDSHAKE_TIMEOUT)) {
        s.pipeError(HC_CONNECT_ERROR);
        return HC_FAILURE;
    }
    s.emit(HC_CONNECTED);
    // 传输连接成功后，先校验 DLL 发送的协议版本。
    PipeServer::Frame frame;
    if (!s.pipe.WaitForFrame(protocol::MSG_HELLO, frame, protocol::DLLSAYHELLO_TIMEOUT)) {
        if (frame.type == protocol::MSG_ERROR) s.error(frame);
        else s.pipeError(HC_HELLO_ERROR, HC_STAGE_HELLO);
        return HC_FAILURE;
    }
    const std::string hello(frame.payload.begin(), frame.payload.end());
    if (hello != s.backend->protocolVersion) {
        s.emit(HC_VERSION_ERROR, nullptr, hello.data(), static_cast<uint32_t>(hello.size()));
        return HC_FAILURE;
    }
    s.emit(HC_HELLO, nullptr, hello.data(), static_cast<uint32_t>(hello.size()));

    // HELLO 已经证明 DLL 读出了共享内存，此时可以关闭 Host 侧 mapping 句柄。
    s.injector.CloseSharedMemory();

    // READY 表示后端运行时初始化完成，可以接受 Lua 命令。
    if (!s.pipe.WaitForFrame(protocol::MSG_READY, frame, protocol::DLLSAYREADY_TIMEOUT)) {
        if (frame.type == protocol::MSG_ERROR) s.error(frame);
        else s.pipeError(HC_READY_ERROR, HC_STAGE_READY);
        return HC_FAILURE;
    }
    if (!s.pipe.IsConnected()) {
        s.pipeError(HC_READY_ERROR, HC_STAGE_READY);
        return HC_FAILURE;
    }
    s.ready = true;
    s.emit(HC_READY);
    return HC_SUCCESS;
}

// ============================================================
// 命令执行
// ============================================================
// 发送一条 Lua 命令并等待 OK、ERROR 或 EXIT 终态帧。
HC_Result command(HC_SessionImpl& s, const char* code, uint32_t length, int32_t timeout) {
    // 发送前限制负载大小，避免协议帧分配超出约定上限。
    if (length > protocol::MAX_PAYLOAD) return s.fail(HC_COMMAND_TOO_LARGE);
    if (!s.pipe.SendFrame(protocol::MSG_CMD, code, length)) return s.fail(HC_SEND_ERROR);

    // 截止时间从发送完成后开始计算；日志回调独立处理，不能重置命令超时。
    const auto begin = std::chrono::steady_clock::now();
    for (;;) {
        if (s.cancelled()) { s.stop(); return HC_FAILURE; }
        int remaining = timeout;
        if (timeout > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin).count();
            remaining = timeout - static_cast<int>(elapsed);
            if (remaining <= 0) {
                s.emit(HC_COMMAND_TIMEOUT); s.pipe.Stop(); return HC_FAILURE;
            }
        }
        // 以短间隔轮询响应，使取消和断线状态能够及时返回前端。
        PipeServer::Frame frame;
        const int wait = remaining > 0 ? (remaining < 50 ? remaining : 50) : (remaining < 0 ? 50 : 0);
        const auto status = s.pipe.ReceiveFrame(frame, wait);
        if (status == PipeServer::ReceiveStatus::Timeout) continue;
        if (status == PipeServer::ReceiveStatus::Disconnected) return s.fail(HC_CONNECTION_LOST);
        // 只有终态帧结束当前命令；日志帧由 PipeServer 直接回调，不会进入此处。
        switch (frame.type) {
        case protocol::MSG_OK: return HC_SUCCESS;
        case protocol::MSG_ERROR: s.error(frame); return HC_FAILURE;
        case protocol::MSG_EXIT:
            s.emit(HC_EXIT_REQUESTED); s.pipe.Stop(); return HC_FAILURE;
        default: continue;
        }
    }
}
}

// ============================================================
// HostCore C ABI
// ============================================================
// 以下函数是前端调用 HostCore 的唯一入口；实现细节不向调用方暴露。
const HC_BackendInfo* HC_CALL HC_GetBackendInfo(uint32_t backend) {
    static const HC_BackendInfo info[] = {
        {IL2CPP_PROFILE.cliName, IL2CPP_PROFILE.runtimeName, IL2CPP_PROFILE.runtimeVersion,
         IL2CPP_PROFILE.runtimeTarget, IL2CPP_PROFILE.dllName, IL2CPP_PROFILE.prompt},
        {MONO_PROFILE.cliName, MONO_PROFILE.runtimeName, MONO_PROFILE.runtimeVersion,
         MONO_PROFILE.runtimeTarget, MONO_PROFILE.dllName, MONO_PROFILE.prompt},
        {UNREAL_PROFILE.cliName, UNREAL_PROFILE.runtimeName, UNREAL_PROFILE.runtimeVersion,
         UNREAL_PROFILE.runtimeTarget, UNREAL_PROFILE.dllName, UNREAL_PROFILE.prompt}
    };
    switch (backend) { case 'i': return &info[0]; case 'm': return &info[1];
                      case 'u': return &info[2]; default: return nullptr; }
}

// 创建并初始化一次性会话对象。
HC_Result HC_CALL HC_CreateSession(const HC_SessionOptions* o, HC_Session* out) {
    if (!out) return HC_INVALID_ARGUMENT;
    *out = nullptr;
    if (!o || o->structSize != sizeof(*o) || o->abiVersion != HC_ABI_VERSION
        || !HC_GetBackendInfo(o->backend)
        || ((o->pid != 0) == (o->processName && *o->processName))) return HC_INVALID_ARGUMENT;
    try { *out = new HC_SessionImpl(*o); return HC_SUCCESS; }
    catch (...) { return HC_FAILURE; }
}

// 启动目标定位、注入和 HELLO/READY 握手流程。
HC_Result HC_CALL HC_StartSession(HC_Session s) {
    if (!s) return HC_INVALID_ARGUMENT;
    if (s->started) return HC_INVALID_ARGUMENT;
    try {
        const HC_Result result = start(*s);
        if (result != HC_SUCCESS) s->stop();
        return result;
    } catch (...) { HC_StopSession(s); return HC_FAILURE; }
}

// 向已经 READY 的后端发送 Lua 代码并等待终态响应。
HC_Result HC_CALL HC_SendCommand(HC_Session s, const char* code, uint32_t length, int32_t timeout) {
    if (!s || !s->ready || (!code && length) || timeout < -1) return HC_INVALID_ARGUMENT;
    try {
        const HC_Result result = command(*s, code, length, timeout);
        if (!s->pipe.IsConnected()) s->stop();
        return result;
    } catch (...) { HC_StopSession(s); return HC_FAILURE; }
}

// 状态查询接口只读取当前状态，不改变会话生命周期。
int32_t HC_CALL HC_IsConnected(HC_Session s) { return s && s->pipe.IsConnected(); }
int32_t HC_CALL HC_IsDisconnected(HC_Session s) {
    if (!s) return 1;
    const HANDLE event = s->pipe.GetDisconnectEventHandle();
    return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

// 请求后端退出，并等待其回送退出确认帧。
HC_Result HC_CALL HC_RequestExit(HC_Session s) {
    if (!s) return HC_INVALID_ARGUMENT;
    try {
        if (!s->pipe.SendFrame(protocol::MSG_EXIT, nullptr, 0)) return HC_FAILURE;
        PipeServer::Frame frame;
        return s->pipe.ReceiveFrame(frame, protocol::EXIT_ACK_TIMEOUT) == PipeServer::ReceiveStatus::Received
            && frame.type == protocol::MSG_EXIT ? HC_SUCCESS : HC_FAILURE;
    } catch (...) { return HC_FAILURE; }
}

// 停止、销毁和默认超时查询接口。
void HC_CALL HC_StopSession(HC_Session s) { if (s) { try { s->stop(); } catch (...) {} } }
void HC_CALL HC_DestroySession(HC_Session s) { if (s) { HC_StopSession(s); delete s; } }
int32_t HC_CALL HC_GetCommandTimeout(void) { return protocol::COMMAND_TIMEOUT; }
int32_t HC_CALL HC_GetStartupScriptTimeout(void) { return protocol::LUAFILE_TIMEOUT; }
