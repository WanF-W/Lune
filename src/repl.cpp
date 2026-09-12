// ============================================================
// repl.cpp — Lua 输入分类与命令响应处理
//
// IsStatement 是一个有意保持轻量的词法判断器，不试图重新实现 Lua
// 解析器；它只决定是否需要为表达式加上 return。
// ============================================================
#include "repl.h"

#include "console_ui.h"
#include "pipe_server.h"
#include "protocol.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>

namespace repl
{
    // ============================================================
    // Lua 输入词法辅助
    // ============================================================
    namespace
    {
        bool IsIdentifierCharacter(char ch)
        {
            // Lua 关键字后面如果紧跟字母、数字或下划线，就只是更长的标识符。
            // 例如 localx 不能被当成 local 语句。
            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
        }

        // 查找赋值号时跳过字符串、注释和嵌套表达式，避免把
        // print("a=b") 或 foo({x = 1}) 误判为赋值语句。
        bool ContainsAssignment(const std::string& input, size_t start)
        {
            enum class State
            {
                Code,        // 正常代码区，可以识别括号和赋值号。
                SingleQuote, // 单引号字符串，反斜杠可以转义下一个字符。
                DoubleQuote, // 双引号字符串，处理规则与单引号相同。
                LongString,  // Lua 长字符串，例如 [[text]] 或 [=[text]=]。
                LongComment, // Lua 长注释，例如 --[[comment]]。
            };

            // state 决定当前字符是否具有语法含义；三个深度只在 Code 状态更新。
            State state = State::Code;
            int longStringLevel = 0;
            int parenthesisDepth = 0;
            int braceDepth = 0;
            int bracketDepth = 0;

            // 只把最外层的单个 '=' 视为赋值。嵌套表、函数参数和字符串中的
            // '=' 都属于表达式内容，不能改变 REPL 的自动回显策略。
            for (size_t i = start; i < input.size(); ++i)
            {
                // 每次循环只处理当前字符；遇到转义字符时会额外跳过一个字符。
                const char ch = input[i];

                if (state == State::SingleQuote)
                {
                    // 引号中的赋值号只是字符串内容，不能影响语句分类。
                    if (ch == '\\') ++i;
                    // 关闭引号后，后面的字符重新回到代码状态。
                    else if (ch == '\'') state = State::Code;
                    continue;
                }

                if (state == State::DoubleQuote)
                {
                    // 双引号字符串同样跳过转义字符和字符串内部的引号。
                    if (ch == '\\') ++i;
                    else if (ch == '"') state = State::Code;
                    continue;
                }

                if (state == State::LongString || state == State::LongComment)
                {
                    // 长字符串/长注释只有匹配同级别的 ]=] 才结束。
                    if (ch == ']')
                    {
                        // 统计结束标记中的等号数量，支持 [=[...]=] 等写法。
                        size_t close = i + 1;
                        while (close < input.size() && input[close] == '=') ++close;
                        if (close < input.size() && input[close] == ']'
                            && static_cast<int>(close - i - 1) == longStringLevel)
                        {
                            // 将循环位置移到结束标记末尾，避免再次处理其中的字符。
                            i = close;
                            state = State::Code;
                        }
                    }
                    continue;
                }

                if (ch == '-' && i + 1 < input.size() && input[i + 1] == '-')
                {
                    // 先判断 -- 后面是否是长注释起始符，否则剩余内容都是单行注释。
                    const size_t bracketStart = i + 2;
                    size_t open = bracketStart + 1;
                    while (open < input.size() && input[open] == '=') ++open;
                    if (bracketStart < input.size() && input[bracketStart] == '['
                        && open < input.size() && input[open] == '[')
                    {
                        // 记录长注释级别，并从第二个 [ 之后继续扫描内容。
                        longStringLevel = static_cast<int>(open - bracketStart - 1);
                        i = open;
                        state = State::LongComment;
                    }
                    else
                    {
                        // 单行注释会吞掉整行，后面不可能再出现有效赋值。
                        i = input.size();
                        break;
                    }
                    continue;
                }

                if (ch == '\'')
                {
                    // 进入单引号字符串，字符串内容全部跳过。
                    state = State::SingleQuote;
                    continue;
                }

                if (ch == '"')
                {
                    // 进入双引号字符串，防止字符串中的 = 被当作赋值。
                    state = State::DoubleQuote;
                    continue;
                }

                if (ch == '[')
                {
                    // Lua 长字符串以 [=*[ 开始；普通索引方括号则继续走深度逻辑。
                    size_t open = i + 1;
                    while (open < input.size() && input[open] == '=') ++open;
                    if (open < input.size() && input[open] == '[')
                    {
                        // 记录开头的等号数量，关闭时必须使用相同级别。
                        longStringLevel = static_cast<int>(open - i - 1);
                        i = open;
                        state = State::LongString;
                        continue;
                    }
                }

                if (ch == '(')
                {
                    // 括号内的 = 属于函数调用或子表达式，不是顶层赋值。
                    ++parenthesisDepth;
                    continue;
                }
                if (ch == ')')
                {
                    // 对不完整输入保持宽容，不让深度减成负数。
                    if (parenthesisDepth > 0) --parenthesisDepth;
                    continue;
                }
                if (ch == '{')
                {
                    // 表构造器中的键值对使用 =，但那不是 REPL 顶层赋值。
                    ++braceDepth;
                    continue;
                }
                if (ch == '}')
                {
                    // 关闭表构造器后，后续字符可以重新参与顶层判断。
                    if (braceDepth > 0) --braceDepth;
                    continue;
                }
                if (ch == '[')
                {
                    // 非长字符串的方括号代表索引表达式，内部 = 不算顶层赋值。
                    ++bracketDepth;
                    continue;
                }
                if (ch == ']')
                {
                    // 对不完整索引输入同样保持非负深度。
                    if (bracketDepth > 0) --bracketDepth;
                    continue;
                }

                if (ch != '=' || parenthesisDepth != 0 || braceDepth != 0 || bracketDepth != 0)
                {
                    // 只有最外层的 = 才值得继续检查运算符组合。
                    continue;
                }

                const char previous = i > start ? input[i - 1] : '\0';
                const char next = i + 1 < input.size() ? input[i + 1] : '\0';
                // 排除 ==、~=、<=、>=，这些都是比较运算而不是赋值。
                if (next == '=' || previous == '~' || previous == '<' || previous == '>') continue;
                // 找到一个顶层单独赋值号，当前输入按语句发送。
                return true;
            }

            // 扫描完整个输入仍未发现赋值号，调用方会把它当表达式处理。
            return false;
        }

