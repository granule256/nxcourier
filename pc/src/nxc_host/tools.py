# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""MCP 工具清单与实现；连接生命周期由统一主机后台管理。"""
from __future__ import annotations
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import traceback
import uuid
from pathlib import Path, PurePosixPath
from . import __version__
from .device import Device, escape_value, unescape_value, parse_payload, log, DEFAULT_IP, DEFAULT_PORT

SERVER_VERSION = __version__
PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "nxc-platform"



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
            "使用统一PC后台管理的完整GDB会话，CLI、Web与MCP共享同一会话。"
            "优先本机GDB，自动模式可选择已经存在的Docker镜像；无需手动开服务，不自动安装依赖。"
            "open启动并可连接调试桩，attach明确附加指定pid（省略时由后台查询当前应用），目标可能暂停。"
            "run执行命令，continue可立即返回running；status查看异步停止事件。"
            "interrupt中断目标，detach释放目标，close尽力detach并回收GDB子进程。"
            "GDB连接或执行结果未知期间后台拒绝独立mem.*以避免资源争用。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["open", "connect", "attach", "run", "interrupt", "pause", "detach", "close", "status", "doctor"],
                    "description": "open/connect建立会话；attach附加应用；run执行；interrupt暂停；detach释放；close关闭；status状态；doctor后端诊断",
                },
                "pid": {"type": "integer", "minimum": 1, "description": "attach的目标进程；省略由后台查询当前应用"},
                "commands": {
                    "type": ["string", "array"],
                    "items": {"type": "string"},
                    "description": "action=run 时要执行的 gdb 命令，字符串（按行拆）或字符串数组",
                },
                "connect": {
                    "type": "boolean",
                    "description": "action=open 时是否连接设备调试桩，默认true；连接与附加是不同状态",
                },
                "timeout": {
                    "type": "number",
                    "description": "命令等待秒数，默认60；continue返回运行状态，停止事件随后由status读取",
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
            "直接由本工具暂存到唯一临时文件，完整读回校验后备份旧文件并重命名提交。\n"
            "写进去**不会立即生效** —— 平台是开机时加载的，需要重启（可用 nxc_call 的 power.reboot）。\n"
            "★★ 默认只允许写平台自己的模块文件；要写别的路径必须显式给 remote_path，"
            "目标必须是设备SD卡绝对路径。写坏模块的后果：下次开机加载失败，拔卡删目录即可复原。"
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
                    "description": "必须为 true；发布前完整读回比对 sha256，不能关闭",
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
                "remote_path": {"type": "string", "description": "设备SD卡绝对文件路径"},
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
            "目标必须是设备SD卡绝对路径，当前设备不使用路径白名单。\n"
            "暂存并完整读回比对 sha256，备份旧文件后重命名提交；校验不能关闭。\n"
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
                    "description": "兼容旧调用方的可选确认值；当前设备允许 SD 卡绝对路径",
                },
                "verify": {"type": "boolean", "description": "必须为 true；发布前完整读回校验不能关闭"},
            },
            "required": ["local_path", "remote_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "nxc_see",
        "description": (
            "★ 看一眼主机现在的画面 —— 图片**直接返回给你**，不需要你再自己去打开文件。\n"
            "内部一次做完：抓帧 → 从 SD 卡分块取回 → 降采样 → 作为图片交给你。\n"
            "★ 一次调用 = 一轮推理。过去要三步（screen.capture → nxc_fetch → 打开本地文件）= 三轮。\n"
            "参数（都可省）：\n"
            "  layer      = 抓哪一层，默认 lcd（LCD 上实际显示的）。\n"
            "  max_width  = 交付前缩到多宽，默认 640。缩到 640 宽时图片 token 约为 720p 的 1/4。\n"
            "  timeout_ms = 单次抓帧超时毫秒，默认 5000。\n"
            "★ 想「操作一下再看结果」请用 nxc_step —— 它把动和看并成一次调用。\n"
            "★ 连不上时先想「主机是不是睡着了」（静置约 148 秒会自动睡、且不会自己醒），请人按一下电源键。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "layer": {"type": "string", "description": "lcd|default|screenshot|recording|lastframe，默认 lcd"},
                "max_width": {"type": "integer", "description": "交付前缩到多宽（像素），默认 640；给 0 表示不缩"},
                "timeout_ms": {"type": "integer", "description": "单次抓帧超时毫秒，默认 5000"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "nxc_step",
        "description": (
            "★★ 一步一往返：按一串键 → 等画面反应 → 抓图 → **把「动作之后」的画面直接返回给你**。\n"
            "这是「边看边操作」的基本单元。一次调用替代原来的四步"
            "（input.seq + wait_stable/wait_change + screen.capture + nxc_fetch）。\n"
            "参数：\n"
            "  steps      = 必填。动作串，与设备侧 input.seq 同一套写法：\n"
            "               名字:毫秒;名字:毫秒…（分号分隔；WAIT:n 是纯等待）\n"
            "               例：\"DRIGHT:150;A:120\"、\"A:120;WAIT:300;A:120\"\n"
            "               可用键：A B X Y L R ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT LSTICK RSTICK HOME CAPTURE\n"
            "  wait       = stable（默认，等画面停住 ⇒ 判断「加载完了没」）\n"
            "               | change（画面一变就回 ⇒ 适合一直在动的游戏画面）\n"
            "               | none（不等，按完立刻抓）\n"
            "  timeout_ms = 等反应的最长毫秒，默认 10000。\n"
            "  layer / max_width 同 nxc_see。\n"
            "★ 动画循环的界面（例如游戏内）永远不会「稳定」⇒ 那种场合用 wait=change，别用默认的 stable。\n"
            "★ 属于会动主机的操作（真的注入按键/摇杆）。"
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "steps": {"type": "string", "description": "动作串，形如 DRIGHT:150;A:120"},
                "wait": {
                    "type": "string",
                    "enum": ["stable", "change", "none"],
                    "description": "等哪种反应，默认 stable",
                },
                "timeout_ms": {"type": "integer", "description": "等反应的最长毫秒，默认 10000"},
                "layer": {"type": "string", "description": "lcd|default|screenshot|recording|lastframe，默认 lcd"},
                "max_width": {"type": "integer", "description": "交付前缩到多宽，默认 640"},
            },
            "required": ["steps"],
            "additionalProperties": False,
        },
    },
]

