#!/usr/bin/env python3
# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""NxCourier 平台 · PC 侧 MCP 服务器（stdio）。

## 设计要点：MCP 侧也做「核心 + 动态发现」，**不为每个设备工具写一个包装**

设备侧是「平台核心 + 注册工具」（见 device/platform/README.md）。
如果 PC 侧给每个设备工具都硬写一个 MCP 工具，就会双向耦合：
设备加一个工具 → 这里要改一次；两边清单还会各自漂移。

所以这里只暴露**两个稳定的入口**：

    nxc_caps   —— 动态发现：连上设备，问它现在有哪些工具、每个工具有什么方法
    nxc_call   —— 唯一执行入口：method="<工具>.<方法>"，params={...}

设备侧加工具，这里**一个字都不用改**。AI 先 nxc_caps 拿清单，再 nxc_call 调用。

## 传输
MCP 的 stdio 传输 = JSON-RPC 2.0，**一行一个 JSON 对象**。
支持：initialize / notifications/initialized / tools/list / tools/call / ping。
日志一律写 stderr —— 写 stdout 会把协议流弄脏。

## 用法
    python3 pc/nxc_mcp.py                     # 作为 MCP stdio 服务器运行
    python3 pc/nxc_mcp.py --ip 192.168.1.50 --port 47800
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import sys
import traceback
from pathlib import Path

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "nxc-platform"
SERVER_VERSION = "0.1.0"

DEFAULT_IP = os.environ.get("NXC_DEVICE_IP", "192.168.1.50")
DEFAULT_PORT = int(os.environ.get("NXC_DEVICE_PORT", "47800"))

# 自我更新的分块参数。
#   ★ 踩过的坑：协议里单个参数值的缓冲是 kArgValueBytes = 512 **含结尾的 '\0'**，
#   所以实际只能放 511 个字符。用 384 字节/块算出来正好是 512 个 base64 字符 ——
#   会被截掉 1 个字符 ⇒ **每个满块丢 1 字节**（实测 371 个满块 = 少了 371 字节）。
#   3060 字节 → base64 4080 字符，塞进放大后的参数缓冲（4096 含 \0）。
DEPLOY_CHUNK = 3060
#   一次连接里连发多少条：回复行上限 48，fs.write 现在每条只回 1 行 ⇒ 20 条很安全。
DEPLOY_BATCH = 20
#   ★ 引导用的小分块：自适应回退，以及"自我更新"这条路径专用。
#   因为要更新的设备可能还跑着参数上限 512 的旧版本，大分块会被静默截断。
BOOTSTRAP_CHUNK = 381
#   默认只允许写平台自己的模块文件 —— 别让这个工具变成任意覆盖 SD 卡的通道。
DEFAULT_REMOTE_MODULE = "/atmosphere/contents/4200000000000012/exefs.nsp"

#   请求行上限（设备侧 kRequestBytes）。组装命令时校验，别撞上限被整条丢掉。
PROTOCOL_LINE_MAX = 8192
#   ★ 单个参数值的上限：设备侧 kArgValueBytes = 4096（含结尾 '\0'）⇒ 最多 4095 个字符。
#   这个数必须跟设备侧的常量**一起改** —— 我漏改过一次，结果校验还在拿老的 511 卡自己。
ARG_VALUE_MAX = 4095


