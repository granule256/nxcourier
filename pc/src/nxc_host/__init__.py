"""NXC PC 主机服务；Python 3.10+，标准库运行时。"""
from importlib.resources import files
from pathlib import Path


def _read_version() -> str:
    try:
        return files(__package__).joinpath("VERSION").read_text(encoding="utf-8").strip()
    except (OSError, FileNotFoundError):
        # 源码树布局：仓库/pc/src/nxc_host；zipapp 使用上面的包内资源。
        try:
            return (Path(__file__).resolve().parents[3] / "VERSION").read_text(encoding="utf-8").strip()
        except OSError:
            return "0.0.0-unknown"


__version__ = _read_version()
