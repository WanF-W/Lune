// ============================================================
// lune.cpp — Lune 程序入口与生命周期编排
//
// 本文件只负责把参数解析、进程定位、注入、握手和 REPL 串成一条流程。
// 控制台细节位于 console_ui，Lua 输入处理位于 repl；底层会话统一通过
// HostCore 的公开接口调用。
// ============================================================
#include "console_ui.h"
#include "session_ui.h"

#include "repl.h"
#include "version.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
    // ============================================================
    // 命令行参数
    // ============================================================
    struct Args
    {
        // 后端选择决定默认 DLL、IPC 名称和 HELLO 版本。
        const HC_BackendInfo* backend = nullptr;
        uint32_t backendSelector = 0;

        // 进程名和 PID 二选一；解析阶段会记录用户实际选择了哪一种。
        std::wstring processName;
        DWORD pid = 0;

        // DLL 和启动脚本都是可选路径，空值表示使用默认行为。
        std::wstring dllPath;
        std::wstring luaScript;

        // 解析失败时保存可直接展示给用户的错误信息。
        std::wstring error;
        bool showHelp = false;
    };

    // 控制处理函数只能设置这个标志；资源回收仍由主线程完成。
    std::atomic<bool> g_shutdownRequested{ false };

    // ============================================================
    // 参数解析
    // ============================================================
    bool IsOption(const std::wstring& value, const wchar_t* shortName, const wchar_t* longName)
    {
        // 短选项和长选项共享同一判断，避免参数分支重复比较字符串。
        return value == shortName || value == longName;
    }

    bool ParsePid(const wchar_t* value, DWORD& pid)
    {
        // 空字符串不是合法 PID；调用方在这里统一拒绝，而不是依赖后续 OpenProcess。
        if (value == nullptr || *value == L'\0') return false;

        // 使用 end 指针确认整个参数都是数字，避免“123abc”被部分解析为 123。
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long parsed = std::wcstoull(value, &end, 10);
        if (errno == ERANGE || end == value || *end != L'\0' || parsed == 0
            || parsed > (std::numeric_limits<DWORD>::max)())
        {
            // 溢出、空输入、尾随字符和零 PID 都属于用户输入错误。
            return false;
        }

        // 上面的范围检查保证转换到 DWORD 不会截断。
        pid = static_cast<DWORD>(parsed);
        return true;
    }

    bool ParseArgs(int argc, wchar_t* argv[], Args& args)
    {
        // 参数解析集中在这里，主流程只消费已经校验过的 Args。
        bool hasName = false;
        bool hasPid = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::wstring option = argv[i] != nullptr ? argv[i] : L"";
            if (option == L"-h" || option == L"--help")
            {
                // help 可以和其他参数一起出现，但最终不再进入注入流程。
                args.showHelp = true;
                continue;
            }

            if (option == L"-i" || option == L"-m" || option == L"-u")
            {
                // 后端必须明确指定且只能指定一次，避免 DLL 和协议配置串用。
                if (args.backend != nullptr)
                {
                    args.error = L"Specify only one backend: -i, -m or -u.";
                    return false;
                }
                args.backendSelector = static_cast<uint32_t>(option[1]);
                args.backend = HC_GetBackendInfo(args.backendSelector);
                continue;
            }

            if (IsOption(option, L"-n", L"--name")
                || IsOption(option, L"-p", L"--pid")
                || IsOption(option, L"-d", L"--dll")
                || IsOption(option, L"-l", L"--lua"))
            {
                // 所有带值选项先统一检查下一个参数，避免 ++i 越界。
                if (i + 1 >= argc || argv[i + 1] == nullptr || argv[i + 1][0] == L'\0')
                {
                    args.error = L"Missing value for option: " + option;
                    return false;
                }
            }

            if (IsOption(option, L"-n", L"--name"))
            {
                // 进程名只保存文本，实际查找交给 HostCore。
                args.processName = argv[++i];
                hasName = true;
            }
            else if (IsOption(option, L"-p", L"--pid"))
            {
                // PID 在写入 Args 前先完成严格解析。
                if (!ParsePid(argv[++i], args.pid))
                {
                    args.error = L"Invalid process ID.";
                    return false;
                }
                hasPid = true;
            }
            else if (IsOption(option, L"-d", L"--dll"))
            {
                // DLL 路径暂不在这里访问文件，统一由 HostCore 校验。
                args.dllPath = argv[++i];
            }
            else if (IsOption(option, L"-l", L"--lua"))
            {
                // 启动脚本路径先保存；握手完成后规范化并拼成 dofile 命令。
                args.luaScript = argv[++i];
            }
            else if (!args.showHelp)
            {
                // 未知选项立即失败，避免拼写错误被静默忽略。
                args.error = L"Unknown option: " + option;
                return false;
            }
        }

        if (args.showHelp) return true;

        if (args.backend == nullptr)
        {
            args.error = L"Specify one backend: -i, -m or -u.";
            return false;
        }

        // 使用异或判断“恰好一个”：同时指定或都未指定都不允许。
        if (hasName == hasPid)
        {
            args.error = L"Specify exactly one of --name or --pid.";
            return false;
        }

        return true;
    }

    // ============================================================
    // 控制台帮助与路径定位
    // ============================================================
    void PrintBanner(const HC_BackendInfo& backend)
    {
        // 保留统一的横线标题框；横线使用青色，标题内容使用黄色。
        ui::color::Cyan();
        std::wcout << L"===============================================================\n\n";
        ui::color::Reset();
        std::wcout << backend.cliName << L" v" LUNE_VERSION_WSTRING L"\n";
        ui::color::Yellow();
        std::wcout << L"  - CLI Companion for " << backend.runtimeName << L"\n\n";
        ui::color::Reset();
        std::wcout << backend.runtimeName << L" v" << backend.runtimeVersion << L"\n";
        ui::color::Yellow();
        std::wcout << L"  - Native C++ Bridge between Lua and "
                   << backend.runtimeTarget << L"\n\n";
        ui::color::Cyan();
        std::wcout << L"===============================================================\n\n";
        ui::color::Reset();
    }

    void PrintHelp(const std::wstring& error = {})
    {
        if (!error.empty())
        {
            // 解析错误先展示，再展示完整用法，方便命令行用户修正参数。
            ui::color::Red();
            std::wcerr << L"[Lune Error] " << error << L"\n\n";
            ui::color::Reset();
        }

        std::wcout << L"Usage: lune -i|-m|-u -n <process.exe> [options]\n";
        std::wcout << L"       lune -i|-m|-u -p <pid> [options]\n\n";
        std::wcout << L"Options:\n";
        std::wcout << L"  -i                  Use Il2CppLua.dll / ilune prompt\n";
        std::wcout << L"  -m                  Use MonoLua.dll / mlune prompt\n";
        std::wcout << L"  -u                  Use UnrealLua.dll / ulune prompt\n";
        std::wcout << L"  -n, --name <name>  Target process name\n";
        std::wcout << L"  -p, --pid <pid>    Target process ID\n";
        std::wcout << L"  -d, --dll <path>   Override the selected backend DLL path\n";
        std::wcout << L"  -l, --lua <path>   Startup Lua script\n";
        std::wcout << L"  -h, --help         Show this help\n";
    }

    std::wstring GetFullPath(const std::wstring& path)
    {
        if (path.empty()) return {};

        std::vector<wchar_t> buffer(MAX_PATH);
        while (true)
        {
            const DWORD length = GetFullPathNameW(
                path.c_str(),
                static_cast<DWORD>(buffer.size()),
                buffer.data(),
                nullptr);
            if (length == 0) return {};
            if (length < buffer.size()) return std::wstring(buffer.data(), length);

            // 缓冲区不足时，Windows 返回所需容量；至少扩大一倍，避免
            // 长路径反复只增长一个字符。
            buffer.resize((std::max)(buffer.size() * 2, static_cast<size_t>(length) + 1));
        }
    }

    std::string Utf8FromWide(const std::wstring& value)
    {
        if (value.empty()) return {};
        if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return {};

        const int inputLength = static_cast<int>(value.size());
        const int length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            inputLength,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (length <= 0) return {};

        std::string result(static_cast<size_t>(length), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                inputLength,
                result.data(),
                length,
                nullptr,
                nullptr) != length)
        {
            return {};
        }
        return result;
    }

    // ============================================================
    // 控制台关闭处理
    // ============================================================
    BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
    {
        // 其他控制事件交给系统默认处理，不改变正常关闭行为。
        if (ctrlType != CTRL_C_EVENT && ctrlType != CTRL_CLOSE_EVENT) return FALSE;

        // 控制处理函数只写原子标志；不要在 Windows 控制线程中获取锁或做 I/O。
        g_shutdownRequested.store(true);
        return TRUE;
    }
}

