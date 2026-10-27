#!/usr/bin/env python3
"""仅监听 127.0.0.1 的 NXC 协议模拟器；所有数据都明确标记 simulator。

导入：SimulatedDevice(port=0).start()，使用后 close()；也支持 with。
手工：python3 pc/tests/helpers/simulated_device.py --port 47802
首行输出 JSON 地址/端口；Ctrl-C 关闭线程及所有 TCP 连接。不访问局域网。
"""
from __future__ import annotations
import argparse
import base64
import json
from pathlib import Path, PurePosixPath
import re
import signal
import socket
import threading


def decode(value):
    return re.sub(r"%([0-9a-fA-F]{2})", lambda match: chr(int(match[1], 16)), value)


def encode(value):
    return "".join(f"%{ord(char):02X}" if char == "%" or char.isspace() or ord(char) < 32
                   else char for char in str(value))


class ProtocolError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


METHODS = {
    "identify": [("id", "", "read", "模拟设备身份")],
    "tele": [("all", "", "read", "模拟温度电量")],
    "title": [("current", "", "read", "模拟当前应用")],
    "screen": [("capture", "path(str),timeout_ms(int=5000)", "read", "模拟截图"),
               ("wait_stable", "timeout_ms(int=5000)", "read", "模拟画面稳定"),
               ("wait_change", "timeout_ms(int=5000)", "read", "模拟画面变化")],
    "input": [("seq", "steps*(str)", "action", "记录模拟按键，不操作真机")],
    "fs": [("ls", "path*(str),offset(int=0),count(int=50)", "read", "内存目录分页"),
           ("stat", "path*(str)", "read", "内存文件属性"),
           ("read", "path*(str),offset(int=0),len(int=24000)", "read", "读取内存文件"),
           ("write", "path*(str),data*(base64),append(0|1=0),allow_empty(0|1=0)", "write", "写入内存文件"),
           ("copy", "src*(str),dst*(str)", "write", "复制内存文件"),
           ("rename", "src*(str),dst*(str),replace(0|1=0),rollback(str)", "write", "模拟Horizon重命名与回滚")],
    "log": [("dump", "lines(int=40)", "read", "模拟日志")],
}


