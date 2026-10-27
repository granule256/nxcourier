#!/usr/bin/env python3
# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""nxc_see / nxc_step 的自测（**协议级**，走真的 JSON-RPC）。

## 为什么必须走协议，而不是直接 import 调函数

`nxc_see` / `nxc_step` 是**唯二**返回图片的 MCP 工具：它们的返回值是一个
`Content` 标记对象（本身就**是** MCP 的 content 数组），而 `handle()` 里有一条
专门的判断 —— 命中它才不把结果 `json.dumps` 成一段文本。

★ 那条分支**只有真的走一遍 JSON-RPC 才测得到**：直接 import 调用 `tool_see()`
只会拿到一个 Python 对象，测不出"服务器最后到底发出了什么"。

## 用法

    python3 pc/tests/manual/see_selftest.py              # 真机取帧（主机要醒着）
    python3 pc/tests/manual/see_selftest.py --no-device   # 仍会调用截图/控制工具，需要设备

## 判据（不通过的会直接抛断言）

1. `tools/list` 里出现 `nxc_see` / `nxc_step`；
2. 调用后返回的 `content` 是**数组**，且同时含 `type:"text"` 与 `type:"image"`；
3. `image` 块带 `mimeType`，`data` 能 base64 解码且以 JPEG magic `FFD8FF` 开头；
4. ★ 回归检查：老工具（`nxc_call`）**仍然**返回纯文本 —— 别把原本的交付方式改坏了。
"""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SERVER = ROOT / "nxc"
OUTDIR = ROOT / "tmp"

JPEG_MAGIC = b"\xff\xd8\xff"


class Server:
    """以 stdio 拉起 MCP 服务器，用一行一条 JSON-RPC 跟它说话。"""

    def __init__(self) -> None:
        # stderr 写临时文件而不是 PIPE：服务器会往 stderr 打日志，
        # 用 PIPE 且不读，日志写满管道就会把服务器**卡死**（测试会莫名其妙挂住）。
        self.errlog = tempfile.NamedTemporaryFile(
            "w+", prefix="nxc-selftest-", suffix=".log", delete=False)
        self.proc = subprocess.Popen(
            [sys.executable, str(SERVER), "mcp"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errlog,
            text=True, bufsize=1)
        self.next_id = 0

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        msg: dict = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                self.dump_stderr()
                raise RuntimeError("服务器没回应就退出了（stderr 见上）")
            line = line.strip()
            if not line:
                continue
            reply = json.loads(line)
            if reply.get("id") == self.next_id:
                return reply

    def notify(self, method: str) -> None:
        assert self.proc.stdin
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def call_tool(self, name: str, arguments: dict) -> dict:
        reply = self.request("tools/call", {"name": name, "arguments": arguments})
        assert "result" in reply, f"{name} 回了 error：{reply.get('error')}"
        return reply["result"]

    def dump_stderr(self) -> None:
        self.errlog.flush()
        text = Path(self.errlog.name).read_text(errors="replace").strip()
        if text:
            print("--- 服务器 stderr ---")
            print(text[-4000:])
            print("---------------------")

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()


def check_image(result: dict, label: str) -> bytes:
    """断言这是"文本 + 图片"的 content 数组，并返回解码后的 JPEG 字节。"""
    content = result.get("content")
    assert isinstance(content, list), f"{label}: content 不是数组，实际是 {type(content)}"
    kinds = [b.get("type") for b in content]
    print(f"  {label}: content 块 = {kinds}")

    assert "image" in kinds, f"{label}: 没有 image 块（只有 {kinds}）—— 图片能力没生效"
    assert "text" in kinds, f"{label}: 没有 text 块（说明连说明文字都丢了）"

    img = next(b for b in content if b["type"] == "image")
    assert img.get("mimeType") == "image/jpeg", f"{label}: mimeType 不对：{img.get('mimeType')}"
    raw = base64.b64decode(img["data"])
    assert raw[:3] == JPEG_MAGIC, f"{label}: 不是 JPEG，头四字节 = {raw[:4].hex()}"
    print(f"  {label}: ✓ JPEG 有效 {len(raw)} 字节（base64 {len(img['data'])} 字符）")

    text = next(b for b in content if b["type"] == "text")["text"]
    print(f"  {label}: 说明文字 = {text.splitlines()[0]}")
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--no-device", action="store_true",
                        help="不碰设备：只测 JSON-RPC 形状与降采样")
    args = parser.parse_args()

    srv = Server()
    failures = 0
    try:
        print("① initialize")
        reply = srv.request("initialize", {"protocolVersion": "2024-11-05"})
        info = reply["result"]["serverInfo"]
        print(f"  serverInfo = {info['name']} {info['version']}")
        srv.notify("notifications/initialized")

        print("② tools/list")
        names = [t["name"] for t in srv.request("tools/list")["result"]["tools"]]
        print(f"  工具 = {names}")
        for want in ("nxc_see", "nxc_step"):
            assert want in names, f"tools/list 里没有 {want}"
        print("  ✓ nxc_see / nxc_step 都已注册")

        print("③ 回归检查：老工具仍然回纯文本")
        legacy = srv.call_tool("nxc_call", {"method": "identify.id"})
        legacy_kinds = {b.get("type") for b in legacy.get("content", [])}
        if legacy_kinds == {"text"}:
            print(f"  ✓ nxc_call 仍是纯文本：{legacy['content'][0]['text'][:80]!r}")
        else:
            print(f"  ✗ nxc_call 的返回形状被改坏了：{legacy_kinds}")
            failures += 1

        if args.no_device:
            print("\n--no-device：跳过真机取帧。")
            return 1 if failures else 0

        OUTDIR.mkdir(exist_ok=True)

        print("④ nxc_see（真机取帧）")
        try:
            raw = check_image(srv.call_tool("nxc_see", {}), "nxc_see")
            out = OUTDIR / "selftest_see.jpg"
            out.write_bytes(raw)
            print(f"  ✓ 已存 {out}")
        except AssertionError as exc:
            print(f"  ✗ {exc}")
            failures += 1

        print("⑤ nxc_step（WAIT:300 —— 不动任何键，只验证管线）")
        try:
            raw = check_image(srv.call_tool("nxc_step", {"steps": "WAIT:300", "wait": "none"}),
                              "nxc_step")
            out = OUTDIR / "selftest_step.jpg"
            out.write_bytes(raw)
            print(f"  ✓ 已存 {out}")
        except AssertionError as exc:
            print(f"  ✗ {exc}")
            failures += 1

        if failures:
            srv.dump_stderr()
    finally:
        srv.close()

    print(f"\n{'全部通过' if not failures else f'{failures} 项失败'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