def log(*parts: object) -> None:
    """日志只能走 stderr：stdout 是协议流。"""
    print("[nxc-mcp]", *parts, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- 设备通信
class Device:
    """与设备的一条**长连接**封装。

    为什么复用连接：这台主机的 TCP 栈对「频繁建连/断连」敏感（FTP 那边有同样的毛病，
    会返 451 或直接 reset）。每次调用都新建连接会偶发 ConnectionResetError。

    重试策略（安全优先）：
      只在**一条响应都还没收到**的时候才重连重试一次 —— 这种情况下"什么都没执行"，
      重试不会造成重复副作用。一旦已经读到过部分响应，就直接抛出，绝不重放。
    """

    def __init__(self, ip: str, port: int, timeout: float = 15.0) -> None:
        self.ip = ip
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._stream = None

    # ---- 连接管理
    def _close(self) -> None:
        try:
            if self._stream is not None:
                self._stream.close()
        except OSError:
            pass
        try:
            if self._sock is not None:
                self._sock.close()
        except OSError:
            pass
        self._stream = None
        self._sock = None

    def _connect(self) -> None:
        self._close()
        self._sock = socket.create_connection((self.ip, self.port), timeout=self.timeout)
        self._sock.settimeout(self.timeout)
        self._stream = self._sock.makefile("rb")

    # ---- 收发
    def _exchange(self, commands: list[str], progress: list[int] | None = None) -> list[dict]:
        """按顺序发一批命令、按顺序读回。

        `progress[0]` 会被更新成"**已经拿到响应的命令条数**"——它只给 `call()` 用来判断
        "还能不能安全重试"，所以**一旦某条命令的响应回来了就算它已执行**（哪怕正文还没读完）。
        """
        results: list[dict] = []
        for idx, cmd in enumerate(commands):
            self._sock.sendall((cmd + "\n").encode())
            raw = self._stream.readline()
            # ★★★ 2026-10-06：**空行不是"设备的回答"，是"对端已经把连接关了"。**
            #   主机重启过之后（今晚重启了好几次），MCP 手里这条长连接会变成半死状态：
            #   `sendall` 还能写进本地缓冲，而 `readline()` 立刻返回 b""。
            #   原来这里把空串当成"无法识别的响应"继续往下走 ⇒ **绕过了下面 call() 里的重连+重试**，
            #   最后被上层翻译成"文件不存在或为空"。
            #   ★ 我照着那句话去查了半天"文件是不是丢了"，其实文件好好的，是连接状态的问题
            #     —— 这就是"报错撒谎"的代价。抛 OSError 才能走重连那条路。
            if raw == b"":
                raise ConnectionResetError(
                    "对端在应答前关闭了连接（多半是主机重启过 ⇒ 长连接已失效）")
            if progress is not None:
                progress[0] = idx + 1      # ★ 响应回来了 ⇒ 这条命令一定执行过了
            header = raw.decode("utf-8", "replace").rstrip("\n")
            entry: dict = {"command": cmd, "header": header}
            if header.startswith("OK "):
                count = int(header.split()[1])
                lines: list[str] = []
                for _ in range(count):
                    body = self._stream.readline()
                    # ★ 正文读到一半就断（不是空行！）—— 咽下去会变成"静默截断的数据"。
                    if body == b"":
                        raise ConnectionResetError(
                            f"应答正文没读完就断线（已读 {len(lines)}/{count} 行）")
                    lines.append(body.decode("utf-8", "replace").rstrip("\n"))
                entry["lines"] = lines
            elif header.startswith("ERR "):
                parts = header.split(" ", 2)
                entry["error"] = {
                    "code": int(parts[1]),
                    "message": parts[2] if len(parts) > 2 else "",
                }
            else:
                entry["error"] = {"code": -1, "message": f"无法识别的响应: {header!r}"}
            results.append(entry)
            if progress is not None:
                progress[0] = len(results)
        return results

    def call(self, commands: list[str]) -> list[dict]:
        progress = [0]
        try:
            if self._sock is None:
                self._connect()
            return self._exchange(commands, progress)
        except OSError as exc:
            self._close()
            # ★★★ 2026-10-06：这里以前是**无条件**重连重试 —— 与类注释里
            #   "只在一条响应都还没收到时才重试"的承诺**不符**，而且真会**重放写操作**
            #   （一批 10 条里前 5 条成功了，剩下 5 条会被连着重放一遍）。
            #   现在按 progress 判断：已经收到过任何响应 ⇒ 绝不重放，把真实情况抛出去。
            if progress[0] > 0:
                raise RuntimeError(
                    f"连接在第 {progress[0] + 1} 条命令上中断（前面 {progress[0]} 条已经执行完）；"
                    f"为避免重放已经生效的操作，**没有自动重试**。原错误：{type(exc).__name__}") from exc
            log(f"连接中断（{type(exc).__name__}），未收到任何响应，重连后重试一次")
            self._connect()
            return self._exchange(commands, [0])

    def call_one(self, command: str) -> dict:
        return self.call([command])[0]

    def close(self) -> None:
        self._close()


# 设备侧对值里的「空格 / % / 控制字符」做了最小转义（%20 / %25 / %XX），
# 因为 k=v 之间是用空格分列的。这里解回来。
_PCT_ESCAPE = re.compile(r"%([0-9A-Fa-f]{2})")


def unescape_value(text: str) -> str:
    """还原设备侧转义过的值。被转义的只有 ASCII（空格/%/控制字符），
    中文等多字节 UTF-8 不参与转义，所以逐字节还原即可。"""
    if "%" not in text:
        return text
    return _PCT_ESCAPE.sub(lambda m: chr(int(m.group(1), 16)), text)


def escape_value(text: str) -> str:
    """把要**发出去**的值转义成协议能承载的形式。

    ★ 为什么要有它（2026-10-05 修）：入方向原来什么都不做，于是卡上**带空格的名字**
    （`visible tinted rocks_3002235272`）根本没有任何写法能表达 —— MCP 通道直接抛错，
    命令行通道回一个长得像「文件不存在」的 404。
    现在两边对称：我们转义 → 设备还原，`fs.ls` 列出来的名字能原样喂回去。
    详见 docs/反馈-参数值含空格无法表达-20261005.md。

    ★ 规则（**只转义"协议承载不了"的字节，其余一律不动**）：
        - 空格 / 制表符 / 换行 / 其它控制字符 → `%XX`（空格 = `%20`）
        - **`%` 原样透传** —— 这样"手写的 `%20`"仍然表示空格（老习惯不受影响），
          想表达**字面的 `%`** 则写 `%25`（和 URL 里的规矩一样）。
        - 不含上述字符的值**原样返回**，所以绝大多数调用逐字节不变。

    ★ 转义是**客户端**的责任；`raw`/`nc` 那种直接发协议行的通道不经过这里。
    """
    if not any(ch == " " or ord(ch) < 0x20 or ord(ch) == 0x7F for ch in text):
        return text
    out: list[str] = []
    for ch in text:
        if ch == " " or ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append("%%%02X" % ord(ch))
        else:
            out.append(ch)          # ★ 含 '%'：原样透传；非 ASCII 也原样（保持 UTF-8）
    return "".join(out)


def parse_payload(lines: list[str]) -> list:
    """'k=v k2=v2' → dict；'#文本' → str。值里的 %20/%25/%XX 会被还原。"""
    out = []
    for line in lines:
        if line.startswith("#"):
            out.append(line[1:])
            continue
        record: dict = {}
        for field in line.split(" "):
            if not field:
                continue
            key, _, value = field.partition("=")
            record[key] = unescape_value(value)
        out.append(record)
    return out


# ---------------------------------------------------------------- MCP 工具定义
TOOLS = [
    {
        "name": "nxc_caps",
        "description": (
            "★ 能力发现的唯一入口：列出目标主机上 NXC 平台**当前**提供的一切 —— "
            "有哪些工具、每个工具有哪些方法、每个方法的**参数表**、**风险等级**"
            "（read=只读 / write=只写 SD 卡 / action=会改设备状态 / danger=可能把设备搞死）"
            "以及有没有教程。\n"
            "**调用任何 nxc_call 之前先调这个**：设备侧的能力是动态的，清单**来自设备自己**"
            "（本服务器不硬编码任何能力）。\n"
            "同时返回设备身份（平台版本、固件版本、开机次数）。\n"
            "想要某个方法的完整用法（示例/注意/返回/相关）：nxc_call 调 `tools.doc`。"
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "nxc_call",
        "description": (
            "在目标主机上执行一个 NXC 平台方法。唯一执行入口，"
            "不为每个设备能力单独建工具。\n"
            "method 形如 `<工具>.<方法>`（先用 nxc_caps 拿到准确的方法名与参数）。\n"
            "params 是传给方法的键值对，例如 {\"path\": \"/switch\"}。\n"
            "另一种用法：把 method 设成 `tools.doc` 并传 {\"name\": \"<工具.方法>\"} 可读该方法的完整教程。\n"
            "不确定有什么可用时，先调 nxc_caps。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "method": {"type": "string", "description": "形如 `<工具>.<方法>`，例如 tele.freq"},
                "params": {
                    "type": "object",
                    "description": (
                        "方法的参数键值对；值**可以含空格**（客户端会按协议自动转义成 %20 发送，"
                        "设备侧对称还原），所以卡上带空格的路径能直接写。"
                        "二进制仍请用 base64。"
                    ),
                    "additionalProperties": {"type": ["string", "number", "boolean"]},
                },
                "raw": {
                    "type": "string",
                    "description": "逃生舱：直接发一整行协议文本（优先用 method/params，仅在协议调试时用）",
                },
            },
            "required": [],
            "additionalProperties": False,
        },
    },
    {
        "name": "nxc_gdb",
        "description": (
            "在目标主机上使用**完整 GDB**：断点、单步、读写寄存器、调用栈、"
            "反汇编、watchpoint、符号。GDB 跑在 **PC 侧**（devkitPro 容器里的 aarch64-none-elf-gdb），"
            "不在设备侧 —— 这样拿到的是完整调试器而不是子集。\n"
            "★★ 与 nxc_call 的 `mem.*` 工具**互斥**：两者抢同一个调试能力，同时用会崩机"
            "（实测 fatal 01000000000d609）。二选一 —— "
            "要「不冻结游戏、持续读写内存」用 nxc_call 的 mem.*；"
            "要「停下来调试（断点 / 调用栈 / 单步）」用本工具。\n"
            "★ GDB 的工作方式是「停下来看」：连接（connect=true）和打断点都会让目标进程停住。\n"
            "用法：action=open 开会话 → action=run 发命令 → action=detach 放开目标让它继续跑"
            "（★ 用 detach，不要用 close 来恢复游戏）→ action=close 收尾。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["open", "run", "detach", "close", "status"],
                    "description": "open=开会话（连上会停住目标）；run=执行 gdb 命令；detach=放开目标让它继续跑；close=关会话；status=看状态",
                },
                "commands": {
                    "type": ["string", "array"],
                    "items": {"type": "string"},
                    "description": "action=run 时要执行的 gdb 命令，字符串（按行拆）或字符串数组",
                },
                "connect": {
                    "type": "boolean",
                    "description": "action=open 时是否立刻 target remote 连上设备（默认 true；连上会停住目标进程）",
                },
                "timeout": {
                    "type": "number",
                    "description": "action=run 的等待秒数，默认 60。若上一条命令让目标继续运行，gdb 要等它停下来才会回来",
                },
            },
            "required": ["action"],
            "additionalProperties": False,
        },
    },
    {
        "name": "nxc_deploy",
        "description": (
            "把本地一个文件**分块写进目标主机**（SD 卡），用于**平台自我更新**。\n"
            "用途：改完设备侧代码、编译出新的 exefs.nsp 之后，**不需要 FTP、不需要拔卡**，"
            "直接由本工具按 384 字节分块走平台的 fs.write 写进去，再读回校验 sha256。\n"
            "写进去**不会立即生效** —— 平台是开机时加载的，需要重启（可用 nxc_call 的 power.reboot）。\n"
            "★★ 默认只允许写平台自己的模块文件；要写别的路径必须显式给 remote_path，"
            "且仍受设备侧白名单限制。写坏模块的后果：下次开机加载失败，拔卡删目录即可复原。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "local_path": {"type": "string", "description": "本地文件绝对路径（如编译产物 dist/exefs.nsp）"},
                "remote_path": {
                    "type": "string",
                    "description": f"设备上的目标路径，默认 {DEFAULT_REMOTE_MODULE}",
                },
                "verify": {
                    "type": "boolean",
                    "description": "写完后读回并比对 sha256（默认 true；多花几秒但对自我更新值得）",
                },
            },
            "required": ["local_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "nxc_fetch",
        "description": (
            "把目标主机SD 卡上的一个文件**取回本地**，供我直接查看。\n"
            "主要用途：`nxc_call` 调 `screen.capture` 抓完图之后，用本工具把那张 JPEG 拉回本机，"
            "这样我就能真的**看见主机画面**（而不是只拿到一堆字节）。\n"
            "也适用于把设备上的日志、存档等文件拉回本地分析。\n"
            "★ 只读，不改设备上的任何东西。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "remote_path": {"type": "string", "description": "设备上的文件路径（必须在 fs 白名单内）"},
                "local_path": {
                    "type": "string",
                    "description": "本地保存路径；不填就用远端文件名存到当前目录",
                },
            },
            "required": ["remote_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "nxc_put",
        "description": (
            "把本地文件写到目标主机SD 卡上的**任意路径** —— "
            "这是 FTP 的替代品，用来装/更新第三方 homebrew 与 sysmodule，"
            "也用来往卡上放任何文件。\n"
            "★ 白名单（/config/nxc、/switch、本平台自己的模块目录、/bootloader）以内的路径直接写；\n"
            "★ 白名单以外需要带 confirm=I-KNOW-THIS-TOUCHES-OUTSIDE-ROOTS —— "
            "门禁在设备侧，这个参数只是透传下去。\n"
            "写完会读回比对 sha256（verify 默认开），**校验不过不要重启**。\n"
            "反方向的操作（把设备上的文件取回本地）用 `nxc_fetch`。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "local_path": {"type": "string", "description": "本地文件路径"},
                "remote_path": {
                    "type": "string",
                    "description": "设备上的目标路径（绝对路径，含 .. 会被拒）",
                },
                "confirm": {
                    "type": "string",
                    "description": "白名单以外需要：I-KNOW-THIS-TOUCHES-OUTSIDE-ROOTS",
                },
                "verify": {"type": "boolean", "description": "写完读回比对 sha256，默认 true"},
            },
            "required": ["local_path", "remote_path"],
            "additionalProperties": False,
        },
    },
]


