"""真实回环 TCP/后台子进程/HTTP/CLI/MCP/zipapp 集成；只用模拟设备。"""
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc/src"))
sys.path.insert(0, str(ROOT / "pc/tests"))
from nxc_host.client import HostClient
from nxc_host.config import Config
from nxc_host.device import Device, parse_payload


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class HostIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue((ROOT / "pc/tests/helpers/simulated_device.py").is_file(),
                        "需要明确标识的回环 TCP 模拟设备")
        fixture = importlib.import_module("helpers.simulated_device")
        self.simulator = fixture.SimulatedDevice().start()
        self.directory = tempfile.TemporaryDirectory(prefix="nxc-integration-")
        self.config = Config(device_ip="127.0.0.1", device_port=self.simulator.port,
                             timeout=2, host_port=free_port())
        env = {key: value for key, value in os.environ.items() if not key.startswith("NXC_")}
        env["NXC_CONFIG_DIR"] = self.directory.name
        self.environment = patch.dict(os.environ, env, clear=True)
        self.environment.start()
        self.client = HostClient(self.config)
        self.clients = [self.client]
        self.pids = set()

    def tearDown(self):
        cleanup_errors = []
        for client in reversed(getattr(self, "clients", [])):
            try:
                status = client.status()
                if status.get("running"):
                    self.pids.add(status["pid"])
                    client.stop()
            except Exception as error:
                cleanup_errors.append(str(error))
        for pid in getattr(self, "pids", ()):
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                try:
                    reaped, _ = os.waitpid(pid, os.WNOHANG)
                    if reaped:
                        break
                except ChildProcessError:
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        break
                time.sleep(0.02)
            else:
                # 只清理本测试实际启动并记录的PID，避免失败时留常驻进程。
                import signal
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                cleanup_errors.append(f"测试后台未正常退出：{pid}")
        if hasattr(self, "simulator"):
            self.simulator.close()
        if hasattr(self, "environment"):
            self.environment.stop()
        if hasattr(self, "directory"):
            self.directory.cleanup()
        self.assertFalse(cleanup_errors, cleanup_errors)

    def start(self):
        status = self.client.start()
        self.pids.add(status["pid"])
        return status

    def cli(self, *args, entry=None, stdin=None, cwd=None):
        env = dict(os.environ)
        env.pop("PYTHONPATH", None)
        command = [sys.executable, str(entry or ROOT / "nxc"), "--json", *args]
        return subprocess.run(command, input=stdin, cwd=cwd or self.directory.name,
                              env=env, capture_output=True, text=True, timeout=30)

    def rpc(self, method, params=None):
        reply = self.client.request({"jsonrpc": "2.0", "id": 1, "method": method,
                                     "params": params or {}})
        self.assertNotIn("error", reply, reply)
        return reply["result"]

    def test_simulator_long_connection_pagination_and_identity(self):
        with_device = Device("127.0.0.1", self.simulator.port, 2)
        try:
            hello = parse_payload(with_device.call_one("hello")["lines"])[0]
            self.assertEqual(hello["platform"], "nxc-simulator")
            self.assertEqual(hello["via"], "simulator")
            result = parse_payload(with_device.call_one("fs.ls path=/config/nxc offset=0 count=2")["lines"])
            self.assertEqual(sum("name" in row for row in result), 2)
            self.assertEqual(result[-1]["next_offset"], "2")
            self.assertEqual(self.simulator.connection_count, 1)
        finally:
            with_device.close()

    def test_simulator_rename_requires_explicit_rollback_for_existing_target(self):
        self.simulator.files["/config/nxc/source"] = b"new simulator"
        self.simulator.files["/config/nxc/target"] = b"old simulator"
        device = Device("127.0.0.1", self.simulator.port, 2)
        try:
            denied = device.call_one("fs.rename src=/config/nxc/source dst=/config/nxc/target replace=1")
            self.assertEqual(denied["error"]["code"], 409)
            self.assertEqual(self.simulator.files["/config/nxc/target"], b"old simulator")
            self.assertEqual(self.simulator.files["/config/nxc/source"], b"new simulator")
            accepted = device.call_one("fs.rename src=/config/nxc/source dst=/config/nxc/target replace=1 rollback=/config/nxc/rollback")
            self.assertNotIn("error", accepted)
            self.assertEqual(self.simulator.files["/config/nxc/target"], b"new simulator")
            self.assertNotIn("/config/nxc/rollback", self.simulator.files)
        finally:
            device.close()

    def test_simulator_script_reports_loopback_port_and_exits_on_termination(self):
        process = subprocess.Popen([sys.executable, str(ROOT / "pc/tests/helpers/simulated_device.py")],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            state = json.loads(process.stdout.readline())
            self.assertEqual(state["ip"], "127.0.0.1")
            self.assertEqual(state["platform"], "nxc-simulator")
            device = Device(state["ip"], state["port"], 2)
            try:
                self.assertIn("via=simulator", device.call_one("hello")["lines"][0])
            finally:
                device.close()
            process.terminate()
            _, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, stderr)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def test_actual_start_cli_mcp_share_pid_device_and_stop_releases_socket(self):
        self.assertFalse(self.client.status()["running"])
        self.assertEqual(self.simulator.connection_count, 0)
        status = self.start()
        self.assertEqual(self.simulator.connection_count, 0, "启动/状态不能连接设备")
        self.assertIn("instance_id", status)
        self.assertIn("config_fingerprint", status)
        result = self.cli("call", "identify.id")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["payload"][0]["platform"], "nxc-simulator")
        messages = [{"jsonrpc": "2.0", "id": 2, "method": "host.status"},
                    {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
                        "name": "nxc_call", "arguments": {"method": "input.seq", "params": {"steps": "A:80"}}}}]
        mcp = self.cli("mcp", stdin="".join(json.dumps(item) + "\n" for item in messages))
        self.assertEqual(mcp.returncode, 0, mcp.stderr)
        replies = [json.loads(line) for line in mcp.stdout.splitlines()]
        self.assertEqual(replies[0]["result"]["pid"], status["pid"])
        self.assertEqual(self.client.status()["pid"], status["pid"])
        self.assertEqual(self.simulator.connection_count, 1)
        self.assertEqual(sum(command.startswith("input.seq ") for command in self.simulator.commands), 1)
        self.client.stop()
        self.assertFalse(self.client.status()["running"])
        deadline = time.monotonic() + 3
        while self.simulator.active_connections and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(self.simulator.active_connections, 0)
        self.assertFalse((Path(self.directory.name) / "service.json").exists())

    def test_concurrent_starts_produce_same_pid(self):
        other = HostClient(self.config)
        self.clients.append(other)
        with ThreadPoolExecutor(max_workers=2) as pool:
            states = list(pool.map(lambda client: client.start(), [self.client, other]))
        self.pids.update(state["pid"] for state in states)
        self.assertEqual(states[0]["pid"], states[1]["pid"])
        self.assertEqual(states[0]["instance_id"], states[1]["instance_id"])
        self.assertEqual(self.simulator.connection_count, 0)

    def test_http_binding_web_transfer_backup_and_screenshot(self):
        self.start()
        request = urllib.request.Request(self.client.url + "/rpc", data=json.dumps({
            "jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                "name": "nxc_call", "arguments": {"method": "input.seq", "params": {"steps": "A:80"}}}}).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=3) as response:
            denied = json.load(response)
        self.assertEqual(denied["error"]["code"], -32009)
        self.assertEqual(self.simulator.commands, [])
        path = "/config/nxc/readme.txt"
        original = self.simulator.files[path]
        data = "模拟上传含空格与%字符\n".encode()
        upload = self.rpc("host.upload", {"remote_path": path, "data": base64.b64encode(data).decode()})
        self.assertTrue(upload["verified"])
        self.assertEqual(self.simulator.files[upload["backup"]], original)
        self.assertEqual(self.simulator.files[path], data)
        self.assertEqual(sum(command.startswith("fs.rename ") for command in self.simulator.commands), 1)
        download = self.rpc("host.download", {"remote_path": path})
        self.assertEqual(base64.b64decode(download["data"]), data)
        self.assertEqual(download["sha256"], hashlib.sha256(data).hexdigest())
        screen = self.rpc("tools/call", {"name": "nxc_see", "arguments": {"max_width": 0}})
        self.assertFalse(screen.get("isError"), screen)
        self.assertTrue(base64.b64decode(screen["content"][1]["data"]).startswith(b"\xff\xd8\xff"))
        self.assertIn("via=simulator", screen["content"][0]["text"])

    def test_zipapp_outside_checkout_start_mcp_web_version_and_stop(self):
        output = Path(self.directory.name) / "nxc"
        subprocess.run([sys.executable, str(ROOT / "pc/build_app.py"), "--output", str(output)],
                       check=True, capture_output=True, text=True)
        start = self.cli("--ip", "127.0.0.1", "--port", str(self.simulator.port),
                         "--host-port", str(self.config.host_port), "--timeout", "2", "start", entry=output)
        self.assertEqual(start.returncode, 0, start.stderr)
        status = json.loads(start.stdout)
        self.pids.add(status["pid"])
        self.assertEqual(status["version"], (ROOT / "VERSION").read_text().strip())
        self.assertEqual(self.simulator.connection_count, 0)
        current = self.cli("status", entry=output)
        self.assertEqual(current.returncode, 0, current.stderr)
        self.assertEqual(json.loads(current.stdout)["pid"], status["pid"])
        messages = [{"jsonrpc": "2.0", "id": 1, "method": "initialize"},
                    {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}]
        mcp = self.cli("mcp", entry=output, stdin="".join(json.dumps(item) + "\n" for item in messages))
        self.assertEqual(mcp.returncode, 0, mcp.stderr)
        replies = [json.loads(line) for line in mcp.stdout.splitlines()]
        self.assertEqual(replies[0]["result"]["serverInfo"]["version"], status["version"])
        self.assertIn("nxc_see", [tool["name"] for tool in replies[1]["result"]["tools"]])
        for path, marker in [("/", b"<!DOCTYPE html>"), ("/assets/app.js", b"host.status"),
                             ("/assets/app.css", b"font")]:
            with urllib.request.urlopen(self.client.url + path, timeout=3) as response:
                self.assertIn(marker.lower(), response.read().lower())
        self.assertEqual(self.cli("stop", entry=output).returncode, 0)
        self.assertFalse(self.client.status()["running"])


if __name__ == "__main__":
    unittest.main()
