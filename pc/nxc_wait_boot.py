#!/usr/bin/env python3
# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""等设备重启完成 —— 并且**说清"现在处于哪一态"**。

## 为什么要有它（用户的直接反馈）

「你的重启后检测有问题，经常我已经完成重启进入系统你检测不到」。
复盘下来，原来那种检测有三个硬伤：

1. **只看 TCP 端口**（`nc -z 47800`）—— 分不清"系统在启动中"还是"我们的模块没起来"，
   更分不清"主机睡着了"（睡着时端口同样不通，而且**不会自己醒**）。
2. **窗口太短**（几十秒），而"人工在 Hekate 菜单里选一项"可能要等一会儿 ⇒ 我等不到就放弃了。
3. ★ **IP 一变就永远找不到** —— DHCP 换了地址时，我还在死盯旧 IP。

所以这个工具：
* **用开机号判断**（设备自己的 `identify.id` 里的 `boot`）—— 这是"真的重启了"的硬证据，
  而不是"端口通了"这种弱证据；
* **一直等到超时**（默认 15 分钟），**每 30 秒**打一条带时间戳的进度，不刷屏；
* 把三种状态分开报：**系统没起 / 系统起来但我们的模块没监听 / 起来了且开机号 +1**；
* **超时后自动找设备**：先看 ARP 表里认识的主机，再去探测它们（**不改任何配置**），
  告诉你"是不是换 IP 了"。

## 用法

    # 最常用：先读当前开机号，然后等它 +1（部署脚本这么用）
    python3 pc/nxc_wait_boot.py

    # 只想看一眼现在的状态
    python3 pc/nxc_wait_boot.py --once

    # 明确等某个开机号
    python3 pc/nxc_wait_boot.py --expect-boot 149