# 文件工具由共享后台选择传输通道；直接协议工具仍保持原有语义。
for _entry in TOOLS:
    if _entry['name'] in ('nxc_put', 'nxc_fetch', 'nxc_deploy'):
        _entry['inputSchema']['properties']['transport'] = {
            'type': 'string', 'enum': ['auto', 'fs', 'ftp'],
            'description': '默认auto在传输前优先探测FTP；可指定fs或ftp，开始后失败不切换通道重放',
        }
        _entry['description'] += '\n统一后台支持 transport=auto|fs|ftp，默认自动优先FTP（设备需运行FTPD）；fs提交需要新版设备接口。'


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
    hello = device.call_one("hello", retry_safe=True)
    if "error" in hello:
        raise RuntimeError(f"设备无应答: {hello['error']['message']}")

    hello_payload = parse_payload(hello["lines"])
    identity = hello_payload[0] if hello_payload and isinstance(hello_payload[0], dict) else {}

    listing = device.call_one("tools.list", retry_safe=True)
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
        if any(c in command for c in "\r\n\x00"):
            raise RuntimeError("raw必须是单行协议命令")
    else:
        method = arguments.get("method")
        if not method:
            raise RuntimeError("缺少 method 参数（或用 raw 直接发协议行）")
        params = arguments.get("params") or {}
        if not isinstance(params, dict):
            raise RuntimeError("params 必须是一个对象")
        command = _device_line(str(method), params)

    result = device.call_one(command)
    if "error" in result:
        return {"command": command, "error": result["error"]}
    return {"command": command, "payload": parse_payload(result.get("lines", []))}


# ---------------------------------------------------------------- 自我更新
def tool_deploy(device: Device, arguments: dict) -> dict:
    """保守分块暂存、完整校验、备份后发布；旧设备不支持时拒绝写入。"""
    return _publish_file(device, arguments, BOOTSTRAP_CHUNK,
                         arguments.get("remote_path") or DEFAULT_REMOTE_MODULE)


def _checked(result: dict, operation: str) -> dict:
    if "error" in result:
        error = result["error"]
        raise RuntimeError(f"{operation}: {error.get('code')} {error.get('message')}")
    return result


def _remote_stat(device: Device, remote: str, confirm: str | None = None,
                 missing_ok: bool = False) -> dict | None:
    result = device.call_one(_device_line("fs.stat", {"path": remote, "confirm": confirm}),
                             retry_safe=True)
    if missing_ok and result.get("error", {}).get("code") == 404:
        return None
    _checked(result, f"fs.stat {remote}")
    for row in parse_payload(result.get("lines", [])):
        if isinstance(row, dict) and "size" in row:
            return row
    raise RuntimeError(f"fs.stat 缺少文件大小: {remote}")


