#!/usr/bin/env python3
"""用 Python 标准库构建可执行 zipapp；运行机器仍需 Python 3.10+。"""
import argparse
from pathlib import Path
import shutil
import tempfile
import zipapp


def build(output: Path) -> Path:
    pc = Path(__file__).resolve().parent
    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nxc-build-") as directory:
        staging = Path(directory)
        shutil.copytree(pc / "src/nxc_host", staging / "nxc_host",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"))
        shutil.copyfile(pc.parent / "VERSION", staging / "nxc_host/VERSION")
        shutil.copyfile(pc.parent / "LICENSE", staging / "LICENSE")
        (staging / "__main__.py").write_text(
            "from nxc_host.cli import main\nraise SystemExit(main())\n", encoding="utf-8")
        zipapp.create_archive(staging, output, interpreter="/usr/bin/env python3",
                              compressed=True)
    output.chmod(output.stat().st_mode | 0o111)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parent / "dist/nxc",
                        help="输出路径，默认 pc/dist/nxc")
    print(f"已构建：{build(parser.parse_args().output)}（运行需要 Python 3.10+）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