退出码：0=等到；1=超时（并已尝试找设备）；2=一开始就连不上。
"""
from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import sys
import time
from datetime import datetime

DEFAULT_IP = os.environ.get("NXC_DEVICE_IP", "192.168.1.50")
PORT_PLATFORM = 47800     # 我们自己的模块
PORT_GDBSTUB = 22225      # Atmosphère 的 GDB 桩（用它判断"系统起来了没"）
POLL_SEC = 2.0
PROGRESS_SEC = 30.0


def tcp_open(ip: str, port: int, timeout: float = 1.5) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def call(ip: str, line: str, timeout: float = 3.0) -> dict:
    """发一条协议命令，返回 {k: v} 字段（重复键以最后一条为准）。"""
    sock = socket.create_connection((ip, PORT_PLATFORM), timeout=timeout)
    stream = sock.makefile("rb")
    sock.sendall((line + "\n").encode())
    header = stream.readline().decode("utf-8", "replace").rstrip("\n")
    out: dict = {"_header": header}
    if header.startswith("OK "):
        for _ in range(int(header.split()[1])):
            text = stream.readline().decode("utf-8", "replace").rstrip("\n")
            if text.startswith("#"):
                continue
            for field in text.split(" "):
                if "=" in field:
                    k, _, v = field.partition("=")
                    out[k] = v
    sock.close()
    return out


def boot_number(ip: str):
    """读开机号；读不到返回 None。"""
    try:
        info = call(ip, "identify.id")
    except OSError:
        return None
    raw = info.get("boot")
    return int(raw) if raw and raw.isdigit() else None


def arp_candidates() -> list[str]:
    """从 ARP 表里拿"我认识的主机"（不扫网段、不改任何东西）。"""
    try:
        out = subprocess.run(["arp", "-a"], capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return []
    found = re.findall(r"\((\d+\.\d+\.\d+\.\d+)\)", out)
    seen, uniq = set(), []
    for ip in found:
        if ip not in seen:
            seen.add(ip)
            uniq.append(ip)
    return uniq


def hunt() -> list[str]:
    """超时后找设备：在 ARP 表里的主机上探 47800 / 22225。"""
    hits = []
    for ip in arp_candidates():
        if tcp_open(ip, PORT_PLATFORM, 1.0):
            hits.append(f"{ip}（我们的平台 47800 在）")
        elif tcp_open(ip, PORT_GDBSTUB, 1.0):
            hits.append(f"{ip}（只是 GDB 桩 22225 在）")
    return hits


def stamp() -> str:
    return datetime.now().strftime("%H:%M:%S")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=DEFAULT_IP)
    ap.add_argument("--timeout", type=float, default=900.0, help="等多久（秒），默认 900")
    ap.add_argument("--expect-boot", type=int, help="明确的期望开机号（默认：当前开机号 + 1）")
    ap.add_argument("--once", action="store_true", help="只报当前状态，不等")
    args = ap.parse_args()

    # ★★ 2026-10-06 晚：**行缓冲**。这个工具最常见的用法是"丢到后台当监视器"
    #   （输出被重定向到文件）—— 而 Python 在非终端下是**块缓冲**的 ⇒ 进度一句都看不到，
    #   直到进程退出。用户上一轮就是这么"什么都没看到"然后把它取消掉的。
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    base = boot_number(args.ip)
    if args.once:
        if base is None:
            print(f"{stamp()} {args.ip}: 47800 {'通但读不到开机号' if tcp_open(args.ip, PORT_PLATFORM) else '不通'}"
                  f"；22225 {'通' if tcp_open(args.ip, PORT_GDBSTUB) else '不通'}")
            return 2
        print(f"{stamp()} {args.ip}: boot={base}（47800 通）")
        return 0

    want = args.expect_boot if args.expect_boot is not None else (None if base is None else base + 1)
    if base is None:
        print(f"{stamp()} ★ 一开始就连不上 {args.ip}:47800 —— 可能在睡觉/没开机/换 IP 了。"
              f"（22225 {'通' if tcp_open(args.ip, PORT_GDBSTUB) else '不通'}）")
    else:
        print(f"{stamp()} 当前 boot={base}，等它变成 {want}（超时 {int(args.timeout)} 秒）")

    started = time.time()
    deadline = started + args.timeout
    last_state = None
    last_progress = started
    while time.time() < deadline:
        ok47800 = tcp_open(args.ip, PORT_PLATFORM)
        if ok47800:
            boot = boot_number(args.ip)
            if boot is not None and want is not None:
                if boot >= want:
                    used = time.time() - started
                    print(f"{stamp()} ★★ 重启完成：boot={boot}（等了 {used:.0f} 秒）⇒ 模块已经在跑")
                    return 0
                # ★★ 2026-10-06 晚：**boot 变小 = 心跳文件被重置过**（最常见原因：配置目录迁移 ——
                #   例如给项目改名时 `/config/knx` → `/config/nxc`，计数会从 1 重新数）。
                #   ★ 这种情况"等 boot=期望值"**永远等不到** ⇒ 必须当成"已经起来"，否则又是一次
                #     "明明进了系统却报检测不到"。这个坑我今晚刚踩过。
                if base is not None and boot < base:
                    used = time.time() - started
                    print(f"{stamp()} ★★ 重启完成：boot 从 {base} 变成 {boot}"
                          f"（变小 = 心跳文件被重置过，例如配置目录迁移）"
                          f"⇒ 模块已经在跑（等了 {used:.0f} 秒）")
                    return 0
                state = f"47800 通，但 boot={boot}（还没到 {want}）"
            else:
                state = "47800 通，但读不到开机号（模块刚起、服务循环还没转起来？）"
        else:
            state = ("22225 通 ⇒ 系统起来了，但我们的模块还没在 47800 监听（或没加载）"
                     if tcp_open(args.ip, PORT_GDBSTUB) else "两个端口都不通 ⇒ 系统在启动中／在菜单上／睡着了")
        if state != last_state:
            print(f"{stamp()} {state}")
            last_state = state
        now = time.time()
        if now - last_progress >= PROGRESS_SEC:
            print(f"{stamp()} …仍在等（已 {now - started:.0f} 秒）")
            last_progress = now
        time.sleep(POLL_SEC)

    print(f"{stamp()} ★ 超时（{int(args.timeout)} 秒）没等到 boot={want}。")
    print("   三种常见原因：① 还在 Hekate 菜单上没选（本机 autoboot=0，要人工选）"
          " ② 主机睡着了（睡着后 47800 不通、也不会自己醒） ③ 换 IP 了")
    hits = hunt()
    if hits:
        print("   ★ ARP 表里发现这些主机上有服务在监听：")
        for h in hits:
            print(f"     - {h}")
        print("     ⇒ 如果里面有你的主机，说明它换 IP 了，用 --ip 指定再试。")
    else:
        print("   ARP 表里没找到任何一个在监听 47800/22225 的主机。")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
