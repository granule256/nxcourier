---
name: nxc-crash-forensics
description: NxCourier 平台出问题时的【只读取证】流程——模块崩/主机睡眠崩/端口不通时，怎么从设备把崩溃报告拉下来、解码该主机的 Result 错误码、判断崩溃是否确定性、以及 47800 不通时用 FTP 当备用通道。触发词：崩溃报告、fatal_reports、2165、omm 崩、睡眠崩溃、端口不通、取证、Result 错误码。
---

# NxCourier 崩溃取证（只读）

> ★ 本文是本项目的**排查流程约定**（随仓库一起发布）。
> 文里提到的 `docs/`、`.workbuddy/memory/`、`事故取证-*/` 都是维护者**本机的记录目录**，
> 不随仓库发布 —— 你要照着做的话，自己挑个位置归档即可。

**为什么有这份**：2026-10-06 排查 `omm` 睡眠崩溃卡了很久，根因之一是
**从没把 `omm` 自己的原始崩溃报告拉到本机**，只靠文档里引的几行。
报告一拉下来，"性质"当场就变了（见下 §四）。

★ **这个技能只做只读动作**：列目录、下载、读文件。
**不写设备、不改卡、不动模块**（那些属于 `nxc-change-log`）。

## 一 ★★★ 备用通道：47800 不通时，用 FTP(5000)

**这是本技能最有价值的一条。** 模块被禁用 / 没起来时 `47800` 拒服务，
但设备上的 **`ftpd`（端口 `5000`，账号 `<用户>`/`<密码>`）** 常常还活着。

```bash
# 列目录
curl -s --list-only --connect-timeout 8 --max-time 30 \
  "ftp://<用户>:<密码>@<设备IP>:5000/atmosphere/fatal_reports/"

# 下载单个文件
curl -s "ftp://<用户>:<密码>@<设备IP>:5000/atmosphere/fatal_reports/<文件名>" -o <本地文件>
```

★ **四个坑**：
1. **端口是 5000，不是 21**。`ftp://host/`（默认 21）会 `Connection refused`。
2. **走代理会连不上**：本机的代理配置里已经把局域网网段排除了（形如 `192.168.*`），
   若你那边报代理错就加 `--noproxy '*'`。
3. `ftpd` 跑在 **hbmenu 进程**里 ⇒ **一启动游戏就没了**（`/modules/sysftpd` 那个是加载器）。
   ⇒ 连不上先确认主机是不是停在 hbmenu/相册界面。
4. ★ 老文档里写的 `tools/device_ftp.py` **并不存在**，别去找。

### 先判"系统没起"还是"我们挂了"

```bash
for p in 47800 22225 6666 5000; do
  (nc -z -G 2 <设备IP> $p && echo "$p 通") || echo "$p 拒绝"
done
```
* `22225`（大气层 GDB 桩）/ `6666`（SysDVR）**也拒** ⇒ **系统没起**
* 只有 `47800` 拒 ⇒ **我们挂了**（系统是好的）

## 二 崩溃报告在哪、怎么读

| 目录 | 装的是 |
| --- | --- |
| `/atmosphere/fatal_reports/` | ★ **别人（系统模块 / 第三方）**的 fatal |
| `/atmosphere/crash_reports/` | **我们自己**（`4200000000000012`）的 |
| `/atmosphere/fatal_reports/dumps/` | 每份对应的转储（`<同名前缀>.bin`，约 512B：寄存器 + TLS） |

★ 文件名格式 `<unix 时间戳>_<ProgramID>.log`。
★ 每个系统模块的 ProgramID 是 `01000000000000XX`：**`...0045` = `omm`**。

## 三 ★★ 解码该主机的 Result 错误码（最容易搞错的一步）

```
Result 是一个 22 位的数：
  bits 0..8   = Module        ← 低 9 位！
  bits 9..21  = Description
  （bits 22..31 保留）

显示成 "2MMM-DDDD" 时：
  第一组 = 2000 + Module      ← ★ 是【Module】，不是 Description
  第二组 = Description
```

