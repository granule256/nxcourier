# NxCourier 平台 · 设备侧（NS）

一个跑在主机上的 **sysmodule**，把能拿到的 NS 系统能力包成统一协议，
给 PC 端 / AI 调用。**核心很小，能力全在工具里** —— 加一个能力 = 加一个工具文件，不改核心。

---

## 一、架构：核心 + 注册工具

```
device/platform/
├── include/nxc_sdk.hpp          ★ 第三方开发者只需要看这一个头文件
├── config.json                  NPDM（权限、内存、syscall 表）
├── Makefile
└── source/
    ├── main.cpp                 入口：最小初始化 + 监听循环
    ├── core/                    ── 核心（不随功能增长）──
    │   ├── platform.hpp         核心内部接口
    │   ├── log.cpp              心跳 / 面包屑 / 内存环形日志
    │   ├── registry.cpp         工具注册表（遍历链接器收集到的段）
    │   └── protocol.cpp         行文本协议 + TCP 收发 + 分发
    └── tools/                   ── 工具（能力都在这里）──
        ├── tool_identify.cpp
        ├── tool_fs.cpp
        ├── tool_log.cpp
        └── tool_probe.cpp
```

**核心只干四件事**：收连接、解析一行命令、找到对应工具、把结果发回去。
它**不知道** fs 是什么、tele 是什么 —— 所以加能力永远不需要动核心。

### 注册是怎么做到的

工具用 `NXC_DEFINE_TOOL` 把描述符放进一个叫 `nxc_tools` 的链接器段：

```cpp
NXC_DEFINE_TOOL(nxc_tool_fs, "fs", "SD 卡文件（白名单内）", kMethods);
```

核心在启动时数一下这个段有多大，就知道有几个工具（`registry.cpp` 里用
`__start_nxc_tools` / `__stop_nxc_tools` 两个链接器符号取边界）。

⇒ **加一个工具不需要在任何清单里加一行**，链接器自己会收集。

---

## 二、协议（完整规范）

行文本，一行一条。**设备侧不需要 JSON 库**；PC 侧网关负责翻译成 JSON / MCP 工具。

### 请求

```
<工具>.<方法> [k=v]... 
```

* 以空格切词；第一个词是方法名；其余是 `k=v`。
* 没有 `=` 的词按位置记为 `_0`、`_1`…（少数方法支持）。
* 一行以 `\n` 结束；`\r` 会被忽略。

### 响应

```
OK <n>\n            ← 后面跟 n 行载荷
<载荷行 1>\n
...
```

出错时：

```
ERR <码> <说明>\n    ← 不带载荷
```

**载荷行有两种**：

| 形式 | 含义 |
| --- | --- |
| `k=v k2=v2` | 结构化的键值对 |
| `#任意文本` | 自由文本（可含空格），日志这类内容用这个 |

**约定**：`k=v` 里的**值不能含空格、制表符与换行** —— 字段之间就是用它们分列的。
这类字节按 `%XX` 转义（**和 URL 一个规矩**）：

| 想表达 | 写 | 例 |
| --- | --- | --- |
| 空格 | `%20` | `path=/a b/c` ⇒ `path=/a%20b/c` |
| 制表符 / 换行 | `%09` / `%0A` | |
| **字面的 `%`** | `%25` | `path=/50%` ⇒ `path=/50%25` |
| 其它控制字符 | `%XX`（该字节的十六进制） | |

★ **入方向会对称还原**（`codec::unescapeValueInPlace`，在 `protocol.cpp` 的 `parseRequest` 里）——
所以 `fs.ls` 回包里的名字（`visible%20tinted%20rocks_3002235272`）**原样拼回就能用**：
带空格的路径从此是可寻址的（2026-10-05 修，背景见
`docs/反馈-参数值含空格无法表达-20261005.md`）。
★ **解码在所有工具看到路径之前完成** ⇒ 白名单 / `..` 检查拿到的是最终字节，不会被 `%2E%2E` 绕过。
★ 多字节 UTF-8（中文等）**不转义**，整串仍是合法 UTF-8。
★ **PC 侧两个客户端会自动转义**（`pc/nxc.py`、MCP 的 `nxc_call`）——
它们只转义"空格这类不可见字符"，所以日常写自然值就行：命令行里含空格的值请用引号，
即 `python3 pc/nxc.py 'fs.ls path="/a b/c"'`；`%` 由调用方负责（字面的 `%` 写 `%25`）。
★ 用 `nc` 手打协议行时**没有客户端帮忙**，得自己写 `%20`。
★ 大块二进制仍走 base64（字母表里没有空格与 `%`，不受影响）。

### 核心内建的五条（不属于任何工具，永远存在）

