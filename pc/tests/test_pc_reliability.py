"""PC 可靠性回归；使用假设备及本地临时文件，不连接真机。"""
import base64
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from nxc_host import tools as m


class Socket:
    def __init__(self, transcript):
        self.stream = io.BytesIO(transcript)
        self.sent = []
        self.closed = False

    def sendall(self, data):
        self.sent.append(data)

    def close(self):
        self.closed = True


class Connection(m.Device):
    def __init__(self, transcripts):
        super().__init__("unused", 1)
        self.sockets = [Socket(t) for t in transcripts]
        self.connects = 0

    def _connect(self):
        self._close()
        self._sock = self.sockets[self.connects]
        self._stream = self._sock.stream
        self.connects += 1


def ok(*lines):
    return {"lines": list(lines)}


class FileDevice:
    """模拟设备原语及故障点，运行真实 PC 上传/读取实现。"""
    def __init__(self, files=None, fail=None, corrupt=None, modern=True, rename_failure=None):
        self.files = dict(files or {})
        self.commands = []
        self.fail = fail
        self.corrupt = corrupt
        self.modern = modern
        self.rename_failure = rename_failure
        self.rename_steps = []

    def call_one(self, command, **kwargs):
        self.commands.append(command)
        method, *fields = command.split(" ")
        p = {k: m.unescape_value(v) for k, v in (f.split("=", 1) for f in fields if "=" in f)}
        if self.fail and self.fail(method, p):
            raise ConnectionResetError("injected disconnect")
        if method == "tools.list":
            lines = ["tool=fs", "method=fs.write params=path*(str),data*(base64),append(0|1=0),allow_empty(0|1=0)"]
            if self.modern:
                lines.append("method=fs.rename params=src*(str),dst*(str),replace(0|1=0),rollback(str)")
            return ok(*lines)
        if method == "fs.stat":
            if p["path"] not in self.files:
                return {"error": {"code": 404, "message": "not-found"}}
            return ok(f"type=file size={len(self.files[p['path']])}")
        if method == "fs.write":
            path = p["path"]
            data = base64.b64decode(p["data"])
            self.files[path] = (self.files.get(path, b"") if p.get("append") == "1" else b"") + data
            if self.corrupt == "stage" and "stage-" in path:
                self.files[path] += b"bad"
            return ok(f"bytes={len(data)}")
        if method == "fs.read":
            path = p["path"]
            if path not in self.files:
                return {"error": {"code": 404, "message": "cannot-open-file"}}
            start, size = int(p["offset"]), int(p["len"])
            data = self.files[path][start:start + size]
            return ok(f"bytes={len(data)} offset={start} eof={int(len(data) < size)}", "#" + base64.b64encode(data).decode())
        if method == "fs.copy":
            self.files[p["dst"]] = self.files[p["src"]]
            if self.corrupt == "backup":
                self.files[p["dst"]] += b"bad"
            return ok()
        if method == "fs.rename":
            src, dst = p["src"], p["dst"]
            rollback = p.get("rollback")
            if dst in self.files:
                if not rollback:
                    return {"error": {"code": 400, "message": "rollback-path-required"}}
                if rollback in self.files or Path(rollback).parent != Path(dst).parent:
                    return {"error": {"code": 400, "message": "invalid-rollback-path"}}
                self.rename_steps.append((dst, rollback))
                self.files[rollback] = self.files.pop(dst)
            self.rename_steps.append((src, dst))
            if self.rename_failure:
                if rollback and rollback in self.files:
                    self.rename_steps.append((rollback, dst))
                    if self.rename_failure == "rollback":
                        return {"error": {"code": 500, "message": f"rename-rollback-failed rollback={rollback}"}}
                    self.files[dst] = self.files.pop(rollback)
                return {"error": {"code": 500, "message": "rename-commit-failed-restored"}}
            self.files[p["dst"]] = self.files.pop(p["src"])
            return ok()
        if method == "screen.capture":
            return ok("bytes=99 via=mock")
        raise AssertionError(command)

    def call(self, commands, **kwargs):
        return [self.call_one(c, **kwargs) for c in commands]


class ReliabilityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.source = Path(self.tmp.name) / "input.bin"
        self.source.write_bytes(b"new contents" * 100)
        self.args = {"local_path": str(self.source), "remote_path": "/module.nsp"}

    def tearDown(self):
        self.tmp.cleanup()

    def test_unacknowledged_mutation_is_not_replayed(self):
        d = Connection([b"", b"OK 0\n"])
        with self.assertRaisesRegex(RuntimeError, "未知"):
            d.call(["input.tap key=A"])
        self.assertEqual(d.connects, 1)
        self.assertIsNone(d._sock)

    def test_explicit_read_retry_and_second_failure_close(self):
        d = Connection([b"", b"OK 1\nidentity=mock\n"])
        self.assertEqual(d.call(["hello"], retry_safe=True)[0]["lines"], ["identity=mock"])
        d = Connection([b"", b""])
        with self.assertRaises(RuntimeError):
            d.call(["hello"], retry_safe=True)
        self.assertIsNone(d._sock)

    def test_batch_reports_completed_responses_and_uncertain_command(self):
        d = Connection([b"OK 0\nOK 2\nfirst\n"])
        with self.assertRaisesRegex(RuntimeError, "已完整应答 1.*第 2.*未知"):
            d.call(["fs.write a=1", "fs.write a=2"])
        self.assertEqual(d.connects, 1)

    def test_invalid_headers_and_body_close_connection(self):
        for transcript in (b"OK x\n", b"OK -1\n", b"ERR x nope\n", b"garbage\n", b"OK 1\nunterminated"):
            with self.subTest(transcript=transcript):
                d = Connection([transcript])
                with self.assertRaises(RuntimeError):
                    d.call(["hello"])
                self.assertIsNone(d._sock)

    def test_structured_percent_is_literal_and_raw_is_unchanged(self):
        class Echo:
            def call_one(self, command):
                return ok()
        self.assertIn("path=/100%2520%25", m.tool_call(Echo(), {"method": "fs.stat", "params": {"path": "/100%20%"}})["command"])
        raw = "fs.stat path=/100%20"
        self.assertEqual(m.tool_call(Echo(), {"raw": raw})["command"], raw)

    def test_structured_method_and_keys_reject_injection(self):
        for method, params in (("hello\nboot.reboot", {}), ("fs.stat", {"path\nboot.reboot": "/x"}), ("fs.stat", {"x=y": 1})):
            with self.subTest(method=method, params=params):
                with self.assertRaises(RuntimeError):
                    m._device_line(method, params)

    def test_empty_upload_emits_write_and_commits(self):
        self.source.write_bytes(b"")
        d = FileDevice({"/module.nsp": b"old"})
        result = m.tool_put(d, self.args)
        self.assertEqual(d.files["/module.nsp"], b"")
        self.assertTrue(result["verified"])
        self.assertTrue(any(c.startswith("fs.write ") and "data= " in c for c in d.commands))

    def test_upload_only_changes_target_at_rename_and_preserves_backup(self):
        d = FileDevice({"/module.nsp": b"old"})
        result = m.tool_deploy(d, self.args)
        self.assertTrue(result["verified"])
        self.assertEqual(d.files["/module.nsp"], self.source.read_bytes())
        self.assertEqual(d.files[result["backup"]], b"old")
        self.assertEqual(d.files[result["rollback"]], b"old")
        self.assertFalse(any(c.startswith("fs.write path=/module.nsp ") for c in d.commands))
        self.assertIn("replace=1", next(c for c in d.commands if c.startswith("fs.rename ")))

    def test_old_device_is_rejected_before_any_write(self):
        d = FileDevice({"/module.nsp": b"old"}, modern=False)
        with self.assertRaisesRegex(RuntimeError, "fs.rename"):
            m.tool_deploy(d, self.args)
        self.assertEqual(d.files, {"/module.nsp": b"old"})

    def test_each_precommit_disconnect_preserves_old_target(self):
        stages = [lambda mth, p: mth == "fs.write", lambda mth, p: mth == "fs.read" and "stage-" in p.get("path", ""), lambda mth, p: mth == "fs.copy", lambda mth, p: mth == "fs.read" and "backup-" in p.get("path", ""), lambda mth, p: mth == "fs.rename"]
        for stage in stages:
            with self.subTest(stage=stage):
                d = FileDevice({"/module.nsp": b"old"}, fail=stage)
                with self.assertRaises(Exception):
                    m.tool_deploy(d, self.args)
                self.assertEqual(d.files["/module.nsp"], b"old")

    def test_corrupt_stage_and_backup_are_mcp_errors(self):
        for corruption in ("stage", "backup"):
            with self.subTest(corruption=corruption):
                d = FileDevice({"/module.nsp": b"old"}, corrupt=corruption)
                with contextlib.redirect_stderr(io.StringIO()):
                    reply = m.handle({"id": 1, "method": "tools/call", "params": {"name": "nxc_deploy", "arguments": self.args}}, d)
                self.assertTrue(reply["result"].get("isError"))
                self.assertEqual(d.files["/module.nsp"], b"old")

    def test_verify_false_cannot_bypass_publication_verification(self):
        d = FileDevice({"/module.nsp": b"old"}, corrupt="stage")
        with self.assertRaises(RuntimeError):
            m.tool_put(d, dict(self.args, verify=False))
        self.assertEqual(d.files["/module.nsp"], b"old")

    def test_read_error_is_not_silent_eof(self):
        d = FileDevice({"/x": b"abc"})
        original = d.call_one
        def read_error(command, **kwargs):
            if "offset=3 " in command:
                return {"error": {"code": 403, "message": "outside-safe-roots"}}
            return original(command, **kwargs)
        d.call_one = read_error
        with self.assertRaisesRegex(RuntimeError, "403.*outside-safe-roots"):
            m.read_remote_file(d, "/x", chunk=3)

    def test_fetch_empty_file_succeeds(self):
        target = str(Path(self.tmp.name) / "empty")
        result = m.tool_fetch(FileDevice({"/empty": b""}), {"remote_path": "/empty", "local_path": target})
        self.assertEqual(result["bytes"], 0)
        self.assertEqual(Path(target).read_bytes(), b"")

    def test_capture_checks_declared_length(self):
        with self.assertRaisesRegex(RuntimeError, "99"):
            m._grab_frame(FileDevice({"/shot": b"short"}), None, 0, "/shot")

    def test_empty_capability_is_required_and_empty_write_is_explicit(self):
        self.source.write_bytes(b"")
        d = FileDevice()
        original = d.call_one
        def missing_empty(command, **kwargs):
            result = original(command, **kwargs)
            if command.startswith("tools.list"):
                result["lines"] = [line.replace(",allow_empty(0|1=0)", "") for line in result["lines"]]
            return result
        d.call_one = missing_empty
        with self.assertRaisesRegex(RuntimeError, "allow_empty"):
            m.tool_put(d, self.args)
        self.assertFalse(d.files)
        d = FileDevice()
        m.tool_put(d, self.args)
        writes = [c for c in d.commands if c.startswith("fs.write ")]
        self.assertEqual(len(writes), 1)
        self.assertIn("allow_empty=1", writes[0])
        self.assertIn("append=0", writes[0])

    def test_new_target_skips_backup_and_each_upload_has_unique_stage(self):
        d = FileDevice()
        first = m.tool_put(d, self.args)
        self.assertIsNone(first["backup"])
        self.assertIsNone(first["rollback"])
        self.assertFalse(any(c.startswith("fs.copy ") for c in d.commands))
        m.tool_put(d, self.args)
        stages = {c.split(" path=", 1)[1].split(" ", 1)[0] for c in d.commands if c.startswith("fs.write ")}
        self.assertEqual(len(stages), 2)

    def test_read_errors_and_malformed_bodies_are_never_eof(self):
        responses = [({"error": {"code": 404, "message": "cannot-open-file"}}, "404.*cannot-open-file"),
                     (ok("bytes=0"), "缺少"), (ok("bytes=1", "#%%%"), "base64"),
                     (ok("bytes=2", "#YQ=="), "长度")]
        for response, error in responses:
            with self.subTest(response=response):
                class BrokenRead:
                    def call_one(self, command, **kwargs):
                        return response
                with self.assertRaisesRegex(RuntimeError, error):
                    m.read_remote_file(BrokenRead(), "/x")

    def test_actual_read_length_advances_offsets(self):
        d = FileDevice({"/x": b"0123456789"})
        original = d.call_one
        def limit_read(command, **kwargs):
            if command.startswith("fs.read "):
                command = command.replace("len=24000", "len=3")
            return original(command, **kwargs)
        d.call_one = limit_read
        self.assertEqual(m.read_remote_file(d, "/x"), b"0123456789")
        offsets = [int(c.split("offset=", 1)[1].split()[0]) for c in d.commands if c.startswith("fs.read ")]
        self.assertEqual(offsets, [0, 3, 6, 9, 10])

    def test_device_rejection_during_stage_write_is_mcp_error(self):
        d = FileDevice({"/module.nsp": b"old"})
        original = d.call_one
        def denied(command, **kwargs):
            if command.startswith("fs.write "):
                return {"error": {"code": 507, "message": "disk-full"}}
            return original(command, **kwargs)
        d.call_one = denied
        with contextlib.redirect_stderr(io.StringIO()):
            reply = m.handle({"id": 1, "method": "tools/call", "params": {"name": "nxc_put", "arguments": self.args}}, d)
        self.assertTrue(reply["result"].get("isError"))
        self.assertIn("disk-full", reply["result"]["content"][0]["text"])
        self.assertEqual(d.files["/module.nsp"], b"old")

    def test_repeated_headers_cannot_consume_next_frames_as_body(self):
        d = Connection([b"OK 1\n\xff\nOK 0\n"])
        with self.assertRaises(RuntimeError):
            d.call(["fs.write x=y"])
        self.assertIsNone(d._sock)

    def test_read_requires_valid_byte_count_before_accepting_eof(self):
        for response in (ok("#"), ok("bytes=-1", "#"), ok("bytes=bad", "#")):
            with self.subTest(response=response):
                class BrokenRead:
                    def call_one(self, command, **kwargs):
                        return response
                with self.assertRaisesRegex(RuntimeError, "字节数"):
                    m.read_remote_file(BrokenRead(), "/x")

    def test_long_target_names_use_short_helpers_in_same_parent(self):
        class NameCheckedDevice(FileDevice):
            def call_one(device, command, **kwargs):
                method, *fields = command.split(" ")
                params = {k: m.unescape_value(v) for k, v in
                          (field.split("=", 1) for field in fields if "=" in field)}
                dest = params.get("path") if method == "fs.write" else params.get("dst")
                if dest and method in ("fs.write", "fs.copy", "fs.rename"):
                    # 使用真实文件系统发现字典模型没有的 basename 上限。
                    (Path(self.tmp.name) / Path(dest).name).write_bytes(b"")
                return super().call_one(command, **kwargs)

        for name, existing in (("x" * 216, True), ("x" * 217, False),
                               ("x" * 255, True), ("中" * 85, True), ("文" * 85, False)):
            with self.subTest(name_bytes=len(name.encode()), existing=existing):
                remote = "/目录/子 目录/" + name
                (Path(self.tmp.name) / name).write_bytes(b"old")
                device = NameCheckedDevice({remote: b"old"} if existing else {})
                result = m.tool_put(device, dict(self.args, remote_path=remote))
                self.assertEqual(result["remote"], remote)
                self.assertEqual(device.files[remote], self.source.read_bytes())
                writes = [c for c in device.commands if c.startswith("fs.write ")]
                stage = m.unescape_value(writes[0].split("path=", 1)[1].split(" ", 1)[0])
                self.assertEqual(Path(stage).parent, Path(remote).parent)
                self.assertRegex(Path(stage).name, r"^\.nxc-stage-[a-f0-9]{32}$")
                if existing:
                    self.assertEqual(Path(result["backup"]).parent, Path(remote).parent)
                    self.assertRegex(Path(result["backup"]).name, r"^\.nxc-backup-[a-f0-9]{32}$")
                    self.assertEqual(device.files[result["backup"]], b"old")
                    self.assertEqual(Path(result["rollback"]).parent, Path(remote).parent)
                    self.assertRegex(Path(result["rollback"]).name, r"^\.nxc-rollback-[a-f0-9]{32}$")
                    self.assertEqual(device.files[result["rollback"]], b"old")

    def test_publication_errors_include_stage_and_backup_paths(self):
        for operation in ("fs.write", "fs.copy"):
            with self.subTest(operation=operation):
                device = FileDevice({"/module.nsp": b"old"},
                                    fail=lambda method, params: method == operation)
                with mock.patch.object(m.uuid, "uuid4", return_value=mock.Mock(hex="1" * 32)):
                    with self.assertRaises(RuntimeError) as failure:
                        m.tool_put(device, self.args)
                self.assertIn("/.nxc-stage-" + "1" * 32, str(failure.exception))
                self.assertIn("/.nxc-backup-" + "1" * 32, str(failure.exception))
                self.assertIn("/.nxc-rollback-" + "1" * 32, str(failure.exception))
                self.assertEqual(device.files["/module.nsp"], b"old")

    def test_executed_rename_with_lost_reply_is_unknown_and_not_replayed(self):
        device = FileDevice({"/module.nsp": b"old"})
        original = device.call_one
        transport = Connection([b"", b"OK 0\n"])
        def lose_rename_reply(command, **kwargs):
            result = original(command, **kwargs)
            if command.startswith("fs.rename "):
                # 设备执行了 rename，随后由真实 Device.call 处理应答丢失。
                return transport.call_one(command, **kwargs)
            return result
        device.call_one = lose_rename_reply
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                reply = m.handle({"id": 1, "method": "tools/call", "params":
                                  {"name": "nxc_put", "arguments": self.args}}, device)
            self.assertTrue(reply["result"].get("isError"))
            message = reply["result"]["content"][0]["text"]
            self.assertIn("执行结果未知", message)
            self.assertIn("未自动重放", message)
            self.assertEqual(transport.connects, 1)
            self.assertEqual(len(transport.sockets[0].sent), 1)
            self.assertEqual(device.files["/module.nsp"], self.source.read_bytes())
            backup = next(path for path in device.files if "backup-" in path)
            self.assertEqual(device.files[backup], b"old")
            self.assertIn(backup, message)
            rollback = next(path for path in device.files if "rollback-" in path)
            self.assertEqual(device.files[rollback], b"old")
            self.assertIn(rollback, message)
            self.assertIsNone(transport._sock)
        finally:
            transport.close()

    def test_rename_without_rollback_capability_is_rejected_before_write(self):
        device = FileDevice({"/module.nsp": b"old"})
        original = device.call_one
        def legacy_rename(command, **kwargs):
            response = original(command, **kwargs)
            if command.startswith("tools.list"):
                response["lines"] = [line.replace(",rollback(str)", "") for line in response["lines"]]
            return response
        device.call_one = legacy_rename
        with self.assertRaisesRegex(RuntimeError, "rollback"):
            m.tool_put(device, self.args)
        self.assertEqual(device.files, {"/module.nsp": b"old"})
        self.assertFalse(any(c.startswith("fs.write ") for c in device.commands))

    def test_failed_commit_restores_old_target_and_preserves_verified_backup(self):
        device = FileDevice({"/module.nsp": b"old"}, rename_failure="commit")
        with self.assertRaisesRegex(RuntimeError, "commit-failed-restored") as failure:
            m.tool_put(device, self.args)
        self.assertEqual(device.files["/module.nsp"], b"old")
        backup = next(path for path in device.files if "backup-" in path)
        stage = next(path for path in device.files if "stage-" in path)
        self.assertEqual(device.files[backup], b"old")
        self.assertEqual(device.files[stage], self.source.read_bytes())
        self.assertEqual(len(device.rename_steps), 3)
        rollback = device.rename_steps[0][1]
        self.assertEqual(device.rename_steps[1], (stage, "/module.nsp"))
        self.assertEqual(device.rename_steps[2], (rollback, "/module.nsp"))
        self.assertNotIn(rollback, device.files)
        self.assertIn(rollback, str(failure.exception))

    def test_failed_rollback_reports_recoverable_paths_without_replaying_commit(self):
        device = FileDevice({"/module.nsp": b"old"}, rename_failure="rollback")
        with contextlib.redirect_stderr(io.StringIO()):
            reply = m.handle({"id": 1, "method": "tools/call", "params":
                              {"name": "nxc_put", "arguments": self.args}}, device)
        self.assertTrue(reply["result"].get("isError"))
        message = reply["result"]["content"][0]["text"]
        self.assertIn("rename-rollback-failed", message)
        self.assertNotIn("/module.nsp", device.files)
        rollback = next(path for path in device.files if "rollback-" in path)
        backup = next(path for path in device.files if "backup-" in path)
        stage = next(path for path in device.files if "stage-" in path)
        for path, expected in ((rollback, b"old"), (backup, b"old"),
                               (stage, self.source.read_bytes())):
            self.assertEqual(device.files[path], expected)
            self.assertIn(path, message)
        self.assertEqual(len([c for c in device.commands if c.startswith("fs.rename ")]), 1)
        self.assertEqual(device.rename_steps, [("/module.nsp", rollback),
                                              (stage, "/module.nsp"), (rollback, "/module.nsp")])


if __name__ == "__main__":
    unittest.main()