class SimulatedDevice:
    def __init__(self, port=0, files=None):
        self.port = port
        self.files = {"/config/nxc/readme.txt": "NXC 模拟设备，不是真机。via=simulator\n".encode(),
                      "/config/nxc/含 空格%文件.txt": b"simulator percent and spaces\n",
                      "/config/nxc/empty.txt": b""}
        self.files.update({f"/config/nxc/page-{index:02}.txt": f"simulator {index}\n".encode()
                           for index in range(55)})
        if files is not None:
            self.files.update(files)
        self.jpeg = Path(__file__).with_name("simulator.jpg").read_bytes()
        self.commands = []
        self.connection_count = 0
        self._lock = threading.RLock()
        self._connections = set()
        self._workers = []
        self._stopping = threading.Event()
        self._listener = None
        self._thread = None

    @property
    def active_connections(self):
        with self._lock:
            return len(self._connections)

    def start(self):
        if self._listener is not None:
            return self
        self._listener = socket.socket()
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", self.port))
        self.port = self._listener.getsockname()[1]
        self._listener.listen(8)
        self._listener.settimeout(0.1)
        self._thread = threading.Thread(target=self._accept, name="nxc-simulator", daemon=True)
        self._thread.start()
        return self

    def _accept(self):
        while not self._stopping.is_set():
            try:
                connection, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self._lock:
                self.connection_count += 1
                self._connections.add(connection)
                worker = threading.Thread(target=self._serve, args=(connection,),
                                          name="nxc-simulator-client", daemon=True)
                self._workers.append(worker)
            worker.start()

    def _serve(self, connection):
        try:
            with connection, connection.makefile("rb") as stream:
                while not self._stopping.is_set():
                    raw = stream.readline(8193)
                    if not raw:
                        return
                    command = raw.decode("utf-8").rstrip("\r\n")
                    with self._lock:
                        self.commands.append(command)
                        try:
                            lines = self._dispatch(command)
                            reply = f"OK {len(lines)}\n" + "".join(line + "\n" for line in lines)
                        except ProtocolError as error:
                            reply = f"ERR {error.code} {error.message}\n"
                        except (ValueError, KeyError, TypeError):
                            reply = "ERR 400 simulator-invalid-arguments\n"
                    connection.sendall(reply.encode())
        except (OSError, UnicodeError):
            pass
        finally:
            with self._lock:
                self._connections.discard(connection)

    def _directories(self):
        directories = {"/"}
        for name in self.files:
            directories.update(str(parent) for parent in PurePosixPath(name).parents)
        return directories

    def _file(self, path):
        if path not in self.files:
            raise ProtocolError(404, "simulator-file-not-found")
        return self.files[path]

    def _dispatch(self, command):
        method, *fields = command.split(" ")
        args = {key: decode(value) for key, separator, value in
                (field.partition("=") for field in fields) if separator}
        identity = "platform=nxc-simulator via=simulator version=simulator-1 boot=1 firmware=simulated"
        if method in ("hello", "identify.id"):
            return [identity]
        if method == "tools.list":
            rows = []
            for tool, methods in METHODS.items():
                if args.get("tool") and args["tool"] != tool:
                    continue
                rows.append(f"tool={tool} methods={len(methods)} tool_doc=1 help=simulator")
                rows.extend(f"method={tool}.{name} risk={risk} params={params} help={encode(help_text)}"
                            for name, params, risk, help_text in methods)
            return rows
        if method == "tools.doc":
            return [f"#模拟器方法 {args.get('name', 'unknown')}：仅访问内存夹具，不操作真机。",
                    "via=simulator platform=nxc-simulator"]
        if method == "tele.all":
            return ["via=simulator battery_percent=88 temperature_soc=42 temperature_pcb=35 charging=1"]
        if method == "title.current":
            return ["running=application pid=42 app_id=0100000000000001 name=NXC%20Simulator via=simulator"]
        if method in ("screen.wait_stable", "screen.wait_change"):
            return ["stable=1 changed=1 frames=3 elapsed_ms=20 via=simulator"]
        if method == "screen.capture":
            path = args.get("path", "/config/nxc/screen.jpg")
            self.files[path] = self.jpeg
            return [f"path={encode(path)} bytes={len(self.jpeg)} via=simulator magic=ffd8ff"]
        if method == "input.seq":
            return [f"steps={len(args.get('steps', '').split(';'))} elapsed_ms=80 via=simulator"]
        if method == "log.dump":
            return ["#NXC simulator: 所有请求仅在127.0.0.1和内存中执行。"]
        if method == "fs.ls":
            path = str(PurePosixPath(args.get("path", "/")))
            directories = self._directories()
            if path not in directories:
                raise ProtocolError(404, "simulator-directory-not-found")
            entries = []
            for directory in directories - {"/"}:
                if str(PurePosixPath(directory).parent) == path:
                    entries.append((PurePosixPath(directory).name, "dir", 0))
            entries.extend((PurePosixPath(name).name, "file", len(data))
                           for name, data in self.files.items() if str(PurePosixPath(name).parent) == path)
            entries.sort()
            offset = max(0, int(args.get("offset", 0)))
            count = max(1, min(100, int(args.get("count", 50))))
            page = entries[offset:offset + count]
            next_offset = offset + len(page) if offset + len(page) < len(entries) else -1
            return [f"name={encode(name)} type={kind} size={size}" for name, kind, size in page] + [
                f"offset={offset} count={len(page)} total={len(entries)} next_offset={next_offset} via=simulator"]
        if method == "fs.stat":
            path = args["path"]
            if path in self.files:
                return [f"path={encode(path)} type=file size={len(self.files[path])} via=simulator"]
            if path in self._directories():
                return [f"path={encode(path)} type=dir size=0 via=simulator"]
            raise ProtocolError(404, "simulator-file-not-found")
        if method == "fs.read":
            data = self._file(args["path"])
            offset = max(0, int(args.get("offset", 0)))
            piece = data[offset:offset + min(24000, max(0, int(args.get("len", 24000))))]
            return [f"bytes={len(piece)} eof={int(offset + len(piece) >= len(data))} via=simulator",
                    "#" + base64.b64encode(piece).decode()]
        if method == "fs.write":
            path = args["path"]
            data = base64.b64decode(args.get("data", ""), validate=True)
            if not data and args.get("allow_empty") != "1":
                raise ProtocolError(400, "simulator-empty-write-needs-allow_empty")
            self.files[path] = (self.files.get(path, b"") if args.get("append") == "1" else b"") + data
            return [f"bytes={len(data)} via=simulator"]
        if method == "fs.copy":
            data = self._file(args["src"])
            if args["dst"] in self.files:
                raise ProtocolError(409, "simulator-destination-exists")
            self.files[args["dst"]] = data
            return [f"bytes={len(data)} via=simulator"]
        if method == "fs.rename":
            src, dst = args["src"], args["dst"]
            self._file(src)
            if dst in self.files:
                rollback = args.get("rollback")
                if args.get("replace") != "1" or not rollback or rollback in self.files:
                    raise ProtocolError(409, "simulator-Horizon-does-not-overwrite")
                # 显式模拟两次不覆盖重命名；成功后删除旧目标的临时回滚文件。
                self.files[rollback] = self.files.pop(dst)
                self.files[dst] = self.files.pop(src)
                del self.files[rollback]
            else:
                self.files[dst] = self.files.pop(src)
            return ["result=renamed via=simulator"]
        raise ProtocolError(404, "simulator-unknown-method")

    def close(self):
        self._stopping.set()
        if self._listener is not None:
            self._listener.close()
        with self._lock:
            connections = list(self._connections)
        for connection in connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        if self._thread is not None:
            self._thread.join(timeout=3)
        for worker in self._workers:
            worker.join(timeout=3)
        if any(worker.is_alive() for worker in self._workers) or (self._thread and self._thread.is_alive()):
            raise RuntimeError("模拟设备线程未退出")

    def __enter__(self):
        return self.start()

    def __exit__(self, *args):
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    stopping = threading.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: stopping.set())
    with SimulatedDevice(args.port) as simulator:
        print(json.dumps({"platform": "nxc-simulator", "via": "simulator",
                          "ip": "127.0.0.1", "port": simulator.port}), flush=True)
        stopping.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