| 命令 | 作用 |
| --- | --- |
| `hello` | 握手：平台名、协议版本、开机次数、工具数 |
| `caps` | 列出全部工具及其方法数（老接口，保留） |
| `help tool=<名字>` | 列出某个工具的全部方法（老接口，保留） |
| ★★ **`tools.list`** | **能力发现**：一条命令拿到**全部工具 + 每个方法的使用摘要**（参数表、风险等级）。可选 `tool=<名>` 只看一个工具 |
| ★★ **`tools.doc name=<工具[.方法]>`** | 取某个工具/方法的**完整教程**（作用/参数/示例/注意/返回/相关）；支持 `offset=`/`lines=` 分页 |

★ 为什么要有后两条：**AI 只能通过协议说话** —— 它读不到仓库里的 README。
所以"这个平台能干什么、某个方法怎么用"必须**从设备里问得出来**，
而不是靠人告诉它、也不是把工具清单**写死在 PC 侧**（PC 侧只负责转发，设备加工具它一个字都不用改）。

**★ 写工具的人请注意（自描述约定）**：每个工具的**全部知识都写在它自己那个 .cpp 里** ——
`nxc::Param[]`（参数表）+ `nxc::MethodInfo`（`risk` + 教程正文）+ 注册时用
`NXC_DEFINE_TOOL_EX(..., 工具级教程)`。核心**不认识**任何工具，`tools.list`/`tools.doc` 只是汇总。
详见 `source/tools/tool_fs.cpp` 末尾的样板。
★ 老写法 `NXC_DEFINE_TOOL(..., kMethods)` 与"方法表里只写三项"**仍然有效**
（新字段都带默认值）⇒ 可以逐个方法慢慢补，不会一次性逼你改完 54 处。

### 手工试一下（不需要写客户端）

```bash
printf 'hello\ncaps\nidentify.id\n' | nc <设备IP> 47800
```

---

## 三、三分钟写一个工具

新建 `source/tools/tool_echo.cpp`，**不改任何其他文件**：

```cpp
#include "nxc_sdk.hpp"

namespace {
void cmdEcho(const nxc::Args& a, nxc::Reply& r) {
    const char* text = a.get("text");
    if (text == nullptr) { r.fail(400, "missing-text"); return; }
    r.kv("echo", text);
    nxc::host::log("echo 被调用 text=%s", text);   // 写进环形日志，PC 可 log.dump 取走
}

const nxc::Method kMethods[] = {
    {"echo", "回显 text= 参数", cmdEcho},
};
}  // namespace

NXC_DEFINE_TOOL(nxc_tool_echo, "echo", "示例工具", kMethods);
```

重新 `make`，`caps` 里就会多出 `echo`。

**平台提供给工具的服务**（`nxc::host`，只有这些，不暴露核心细节）：

| 接口 | 作用 |
| --- | --- |
| `host::log(fmt, ...)` | 追加到内存环形日志（PC 用 `log.dump` 取走） |
| `host::breadcrumb(fmt, ...)` | 覆盖写"当前进度"到 SD —— ★ **崩了靠它定位** |
| `host::bootCount()` | 本次开机是第几次 |
| `host::toolCount()` | 已注册工具数 |
| `host::heartbeatPath()` | 心跳文件路径 |
| `host::dumpLog(reply, n)` | 把环形日志写进应答 |

---

## 四、内置工具

| 工具 | 方法 | 说明 |
| --- | --- | --- |
| `identify` | `id` | 平台/固件版本、开机次数、工具数 |
| `log` | `dump` `info` | 取走内存环形日志 |
| `fs` | `roots` `ls` `stat` `read` `write` `copy` `mkdir` `rm` | SD 卡文件，**白名单内**（含平台的自我更新） |
| `probe` | `list` `try` `group` `results` | ★ 能力探针：逐个试系统服务，产出能力矩阵 |
| `tele` | `freq` `temp` `battery` `perf` `all` | 主机遥测：频率 / 温度 / 电池 / 性能模式 |
| `clk` | `list` `get` `set` `restore` | CPU/GPU/MEM 频率，**带原值回滚** |
| `power` | `status` `reboot` `shutdown` | 电源。重启/关机**必须带确认口令**，可选 `via=bpc\|spsm` |
| `title` | `list` `current` `name` | 主机上「有什么、在跑什么」（只读） |
| `mem` | `info` `read` `peek` `write` | ★ **读写运行中游戏的内存（不冻结游戏）**。与 GDB 互斥，见下 |

### ★★ `mem` 与 PC 侧 GDB 互斥 —— AI 必须二选一

这两条路**抢同一个调试能力**，同时用会互相抢句柄，实测崩机（fatal `01000000000d609`）。

| | `mem`（设备侧） | GDB（PC 侧） |
| --- | --- | --- |
| 实现 | 内核 SVC：`svcDebugActiveProcess` → `svcRead/WriteDebugProcessMemory` → `svcCloseHandle` | 容器里的 `aarch64-none-elf-gdb` 连 `22225` |
| 工作方式 | **每次操作 attach → 读/写 → 立刻 detach**，游戏全程照常运行 | **停下来看**：连接和断点都会让目标进程停住 |
| 能做什么 | 读字节、按类型读一个数、写内存（带确认口令） | 断点、单步、寄存器、调用栈、反汇编、watchpoint、符号 |
| 做不到什么 | 断点 / 单步 / 调用栈 / 符号 | — |
| 适合 | **常驻服务持续读**（AI 一边陪你玩一边看状态） | **人坐下来定位问题** |

