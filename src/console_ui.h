// ============================================================
// console_ui.h — Lune 控制台交互层
//
// 负责控制台输入、UTF-8/UTF-16 转换、颜色输出和异步日志排版。
// 本模块不参与协议解析，也不决定 Lua 命令的执行结果。
// ============================================================
#pragma once

#include <windows.h>

#include <string>

namespace ui
{
    // ============================================================
    // 控制台颜色
    // ============================================================
    namespace color
    {
        // 设置后续控制台文本颜色；重定向输出时调用会自然失效。
        void Reset();
        void Red();
        void Green();
        void Yellow();
        void Cyan();
        void Gray();
    }

    // ============================================================
    // 文本输出
    // ============================================================
    // 将协议中的 UTF-8 文本安全输出到控制台或重定向目标。
    void SafePrintUtf8(const std::string& text, bool useStderr = false);

    // 后台管道线程复制日志后立即排出；队列只用于保护跨线程输出期间的日志。
    void QueueAsyncLog(const char* text);

    // 取出当前排队的日志并输出。addSeparator 为 true 时在日志块后补一个空行。
    // 返回 true 表示本次确实取出了日志。
    bool DrainAsyncLogs(bool addSeparator);

    // 从控制台读取一行 UTF-16 输入，再转换为发送给 DLL 的 UTF-8。
    // cancelEvent 由会话层提供；目标进程/管道断开时可打断当前输入。
    bool ReadLineUtf8(std::string& outUtf8, HANDLE cancelEvent = nullptr);

    // 以下状态只用于异步日志和提示符之间的排版协调。
    void SetPromptActive(bool active);
    void SetPromptSeparate();

    // 设置当前后端的会话提示符；字符串由 BackendProfile 静态持有。
    void SetPrompt(const wchar_t* prompt);
    void PrintPrompt();
}