```bash
# 0x2A5 -> ?
python3 -c "r=0x2A5; print('Module', r & 0x1FF, 'Desc', (r>>9) & 0x1FFF)"
#  -> Module 165 Desc 1   => 显示 2165-0001
```

★ **两个坑**：
* **别把低 9 位当 Description** —— 记成"Module 在低 9 位"。
* 解码出来的 **Module 不一定是"崩掉的那个进程"**！
  例：`omm` 崩了，但 `0x2A5` 的 Module 是 **165 = `Spsm`**（`omm` 自己是 **177**）。
  ⇒ 要说清「**崩的是哪个进程**（看 `Process Name`/`Program ID`）」和
  「**错误码属于哪个模块**（解码 Result）」——**这是两件事**。

★ 已攒下的对照（省得每次重查）：

| Result | Module | Desc | 意思 |
| --- | --- | --- | --- |
| `0x2A5` | 165 `Spsm` | 1 | PmControl dispatched request timed out（PSC 电源切换派发超时） |
| `0x7D2A5` | 165 `Spsm` | 1001 | PmRequest aborted |
| `0x4A2` | 162 | 2 | 社区说的 `2162-0002` |
| `0x2A5` 场景里 **`omm` 是 177** | | | 别混 |

★ 权威来源（改前先查，别背）：`libnx` 的 `nx/include/switch/result.h`（`R_MODULE`/`R_DESCRIPTION`）
+ `switchbrew.org/wiki/Error_codes`（模块表 + 显示规则）。

## 四 ★★ 判断"是不是同一个崩溃"

```bash
cd <报告目录>
shasum -a 256 *.log | awk '{print $1}' | sort | uniq -c     # 看有几组
ls -l *.log                                                  # 字节数是否一致
```
* ★ **长度全等 + 只有地址不同 ⇒ 确定性崩溃**（不是内存踩踏），可以直接当"一回事"处理。
* 拉几份比对 `PC`/`LR` 相对 `Start Address` 的偏移，以及 TLS 里的 IPC 消息头
  （`53 46 43 49` = `"SFCI"`，即 Horizon 的 CMIF 魔数；后两个 u32 是 `version` 与 `command_id`）。
* ★ TLS 里那条消息**是"入站请求"还是"出站回复"要靠反汇编才能定死** —— 别当成定论。
* ★ **想一击定案就得有那个模块的 NSO**，把 `PC 偏移` 丢给
  `aarch64-none-elf-objdump -d` / `addr2line`。卡上通常没有固件转储（`/backup` 只有 PRODINFO）。

## 五 归档（取证材料留在本机，不入公开仓库）

★ 跟先例走：仓库根上开一个 `事故取证-<日期>-<一句话>/`，把 `.log` 和 `dumps/` 一起放进去
（十几份也就一二百 KB），**供本机后续查阅**。

★ **但不要再 `git add` 它们** —— 2026-10-06 仓库转为公开后，`.gitignore` 已把
`事故取证-*/` 与 `救急包/` 排除：这些是崩溃转储 + 系统模块的内存镜像，
只对本机有意义，不适合公开。
★ 结论要落到 `docs/` 的排查/分析文档里（那才是入库的部分）；**别放 `tmp/`**
（它被忽略，且会被当成可随时清理的中间物）。

## 六 收尾

* 按 `nxc-change-log` 的规矩在 `docs/改动日志-平台.md` 顶部加一条：
  ★ 写明 **"部署了吗＝否"**（只读取证没有产物）、**"设备侧只做了只读动作"**、验证方式。
* 结论要落到 `docs/` 的排查/分析文档里，**并同步 `.workbuddy/memory/MEMORY.md` 的"头号未决问题"**，
  否则下一个会话会拿着过期结论继续跑。