★ `mem` 的实现是**照抄 sys-botbase** 的成熟做法（它是常年可用的活内存机器人），
不是自己猜的 IPC 命令号 —— 也**用不着** `dmnt:cht`（libnx 根本没封装那个）。

★ 冲突提示写在了两处：设备侧 `mem` 工具的描述（`nxc_caps` 能看到）与 MCP 工具 `nxc_gdb` 的描述。

### ★★ 冲突到底在哪一层（2026-10-05 实测，比原说法更精确）

实测：GDB 挂在目标上的同时调 `mem.*`：

| 操作 | 走什么 | GDB 未挂 | **GDB 已挂** |
| --- | --- | --- | --- |
| `mem.info` | `ldr:dmnt` + `svcGetInfo` | ✅ | ✅ **仍然可用** |
| `mem.read` | `svcReadDebugProcessMemory` | ✅ | ❌ **`read-failed`** |
| 会不会崩机 | — | — | ❌ **不会崩，干净返回错误** |

⇒ 两点修正：
1. **互斥不在 attach 层**（`svcDebugActiveProcess` 两边都能拿到句柄），
   **而在真正的内存访问层**。
2. 开工单流传的「dmnt 与 GDB 互斥会崩机（fatal `01000000000d609`）」
   **在我们的实现下不成立** —— 表现为读失败，不是崩机。

### ★ `nxc_gdb` 的正确用法：必须显式按 pid 附加

```
action=open connect=true                       → 连上桩，但 info threads 是空的（No threads）
action=run ["attach 141"]                      → ★ 必须显式按 pid 附加，pid 从 mem.info / title.current 拿
action=run ["info threads", "info registers"]  → 这时才有完整线程列表和寄存器
action=detach                                  → 放开目标，让它继续跑
```

★ 光用 `target remote` 是**不够的**：桩会接受连接，但不会选中任何进程
（试过 `target extended-remote` 也一样）。必须先 `attach <pid>`。

★ 实测附加成功后看到的线程名（可以据此确认挂到的是不是真游戏）：
```
* 1  Thread 141.673 "Thread_0x0000001FFFC6F83C"
  2  Thread 141.656 "MainThread"
  3  Thread 141.667 "NxSystemThread"
  5  Thread 141.672 "LibcurlGc"
```

★ ★ **关闭会话必须用 `detach`，绝不能用 `kill`** —— `kill` 在远程目标上会**真的把游戏关掉**。


### `mem` 的用法

```
mem.info                                   → pid / program_id / 模块基址 / 堆基址（先拿 main_base）
mem.read  addr=0x... [size=（默认256，上限3072）]  → 读字节，返回 base64
mem.peek  addr=0x... [type=u8|u16|u32|u64|s8|s16|s32|s64|f32|f64] → 按类型读一个数
mem.write addr=0x... data=<base64> confirm=I-KNOW-THIS-WRITES-MEMORY
```

★ `mem.write` 直接改动运行中游戏的内存，**可能让游戏崩溃或损坏存档**，所以带确认口令。

**2026-10-05 真机验收（走 MCP 工具真实调用，游戏运行中）**：

```
mem.info   → pid=141  program_id=0x010021C000B6A000  attach=ok
             4 个模块：0x…404000(0x4000) / 0x…404000+…(0x112000) / 0x…516000(0x9D1000) / 0x…EE7000(0x69A000)
             main_base=0x0000006CA4404000（索引 1，与 sys-botbase 的规则一致）
             heap_base=0x0000001042600000
mem.read addr=0x…404000 size=64   → 64 字节 base64
             解码后偏移 0x08 是 `4D 4F 44 30` = 「MOD0」——该主机的模块头魔数
             偏移 0x30 起是 `ff 43 02 d1 / fd 7b 07 a9 / fd c3 01 91 / f4 4f 08 a9`
             = `sub sp,sp,#0x90` / `stp x29,x30,[sp,#0x70]` / `add x29,sp,#0x70` / `stp x20,x19,[sp,#0x80]`
             —— 一段标准 ARM64 函数序言