def _fs_methods(device: Device) -> dict:
    listing = _checked(device.call_one("tools.list tool=fs", retry_safe=True), "tools.list tool=fs")
    return {row["method"]: row for row in parse_payload(listing.get("lines", []))
            if isinstance(row, dict) and "method" in row}


def _transfer_info(device: Device, methods: dict | None = None) -> dict | None:
    """只探测明确声明的新接口；旧设备或旧替身不会收到未知命令。"""
    methods = _fs_methods(device) if methods is None else methods
    if "fs.transfer_info" not in methods:
        return None
    if "fs.sha256" not in methods:
        raise RuntimeError("设备传输能力声明缺少 fs.sha256")
    result = _checked(device.call_one("fs.transfer_info", retry_safe=True), "传输能力协商")
    fields = _strict_fields(result, "传输能力")
    limits = {"write_chunk": DEPLOY_CHUNK, "read_chunk": 24000, "max_inflight": 2, "sha256": 1}
    info = {}
    for key, limit in limits.items():
        value = fields.get(key, "")
        if not re.fullmatch(r"[0-9]+", value) or not 1 <= int(value) <= limit:
            raise RuntimeError(f"设备传输能力 {key} 非法或缺失")
        info[key] = int(value)
    return info


def _strict_fields(result: dict, operation: str) -> dict:
    fields = {}
    for line in result.get("lines", []):
        for field in line.split():
            key, sep, value = field.partition("=")
            if not sep or key in fields:
                raise RuntimeError(f"{operation}响应字段损坏或重复")
            fields[key] = value
    return fields


def _iter_remote_file(device: Device, remote: str, *, confirm=None, info=None,
                      size: int | None = None, chunk: int = 24000):
    """逐块产出；只有协商成功才流水，且不预发超过stat声明的长度。"""
    if chunk <= 0:
        raise RuntimeError("读取分块必须大于 0")
    if info is None:
        offset = 0
        while True:
            piece = _read_once(device, remote, offset, chunk, confirm)
            if not piece:
                return
            if len(piece) > chunk:
                raise RuntimeError("fs.read返回长度超过请求")
            yield piece
            offset += len(piece)
    else:
        if size is None or size < 0:
            raise RuntimeError("流水读取需要有效的文件大小")
        chunk = min(chunk, info["read_chunk"])
        offset = 0
        while offset < size:
            requests = []
            for _ in range(info["max_inflight"]):
                if offset >= size:
                    break
                want = min(chunk, size - offset)
                requests.append((offset, want, _device_line("fs.read", {
                    "path": remote, "offset": offset, "len": want, "confirm": confirm})))
                offset += want
            results = device.call_windowed([r[2] for r in requests], window=info["max_inflight"])
            if len(results) != len(requests):
                raise RuntimeError("fs.read流水应答数量不匹配")
            for result, (start, want, _) in zip(results, requests):
                piece = _read_piece(result, start)
                if len(piece) != want:
                    raise RuntimeError(f"fs.read offset={start} 读取长度不符：{len(piece)}/{want}")
                yield piece


def _remote_file_hash(device: Device, remote: str, confirm, info: dict | None) -> dict:
    if info is not None:
        result = _checked(device.call_one(_device_line("fs.sha256", {"path": remote, "confirm": confirm}),
                                          retry_safe=True), "远程摘要")
        fields = _strict_fields(result, "远程摘要")
        if (not re.fullmatch(r"[0-9]+", fields.get("bytes", "")) or
                not re.fullmatch(r"[0-9a-fA-F]{64}", fields.get("sha256", ""))):
            raise RuntimeError("远程摘要响应长度或SHA256无效")
        return {"bytes": int(fields["bytes"]), "sha256": fields["sha256"].lower()}
    digest = hashlib.sha256()
    count = 0
    for piece in _iter_remote_file(device, remote, confirm=confirm):
        digest.update(piece)
        count += len(piece)
    return {"bytes": count, "sha256": digest.hexdigest()}


def remote_file_hash(device: Device, remote: str, confirm: str | None = None) -> dict:
    """新设备流式设备摘要；旧设备分块读回计算，不保存完整文件。"""
    return _remote_file_hash(device, remote, confirm, _transfer_info(device))