# ---------------------------------------------------------------- 工具实现
def _split_params(text: str) -> list[dict]:
    """把设备侧压成一行参数摘要（`path*(str),data*(base64),append(0|1=0)`）拆成结构化表。

    ★ 这是**从设备读来的**，不是本文件写死的 —— 设备加参数，这里自动就有。
    """
    out: list[dict] = []
    for part in filter(None, (p.strip() for p in text.split(","))):
        name, _, rest = part.partition("(")
        body = rest.rstrip(")")
        ptype, _, pdef = body.partition("=")
        required = name.endswith("*")
        out.append({
            "name": name.rstrip("*"),
            "type": ptype,
            "required": required,
            "default": pdef or None,
        })
    return out


def tool_caps(device: Device) -> dict:
    """设备身份 + **完整能力清单**（工具 / 方法 / 参数 / 风险 / 有没有教程）。

    ★ 2026-10-06 晚改成用设备侧的 `tools.list`：
      · **一次调用**就拿到全部工具 + 每个方法的使用摘要（原来是 `caps` + 14 次 `help`）；
      · 参数表、风险等级（read/write/action/danger）、是否有教程 —— 全部**来自设备自己**
        ⇒ 设备加工具/改方法，**本文件一个字都不用改**。
    """
    hello = device.call_one("hello")
    if "error" in hello:
        raise RuntimeError(f"设备无应答: {hello['error']['message']}")

    hello_payload = parse_payload(hello["lines"])
    identity = hello_payload[0] if hello_payload and isinstance(hello_payload[0], dict) else {}

    listing = device.call_one("tools.list")
    if "error" in listing:
        raise RuntimeError(f"tools.list 失败: {listing['error']['message']}")

    tools_out: list[dict] = []
    cur: dict | None = None
    for item in parse_payload(listing["lines"]):
        if not isinstance(item, dict):
            continue
        if "tool" in item:                       # 工具行
            cur = {
                "name": item["tool"],
                "help": item.get("help", ""),
                "methodCount": int(item.get("methods", 0) or 0),
                "hasToolDoc": item.get("tool_doc") == "1",
                "methods": [],
            }
            tools_out.append(cur)
        elif "method" in item and cur is not None:   # 方法行：method=<工具>.<方法>
            call = item["method"]
            short = call.split(".", 1)[1] if "." in call else call
            cur["methods"].append({
                "name": short,
                "call": call,                    # ★ 可直接喂给 nxc_call 的 method
                "help": item.get("help", ""),
                "risk": item.get("risk", ""),    # read / write / action / danger
                "params": _split_params(item.get("params", "")),
            })

    return {
        "identity": identity,
        "toolCount": len(tools_out),
        "methodCount": sum(t["methodCount"] for t in tools_out),
        "tools": tools_out,
        "howTo": ("用 nxc_call 的 method=<methods[].call> 执行；"
                  "risk=action/danger 的方法改动设备状态、通常要带 confirm 口令"
                  "（口令在报错消息里会给）；参数细节与完整教程："
                  "nxc_call method=tools.doc params={name: <call>}"),
    }