mem.peek addr=0x…404008 type=u32 → 0x30444F4D（即 "MOD0" 小端）
title.current（读完之后再查一次） → 应用仍在跑（pid 未变）⇒ **游戏没有被冻住**
```

⇒ **读到的确实是加载进内存的真实模块内容**（魔数 + 可辨认的机器码），不是空壳或缓存。

★ **一个待确认的疑点（不要当成已结论）**：本次运行的应用 `program_id` 是 `0x010021C000B6A000`，
用 `title.name` 查这个 id 得到的是「The Binding of Isaac: Afterbirth+」。**但**同一个 id 也出现在设备的
`/atmosphere/contents/` 覆盖目录里，且早前 FTP 的写入前检查回报该 Application 的模块是
`hbl.elf + ftpd.elf` —— 这是 CFW 上常见的「**拿一个真游戏的 title id 当壳来跑 hbmenu**」手法。
所以「读到的到底是以撒本体还是 hbmenu」**需要对着屏幕确认**，不能只看 title id 就下结论。

### `title`：主机上有什么、在跑什么

| 方法 | 说明 | 依赖的服务（均已实测可用） |
| --- | --- | --- |
| `title.list [count=] [offset=]` | 已安装应用列表（`application_id` + 最后更新时间） | `ns` |
| `title.current` | 当前在跑什么（应用进程 id → `program_id`；没在跑返回 `running=none`） | `pm:dmnt` |
| `title.name app=<id>` | 读 NACP 拿应用名 / 作者 / 版本号 | `ns` |
| `title.processes` | 列出正在跑的进程（pid + program_id）—— 决定"关哪个"之前先看它 | `pm:dmnt` |
| ★ `title.launch app=<id> [storage=] confirm=I-KNOW-THIS-LAUNCHES` | **启动应用**（`storage` 默认 `any`，让系统自己找） | `pm:shell` 的 `pmshellLaunchProgram` |
| ★ `title.terminate app=<id>\|pid=<n> confirm=I-KNOW-THIS-CLOSES` | **结束应用** | `pm:shell` 的 `pmshellTerminateProcess` |

★ 启动/结束都要**确认口令**，而且 `title.terminate` 会**丢失未保存进度**（help 里写明了）。
`title.launch` 的 `storage` 取 `any`（默认）/ `sd` / `user` / `system` / `gc`——
默认走 `any` 是有意的：CFW 上应用可能装在 SD 也可能在 NAND，写死一个反而会失败。

**2026-10-05 真机验收（走 MCP 工具真实调用）**：

```
title.current                         → running=application pid=141 program_id=0x010021C000B6A000
title.list count=14                   → 14 个已安装应用
title.name app=0x01007EF00011E000     → <应用名> / <发行商> / 1.6.0
title.name app=0x010021C000B6A000     → <应用名> / <发行商> / 1.7.9b
```

★ 最后一条正是**当时正在运行的应用** —— 也就是说 AI 能直接报出「你当前跑的是哪个应用」。

★ `title.name` 需要一块 ~144KB 的缓冲（NACP 0x4000 + 内嵌图标 0x20000），
**按需 `malloc`、用完即还**，不放进 BSS —— sysmodule 的静态内存要省着用。

### 计划中的内置工具（按用户定调的"添头"）

| 工具 | 做法 | 状态 |
| --- | --- | --- |
| `title.launch` / `title.terminate` | `pmshellLaunchProgram` / `pmshellTerminateProcess` | 🔜 会改变主机状态，要单独设计确认流程与验证时机 |
| `gdb` | **设备侧当 GDB 客户端**，连 `127.0.0.1:22225`，把断点/读内存转成平台方法 | 🔜 好处：PC 只连平台一个端口。代价：要自己实现 GDB 的 RSP 协议；且与 `dmnt` 互斥 |
| `net.ftp` | 让平台自己变成 FTP 服务端，老工具链可直连 | 🔜 只有当"外部工具必须用 FTP"时才值得做；`fs.*` 已覆盖大部分需求 |
| 抓画面 | 走 SysDVR（它本身就是 sysmodule） | 🔜 属添头；注意 `caps:u`/`caps:su` 实测打不开，所以不能走截图服务 |

### 遥测读数从哪来（`tele`）

| 读数 | 来源 | 备注 |
| --- | --- | --- |
| `cpu_hz` / `gpu_hz` / `mem_hz` | `clkrst` | 这是**系统请求值**，不是硬件瞬时值 |
| `skin_mc` | `tc` 服务 | 机身温度（毫摄氏度） |
| `soc_c` / `pcb_c` | I2C 直读 `tmp451` 的 reg0 / reg1 | ★ 与 sys-clk 同一颗传感器；**reg 与 SOC/PCB 的对应关系需在真机上与 sys-clk 覆盖层对照一次再定案** |
| `battery_percent` 等 | `psm` | 电量 / 充电器类型 / 供电是否充足 / 电压状态 |
| `perf_mode` / `perf_config` | `apm` | normal / boost |

### ★ 两个「写」操作的保护

| 操作 | 保护 |
| --- | --- |
| `fs.write` | 路径白名单（`/config/nxc`、`/switch`、**平台自己的模块目录**），路径含 `..` 拒绝 |
| `clk.set` | **先读原值再写**；读不到原值就**拒绝设置**（`409`）；可用 `clk.restore` 还原。★ 原值只存在内存里，模块重启即丢 |
| `power.reboot` | 必须带 `confirm=I-KNOW-THIS-REBOOTS` |
| `power.shutdown` | 必须带 `confirm=I-KNOW-THIS-SHUTS-DOWN` |
| `title.launch` | 必须带 `confirm=I-KNOW-THIS-LAUNCHES` |
| `title.terminate` | 必须带 `confirm=I-KNOW-THIS-CLOSES`（会丢未保存进度） |
| `mem.write` | 必须带 `confirm=I-KNOW-THIS-WRITES-MEMORY`（可能损坏存档） |

### ★ `fs` 白名单的第三个根：平台的自我更新

白名单第 3 个根是 **`/atmosphere/contents/4200000000000012`** —— 也就是平台自己的模块目录。
加它的理由很实际：**此前每次更新模块都要"退游戏 → 开 hbmenu → 开 ftpd → 上传 → 重启"，
一环都不能少；有了它，平台可以自己覆盖自己的 `exefs.nsp`，之后只要重启就行。**

代价要说清楚：**写坏了自己，下次开机模块加载失败 —— 拔卡删掉目录即可复原。**

配套动作：
* 写进这个根的会被记成 **`fs.write SELF-UPDATE …`** 面包屑（比普通写入更醒目），
  便于"写坏了回溯到底写了什么、多大"。
* 新增 **`fs.copy src= dst=`** —— 自我更新前先把旧的 `exefs.nsp` 备份成 `exefs.nsp.bak`。
* 这个根**没有**确认口令：它保护的是平台自己（可恢复），不是用户数据，加口令只会给
  分块上传（130KB 要拆很多次）添无意义的摩擦。

★ **重启/关机是调研时的意外收获**：`bpc` 服务自带 `bpcRebootSystem()` / `bpcShutdownSystem()`，
而 `bpc` 在 M1 探针里初始化成功 ⇒ **不必等 `spsm`**。（开工单曾断言"真正的 reboot 要 `spsmShutdown`，
在 sysmodule 里做不到"。）

★ `power.*` 另有一条 `spsm` 通道（`via=spsm` → `spsmShutdown(bool reboot)`）。
它同时是 **spsm 的客户端级验证** —— 开工单说它"能开但客户端一连就 reset"，只测 init 不算数。

---

## 五、崩溃定位：为什么这么设计

**sysmodule 没有 stdout，崩机在用户看来就是黑屏。** 所以有两套互补的日志：

| | 存放 | 崩溃后 | 用途 |
| --- | --- | --- | --- |
| **面包屑** | SD 上的 `boot_progress.txt`，**覆盖写** | ✅ 仍在 | 记录"**现在**走到哪一步"——最后一行就是凶手 |
| **环形日志** | 内存里，128 条 | ❌ 丢 | 记录"**刚才**发生了什么"——PC 连上就能取走 |
| **探针结果** | SD 上的 `probe_results.txt`，**追加写** | ✅ 累积保留 | 每个服务的通/崩结论，跨重启累积 |

★ 写工具时的铁律：**任何有副作用的动作，都要在动作之前写 `breadcrumb`。**
顺序反了就失去定位能力。

---

## 六、安全边界

* ★★ **`fs.*` 现在覆盖整张 SD 卡**（2026-10-06 按用户要求**删掉了路径白名单**）：
  任意**绝对路径**直接读写、**不需要任何口令**，`..` 也放行（SD 根之上没有东西可穿）。
  唯一保留的规则是"**路径必须以 `/` 开头**" —— 那是**正确性**要求（设备侧的当前工作目录不可预期），
  不是安全限制。
* ★ **仍然碰不到主机内部储存（NAND / 系统分区）**：`fs.*` 只挂了 SD（`fsdevMountSdmc`），
  **从未挂 `bis:`**。⇒ 想动内部储存，本平台做不到。
* ★ **写入留痕仍在**：两类写入会单独写一条更醒目的面包屑 —— 写自己的模块目录（= **自我更新**）
  与写 `/bootloader`（= **影响下次开机落点**），方便事后回溯"是谁改的"。
* **动作类方法要确认口令**（6 个）：`mem.write` · `title.launch` · `title.terminate` ·
  `boot.set` · `power.reboot` · `power.shutdown`，各自要一个显式的 `confirm=` 串。
  ★ `clk.set` 不走口令，它的保护是"**先记原值，读不到原值就拒设**"。
  `fs.write` / `fs.rm` / `save.backup` 这类"只动 SD"的不要口令，但会留痕。
* ★ **`flags/boot2.flag` 必须加**（早期"不加"的说法已被真机实测推翻，理由见第七节）：
  不加的结果**不是**"换个阶段加载"，而是**模块根本不会被加载**。
* 启动路径上只写一行面包屑，不做重 IO。
* 只开一个端口（`47800`）。
* ★ **`nv`（GPU 驱动）是禁区**：实测一调就把模块搞死。已在 `tool_probe.cpp` 标 `KNOWN-FATAL`。

---

## 六·五、PC 侧接入：一个 MCP 入口，不是「每个工具一个 MCP 工具」

`pc/nxc_mcp.py` 是 PC 侧的 MCP 服务器（stdio）。它的设计原则是
**和设备侧同构：核心 + 动态发现**，**绝不为每个设备工具硬写一个 MCP 工具**。

```
nxc_caps   ← 动态发现：连上设备问「你现在有哪些工具、每个工具有什么方法」
nxc_call   ← 唯一执行入口：method="<工具>.<方法>"，params={...}
nxc_gdb    ← PC 侧完整 GDB（断点/单步/调用栈/反汇编）。★ 与 nxc_call 的 mem.* 互斥
```

**前两个是设备侧平台的入口，永远不变。** 设备上加工具 ⇒ 它们一个字都不用改，
AI 先 `nxc_caps` 拿清单再 `nxc_call` 调用即可。

**第三个（`nxc_gdb`）是个例外，而且是刻意的**：GDB 不在设备侧 —— 它跑在 PC 侧的
devkitPro 容器里（`aarch64-none-elf-gdb`，连 22225）。理由见
本机开发记录 §十九：
设备侧重造只会得到一个更弱的子集，还会引入平台里第一个"有状态"的东西。

★ **`mem.*` 与 `nxc_gdb` 二选一**（抢同一个调试能力，同时用崩机）：

| 想做的事 | 用哪个 |
| --- | --- |
| 不冻结地持续读游戏内存（常驻观察） | `nxc_call` 的 `mem.*` |
| 停下来调试（断点 / 调用栈 / 单步） | `nxc_gdb` |

`nxc_gdb` 的用法：`action=open` 开会话（`connect=true` 会连上并停住目标）→
`action=run` 发 gdb 命令 → `action=close` 关会话 → `action=status` 看状态。


为什么不在 MCP 侧给每个设备工具建一个包装：那会造成**双向耦合**（设备改一次、PC 改一次），
两边清单还会各自漂移。当前设计下设备侧的 **14 个工具 / 54 个方法**全部通过这 2 个入口可达
（★ 2026-10-06 更正：这里原来写"7 个工具 / 27 个方法"，是早期数字；**准确值以设备自己回的
`caps` 为准**，别手抄）。

★ **长连接**：MCP 服务器与设备之间维持**一条长连接**，而不是每次调用新建。
这台主机的 TCP 栈对「频繁建连/断连」敏感（FTP 那边有同样的毛病）——
实测每次新建连接时 `clk.get` 会偶发 `ConnectionResetError`，改成长连接后消失。

★ **重试策略（安全优先）**：只在**一条响应都还没收到**时才重连重试一次；
一旦读到过部分响应就直接抛出，**绝不重放**，避免重复执行写操作。

### 让 AI 真的能调（任何支持 MCP 的客户端）

配置写在**客户端自己的** MCP 配置里（以本仓库克隆到哪儿为准）：

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

写完**不会自动生效**：要在客户端的连接器管理里「信任」它才会启用
（例：WorkBuddy 是「连接器管理」右上角的「自定义连接器」入口；Claude Desktop 等各客户端位置不同）。

### 手工冒烟测试（不需要任何客户端）

```bash
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"nxc_call","arguments":{"method":"tele.all"}}}' \
 | python3 pc/nxc_mcp.py
