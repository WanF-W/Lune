// ============================================================
// injector.cpp — DLL 注入器实现
// ============================================================
// 本文件实现 injector.h 中声明的 Injector 类。
// 负责进程查找、共享内存创建和远程线程注入，不处理管道帧和 Lua 业务。
// ============================================================

#include "injector.h"
#include "protocol.h"
#include "win_handle.h"

// Windows API
#include <tlhelp32.h> // CreateToolhelp32Snapshot（进程枚举）
#include <cwchar>     // _wcsicmp（宽字符串不区分大小写比较）
#include <vector>


// ============================================================
// NtCreateThreadEx 函数指针类型定义
// ============================================================
// NtCreateThreadEx 是 ntdll.dll 中的未文档化 API。
// 它只作为可选后备路径，不能替代正常的 CreateRemoteThread。

// NtCreateThreadEx 函数指针类型
typedef NTSTATUS(NTAPI* pfnNtCreateThreadEx)(
    PHANDLE                hThread,           // [out] 线程句柄
    ACCESS_MASK            DesiredAccess,     // 请求的访问权限
    LPVOID                 ObjectAttributes,  // 对象属性（通常为 NULL）
    HANDLE                 ProcessHandle,     // 目标进程句柄
    LPTHREAD_START_ROUTINE lpStartAddress,    // 线程函数地址
    LPVOID                 lpParameter,       // 线程参数
    BOOL                   CreateSuspended,   // 是否创建后挂起
    ULONG                  StackZeroBits,     // 栈零填充位数
    ULONG                  SizeOfStackCommit, // 栈提交大小（0 = 默认）
    ULONG                  SizeOfStackReserve,// 栈保留大小（0 = 默认）
    LPVOID                 lpBytesBuffer);    // 属性缓冲区（通常为 NULL）


// ============================================================
// 按进程名查找进程 ID
// ============================================================
DWORD Injector::FindProcessByName(const std::wstring& processName)
{
    // 创建进程快照（包含所有进程的信息）
    // TH32CS_SNAPPROCESS 表示拍摄进程列表快照
    // 0表示所有进程
    win::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));

    // 快照创建失败
    if (!snapshot) return 0;

    // 初始化进程条目结构
    PROCESSENTRY32W pe32{};
    // 必须设置大小
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    // 查找结果
    DWORD resultPid = 0;

    // 遍历进程列表
    if (Process32FirstW(snapshot.Get(), &pe32))
    {
        // 遍历所有进程
        do
        {
            // 不区分大小写比较进程名
            // pe32.szExeFile 是可执行文件的文件名（不含路径）
            if (_wcsicmp(pe32.szExeFile, processName.c_str()) == 0)
            {
                // 找到匹配的进程
                resultPid = pe32.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot.Get(), &pe32));
    }

    // 返回结果
    return resultPid;
}


// ============================================================
// 获取指定 PID 的进程名
// ============================================================
std::wstring Injector::GetProcessName(DWORD pid)
{
    // 创建进程快照
    win::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));

    // 快照创建失败
    if (!snapshot) return L"";

    // 初始化进程条目结构
    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    // 查找结果
    std::wstring result;

    // 遍历进程列表
    if (Process32FirstW(snapshot.Get(), &pe32))
    {
        do
        {
            if (pe32.th32ProcessID == pid)
            {
                // 找到目标进程
                result = pe32.szExeFile;
                break;
            }
        } while (Process32NextW(snapshot.Get(), &pe32));
    }

    return result;
}


// ============================================================
// 打开目标进程
// ============================================================
HANDLE Injector::OpenTargetProcess(DWORD pid)
{
    // 定义所需的访问权限
    // 这些权限是 DLL 注入所必需的
    DWORD accessRights =
        PROCESS_CREATE_THREAD |    // 创建远程线程
        PROCESS_VM_OPERATION |     // VirtualAllocEx / VirtualFreeEx
        PROCESS_VM_WRITE |         // WriteProcessMemory
        PROCESS_QUERY_INFORMATION; // 查询进程信息

    // 打开目标进程
    HANDLE hProcess = OpenProcess(
        accessRights,    // 请求的访问权限
        FALSE,           // 不继承句柄
        pid);            // 目标进程 ID

    return hProcess;     // OpenProcess 失败返回 nullptr
}