def tool_call(device: Device, arguments: dict) -> dict:
    raw = arguments.get("raw")
    if raw:
        command = str(raw).strip()
    else:
        method = arguments.get("method")
        if not method:
            raise RuntimeError("缺少 method 参数（或用 raw 直接发协议行）")
        params = arguments.get("params") or {}
        if not isinstance(params, dict):
            raise RuntimeError("params 必须是一个对象")
        parts = [str(method)]
        for key, value in params.items():
            # ★ 2026-10-05 改：这里原来是"值含空格/换行就抛错"，结果是
            #   **卡上带空格的路径根本无法表达**（`fs.ls` 列得出来、喂不回去）。
            #   现在改成自动转义（设备侧对称还原），参数值可以放心含空格。
            #   想发**原始协议行**（不要任何转义）请用 raw。
            parts.append(f"{key}={escape_value(str(value))}")
        command = " ".join(parts)

    result = device.call_one(command)
    if "error" in result:
        return {"command": command, "error": result["error"]}
    return {"command": command, "payload": parse_payload(result.get("lines", []))}


# ---------------------------------------------------------------- 自我更新
def tool_deploy(device: Device, arguments: dict) -> dict:
    """把本地文件分块写进设备，再读回校验。用来做平台的自我更新（不需要 FTP）。"""
    local = arguments.get("local_path")
    if not local:
        raise RuntimeError("缺少 local_path")
    source = Path(local)
    if not source.is_file():
        raise RuntimeError(f"本地文件不存在：{local}")

    remote = arguments.get("remote_path") or DEFAULT_REMOTE_MODULE
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()

    # ★ 自我更新**故意用保守分块**：它是"给可能还跑着旧版本的设备装新版本"的引导路径，
    #   而旧版本的参数上限可能是 512 —— 用大分块会被截断，把我们自己的模块写坏。
    commands = _build_write_commands(data, remote, arguments.get("confirm"),
                                     chunk=BOOTSTRAP_CHUNK)

    for start in range(0, len(commands), DEPLOY_BATCH):
        batch = commands[start:start + DEPLOY_BATCH]
        for result in device.call(batch):
            if "error" in result:
                raise RuntimeError(
                    f"写到约第 {start * BOOTSTRAP_CHUNK} 字节处失败：{result['error']['message']}"
                )

    out: dict = {
        "local": str(source),
        "remote": remote,
        "bytes": len(data),
        "sha256": digest,
        "chunks": len(commands),
    }

    # 尺寸校验（便宜，必做）
    stat = parse_payload(device.call_one(f"fs.stat path={escape_value(remote)}").get("lines", []))
    remote_size = None
    for record in stat:
        if isinstance(record, dict) and "size" in record:
            remote_size = int(record["size"])
    out["remote_size"] = remote_size
    if remote_size != len(data):
        out["size_mismatch"] = True

    # 深度校验：读回比对 sha256（默认开 —— 这是自我更新，值得几秒钟）
    if arguments.get("verify", True):
        # ★ 复用 read_remote_file：它按**实际收到的字节数**推进偏移，
        #   所以即使设备侧的单次读取上限与这里的期望不一致，读回也不会出现空洞。
        # ★ 2026-10-05 修：读回那一步也要带越界口令（与 tool_put 同一条理由）——
        # 否则 `remote_path` 指到白名单以外时，校验永远是 0 字节。
        collected = read_remote_file(device, remote, confirm=arguments.get("confirm"))
        out["readback_bytes"] = len(collected)
        out["readback_sha256"] = hashlib.sha256(collected).hexdigest()
        out["verified"] = out["readback_sha256"] == digest

    return out


