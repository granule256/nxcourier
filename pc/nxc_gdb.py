#!/usr/bin/env python3
# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""NxCourier · PC 侧 GDB 会话驱动。

## 为什么 GDB 放在 PC 侧，而不是设备侧

设备侧做 GDB 客户端要重造一个成熟调试器的子集，还会引入平台里第一个"有状态"的东西；
而 GDB 一旦下断点就会**冻结进程**，跟"常驻服务"的语义是拧的。详见
本机开发记录 §十九。

这里改成：**在 PC 侧起一个长驻的 gdb 子进程**，对 AI 暴露成工具。
好处是拿到的是**完整** GDB（符号表 / 反汇编 / 调用栈 / watchpoint），不是子集。

## ★★ 与平台的 `mem` 工具互斥

两者抢的是同一个调试能力：
* 本模块 = PC 侧 GDB，走 TCP `22225` 的调试桩；
* `mem` 工具 = 设备侧 `svcDebugActiveProcess`。
同时用会互相抢句柄 —— 实测崩机（fatal `01000000000d609`）。**二选一。**

## 环境

本机**没有**裸装的 gdb，用的是 devkitPro 容器里的 `aarch64-none-elf-gdb`
（和主线项目 `isaac mod/tools/read_device_state_via_gdb.py` 同一条路）。
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import threading
import time

MARKER = "__NXC_GDB_END__"

DEFAULT_IP = os.environ.get("NXC_DEVICE_IP", "192.168.1.50")
DEFAULT_GDB_PORT = int(os.environ.get("NXC_GDB_PORT", "22225"))

DOCKER_IMAGE = os.environ.get("NXC_GDB_IMAGE", "devkitpro/devkita64:latest")

# 容器内直接 exec gdb；不带 -ex，初始化命令走 stdin（避免嵌套引号地狱）。
DOCKER_CMD = [
    "docker", "run", "--rm", "-i",
    DOCKER_IMAGE,
    "bash", "-lc",
    ". /opt/devkitpro/devkita64.sh && exec aarch64-none-elf-gdb -q -nx",
]

INIT_COMMANDS = [
    "set confirm off",
    "set pagination off",
    "set height 0",
    "set width 0",
    "set print elements 0",
    "set interactive-mode off",
]


