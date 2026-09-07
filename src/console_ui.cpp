// ============================================================
// console_ui.cpp — Lune 控制台交互实现
//
// Lune 只负责把 UTF-8 文本安全显示在控制台，并处理用户输入和提示符排版。
// 日志内容不在这里解析，也不包含任何具体运行时的格式化规则。
// ============================================================
#include "console_ui.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ui
{
    namespace color
    {
        void Reset();
        void Yellow();
    }

    namespace
    {
        std::recursive_mutex& OutputMutex()
        {
            // 颜色设置、日志块和提示符重绘共享同一把锁，保证每次输出完整。
            static std::recursive_mutex mutex;
            return mutex;
        }

        void SetColor(WORD attributes)
        {
            // 颜色设置和文本输出使用同一把锁，避免线程在换色期间互相穿插。
            std::lock_guard<std::recursive_mutex> lock(OutputMutex());
            HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
            if (output != nullptr && output != INVALID_HANDLE_VALUE)
            {
                // 输出被重定向到文件时，调用失败不会影响后续文本输出。
                SetConsoleTextAttribute(output, attributes);
            }
        }

        std::atomic<bool> g_promptActive{ false };
        std::atomic<bool> g_promptSeparate{ false };
        // profile 中的提示符拥有静态生命周期，因此这里只保存借来的指针。
        const wchar_t* g_prompt = L"lune >> ";

        constexpr size_t MAX_UI_LOG_QUEUE_ITEMS = 1024;
        constexpr size_t MAX_UI_LOG_QUEUE_BYTES = 4 * 1024 * 1024;
        std::mutex g_logMutex;
        std::deque<std::string> g_logQueue;
        size_t g_logQueueBytes = 0;

        bool TakeLogBatch(std::string& output)
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (g_logQueue.empty()) return false;

            bool hadMessage = false;
            while (!g_logQueue.empty())
            {
                std::string message = std::move(g_logQueue.front());
                g_logQueue.pop_front();
                g_logQueueBytes -= message.size();
                hadMessage = true;

                // 不同 MSG_LOG 帧是同一输出流的连续片段；只有前一段没有行尾时，
                // 才在两段之间补换行，避免文本粘在一起。
                if (!output.empty()
                    && output.back() != L'\n'
                    && output.back() != L'\r')
                {
                    output.push_back('\n');
                }
                output += message;
            }

            if (!hadMessage) return false;

            // 每个日志批次统一只保留一个行尾；块之间的空行由调用方决定。
            while (!output.empty()
                && (output.back() == '\n' || output.back() == '\r'))
            {
                output.pop_back();
            }
            if (output.empty()) output = "\n";
            else output.push_back('\n');
            return true;
        }

    }

    namespace color
    {
        void Reset()
        {
            // 恢复普通白色，避免颜色状态泄漏到下一条消息。
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        }

        void Red()
        {
            // 红色用于错误和失败提示。
            SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        }

        void Green()
        {
            // 绿色用于成功状态和握手完成提示。
            SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        }

        void Yellow()
        {
            // 黄色用于提示符、警告和需要用户注意的状态。
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        }

        void Cyan()
        {
            // 青色用于标题等结构性信息。
            SetColor(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        }

        void Gray()
        {
            // 灰色用于不需要强调的流程状态。
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        }
    }

    void SafePrintUtf8(const std::string& text, bool useStderr)
    {
        // 空文本不需要获取句柄或执行编码转换。
        if (text.empty()) return;

        // 一次完整输出期间保持锁，避免多线程输出互相穿插。
        std::lock_guard<std::recursive_mutex> lock(OutputMutex());
        // 根据调用方选择标准输出或标准错误输出。
        HANDLE output = GetStdHandle(useStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        if (output == nullptr || output == INVALID_HANDLE_VALUE) return;

        // 协议文本固定为 UTF-8。非法字节直接拒绝，避免损坏日志被静默替换。
        const int wideLength = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0);
        if (wideLength <= 0) return;

        // 先查询并分配精确的 UTF-16 容量，避免中文或长日志被固定缓冲区截断。
        std::wstring wideText(static_cast<size_t>(wideLength), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data(),
                static_cast<int>(text.size()),
                wideText.data(),
                wideLength) != wideLength)
        {
            return;
        }

        // 真正的 Windows 控制台支持 UTF-16，直接调用 WriteConsoleW 绕过代码页。
        DWORD written = 0;
        if (GetFileType(output) == FILE_TYPE_CHAR)
        {
            WriteConsoleW(
                output,
                wideText.data(),
                static_cast<DWORD>(wideText.size()),
                &written,
                nullptr);
            return;
        }

        // 输出被重定向到文件或管道时，保留原始 UTF-8 字节写出。
        DWORD bytesWritten = 0;
        WriteFile(
            output,
            text.data(),
            static_cast<DWORD>(text.size()),
            &bytesWritten,
            nullptr);
    }

    void QueueAsyncLog(const char* text)
    {
        // PipeServer 传入的指针只在本次回调期间有效，不能保存到其他线程。
        if (text == nullptr) return;

        std::string message(text);
        if (message.size() > MAX_UI_LOG_QUEUE_BYTES) return;

        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (g_logQueue.size() >= MAX_UI_LOG_QUEUE_ITEMS
                || message.size() > MAX_UI_LOG_QUEUE_BYTES - g_logQueueBytes)
            {
                // 控制台显示落后时丢弃新日志，不能让后台管道线程无限增长内存。
                return;
            }
            g_logQueueBytes += message.size();
            g_logQueue.emplace_back(std::move(message));
        }

        // 日志不能等到用户按 Enter；后台回调入队后立即排出。
        DrainAsyncLogs(false);
    }

    bool DrainAsyncLogs(bool addSeparator)
    {
        std::string output;
        if (!TakeLogBatch(output)) return false;

        // 输入线程使用 Windows 原生行编辑时，后台日志仍需即时可见。
        // 只在确实有活动提示符时换行并恢复提示符，不触碰控制台输入缓冲区。
        const bool restorePrompt = g_promptActive.load();
        std::lock_guard<std::recursive_mutex> lock(OutputMutex());
        if (restorePrompt) SafePrintUtf8("\n");

        // Lune 不解析 IL2CPP、Mono 或 Unreal 的日志结构，后端文本原样显示。
        SafePrintUtf8(output);
        if (addSeparator) std::wcout << L"\n";
        if (restorePrompt)
        {
            g_promptSeparate.store(true);
            PrintPrompt();
        }
        std::wcout.flush();
        return true;
    }

    bool ReadLineUtf8(std::string& outUtf8, HANDLE cancelEvent)
    {
        // 使用 ReadConsoleW 交给 Windows 控制台处理行编辑和 IME。
        // ReadConsoleInputW 只能看到低级键盘事件，中文输入法的组合态和候选
        // 确认并不是普通 ASCII 按键，自己重绘会导致输入状态与显示状态脱节。
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) return false;

        DWORD originalMode = 0;
        if (!GetConsoleMode(input, &originalMode)) return false;

        // 确保高层读取使用行输入和系统回显；原始模式可能由宿主终端留下。
        const DWORD lineMode = originalMode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
        if (!SetConsoleMode(input, lineMode)) return false;

        struct ConsoleModeGuard
        {
            HANDLE input;
            DWORD mode;
            ~ConsoleModeGuard()
            {
                SetConsoleMode(input, mode);
            }
        } restoreMode{input, originalMode};

        struct ReadState
        {
            std::mutex mutex;
            std::wstring line;
            bool success = false;
        };

        HANDLE completeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (completeEvent == nullptr) return false;

        ReadState state;
        std::thread reader;
        try
        {
            reader = std::thread([&state, input, completeEvent]()
            {
                std::wstring line;
                bool success = false;
                try
                {
                    wchar_t buffer[256]{};
                    while (true)
                    {
                        DWORD readCount = 0;
                        if (!ReadConsoleW(input, buffer, 255, &readCount, nullptr)
                            || readCount == 0)
                        {
                            break;
                        }

                        bool completed = false;
                        for (DWORD i = 0; i < readCount; ++i)
                        {
                            // 控制台行输入通常返回 CRLF；只把 LF 作为行结束，
                            // 避免把 CRLF 的第二个字符留给下一条命令。
                            if (buffer[i] == L'\r') continue;
                            if (buffer[i] == L'\n')
                            {
                                completed = true;
                                break;
                            }
                            line += buffer[i];
                        }

                        if (completed)
                        {
                            success = true;
                            break;
                        }
                    }
                }
                catch (...)
                {
                    success = false;
                }
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.line.swap(line);
                    state.success = success;
                }
                SetEvent(completeEvent);
            });
        }
        catch (...)
        {
            CloseHandle(completeEvent);
            return false;
        }

        HANDLE handles[2]{};
        DWORD handleCount = 1;
        handles[0] = completeEvent;
        if (cancelEvent != nullptr)
        {
            handles[handleCount++] = cancelEvent;
        }

        const DWORD waitResult = WaitForMultipleObjects(handleCount, handles, FALSE, INFINITE);
        bool interrupted = waitResult != WAIT_OBJECT_0;
        if (interrupted)
        {
            // ReadConsoleW 是 reader 线程上的同步 I/O；取消它后再 join，
            // 确保 state 和 completeEvent 在后台线程退出后才释放。
            CancelSynchronousIo(reader.native_handle());
        }

        if (reader.joinable()) reader.join();
        CloseHandle(completeEvent);
        if (interrupted) return false;

        std::wstring line;
        bool success = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            line = std::move(state.line);
            success = state.success;
        }
        if (!success) return false;

        if (line.empty())
        {
            // 空行是有效输入，由上层决定是否忽略。
            outUtf8.clear();
            return true;
        }

        // 先计算 UTF-8 所需字节数，再按精确大小分配输出缓冲区。
        const int utf8Length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            line.data(),
            static_cast<int>(line.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8Length <= 0) return false;

        outUtf8.resize(static_cast<size_t>(utf8Length));
        return WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            line.data(),
            static_cast<int>(line.size()),
            outUtf8.data(),
            utf8Length,
            nullptr,
            nullptr) == utf8Length;
    }

    void SetPromptActive(bool active)
    {
        // 主线程开始或结束读取一条命令时更新该状态。
        g_promptActive.store(active);
    }

    void SetPromptSeparate()
    {
        // 延迟到下一次 PrintPrompt 再插入空行，避免重复换行。
        g_promptSeparate.store(true);
    }

    void SetPrompt(const wchar_t* prompt)
    {
        // 提示符来自静态 BackendProfile，不复制字符串，也不持有外部资源。
        if (prompt != nullptr && *prompt != L'\0') g_prompt = prompt;
    }

    void PrintPrompt()
    {
        std::lock_guard<std::recursive_mutex> lock(OutputMutex());
        // 命令完成或异步日志出现后，在新提示符前补一个可读的分隔空行。
        if (g_promptSeparate.exchange(false)) std::wcout << L"\n";

        // 提示符只由主线程打印；后台日志线程永远不会在这里重入。
        g_promptActive.store(true);
        color::Yellow();
        std::wcout << g_prompt;
        color::Reset();
        std::wcout.flush();
    }
}
