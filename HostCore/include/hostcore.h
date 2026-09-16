// ============================================================
// hostcore.h — HostCore 对外 C ABI 声明
// ============================================================
// HostCore 为前端提供进程定位、DLL 注入、命名管道会话和命令控制能力。
// 本头文件只暴露稳定的 C ABI；具体线程、句柄、管道和协议实现保持私有。
// 仅针对 Windows x64。
// ============================================================
#pragma once

// C 基础类型；接口不得依赖 C++ STL 或编译器特定的 C++ ABI。
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

// ============================================================
// 导出与调用约定
// ============================================================
#ifdef HOSTCORE_EXPORTS
#define HC_API __declspec(dllexport)
#else
#define HC_API __declspec(dllimport)
#endif
#define HC_CALL __cdecl

// 保持结构体布局在不同调用方之间一致。
#pragma pack(push, 8)
#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// 基础类型与返回值
// ============================================================
// 会话使用不透明句柄，内部对象和生命周期由 HostCore 管理。
typedef struct HC_SessionImpl* HC_Session;

// 所有公开函数统一使用此结果类型。
typedef int32_t HC_Result;
#define HC_SUCCESS 0
#define HC_FAILURE 1
#define HC_INVALID_ARGUMENT 2
#define HC_ABI_VERSION 2

// ============================================================
// 后端展示信息
// ============================================================
// 仅用于前端展示；管道名称、共享内存名称和其他 IPC 配置由 HostCore 私有管理。
// 返回指针指向 HostCore 的静态数据，只读且在 DLL 卸载前有效。
typedef struct HC_BackendInfo {
    const wchar_t* cliName;         // 传统 CLI 后端名称。
    const wchar_t* runtimeName;     // 运行时名称。
    const wchar_t* runtimeVersion;  // 后端协议版本的展示文本。
    const wchar_t* runtimeTarget;   // 目标运行时名称。
    const wchar_t* dllName;         // 默认后端 DLL 文件名。
    const wchar_t* prompt;          // CLI 前端使用的提示符。
} HC_BackendInfo;

// ============================================================
// 会话事件
// ============================================================
// 事件类型按会话阶段、后端消息和状态变化分组。
enum HC_EventKind {
    HC_LOCATING_TARGET = 1, HC_TARGET_FOUND, HC_TARGET_ERROR,
    HC_LOCATING_DLL, HC_DLL_FOUND, HC_DLL_ERROR, HC_DIRECTORY_ERROR,
    HC_DEFAULT_DLL_ERROR, HC_PIPE_NAME_ERROR, HC_CREATING_PIPE, HC_PIPE_ERROR,
    HC_INJECTING, HC_INJECTION_ERROR, HC_INJECTED, HC_CONNECTING,
    HC_CONNECT_ERROR, HC_CONNECTED, HC_HELLO_ERROR, HC_VERSION_ERROR,
    HC_HELLO, HC_READY_ERROR, HC_READY, HC_LOG, HC_BACKEND_ERROR,
    HC_COMMAND_TOO_LARGE, HC_SEND_ERROR, HC_COMMAND_TIMEOUT,
    HC_CONNECTION_LOST, HC_EXIT_REQUESTED, HC_DISCONNECTED
};

enum HC_ErrorStage {
    HC_STAGE_NONE = 0, HC_STAGE_CONNECT, HC_STAGE_WAIT, HC_STAGE_COMPLETE,
    HC_STAGE_HELLO, HC_STAGE_READY
};