```

---

## 七、构建与部署

### ★ 参考实现：`olliz0r/sys-botbase`（动手前先读它）

它是一个**久经使用**的「sysmodule + TCP 命令服务器」（自动化用，端口 6000，TID `430000000000000B`），
和本项目几乎同类。它的 `sys-botbase/source/main.c` 里有几条做法我们直接抄了：

| 做法 | 我们是否采用 | 说明 |
| --- | --- | --- |
| ★ **`__appInit` 里不调 `smExit()`**（放到 `__appExit`） | ✅ 已改 | **运行时还要开服务**（工具层的 `xxxInitialize()` 要经 `sm:` 拿端口）；启动时关掉 sm，后面就开不出服务 |
| ★ `__appInit` 开头 `svcSleepThread(20s)` 等系统就绪 | ⚠️ 换成"在 main 里有上限地重试" | 目的相同（boot2 阶段 `bsd:u` 未就绪），我们的方式不阻塞启动 |
| 堆 4.5 MB（`HEAP_SIZE 0x00480000`） | ❌ 保留 512 KB | 官方模板是 512 KB；我们的 `Args`/`Reply` 都是静态，堆只给 libnx 内部用 |
| ★ `dup2(clientFd, STDOUT_FILENO)` 把 stdout 接到客户端 | 🔜 待做 | **解决"sysmodule 没有 stdout"**：之后工具的 `printf` 会直接发到客户端 |
| `poll()` 多客户端 accept 循环 | 🔜 待做（首期是单客户端阻塞式） | 多个 PC 程序同时连时才需要 |
| 用 `threadCreate/threadStart` | 首期单线程 | 我们的设计不需要多线程 |
| `toolbox.json` 放模块目录 | ✅ 已加 | 模块会出现在 **sysmodules 工具箱/覆盖层**里，可开关，不必手改文件 |

★ 另一条关键旁证：**sys-botbase 在 `__appInit` 里对初始化失败直接 `fatalThrow`，却被大量玩家日常使用** ——
说明"boot2 阶段初始化失败"在实践中**不至于搞砖**，风险主要来自自己代码的逻辑。
（我们仍选择"失败就优雅退出"，因为这样更好查。）

### 部署前核查（2026-10-05 已在真机上读过，可复核）

| 项 | 实测结果 |
| --- | --- |
| 目标 TID 是否被占 | ✅ `4200000000000012` **没被占用**（已占的 42 开头是 `…E51A/E51B/0000/000B/000E/0010`） |
| **加载布局** | ✅ **`<TID>/exefs.nsp`（一个文件）** —— 设备上的 `sys-clk`（`00FF0000636C6BFF`）就是这么放的 |
| 我方产物内部结构 | ✅ `PFS0` 里两个条目：`main.npdm`(968B) + `main`(105693B)，与 sys-clk 一致 |
| `/config` 是否可写 | ✅ 该目录存在且有大量 `<工具名>/` 子目录，`/config/nxc` 符合既有约定 |
| ★ `flags/boot2.flag` | **必须加，否则模块根本不会被加载**（见下方「★ 必须加 boot2.flag」）。sys-clk 就带它 |

### ★ 必须加 `flags/boot2.flag`（2026-10-05 真机实测纠正）

第一次部署时：`exefs.nsp` 已就位、NPDM 的 `program_id` 也正确，但**心跳文件没出现、端口 47800 连不上、
`/atmosphere/fatal_errors/` 也不存在** ⇒ 结论是"**压根没被加载**"。

原因（源码级证据）：Atmosphère 的 boot2 遍历 SD 卡上的程序时会检查
`cfg::HasContentSpecificFlag(program_id, "boot2")`，**只有带这个标志的程序才被纳入启动流程**。
标志文件路径是 `atmosphere/contents/%016llx/flags/%s.flag`。
卡上每个现成 sysmodule（sys-clk / sys-patch / tas-script）**都带它**。

⇒ 流传的「不加 `boot2.flag`，模块会在正常阶段加载」是**错的** —— 不加的结果是**装了但永远不启动**。

**代价与缓解**：加了之后模块在 **boot2 阶段**启动，此时**不能 abort、不能阻塞**，否则可能开不了机。所以代码里做了三件事：

1. `__appInit` **只用已知可用的服务**（sm / setsys / fs / fsdevMountSdmc），任何一步失败**只记录状态位**，
   **绝不 `diagAbortWithResult`、绝不"钉住不返回"**；
2. 失败时让 `main` **正常 `return`**（进程退出）—— 这是最安全的失败模式；
3. `socketInitialize` **不放在 `__appInit`**：boot2 阶段 `bsd:u` 未必就绪，改在 `main` 里**带重试**做
   （最多 60 次 × 1s，起不来就退出）。


### 部署步骤

```bash
# 构建（本机没有 devkitPro，走 docker）
docker run --rm -v "$PWD":/work -w /work devkitpro/devkita64:latest \
  bash -lc '. /opt/devkitpro/devkita64.sh && make -j4'