def _publish_file(device: Device, arguments: dict, chunk: int, remote: str | None) -> dict:
    local = arguments.get("local_path")
    if not local or not remote:
        raise RuntimeError("缺少 local_path 或 remote_path")
    source = Path(local)
    if not source.is_file():
        raise RuntimeError(f"本地文件不存在：{local}")
    if arguments.get("verify", True) is not True:
        raise RuntimeError("发布前必须完整读回校验，verify=false 不受支持")
    source_size = source.stat().st_size
    confirm = arguments.get("confirm")
    methods = _fs_methods(device)
    rename_params = _split_params(methods.get("fs.rename", {}).get("params", ""))
    if not {"src", "dst", "replace", "rollback"}.issubset({p["name"] for p in rename_params}):
        raise RuntimeError("设备缺少 fs.rename replace/rollback 提交能力；请先通过离线方式更新设备")
    write_params = _split_params(methods.get("fs.write", {}).get("params", ""))
    if not source_size and "allow_empty" not in {p["name"] for p in write_params}:
        raise RuntimeError("设备 fs.write 缺少 allow_empty 空文件支持；拒绝上传")
    info = _transfer_info(device, methods)
    if info is not None:
        chunk = min(chunk, info["write_chunk"])
    window = info["max_inflight"] if info else 1
    token = uuid.uuid4().hex
    parent = PurePosixPath(remote).parent
    stage = str(parent / f".nxc-stage-{token}")
    backup = str(parent / f".nxc-backup-{token}")
    rollback = str(parent / f".nxc-rollback-{token}")
    old_stat = _remote_stat(device, remote, confirm, missing_ok=True)
    if old_stat is not None and old_stat.get("type") != "file":
        raise RuntimeError(f"上传目标不是文件：{remote}")
    digest = hashlib.sha256()
    count = chunks = 0
    try:
        with source.open("rb") as stream:
            done = False
            while not done:
                commands, lengths = [], []
                for _ in range(window):
                    piece = stream.read(chunk)
                    if not piece and chunks:
                        done = True
                        break
                    if not piece:
                        done = True
                    commands.append(_write_command(piece, stage, confirm, first=chunks == 0))
                    lengths.append(len(piece))
                    digest.update(piece)
                    count += len(piece)
                    chunks += 1
                    if done:
                        break
                if not commands:
                    break
                results = (device.call_windowed(commands, window=window) if info else device.call(commands))
                if len(results) != len(commands):
                    raise RuntimeError("写入暂存文件应答数量不匹配")
                for result, length in zip(results, lengths):
                    _checked(result, f"写入暂存文件 {stage}")
                    fields = _strict_fields(result, "写入暂存文件")
                    if fields.get("bytes") != str(length):
                        raise RuntimeError("写入暂存文件确认字节数不匹配")
        if count != source_size:
            raise RuntimeError("本地文件在上传期间大小变化，取消发布")
        expected = {"bytes": count, "sha256": digest.hexdigest()}
        collected = _remote_file_hash(device, stage, confirm, info)
        if collected != expected:
            raise RuntimeError("暂存文件完整校验失败，目标尚未修改")
        if old_stat is not None:
            original = _remote_file_hash(device, remote, confirm, info)
            if original["bytes"] != int(old_stat["size"]):
                raise RuntimeError("原目标读回长度不匹配，取消发布")
            _checked(device.call_one(_device_line("fs.copy", {"src": remote, "dst": backup,
                                                             "confirm": confirm})), "备份旧目标")
            saved = _remote_file_hash(device, backup, confirm, info)
            if saved != original:
                raise RuntimeError("旧目标备份校验失败，取消发布")
        _checked(device.call_one(_device_line("fs.rename", {"src": stage, "dst": remote,
                                                           "replace": 1, "confirm": confirm,
                                                           "rollback": rollback if old_stat is not None else None})),
                 f"提交暂存文件 {stage} -> {remote}")
    except Exception as exc:
        raise RuntimeError(
            f"发布失败：{exc}；目标路径：{remote}；暂存路径：{stage}；"
            f"备份路径：{backup if old_stat is not None else '无（原目标不存在）'}；"
            f"回滚路径：{rollback}"
            "。辅助文件可能尚未创建或已提交，请核实后清理") from exc
    return {"local": str(source), "remote": remote, "bytes": count, "sha256": expected["sha256"],
            "chunks": chunks, "chunk_bytes": chunk, "remote_size": collected["bytes"],
            "readback_bytes": collected["bytes"], "readback_sha256": collected["sha256"], "verified": True,
            "backup": backup if old_stat is not None else None,
            "rollback": rollback if old_stat is not None else None}