// 事件文本只在回调持续期间借用有效，不得由调用方释放或长期保存指针。
// text 为 UTF-8，并通过 textLength 指定字节长度；wideText 为 Windows UTF-16。
// category/line 沿用后端现有错误语义：1 Lua、2 Il2Cpp、3 CSharp、4 Lune、5 Mono。
// 回调可能运行在调用线程或管道读取线程；回调不得抛出异常或调用会改变会话状态的 API。
// UI 如需异步处理，必须在回调期间复制文本和所需字段。
typedef struct HC_Event {
    uint32_t kind;                  // HC_EventKind。
    uint32_t pid;                   // 当前目标进程 PID。
    const wchar_t* wideText;        // UTF-16 文本，适合路径和进程名。
    const char* text;               // UTF-8 文本，适合日志和错误描述。
    uint32_t textLength;            // text 的字节长度，不包含结尾零。
    int32_t category;               // 后端错误类别；非错误事件通常为 4。
    int32_t line;                   // 后端错误行号；未知时为 -1。
    const char* expectedVersion; // HC_VERSION_ERROR 使用的静态后端版本。
    uint32_t errorStage;            // HC_ErrorStage；普通事件为 HC_STAGE_NONE。
    uint32_t win32Error;            // 原始 DWORD；逻辑错误使用对应系统错误码。
    const char* errorApi;           // 失败的 Win32 API；逻辑错误为 NULL。
    int32_t timedOut;               // 由 HostCore 判断，前端不推断管道状态。
    const char* errorStageName;     // Connect / Wait / Complete / Hello / Ready。
    const wchar_t* errorDescription; // FormatMessageW 文本，只在回调期间有效。
} HC_Event;

// 事件回调由 HostCore 同步调用；context 原样回传给调用方。
typedef void (HC_CALL *HC_EventCallback)(void* context, const HC_Event* event);

// 返回非零表示调用方要求取消当前会话等待。
typedef int32_t (HC_CALL *HC_CancelCallback)(void* context);

// ============================================================
// 会话创建参数
// ============================================================
// pid 与 processName 必须且只能提供一个；dllPath 为空时使用 HostCore 默认路径。
typedef struct HC_SessionOptions {
    uint32_t structSize;            // 必须填写 sizeof(HC_SessionOptions)。
    uint32_t abiVersion;            // 必须匹配 HC_ABI_VERSION。
    uint32_t backend; // ASCII 'i'、'm' 或 'u'。
    uint32_t pid;     // 非零 PID；与 processName 必须且只能提供一个。
    const wchar_t* processName;     // 目标进程名；使用 PID 时传 NULL 或空字符串。
    const wchar_t* dllPath; // NULL/空字符串：使用 HostCore 默认 DLL 路径。
    HC_EventCallback eventCallback; // 状态、日志和错误事件回调，可为 NULL。
    HC_CancelCallback cancelCallback; // 等待期间的取消检查回调，可为 NULL。
    void* context;                   // 回调上下文，不由 HostCore 解释或释放。
} HC_SessionOptions;

// ============================================================
// HostCore 公共接口
// ============================================================
// 根据后端标识返回静态展示信息；backend 使用 ASCII 'i'、'm' 或 'u'。
HC_API const HC_BackendInfo* HC_CALL HC_GetBackendInfo(uint32_t backend);

// 创建一次性会话。成功后由 HC_DestroySession 释放。
HC_API HC_Result HC_CALL HC_CreateSession(const HC_SessionOptions* options, HC_Session* session);

// 启动一次性会话，依次执行目标定位、管道创建、DLL 注入、HELLO 和 READY。
// Start/Send/Exit/Stop/Destroy 必须由会话所有者串行调用；状态查询允许并发调用。
HC_API HC_Result HC_CALL HC_StartSession(HC_Session session);

// 发送 Lua 代码并等待终态响应。timeoutMs 为 -1 时无限等待；0 保持旧版轮询语义。
HC_API HC_Result HC_CALL HC_SendCommand(HC_Session session, const char* code, uint32_t length, int32_t timeoutMs);

// 查询当前是否已经建立管道连接。
HC_API int32_t HC_CALL HC_IsConnected(HC_Session session);

// 查询连接断开或会话停止状态。
HC_API int32_t HC_CALL HC_IsDisconnected(HC_Session session);

// 请求后端退出并等待退出确认。
HC_API HC_Result HC_CALL HC_RequestExit(HC_Session session);

// 停止会话并释放管道、线程和共享内存等运行时资源；可重复调用。
HC_API void HC_CALL HC_StopSession(HC_Session session);

// 停止并销毁会话；销毁后句柄不可继续使用。
HC_API void HC_CALL HC_DestroySession(HC_Session session);

// 返回默认命令执行超时时间，单位为毫秒。
HC_API int32_t HC_CALL HC_GetCommandTimeout(void);

// 返回默认启动脚本执行超时时间，单位为毫秒。
HC_API int32_t HC_CALL HC_GetStartupScriptTimeout(void);
#ifdef __cplusplus
}
#endif
#pragma pack(pop)
