"""源码与 zipapp 的公开入口回归，不连接真机。"""
import importlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc/src"))


class PackageTests(unittest.TestCase):
    def test_package_version_and_injected_gdb_preserve_mcp_shape(self):
        self.assertTrue((ROOT / "pc/src/nxc_host/__init__.py").is_file(),
                        "正式 nxc_host 包尚未迁移")
        package = importlib.import_module("nxc_host")
        tools = importlib.import_module("nxc_host.tools")
        self.assertEqual(package.__version__, (ROOT / "VERSION").read_text().strip())
        calls = []
        def gdb(action, args):
            calls.append((action, args))
            return {"status": "已连接"}
        reply = tools.handle({"id": 7, "method": "tools/call", "params": {
            "name": "nxc_gdb", "arguments": {"action": "status"}}}, None, gdb)
        self.assertEqual(calls, [("status", {"action": "status"})])
        self.assertEqual(reply["id"], 7)
        self.assertEqual(json.loads(reply["result"]["content"][0]["text"]),
                         {"status": "已连接"})

    def test_zipapp_runs_from_outside_checkout_with_embedded_version(self):
        self.assertTrue((ROOT / "pc/build_app.py").is_file(), "zipapp 构建入口尚未实现")
        with tempfile.TemporaryDirectory(prefix="nxc-package-") as directory:
            output = Path(directory) / "nxc"
            subprocess.run([sys.executable, str(ROOT / "pc/build_app.py"),
                            "--output", str(output)], cwd=directory, check=True,
                           capture_output=True, text=True)
            env = dict(os.environ)
            env.pop("PYTHONPATH", None)
            result = subprocess.run([sys.executable, str(output), "--version"],
                                    cwd=directory, env=env, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn((ROOT / "VERSION").read_text().strip(), result.stdout)
            self.assertTrue(os.access(output, os.X_OK))


if __name__ == "__main__":
    unittest.main()