```

因为工程里有 `config.json`，产出的是 **ExeFS PFS0（`nxc.nsp`）** 而不是 `.nro`。

**★ 部署顺序（2026-10-06 修订：先核对、再留底、后上传）**

```bash
# 0) 先确认主机醒着 —— 它静置约 148 秒会自己睡着，睡着后 47800 完全够不着
nc -z <设备IP> 47800

# 1) ★ 先核对卡上现在是什么：把卡上那份取回来算 sha256，与记录比。
#    这一步同时证明"仓库里的回退副本 = 卡上实物"（以前没做，吃过亏）。
#    走 MCP：nxc_fetch remote_path=/atmosphere/contents/4200000000000012/exefs.nsp

# 2) 卡上留底（把旧的复制成一份带时间与旧哈希的备份）
#    nxc_call fs.copy src=/atmosphere/contents/4200000000000012/exefs.nsp \
#                     dst=/config/nxc/_prev-exefs-<时间>-<旧哈希前8位>.nsp

# 3) 上传：走 MCP 的 nxc_deploy（47800 的 fs.write 分块写，自带读回 sha256 校验）
#    ★ 不需要 FTP、不需要拔卡；它必须回 verified=true 才算成功。
#    备选（47800 不通时）：FTP 5000，账号 <用户>/<密码>，用 curl 传
#    ★ 老文档里的 tools/device_ftp.py 并不存在。