// ============================================================
// 创建共享内存并写入管道名称
// ============================================================
HANDLE Injector::CreateSharedMemory(
    DWORD pid,
    const wchar_t* pipeName,
    const wchar_t* sharedMemoryPrefix)
{
    // 共享内存名称由 PID 决定，参数无效时不要创建任何系统对象。
    if (pid == 0 || pipeName == nullptr || *pipeName == L'\0'
        || sharedMemoryPrefix == nullptr || *sharedMemoryPrefix == L'\0')
    {
        return nullptr;
    }

    // 构造共享内存名称
    // 格式：<后端共享内存前缀><PID>
    // DLL 用 GetCurrentProcessId() 获取相同的 PID 来打开此共享内存
    // 固定名称是现有 DLL 协议的一部分，不能在此处改成随机名称。
    wchar_t shmName[128];
    // swprintf_s(目标宽字符数组, 宽字符数量,格式化字符串必须加 L 前缀 如 L"Hello %s",...可变参数)
    if (swprintf_s(shmName, 128, L"%s%lu", sharedMemoryPrefix, pid) < 0) return nullptr;

    // 创建共享内存
    // CreateFileMappingW 创建一个可被其他进程通过名称打开的共享内存
    // PAGE_READWRITE 表示可读写
    // 使用页文件创建匿名 backing store，再通过名称让目标进程打开它。
    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,                           // 不关联文件（纯内存映射）
        nullptr,                                        // 默认安全属性
        PAGE_READWRITE,                                 // 读写权限
        0,                                              // 高 32 位大小（0 表示不超过 4GB）
        static_cast<DWORD>(protocol::SHARED_MEM_SIZE),  // 低 32 位大小（512 字节）
        shmName);                                       // 共享内存名称

    if (hMap == nullptr) return nullptr;                // 创建失败

    // 映射共享内存到本进程地址空间
    // 只在当前进程映射一次；写完后解除映射，但保留句柄给 DLL 打开。
    void* mapped = MapViewOfFile(
        hMap,           // 共享内存句柄
        FILE_MAP_WRITE, // 写入权限
        0, 0, 0);       // 从开头开始 映射全部

    // 映射失败 关闭句柄
    if (mapped == nullptr)
    {
        CloseHandle(hMap);
        return nullptr;
    }

    // 写入管道名称
    // 清零共享内存（确保没有残留数据）
    // 先清零整个区域，确保 DLL 不会看到上一次残留的尾部数据。
    ZeroMemory(mapped, protocol::SHARED_MEM_SIZE);

    // 将管道名称拷贝到共享内存
    // 使用 wcsncpy_s 防止缓冲区溢出
    // 安全拷贝宽字符串方法 wcsncpy_s(目标缓冲区, 目标最多容纳的宽字符数, 源字符串, 截断策略)
    // 使用安全拷贝并保留结尾零，DLL 会把这块区域当作宽字符串读取。
    const errno_t copyResult = wcsncpy_s(
        static_cast<wchar_t*>(mapped),
        protocol::SHARED_MEM_SIZE / sizeof(wchar_t),
        pipeName,
        _TRUNCATE);

    if (copyResult != 0)
    {
        UnmapViewOfFile(mapped);
        CloseHandle(hMap);
        return nullptr;
    }

    // 解除映射（但保持共享内存句柄打开）
    // 共享内存只要至少有一个打开的句柄就保持有效
    // DLL 会通过 OpenFileMappingW 打开它
    // DLL 仍可通过名称打开 mapping；本地映射已经不再需要。
    UnmapViewOfFile(mapped);

    // 返回句柄 调用方负责后续关闭
    return hMap;
}