def _write_command(piece: bytes, remote: str, confirm=None, *, first: bool) -> str:
    encoded = base64.b64encode(piece).decode("ascii")
    command = _device_line("fs.write", {"path": remote, "data": encoded,
                                        "append": 0 if first else 1, "confirm": confirm,
                                        "allow_empty": 1 if not piece else None})
    if len(encoded) > ARG_VALUE_MAX:
        raise RuntimeError(f"分块编码后 {len(encoded)} 字符，超过协议单值上限 {ARG_VALUE_MAX}")
    if len(command.encode("utf-8")) >= PROTOCOL_LINE_MAX:
        raise RuntimeError(f"命令超过请求行上限 {PROTOCOL_LINE_MAX}")
    return command


def _build_write_commands(data: bytes, remote: str, confirm: str | None = None,
                          chunk: int | None = None) -> list[str]:
    """兼容旧调用的bytes辅助函数；真实文件上传使用有界流式分块。"""
    chunk = DEPLOY_CHUNK if chunk is None else chunk
    if chunk <= 0:
        raise RuntimeError("写入分块必须大于0")
    return [_write_command(data[offset:offset + chunk], remote, confirm, first=offset == 0)
            for offset in range(0, max(1, len(data)), chunk)]


def tool_put(device: Device, arguments: dict) -> dict:
    """通用上传与模块部署使用同一套暂存、校验及备份提交语义。"""
    return _publish_file(device, arguments, DEPLOY_CHUNK, arguments.get("remote_path"))


# ---------------------------------------------------------------- 取回文件
def _read_piece(result: dict, offset: int) -> bytes:
    _checked(result, f"fs.read offset={offset}")
    payload = parse_payload(result.get("lines", []))
    encoded = next((p for p in payload if isinstance(p, str)), None)
    if encoded is None:
        raise RuntimeError(f"fs.read offset={offset} 缺少 base64 正文，不能当作 EOF")
    try:
        data = base64.b64decode(encoded, validate=True)
    except ValueError as exc:
        raise RuntimeError(f"fs.read offset={offset} base64 正文损坏") from exc
    counts = [row["bytes"] for row in payload if isinstance(row, dict) and "bytes" in row]
    if len(counts) != 1 or not re.fullmatch(r"[0-9]+", counts[0]):
        raise RuntimeError(f"fs.read offset={offset} 缺少或损坏字节数，不能当作 EOF")
    if int(counts[0]) != len(data):
        raise RuntimeError(f"fs.read offset={offset} 声明长度与正文不符")
    return data


def _read_once(device: Device, remote: str, offset: int, want: int,
               confirm: str | None = None) -> bytes:
    command = _device_line("fs.read", {"path": remote, "offset": offset, "len": want,
                                       "confirm": confirm})
    return _read_piece(device.call_one(command, retry_safe=True), offset)


def read_remote_file(device: Device, remote: str, chunk: int = 24000, batch: int = 10,
                     confirm: str | None = None) -> bytes:
    """按实际字节数推进；只有明确的空正文代表 EOF，设备 ERR 原样上抛。

    逐条读取避免预发超出 EOF 的命令，也兼容设备缩小实际读取上限。
    batch 保留为兼容参数，状态未知时不返回已取得的半文件。
    """
    if chunk <= 0:
        raise RuntimeError("读取分块必须大于 0")
    data = bytearray()
    while True:
        piece = _read_once(device, remote, len(data), chunk, confirm)
        if not piece:
            return bytes(data)
        data.extend(piece)


def tool_fetch(device: Device, arguments: dict) -> dict:
    """流式写同目录临时文件；完整传输和长度验证后才替换本地目标。"""
    remote = arguments.get("remote_path")
    if not remote:
        raise RuntimeError("缺少 remote_path")
    target = Path(arguments.get("local_path") or Path(remote).name)
    confirm = arguments.get("confirm")
    info = _transfer_info(device)
    record = _remote_stat(device, remote, confirm)
    if record.get("type") != "file":
        raise RuntimeError(f"目标是目录不是文件：{remote}")
    size_text = record.get("size", "")
    if not re.fullmatch(r"[0-9]+", size_text):
        raise RuntimeError("远程文件大小无效")
    size = int(size_text)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    count = 0
    digest = hashlib.sha256()
    try:
        with tempfile.NamedTemporaryFile(mode="wb", dir=target.parent, prefix=".nxc-fetch-",
                                         delete=False) as stream:
            temporary = Path(stream.name)
            for piece in _iter_remote_file(device, remote, confirm=confirm, info=info, size=size):
                if count + len(piece) > size:
                    raise RuntimeError("读取长度不符：远程文件超过声明大小；不保存半文件")
                stream.write(piece)
                digest.update(piece)
                count += len(piece)
        if count != size:
            raise RuntimeError(f"读取长度不符：{count}/{size} 字节；不保存半文件：{remote}")
        result = {"bytes": count, "sha256": digest.hexdigest()}
        if info is not None and _remote_file_hash(device, remote, confirm, info) != result:
            raise RuntimeError("下载摘要校验失败；不替换本地文件")
        temporary.replace(target)
        return {"remote": remote, "local": str(target.resolve()), **result}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


