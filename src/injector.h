// ============================================================
// injector.h — Lune DLL 注入器声明
// ============================================================
// 本模块运行在 lune.exe（注入器）中，负责进程定位、共享内存和 DLL 注入。
// 远程线程未完成前不得释放 DLL 路径；共享内存句柄由握手流程决定释放时机。
// 仅针对 Windows x64。
// ============================================================
#pragma once
#include <windows.h>
#include <string>

// ============================================================
// Injector — DLL 注入器（静态工具类）
// ============================================================
class Injector
{
public:
    // 使用 CreateToolhelp32Snapshot 枚举所有进程，并以不区分大小写的方式
    // 按 processName 比较进程名；返回 0 表示未找到。
    static DWORD FindProcessByName(const std::wstring& processName);

    // 获取指定 PID 的进程名；失败返回空字符串。
    static std::wstring GetProcessName(DWORD pid);

    // 按注入所需权限打开目标进程；失败返回 nullptr。
    // 所需权限：PROCESS_CREATE_THREAD、PROCESS_VM_OPERATION、PROCESS_VM_WRITE、
    // PROCESS_QUERY_INFORMATION。
    static HANDLE OpenTargetProcess(DWORD pid);

    // ============================================================
    // DLL 注入
    // ============================================================
    // 创建共享内存后，将 DLL 路径传入目标进程并启动远程 LoadLibraryW 线程。
    // 远程线程完成后释放路径内存；共享内存句柄由握手完成后的调用方关闭。
    static bool Inject(
        DWORD pid,
        const std::wstring& dllPath,
        const wchar_t* pipeName,
        const wchar_t* sharedMemoryPrefix);

    // 释放共享内存
    static void CloseSharedMemory();

private:
    // 禁止实例化（纯静态工具类）
    Injector() = delete;
    ~Injector() = delete;

    // 创建共享内存并写入管道名称；共享内存名称由目标 PID 参与构造。
    static HANDLE CreateSharedMemory(
        DWORD pid,
        const wchar_t* pipeName,
        const wchar_t* sharedMemoryPrefix);

    // 使用 NtCreateThreadEx 作为 CreateRemoteThread 的后备方案。
    // 该 API 未文档化，因此只在首选方式失败时动态加载并尝试使用。
    static HANDLE InjectViaNtCreateThreadEx(HANDLE hProcess, void* loadLibrary, void* param);

    // 共享内存句柄
    static HANDLE shareMemHandle;
};
