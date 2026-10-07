# NxCourier

当前版本 **v1.0.0**。版本号的单一来源是仓库根目录的 [`VERSION`](VERSION)：设备侧由 Makefile
在构建时注入（`identify.id` 报的 `version=` 就是它），PC 侧的 MCP 服务器读同一个文件。

在一台装了 **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**（开源自制系统）的主机上常驻一个
自写的系统模块（sysmodule），把主机能力暴露成一条**行文本协议**的网络接口。局域网里的 PC 脚本与 AI
客户端通过它读状态、读写 SD 卡、模拟按键、抓画面、重启主机，不需要额外的桌面端工具，也不需要拔卡。

* **设备侧**：14 个工具 / 54 个方法，监听 TCP `47800`
* **PC 侧**：一个 MCP 服务器（6 个工具）+ 一组命令行脚本

## 设计要点

**能力自描述。** 设备自己知道会什么：`tools.list` 一条命令返回全部工具、方法、参数与风险等级，
`tools.doc` 取某个方法的完整教程。调用方不需要预先知道任何方法名，PC 侧也不保存工具清单 ——
设备侧加了新工具，PC 侧自动就能用。

**核心与工具解耦。** 核心只负责协议与调度；每个工具的参数、风险等级、文档都写在它自己的
`source/tools/tool_*.cpp` 里，靠链接器段在启动时自动注册。新增一个工具不需要改核心、不需要改 Makefile。

**PC 侧只有一个执行入口。** MCP 服务器不把 14 个设备工具硬翻成 14 个 MCP 工具，而是「两个发现/执行工具
+ 四个文件与调试工具」。

## 能干什么

| 工具 | 方法数 | 用途 |
| --- | --- | --- |
| `identify` | 1 | 平台 / 固件版本、开机次数、工具数、依赖服务状态 |
| `tele` | 5 | 频率 / 温度 / 电量 / 性能模式 |
| `title` | 6 | 装了哪些应用、正在跑什么、启动与结束（带口令） |
| `screen` | 4 | 抓一帧 JPEG、等画面变化、等画面稳定 |
| `input` | 5 | 模拟按键与摇杆（虚拟手柄），可一次跑一串动作 |
| `mem` | 4 | 读 / 写运行中进程的内存，不冻结进程（与 PC 侧 GDB 互斥） |
| `net` | 3 | 本机 IP / 试连一个端口 / 自检外部依赖 |
| `fs` | 8 | SD 卡文件：读、写、列、复制、建目录、删 |
| `save` | 2 | 枚举存档；把某个应用的存档整棵目录树备份到 SD |
| `boot` | 3 | 读写 Hekate 启动项（改前自动备份） |
| `power` | 3 | 电源状态、重启、关机（带口令） |
| `clk` | 4 | CPU / GPU / MEM 频率，带原值回滚 |
| `probe` | 4 | 逐个试 36 个系统服务，产出能力矩阵 |
| `log` | 2 | 取走模块内存里的环形日志 |

另有 5 条核心内建命令：`hello` · `caps` · `help` · `tools.list` · `tools.doc`。

## 环境要求

* 一台装了 **Atmosphère**（开源自制系统）的主机，SD 卡可读写
* 构建需要 devkitPro + libnx；本仓库的示例命令走 docker 镜像 `devkitpro/devkita64:latest`

> 开发与实测环境：Atmosphère 1.11.2 / 固件 22.5.0 / Mariko。其它版本未验证。

## 构建

```bash
# 本机装了 devkitPro：
cd device/platform && make -j4

# 没装 devkitPro（走 docker）：
# ★ 请在仓库根目录下执行，并把「根目录」挂进容器 —— Makefile 要从根目录的 VERSION 取版本号。
docker run --rm -v "$PWD":/work -w /work/device/platform devkitpro/devkita64:latest \
  bash -lc '. /opt/devkitpro/devkita64.sh && make -j4'
```

产出 `nxc.nsp`（ExeFS PFS0 格式）。`device/platform/dist/` 里另放了一份可以直接使用的构建产物。
`nxc-recovery` 是独立的小程序，在 `tools/nxc-recovery/` 里用同样的命令构建。

## 装到卡上

```
sdmc:/atmosphere/contents/4200000000000012/exefs.nsp         模块本体（nxc.nsp 改名而来）
sdmc:/atmosphere/contents/4200000000000012/flags/boot2.flag  启动标记
sdmc:/atmosphere/contents/4200000000000012/toolbox.json      可选，让它出现在 sysmodules 覆盖层里
sdmc:/switch/nxc-recovery.nro                                可选
```

`flags/boot2.flag` 不能省。少了它模块不会被加载，表现是「装好了但一点反应也没有」。
模块的 TID 写在 `device/platform/config.json` 里，改的时候记得同时改卡上的目录名。

## 用起来

### 先确认模块活着

```bash
printf 'hello\n' | nc <设备IP> 47800
```

```
OK 4
platform=nxc
proto=1
boot=145
tools=14
```

`identify.id` 会给出更完整的一份：平台名与版本、开机次数、工具数、依赖服务状态、固件版本。

连不上时先看两件事：主机是不是进入睡眠了（睡眠后监听端口会关闭，按一下电源键唤醒即可），以及
`<设备IP>` 是不是主机当前在局域网里的地址（DHCP 可能换过）。

### 命令行客户端

仓库带了一个 PC 侧客户端，负责转义、超时与 JSON 输出，比手敲协议方便：

```bash
python3 pc/nxc.py hello
python3 pc/nxc.py 'probe.list'
python3 pc/nxc.py 'fs.ls path=/atmosphere/contents'
python3 pc/nxc.py --json 'identify.id'
```

默认连 `$NXC_DEVICE_IP`（未设置时是 `192.168.1.50`），可用 `--ip` / `--port` 覆盖。