// 初始化
void* Injector::shareMemHandle = nullptr;

// ============================================================
// DLL 注入主函数
// ============================================================
bool Injector::Inject(
    DWORD pid,
    const std::wstring& dllPath,
    const wchar_t* pipeName,
    const wchar_t* sharedMemoryPrefix)
{
    // 注入流程需要同时持有进程句柄、共享内存句柄和远程路径内存。
    // 入口先拒绝明显无效的参数，避免产生半初始化状态。
    if (pid == 0 || dllPath.empty() || pipeName == nullptr || *pipeName == L'\0'
        || sharedMemoryPrefix == nullptr || *sharedMemoryPrefix == L'\0')
    {
        return false;
    }

    // 共享内存需要跨越 Inject 与 HELLO 握手两个阶段，因此由静态句柄持有。
    // 每次新注入前先关闭上一次可能遗留的句柄，避免句柄状态串线。
    // 清理上一轮可能残留的共享内存，保证同一个 Injector 状态不会串线。
    CloseSharedMemory();
    shareMemHandle = CreateSharedMemory(pid, pipeName, sharedMemoryPrefix);
    if (shareMemHandle == nullptr) return false;

    // 进程句柄使用 RAII；任意后续失败都会自动关闭它。
    win::UniqueHandle process(OpenTargetProcess(pid));
    if (!process)
    {
        CloseSharedMemory();
        return false;
    }

    // 动态扩大缓冲区，避免把 DLL 路径限制在 MAX_PATH 内。
    // LoadLibraryW 需要目标进程可访问的绝对路径；初始仍使用常见 MAX_PATH 容量。
    std::vector<wchar_t> pathBuffer(MAX_PATH);
    DWORD pathLength = GetFullPathNameW(
        dllPath.c_str(),
        static_cast<DWORD>(pathBuffer.size()),
        pathBuffer.data(),
        nullptr);

    while (pathLength >= pathBuffer.size())
    {
        // 长路径时 GetFullPathNameW 返回所需容量，按返回值扩大后重试。
        pathBuffer.resize(pathLength + 1);
        pathLength = GetFullPathNameW(
            dllPath.c_str(),
            static_cast<DWORD>(pathBuffer.size()),
            pathBuffer.data(),
            nullptr);
    }

    if (pathLength == 0)
    {
        CloseSharedMemory();
        return false;
    }

    // 路径解析成功后确认它确实是文件，而不是目录或不存在的路径。
    const std::wstring fullPath(pathBuffer.data(), pathLength);
    const DWORD attributes = GetFileAttributesW(fullPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        CloseSharedMemory();
        return false;
    }

    // 远程线程只接收 DLL 路径指针，因此要把完整 UTF-16 字符串复制到目标进程。
    const size_t dllPathSize = (fullPath.size() + 1) * sizeof(wchar_t);
    void* remoteMemory = VirtualAllocEx(
        process.Get(),
        nullptr,
        dllPathSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (remoteMemory == nullptr)
    {
        CloseSharedMemory();
        return false;
    }

    // 这个 lambda 只释放已经确认安全的远程内存；超时路径不能调用它。
    auto freeRemoteMemory = [&]() {
        if (remoteMemory != nullptr)
        {
            VirtualFreeEx(process.Get(), remoteMemory, 0, MEM_RELEASE);
            remoteMemory = nullptr;
        }
    };

    // 写入后检查实际字节数，避免远程线程读取截断路径。
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(
            process.Get(),
            remoteMemory,
            fullPath.c_str(),
            dllPathSize,
            &bytesWritten)
        || bytesWritten != dllPathSize)
    {
        freeRemoteMemory();
        CloseSharedMemory();
        return false;
    }

    // LoadLibraryW 位于目标进程必有的 kernel32.dll 中，取得本进程的导出地址。
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* loadLibrary = kernel32 != nullptr
        ? reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW"))
        : nullptr;
    if (loadLibrary == nullptr)
    {
        freeRemoteMemory();
        CloseSharedMemory();
        return false;
    }

    // 优先使用文档化 API，失败后才尝试未文档化的 NtCreateThreadEx。
    HANDLE rawThread = CreateRemoteThread(
        process.Get(),
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
        remoteMemory,
        0,
        nullptr);
    if (rawThread == nullptr)
    {
        rawThread = InjectViaNtCreateThreadEx(process.Get(), loadLibrary, remoteMemory);
    }

    // 线程句柄由 RAII 持有；只要线程尚未完成，远程路径内存就必须保留。
    win::UniqueHandle thread(rawThread);
    if (!thread)
    {
        freeRemoteMemory();
        CloseSharedMemory();
        return false;
    }

    // 等待 LoadLibraryW 完成，超时不能强行假设线程已经停止。
    const DWORD waitResult = WaitForSingleObject(thread.Get(), protocol::DLLINIT_TIMEOUT);
    if (waitResult != WAIT_OBJECT_0)
    {
        // 不能终止仍在执行的加载线程，也不能释放它可能正在读取的路径。
        // 保留远程内存比制造目标进程 use-after-free 更安全；本次注入结果视为不确定。
        CloseSharedMemory();
        return false;
    }

    // 只有线程明确结束后，才能读取返回值并释放远程路径内存。
    // 共享内存必须继续保持到 DLL 工作线程读出管道名并发送 HELLO；
    // LoadLibraryW 返回只表示 DllMain 已结束，不表示工作线程已经运行。
    DWORD exitCode = 0;
    const bool gotExitCode = GetExitCodeThread(thread.Get(), &exitCode) != FALSE;
    freeRemoteMemory();
    return gotExitCode && exitCode != 0;
}