def docker_available() -> tuple[bool, str]:
    if shutil.which("docker") is None:
        return False, "找不到 docker 命令"
    probe = subprocess.run(["docker", "image", "inspect", DOCKER_IMAGE],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        return False, f"本机没有 {DOCKER_IMAGE} 镜像"
    return True, "ok"


class GdbError(RuntimeError):
    pass


class GdbSession:
    """一条长驻的 gdb 会话。

    命令—响应的分界靠**哨兵行**：每条命令后面追加 `echo <MARKER>\\n`，
    然后一直读到那行为止。这样不用去解析 gdb 的提示符（`(gdb)`）什么时候出现，
    也不用管输出里混着 `^done` 之类的格式。
    """

    def __init__(self, ip: str = DEFAULT_IP, port: int = DEFAULT_GDB_PORT) -> None:
        self.ip = ip
        self.port = port
        self.proc: subprocess.Popen | None = None
        self.connected = False
        self._lock = threading.Lock()

    # ---- 生命周期
    def start(self, connect: bool = True, timeout: float = 60.0) -> str:
        if self.proc is not None:
            raise GdbError("会话已经在跑了（先 close）")

        ok, why = docker_available()
        if not ok:
            raise GdbError(why)

        self.proc = subprocess.Popen(
            DOCKER_CMD,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )

        banner = self._pump(list(INIT_COMMANDS), timeout=timeout)
        if connect:
            banner += "\n" + self._pump(
                [f"target remote {self.ip}:{self.port}"], timeout=timeout
            )
            self.connected = True
        return banner

    def close(self) -> str:
        """结束会话。

        ★★ 这里**绝不能用 `kill`** —— 在远程目标上 `kill` 会真的终止被调试的进程，
        也就是把你的游戏直接关掉。正确做法是 `detach`：放开目标并让它继续运行。
        """
        if self.proc is None:
            return "会话本来就没开"
        try:
            self.proc.stdin.write("detach\nquit\n")
            self.proc.stdin.flush()
        except (OSError, ValueError):
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # 这里杀的是 gdb 自己，不是游戏
        self.proc = None
        self.connected = False
        return "会话已关闭（已 detach，目标继续运行）"

    def detach_only(self) -> str:
        """只 detach 不断 gdb —— 放开目标让它继续跑，但保留 gdb 进程便于重新附加。"""
        if self.proc is None:
            raise GdbError("会话没开")
        out = self.run(["detach"])
        self.connected = False
        return out

    def status(self) -> dict:
        alive = self.proc is not None and self.proc.poll() is None
        return {
            "running": alive,
            "connected": self.connected,
            "target": f"{self.ip}:{self.port}",
            "image": DOCKER_IMAGE,
        }

    # ---- 执行
    def _pump(self, commands: list[str], timeout: float) -> str:
        """发一批命令，读到哨兵行为止。"""
        if self.proc is None:
            raise GdbError("会话没开（先 start）")

        payload = "\n".join(list(commands) + [f"echo {MARKER}\\n"]) + "\n"
        self.proc.stdin.write(payload)
        self.proc.stdin.flush()

        out: list[str] = []
        deadline = time.time() + timeout
        while True:
            if time.time() > deadline:
                # 目标在跑（例如 continue 之后）时 gdb 不会回到提示符，哨兵也就不会出现。
                raise GdbError(
                    f"等待 gdb 响应超时（{timeout:.0f}s）。"
                    "如果上一条是 continue/step 之类让目标继续运行的命令，"
                    "目标正在跑，gdb 要等它停下来才会回来 —— 这正是 GDB「停下来看」的工作方式。"
                )
            line = self.proc.stdout.readline()
            if line == "":
                raise GdbError("gdb 进程意外结束了（检查 docker 是否可用）")
            if MARKER in line:
                break
            out.append(line.rstrip("\n"))
        return "\n".join(out)

    def run(self, commands: list[str], timeout: float = 60.0) -> str:
        with self._lock:
            return self._pump(commands, timeout)


# 会话是模块级单例：MCP 服务器一个进程服务一个 AI，够用。
_session: GdbSession | None = None


def get_session() -> GdbSession:
    global _session
    if _session is None:
        _session = GdbSession()
    return _session


def handle(action: str, args: dict) -> dict:
    """给 MCP 层用：action ∈ open / run / close / detach / status。"""
    session = get_session()

    if action == "status":
        return session.status()

    if action == "open":
        connect = bool(args.get("connect", True))
        banner = session.start(connect=connect)
        return {"action": "open", "output": banner, **session.status()}

    if action == "close":
        return {"action": "close", "output": session.close()}

    if action == "detach":
        # ★ 用这个而不是 close：只放开目标让它继续跑，不杀进程。
        return {"action": "detach", "output": session.detach_only()}

    if action == "run":
        raw = args.get("commands")
        if isinstance(raw, str):
            commands = [line for line in raw.splitlines() if line.strip()]
        elif isinstance(raw, list):
            commands = [str(c) for c in raw]
        else:
            raise GdbError("commands 必须是字符串或字符串数组")
        if not commands:
            raise GdbError("commands 是空的")
        timeout = float(args.get("timeout", 60.0))
        return {"action": "run", "commands": commands,
                "output": session.run(commands, timeout=timeout)}

    raise GdbError(f"未知 action：{action}（可用 open / run / detach / close / status）")


if __name__ == "__main__":
    # 手工冒烟：python3 pc/nxc_gdb.py [--no-connect]
    connect = "--no-connect" not in sys.argv
    s = GdbSession()
    print("=== status ===", s.status())
    print("=== open ===")
    print(s.start(connect=connect))
    print("=== run: info files 前几行 ===")
    print(s.run(["show version", "info program"], timeout=30)[:800])
    print("=== close ===", s.close())
    print("=== final status ===", s.status())