def _build_write_commands(data: bytes, remote: str,
                          confirm: str | None = None,
                          chunk: int | None = None) -> list[str]:
    """把文件切成 fs.write 命令序列。

    第一块 append=0（截断重写），其余 append=1（追加）。
    confirm 会拼在**每一条**上 —— 因为设备侧是逐条做路径门禁的，
    只给第一条带口令会导致后面的块全被拒。
    """
    if chunk is None:
        chunk = DEPLOY_CHUNK
    suffix = f" confirm={confirm}" if confirm else ""
    # ★ 值要转义：协议用空格分列字段，路径里的空格必须先变 %20（设备侧会还原）。
    path_field = escape_value(remote)
    commands: list[str] = []
    for offset in range(0, len(data), chunk):
        piece = data[offset:offset + chunk]
        encoded = base64.b64encode(piece).decode("ascii")
        command = (f"fs.write path={path_field} data={encoded} "
                   f"append={0 if offset == 0 else 1}{suffix}")
        # ★ 先自己校验长度：撞上协议上限会被整条丢掉，而静默丢一条 = 文件少一块。
        if len(encoded) > ARG_VALUE_MAX:
            raise RuntimeError(
                f"分块编码后 {len(encoded)} 字符，超过协议单值上限 {ARG_VALUE_MAX}"
                f"（设备侧 kArgValueBytes=4096）；请调小 DEPLOY_CHUNK"
            )
        if len(command) >= PROTOCOL_LINE_MAX:
            raise RuntimeError(f"命令长 {len(command)}，超过请求行上限 {PROTOCOL_LINE_MAX}")
        commands.append(command)
    return commands


