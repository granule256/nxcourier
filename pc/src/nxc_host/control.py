#!/usr/bin/env python3
# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""NxCourier 控制小工具 —— 把「截图 / 按键 / 摇杆」做成一条命令，方便一步步看着屏幕走。

    PYTHONPATH=pc/src python3 -m nxc_host.control shot [名字]          截图并取回本机 tmp/<名字>.jpg（默认 step）
    PYTHONPATH=pc/src python3 -m nxc_host.control press A,B [hold_ms]  按键（多个用逗号分隔）
    PYTHONPATH=pc/src python3 -m nxc_host.control stick lx ly [hold_ms]  左摇杆（中心 0，满偏 ±32767）
    PYTHONPATH=pc/src python3 -m nxc_host.control wait 秒              等待
    PYTHONPATH=pc/src python3 -m nxc_host.control current              看当前在跑什么
    PYTHONPATH=pc/src python3 -m nxc_host.control close [pid]          直接关闭应用；用户判图后决定是否按键
    PYTHONPATH=pc/src python3 -m nxc_host.control act "步骤" [--stable]  ★★ 按一串键 → 等画面反应（边看边控制的核心）
                                               步骤写法同 input.seq：DRIGHT:120;WAIT:300;A:120
                                               默认等「画面变了」；--stable 等「画面停住」

为什么单独做个脚本：**"看着屏幕操作"需要反复 截图→看→按键**，
把它压缩成一条命令，每一步的往返成本才低。

`close` 只发送关闭请求，不会自动按键。系统可能显示提示框；
请先截图查看，由用户明确确认当前画面和下一步操作后再执行输入。
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

from . import tools as nxc_mcp

DEVICE = nxc_mcp.Device(os.environ.get("NXC_DEVICE_IP", "192.168.1.50"), 47800, 60.0)
OUTDIR = Path.cwd() / "tmp"


def shot(name: str = "step") -> None:
    result = nxc_mcp.tool_call(DEVICE, {"method": "screen.capture"})
    if "error" in result:
        print("抓图失败:", result["error"])
        return
    OUTDIR.mkdir(exist_ok=True)
    out = nxc_mcp.tool_fetch(DEVICE, {
        "remote_path": "/config/nxc/screen.jpg",
        "local_path": str(OUTDIR / f"{name}.jpg"),
    })
    print(f"{out['local']}  {out['bytes']}B")


def press(buttons: str, hold_ms: int = 150) -> None:
    result = nxc_mcp.tool_call(DEVICE, {"method": "input.press",
                                        "params": {"buttons": buttons, "hold_ms": hold_ms}})
    print(result.get("payload", result))


def stick(lx: int, ly: int, hold_ms: int = 400) -> None:
    result = nxc_mcp.tool_call(DEVICE, {"method": "input.press",
                                        "params": {"lx": lx, "ly": ly, "hold_ms": hold_ms}})
    print(result.get("payload", result))


def close(pid: int | None = None) -> None:
    """关闭应用后由用户判图；不根据 JPEG 大小推断界面内容。"""
    if pid is None:
        result = nxc_mcp.tool_call(DEVICE, {"method": "title.current"})
        if "error" in result:
            print("查询当前应用失败:", result["error"])
            return
        payload = result.get("payload")
        if not isinstance(payload, list) or not payload or not isinstance(payload[0], dict):
            print("查询当前应用失败: 响应缺少应用状态", result)
            return
        cur = payload[0]
        if cur.get("running") == "none":
            print("当前没有应用在跑，不用关")
            return
        if cur.get("running") != "application":
            print("查询当前应用失败: 无法识别应用状态", cur)
            return
        try:
            pid = int(cur["pid"])
            if pid <= 0:
                raise ValueError("pid 必须为正数")
        except (KeyError, TypeError, ValueError):
            print("查询当前应用失败: 响应缺少有效进程号", cur)
            return

    result = nxc_mcp.tool_call(DEVICE, {"method": "title.terminate",
                                        "params": {"pid": pid,
                                                   "confirm": "I-KNOW-THIS-CLOSES"}})
    print("terminate:", result.get("payload", result))
    if "error" in result:
        return

    print("关闭请求已返回。系统可能显示提示框，请先执行 shot 查看截图；"
          "由用户明确确认当前画面和下一步按键后，再执行 press。")


def act(steps: str, timeout_ms: int = 5000, stable: bool = False) -> None:
    """★ 一次做完「按一串键 → 等画面反应」—— 这是边看边控制的日常用法。

    默认等 `wait_change`（画面**变了**就算生效）；
    加 `--stable` 则等 `wait_stable`（画面**停住**才算完事，用来判断"加载完了没"）。

    ★ 为什么这是重要的一块：设备端连着抓帧比对，
    所以"按键之后要等多久"不再是猜的 —— 画面一变（或一稳）立刻回。
    """
    t0 = time.time()
    result = nxc_mcp.tool_call(DEVICE, {"method": "input.seq", "params": {"steps": steps}})
    print("input.seq:", result.get("payload", result))
    if "error" in result:
        return

    method = "screen.wait_stable" if stable else "screen.wait_change"
    result = nxc_mcp.tool_call(DEVICE, {"method": method, "params": {"timeout_ms": timeout_ms}})
    payload = result.get("payload") or result.get("error")
    elapsed = time.time() - t0

    if "error" in result:
        print(f"{method}: 失败 →", payload)
        return
    row = payload[0] if isinstance(payload, list) and payload and isinstance(payload[0], dict) else {}
    if stable:
        verdict = "画面已停住（加载/过渡结束）" if row.get("stable") == "1" else "超时前一直没停"
        print(f"★ {verdict}    frames={row.get('frames')}  held={row.get('held_frames')}"
              f"  用了 {elapsed*1000:.0f}ms")
    else:
        verdict = "动作生效了（画面确实变了）" if row.get("changed") == "1" else "超时内画面没动"
        print(f"★ {verdict}    frames={row.get('frames')}  用了 {elapsed*1000:.0f}ms")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    try:
        if cmd == "shot":
            shot(sys.argv[2] if len(sys.argv) > 2 else "step")
        elif cmd == "press":
            press(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 150)
        elif cmd == "stick":
            stick(int(sys.argv[2]), int(sys.argv[3]),
                  int(sys.argv[4]) if len(sys.argv) > 4 else 400)
        elif cmd == "wait":
            time.sleep(float(sys.argv[2]))
            print(f"等了 {sys.argv[2]}s")
        elif cmd == "current":
            print(nxc_mcp.tool_call(DEVICE, {"method": "title.current"}).get("payload"))
        elif cmd == "close":
            close(int(sys.argv[2]) if len(sys.argv) > 2 else None)
        elif cmd == "act":
            stable = "--stable" in sys.argv
            act(sys.argv[2], stable=stable)
        else:
            print(__doc__)
            return 1
    finally:
        DEVICE.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
