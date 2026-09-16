#include "session_ui.h"
#include "console_ui.h"
#include <iostream>
#include <string>

namespace ui {
void HC_CALL DisplaySessionEvent(void*, const HC_Event* e) {
    if (!e) return;
    const std::string text = e->textLength ? std::string(e->text, e->textLength) : std::string{};
    if (e->kind == HC_LOG) { QueueAsyncLog(text.c_str()); return; }
    DrainAsyncLogs(false);
    if (e->errorStage != HC_STAGE_NONE) {
        color::Red();
        std::string message = "[Lune Error] " + std::string(e->errorStageName ? e->errorStageName : "Pipe")
            + (e->timedOut ? " timeout" : " failed");
        if (e->errorApi) message += " / " + std::string(e->errorApi);
        message += " / Win32 " + std::to_string(e->win32Error) + ": ";
        SafePrintUtf8(message, true);
        std::wcerr << (e->errorDescription ? e->errorDescription : L"") << L"\n";
        color::Reset();
        return;
    }
    switch (e->kind) {
    case HC_LOCATING_TARGET: color::Gray(); std::wcout << L"[*] Locating target process...\n"; break;
    case HC_TARGET_FOUND:
        color::Green(); std::wcout << L"[+] Target: " << e->wideText << L" (PID: " << e->pid << L")\n"; break;
    case HC_TARGET_ERROR:
        color::Red();
        if (e->wideText) std::wcerr << L"[Lune Error] Process not found: " << e->wideText << L"\n";
        else std::wcerr << L"[Lune Error] Cannot open process with PID " << e->pid << L"\n";
        break;
    case HC_LOCATING_DLL: color::Gray(); std::wcout << L"[*] Locating " << e->wideText << L"...\n"; break;
    case HC_DLL_FOUND: color::Green(); std::wcout << L"[+] DLL: " << e->wideText << L"\n"; break;
    case HC_DLL_ERROR: color::Red(); std::wcerr << L"[Lune Error] DLL not found or is not a file: " << e->wideText << L"\n"; break;
    case HC_DIRECTORY_ERROR: color::Red(); std::wcerr << L"[Lune Error] Cannot determine the Lune executable directory.\n"; break;
    case HC_DEFAULT_DLL_ERROR:
        color::Red(); std::wcerr << L"[Lune Error] Cannot find " << e->wideText << L" next to lune.exe.\n"
            << L"    Use --dll to specify the path explicitly.\n"; break;
    case HC_PIPE_NAME_ERROR: color::Red(); std::wcerr << L"[Lune Error] Failed to build the pipe name.\n"; break;
    case HC_CREATING_PIPE: color::Gray(); std::wcout << L"[*] Creating pipe server...\n"; break;
    case HC_PIPE_ERROR: color::Red(); std::wcerr << L"[Lune Error] Failed to create pipe server.\n"; break;
    case HC_INJECTING: color::Gray(); std::wcout << L"[*] Injecting DLL...\n"; break;
    case HC_INJECTION_ERROR: color::Red(); std::wcerr << L"[Lune Error] Injection failed.\n"; break;
    case HC_INJECTED: color::Green(); std::wcout << L"[+] DLL injected\n"; break;
    case HC_CONNECTING: color::Gray(); std::wcout << L"[*] Waiting for DLL to connect...\n"; break;
    case HC_CONNECT_ERROR: color::Red(); std::wcerr << L"[Lune Error] DLL connection failed.\n"; break;
    case HC_CONNECTED: color::Green(); std::wcout << L"[+] DLL connected\n"; break;
    case HC_HELLO_ERROR: color::Red(); std::wcerr << L"[Lune Error] Handshake failed: no HELLO.\n"; break;
    case HC_VERSION_ERROR:
        color::Red(); SafePrintUtf8("[Lune Error] Version mismatch: expected "
            + std::string(e->expectedVersion) + ", received " + text + "\n", true); break;
    case HC_HELLO: color::Green(); SafePrintUtf8("[+] Handshake: " + text + "\n"); break;
    case HC_READY_ERROR: color::Red(); std::wcerr << L"[Lune Error] Handshake failed: no READY.\n"; break;
    case HC_READY: color::Green(); std::wcout << L"[+] Ready\n\n"; break;
    case HC_BACKEND_ERROR: {
        const char* label = "Lune Error";
        switch (e->category) {
        case 1: label = "Lua Error"; break;
        case 2: label = "Il2Cpp Error"; break;
        case 3: label = "CSharp Error"; break;
        case 5: label = "Mono Error"; break;
        }
        std::string message = text;
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) message.pop_back();
        if (message.empty()) message = "execution failed";
        std::string output = "[" + std::string(label) + "]";
        if (e->line > 0) output += " line " + std::to_string(e->line);
        color::Red(); SafePrintUtf8(output + ": " + message + "\n"); break;
    }
    case HC_COMMAND_TOO_LARGE: color::Red(); SafePrintUtf8("[Lune Error] Command is too large (maximum 1 MB)\n"); break;
    case HC_SEND_ERROR: color::Red(); SafePrintUtf8("[Lune Error] Failed to send command\n"); break;
    case HC_COMMAND_TIMEOUT: color::Red(); SafePrintUtf8("[Lune Error] Command timeout\n"); break;
    case HC_CONNECTION_LOST: color::Red(); SafePrintUtf8("[Lune Error] Connection lost\n"); break;
    case HC_EXIT_REQUESTED: color::Yellow(); SafePrintUtf8("[*] DLL requested disconnect\n"); break;
    default: break;
    }
    color::Reset();
}
}