// ============================================================
// 使用 NtCreateThreadEx 注入 DLL（后备方案）
// ============================================================
HANDLE Injector::InjectViaNtCreateThreadEx(HANDLE hProcess, void* loadLibrary, void* param)
{
    // 这是兼容性后备路径；主路径失败时才动态查找，避免静态依赖未文档化 API。
    // 动态加载 ntdll.dll
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    // 无法获取 ntdll 句柄
    if (hNtdll == nullptr) return nullptr;

    // 获取 NtCreateThreadEx 函数地址
    auto pNtCreateThreadEx = reinterpret_cast<pfnNtCreateThreadEx>(GetProcAddress(hNtdll, "NtCreateThreadEx"));

    // 函数不存在
    if (pNtCreateThreadEx == nullptr) return nullptr;

    // 调用 NtCreateThreadEx 创建远程线程
    HANDLE hThread = nullptr;
    // 线程参数仍然是目标进程中的 DLL 路径地址，所有权规则与主路径相同。
    NTSTATUS status = pNtCreateThreadEx(
        &hThread,                                              // [out] 线程句柄
        THREAD_ALL_ACCESS,                                     // 完全访问权限
        nullptr,                                               // 默认对象属性
        hProcess,                                              // 目标进程句柄
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), // 线程函数
        param,                                                 // 参数（DLL 路径地址）
        FALSE,                                                 // 不挂起
        0,                                                     // 默认栈零填充
        0,                                                     // 默认栈提交大小
        0,                                                     // 默认栈保留大小
        nullptr);                                              // 无属性缓冲区

    // 检查返回状态
    // NTSTATUS >= 0 表示成功
    if (status < 0 || hThread == nullptr) return nullptr;

    // 成功 返回线程句柄
    return hThread;
}

// ============================================================
// 清理释放共享内存
// ============================================================
void Injector::CloseSharedMemory()
{
    // 共享内存句柄由 Inject 创建，但其释放时机由握手流程决定。
    if (shareMemHandle)
    {
        CloseHandle(shareMemHandle);
        shareMemHandle = nullptr;
    }
}