# ---------------------------------------------------------------- 画面内联（让模型"真的看见")
#
# ★ 这一节解决的是「看一眼要花几轮推理」的问题，不是「画面传得够不够快」。
#
#   原来 AI 要看画面必须走三步：nxc_call screen.capture → nxc_fetch → 客户端再打开本地文件。
#   每一步都是一轮模型推理。而实测（OSWorld-Human，arXiv:2506.16042）表明：这类循环里
#   **模型推理占 75%~94% 的总延迟，截图捕获与动作执行合计不到 3%**
#   ⇒ 该省的是「轮次」，不是「把管道做快」（把管道做快反而可能让 agent 重复看、重复动作）。
#
#   所以这里把「抓帧 → 取回 → 交付」合成**一次**调用，并把图**直接**放进 MCP 的 content 数组
#   （`{"type":"image","data":<base64>,"mimeType":...}` —— MCP 官方规范里的标准 content block）。
#
# ★ 为什么仍然要经 SD 卡中转：设备侧单条应答上限是 `kReplyBytes = 32768` 字节，
#   而一帧 720p JPEG 约 188KB ⇒ 塞不进应答，只能落卡再分块读回。
#   （分块读回本身不慢：实测单次 `fs.read` 往返约 11.8ms，整帧约 80ms。）

IMAGE_MAX_WIDTH_DEFAULT = 640
SCREEN_REMOTE_PATH = "/config/nxc/screen.jpg"


class Content(list):
    """标记：这个返回值**本身**就是 MCP 的 content 数组，不要再被包成一段 text。

    用 list 子类而不是普通 list —— 这样能跟"某个工具恰好返回了一个 list"区分开。
    """


def image_block(jpeg: bytes, mime: str = "image/jpeg") -> dict:
    """一张图 → MCP 的 image content block。"""
    return {
        "type": "image",
        "data": base64.b64encode(jpeg).decode("ascii"),
        "mimeType": mime,
    }


def _downscale(jpeg: bytes, max_width: int) -> tuple[bytes, str]:
    """把一帧缩到 max_width 宽。返回 (字节, 走的哪条路)。

    ★ 为什么要缩：Anthropic 的图片 token 算法是 ⌈w/28⌉×⌈h/28⌉ ≈ w×h/750
      ⇒ 1280×720 ≈ 1229 token，640×360 ≈ 307 token（约 1/4）。
      官方文档也明说 "prefer resizing images before uploading them"。
    ★ 三条路，依次尝试：
      ① Pillow（若这个解释器装了）—— 快，还能控 JPEG 质量；
      ② macOS 自带的 `sips` —— 不装任何东西就能用，实测一帧约 40ms；
      ③ 都不行就原样返回，并在文本里如实说明（宁可显式降级，也不悄悄假装缩过）。
    """
    if max_width <= 0:
        return jpeg, "disabled(未缩放)"

    try:
        from PIL import Image  # 局部导入：没装也不该影响其它工具
    except ImportError:
        Image = None  # type: ignore[assignment]

    if Image is not None:
        try:
            import io

            with Image.open(io.BytesIO(jpeg)) as im:
                if im.width <= max_width:
                    return jpeg, "pillow(本来就不宽)"
                height = max(1, round(im.height * max_width / im.width))
                resized = im.convert("RGB").resize((max_width, height), Image.LANCZOS)
                buf = io.BytesIO()
                resized.save(buf, format="JPEG", quality=72, optimize=True)
                return buf.getvalue(), f"pillow({max_width}px)"
        except Exception:  # noqa: BLE001 - 退到 sips，不要因为这个把工具弄崩
            pass

    sips = shutil.which("sips")
    if sips:
        try:
            with tempfile.TemporaryDirectory(prefix="nxc-img-") as tmp:
                src = Path(tmp) / "in.jpg"
                dst = Path(tmp) / "out.jpg"
                src.write_bytes(jpeg)
                proc = subprocess.run(
                    [sips, "-Z", str(max_width), "-s", "formatOptions", "normal",
                     str(src), "--out", str(dst)],
                    capture_output=True, timeout=10,
                )
                if proc.returncode == 0 and dst.is_file() and dst.stat().st_size > 0:
                    return dst.read_bytes(), f"sips({max_width}px)"
        except Exception:  # noqa: BLE001
            pass

    return jpeg, "none(没装 Pillow 也没有 sips，未缩放)"