def tool_put(device: Device, arguments: dict) -> dict:
    """把本地文件写到设备上的**任意路径**（这就是 FTP 的替代品）。

    ★ 为什么要它：平台本来就有 `fs.write`，但 PC 侧只有 `nxc_deploy` —— 那个只能写平台自己的模块。
    于是"装个第三方 sysmodule"就只能去开 FTP（而 FTP 一启动游戏就没了）。
    这个工具把上传能力**通用化**了。

    白名单以外的路径要带 `confirm=I-KNOW-THIS-TOUCHES-OUTSIDE-ROOTS`
    —— 门禁在设备侧，这里只是把它透传下去。
    """
    local = arguments.get("local_path")
    remote = arguments.get("remote_path")
    if not local:
        raise RuntimeError("缺少 local_path")
    if not remote:
        raise RuntimeError("缺少 remote_path")
    source = Path(local)
    if not source.is_file():
        raise RuntimeError(f"本地文件不存在：{local}")

    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    verify = arguments.get("verify", True)

    def attempt(chunk: int) -> dict:
        commands = _build_write_commands(data, remote, arguments.get("confirm"), chunk=chunk)
        for start in range(0, len(commands), DEPLOY_BATCH):
            for result in device.call(commands[start:start + DEPLOY_BATCH]):
                if "error" in result:
                    raise RuntimeError(
                        f"写到约第 {start * chunk} 字节处失败：{result['error']['message']}"
                    )
        info: dict = {
            "local": str(source),
            "remote": remote,
            "bytes": len(data),
            "sha256": digest,
            "chunks": len(commands),
            "chunk_bytes": chunk,
        }
        if verify:
            collected = read_remote_file(device, remote, chunk=24000,
                                         confirm=arguments.get("confirm"))
            info["readback_bytes"] = len(collected)
            info["readback_sha256"] = hashlib.sha256(collected).hexdigest()
            info["verified"] = info["readback_sha256"] == digest
        return info

    # ★ 先试大分块；校验不过就退回保守分块重写一遍。
    #   为什么需要：对端可能是**参数上限 512 的旧版本** ——
    #   大分块会被静默截断，写出来的是坏文件。有了这个回退，工具对两端版本差异免疫。
    out = attempt(DEPLOY_CHUNK)
    if verify and not out.get("verified") and DEPLOY_CHUNK != BOOTSTRAP_CHUNK:
        nxc_note = "大分块校验不过（对端可能是旧版本），退回保守分块重写"
        out = attempt(BOOTSTRAP_CHUNK)
        out["fallback"] = nxc_note
    return out


