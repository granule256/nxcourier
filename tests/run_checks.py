#!/usr/bin/env python3
"""统一宿主回归入口；不连接真机，不启动常驻服务。"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CXX = shlex.split(os.environ.get("CXX", "c++"))
CPP_TESTS = (
    "tests/test_sdk_validation.cpp",
    "tests/test_job_queue.cpp",
    "tests/test_input_plan.cpp",
    "device/platform/tests/test_codec.cpp",
    "device/platform/tests/test_fixes.cpp",
    "device/platform/tests/test_toolinfo.cpp",
)


def main():
    suite = unittest.TestSuite()
    for directory in (ROOT / "tests", ROOT / "pc/tests"):
        # 两个目录独立发现，新增 test_*.py 无需维护手写清单。
        suite.addTests(unittest.TestLoader().discover(
            str(directory), pattern="test_*.py", top_level_dir=str(directory)))
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        return 1
    with tempfile.TemporaryDirectory(prefix="nxc-checks-") as temp:
        for index, relative in enumerate(CPP_TESTS):
            print(f"\n宿主 C++：{relative}", flush=True)
            binary = str(Path(temp) / f"test-{index}")
            subprocess.run(CXX + [
                "-std=c++17", "-Wall", "-Wextra", "-pthread",
                "-I" + str(ROOT / "device/platform/include"),
                "-I" + str(ROOT / "device/platform/source/core"),
                str(ROOT / relative), "-o", binary,
            ], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True)
    # test_files_saves_*.cpp 是 Python 测试自行编译的辅助程序，不重复独立运行。
    # test_poll_wait.py 针对旧永久 poll 设计；see_selftest 即使 --no-device 也会访问真机。
    for relative in (
        "pc/tests/helpers/deadconn.py",
        "device/platform/tests/test_listener_recovery.py",
        "device/platform/tests/check_error_msgs.py",
    ):
        print(f"\n宿主脚本：{relative}", flush=True)
        subprocess.run([sys.executable, str(ROOT / relative)], cwd=ROOT, check=True)
    print("\n宿主回归全部通过（未连接真机）。", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"宿主检查失败：{error}", file=sys.stderr)
        raise SystemExit(1)
