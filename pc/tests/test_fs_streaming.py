"""fs传输协商、有界流水与流式IO回归；不连接真机。"""
import hashlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from test_pc_reliability import Connection, FileDevice, ok, m


class ModernDevice(FileDevice):
    def __init__(self, *args, info="write_chunk=3060 read_chunk=24000 max_inflight=2 sha256=1", **kwargs):
        super().__init__(*args, **kwargs)
        self.info = info
        self.windows = []

    def call_one(self, command, **kwargs):
        if command == "tools.list tool=fs":
            result = super().call_one(command, **kwargs)
            result["lines"] += ["method=fs.transfer_info", "method=fs.sha256 params=path*(str)"]
            return result
        if command == "fs.transfer_info":
            self.commands.append(command)
            return ok(self.info)
        if command.startswith("fs.sha256 "):
            self.commands.append(command)
            path = m.unescape_value(command.split("path=", 1)[1].split()[0])
            content = self.files[path]
            return ok(f"bytes={len(content)} sha256={hashlib.sha256(content).hexdigest()}")
        return super().call_one(command, **kwargs)

    def call_windowed(self, commands, *, window=2, retry_safe=False):
        commands = list(commands)
        self.windows.append((commands, window, retry_safe))
        return self.call(commands)


class FsStreamingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.source = Path(self.tmp.name) / "in"
        self.content = bytes(range(251)) * 501
        self.source.write_bytes(self.content)
        self.args = {"local_path": str(self.source), "remote_path": "/live"}

    def tearDown(self):
        self.tmp.cleanup()

    def test_window_sends_two_before_first_read_and_never_three(self):
        device = Connection([b"OK 0\n" * 5])
        sock = device.sockets[0]
        original = sock.stream
        class Stream:
            replies = 0
            def readline(stream):
                self.assertLessEqual(len(sock.sent) - stream.replies, 2)
                if not stream.replies:
                    self.assertEqual(len(sock.sent), 2)
                stream.replies += 1
                return original.readline()
            def close(stream):
                original.close()
        sock.stream = Stream()
        result = device.call_windowed((f"fs.write n={n}" for n in range(5)))
        self.assertEqual(len(result), 5)

    def test_window_disconnect_does_not_replay_and_reports_range(self):
        device = Connection([b"OK 0\n", b"OK 0\n" * 4])
        with self.assertRaisesRegex(RuntimeError, "已完整应答 1.*第 2.*3.*未知.*未自动重放"):
            device.call_windowed(["a", "b", "c", "d"], retry_safe=True)
        self.assertEqual(device.connects, 1)
        self.assertEqual(device.sockets[0].sent, [b"a\n", b"b\n", b"c\n"])
        self.assertIsNone(device._sock)

    def test_window_rejects_unbounded_and_invalid_frames(self):
        device = Connection([b"OK 1\nbroken"])
        with self.assertRaises(ValueError):
            device.call_windowed(["x"], window=3)
        self.assertEqual(device.connects, 0)
        with self.assertRaisesRegex(RuntimeError, "未知"):
            device.call_windowed(["x", "y"])
        self.assertIsNone(device._sock)

    def test_modern_upload_is_streamed_and_hashes_without_readback(self):
        device = ModernDevice({"/live": b"old"})
        with mock.patch.object(Path, "read_bytes", side_effect=AssertionError("whole-file read")), \
             mock.patch.object(m, "read_remote_file", side_effect=AssertionError("whole remote read")):
            result = m.tool_put(device, self.args)
        self.assertEqual(device.files["/live"], self.content)
        self.assertEqual(device.files[result["backup"]], b"old")
        self.assertEqual(result["sha256"], hashlib.sha256(self.content).hexdigest())
        self.assertTrue(device.windows)
        self.assertTrue(all(len(c) <= 2 and w == 2 and not r for c, w, r in device.windows))
        self.assertFalse(any(c.startswith("fs.read ") for c in device.commands))

    def test_upload_reads_only_bounded_chunks_and_starts_before_file_is_consumed(self):
        device = ModernDevice()
        original_open = Path.open
        testcase = self
        class GuardedSource:
            reads = 0
            def __init__(self):
                self.stream = original_open(testcase.source, "rb")
            def __enter__(self):
                return self
            def __exit__(self, *args):
                self.stream.close()
            def read(self, size=-1):
                testcase.assertGreater(size, 0)
                testcase.assertLessEqual(size, 3060)
                self.reads += 1
                if self.reads > 2:
                    testcase.assertTrue(device.windows, "不得先读完整文件或生成完整命令列表")
                return self.stream.read(size)
        def guarded_open(path, *args, **kwargs):
            return GuardedSource() if path == self.source else original_open(path, *args, **kwargs)
        with mock.patch.object(Path, "open", guarded_open), \
             mock.patch.object(m, "_build_write_commands", side_effect=AssertionError("full command list")):
            m.tool_put(device, self.args)
        self.assertEqual(device.files["/live"], self.content)

    def test_negotiated_smaller_chunks_and_single_window_are_honored(self):
        device = ModernDevice(info="write_chunk=512 read_chunk=1024 max_inflight=1 sha256=1")
        result = m.tool_put(device, self.args)
        self.assertEqual(result["chunk_bytes"], 512)
        self.assertTrue(all(len(c) == w == 1 for c, w, _ in device.windows))
        device.windows.clear()
        target = Path(self.tmp.name) / "out"
        m.tool_fetch(device, {"remote_path": "/live", "local_path": str(target)})
        self.assertEqual(target.read_bytes(), self.content)
        self.assertTrue(all(len(c) == w == 1 and "len=1024" in c[0]
                            for c, w, _ in device.windows[:-1]))

    def test_modern_empty_transfer_emits_one_write_and_no_read(self):
        self.source.write_bytes(b"")
        device = ModernDevice()
        result = m.tool_put(device, self.args)
        self.assertEqual(result["bytes"], 0)
        self.assertEqual(result["chunks"], 1)
        target = Path(self.tmp.name) / "out"
        m.tool_fetch(device, {"remote_path": "/live", "local_path": str(target)})
        self.assertEqual(target.read_bytes(), b"")
        self.assertFalse(any(c.startswith("fs.read ") for c in device.commands))

    def test_modern_corrupt_stage_and_failed_commit_keep_original(self):
        for kwargs in ({"corrupt": "stage"}, {"corrupt": "backup"}, {"rename_failure": "commit"}):
            with self.subTest(kwargs=kwargs):
                device = ModernDevice({"/live": b"old"}, **kwargs)
                with self.assertRaisesRegex(RuntimeError, "发布失败.*暂存路径.*备份路径.*回滚路径"):
                    m.tool_put(device, self.args)
                self.assertEqual(device.files["/live"], b"old")

    def test_modern_short_read_and_hash_mismatch_preserve_local_target(self):
        for failure in ("short", "hash"):
            with self.subTest(failure=failure):
                device = ModernDevice({"/live": self.content})
                original = device.call_one
                def broken(command, **kwargs):
                    if failure == "short" and command.startswith("fs.read "):
                        return ok("bytes=0", "#")
                    if failure == "hash" and command.startswith("fs.sha256 "):
                        return ok(f"bytes={len(self.content)} sha256=" + "0" * 64)
                    return original(command, **kwargs)
                device.call_one = broken
                target = Path(self.tmp.name) / "out"
                target.write_bytes(b"keep")
                with self.assertRaisesRegex(RuntimeError, "长度不符|摘要"):
                    m.tool_fetch(device, {"remote_path": "/live", "local_path": str(target)})
                self.assertEqual(target.read_bytes(), b"keep")
                self.assertEqual(list(target.parent.glob(".nxc-fetch-*")), [])

    def test_legacy_upload_streams_and_never_calls_new_methods(self):
        device = FileDevice({"/live": b"old"})
        with mock.patch.object(Path, "read_bytes", side_effect=AssertionError("whole-file read")), \
             mock.patch.object(m, "read_remote_file", side_effect=AssertionError("whole remote read")):
            result = m.tool_put(device, self.args)
        self.assertEqual(device.files["/live"], self.content)
        self.assertTrue(result["verified"])
        self.assertFalse(any(c.startswith(("fs.sha256", "fs.transfer_info")) for c in device.commands))

    def test_invalid_advertised_info_refused_before_writes(self):
        for info in ("write_chunk=0 read_chunk=24000 max_inflight=2 sha256=1",
                     "write_chunk=3061 read_chunk=24000 max_inflight=2 sha256=1",
                     "write_chunk=3060 read_chunk=24001 max_inflight=2 sha256=1",
                     "write_chunk=3060 read_chunk=24000 max_inflight=3 sha256=1",
                     "write_chunk=3060 read_chunk=24000 max_inflight=2 sha256=0",
                     "write_chunk=3060 read_chunk=24000 sha256=1"):
            with self.subTest(info=info):
                device = ModernDevice(info=info)
                with self.assertRaisesRegex(RuntimeError, "能力"):
                    m.tool_put(device, self.args)
                self.assertFalse(any(c.startswith("fs.write ") for c in device.commands))

    def test_capability_network_error_is_not_legacy_fallback(self):
        device = ModernDevice()
        original = device.call_one
        def failing(command, **kwargs):
            if command == "fs.transfer_info":
                raise ConnectionResetError("capability disconnect")
            return original(command, **kwargs)
        device.call_one = failing
        with self.assertRaisesRegex(ConnectionResetError, "disconnect"):
            m.tool_put(device, self.args)
        self.assertFalse(device.windows)

    def test_fetch_streams_into_temporary_and_bounds_requests_by_stat(self):
        device = ModernDevice({"/live": self.content})
        target = Path(self.tmp.name) / "out"
        with mock.patch.object(m, "read_remote_file", side_effect=AssertionError("whole remote read")):
            result = m.tool_fetch(device, {"remote_path": "/live", "local_path": str(target)})
        self.assertEqual(target.read_bytes(), self.content)
        self.assertEqual(result["bytes"], len(self.content))
        self.assertTrue(device.windows)
        for commands, _, retry in device.windows:
            self.assertFalse(retry)
            for command in commands:
                fields = dict(f.split("=", 1) for f in command.split()[1:])
                self.assertLess(int(fields["offset"]), len(self.content))
                self.assertLessEqual(int(fields["offset"]) + int(fields["len"]), len(self.content))
        self.assertEqual(list(target.parent.glob(".nxc-fetch-*")), [])

    def test_failed_fetch_preserves_destination_and_cleans_temporary(self):
        device = ModernDevice({"/live": self.content}, fail=lambda method, p: method == "fs.read")
        target = Path(self.tmp.name) / "out"
        target.write_bytes(b"keep")
        with self.assertRaises(Exception):
            m.tool_fetch(device, {"remote_path": "/live", "local_path": str(target)})
        self.assertEqual(target.read_bytes(), b"keep")
        self.assertEqual(list(target.parent.glob(".nxc-fetch-*")), [])

    def test_remote_hash_legacy_and_modern_and_invalid_digest(self):
        for cls in (FileDevice, ModernDevice):
            device = cls({"/live": self.content})
            with mock.patch.object(m, "read_remote_file", side_effect=AssertionError("whole remote read")):
                self.assertEqual(m.remote_file_hash(device, "/live"),
                                 {"bytes": len(self.content), "sha256": hashlib.sha256(self.content).hexdigest()})
        device = ModernDevice({"/live": self.content})
        original = device.call_one
        device.call_one = lambda c, **kw: ok("bytes=1 sha256=not-a-hash") if c.startswith("fs.sha256 ") else original(c, **kw)
        with self.assertRaisesRegex(RuntimeError, "摘要"):
            m.remote_file_hash(device, "/live")


if __name__ == "__main__":
    unittest.main()
