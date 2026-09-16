# Lune 2.0.0 Release Notes

## 版本信息

- Lune 产品版本：`2.0.0`
- HostCore 产品版本：`1.0.0`
- 版本类型：架构重构。本次主要将 HostCore 从 Lune 前端中抽离为独立项目，不改变 Lune 的命令行使用方式和后端协议帧格式。
- 版本含义：Lune `2.0.0` 与 HostCore `1.0.0` 都只是产品迭代记录，不代表协议版本、运行时版本或兼容性版本，也不参与连接过程中的版本校验。
- 整理依据：当前 Lune 仓库的源码、工程文件、HostCore 项目和使用文档。
- 本次只进行源码、工程和文档调整，未编译、未运行测试或启动目标游戏；下文描述的是已落到工作区的实现，不代表已经完成运行时验证。

<a name="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [`src/version.h`](../src/version.h) 记录 Lune 的产品版本 `2.0.0`，用于 Lune 自身的版本资源和产品迭代记录。
- HostCore 产品版本 `1.0.0`作为本次组件抽离的迭代记录；当前 HostCore 的公开接口版本由 `HC_ABI_VERSION` 单独定义。
- Lune 产品版本和 HostCore 产品版本不会写入后端 HELLO 握手，也不用于判断 `Lune.exe`、`HostCore.dll` 与后端 DLL 是否匹配。
- 实际配套关系由所选后端的协议配置决定。`HostCore/src/backend_profile.h` 中的后端版本、IPC 名称和握手字符串必须与对应的 `Il2CppLua.dll`、`MonoLua.dll` 或 `UnrealLua.dll` 一致。
- HostCore 公开 C ABI 当前为 `HC_ABI_VERSION 2`；它用于检查公开头文件与 HostCore 实现的接口布局，与 HostCore 产品版本 `1.0.0` 是两套不同的标识。
- 后端 DLL 的 HELLO 字符串不匹配时，连接仍会在进入 READY 或 REPL 前终止。仅修改 Lune 或 HostCore 产品版本号不会改变这一行为。

<a name="hostcore-extraction"></a>
## 2. HostCore 抽离与职责边界

本版本将 Lune 原有的底层进程控制和通信实现整理到 HostCore：

- 目标进程定位；
- 后端 DLL 路径处理与 DLL 注入；
- 共享内存和命名管道通信；
- 帧协议收发、HELLO/READY 流程和超时处理；
- 日志、错误和断线事件分发；
- 会话启动、停止、退出和资源回收。

Lune 保留控制端职责：

- 命令行参数解析；
- 控制台输入输出、颜色和提示符；
- Lua 输入分类与 REPL 交互；
- 启动脚本、退出流程和用户可见错误展示；
- Lune 自身产品版本记录。

HostCore 不负责控制台显示，也不依赖 Lune 的 UI 实现。Lune 只包含 HostCore 的公开头文件，通过 [`HostCore/include/hostcore.h`](../HostCore/include/hostcore.h) 提供的 C ABI 创建和管理会话。这样同一套核心能力可以继续服务于命令行前端，也可以供后续 WinForms 等 UI 工具复用。

当前运行关系如下：

```text
Lune.exe
  └─ HostCore.dll
       ├─ 定位目标进程、注入后端 DLL、管理会话和命名管道
       └─ 连接 Il2CppLua.dll / MonoLua.dll / UnrealLua.dll
```

<a name="session-and-behavior"></a>
## 3. 会话流程与既有行为

- `HC_StartSession` 负责依次执行目标定位、管道创建、后端 DLL 注入、连接等待、HELLO 校验和 READY 等待。
- `HC_SendCommand`、`HC_RequestExit`、`HC_StopSession` 和 `HC_DestroySession` 负责命令发送、退出通知、通信停止和资源回收。
- HostCore 将日志、连接状态、错误和断线结果以结构化事件交给 Lune；Lune 负责将这些事件映射为原有控制台输出。
- 当前默认等待上限保持不变：注入 10 秒、连接 15 秒、HELLO 5 秒、READY 45 秒、普通命令 30 秒、启动脚本 60 秒、退出确认 3 秒。
- 三个后端原有的管道名、共享内存名、协议帧格式和后端协议版本不因本次抽离改变。
- Lune 仍通过 `-i`、`-m` 或 `-u` 选择后端，也可以通过 `--dll` 指定后端 DLL 路径。

<a name="repository-and-project"></a>
## 4. 仓库、解决方案与工程调整

Lune 与 HostCore 现在位于同一个 Git 仓库，并由同一个解决方案管理：

