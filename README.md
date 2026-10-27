# NxCourier

在装了 **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**（开源自制系统）的主机上常驻一个自写的小程序，
把主机的能力做成一条文本协议挂在局域网里。PC 脚本和 AI 客户端连上去就能用：读状态、读写 SD 卡、
模拟按键、抓画面、重启关机、调频率、备份存档、读运行中的进程内存。

当前设备源码提供 14 个工具 / 59 个方法（已安装版本以 `nxc caps` 为准），监听 TCP `47800`。PC 侧使用一个 `nxc` 入口和一个本机后台，
CLI、MCP（8 个工具）和 Web 共用设备连接、FTP 客户端及 GDB 会话；无需额外桌面壳，也不用拔卡。

当前版本 **v1.0.3**，用户安装包见 [GitHub Release](https://github.com/granule256/nxcourier/releases/tag/v1.0.3)，版本号定义在仓库根目录的 [`VERSION`](VERSION)。PC包内含单个 `nxc`，NS包仅含模块和启动标记；新版NS模块已通过编译与宿主测试，尚未完成真机验收。

## 设计要点

**能力自描述。** 设备自己知道会什么：`tools.list` 一条命令返回全部工具、方法、参数与风险等级，
`tools.doc` 取某个方法的完整教程。调用方不需要预先知道任何方法名，PC 侧也不保存工具清单 ——
设备侧加了新工具，PC 侧自动就能用。

**核心与工具解耦。** 核心只负责协议与调度；每个工具的参数、风险等级、文档都写在它自己的
`source/tools/tool_*.cpp` 里，靠链接器段在启动时自动注册。新增一个工具不需要改核心、不需要改 Makefile。

**通用设备命令统一执行。** MCP 服务器通过 `nxc_call` 执行通用设备命令，
其余工具提供能力发现、文件传输、GDB 调试、画面查看及按键后查看。

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
| `fs` | 11 | SD 卡文件：传输能力、SHA-256、分页列目录、读写、复制、重命名、建目录、删 |
| `save` | 4 | 按用户/存档选择、独立备份、完整性验证及明确确认后的恢复 |
| `boot` | 3 | 读写 Hekate 启动项（改前自动备份） |
| `power` | 3 | 电源状态、重启、关机（带口令） |
| `clk` | 4 | CPU / GPU / MEM 频率，带原值回滚 |
| `probe` | 4 | 逐个试 36 个系统服务，产出能力矩阵 |
| `log` | 2 | 取走模块内存里的环形日志 |

另有核心内建命令：`hello` · `caps` · `help` · `tools.list` · `tools.doc`，
以及可从独立连接发起的 `cancel`（请求取消全部任务）。

## 环境要求

* 一台装了 **Atmosphère**（开源自制系统）的主机，SD 卡可读写
* 构建需要 devkitPro + libnx；下方 Docker 命令与 CI 使用同一固定镜像摘要
* PC 源码入口与发布运行包均需要 Python 3.10+，运行时只使用 Python 标准库
* 宿主回归还需要 C++17 编译器及 POSIX 环境，不需要连接主机

> 开发与实测环境：Atmosphère 1.11.2 / 固件 22.5.0 / Mariko。其它版本未验证。

## 构建

```bash
# 本机装了 devkitPro：
cd device/platform && make -j4

# 没装 devkitPro（走 docker）：
# ★ 请在仓库根目录下执行，并把「根目录」挂进容器 —— Makefile 要从根目录的 VERSION 取版本号。
docker run --rm -v "$PWD":/work -w /work/device/platform \
  devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282 \
  bash -lc '. /opt/devkitpro/devkita64.sh && make -j4'
```

产出 `nxc.nsp`（ExeFS PFS0 格式）。`device/platform/dist/` 保留更新前主分支的设备成品，**不是当前源码的构建**；最新版安装包从上方Release下载，验收范围以发布说明为准。该成品为旧版14工具54方法，长度229103字节，SHA-256 为 `fc5645d6ba53d8f4240d40a1f6e7aff034bb57b1b01af500dcaff2487f4e4a8d`；新设备源码须完成目标固件验收后再替换发行成品。
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

当前源码固定启用正常监听，并在睡眠前关闭连接、唤醒后恢复监听；不再读取 `net_mode.txt` 或 `psc_mode.txt`。
新构建的最小安装只需要 `exefs.nsp` 和空的 `flags/boot2.flag`；`toolbox.json` 与救援工具可选。
PSC注册失败时退出，不开放缺少睡眠处理的监听口。旧模式文件即使留在SD卡上也不会影响新模块。

**旧成品兼容：** `device/platform/dist/exefs.nsp` 仍是保留的旧版，使用它时仍须配套复制
`dist/config/nxc/` 到SD卡 `/config/nxc/`（`listen` / `close`）。这些配置只为旧成品保留；
移除配置依赖后的新设备构建仍需真机验收，不能只删除旧模块的配置来升级。

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

### 启动 PC 后台与 Web

在仓库根目录执行：

```bash
./nxc --ip 192.168.1.50 start
./nxc web
./nxc status
./nxc doctor
```

将示例 IP 换成设备的局域网地址。后台默认只监听 `127.0.0.1:47801`，设备目标默认是
`192.168.1.50:47800`。`web` 启动或复用后台后打开浏览器；普通命令和 MCP 首次请求也可自动启动后台。
`status` 只查看，不自动启动。需要在当前终端运行时使用 `./nxc start --foreground`；结束共享后台用 `./nxc stop`。

Web 包含概览、画面与控制、文件、调试、能力和日志六个页面。概览可保存设备 IP、端口、请求超时和 FTP 设置（默认设备端口 5000、匿名登录），
后台通过 `host.configure` 切换目标；有 GDB 会话时须先关闭会话。保存后按需连接设备，打开网页本身不会
附加进程、抓帧或发送按键。Web 单个上传/下载文件上限为 8 MiB，下载由浏览器保存；较大文件使用 CLI。

启动时保存的目标配置位于 `~/.config/nxc/config.json`；`NXC_CONFIG_DIR` 可更改整个配置/状态目录。
同一目录还保存后台状态、日志及 CLI 默认截图。新 CLI 进程默认读取已保存配置，配置优先级为
命令行参数、环境变量、配置文件、内置默认值。可用环境变量：

| 配置 | 环境变量 |
| --- | --- |
| 设备地址、端口、超时 | `NXC_DEVICE_IP`、`NXC_DEVICE_PORT`、`NXC_TIMEOUT` |
| 本机后台端口 | `NXC_HOST_PORT` |
| 设备 FTP 端口、用户名、密码 | `NXC_FTP_PORT`、`NXC_FTP_USER`、`NXC_FTP_PASSWORD` |
| GDB 后端、可执行文件、已有镜像 | `NXC_GDB_BACKEND`、`NXC_GDB_PATH`、`NXC_GDB_IMAGE` |

环境变量会覆盖 Web 保存的对应值。已有后台的目标或运行配置与 CLI 不一致时会拒绝执行，
先确认配置，再停止旧后台并启动；Web 保存目标后，新 CLI 会读取新配置。
长驻 MCP 客户端须断开并重新连接，才能读取新目标。旧目标请求及旧网页操作绑定原后台实例和配置，
目标变化后失效，不自动跟随新目标，也不自动重放中断的操作。旧网页应刷新概览，确认目标后再继续。

### 后台 FTP 一站式向导

文件页上方提供“后台 FTP 服务”卡片，无需另开 FTP 客户端：

1. 点击“检测后台服务”，核对模块文件、版本、开机加载、端口和登录状态。文件存在不会直接判为运行成功。
2. 未安装时点击“一键安装官方版”；也可导入本地 ZIP，再点击“安装已导入包”。固定来源为 [cathery/sys-ftpd 1.0.5](https://github.com/cathery/sys-ftpd/releases/tag/1.0.5)（原 sys-ftpd-light），本地包只接受相同版本四项内容，允许重新打包。
3. 已识别模块缺配置或启动标记时，使用“配置匿名并启用后台”；原配置会先备份，现有端口保留。PC端口不一致时可点击“使用设备端口”。
4. 首次安装完成显示“待重启验证”。可以自行重启后重新检测，也可在保存应用进度后明确确认“重启设备并验证”；安装本身不会自动重启。登录及目录验证通过后才显示“已就绪”。

安装通过已在线的 NXC `fs` 完成，不依赖尚未安装的 FTP，兼容当前旧 NXC 的 381B分块。启动标记最后写入，缺少必要接口或可用空标记来源时写前拒绝并说明原因。已有模块不覆盖升级；不明模块或来源不明的FTP占用端口时停止安装，现有文件传输仍可使用。

后台服务来自官方固定包，ZIP长度130642字节，SHA-256为
`004e157224f105902b0952e9bdcb0872f5e23c1962985a4bc14aa62f1921febc`；
模块目录为 `/atmosphere/contents/420000000000000E/`。
导入包逐项核验固定内容，拒绝路径穿越、符号链接、重复文件及超限解压。
安装先暂存并回读核验，配置修改前备份；失败报告已写路径和恢复路径，不自动重放未知写入。
文件存在但端口未开标为待加载，登录失败提示配置，NXC查询失败标为未知。
官方说明适配固件19.0.0与Atmosphère1.8.0；固件22.5.0已验证该模块登录和读取，其他组合需自行验证。

官方包下载后在 PC 缓存并校验固定 SHA-256，本地 ZIP 上限4MiB，不直接解压到SD；本项目不随包分发第三方模块。版本、来源、摘要和恢复路径可在卡片详情中查看。需要 NXC 已安装并在线；不代表可在未安装任何设备模块的主机上直接远程部署。首次安装已由隔离旧设备替身验证，真机只验证已有后台服务的检测与复用，尚未验收首次安装。

### 命令行操作

```bash
./nxc caps
./nxc call identify.id --json
./nxc call fs.ls path=/atmosphere/contents
./nxc call fs.ls 'path=/switch/含空格的目录'
./nxc ftp status
./nxc put ./example.txt /config/nxc/example.txt --transport auto
./nxc fetch /config/nxc/example.txt ./downloaded.txt --transport ftp
./nxc deploy ./device/platform/dist/exefs.nsp
./nxc see --output ./screen.jpg
./nxc step 'A:120;WAIT:300' --wait change --output ./after.jpg
./nxc sha /atmosphere/contents/4200000000000012/exefs.nsp
./nxc boot --once
./nxc boot --expect-boot 150 --max-wait 900
./nxc stop
```

`call` 的参数使用自然字符串，含空格时加 shell 引号，不用手动写 `%20`；协议调试可用
`./nxc call --raw 'fs.ls path=/switch'` 原样发送。设备方法和参数以 `caps` 的实时结果为准。
`--ip`、`--port`、`--timeout`、`--host-port`、`--gdb-backend`、`--gdb-path`、`--json` 等全局参数
可以放在子命令前后，完整帮助见 `./nxc --help`。

文件工具默认 `--transport auto`：传输前优先探测 FTP，不可用时选择 `fs`；也可指定 `ftp` 或 `fs`。
FTP 客户端集成在 PC 后台，Switch 仍需运行 FTPD；PC 不另开 FTP 服务。Web 文件页同样可以选择通道、检测 FTP 和浏览目录。
选定通道后失败不自动降级、不重放写入；仅 FTP 只读预检可在空闲连接失效后重新连接一次。
上传和部署先暂存、核验 SHA-256 与长度，再保留旧文件备份并提交；新版 fs 使用设备摘要，旧 fs 使用流式读回。
FTP 已支持旧设备模块上的文件传输；fs 上传仍需要 `fs.rename replace/rollback` 接口。
FTP使用标准库 `ftplib` 的被动二进制模式；目录优先MLSD，缺少时使用NLST/SIZE/CWD，不把任意550错误视为文件不存在。
fs仅在有效的 `fs.transfer_info` / `fs.sha256` 能力声明后启用最多两条在途请求；旧设备单条传输。
能力协商异常或传输中断直接报错，已发送但未确认的修改视为结果未知。
上传提交失败尝试恢复旧目标，无法确认时报告暂存与备份路径；直接 `fs.write` 仍是原地写入，不能替代部署流程。

CLI 上传下载逐块处理文件，Web 保留 8 MiB 上限；断点续传尚未提供。下载先写本地临时文件，完整验证后才替换目标；失败保留本地原文件。
`deploy` 只发布文件，不自动重启。`boot --once` 查询当前开机号；
不带 `--once` 时默认等待当前开机号增加，也可用 `--expect-boot` 指定目标，命令本身不会重启设备。
see/step 图片由 CLI 写入本机，未指定 `--output` 时保存到配置目录的 `downloads/`，终端不打印整段 base64。
CLI 的本地文件路径均在调用端解析成绝对路径；`fetch` 未指定本地路径时保存到调用者当前目录，
不受后台启动目录影响。

### GDB 调试

GDB 运行在 PC 侧，由共享后台持有。先用 `./nxc doctor` 查看实际可用后端；优先使用可用的本机 GDB，
可通过 `--gdb-path` 指定具备目标架构支持的可执行文件。`auto` 也可使用本机已有 Docker 和镜像；
不会自动安装工具或拉取镜像。缺少后端时会明确报告，原生 GDB 在未验证的平台上不承诺可用。

```bash
./nxc --gdb-path /absolute/path/to/aarch64-none-elf-gdb doctor
./nxc gdb open --no-connect
./nxc gdb status
./nxc gdb close

# 以下操作会连接或实际调试设备：
./nxc gdb open
./nxc gdb attach          # 查询并附加当前应用；也可用 --pid 123 指定进程
./nxc gdb run 'info threads' 'bt'
./nxc gdb interrupt
./nxc gdb detach
./nxc gdb close
```

打开调试器、连接目标、附加进程及目标正在运行/暂停是不同状态，以 `gdb status` 为准。
GDB 命令超时会报告结果未知，不自动重放；仍可能占用调试资源，需要先分离或关闭并确认状态。
默认使用 `extended-remote` 连接，支持先连接调试桩、再附加当前应用。
`detach` 分离目标并断开远程连接，保留本机 GDB 进程；之后再 `close` 收尾。
若显式执行原始 MI `-target-detach`，扩展远程连接仍会保留，需继续 disconnect 或 close。
连接和调试可能暂停应用；会话连接或结果未知时，
后台拒绝独立 `mem.*` 访问，避免争用。

### 构建单入口运行包

```bash
python3 pc/build_app.py --output pc/dist/nxc
python3 pc/dist/nxc --version
./pc/dist/nxc web
```

构建只使用标准库 zipapp，不安装打包器。产物 `pc/dist/nxc` 是无扩展名的可执行 Python 运行包，
包含 PC 源码和 Web 资源；运行机器仍须提供 Python 3.10+，不是无需 Python 的原生二进制。
可以将它放到其他目录，用绝对路径调用；CI 同时上传此运行包。下载后可用 `python3 /path/to/nxc`
运行；需要直接执行或作为 MCP 的 command 时，先确保文件有执行权限，例如 `chmod +x /path/to/nxc`。

### 接 MCP（让 AI 直接用）

```json
{
  "mcpServers": {
    "nxc": {
      "command": "/absolute/path/to/NXC/nxc",
      "args": ["mcp"]
    }
  }
}
```

`command` 可换成发布运行包 `nxc` 的绝对路径。默认读取上述已保存目标配置；需要独立目录时在
MCP 配置的 `env` 中设置 `NXC_CONFIG_DIR`。也可显式设置 `NXC_DEVICE_IP` / `NXC_DEVICE_PORT` 等环境变量，
但它们会覆盖保存值；更改目标后应重连长驻 MCP。配好后按客户端要求信任该连接器。

MCP stdio 只代理 JSON-RPC，日志写到 stderr。MCP 断开或 stdin 结束不停止共享后台，
CLI、Web 和其他 MCP 客户端可继续使用；统一停止使用 `nxc stop`。

8 个 MCP 工具：`nxc_caps`（发现设备能力）· `nxc_call`（通用设备命令）· `nxc_deploy`（上传模块并读回校验）·
`nxc_fetch`（取回设备文件）· `nxc_put`（上传文件）· `nxc_gdb`（驱动 PC 侧 GDB）·
`nxc_see`（直接返回当前画面）· `nxc_step`（执行按键步骤后返回画面）。

如果游戏的本地无线通信模式使主机断开路由器 Wi-Fi，PC 侧连接会暂时不可达；
恢复同一局域网连接后才能继续使用，端口超时本身不代表主机卡死。

## 协议

一条请求一行：

```
<工具>.<方法> [键=值]...
```

以空格切词，第一个词是方法名，其余是 `键=值`（少数方法也接受没有 `=` 的位置参数）。值里不能出现空格、
制表符与换行，这类字节按 `%XX` 转义，规矩和 URL 一样：空格写 `%20`，字面的 `%` 写 `%25`；多字节 UTF-8
不转义。`nxc call` 与 MCP 的 `nxc_call` 会自动转义，日常写自然值即可；`--raw` 不做转义。

响应是 `OK <行数>` 加若干行载荷，载荷行要么是 `键=值`，要么以 `#` 打头表示自由文本（可含空格，日志类内容
用这种）。失败时不带载荷，直接回 `ERR <码> <说明>`：

| 码 | 含义 |
| --- | --- |
| 400 | 请求或参数不合法 |
| 403 | 缺少或写错确认口令 |
| 404 | 工具 / 方法 / 目标不存在 |
| 409 | 前置条件不满足，例如 `clk.set` 读不到原值就拒绝写入 |
| 413 | 请求或单个载荷过大 |
| 422 | 备份清单或文件完整性验证失败 |
| 499 | 存档操作被取消，不能当作完成 |
| 500 | 执行失败 |
| 503 | 依赖的系统服务当前不可用，例如网络或 SD 卡未就绪 |

没有 `=` 的请求参数按位置记为 `_0`、`_1` 等；请求以换行结束。结构化载荷一行可包含多个 `k=v`，
二进制使用 base64。超长、重复或过量参数会被拒绝。

能力发现使用 `tools.list [tool=<工具名>]`；教程使用 `tools.doc name=<工具[.方法]> [offset=] [lines=]`。
工具参数、确认口令及分页范围以设备返回的教程为准。

## 安全边界

| 级别 | 方法 |
| --- | --- |
| 只读 | `identify.*` `tele.*` `title.list/current/name/processes` `screen.info/capture/wait_*` `input.status` `mem.info/read/peek` `net.*` `fs.roots/ls/stat/read/transfer_info/sha256` `save.list/verify` `boot.list/status` `power.status` `clk.list/get` `probe.list/results` `log.*` |
| 需确认口令 | `title.launch` `title.terminate` `mem.write` `boot.set` `power.reboot` `power.shutdown` `save.restore`（`confirm=RESTORE_SAVE`） |
| 无口令但会改动设备状态 | `input.press/release/seq/detach` `clk.set/restore` `fs.write/copy/rename/mkdir/rm` `save.backup` `probe.try/group` `cancel` |
| 默认拒跑 | `probe.try svc=nv` —— `nv` 被标为已知危险，默认回 `result=refused`，加 `force=1` 才会执行 |

要知道的边界：

1. `mem.*` 与 PC 侧 GDB（`22225`）互斥，两者抢同一个调试能力，只能二选一。
2. `fs.*` 只能碰 SD 卡，碰不到主机内部储存（NAND / 系统分区）。
3. 模块的 NPDM 声明了全量服务访问（`"service_access": ["*"]`）。上表是协议层的分级，不是内核层的沙箱 ——
   能连到 `47800` 的人就等于拿到了这个模块的全部权限。设备协议仍为明文且没有鉴权，不要把它暴露到公网。
4. PC 后台只绑定回环地址 `127.0.0.1`，不启用 TLS；设备侧的局域网权限模型保持原有约定。

## 已知限制

* `title.launch` 默认普通应用路径只接入 libnx 明确支持固件 `1.0.0..17.0.1` 的 `nsdev` 接口，尚未真机验收。
  当前测试固件 `22.5.0` 返回 `409 application-launch-unsupported`；请用户判图后通过主界面操作启动。
  显式 `mode=applet|program|libapplet` 保留为实验接口，不会自动回退调用。
* 关闭应用后若出现提示，须先查看画面，由用户判断下一次输入，不会按 JPEG 大小自动按 A。
* PC 统一后台已于 2026-10-27 通过真机能力发现、遥测、截图、临时文件读回及 Docker GDB 14.1 的
  当前应用附加、线程/寄存器读取、分离和再次附加；原生 GDB 的各平台兼容性尚未全面验收。
  PC与旧设备模块的FTP文件目录、上传下载及已有后台服务复用已真机通过。设备源码中的新版fs流水/摘要/提交和新增存档接口仍需目标设备验收。
* `title.name` 对系统应用取不到名字（系统标题没有元数据，属客观限制）。
* `power.status` 的睡眠键读数在该固件上取不到。
* 主机进入睡眠时连接会断开，醒来后需要重连。
* 当前真机环境存在中文文件名创建/目录枚举兼容限制：部分中文目标经暂存重命名后可按路径读取、校验，
  却不在FTP和设备目录列表中显示；直接FTP创建也失败，根因尚未定位。现场优先使用ASCII目标文件名，
  不能将本地FTP替身的Unicode测试通过视作所有设备兼容。
* 存档备份和恢复都要求目标应用完全关闭；新恢复流程尚未真机验收。
* 文件系统不支持覆盖式原子重命名：在线更新使用已校验备份、两步切换及失败回滚，切换中仍有掉电窗口。
* 主机上若另跑着别的调试或传输组件（GDB 桩、FTP、视频流等），它们不属于本项目；其中 GDB 桩与
  本平台的 `mem.*` 互斥。

## nxc-recovery

`tools/nxc-recovery/` 是一个普通的 NRO 自制程序，不是系统模块，也不依赖 `47800` 或 FTP。它只做一件事：
当平台把自己写坏、或者刷进一份不能用的版本时，把一份已知可用的模块写回模块目录并重启。

1. 平时就把 `nxc-recovery.nro` 放到 `sdmc:/switch/`，并在 `sdmc:/switch/nxc-recovery/` 里留一份已知可用的模块。
2. 出事时从 hbmenu 启动它，上下选镜像、按 A，它会写回、逐字节校验，然后重启。

## 目录

```
device/platform/      设备侧 sysmodule：核心、工具、SDK、构建脚本与保留的旧设备成品
tools/nxc-recovery/   独立小程序
nxc                   PC 源码树统一入口
pc/src/nxc_host/      配置、后台、设备工具、GDB、CLI/MCP 和 Web 资源
pc/build_app.py       标准库 zipapp 构建入口，产出 pc/dist/nxc
pc/tests/            无设备回归；manual/ 为手动测试，不是产品入口
skills/               本项目的开发流程约定（改动留痕、崩溃取证工具链）
tests/                可复用宿主回归与统一检查入口
```

## 开发与验证

PC 的 `config.py` / `client.py` 管理配置与后台启动，`runtime.py` / `daemon.py` 持有共享会话并提供本机HTTP服务，
`device.py` 负责设备协议，`services/` 管理文件、FTP和GDB，`mcp.py` / `web/` 提供客户端入口。
请求在执行锁内复核后台实例和目标配置；退出先标记关闭，再释放资源，避免排队操作在退出后继续执行。

设备核心位于 `device/platform/source/core/`，负责注册、协议、网络、日志、睡眠与队列；
工具位于 `source/tools/`，SDK为 `device/platform/include/nxc_sdk.hpp`。
网络循环与串行工作线程分开，队列容量8，每客户端最多2条未完成请求；连接代次防止旧应答发给新连接。
慢客户端不会无限阻塞其他连接。独立连接发送 `cancel` 可请求取消，但不能强制中断同步系统IPC，也不承诺回滚已执行修改。

### 新增设备工具

在 `device/platform/source/tools/` 新建文件即可，无需改核心清单或Makefile：

```cpp
#include "nxc_sdk.hpp"
namespace {
void echo(const nxc::Args& a, nxc::Reply& r) {
    const char* text = a.get("text");
    if (!text) { r.fail(400, "missing-text"); return; }
    r.kv("echo", text);
}
const nxc::Method methods[] = {{"echo", "回显 text= 参数", echo}};
}
NXC_DEFINE_TOOL(nxc_tool_echo, "echo", "示例工具", methods);
```

工具描述符由链接器的 `nxc_tools` 段自动收集。正式工具应补齐 `nxc::Param[]`、`nxc::MethodInfo` 的风险等级与教程，
并使用 `NXC_DEFINE_TOOL_EX` 提供工具级说明，参考 `tool_fs.cpp` 末尾。
耗时循环调用 `nxc::host::operationCancelled()`；有副作用前用 `host::breadcrumb()` 记录进度，诊断日志用 `host::log()`。

### 离线检查

```bash
python3 -W error tests/run_checks.py
python3 pc/build_app.py --output pc/dist/nxc
python3 pc/dist/nxc --version
```

检查需要Python3.10+、C++17编译器及POSIX；运行Python回归、6个独立C++程序、死连接、监听恢复和文案检查，不连接真实设备。
设备交叉编译使用上方固定镜像，验收构建应指定全新目录，例如 `make BUILD=build-check -j2`，避免旧对象掩盖变化。
CI运行宿主回归、构建PC运行包并交叉编译设备；不会自动覆盖 `dist/exefs.nsp`。

### 故障排查

| 现象 | 排查方法 |
| --- | --- |
| 无心跳、端口不通 | 核对模块目录、`flags/boot2.flag` 和网络配置；查看 `/atmosphere/fatal_errors/` 是否有转储 |
| 有心跳、端口不通 | 查看 `/config/nxc/boot_progress.txt` 最后一行；确认设备醒着、socket初始化和PSC注册成功 |
| 端口通但无应答 | 用 `identify.id` 核对实际服务状态与开机号，不能只凭TCP握手判为启动完成 |
| 需要恢复模块 | 使用前述 `nxc-recovery`，或离线换回已知可用成品；停用可移走模块启动标记 |

`/config/nxc/heartbeat.txt` 记录心跳，`boot_progress.txt` 保存最近进度；内存环形日志通过 `log.dump` 读取，重启会丢失。
崩溃可用匹配该构建的 `nxc.elf` 和 `aarch64-none-elf-addr2line -e nxc.elf -f -C <偏移>` 定位。

### v1.0.3 更新与验证范围（2026-10-27）

- PC入口统一为 `nxc` 和共享后台，增加Web、GDB MI2会话管理、流式文件双通道及后台FTP向导。
- 设备源码完善有界队列、取消与监听恢复、文件分页/摘要/提交、存档身份与完整备份校验。
- 修复断线重放修改、旧目标请求误发、上传校验与备份恢复、GDB超时及资源占用状态问题。
- macOS与Linux各235项测试、独立C++及网络回归、设备交叉编译和PC运行包冒烟通过。
- 宿主测试与构建不代替真机验收；新设备源码、存档恢复及掉电恢复仍需对应设备验证。
- 移除网络与PSC实验模式，正常监听和睡眠断连固定启用；新模块无需两个模式配置文件。

## 许可

本项目以 **GNU General Public License v2.0**（`GPL-2.0-only`）发布，全文见 [`LICENSE`](LICENSE)。

`Copyright (C) 2026 granule256`

你可以在 GPL-2.0 的条款下使用、修改、再分发；分发衍生作品时必须以同样的许可提供源码。

## 免责

本项目是自制软件（homebrew）作品，只在你自己的设备上使用。修改系统模块有导致主机无法开机的风险，
仓库里带了 `nxc-recovery`，但风险自负。本项目不包含任何厂商的专有代码或内容。