        const char* ErrorCategoryLabel(protocol::ErrorCategory category)
        {
            switch (category)
            {
            case protocol::ErrorCategory::Lua: return "Lua Error";
            case protocol::ErrorCategory::Il2Cpp: return "Il2Cpp Error";
            case protocol::ErrorCategory::Mono: return "Mono Error";
            case protocol::ErrorCategory::CSharp: return "CSharp Error";
            case protocol::ErrorCategory::Lune: return "Lune Error";
            default: return "Lune Error";
            }
        }

        void PrintErrorPayloadImpl(const std::vector<uint8_t>& payload)
        {
            protocol::ErrorPayloadView error;
            std::string message;
            protocol::ErrorCategory category = protocol::ErrorCategory::Lune;
            int32_t line = -1;

            if (protocol::DecodeErrorPayload(payload.data(),
                    static_cast<uint32_t>(payload.size()), error))
            {
                category = error.category;
                line = error.line;
                if (error.messageLength > 0)
                    message.assign(error.message, error.messageLength);
            }
            else
            {
                // 兼容旧版 DLL 的纯文本 MSG_ERROR；新协议不会走这里。
                message.assign(payload.begin(), payload.end());
            }

            while (!message.empty()
                && (message.back() == '\r' || message.back() == '\n'))
            {
                message.pop_back();
            }
            if (message.empty()) message = "execution failed";

            std::string output = "[";
            output += ErrorCategoryLabel(category);
            output += "]";
            if (line > 0)
            {
                output += " line ";
                output += std::to_string(line);
            }
            output += ": ";
            output += message;
            output += "\n";

            ui::color::Red();
            // 后端日志和错误必须走同一个控制台输出流；否则 stdout/stderr
            // 在重定向或缓冲场景下仍可能出现视觉上的乱序。
            ui::SafePrintUtf8(output);
            ui::color::Reset();
        }
    }

    void PrintErrorPayload(const std::vector<uint8_t>& payload)
    {
        PrintErrorPayloadImpl(payload);
    }

    // ============================================================
    // Lua 语句判断
    // ============================================================
    bool IsStatement(const std::string& input)
    {
        // 去掉开头空白，但保留原始 input；原始内容仍会完整发送给 DLL。
        const size_t start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return true;

        // 多行输入不做自动 return，交给 Lua 编译器判断。
        // 多行输入可能包含完整控制流，不能用单行启发式强行加 return。
        if (input.find('\n') != std::string::npos) return true;

        static constexpr const char* keywords[] = {
            "local", "return", "if", "for", "while", "do",
            "function", "repeat", "until", "break", "end", "goto"
        };

        for (const char* keyword : keywords)
        {
            // 关键字必须匹配完整单词，避免把 localx 识别为 local。
            const size_t length = strlen(keyword);
            if (input.compare(start, length, keyword) == 0
                && (start + length == input.size()
                    || !IsIdentifierCharacter(input[start + length])))
            {
                return true;
            }
        }

        if (ContainsAssignment(input, start)) return true;

        // 单行注释本身也是语句，原样发送可以让 Lua 统一处理换行和语法。
        return input.compare(start, 2, "--") == 0;
    }

