"""CLI 使用真实本地 TCP 回归断线和协议损坏，不连接设备。"""
import socket
import subprocess
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nxc_host import legacy_cli as nxc


class ReplyServer:
    def __init__(self, responses):
        self.responses = responses
        self.commands = []
        self.connections = 0
        self.errors = []
        self.stopping = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def serve(self):
        while not self.stopping.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connections += 1
            try:
                with connection:
                    connection.settimeout(2)
                    with connection.makefile("rb") as stream:
                        for response in self.responses:
                            command = stream.readline()
                            if not command:
                                break
                            self.commands.append(command.decode().rstrip("\n"))
                            connection.sendall(response)
            except Exception as exc:
                self.errors.append(exc)

    def __exit__(self, *exc):
        self.stopping.set()
        self.listener.close()
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise AssertionError("本地测试 TCP 线程未退出")
        if self.errors:
            raise AssertionError(self.errors)


class CliTransportTests(unittest.TestCase):
    def test_eof_header_reports_unknown_without_replaying_write(self):
        command = "fs.write path=/x data=YQ== append=1"
        with ReplyServer([b""]) as server:
            with self.assertRaisesRegex(RuntimeError, "执行结果未知.*未自动重放"):
                nxc.run("127.0.0.1", server.port, [command], 2)
            self.assertEqual(server.commands, [command])
            self.assertEqual(server.connections, 1)

    def test_eof_body_never_returns_partial_response(self):
        with ReplyServer([b"OK 0\n", b"OK 2\n#partial\n"]) as server:
            with self.assertRaisesRegex(RuntimeError, "已完整应答 1.*第 2.*未知"):
                nxc.run("127.0.0.1", server.port, ["hello", "fs.read path=/x"], 2)
            self.assertEqual(server.commands, ["hello", "fs.read path=/x"])
            self.assertEqual(server.connections, 1)

    def test_bad_frames_raise_instead_of_returning_corrupt_payloads(self):
        for response in (b"OK x\n", b"OK -1\n", b"ERR x bad\n", b"nonsense\n",
                         b"OK 0", b"OK 1\nunterminated", b"OK 1\n\xff\n"):
            with self.subTest(response=response), ReplyServer([response]) as server:
                with self.assertRaisesRegex(RuntimeError, "执行结果未知"):
                    nxc.run("127.0.0.1", server.port, ["fs.write path=/x data=YQ=="], 2)
                self.assertEqual(len(server.commands), 1)
                self.assertEqual(server.connections, 1)

    def test_normal_api_preserves_raw_commands_and_response_shape(self):
        commands = ["fs.stat path=/a%20b%25", "missing.method"]
        with ReplyServer([b"OK 2\npath=/a%20b%25\n#note\n", b"ERR 404 unknown-method\n"]) as server:
            results = nxc.run("127.0.0.1", server.port, commands, 2)
            self.assertEqual(server.commands, commands)
        self.assertEqual(results, [
            {"command": commands[0], "header": "OK 2", "lines": ["path=/a%20b%25", "#note"]},
            {"command": commands[1], "header": "ERR 404 unknown-method",
             "error": {"code": 404, "message": "unknown-method"}},
        ])
        self.assertEqual(nxc.to_structured(results[0]["lines"]), [{"path": "/a b%"}, "note"])

    def test_cli_encoding_keeps_handwritten_percent_escapes(self):
        for value in ("/a%20b%25", "/a%2Fb", "/100%", "/%2520"):
            self.assertEqual(nxc.escape_value(value), value)
        self.assertEqual(nxc.encode_command('fs.stat path="/a b/%20/中文"'),
                         "fs.stat path=/a%20b/%20/中文")
        self.assertEqual(nxc.escape_value("a\nb\tc"), "a%0Ab%09c")
        self.assertEqual(nxc.unescape_value("a%20b%25"), "a b%")

    def test_cli_main_reports_transport_error_with_nonzero_exit(self):
        with ReplyServer([b"OK 2\n#partial\n"]) as server:
            result = subprocess.run(
                [sys.executable, "-m", "nxc_host.legacy_cli", "--ip", "127.0.0.1",
                 "--port", str(server.port), "--timeout", "2", "--json",
                 "fs.write path=/x data=YQ=="], capture_output=True, text=True, timeout=5,
                env=dict(__import__("os").environ, PYTHONPATH=str(Path(__file__).resolve().parents[1] / "src")))
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("执行结果未知", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
