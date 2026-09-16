// ============================================================
// protocol.h — Lune 通用二进制帧通信协议
// ============================================================
// 定义 Lune 与配套 DLL 之间共有的消息类型和帧格式。
// 帧格式：[1 字节类型][4 字节小端长度][N 字节负载]，最大负载为 1 MiB。
// DLL 的协议版本和 IPC 名称由 backend_profile 提供；它们必须与配套 DLL
// 的协议定义保持一致。
// ============================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace protocol
{
    // ============================================================
    // 消息类型定义
    // ============================================================
    enum MessageType : uint8_t
    {
        // DLL → EXE
        MSG_HELLO = 0x10,   // 握手：负载为当前后端 profile 的协议版本
        MSG_READY = 0x11,   // 初始化就绪：负载为状态描述文本
        MSG_LOG   = 0x20,   // Lua print 输出：负载为输出文本
        MSG_ERROR = 0x21,   // 错误信息：负载为 [类别][可选行号][错误描述]
        MSG_OK    = 0x22,   // 命令执行成功：无负载

        // EXE → DLL
        MSG_CMD   = 0x30,   // 执行 Lua 代码：负载为 Lua 源码
        MSG_FILE  = 0x31,   // 执行 Lua 文件：负载为文件路径

        // 双向
        MSG_EXIT  = 0xFF,   // 退出通知：无负载
    };

    // 最大负载大小：1 MB
    constexpr size_t MAX_PAYLOAD = 1024 * 1024;

    // MSG_ERROR 的统一错误层级。错误文本本身不再混入 Lua 的 source name，
    // 由协议显式传输类别和可选行号，CLI 只负责展示。
    enum class ErrorCategory : uint8_t
    {
        Lua = 1,
        Il2Cpp = 2,
        CSharp = 3,
        Lune = 4,
        Mono = 5,
    };

    constexpr size_t ERROR_HEADER_SIZE = 5; // 类别 1 字节 + 行号 4 字节 LE

    struct ErrorPayloadView
    {
        ErrorCategory category = ErrorCategory::Lune;
        int32_t line = -1;
        const char* message = nullptr;
        uint32_t messageLength = 0;
    };

    inline bool DecodeErrorPayload(const uint8_t* data, uint32_t length, ErrorPayloadView& view)
    {
        if (data == nullptr || length < ERROR_HEADER_SIZE)
        {
            return false;
        }

        const uint8_t rawCategory = data[0];
        if (rawCategory < static_cast<uint8_t>(ErrorCategory::Lua)
            || rawCategory > static_cast<uint8_t>(ErrorCategory::Mono))
        {
            return false;
        }

        const uint32_t encodedLine = static_cast<uint32_t>(data[1])
            | (static_cast<uint32_t>(data[2]) << 8)
            | (static_cast<uint32_t>(data[3]) << 16)
            | (static_cast<uint32_t>(data[4]) << 24);

        view.category = static_cast<ErrorCategory>(rawCategory);
        view.line = static_cast<int32_t>(encodedLine);
        view.message = reinterpret_cast<const char*>(data + ERROR_HEADER_SIZE);
        view.messageLength = length - static_cast<uint32_t>(ERROR_HEADER_SIZE);
        return true;
    }


    // ============================================================
    // 协议常量
    // ============================================================

    // 帧头大小：1 字节类型 + 4 字节长度 = 5 字节
    constexpr size_t HEADER_SIZE = 5;

    // LoadLibraryW 执行超时（DLL 注入后执行 DllMain 超时）
    constexpr DWORD DLLINIT_TIMEOUT = 10000;

    // 等待 DLL 连接控制端管道的超时：15 秒。
    constexpr int HANDSHAKE_TIMEOUT = 15000;

    // Hello超时：5 秒（DLL 连接命名管道后发送 Hello 允许延迟时间）
    constexpr int DLLSAYHELLO_TIMEOUT = 5000;

    // Ready 超时：45 秒（DLL 完成运行时和 Lua 初始化后发送 Ready）。
    constexpr int DLLSAYREADY_TIMEOUT = 45000;

    // 命令执行超时：30 秒
    constexpr int COMMAND_TIMEOUT = 30000;

    // 启动脚本执行超时：60 秒
    constexpr int LUAFILE_TIMEOUT = 60000;

    // 主动退出时等待 DLL 回送 MSG_EXIT 确认；超时后仍会关闭管道，
    // DLL 会通过管道断开路径自行收尾。
    constexpr int EXIT_ACK_TIMEOUT = 3000;

    // 共享内存最大大小（字节） 足够容纳一个管道名称
    constexpr size_t SHARED_MEM_SIZE = 512;
}