def _device_line(method: str, params: dict) -> str:
    """拼一条协议行（与 tool_call 同一套转义规则）。值为 None 的参数直接省略。"""
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?", method):
        raise RuntimeError("method 包含非法字符")
    if any(not isinstance(key, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key)
           for key in params):
        raise RuntimeError("参数键包含非法字符")
    parts = [method]
    for key, value in params.items():
        if value is None:
            continue
        parts.append(f"{key}={escape_value(str(value))}")
    return " ".join(parts)


def _grab_frame(device: Device, layer: str | None, timeout_ms: int,
                remote_path: str) -> tuple[bytes | None, list[str]]:
    """抓一帧并拉回本地。返回 (JPEG 字节 或 None, 过程记事)。"""
    notes: list[str] = []
    params: dict = {"path": remote_path}
    if layer:
        params["layer"] = layer
    if timeout_ms:
        params["timeout_ms"] = timeout_ms

    captured = device.call_one(_device_line("screen.capture", params))
    if "error" in captured:
        err = captured["error"]
        notes.append(f"screen.capture 失败：{err.get('code')} {err.get('message')}")
        return None, notes

    payload = parse_payload(captured.get("lines", []))
    row = payload[0] if payload and isinstance(payload[0], dict) else {}
    notes.append(f"抓帧 {row.get('bytes', '?')} 字节 via={row.get('via', '?')} magic={row.get('magic', '?')}")

    jpeg = read_remote_file(device, remote_path)
    if "bytes" not in row or int(row["bytes"]) != len(jpeg):
        raise RuntimeError(f"抓帧读回长度不符：声明 {row.get('bytes', '?')}，实际 {len(jpeg)} 字节")
    if not jpeg:
        notes.append("从设备取回 0 字节（读通道问题，再试一次通常就好）")
        return None, notes
    return jpeg, notes


_DEVICE_ASLEEP_HINT = (
    "请核对主机是否睡眠、IP是否变化，以及是否仍连接同一路由器。"
    "游戏本地无线模式可能使路由器 Wi-Fi 暂时不可达；不能仅凭端口超时判断模块或游戏卡死。"
    "若确认睡眠，请按电源键唤醒后重连。"
)


def tool_see(device: Device, arguments: dict):
    """★ 看一眼主机现在的画面 —— 图片直接返回给模型。

    一次调用顶过去三步（screen.capture → nxc_fetch → 客户端打开本地文件）。
    """
    layer = arguments.get("layer")
    timeout_ms = int(arguments.get("timeout_ms") or 5000)
    max_width = arguments.get("max_width")
    max_width = IMAGE_MAX_WIDTH_DEFAULT if max_width is None else int(max_width)
    remote_path = arguments.get("path") or SCREEN_REMOTE_PATH

    jpeg, notes = _grab_frame(device, layer, timeout_ms, remote_path)
    if jpeg is None:
        return {"error": "capture-failed", "notes": notes, "hint": _DEVICE_ASLEEP_HINT}

    small, via = _downscale(jpeg, max_width)
    notes.append(f"交付尺寸：{via}，{len(jpeg)} → {len(small)} 字节")

    return Content([
        {"type": "text", "text": "主机画面（下面这张图就是当前屏幕）：\n" + "\n".join(notes)},
        image_block(small),
    ])