# ---------------------------------------------------------------- 取回文件
def _read_once(device: Device, remote: str, offset: int, want: int,
               confirm: str | None = None) -> bytes | None:
    """读一块；返回实际拿到的字节（None 表示出错/到头）。

    ★ 2026-10-05 修：`fs.read` 与 `fs.write` **一样要过设备侧的路径门禁** —— 读**白名单以外**
    的路径也得带 `confirm=I-KNOW-THIS-TOUCHES-OUTSIDE-ROOTS`，否则设备回 `403 outside-safe-roots`。
    原来这里没带 ⇒ 往游戏 runtime 目录写完之后的"读回校验"**永远是 0 字节** ⇒ `verified=false`
    ⇒ 还会白触发一次 `tool_put` 的"退回保守分块**重写**"（把整个文件又写了一遍）。
    """
    suffix = f" confirm={confirm}" if confirm else ""
    result = device.call_one(f"fs.read path={escape_value(remote)} offset={offset} len={want}{suffix}")
    if "error" in result:
        return None
    payload = parse_payload(result.get("lines", []))
    b64 = next((p for p in payload if isinstance(p, str)), None)
    if b64 is None:
        return None
    return base64.b64decode(b64)


def read_remote_file(device: Device, remote: str, chunk: int = 24000, batch: int = 10,
                     confirm: str | None = None) -> bytes:
    """分块把设备上的文件读回来。

    ★★ 两条必须遵守的规矩（都是踩出来的）：

    1. **按实际收到的字节数推进偏移**，不能按"请求了多少"推进。
       踩过的坑：PC 侧把分块升到 24000，而设备上跑的还是旧版本（上限 3072）——
       设备每次只回 3072，PC 却按 24000 往前跳 ⇒ **跳着读、数据有洞**，校验直接失败。

    2. **只有"读回 0 字节"才算文件到头**，不能拿"少于请求量"当 EOF。
       踩过的坑：设备上限 3072 时，第一次只回 3072（< 请求的 24000），
       我把它当成读完了 ⇒ 152KB 的文件只取回 3072 字节。

    所以：先**探一次**问出设备实际能回多大一块，再用那个步长读，**一直读到空**为止。
    """
    read_suffix = f" confirm={confirm}" if confirm else ""
    path_field = escape_value(remote)
    first = _read_once(device, remote, 0, chunk, confirm)
    if not first:
        return b""
    step = len(first)               # 设备实际能回的大小（可能小于 chunk）
    data = bytearray(first)
    offset = step

    while True:
        commands = [
            f"fs.read path={path_field} offset={offset + i * step} len={step}{read_suffix}"
            for i in range(batch)
        ]
        progressed = False
        for result in device.call(commands):
            if "error" in result:
                return bytes(data)
            payload = parse_payload(result.get("lines", []))
            b64 = next((p for p in payload if isinstance(p, str)), None)
            if b64 is None:
                return bytes(data)
            piece = base64.b64decode(b64)
            if not piece:
                return bytes(data)   # ★ 只有这里才是真正的"到头了"
            data.extend(piece)
            offset += len(piece)
            progressed = True
        if not progressed:
            return bytes(data)