### 接 MCP（让 AI 直接用）

```json
{
  "mcpServers": {
    "nxc": {
      "command": "<python3 绝对路径>",
      "args": ["<本仓库路径>/pc/nxc_mcp.py"],
      "env": { "NXC_DEVICE_IP": "<设备IP>", "NXC_DEVICE_PORT": "47800" }
    }
  }
}
```

配好后要在客户端的连接器管理里「信任」它才会启用。

6 个 MCP 工具：`nxc_caps`（发现设备能力）· `nxc_call`（唯一执行入口）· `nxc_deploy`（上传模块并读回校验）·
`nxc_fetch`（把设备上的文件取回本地，例如截图）· `nxc_put`（上传小文件）· `nxc_gdb`（驱动 PC 侧完整 GDB）。

## 协议

一条请求一行：

```
<工具>.<方法> [键=值]...
```

以空格切词，第一个词是方法名，其余是 `键=值`（少数方法也接受没有 `=` 的位置参数）。值里不能出现空格、
制表符与换行，这类字节按 `%XX` 转义，规矩和 URL 一样：空格写 `%20`，字面的 `%` 写 `%25`；多字节 UTF-8
不转义。`pc/nxc.py` 与 MCP 的 `nxc_call` 会自动转义，日常写自然值即可。

响应是 `OK <行数>` 加若干行载荷，载荷行要么是 `键=值`，要么以 `#` 打头表示自由文本（可含空格，日志类内容
用这种）。失败时不带载荷，直接回 `ERR <码> <说明>`：

| 码 | 含义 |
| --- | --- |
| 400 | 请求或参数不合法 |
| 403 | 缺少或写错确认口令 |
| 404 | 工具 / 方法 / 目标不存在 |
| 409 | 前置条件不满足，例如 `clk.set` 读不到原值就拒绝写入 |
| 413 | 请求或单个载荷过大 |
| 500 | 执行失败 |
| 503 | 依赖的系统服务当前不可用，例如网络或 SD 卡未就绪 |

完整规范（转义细节、分页、怎么新增一个工具）在 `device/platform/README.md`。

## 安全边界

| 级别 | 方法 |
| --- | --- |
| 只读 | `identify.*` `tele.*` `title.list/current/name/processes` `screen.info/capture/wait_*` `input.status` `mem.info/read/peek` `net.*` `fs.roots/ls/stat/read` `save.list` `boot.list/status` `power.status` `clk.list/get` `probe.list/results` `log.*` |
| 需确认口令 | `title.launch` `title.terminate` `mem.write` `boot.set` `power.reboot` `power.shutdown` |
| 无口令但会改动设备状态 | `input.press/release/seq/detach` `clk.set/restore` `fs.write/copy/mkdir/rm` `save.backup` `probe.try/group` |
| 默认拒跑 | `probe.try svc=nv` —— `nv` 被标为已知危险，默认回 `result=refused`，加 `force=1` 才会执行 |

三点要知道的边界：

1. `mem.*` 与 PC 侧 GDB（`22225`）互斥，两者抢同一个调试能力，只能二选一。
2. `fs.*` 只能碰 SD 卡，碰不到主机内部储存（NAND / 系统分区）。
3. 模块的 NPDM 声明了全量服务访问（`"service_access": ["*"]`）。上表是协议层的分级，不是内核层的沙箱 ——
   能连到 `47800` 的人就等于拿到了这个模块的全部权限。协议是明文且没有鉴权，不要把它暴露到公网。

## 已知限制

* `title.launch` 启动普通应用这条路未走通；替代做法是用 `input` 在主界面按 A。
* `title.name` 对系统应用取不到名字（系统标题没有元数据，属客观限制）。
* `power.status` 的睡眠键读数在该固件上取不到。
* 主机进入睡眠时连接会断开，醒来后需要重连。
* 主机上若另跑着别的调试或传输组件（GDB 桩、FTP、视频流等），它们不属于本项目；其中 GDB 桩与
  本平台的 `mem.*` 互斥。

## nxc-recovery

`tools/nxc-recovery/` 是一个普通的 NRO 自制程序，不是系统模块，也不依赖 `47800` 或 FTP。它只做一件事：
当平台把自己写坏、或者刷进一份不能用的版本时，把一份已知可用的模块写回模块目录并重启。

1. 平时就把 `nxc-recovery.nro` 放到 `sdmc:/switch/`，并在 `sdmc:/switch/nxc-recovery/` 里留一份已知可用的模块。
2. 出事时从 hbmenu 启动它，上下选镜像、按 A，它会写回、逐字节校验，然后重启。

## 目录

```
device/platform/      设备侧 sysmodule：源码（核心 + 各工具）、构建脚本、详细手册、一份已验证的产物
tools/nxc-recovery/   独立小程序
pc/                   PC 侧脚本与 MCP 服务器
skills/               本项目的开发流程约定（改动留痕、崩溃取证工具链）
```

加工具直接改 `device/platform/source/tools/`：每个工具的参数、风险等级、教程都写在它自己的 `.cpp` 里
（样板见 `tool_fs.cpp` 末尾）。想知道这个平台现在会什么，问设备自己：`tools.list`。

## 许可

本项目以 **GNU General Public License v2.0**（`GPL-2.0-only`）发布，全文见 [`LICENSE`](LICENSE)。

`Copyright (C) 2026 granule256`

你可以在 GPL-2.0 的条款下使用、修改、再分发；分发衍生作品时必须以同样的许可提供源码。

## 免责

本项目是自制软件（homebrew）作品，只在你自己的设备上使用。修改系统模块有导致主机无法开机的风险，
仓库里带了 `nxc-recovery`，但风险自负。本项目不包含任何厂商的专有代码或内容。