def tool_step(device: Device, arguments: dict):
    """★★ 一步一往返：按一串键 → 等画面反应 → 抓图 → 把「动作之后」的画面交给模型。

    这是「边看边操作」的基本单元。一次调用替代原来的四步：
    input.seq + screen.wait_stable/wait_change + screen.capture + nxc_fetch。
    """
    steps = arguments.get("steps")
    if not steps:
        raise RuntimeError(
            "缺少 steps。动作串写法与设备侧 input.seq 相同："
            "名字:毫秒;名字:毫秒…（分号分隔，WAIT:n 是纯等待），例如 DRIGHT:150;A:120")

    wait = str(arguments.get("wait") or "stable")
    if wait not in ("stable", "change", "none"):
        raise RuntimeError("wait 只能是 stable / change / none")

    timeout_ms = int(arguments.get("timeout_ms") or 10000)
    layer = arguments.get("layer")
    max_width = arguments.get("max_width")
    max_width = IMAGE_MAX_WIDTH_DEFAULT if max_width is None else int(max_width)

    notes: list[str] = []

    # ① 动作
    act = device.call_one(_device_line("input.seq", {"steps": steps}))
    if "error" in act:
        err = act["error"]
        return {
            "error": "input.seq-failed",
            "code": err.get("code"),
            "message": err.get("message"),
            "hint": "动作串里出现不认识的键名时会回错。可用 nxc_call 调 input.status 看可用键名。",
        }
    act_payload = parse_payload(act.get("lines", []))
    arow = act_payload[0] if act_payload and isinstance(act_payload[0], dict) else {}
    notes.append(f"动作已下发：steps={arow.get('steps', '?')} elapsed_ms={arow.get('elapsed_ms', '?')}")

    # ② 等画面反应（设备端连着抓帧比对，不是盲等固定秒数）
    if wait != "none":
        method = "screen.wait_stable" if wait == "stable" else "screen.wait_change"
        waited = device.call_one(_device_line(method, {"timeout_ms": timeout_ms, "layer": layer}))
        if "error" in waited:
            notes.append(f"{method} 失败：{waited['error'].get('message')}")
        else:
            wp = parse_payload(waited.get("lines", []))
            wrow = wp[0] if wp and isinstance(wp[0], dict) else {}
            if wait == "stable":
                verdict = ("画面已停住（加载/过渡结束）" if wrow.get("stable") == "1"
                           else f"超时前画面一直没停（{timeout_ms}ms —— 动画界面永远不会稳定，这种场合请用 wait=change）")
            else:
                verdict = ("画面确实变了" if wrow.get("changed") == "1"
                           else f"超时内画面没动（{timeout_ms}ms）")
            notes.append(f"等待结果：{verdict} frames={wrow.get('frames', '?')} elapsed_ms={wrow.get('elapsed_ms', '?')}")

    # ③ 抓图 + ④ 交付（图片本身）
    jpeg, grab_notes = _grab_frame(device, layer, 5000, SCREEN_REMOTE_PATH)
    notes.extend(grab_notes)
    if jpeg is None:
        return {"error": "capture-failed", "notes": notes, "hint": _DEVICE_ASLEEP_HINT}

    small, via = _downscale(jpeg, max_width)
    notes.append(f"交付尺寸：{via}，{len(jpeg)} → {len(small)} 字节")

    return Content([
        {"type": "text", "text": "动作已完成。下面这张图是**动作之后**的画面：\n" + "\n".join(notes)},
        image_block(small),
    ])


# ---------------------------------------------------------------- JSON-RPC 层
def rpc_result(req_id, result) -> dict:
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def rpc_error(req_id, code: int, message: str) -> dict:
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


def text_content(text: str) -> dict:
    return {"content": [{"type": "text", "text": text}]}


def handle(request: dict, device: Device, gdb_handler=None) -> dict | None:
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
                if gdb_handler is None:
                    from .services.gdb import handle as gdb_handler
                payload = gdb_handler(str(arguments.get("action", "")), arguments)
            elif name == "nxc_deploy":
                payload = tool_deploy(device, arguments)
            elif name == "nxc_fetch":
                payload = tool_fetch(device, arguments)
            elif name == "nxc_put":
                payload = tool_put(device, arguments)
            elif name == "nxc_see":
                payload = tool_see(device, arguments)
            elif name == "nxc_step":
                payload = tool_step(device, arguments)
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

        # ★★★ 2026-10-08：图片类工具（nxc_see / nxc_step）返回的是 Content 标记对象 ——
        #   它**本身**就是 MCP 的 content 数组，不能再被 json.dumps 包成一段文本
        #   （那样图片会退化成一大坨 base64 文本，等于白做）。
        if isinstance(payload, Content):
            return rpc_result(req_id, {"content": list(payload)})

        is_error = isinstance(payload, dict) and "error" in payload
        text = json.dumps(payload, ensure_ascii=False, indent=2)
        reply = text_content(text)
        if is_error:
            reply["isError"] = True
        return rpc_result(req_id, reply)

    if req_id is None:
        return None  # 其它通知，忽略
    return rpc_error(req_id, -32601, f"Method not found: {method}")
