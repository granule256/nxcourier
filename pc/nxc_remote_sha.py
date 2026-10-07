#!/usr/bin/env python3
"""算设备上某个文件的 sha256（分块读 + 本地哈希）。

## 为什么要单独有这个工具

2026-10-06 晚我连着两次手工写这段代码：
**部署前要"核对卡上现在是什么"**（把 `exefs.nsp` 读回来算哈希，跟记录比）——
这一步能一次证明"本地回退副本 = 卡上实物"。而 MCP 的 `nxc_fetch` 把整个文件取回本地
只为了算个哈希，既慢又把磁盘写满临时文件；它还曾经因为**长连接已死**谎报"文件不存在"。

所以：**只读、只算、不落盘**。

## 用法

    python3 pc/nxc_remote_sha.py /atmosphere/contents/4200000000000012/exefs.nsp
    python3 pc/nxc_remote_sha.py <路径> --expect 5ee6c2be…   # 顺手比对，不一致就退出码 1
    # 本地文件也打一份对照（方便直接比）
    python3 pc/nxc_remote_sha.py <路径> --compare-local dist/exefs.nsp
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
import socket
import sys
from pathlib import Path

DEFAULT_IP = os.environ.get("NXC_DEVICE_IP", "192.168.1.50")
DEFAULT_PORT = 47800
CHUNK = 24000          # 设备侧 fs.read 的单块上限


def read_all(ip: str, port: int, path: str, timeout: float = 20.0):
    """分块把文件读回来。

    ★ 两条必须遵守的规矩（都是踩出来的）：
      1. **按实际收到的字节数推进偏移**，不能按"请求了多少"推进；
      2. **只有"读回 0 字节"才算到头**，不能拿"少于请求量"当 EOF。
    """
    sock = socket.create_connection((ip, port), timeout=timeout)
    stream = sock.makefile("rb")

    def call(line: str) -> list[str]:
        sock.sendall((line + "\n").encode())
        header = stream.readline().decode("utf-8", "replace").rstrip("\n")
        if header == "":
            raise RuntimeError("对端在应答前关闭了连接（主机重启过？长连接已失效）")
        if not header.startswith("OK "):
            raise RuntimeError(f"设备回的不是成功：{header}")
        return [stream.readline().decode("utf-8", "replace").rstrip("\n")
                for _ in range(int(header.split()[1]))]

    digest = hashlib.sha256()
    data = bytearray()
    offset = 0
    while True:
        meta, b64 = {}, None
        for line in call(f"fs.read path={path} offset={offset} len={CHUNK}"):
            if line.startswith("#"):
                b64 = line[1:]
            else:
                k, _, v = line.partition("=")
                meta[k] = v
        if b64 is None:
            raise RuntimeError("应答里没有内容行（读不到数据）")
        piece = base64.b64decode(b64)
        if not piece:
            break                      # ★ 真正的到头
        data.extend(piece)
        digest.update(piece)
        offset += len(piece)
        if meta.get("eof") == "1":
            break
    sock.close()
    return bytes(data), digest.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("remote_path")
    ap.add_argument("--ip", default=DEFAULT_IP)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--expect", help="期望的 sha256；不一致则以退出码 1 结束")
    ap.add_argument("--compare-local", help="顺便算一下这个本地文件的 sha256 做对照")
    args = ap.parse_args()

    try:
        data, sha = read_all(args.ip, args.port, args.remote_path)
    except OSError as exc:
        print(f"连不上设备（{args.ip}:{args.port}）：{type(exc).__name__}: {exc}")
        print("★ 先确认主机醒着：这类模块常配「静置约 148 秒自动睡眠、且睡着不自醒」。")
        return 2
    except RuntimeError as exc:
        print(f"读取失败：{exc}")
        return 2

    print(f"远端 {args.remote_path}")
    print(f"  bytes  = {len(data)}")
    print(f"  sha256 = {sha}")

    ok = True
    if args.compare_local:
        local = Path(args.compare_local).read_bytes()
        local_sha = hashlib.sha256(local).hexdigest()
        same = local_sha == sha
        print(f"本地 {args.compare_local}")
        print(f"  bytes  = {len(local)}")
        print(f"  sha256 = {local_sha}")
        print(f"  ⇒ {'一致 ✓' if same else '★ 不一致（远端和本地不是同一份！）'}")
        ok = ok and same
    if args.expect:
        same = sha == args.expect
        print(f"  ⇒ 期望 {args.expect}：{'一致 ✓' if same else '★ 不一致'}")
        ok = ok and same
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
