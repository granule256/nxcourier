# ================================================================
# NxCourier · Copyright (C) 2026 granule256
#
# 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
#（GPL-2.0-only）发布，不带任何担保。
# 完整条款见仓库根目录的 LICENSE。
# ================================================================

"""设备 TCP 长连接、完整帧校验与协议参数编解码。"""
from __future__ import annotations
import os
import re
import socket
import sys

DEFAULT_IP = os.environ.get("NXC_DEVICE_IP", "192.168.1.50")
DEFAULT_PORT = int(os.environ.get("NXC_DEVICE_PORT", "47800"))
PROTOCOL_LINE_MAX = 8192
ARG_VALUE_MAX = 4095


def log(*parts: object) -> None:
    """日志只能走 stderr：stdout 是协议流。"""
    print("[nxc-mcp]", *parts, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- 设备通信
class Device:
    """与设备的一条**长连接**封装。

    为什么复用连接：这台主机的 TCP 栈对「频繁建连/断连」敏感（FTP 那边有同样的毛病，
    会返 451 或直接 reset）。每次调用都新建连接会偶发 ConnectionResetError。

    默认不重放命令：发送后断线意味着执行结果未知。
    只有调用点明确标记 retry_safe=True 的只读请求可重连重试一次。
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
    def _read_response(self, cmd: str) -> dict:
        """单条完整帧解析；顺序与窗口调用共用同样的协议校验。"""
        raw = self._stream.readline()
        if not raw or not raw.endswith(b"\n"):
            raise ConnectionResetError("响应头缺失或未完整接收")
        header = raw.decode("utf-8").rstrip("\n")
        entry: dict = {"command": cmd, "header": header}
        success = re.fullmatch(r"OK ([0-9]+)", header)
        failure = re.fullmatch(r"ERR ([0-9]+)(?: (.*))?", header)
        if success:
            count = int(success.group(1))
            if count > 4096:
                raise ValueError(f"响应行数异常: {count}")
            lines = []
            for _ in range(count):
                body = self._stream.readline()
                if not body or not body.endswith(b"\n"):
                    raise ConnectionResetError(f"响应正文未完整接收（{len(lines)}/{count}）")
                lines.append(body.decode("utf-8").rstrip("\n"))
            entry["lines"] = lines
        elif failure:
            entry["error"] = {"code": int(failure.group(1)), "message": failure.group(2) or ""}
        else:
            raise ValueError(f"无法识别的响应头: {header!r}")
        return entry

    def _exchange(self, commands: list[str], progress: list[int] | None = None) -> list[dict]:
        """完整读取一条响应后才推进进度；损坏帧必须由 call 关闭连接。"""
        results: list[dict] = []
        for cmd in commands:
            self._sock.sendall((cmd + "\n").encode())
            results.append(self._read_response(cmd))
            if progress is not None:
                progress[0] = len(results)
        return results

    def call_windowed(self, commands, window: int = 2, *, retry_safe: bool = False) -> list[dict]:
        """协商后使用的有界流水，最多两条未应答；失败始终关闭且不重放。

        retry_safe保留接口一致性，但窗口失败可能涉及多条命令，不能整体重试。
        命令可为迭代器，调用方按小批处理结果，避免完整文件命令列表。
        """
        if type(window) is not int or not 1 <= window <= 2:
            raise ValueError("window必须为1或2")
        iterator = iter(commands)
        pending: list[str] = []
        results: list[dict] = []
        sent = 0
        exhausted = False
        try:
            while pending or not exhausted:
                while len(pending) < window and not exhausted:
                    try:
                        command = next(iterator)
                    except StopIteration:
                        exhausted = True
                        break
                    if self._sock is None:
                        self._connect()
                    # sendall可能部分发送后抛错，这条同样属于未知范围。
                    sent += 1
                    pending.append(command)
                    self._sock.sendall((command + "\n").encode())
                if pending:
                    results.append(self._read_response(pending[0]))
                    pending.pop(0)
            return results
        except Exception as exc:
            self._close()
            unknown = (f"第 {len(results) + 1} 至 {sent} 条命令执行结果未知"
                       if sent > len(results) else "没有已发送未确认命令")
            raise RuntimeError(f"已完整应答 {len(results)} 条；{unknown}，未自动重放。"
                               f"原错误：{type(exc).__name__}: {exc}") from exc

    def call(self, commands: list[str], *, retry_safe: bool = False) -> list[dict]:
        # 即使显式允许重试，也只重连一次；失败连接绝不能留给下一次调用。
        for attempt in range(2 if retry_safe else 1):
            progress = [0]
            try:
                if self._sock is None:
                    self._connect()
                return self._exchange(commands, progress)
            except (OSError, ValueError, UnicodeError) as exc:
                self._close()
                if retry_safe and attempt == 0:
                    log(f"只读请求连接/协议失败（{type(exc).__name__}），重连重试一次")
                    continue
                raise RuntimeError(
                    f"已完整应答 {progress[0]} 条；第 {progress[0] + 1} 条命令执行结果未知，"
                    f"未自动重放。原错误：{type(exc).__name__}: {exc}") from exc
        raise AssertionError("unreachable")

    def call_one(self, command: str, *, retry_safe: bool = False) -> dict:
        return self.call([command], retry_safe=retry_safe)[0]

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
    """结构化参数表示自然值；百分号、空白和控制字节必须对称转义。"""
    return "".join("%%%02X" % ord(ch) if ch == "%" or ch == " " or ord(ch) < 0x20
                   or ord(ch) == 0x7F else ch for ch in text)


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