// ============================================================
// 程序入口
// ============================================================
int wmain(int argc, wchar_t* argv[])
{
    // 启动阶段按“解析 -> 定位 -> 建立 IPC -> 注入 -> 握手”的顺序执行；
    // do/while(false) 让所有失败路径统一经过底部清理。
    Args args;
    if (!ParseArgs(argc, argv, args))
    {
        // 参数错误不创建任何外部资源，直接返回命令行错误码。
        PrintHelp(args.error);
        return 1;
    }

    if (args.showHelp)
    {
        // help 是成功请求，不应被当成参数错误。
        PrintHelp();
        return 0;
    }

    // ============================================================
    // 启动前检查
    // ============================================================
    const HC_BackendInfo& backend = *args.backend;
    ui::SetPrompt(backend.prompt);
    PrintBanner(backend);

    struct DisplayContext {
        HANDLE disconnectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~DisplayContext() { if (disconnectEvent) CloseHandle(disconnectEvent); }
        bool lifecycleStarted = false;
        bool ctrlHandlerInstalled = false;
    } display;
    struct SessionOwner {
        HC_Session handle = nullptr;
        ~SessionOwner() { HC_DestroySession(handle); }
    } session;
    if (!display.disconnectEvent) {
        ui::color::Red();
        std::wcerr << L"[Lune Error] Failed to create session.\n";
        ui::color::Reset();
        return 1;
    }
    HC_SessionOptions options{};
    options.structSize = static_cast<uint32_t>(sizeof(options));
    options.abiVersion = HC_ABI_VERSION;
    options.backend = args.backendSelector;
    options.pid = args.pid;
    options.processName = args.processName.c_str();
    options.dllPath = args.dllPath.c_str();
    options.context = &display;
    options.cancelCallback = [](void*) -> int32_t { return g_shutdownRequested.load() ? 1 : 0; };
    options.eventCallback = [](void* context, const HC_Event* event) {
        auto& state = *static_cast<DisplayContext*>(context);
        if (event->kind == HC_CREATING_PIPE) {
            state.lifecycleStarted = true;
            state.ctrlHandlerInstalled = SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE) != FALSE;
            if (!state.ctrlHandlerInstalled) {
                ui::color::Yellow();
                std::wcerr << L"[Lune Error] Ctrl+C handler could not be installed.\n";
                ui::color::Reset();
            }
        }
        if (event->kind == HC_DISCONNECTED) {
            SetEvent(state.disconnectEvent);
            return;
        }
        ui::DisplaySessionEvent(nullptr, event);
    };
    if (HC_CreateSession(&options, &session.handle) != HC_SUCCESS) {
        ui::color::Red();
        std::wcerr << L"[Lune Error] Failed to create session.\n";
        ui::color::Reset();
        return 1;
    }
    int result = 1;
    do
    {
        if (HC_StartSession(session.handle) != HC_SUCCESS) {
            if (!display.lifecycleStarted) return 1;
            break;
        }
        // ============================================================
        // 启动脚本
        // ============================================================
        if (!args.luaScript.empty())
        {
            // 启动脚本直接按 Lua 的 dofile 语义执行；Lune 只拼接命令并发送。
            ui::color::Gray();
            std::wcout << L"[*] Executing startup script: " << args.luaScript << L"\n";
            ui::color::Reset();

            const std::wstring scriptPath = GetFullPath(args.luaScript);
            if (scriptPath.empty())
            {
                ui::color::Red();
                std::wcerr << L"[Lune Error] Cannot resolve startup script path.\n";
                ui::color::Reset();
                break;
            }
            std::string scriptPathUtf8 = Utf8FromWide(scriptPath);
            if (scriptPathUtf8.empty())
            {
                ui::color::Red();
                std::wcerr << L"[Lune Error] Cannot convert startup script path to UTF-8.\n";
                ui::color::Reset();
                break;
            }
            for (char& ch : scriptPathUtf8)
                if (ch == '\\') ch = '/';
            const std::string script = "dofile(\"" + scriptPathUtf8 + "\")";

            // 脚本错误由 Lua 的 dofile 原样返回；只有断线才阻止进入 REPL。
            if (!repl::ExecuteCommand(session.handle, script, HC_GetStartupScriptTimeout())
                && !HC_IsConnected(session.handle))
            {
                break;
            }
            std::wcout << L"\n";
        }

        // ============================================================
        // 交互式 REPL
        // ============================================================
        while (!g_shutdownRequested.load())
        {
            // 每一轮只负责一条用户输入：显示提示符、读取、发送、等待响应。
            ui::DrainAsyncLogs(false);
            ui::PrintPrompt();

            std::string input;
            if (!ui::ReadLineUtf8(input, display.disconnectEvent))
            {
                // 输入被中断时没有回车回显，先结束提示符行再输出退出消息。
                ui::SetPromptActive(false);
                ui::SafePrintUtf8("\n");
                if (!g_shutdownRequested.load() && !HC_IsConnected(session.handle))
                {
                    ui::color::Red();
                    ui::SafePrintUtf8("[Lune Error] Target process disconnected.\n", true);
                    ui::color::Reset();
                }
                break;
            }
            ui::SetPromptActive(false);
            if (g_shutdownRequested.load()) break;

            if (input.empty() || input.find_first_not_of(" \t\r\n") == std::string::npos) continue;

            if (input == "exit" || input == "quit")
            {
                // 正常退出先通知 DLL，随后由底部 Stop 取消读线程并释放资源。
                ui::color::Gray();
                std::wcout << L"\n[*] Sending exit signal...\n";
                ui::color::Reset();
                if (HC_RequestExit(session.handle) == HC_SUCCESS)
                {
                    ui::color::Green();
                    std::wcout << L"[+] DLL disconnected\n";
                    ui::color::Reset();
                }
                break;
            }

            // 表达式包装 return 只影响 REPL 回显，不改变已经是语句的源码。
            const std::string code = repl::IsStatement(input) ? input : "return " + input;
            if (!repl::ExecuteCommand(session.handle, code, HC_GetCommandTimeout())
                && !HC_IsConnected(session.handle))
            {
                ui::color::Red();
                std::wcerr << L"[Lune Error] Connection lost. DLL may have unloaded.\n";
                ui::color::Reset();
                break;
            }

            // 命令已经有终结响应；把此刻已经到达的异步日志先放在上一条命令之后，
            // 再创建下一条提示符，避免日志在空提示符后才补出来。
            ui::DrainAsyncLogs(false);
            ui::SetPromptSeparate();
        }

        result = 0;
    } while (false);

    if (g_shutdownRequested.load()) result = 0;

    // ============================================================
    // 统一清理
    // ============================================================
    // 所有退出路径都走到这里，保证管道、共享内存和控制处理器按顺序清理。
    ui::color::Gray();
    std::wcout << L"[*] Shutting down...\n";
    ui::color::Reset();
    HC_StopSession(session.handle);

    if (display.ctrlHandlerInstalled) SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    if (result == 0)
    {
        ui::color::Green();
        std::wcout << L"[+] Done\n\n";
        ui::color::Reset();
        std::wcout << L"Thanks for using\n\n";
    }

    return result;
}