# 4) 重启：nxc_call power.reboot confirm=I-KNOW-THIS-REBOOTS
#    ★ 请求超时是正常的 —— 机器在回包之前就重启了。
#    ★★ hekate_ipl.ini 现在是 autoboot=0 ⇒ 重启会停在 Hekate 菜单，需要人工选一项。
#       （autoboot 不只影响冷启动：本机的 /atmosphere/reboot_payload.bin 就是 Hekate 本体。）

# 5) ★★ 核对真的生效了：用【开机号】判断 —— **不要用 `nc -z` 判断"起来了没"**
#    ★ 踩过的坑：监听口在 main() 里约 13.4 秒就建好，但**服务循环还没转起来**时，
#      TCP 照样能被 accept（backlog 收下三次握手），而 identify.id **不回话**
#      ⇒ 只看"端口通"会误判成"已经起来了"，紧接着读开机号失败，报成"检测不到"。
#      （另一个坑：人工在 Hekate 菜单里选一项可能要 30~50 秒，短窗口经常等不到。）
#    推荐直接用项目自带的等待工具：按开机号判断、把三种状态分开报、
#    超时后还会去 ARP 表里找设备（防 DHCP 换 IP）：
python3 pc/nxc_wait_boot.py            # 先读当前开机号，然后等它 +1
python3 pc/nxc_wait_boot.py --once     # 只想看一眼现在处于哪一态
#    手工版（能用，但要知道上面那个坑）：
printf 'identify.id\n' | nc <设备IP> 47800     # boot 应该 +1

