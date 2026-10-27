#!/usr/bin/env python3
"""宿主机测试：MCP 客户端在"长连接已死"时的行为。

## 为什么要有它（2026-10-06 晚的真实事故）

我调用 `nxc_fetch` 去取卡上的 `exefs.nsp`，它回了一句
**"读不到内容（文件不存在或为空）"** —— 我照着那句话去查"文件怎么丢了"，
查了好几分钟；其实**文件好好的**：主机重启过，MCP 手里那条长连接已经死了，
`readline()` 立刻返回空串，而被当成"设备回了一句无法识别的话"，
于是**绕过了重连重试**，最后撒谎成"文件不存在"。

顺带发现第二个问题：类注释承诺"只在**一条响应都还没收到**时才重试"，
但实现里是**无条件**重连重试整个批次 —— 一批 10 条里前 5 条成功了，
剩下 5 条会被连着重放一遍（写操作会重复执行）。

2026-10-23：即使一条应答也没收到，设备也可能已经执行命令。
只有调用点明确声明 retry_safe=True 的只读请求才能自动重试。
这个测试用一个**假设备**（本地 TCP 服务器）验证三个场景：

* **情形 A**：显式允许重试的读请求，第一条响应没回来
  ⇒ 必须重连 + 重试一次，并且**要拿到结果**。
* **情形 B**：第一条回了、第二条断了（已经执行过东西了）
  ⇒ **绝不能重放**，必须抛错说清"前面几条已经执行完"。
* **情形 C**：写命令已经执行，但第一条响应丢失
  ⇒ 默认绝不重放，明确报告执行结果未知。

跑法（不需要真机）：
    python3 pc/tests/helpers/deadconn.py
"""
import socket
import threading
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src"))


def load_module():
    from nxc_host import device
    return device


def read_line(stream):
    data = stream.readline()
    if not data:
        return None
    return data.decode("utf-8", "replace").rstrip("\n")


class FakeDevice:
    """假设备：本地 TCP 服务器 + 可脚本化的行为。

    `handler(连接序号, 连接, 收到的命令列表)` 决定第 N 次连接怎么表现。
    收到的命令会逐个追加进 `seen` —— 测试靠它检查"有没有被重放"。
    """

    def __init__(self, handler):
        self.handler = handler
        self.seen = []
        self.connections = 0
        self._srv = socket.socket()
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", 0))
        self._srv.listen(4)
        self.port = self._srv.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        for _ in range(8):
            try:
                conn, _ = self._srv.accept()
            except OSError:
                return
            self.connections += 1
            try:
                self.handler(self.connections, conn, self.seen)
            except Exception:
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def stop(self):
        try:
            self._srv.close()
        except OSError:
            pass


# ---------------------------------------------------------------- 情形 A
def handler_dead_first(conn_no, conn, seen):
    stream = conn.makefile("rb")
    cmd = read_line(stream)
    seen.append(cmd)
    if conn_no == 1:
        # ★ 收到了命令，但**什么都不回**就关掉 —— 这就是"半死长连接"的样子。
        return
    conn.sendall(b"OK 1\n#pong\n")


# ---------------------------------------------------------------- 情形 B
def handler_die_on_second(conn_no, conn, seen):
    stream = conn.makefile("rb")
    seen.append(read_line(stream))
    conn.sendall(b"OK 1\n#first\n")
    seen.append(read_line(stream))          # 客户端会发第二条
    # 收到第二条、**不回**就关掉 ⇒ 此时客户端"已经收到过 1 条响应"。


def main():
    mod = load_module()
    failed = 0

    def check(ok, what, detail=""):
        nonlocal failed
        if ok:
            print(f"  ok  {what}")
        else:
            failed += 1
            print(f"  x   {what}  {detail}")

    print("[A] 显式允许只读重试，第一条响应没回来 ⇒ 重连并拿到结果")
    fake = FakeDevice(handler_dead_first)
    dev = mod.Device("127.0.0.1", fake.port, timeout=5)
    try:
        result = dev.call_one("ping", retry_safe=True)
    finally:
        dev.close()
        fake.stop()
    check(result.get("header") == "OK 1", "重试后拿到了应答", str(result))
    check(result.get("lines") == ["#pong"], "正文也读对了", str(result.get("lines")))
    check(fake.connections >= 2, "确实重连了一次（新开了一条连接）", f"connections={fake.connections}")
    check(fake.seen.count("ping") == 2, "同一条命令只被发了两次（首次 + 重试）", str(fake.seen))

    print("[B] 已经收到过响应后断线 ⇒ 绝不重放，必须抛错说明")
    fake = FakeDevice(handler_die_on_second)
    dev = mod.Device("127.0.0.1", fake.port, timeout=5)
    raised = None
    try:
        dev.call(["one", "two"])
    except RuntimeError as exc:
        raised = exc
    finally:
        dev.close()
        fake.stop()
    check(raised is not None, "抛了 RuntimeError（而不是静默返回半批结果）", repr(raised))
    check(raised is not None and "已完整应答 1 条" in str(raised) and "未自动重放" in str(raised),
          "错误里说明前一条已完成，且未自动重放", str(raised))
    check(fake.seen.count("two") == 1, "★ 第二条命令【没有】被重放", str(fake.seen))

    print("[C] 写命令已接收但响应没回来 ⇒ 不重放并报告结果未知")
    fake = FakeDevice(handler_dead_first)
    dev = mod.Device("127.0.0.1", fake.port, timeout=5)
    raised = None
    try:
        dev.call_one("input.tap key=A")
    except RuntimeError as exc:
        raised = exc
    finally:
        dev.close()
        fake.stop()
    check(raised is not None and "执行结果未知" in str(raised),
          "响应缺失被报告为执行结果未知", str(raised))
    check(fake.connections == 1, "没有自动重连发送写命令", f"connections={fake.connections}")
    check(fake.seen == ["input.tap key=A"], "写命令只接收一次", str(fake.seen))

    print()
    if failed:
        print(f"{failed} 项失败")
        return 1
    print("全部通过 ✓")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