```text
Lune/
├─ Lune.slnx
├─ Lune.vcxproj
├─ src/                         # Lune 控制端
├─ release_notes/               # 长期发布记录
└─ HostCore/
   ├─ HostCore.vcxproj          # HostCore 动态库
   ├─ include/hostcore.h        # 公开 C ABI
   └─ src/                      # 注入、管道、协议和会话实现
```

- [`Lune.slnx`](../Lune.slnx) 包含 `Lune.vcxproj` 和 `HostCore/HostCore.vcxproj` 两个项目。
- [`Lune.vcxproj`](../Lune.vcxproj) 通过项目引用链接 `HostCore.lib`，并在构建后将 `HostCore.dll` 复制到 Lune 输出目录。
- `backend_profile.h`、`injector.*`、`pipe_server.*`、`protocol.h` 和 `win_handle.h` 已归入 `HostCore/src/`；Lune 不再直接包含这些内部实现。
- Lune 只依赖 HostCore 的公开头文件，不依赖 HostCore 内部管道、注入器或协议头文件。
- HostCore 的 `.vs/`、`bin/`、`obj/` 等本地工程和编译产物不纳入 Git 版本控制。
- README 保持当前版本使用手册，历史变更由本文件记录。

<a name="protocol-and-compatibility"></a>
## 5. 协议、ABI 与兼容性

本次调整区分三类不同的版本标识：

| 标识 | 示例 | 是否参与后端连接校验 | 用途 |
| --- | --- | --- | --- |
| Lune 产品版本 | `2.0.0` | 否 | Lune 产品迭代记录 |
| HostCore 产品版本 | `1.0.0` | 否 | HostCore 组件迭代记录 |
| HostCore C ABI | `HC_ABI_VERSION 2` | 用于 API 接口匹配 | 检查公开头文件与实现是否配套 |
| 后端协议版本 | `Il2CppLua/4.1.1` 等 | 是 | HELLO 握手与后端配套检查 |

连接时，HostCore 根据 [`backend_profile.h`](../HostCore/src/backend_profile.h) 使用所选后端的握手配置。Lune 2.0.0 与 HostCore 1.0.0 可以作为产品记录独立递增，但不能替代后端协议版本，也不能替代 HostCore C ABI 检查。

后端协议版本发生变化时，仍需同步更新 HostCore 的对应 profile，并准备匹配版本的后端 DLL。版本不匹配时继续显示 `expected` / `received` 并终止连接。

<a name="build-and-distribution"></a>
## 6. 构建、发布与运行依赖

构建环境：

- Windows x64；
- Visual Studio 2026，v145 工具集；
- Windows SDK 10.0；
- C++20。

在 Visual Studio 中打开 [`Lune.slnx`](../Lune.slnx)，选择 `Release | x64` 生成解决方案。Lune 项目会先构建 HostCore 项目，再链接 `HostCore.lib`，并将 `HostCore.dll` 复制到 Lune 输出目录。

构建后的最小运行目录需要包含：

```text
Lune.exe
HostCore.dll
Il2CppLua.dll       # 使用 -i 时需要
MonoLua.dll         # 使用 -m 时需要
UnrealLua.dll       # 使用 -u 时需要
```

三个后端 DLL 由各自项目单独构建或获取，实际运行时只需要准备本次选择的一个。后端 DLL 必须与目标游戏运行时、进程位数以及 HostCore 中对应的协议配置匹配。注入仍受目标进程权限和系统安全策略影响，必要时需要管理员权限。

<a name="migration"></a>
## 7. 迁移说明

- 新克隆的 Lune 仓库只需要克隆一个仓库，不再需要额外准备同级的 `HostCore` 目录。
- 现有工作区应使用根目录的 `Lune.slnx`；不再使用独立的 `HostCore.slnx`。
- 外部脚本或工程如果仍引用 `..\\HostCore`，需要改为仓库内的 `HostCore` 路径。
- 运行时发布目录仍需要同时携带 `Lune.exe` 和 `HostCore.dll`；HostCore 被合入仓库不代表 DLL 会被静态链接进 Lune.exe。

<a name="project-and-documentation"></a>
## 8. 工程与文档调整

- 将 HostCore 源码、公开头文件、工程文件和内部文档纳入 Lune 仓库。
- 将 Lune 解决方案调整为同时包含 Lune 与 HostCore 两个项目。
- 将 Lune 的底层调用改为通过 HostCore C ABI 创建、控制和销毁会话。
- 更新 Lune README、HostCore README 和工程路径说明，明确产品版本、ABI 版本和后端协议版本的区别。
- 新增本文件作为 Lune 2.0.0 的长期详细发布记录。
