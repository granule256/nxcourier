#!/usr/bin/env python3
# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""NxCourier 平台的 PC 侧极简客户端。

协议是行文本（见根目录 README.md 的“协议”部分）：
    请求： <工具>.<方法> [k=v]...
    响应： OK <n>\\n  +  n 行载荷        （载荷行要么是 k=v，要么以 '#' 打头是自由文本）
           ERR <码> <说明>\\n

用法：
    PYTHONPATH=pc/src python3 -m nxc_host.legacy_cli hello
    PYTHONPATH=pc/src python3 -m nxc_host.legacy_cli caps
    PYTHONPATH=pc/src python3 -m nxc_host.legacy_cli help tool=probe
    PYTHONPATH=pc/src python3 -m nxc_host.legacy_cli 'probe.list' 'probe.group n=1'
    echo 'identify.id' | PYTHONPATH=pc/src python3 -m nxc_host.legacy_cli -          # 从 stdin 读命令

选项：
    --ip / --port        设备地址与端口（默认取环境变量 NXC_DEVICE_IP，未设置时为 192.168.1.50:47800）
    --json               把载荷解析成结构化 JSON 输出（给程序吃）
    --timeout            读超时秒数（默认 15）

值的写法：**写自然值**，脚本负责把协议承载不了的字节转义（空格 → %20）。
含空格的值请用引号：
    PYTHONPATH=pc/src python3 -m nxc_host.legacy_cli 'fs.ls path="/atmosphere/.../visible tinted rocks_3002235272"'
`%` 是协议字符，本脚本**原样透传** —— 想表达字面的 `%` 请写 `%25`。
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import sys

from . import device as transport


def run(ip: str, port: int, commands: list[str], timeout: float) -> list[dict]:
    """复用 MCP 的完整帧校验；原始命令默认不重放，异常不返回半批结果。"""
    device = transport.Device(ip, port, timeout)
    try:
        return device.call(commands)
    finally:
        device.close()


def unescape_value(text: str) -> str:
    """复用设备响应转义规则，保留既有 CLI 函数接口。"""
    return transport.unescape_value(text)


def escape_value(text: str) -> str:
    """把要发出去的值转义成协议能承载的形式（设备侧会对称还原）。

    ★ 规则：**只转义协议承载不了的字节** —— 空格/制表符/换行/其它控制字符 → `%XX`；
      `%` **原样透传**（所以"手写的 `%20`"仍然表示空格，老习惯不受影响；
      想表达字面的 `%` 就写 `%25`，和 URL 一样）。其余字符一个都不动。
    """
    # 复用值编码，但 CLI 是手写协议入口，继续保留已有的百分号转义。
    return "%".join(transport.escape_value(part) for part in text.split("%"))


def encode_command(line: str) -> str:
    """把用户写的命令行转成**协议形式**：方法名与键不动，只转义 `k=v` 里的值。

    ★ 为什么放在客户端：协议用空格分列字段，所以值里的空格必须先转义。
      这样你就能直接写自然路径 —— 含空格的值请用引号（shell 那套写法）：

          PYTHONPATH=pc/src python3 -m nxc_host.legacy_cli 'fs.ls path="/atmosphere/.../visible tinted rocks_3002235272"'

      （不加引号会被本函数切成两段：`path=/…/visible` + 两个位置参数。）
    ★ 没有 `=` 的词（位置参数）原样保留 —— 它们本来就不是 `k=v`。
    """
    try:
        tokens = shlex.split(line)
    except ValueError:
        return line          # 引号不成对等：原样发出去，让设备侧报错，别在客户端吞掉
    if not tokens:
        return line
    out = [tokens[0]]
    for tok in tokens[1:]:
        key, sep, value = tok.partition("=")
        out.append(f"{key}={escape_value(value)}" if sep else tok)
    return " ".join(out)


def to_structured(lines: list[str]) -> list:
    """把载荷行转成结构：'k=v k2=v2' → dict；'#文本' → str。"""
    return transport.parse_payload(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("commands", nargs="*", help="要发的命令；用 - 表示从 stdin 读")
    parser.add_argument("--ip", default=os.environ.get("NXC_DEVICE_IP", "192.168.1.50"))
    parser.add_argument("--port", type=int, default=47800)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--json", action="store_true", help="输出结构化 JSON")
    args = parser.parse_args()

    commands = list(args.commands)
    if not commands or commands == ["-"]:
        commands = [line.strip() for line in sys.stdin if line.strip()]
    if not commands:
        parser.error("没有命令可发")

    # ★ 值里的空格在这里转义成 %20（可以用引号写含空格的值：path="/a b/c"）。
    #   成功回包里的名字是设备转义过的（visible%20tinted%20rocks_…），
    #   本脚本解析时会还原成真名；**两种写法喂回去都能用**（值里的 % 原样透传）。
    commands = [encode_command(cmd) for cmd in commands]

    try:
        results = run(args.ip, args.port, commands, args.timeout)
    except (OSError, RuntimeError) as exc:
        print(f"通信失败：{type(exc).__name__}: {exc}", file=sys.stderr)
        return 2

    if args.json:
        for entry in results:
            if "lines" in entry:
                entry["payload"] = to_structured(entry.pop("lines"))
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 0

    for entry in results:
        print(f">>> {entry['command']}")
        if "lines" in entry:
            print(entry["header"])
            for line in entry["lines"]:
                print(f"    {line}")
        else:
            print(entry["header"])
            print(f"    [错误] {entry['error']['message']}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