def tool_fetch(device: Device, arguments: dict) -> dict:
    """把设备上的文件取回本地磁盘。主要用途：把 screen.capture 出的截图拉回来。"""
    remote = arguments.get("remote_path")
    if not remote:
        raise RuntimeError("缺少 remote_path")

    local = arguments.get("local_path")
    if not local:
        local = Path(remote).name

    data = read_remote_file(device, remote)

    # ★★★ 2026-10-06：**不要把"读不到"一律说成"文件不存在或为空"。**
    #   我按原来那句话去查过"文件怎么丢了"，查了半天 —— 其实设备上文件好好的，
    #   是 MCP 那条长连接已经失效、第一条命令的响应压根没回来（见 Device._exchange 顶部注释）。
    #   所以这里先 `fs.stat` 问一句，把**三种完全不同的情况分开说**：
    #     ① 路径真的不在  ② 文件真的是 0 字节  ③ 文件有内容、却一个字节都没读回来（读通道问题）
    stat_records = parse_payload(
        device.call_one(f"fs.stat path={escape_value(remote)}").get("lines", []))
    remote_size: int | None = None
    remote_type: str | None = None
    for record in stat_records:
        if isinstance(record, dict) and "size" in record:
            remote_size = int(record["size"])
            remote_type = str(record.get("type", ""))

    if not data:
        if remote_size is None:
            raise RuntimeError(f"读不到内容，而且 fs.stat 也没给出大小 —— 设备上很可能真的没有这个路径：{remote}")
        if remote_type == "dir":
            raise RuntimeError(f"这个路径是【目录】不是文件，取不回来：{remote}")
        if remote_size == 0:
            raise RuntimeError(f"设备上这个文件确实存在，但它是 0 字节：{remote}")
        raise RuntimeError(
            f"★ 设备上这个文件有 {remote_size} 字节，却一个字节都没读回来 —— 这是【读通道】的问题"
            f"（长连接状态不对，常见于主机刚重启过），**不是文件不存在**。再调用一次通常就好：{remote}")

    if remote_size is not None and len(data) < remote_size:
        # ★ 静默返回半截文件比报错危险得多（会被当成完整副本去做校验/部署）。
        raise RuntimeError(
            f"只读到 {len(data)}/{remote_size} 字节，读取中途断了。"
            f"**不要把这份不完整的文件当完整副本用**，重试一次：{remote}")

    target = Path(local)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)

    return {
        "remote": remote,
        "local": str(target.resolve()),
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


# ---------------------------------------------------------------- JSON-RPC 层
def rpc_result(req_id, result) -> dict:
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def rpc_error(req_id, code: int, message: str) -> dict:
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


def text_content(text: str) -> dict:
    return {"content": [{"type": "text", "text": text}]}


def handle(request: dict, device: Device) -> dict | None:
    method = request.get("method")
    req_id = request.get("id")

    if method == "initialize":
        return rpc_result(req_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })

    if method in ("notifications/initialized", "initialized"):
        return None  # 通知，不回包

    if method == "ping":
        return rpc_result(req_id, {})

    if method == "tools/list":
        return rpc_result(req_id, {"tools": TOOLS})

    if method == "tools/call":
        params = request.get("params") or {}
        name = params.get("name")
        arguments = params.get("arguments") or {}
        try:
            if name == "nxc_caps":
                payload = tool_caps(device)
            elif name == "nxc_call":
                payload = tool_call(device, arguments)
            elif name == "nxc_gdb":
                # 懒加载：缺少这个模块时不该把整个 MCP 服务器带崩。
                import nxc_gdb  # noqa: PLC0415
                payload = nxc_gdb.handle(str(arguments.get("action", "")), arguments)
            elif name == "nxc_deploy":
                payload = tool_deploy(device, arguments)
            elif name == "nxc_fetch":
                payload = tool_fetch(device, arguments)
            elif name == "nxc_put":
                payload = tool_put(device, arguments)
            else:
                return rpc_result(req_id, {
                    "content": [{"type": "text", "text": f"未知工具：{name}"}],
                    "isError": True,
                })
        except Exception as exc:  # noqa: BLE001 - 任何失败都要变成可读的工具结果，不能弄崩服务器
            log("工具执行失败:", traceback.format_exc().replace("\n", " | "))
            return rpc_result(req_id, {
                "content": [{"type": "text", "text": f"执行失败：{type(exc).__name__}: {exc}"}],
                "isError": True,
            })

        is_error = isinstance(payload, dict) and "error" in payload
        text = json.dumps(payload, ensure_ascii=False, indent=2)
        reply = text_content(text)
        if is_error:
            reply["isError"] = True
        return rpc_result(req_id, reply)

    if req_id is None:
        return None  # 其它通知，忽略
    return rpc_error(req_id, -32601, f"Method not found: {method}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ip", default=DEFAULT_IP)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    device = Device(args.ip, args.port, args.timeout)
    log(f"启动，目标设备 {args.ip}:{args.port}")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            log("收到非 JSON 行，已忽略:", line[:120])
            continue

        try:
            response = handle(request, device)
        except Exception:  # noqa: BLE001
            log("处理请求时异常:", traceback.format_exc().replace("\n", " | "))
            response = rpc_error(request.get("id"), -32603, "internal error")

        if response is not None:
            sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