    // ============================================================
    // 命令发送与响应处理
    // ============================================================
    bool ExecuteCommand(
        PipeServer& server, const std::string& code, int timeoutMs)
    {
        // MAX_PAYLOAD 是协议边界，先在 REPL 层拒绝过大命令，避免无效发送。
        if (code.size() > protocol::MAX_PAYLOAD)
        {
            ui::color::Red();
            ui::SafePrintUtf8("[Lune Error] Command is too large (maximum 1 MB)\n");
            ui::color::Reset();
            return false;
        }

        // 命令内容不在这里改写；启动脚本也已经在调用方拼成 dofile 命令。
        if (!server.SendFrame(
                protocol::MSG_CMD,
                code.data(),
                static_cast<uint32_t>(code.size())))
        {
            ui::color::Red();
            ui::SafePrintUtf8("[Lune Error] Failed to send command\n");
            ui::color::Reset();
            return false;
        }

        // 从发送完成开始计时，确保网络/管道等待也包含在命令超时内。
        const auto startTime = std::chrono::steady_clock::now();

        // 一条命令只消费到自己的终结响应；日志由 ReaderLoop 异步交给 UI。
        while (true)
        {
            // 每次收到日志或无关帧后重新计算剩余时间，不能重置整个超时窗口。
            int remaining = timeoutMs;
            if (timeoutMs > 0)
            {
                // 使用 steady_clock，避免系统时间调整导致超时倒退或提前。
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                remaining = timeoutMs - static_cast<int>(elapsed);
                if (remaining <= 0)
                {
                    // 超时后关闭当前连接，防止迟到的 OK 被下一条命令消费。
                    ui::color::Red();
                    ui::SafePrintUtf8("[Lune Error] Command timeout\n");
                    ui::color::Reset();
                    server.Stop();
                    return false;
                }
            }

            // ReaderLoop 把日志放入 UI 队列；这里用短等待持续排空，
            // 确保后端执行较慢时日志也能及时显示，而不是等 MSG_OK 才出现。
            PipeServer::Frame frame;
            constexpr int LOG_POLL_INTERVAL_MS = 50;
            const int waitMs = (remaining > 0)
                ? (remaining < LOG_POLL_INTERVAL_MS ? remaining : LOG_POLL_INTERVAL_MS)
                : remaining;
            const auto status = server.ReceiveFrame(frame, waitMs);
            if (status == PipeServer::ReceiveStatus::Timeout)
            {
                // 这里通常只是一次日志轮询超时；真正的命令超时由循环顶部
                // 根据 startTime 判断，避免把慢命令误报成超时。
                ui::DrainAsyncLogs(false);
                continue;
            }

            if (status == PipeServer::ReceiveStatus::Disconnected)
            {
                ui::DrainAsyncLogs(false);
                // 没有响应且连接已断开，不再尝试读取队列或重发命令。
                ui::color::Red();
                ui::SafePrintUtf8("[Lune Error] Connection lost\n");
                ui::color::Reset();
                return false;
            }

            // ReaderLoop 按管道顺序先处理 MSG_LOG，再把控制帧放入队列；
            // 消费控制帧前再排空一次，保证最终响应前的日志不会落到下一条命令。
            ui::DrainAsyncLogs(false);

            switch (frame.type)
            {
            case protocol::MSG_OK:
                // OK 是本条命令的终结响应，返回成功给调用方。
                ui::DrainAsyncLogs(false);
                return true;

            case protocol::MSG_ERROR:
            {
                ui::DrainAsyncLogs(false);
                PrintErrorPayload(frame.payload);
                return false;
            }

            case protocol::MSG_EXIT:
                // DLL 主动退出后停止读线程，避免主循环继续向失效连接发送命令。
                ui::color::Yellow();
                ui::SafePrintUtf8("[*] DLL requested disconnect\n");
                ui::color::Reset();
                server.Stop();
                return false;

            default:
                // 传输层已经过滤未知类型；此处保留防御性分支以避免异常帧终止进程。
                // ReaderLoop 只应把协议帧放入队列；未知帧会在传输层被拒绝。
                continue;
            }
        }
    }
}
