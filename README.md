# Lune

Lune 是一个通用的 Windows x64 Lua 控制端：定位游戏进程、注入配套 DLL，
通过命名管道与 DLL 交互，并把 DLL 返回的日志和命令结果显示在控制台。

Lune 不处理 IL2CPP、Mono 或 Unreal 的运行时细节。类、对象、方法、属性、
Hook 和 Lua 运行时逻辑全部由配套 DLL 完成。

## 使用

启动时必须选择一个后端，并指定进程名或 PID：

```bat
lune -i -n Game.exe
lune -m -p 1234
lune -u -n Game-Win64-Shipping.exe
```

后端选项如下：

| 选项 | 配套 DLL | 会话提示符 |
| --- | --- | --- |
| `-i` | `Il2CppLua.dll` | `ilune >>` |
| `-m` | `MonoLua.dll` | `mlune >>` |
| `-u` | `UnrealLua.dll` | `ulune >>` |

通用选项：

| 选项 | 说明 |
| --- | --- |
| `-n <name>` / `--name <name>` | 按进程名定位 |
| `-p <pid>` / `--pid <pid>` | 按 PID 定位 |
| `-d <path>` / `--dll <path>` | 覆盖当前后端的默认 DLL 路径 |
| `-l <path>` / `--lua <path>` | 握手完成后执行启动脚本 |
| `-h` / `--help` | 显示帮助 |

默认情况下，Lune 从自身所在目录查找当前后端 DLL。

## 版本校验

`-i` 后端要求 DLL 握手字符串精确等于 `Il2CppLua/4.0.0`。不匹配时显示
expected / received 并终止连接，不进入 READY 等待或 REPL。

各后端的显示版本和握手字符串维护在 `src/backend_profile.h`。Lune 自身版本
维护在 `src/version.h`，独立于 DLL 版本；更新 IL2CPP 后端无需修改 Mono / Unreal 配置。

## 工作流程

```text
解析后端与目标进程
  -> 创建命名管道
  -> 创建共享内存并注入 DLL
  -> HELLO / READY 握手
  -> 转发启动脚本或用户输入
  -> 输出日志和命令结果
```

Lune 的会话使用 `HELLO`、`READY`、`LOG`、`OK`、`ERROR`、`CMD` 和 `EXIT`；
协议保留 `FILE` 类型，但当前命令行不发送该帧。
启动脚本会被拼成一条 `dofile("绝对路径")` 命令，通过 `MSG_CMD` 交给目标进程内
的 Lua 运行时执行。
目标进程或 DLL 断开时，控制台输入会被唤醒并结束当前会话；输入 `exit` 或 `quit` 时会
先等待 DLL 的退出确认，超时后再关闭管道。
日志内容按 DLL 返回的 UTF-8 文本输出，不根据文本猜测运行时类型。

## 构建

环境要求：Windows x64、Visual Studio 2026、v145 工具集和 Windows SDK。

```bat
msbuild Lune.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物为 `Lune.exe`。运行时通过 `-i`、`-m` 或 `-u` 选择对应协议配置。

## 目录结构

```text
src/
  lune.cpp             参数解析、注入、握手和会话编排
  backend_profile.h    IL2CPP / Mono / Unreal 的 DLL 与协议配置
  injector.*           进程定位、共享内存和 DLL 注入
  pipe_server.*        命名管道和二进制帧传输
  protocol.h           通用消息类型、帧格式和超时限制
  console_ui.*         UTF-8 控制台输入输出和提示符
  repl.*               Lua 命令发送与响应处理
  win_handle.h         Windows HANDLE RAII 封装
  version.*            Lune 自身版本资源
```

## License

MIT License。
