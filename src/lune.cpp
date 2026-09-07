// ============================================================
// lune.cpp — Lune 程序入口与生命周期编排
//
// 本文件只负责把参数解析、进程定位、注入、握手和 REPL 串成一条流程。
// 控制台细节位于 console_ui，Lua 命令处理位于 repl，管道和注入实现不
// 直接依赖彼此的业务逻辑。
// ============================================================
#include "console_ui.h"
#include "backend_profile.h"
#include "injector.h"
#include "pipe_server.h"
#include "protocol.h"
#include "repl.h"
#include "version.h"
#include "win_handle.h"

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
        const BackendProfile* backend = nullptr;

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
                args.backend = FindBackend(option[1]);
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
                // 进程名只保存文本，实际查找交给 Injector。
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
                // DLL 路径暂不在这里访问文件，统一由 LocateDll 校验。
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
    void PrintBanner(const BackendProfile& backend)
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

    bool IsRegularFile(const std::wstring& path)
    {
        // 目录不能作为 DLL 或脚本文件传入；这里仅做属性检查。
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
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

    std::wstring GetExecutablePath()
    {
        // MAX_PATH 只是初始容量；路径过长时根据 API 返回值动态扩大。
        std::vector<wchar_t> buffer(MAX_PATH);
        while (true)
        {
            const DWORD length = GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length == 0) return {};
            // length + 1 < size 表示返回值没有触及缓冲区上限，可以安全构造字符串。
            if (length + 1 < buffer.size()) return std::wstring(buffer.data(), length);
            // API 返回长度达到容量时，扩大后重试，避免截断 DLL 路径。
            buffer.resize(buffer.size() * 2);
        }
    }

    std::wstring LocateDll(const std::wstring& explicitPath, const BackendProfile& backend)
    {
        if (!explicitPath.empty())
        {
            // 显式路径优先级最高；不自动替换用户给出的路径。
            if (IsRegularFile(explicitPath)) return explicitPath;

            ui::color::Red();
            std::wcerr << L"[Lune Error] DLL not found or is not a file: " << explicitPath << L"\n";
            ui::color::Reset();
            return {};
        }

        // 默认路径跟随 exe，而不是当前工作目录，避免从不同目录启动时行为变化。
        const std::wstring executablePath = GetExecutablePath();
        const size_t separator = executablePath.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
        {
            ui::color::Red();
            std::wcerr << L"[Lune Error] Cannot determine the Lune executable directory.\n";
            ui::color::Reset();
            return {};
        }

        // 默认 DLL 与 lune.exe 同目录，而不是与当前工作目录同目录。
        const std::wstring candidate = executablePath.substr(0, separator + 1) + backend.dllName;
        if (IsRegularFile(candidate)) return candidate;

        ui::color::Red();
        std::wcerr << L"[Lune Error] Cannot find " << backend.dllName << L" next to lune.exe.\n";
        std::wcerr << L"    Use --dll to specify the path explicitly.\n";
        ui::color::Reset();
        return {};
    }

    DWORD LocateTarget(const Args& args)
    {
        if (args.pid != 0)
        {
            // PID 模式先尝试打开进程，尽早报告权限或进程不存在问题。
            win::UniqueHandle process(Injector::OpenTargetProcess(args.pid));
            if (process) return args.pid;

            ui::color::Red();
            std::wcerr << L"[Lune Error] Cannot open process with PID " << args.pid << L"\n";
            ui::color::Reset();
            return 0;
        }

        // 名称模式由 Injector 枚举进程并执行不区分大小写匹配。
        const DWORD pid = Injector::FindProcessByName(args.processName);
        if (pid == 0)
        {
            ui::color::Red();
        std::wcerr << L"[Lune Error] Process not found: " << args.processName << L"\n";
            ui::color::Reset();
        }
        return pid;
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
    const BackendProfile& backend = *args.backend;
    ui::SetPrompt(backend.prompt);
    PrintBanner(backend);

    ui::color::Gray();
    std::wcout << L"[*] Locating target process...\n";
    ui::color::Reset();
    // 先定位目标进程，后续管道和注入都依赖这个 PID。
    const DWORD pid = LocateTarget(args);
    if (pid == 0) return 1;

    const std::wstring processName = Injector::GetProcessName(pid);
    ui::color::Green();
    std::wcout << L"[+] Target: "
               << (processName.empty() ? L"<unknown>" : processName)
               << L" (PID: " << pid << L")\n";
    ui::color::Reset();

    ui::color::Gray();
    std::wcout << L"[*] Locating " << backend.dllName << L"...\n";
    ui::color::Reset();
    // 进程确认后再解析 DLL，避免找不到进程时无意义地访问文件系统。
    const std::wstring dllPath = LocateDll(args.dllPath, backend);
    if (dllPath.empty()) return 1;

    ui::color::Green();
    std::wcout << L"[+] DLL: " << dllPath << L"\n";
    ui::color::Reset();

    // ============================================================
    // 创建 IPC 服务
    // ============================================================
    // 管道名必须与配套 DLL 根据 PID 生成的名称完全一致。
    wchar_t pipeName[128]{};
    if (swprintf_s(pipeName, 128, L"%s%lu", backend.pipePrefix, pid) < 0)
    {
        ui::color::Red();
        std::wcerr << L"[Lune Error] Failed to build the pipe name.\n";
        ui::color::Reset();
        return 1;
    }

    // server 的生命周期覆盖整个注入和 REPL 阶段，离开作用域前统一 Stop。
    PipeServer server;
    const bool ctrlHandlerInstalled = SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE) != FALSE;
    if (!ctrlHandlerInstalled)
    {
        ui::color::Yellow();
        std::wcerr << L"[Lune Error] Ctrl+C handler could not be installed.\n";
        ui::color::Reset();
    }

    server.SetLogCallback([](const char* text) {
        // PipeServer 不理解业务；后台线程只把日志交给 console_ui 排队。
        // 控制台日志写入在回调线程即时完成；提示符状态仍由 console_ui 统一协调。
        ui::QueueAsyncLog(text);
    });

    int result = 1;
    do
    {
        // 先创建管道，再注入 DLL，确保 DLL 连接时服务端已经准备完成。
        ui::color::Gray();
        std::wcout << L"[*] Creating pipe server...\n";
        ui::color::Reset();
        if (!server.Start(pipeName))
        {
            ui::color::Red();
            std::wcerr << L"[Lune Error] Failed to create pipe server.\n";
            ui::color::Reset();
            break;
        }
        if (g_shutdownRequested.load()) break;

        // 注入失败时直接进入统一清理，不继续等待不存在的连接。
        ui::color::Gray();
        std::wcout << L"[*] Injecting DLL...\n";
        ui::color::Reset();
        if (!Injector::Inject(pid, dllPath, pipeName, backend.sharedMemoryPrefix))
        {
            ui::color::Red();
            std::wcerr << L"[Lune Error] Injection failed.\n";
            ui::color::Reset();
            break;
        }
        if (g_shutdownRequested.load()) break;

        ui::color::Green();
        std::wcout << L"[+] DLL injected\n";
        ui::color::Reset();

        ui::color::Gray();
        std::wcout << L"[*] Waiting for DLL to connect...\n";
        ui::color::Reset();
        // 等待 DLL 连接；连接超时会由 PipeServer 取消挂起的 ConnectNamedPipe。
        if (!server.WaitForClient(protocol::HANDSHAKE_TIMEOUT))
        {
            if (!g_shutdownRequested.load())
            {
                ui::color::Red();
                std::wcerr << L"[Lune Error] DLL did not connect within "
                           << protocol::HANDSHAKE_TIMEOUT / 1000 << L" seconds.\n";
                ui::color::Reset();
            }
            break;
        }

        ui::color::Green();
        std::wcout << L"[+] DLL connected\n";
        ui::color::Reset();

        ui::color::Gray();
        std::wcout << L"[*] Handshake...\n";
        ui::color::Reset();

        // ============================================================
        // HELLO / READY 版本握手
        // ============================================================
        // HELLO 和 READY 必须按顺序到达，WaitForFrame 会拒绝乱序或错误帧。
        PipeServer::Frame frame;
        if (!server.WaitForFrame(protocol::MSG_HELLO, frame, protocol::DLLSAYHELLO_TIMEOUT))
        {
            ui::color::Red();
            if (frame.type == protocol::MSG_ERROR)
            {
                ui::DrainAsyncLogs(false);
                repl::PrintErrorPayload(frame.payload);
            }
            else
            {
                std::wcerr << L"[Lune Error] Handshake failed: no HELLO.\n";
            }
            ui::color::Reset();
            break;
        }

        // HELLO 负载是版本字符串，必须精确匹配，避免两端协议不兼容时继续运行。
        const std::string hello(frame.payload.begin(), frame.payload.end());
        if (hello != backend.protocolVersion)
        {
            ui::color::Red();
            ui::SafePrintUtf8(
                "[Lune Error] Version mismatch: expected " + std::string(backend.protocolVersion)
                + ", received " + hello + "\n", true);
            ui::color::Reset();
            break;
        }

        ui::color::Green();
        ui::SafePrintUtf8("[+] Handshake: " + hello + "\n");
        ui::color::Reset();

        // DLL 已经打开共享内存并发送 HELLO，现在可以释放 EXE 侧句柄。
        Injector::CloseSharedMemory();

        // DLL 已连接但可能仍在等待 IL2CPP 初始化，因此 READY 使用更长超时。
        if (!server.WaitForFrame(protocol::MSG_READY, frame, protocol::DLLSAYREADY_TIMEOUT))
        {
            ui::color::Red();
            if (frame.type == protocol::MSG_ERROR)
            {
                ui::DrainAsyncLogs(false);
                repl::PrintErrorPayload(frame.payload);
            }
            else
            {
                std::wcerr << L"[Lune Error] Handshake failed: no READY.\n";
            }
            ui::color::Reset();
            break;
        }

        // 初始化期间到达的 MSG_LOG 由后台线程暂存，进入 REPL 前统一显示。
        ui::DrainAsyncLogs(false);

        ui::color::Green();
        std::wcout << L"[+] Ready\n\n";
        ui::color::Reset();

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
            if (!repl::ExecuteCommand(server, script, protocol::LUAFILE_TIMEOUT)
                && !server.IsConnected())
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
            if (!ui::ReadLineUtf8(input, server.GetDisconnectEventHandle()))
            {
                // 输入被中断时没有回车回显，先结束提示符行再输出退出消息。
                ui::SetPromptActive(false);
                ui::SafePrintUtf8("\n");
                if (!g_shutdownRequested.load() && !server.IsConnected())
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
                if (server.SendFrame(protocol::MSG_EXIT, nullptr, 0))
                {
                    PipeServer::Frame exitFrame;
                    const auto status = server.ReceiveFrame(
                        exitFrame,
                        protocol::EXIT_ACK_TIMEOUT);
                    if (status == PipeServer::ReceiveStatus::Received
                        && exitFrame.type == protocol::MSG_EXIT)
                    {
                        ui::color::Green();
                        std::wcout << L"[+] DLL disconnected\n";
                        ui::color::Reset();
                    }
                }
                break;
            }

            // 表达式包装 return 只影响 REPL 回显，不改变已经是语句的源码。
            const std::string code = repl::IsStatement(input) ? input : "return " + input;
            if (!repl::ExecuteCommand(server, code, protocol::COMMAND_TIMEOUT)
                && !server.IsConnected())
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
    server.Stop();
    Injector::CloseSharedMemory();

    if (ctrlHandlerInstalled) SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    if (result == 0)
    {
        ui::color::Green();
        std::wcout << L"[+] Done\n\n";
        ui::color::Reset();
        std::wcout << L"Thanks for using\n\n";
    }

    return result;
}
