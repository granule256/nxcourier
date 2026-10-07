#!/usr/bin/env python3
"""NxCourier 控制小工具 —— 把「截图 / 按键 / 摇杆」做成一条命令，方便一步步看着屏幕走。

    python3 pc/nxc_ctl.py shot [名字]          截图并取回本机 tmp/<名字>.jpg（默认 step）
    python3 pc/nxc_ctl.py press A,B [hold_ms]  按键（多个用逗号分隔）
    python3 pc/nxc_ctl.py stick lx ly [hold_ms]  左摇杆（中心 0，满偏 ±32767）
    python3 pc/nxc_ctl.py wait 秒              等待
    python3 pc/nxc_ctl.py current              看当前在跑什么
    python3 pc/nxc_ctl.py close [pid]          ★ 直接关闭应用，并自动按掉系统错误框
    python3 pc/nxc_ctl.py act "步骤" [--stable]  ★★ 按一串键 → 等画面反应（边看边控制的核心）
                                               步骤写法同 input.seq：DRIGHT:120;WAIT:300;A:120
                                               默认等「画面变了」；--stable 等「画面停住」

为什么单独做个脚本：**"看着屏幕操作"需要反复 截图→看→按键**，
把它压缩成一条命令，每一步的往返成本才低。

★ `close` 为什么是"两步"：`title.terminate` 强制终止任何应用，
系统**一定**会弹「由于发生错误，软件已关闭。」（实测
`pmshellTerminateProcess` 与 `pmshellTerminateProgram` 两种模式都一样）。
所以"关得干净"要靠**再按一次 A 把框按掉** —— 关闭本身仍然是直接从进程层做的，
不是走菜单操作。
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import nxc_mcp  # noqa: E402

DEVICE = nxc_mcp.Device(os.environ.get("NXC_DEVICE_IP", "192.168.1.50"), 47800, 60.0)
OUTDIR = Path(__file__).resolve().parent.parent / "tmp"


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


def _screen_bytes() -> int:
    """抓一张图，只回报字节数 —— 用来判断"现在是不是还停在错误框上"。

    依据：实测那张「由于发生错误，软件已关闭。」的框只有约 37KB，
    而正常主界面约 188KB、游戏画面 150～370KB。所以尺寸是个可靠的判据。
    """
    result = nxc_mcp.tool_call(DEVICE, {"method": "screen.capture"})
    if "error" in result:
        return 0
    return int(result["payload"][0]["bytes"])


def close(pid: int | None = None, dismiss: bool = True) -> None:
    """直接关闭正在跑的应用，并（默认）自动按掉系统错误框。

    ★ 为什么必须"关 + 按框"两步：强制终止任何应用，系统**一定**会弹
      「由于发生错误，软件已关闭。」—— 实测 `pmshellTerminateProcess` 与
      `pmshellTerminateProgram`（`mode=process` / `mode=program`）都一样。
      关闭动作本身仍是直接调 `title.terminate`，**不是走菜单操作**。

    ★ 为什么不能简单"等一下再按一次 A"：实测按早了没用 ——
      **框是在进程真正死掉之后才弹出来的**，时间点不好猜。
      所以这里**轮询检测**：抓图看尺寸，确认框已经出现才按 A，按完再确认它没了。
      （盲按多次 A 是危险的：主界面上多按一次 A 会直接把游戏启动起来。）
    """
    if pid is None:
        cur = nxc_mcp.tool_call(DEVICE, {"method": "title.current"}).get("payload", [{}])[0]
        if cur.get("running") != "application":
            print("当前没有应用在跑，不用关")
            return
        pid = int(cur["pid"])

    result = nxc_mcp.tool_call(DEVICE, {"method": "title.terminate",
                                        "params": {"pid": pid,
                                                   "confirm": "I-KNOW-THIS-CLOSES"}})
    print("terminate:", result.get("payload", result))
    if "error" in result:
        return

    if not dismiss:
        return

    # 轮询：等框出现 → 按 A → 确认它没了。
    for attempt in range(1, 7):
        time.sleep(2.0)
        size = _screen_bytes()
        if size == 0:
            continue
        if size > 60000:
            print(f"界面已恢复正常（画面 {size}B，第 {attempt} 次检查）—— 提示框已关掉")
            return
        print(f"  第 {attempt} 次：检测到提示框（{size}B），按 A 关闭")
        press("A", 150)
    print("警告：重试 6 次仍未恢复，请人工看一眼屏幕")


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
