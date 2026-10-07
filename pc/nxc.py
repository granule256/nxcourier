#!/usr/bin/env python3
"""NxCourier 平台的 PC 侧极简客户端。

协议是行文本（见 device/platform/README.md 第二节）：
    请求： <工具>.<方法> [k=v]...
    响应： OK <n>\\n  +  n 行载荷        （载荷行要么是 k=v，要么以 '#' 打头是自由文本）
           ERR <码> <说明>\\n

用法：
    python3 pc/nxc.py hello
    python3 pc/nxc.py caps
    python3 pc/nxc.py help tool=probe
    python3 pc/nxc.py 'probe.list' 'probe.group n=1'
    echo 'identify.id' | python3 pc/nxc.py -          # 从 stdin 读命令

选项：
    --ip / --port        设备地址与端口（默认取环境变量 NXC_DEVICE_IP，未设置时为 192.168.1.50:47800）
    --json               把载荷解析成结构化 JSON 输出（给程序吃）
    --timeout            读超时秒数（默认 15）

值的写法：**写自然值**，脚本负责把协议承载不了的字节转义（空格 → %20）。
含空格的值请用引号：
    python3 pc/nxc.py 'fs.ls path="/atmosphere/.../visible tinted rocks_3002235272"'
`%` 是协议字符，本脚本**原样透传** —— 想表达字面的 `%` 请写 `%25`。
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import socket
import sys


def run(ip: str, port: int, commands: list[str], timeout: float) -> list[dict]:
    """发一批命令，返回每条命令的解析结果。"""
    results: list[dict] = []
    with socket.create_connection((ip, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        stream = sock.makefile("rb")
        for cmd in commands:
            sock.sendall((cmd + "\n").encode())
            header = stream.readline().decode("utf-8", "replace").rstrip("\n")
            entry: dict = {"command": cmd, "header": header}

            if header.startswith("OK "):
                count = int(header.split()[1])
                lines: list[str] = []
                for _ in range(count):
                    lines.append(stream.readline().decode("utf-8", "replace").rstrip("\n"))
                entry["lines"] = lines
            elif header.startswith("ERR "):
                parts = header.split(" ", 2)
                entry["error"] = {"code": int(parts[1]), "message": parts[2] if len(parts) > 2 else ""}
            else:
                entry["error"] = {"code": -1, "message": f"无法识别的响应头: {header!r}"}
            results.append(entry)
    return results


import re

# 设备侧对值里的「空格 / % / 控制字符」做了最小转义（%20 / %25 / %XX），
# 因为 k=v 之间是用空格分列的。这里解回来。
_PCT_ESCAPE = re.compile(r"%([0-9A-Fa-f]{2})")


def unescape_value(text: str) -> str:
    """把设备侧转义过的值还原。被转义的只有 ASCII（空格/%/控制字符），
    中文等多字节 UTF-8 不参与转义，所以逐字节还原即可。"""
    if "%" not in text:
        return text
    return _PCT_ESCAPE.sub(lambda m: chr(int(m.group(1), 16)), text)


def escape_value(text: str) -> str:
    """把要发出去的值转义成协议能承载的形式（设备侧会对称还原）。

    ★ 规则：**只转义协议承载不了的字节** —— 空格/制表符/换行/其它控制字符 → `%XX`；
      `%` **原样透传**（所以"手写的 `%20`"仍然表示空格，老习惯不受影响；
      想表达字面的 `%` 就写 `%25`，和 URL 一样）。其余字符一个都不动。
    """
    if not any(ch == " " or ord(ch) < 0x20 or ord(ch) == 0x7F for ch in text):
        return text
    return "".join("%%%02X" % ord(ch) if (ch == " " or ord(ch) < 0x20 or ord(ch) == 0x7F) else ch
                   for ch in text)


def encode_command(line: str) -> str:
    """把用户写的命令行转成**协议形式**：方法名与键不动，只转义 `k=v` 里的值。

    ★ 为什么放在客户端：协议用空格分列字段，所以值里的空格必须先转义。
      这样你就能直接写自然路径 —— 含空格的值请用引号（shell 那套写法）：

          python3 pc/nxc.py 'fs.ls path="/atmosphere/.../visible tinted rocks_3002235272"'

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
    out = []
    for line in lines:
        if line.startswith("#"):
            out.append(line[1:])
            continue
        record = {}
        for field in line.split(" "):
            if not field:
                continue
            key, _, value = field.partition("=")
            record[key] = unescape_value(value)
        out.append(record)
    return out


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
    except OSError as exc:
        print(f"连接失败：{type(exc).__name__}: {exc}", file=sys.stderr)
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
