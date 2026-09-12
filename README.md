# Lune

Lune 是 [Il2CppLua](https://github.com/WanF-W/Il2CppLua)、[MonoLua](https://github.com/WanF-W/MonoLua) 和 [UnrealLua](https://github.com/WanF-W/UnrealLua) 共用的 Windows x64 控制台工具：查找目标进程、注入配套 DLL，并通过命名管道提供 Lua REPL。

Lune 由原来的 ILune、MLune、ULune 三个控制端合并而来，使用一个 `Lune.exe`，通过启动参数选择后端。

Lune 负责进程定位、注入、通信和控制台交互；运行时适配、反射、对象访问、方法调用、Hook 和 Lua 执行由对应 DLL 完成。两端通过 `HELLO` 帧严格校验所选后端的协议版本。

## 功能

- 通过 `-i`、`-m`、`-u` 选择 IL2CPP、Mono 或 Unreal 后端。
- 按进程名或 PID 查找目标进程。
- 默认从 `Lune.exe` 所在目录查找所选后端 DLL，也可以显式指定路径。
- 使用 `CreateRemoteThread` 注入，失败时尝试 `NtCreateThreadEx` 后备路径。
- 使用共享内存传递管道名称，使用命名管道传输长度前缀帧。
- 通过 `HELLO` 校验协议版本，通过 `READY` 等待 DLL 初始化完成。
- 实时显示 Lua 输出和 Hook 日志，展示错误类别及后端提供的行号。
- 自动区分 Lua 语句和表达式；表达式会包装为 `return ...` 以回显结果。
- 支持启动脚本、`exit` / `quit` 和 Ctrl+C 退出。

## 快速开始

将 `Lune.exe` 与所需后端的匹配版本 DLL 放在同一目录，先启动目标游戏，再选择对应后端连接。只需准备本次使用的 DLL。

| 后端 | 目标运行时 | 默认 DLL | 会话提示符 |
| --- | --- | --- | --- |
| `-i` | Unity IL2CPP | `Il2CppLua.dll` | `ilune >>` |
| `-m` | Unity Mono | `MonoLua.dll` | `mlune >>` |
| `-u` | Unreal Engine | `UnrealLua.dll` | `ulune >>` |

按进程名连接：

```bat
Lune.exe -i --name Game.exe
Lune.exe -m --name Game.exe
Lune.exe -u --name Game-Win64-Shipping.exe
```

以上命令分别对应三种后端，每次选择与目标游戏匹配的一条。也可以直接使用 PID：

```bat
Lune.exe -i --pid 1234
```

进入提示符后输入 Lua，例如：

```lua
print("hello from Lune")
1 + 2
```

运行时专用的 Lua API、对象模型和调用示例请参阅 [Il2CppLua README](https://github.com/WanF-W/Il2CppLua#readme)、[MonoLua README](https://github.com/WanF-W/MonoLua#readme) 或 [UnrealLua README](https://github.com/WanF-W/UnrealLua#readme)。

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `-i` | 使用 Il2CppLua 后端 |
| `-m` | 使用 MonoLua 后端 |
| `-u` | 使用 UnrealLua 后端 |
| `-n` / `--name <name>` | 目标进程名，不区分大小写 |
| `-p` / `--pid <pid>` | 目标进程 ID |
| `-d` / `--dll <path>` | DLL 路径；默认查找 exe 同目录的所选后端 DLL |
| `-l` / `--lua <path>` | 连接并完成初始化后，在游戏进程内执行启动脚本 |
| `-h` / `--help` | 显示帮助 |

启动会话时，`-i`、`-m`、`-u` 必须且只能指定一个，`--name` 和 `--pid` 也必须且只能指定一个：

```bat
Lune.exe -i --name Game.exe --dll C:\Mods\Il2CppLua.dll
Lune.exe -m --pid 1234 --lua .\scripts\startup.lua
Lune.exe -u --name Game-Win64-Shipping.exe --dll "C:\My Mods\UnrealLua.dll"
```

`--dll` 只覆盖 DLL 路径，不会改变后端、IPC 名称或握手版本。Lune 不会自动识别目标运行时；即使显式指定 DLL，也需要选择匹配的后端参数。

## 工作流程

```text
解析后端和参数
  -> 查找进程和 DLL
  -> 创建命名管道服务器
  -> 创建共享内存并注入 DLL
  -> 等待 DLL 连接
  -> HELLO 版本握手
  -> READY 初始化握手
  -> 可选执行启动脚本
  -> REPL：发送命令并等待 OK / ERROR
  -> 退出、断线或 Ctrl+C
```

后台读取线程负责接收帧，将 `MSG_LOG` 交给 UI 回调，其余控制帧进入有上限的响应队列，由主线程消费。UI 在等待响应和控制台输入期间显示异步日志。停止时先取消挂起的 I/O，再等待读取线程退出，最后关闭句柄。

## 通信协议与版本

帧格式为：

```text
[1 字节类型][4 字节小端长度][N 字节负载]
```

单帧最大负载为 1 MiB。当前消息类型如下：

| 方向 | 类型 | 用途 |
| --- | --- | --- |
| DLL -> Lune | `MSG_HELLO` | 发送后端协议版本 |
| DLL -> Lune | `MSG_READY` | 通知运行时与 Lua 初始化完成 |
| DLL -> Lune | `MSG_LOG` | Lua 输出或 Hook 日志 |
| DLL -> Lune | `MSG_ERROR` / `MSG_OK` | 返回命令结果 |
| Lune -> DLL | `MSG_CMD` | 执行 Lua 源码；启动脚本也通过此类型发送 |
| Lune -> DLL | `MSG_FILE` | 协议保留类型，当前命令行不发送 |
| 双向 | `MSG_EXIT` | 通知对端结束 |

Lune 将 `HELLO` 负载与所选后端的握手字符串精确比较。当前配置如下：

| 后端 | 要求的 HELLO 负载 |
| --- | --- |
| `-i` | `Il2CppLua/4.1.0` |
| `-m` | `MonoLua/2.0.0` |
| `-u` | `UnrealLua/1.0.0` |

版本不匹配时会显示 `expected` / `received` 并终止连接，不继续等待 `READY` 或进入 REPL。

[backend_profile.h](src/backend_profile.h) 维护各后端的默认 DLL、显示版本、IPC 名称前缀、握手字符串和提示符；[protocol.h](src/protocol.h) 维护通用消息类型、负载格式和超时；[version.h](src/version.h) 只维护 Lune 自身产品版本。Lune 的版本号与三个 DLL 的版本独立，配套关系以所选后端的协议配置为准。

`MSG_ERROR` 支持类别、可选行号和错误文本，控制台也保留对旧纯文本错误负载的显示兼容。该兼容不绕过 `HELLO` 版本校验。

## REPL 与脚本

单行表达式会自动包装为 `return <expr>`，赋值、控制流、函数定义、注释和多行输入会原样发送。REPL 的判断只是输入便利功能，最终语法判断仍由 Lua 完成。

```text
ilune >> print("hello")
hello

ilune >> 1 + 2
3
```

日志按 DLL 返回的 UTF-8 文本输出，Lune 不根据反射签名或 dump 文本猜测运行时类型并重新着色。

使用 `--lua` 执行启动脚本：

```bat
Lune.exe -i --name Game.exe --lua .\scripts\startup.lua
```

相对路径以启动 Lune 的当前工作目录为基准。Lune 将路径转为绝对路径，拼成 `dofile("绝对路径")`，再作为 `MSG_CMD` 发给游戏内 Lua；文件由目标进程读取，因此目标进程必须能够访问该文件。

Lune 不读取并传输启动脚本正文，1 MiB 的帧上限约束的是发送的命令负载，不是脚本文件大小。文件不存在、读取失败或 Lua 执行错误由后端返回；连接仍然有效时，显示错误后继续进入 REPL。启动脚本执行超时会终止连接。

REPL 中直接调用 `dofile()` 时，相对路径以游戏进程工作目录为基准。需要稳定定位文件时，建议使用绝对路径：

```lua
dofile([[C:\Scripts\test.lua]])
```

输入 `exit` 或 `quit` 会发送退出帧，最多等待 3 秒的 DLL 退出确认后关闭管道。Ctrl+C 请求结束当前会话并进入清理流程；目标进程或 DLL 断开时，控制台输入会被唤醒并退出。

## 目录结构

```text
README.md              使用说明
MIGRATION.md           ILune / MLune / ULune 合并与迁移说明
src/
  lune.cpp             参数、启动流程、握手和 REPL 主循环
  backend_profile.h    三个后端的 DLL、版本、IPC 名称和提示符配置
  console_ui.*         控制台输入、UTF-8 输出、颜色和异步日志
  repl.*               Lua 输入分类、命令发送和响应处理
  injector.*           进程查找、共享内存和 DLL 注入
  pipe_server.*        EXE 侧命名管道、重叠 I/O 和响应队列
  protocol.h           通用消息类型、负载格式和超时定义
  version.h            Lune 产品版本定义
  version.rc           Windows 文件版本资源
  win_handle.h         Windows HANDLE 的最小 RAII 封装
```

模块边界保持简单：`lune.cpp` 编排流程，`backend_profile` 提供后端配置，`injector` 不处理管道帧，`pipe_server` 不理解 Lua 业务，`repl` 不负责控制台渲染，UI 不参与协议和注入。

## 构建

环境要求：Windows x64、Visual Studio 2026（v145 工具集）和 Windows SDK 10.0。工程提供 `Release | x64` 与 `Debug | x64` 配置。

在 Visual Studio 中打开 `Lune.slnx`，选择 `Release | x64` 生成。工程启用 C++20、Level 4 警告、警告视为错误、SDL 检查和 UTF-8 源文件编码。

命令行构建示例：

```bat
msbuild Lune.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物为 `Lune.exe`；各后端 DLL 由对应项目单独构建。

## 运行限制与故障排查

- 仅支持 Windows x64，后端 DLL 必须适用于目标游戏的运行时与位数。
- 找不到 DLL 时，检查 `Lune.exe` 同目录下的文件，或使用 `--dll` 指定路径。
- 版本不匹配时，按 `expected` 提示更换配套 DLL；改文件名不会改变协议版本。
- 注入受目标进程权限和系统安全策略影响，必要时以管理员权限运行。
- DLL 加载线程等待超时后，注入结果不确定；Lune 保留可能仍被远程线程读取的路径内存。应重启目标进程后再重试。
- 管道连接、HELLO 和 READY 的等待上限分别为 15 秒、5 秒和 45 秒；普通命令为 30 秒，启动脚本为 60 秒。
- 命令超时、传输错误或响应队列溢出会结束当前连接，避免迟到的响应被下一条命令误消费；当前会话不自动重连。
- 脚本加载失败时，检查目标进程对脚本路径的访问权限，并区分 Lune 与游戏进程的工作目录。

## 维护约定

- 修改消息类型、负载格式或 IPC 名称规则时，同步检查受影响的 DLL 项目，保持两端协议兼容。
- 更新某个后端时，在 `backend_profile.h` 中维护该后端的版本与配置；Lune 自身产品版本由 `version.h` 单独维护。
- 运行时专用逻辑放在对应 DLL 中，共用的注入、传输和控制台流程保持统一。
- 资源所有权应在创建处明确，并尽量使用 RAII；跨线程停止先取消 I/O，再等待线程退出，最后关闭句柄。

## License

MIT License，详见 [LICENSE.txt](LICENSE.txt)。