# 5b) 核对【卡上那份】到底是哪一版（部署前核对、部署后复查都用它）：
python3 pc/nxc_remote_sha.py /atmosphere/contents/4200000000000012/exefs.nsp \
    --compare-local dist/exefs.nsp     # 卡上 / 本地 / 期望值 三方比对
```

**6) 部署后看这两个文件**（证明模块真的被加载了）：

```
/config/nxc/heartbeat.txt       ← 心跳：证明"加载成功"
/config/nxc/boot_progress.txt   ← 面包屑：证明"走到哪一步"
```

**7) 验证服务端。**

```bash
printf 'hello\nidentify.id\n' | nc <设备IP> 47800
```

### 出问题时怎么查（按这个顺序排除）

| 现象 | 结论 |
| --- | --- |
| 心跳文件没出现 **且** 端口不通 **且** `/atmosphere/fatal_errors/` 不存在 | **没被加载** ⇒ 先查 `flags/boot2.flag` 在不在，再查目录名与 NPDM 的 `program_id` 是否一致 |
| 心跳文件出现、但端口不通 | 加载了但卡在 `main` ⇒ 看**面包屑文件的最后一行** |
| 出现 `/atmosphere/fatal_errors/*.bin` | 崩了。boot2 阶段的崩溃可能表现为**开机致命错误** |
4. 拔掉模块（删掉 `/atmosphere/contents/4200000000000012/`）即可完全回到原状 —— 我们**没有**放任何 `flags/`。


### 2026-10-05 实测记录（本机 docker，全部可复核）

| 项 | 结果 |
| --- | --- |
| 产物 | `nxc.npdm` 968B / `nxc.nso` 105693B / `nxc.nsp` 106757B / `nxc.elf` 5.6MB |
| `nxc.nsp` 的 magic | **`PFS0`** ⇒ 确实是 ExeFS 容器，改名 `exefs.nsp` 即可部署 |
| NPDM 里的 Title ID | ★ **`0x4200000000000012` 正确写入**（在文件偏移 656 处搜到其小端字节），**不含**模板默认值 |
| `nxc_tools` 段 | 存在，大小 `0x80` = 128 字节；`__start_nxc_tools`(0x2be90) → `__stop_nxc_tools`(0x2bf10) |
| 工具数 | 128 ÷ 32（`nxc::Tool` 大小）= **4 个** ✓ 与注册的 identify / fs / log / probe 一致 |

★ **关于那 11 条 `field not present` 警告**：用官方模板编出来的 NPDM 会打印

```
Failed to get system_resource_size (field not present).
... （共 11 条）
Failed to get program_id (field not present).
```

**这些是 npdmtool 找可选/旧名字段的正常噪音，不代表你的字段名写错。**
判据不是"有没有警告"，而是**直接读 NPDM 二进制，搜你的 Title ID 小端字节**：

```bash
python3 -c "import struct,pathlib;b=pathlib.Path('nxc.npdm').read_bytes();\
print(b.find(struct.pack('<Q',0x4200000000000012)))"   # ≥0 即写进去了
```

（开工单 §4.2 曾把这段警告当成"title_id 写错的证据"并推断必须改用 `program_id` —— **该推断是错的**。）

---

## 八、下一步

1. **M0**：把上面这版装上，确认 `heartbeat.txt` 出现、`hello` 能应答。
2. **M1**：跑 `probe.group n=1` → `2` → `3` → `4`，产出能力矩阵。
3. 按矩阵补 `clk` / `tele` 工具。
4. 再谈 `gdb` / `net.ftp` 这两个添头。
