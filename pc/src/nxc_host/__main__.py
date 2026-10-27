"""python -m nxc_host 和 zipapp 统一入口。"""
from .cli import main

if __name__ == '__main__':
    raise SystemExit(main())
