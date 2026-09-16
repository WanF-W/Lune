# HostCore

HostCore 是 Windows x64 原生 C++ 动态库，承接 Lune 的进程定位、后端配置、DLL 注入、命名管道、协议编解码和会话管理。它不输出控制台内容，也不依赖三个后端的运行时实现。

## 项目与产物

- `Lune.slnx` 中包含 `Lune.vcxproj` 与 `HostCore.vcxproj` 两个项目：Visual Studio v145，C++20，Debug/Release x64。
- `include/hostcore.h`：唯一公开头文件，C ABI、不透明会话句柄、固定宽度整数。
- `src/hostcore.cpp`：定位、启动握手、命令响应、事件分发与生命周期。
- `src/backend_profile.h`：三个后端的原有 DLL、IPC 名称和版本配置。
- `src/injector.*`、`src/pipe_server.*`、`src/protocol.h`、`src/win_handle.h`：内部实现，不供调用者包含。

构建配置将 DLL 和导入库输出到 `bin/x64/<Configuration>/HostCore.dll`、`HostCore.lib`，中间文件在 `obj/`。Lune 通过同一解决方案中的项目引用链接导入库，并在构建后复制 HostCore.dll 到 Lune.exe 的输出目录。

本次实现没有执行编译、测试或目标进程注入；编译与运行验收需另行明确要求。

## 调用约定

1. 零初始化 `HC_SessionOptions`，填写 `structSize = sizeof(options)`、`abiVersion = HC_ABI_VERSION`、后端字符 `'i'` / `'m'` / `'u'`，以及 PID 或进程名（二选一）。DLL 路径可省略。
2. 注册事件回调、可选的取消查询回调和调用者上下文，然后调用 `HC_CreateSession`。输入字符串在创建时复制，不跨模块转移内存所有权。
3. `HC_StartSession` 同步完成进程与 DLL 定位、创建管道并提交异步连接、注入、等待连接完成、HELLO 版本检查和 READY 等待。失败时自动回收运行时资源，调用者仍须 Destroy 释放会话对象。
4. `HC_SendCommand` 接收 UTF-8 Lua 源码和字节长度，同步等待终结响应；日志仍在后台读线程实时回调。`HC_GetCommandTimeout` 与 `HC_GetStartupScriptTimeout` 提供原有默认值。
5. `HC_RequestExit` 发送退出通知并等待原有退出确认；它返回确认是否成功，不替代资源清理。
6. `HC_StopSession` 停止通信并回收资源，可重复调用。`HC_DestroySession` 包含 Stop，最终释放句柄；销毁后不得再使用该句柄。

会话只启动一次。Start/SendCommand/RequestExit/Stop/Destroy 由同一所有者串行调用，不能并发或从回调重入；只读连接状态查询允许并发。同一目标的固定 IPC 名称保持原样，因此不要同时向同一个目标启动多个会话。不同会话各自持有注入共享内存，避免一方清理另一方的句柄。

`HC_SUCCESS` 表示操作成功；`HC_FAILURE` 保留原有失败含义，具体启动阶段或后端错误通过事件返回；`HC_INVALID_ARGUMENT` 只用于新库 API 的参数或生命周期误用，不新增管道错误分类。`HC_SendCommand` 的超时保持原有规则：正数表示毫秒期限，`-1` 无限等待，`0` 持续非阻塞轮询直到终结响应或断线。

## 事件和 UI

事件包含种类、PID、UTF-16 文本、带长度的 UTF-8 文本，以及后端错误类别和行号。文本只在当前回调内有效；需异步分发时由调用者复制。回调不得抛出异常。后端错误的原有纯文本兼容路径保留。

当前公开 ABI 为 2，HostCore 与 Lune 必须使用同一版头文件配套构建；后端 DLL 协议版本不变。管道创建、连接和握手失败事件增加 `errorStage`、`errorStageName`、`errorApi`、`win32Error`、`timedOut` 和 UTF-16 `errorDescription`（`FormatMessageW` 文本）。Win32 API 失败保留原始错误码；协议错误、取消检查和条件变量握手超时使用对应系统错误码，`errorApi` 为 NULL。Lune 只显示这些字段，不推断连接状态或超时原因。

`Start` 只提交一次 `ConnectNamedPipe`，`WaitForClient` 只等待该请求。立即成功和 `ERROR_PIPE_CONNECTED` 均进入握手；`ERROR_IO_PENDING` 等待完成。只有连接等待的 `WaitForSingleObject` 返回 `WAIT_TIMEOUT` 且达到整体期限，才报告连接超时；等待失败、完成失败和读写断线保留各自错误。连接和握手等待、挂起写入以及命令响应等待会周期检查取消回调。

停止时取消 I/O，等待读写和挂起连接结束后释放句柄；读写提交与取消共享句柄锁，防止取消后又提交挂起 I/O。断线后读线程回收管道实例，已完整收到的终态帧可由当前调用消费，随后 Stop 清除剩余队列。再次注入必须创建新会话，不能重复启动旧会话。

启动事件在 Start 调用线程返回；日志和 `HC_DISCONNECTED` 从后台读线程返回。UI 应自行调度到合适的线程。Stop/Destroy 会等待读线程结束，返回后不会再收到后台回调；调用者上下文必须至少存活到 Destroy 返回。

Lune 收到 `HC_DISCONNECTED` 后设置自己持有的控制台取消事件，继续使用原有的事件等待取消 ReadConsoleW。HostCore 的管道句柄、重叠 I/O、线程及内部断线事件不会越过 DLL 边界。`HC_IsConnected` 查询连接标志；`HC_IsDisconnected` 查询断线或停止通知是否已置位。

未来 C# 调用时使用 x64、`CallingConvention.Cdecl`、顺序布局/8 字节打包、`IntPtr` 会话及字符串指针；宽字符串使用 UTF-16，源码/日志使用 UTF-8，布尔结果为 32 位整数。将回调委托及上下文保持存活直到 Destroy 返回，且不要从 UI 线程同步等待会把事件同步派发回该线程的工作线程。

## 本阶段保持的行为

- 三个后端的管道名、共享内存名、版本和帧格式不变。
- 注入等待 10 秒、连接等待 15 秒、HELLO 5 秒、READY 45 秒、命令 30 秒、启动脚本 60 秒、退出确认 3 秒。
- 共享内存仍在匹配 HELLO 后释放；原有注入回退与超时处理不变。
- 命令超时仍停止连接，避免迟到响应进入下一条命令。
- CLI 参数、Lua 表达式判断、启动脚本拼接、控制台输出及退出编排留在 Lune。
- 三个后端统一使用 HostCore 的连接和回收行为，不修改三个后端项目。
