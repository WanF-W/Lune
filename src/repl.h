// ============================================================
// repl.h — Lua REPL 业务层
//
// 只负责把用户输入转换为协议命令并处理命令响应，不负责控制台布局、
// 管道 I/O 细节或 DLL 注入。
// ============================================================
#pragma once

#include <string>
#include <vector>

class PipeServer;

namespace repl
{
    // 判断输入是否应按 Lua 语句原样执行。
    // 表达式由调用方包装为 return <expr>，从而显示返回值；赋值、控制流、
    // 函数定义、注释和多行输入保持原样。该函数不是 Lua 解析器，最终语法
    // 检查仍由游戏内 Lua 完成。
    bool IsStatement(const std::string& input);

    // 统一展示结构化 MSG_ERROR；握手阶段和命令阶段使用同一格式。
    void PrintErrorPayload(const std::vector<uint8_t>& payload);

    // 发送一条 Lua 命令并等待最终响应。
    // MSG_LOG 由 PipeServer 的后台线程交给 UI 队列；本函数只消费 MSG_OK、
    // MSG_ERROR 和 MSG_EXIT。超时会终止当前连接，避免迟到响应污染下一条命令。
    // 参数：server 为已完成握手的管道服务端，code 为 UTF-8 Lua 源码，
    // timeoutMs 为等待最终响应的超时时间，-1 表示无限等待。
    // 返回 true 表示收到 MSG_OK；false 表示错误、断线、退出或超时。
    bool ExecuteCommand(PipeServer& server, const std::string& code, int timeoutMs);
}
