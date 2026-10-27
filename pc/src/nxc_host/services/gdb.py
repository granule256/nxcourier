"""PC 侧 GDB/MI2 会话：真正超时、异步目标状态与受控进程生命周期。

CLI、Web 和 MCP 应通过同一个 HostRuntime 持有本对象。这里从不自动附加
进程、不安装 GDB、不拉取镜像，也不向远程目标发送 kill。
"""
from __future__ import annotations

import ast
from collections import deque
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import threading
import time
import uuid

DEFAULT_IP = os.environ.get('NXC_DEVICE_IP', '192.168.1.50')
DEFAULT_GDB_PORT = int(os.environ.get('NXC_GDB_PORT', '22225'))
DOCKER_IMAGE = os.environ.get('NXC_GDB_IMAGE', 'devkitpro/devkita64:latest')
_RECORD = re.compile(r'^(\d*)\^([a-z-]+)(.*)$')
_MESSAGE = re.compile(r'(?:^|,)msg=("(?:\\.|[^"\\])*")')


class GdbError(RuntimeError):
    """可直接展示给用户的 GDB 错误。"""


def _decode(value: str) -> str:
    try:
        return ast.literal_eval(value)
    except (ValueError, SyntaxError):
        return value


def _bundled_candidates() -> list[Path]:
    root = Path(__file__).resolve().parents[1]
    return [root / 'runtime/gdb/bin/aarch64-none-elf-gdb',
            root / 'runtime/gdb/aarch64-none-elf-gdb', root / 'runtime/gdb/gdb',
            root / 'runtime/gdb']


def _timeout(value) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise GdbError('timeout 必须是大于 0 的秒数') from error
    if not math.isfinite(result) or result <= 0:
        raise GdbError('timeout 必须是大于 0 的有限秒数')
    return result


class GdbSession:
    def __init__(self, ip: str = DEFAULT_IP, port: int = DEFAULT_GDB_PORT,
                 backend: str = 'auto', executable: str | None = None,
                 image: str | None = None):
        if backend not in ('auto', 'native', 'docker'):
            raise GdbError('gdb backend 必须是 auto、native 或 docker')
        if not isinstance(ip, str) or not ip or any(c in ip for c in '\r\n\0'):
            raise GdbError('GDB 目标地址无效')
        self.ip, self.port = ip, int(port)
        if not 1 <= self.port <= 65535:
            raise GdbError('GDB 端口必须在 1 到 65535 之间')
        self._target = f'{self.ip}:{self.port}'
        self.backend = backend
        self.executable = executable
        self.image = image or DOCKER_IMAGE
        self.proc: subprocess.Popen | None = None
        self.connected = False
        self._uncertain = False
        self.attached_pid: int | None = None
        self._state = 'closed'
        self._selected_backend: str | None = None
        self._selected_executable: str | None = None
        self._docker: str | None = None
        self._container: str | None = None
        self._reader: threading.Thread | None = None
        self._writer: threading.Thread | None = None
        self._outgoing: queue.Queue = queue.Queue(maxsize=64)
        self._lock = threading.RLock()
        self._commands = threading.Lock()
        self._lifecycle = threading.RLock()
        self._pending: dict[int, dict] = {}
        self._token = 0
        self._events: deque[str] = deque(maxlen=100)
        self._closing = False

    def doctor(self) -> dict:
        """只读探测本机依赖；不启动 GDB、设备连接或容器。"""
        diagnostics = []
        if self.backend != 'docker':
            explicit = self.executable or os.environ.get('NXC_GDB_PATH')
            candidates = [explicit] if explicit else list(_bundled_candidates()) + [
                'aarch64-none-elf-gdb', 'gdb-multiarch', 'gdb']
            for candidate in candidates:
                path = shutil.which(str(candidate))
                if path:
                    return {'available': True, 'backend': 'native', 'executable': path,
                            'image': self.image, 'diagnostics': diagnostics}
            if explicit:
                diagnostics.append(f'指定的 GDB 不存在或不可执行：{explicit}')
                return {'available': False, 'backend': self.backend,
                        'executable': str(explicit), 'diagnostics': diagnostics}
            diagnostics.append('未发现打包或本机 aarch64-none-elf-gdb/gdb-multiarch/gdb')
        if self.backend != 'native':
            docker = shutil.which('docker')
            if not docker:
                diagnostics.append('找不到 docker 命令')
            else:
                try:
                    probe = subprocess.run([docker, 'image', 'inspect', self.image],
                                           capture_output=True, text=True, timeout=3)
                    if probe.returncode == 0:
                        return {'available': True, 'backend': 'docker', 'executable': docker,
                                'image': self.image, 'diagnostics': diagnostics}
                    diagnostics.append(f'Docker 不可用或本机没有镜像 {self.image}：'
                                       + probe.stderr.strip()[:400])
                except (OSError, subprocess.TimeoutExpired) as error:
                    diagnostics.append(f'Docker 只读探测失败：{error}')
        diagnostics.append('请配置 NXC_GDB_PATH/--gdb-path 或准备现有 Docker 镜像；不会自动安装或拉取')
        return {'available': False, 'backend': self.backend, 'executable': None,
                'image': self.image, 'diagnostics': diagnostics}

    def status(self) -> dict:
        with self._lock:
            alive = self.proc is not None and self.proc.poll() is None
            return {'running': alive, 'connected': bool(alive and self.connected),
                    'uncertain': self._uncertain,
                    'attached_pid': self.attached_pid if alive else None,
                    'state': self._state if alive else 'closed',
                    'backend': self._selected_backend or self.backend,
                    'target': self._target, 'image': self.image,
                    'executable': self._selected_executable or self.executable,
                    'events': list(self._events)}

    def _read(self, process: subprocess.Popen):
        try:
            for raw in process.stdout:
                line = raw.rstrip('\r\n')
                if not line or line.strip() == '(gdb)':
                    continue
                match = _RECORD.match(line)
                with self._lock:
                    self._events.append(line[:4096])
                    if line.startswith('*running'):
                        self._state = 'running'
                    elif line.startswith('*stopped'):
                        self._state = 'stopped'
                        if 'reason="exited' in line:
                            self.attached_pid = None
                    elif line.startswith('=thread-group-exited'):
                        self.attached_pid = None
                    elif line.startswith('=connection-removed'):
                        self.connected = False
                        self.attached_pid = None
                        self._state = 'idle'
                    if match:
                        token, result, payload = match.groups()
                        pending = self._pending.get(int(token)) if token else None
                        if result == 'connected':
                            # 迟到响应仍可提供真实状态，但绝不交给别的 token。
                            self.connected = True
                        if result == 'running':
                            self._state = 'running'
                        if pending:
                            pending['queue'].put((result, payload, line))
                    elif line[:1] in ('~', '@', '&'):
                        message = _decode(line[1:])[:4096]
                        for pending in self._pending.values():
                            pending['output'].append(message)
        except (OSError, ValueError) as error:
            with self._lock:
                self._events.append(f'GDB reader: {error}')
        finally:
            with self._lock:
                if self.connected:
                    self._uncertain = True  # EOF 不是远程 detach 成功的证明。
                self.connected = False
                self.attached_pid = None
                self._state = 'closed'
                for pending in self._pending.values():
                    pending['queue'].put(None)

    def _write_requests(self, process: subprocess.Popen):
        # stdin 管道本身也可能阻塞。调用方只入队，不在持有请求锁时 write/flush。
        while True:
            item = self._outgoing.get()
            if item is None:
                return
            token, command = item
            with self._lock:
                pending = self._pending.get(token)
                if pending is not None:
                    pending['sent'] = True
            if pending is None:
                continue  # 排队期间已经超时的命令绝不补发。
            try:
                process.stdin.write(f'{token}{command}\n')
                process.stdin.flush()
            except (OSError, ValueError) as error:
                pending['queue'].put(GdbError(f'写入 GDB 失败：{error}；执行结果未知，未重放命令'))

    def _request(self, command: str, timeout: float, *, closing: bool = False) -> str:
        timeout = _timeout(timeout)
        with self._lock:
            process = self.proc
            if process is None or process.poll() is not None:
                raise GdbError('GDB 会话未启动或进程已退出，请先 open')
            if self._closing and not closing:
                raise GdbError('GDB 会话正在关闭')
            self._token += 1
            token = self._token
            pending = {'queue': queue.Queue(), 'output': deque(maxlen=256), 'sent': False}
            self._pending[token] = pending
        unknown = False
        try:
            try:
                with self._lock:
                    if self._closing and not closing:
                        raise GdbError('GDB 会话正在关闭，本条命令未发送')
                    self._outgoing.put_nowait((token, command))
            except queue.Full as error:
                raise GdbError('GDB 命令队列已满，本条命令未执行') from error
            try:
                result = pending['queue'].get(timeout=timeout)
            except queue.Empty as error:
                unknown = True
                raise GdbError(f'等待 GDB 响应超时（{timeout:g}s），执行结果未知；未重放命令') from error
            if isinstance(result, GdbError):
                unknown = True
                raise result
            if result is None:
                unknown = True
                raise GdbError('GDB 进程意外结束；执行结果未知，未重放命令')
            kind, payload, record = result
            if kind == 'error':
                message = _MESSAGE.search(payload)
                raise GdbError(str(_decode(message.group(1))) if message else payload)
            with self._lock:
                if command.startswith('-target-select ') or command.startswith('-target-attach '):
                    self.connected = True
                    if command.startswith('-target-select '):
                        self._target = str(_decode(command.split(' ', 2)[2]))
                    if command.startswith('-target-attach '):
                        self.attached_pid = int(command.rsplit(' ', 1)[1])
                    if self._state not in ('stopped', 'running'):
                        self._state = 'connected'
                elif command == '-target-detach':
                    # extended-remote 分离进程后仍保留连接，不能提前释放所有权。
                    self.attached_pid = None
                    self._state = 'connected' if self.connected else 'idle'
                    if not self.connected:
                        self._uncertain = False  # 成功分离且已收到连接移除事件。
                elif command == '-target-disconnect':
                    self._uncertain = False
                    self.connected = False
                    self.attached_pid = None
                    self._state = 'idle'
                return ''.join(pending['output']) + record
        except (OSError, ValueError) as error:
            unknown = True
            raise GdbError(f'写入 GDB 失败：{error}；执行结果未知，未重放命令') from error
        finally:
            with self._lock:
                if unknown and pending['sent'] and self.proc is process:
                    self._uncertain = True
                    if self._state != 'closed':
                        self._state = 'unknown'
                self._pending.pop(token, None)

    def start(self, connect: bool = True, timeout: float = 60.0) -> str:
        timeout = _timeout(timeout)
        with self._lifecycle:
            if self.status()['running']:
                raise GdbError('GDB 会话已经启动，请先 close')
            if self.proc is not None:
                self.close()
            diagnostic = self.doctor()
            if not diagnostic['available']:
                raise GdbError('GDB 后端不可用：' + '；'.join(diagnostic['diagnostics']))
            self._selected_backend = diagnostic['backend']
            self._selected_executable = diagnostic['executable']
            self._closing = False
            flags = ['--quiet', '--nx', '--interpreter=mi2']
            if self._selected_backend == 'native':
                command = [self._selected_executable, *flags]
            else:
                self._docker = self._selected_executable
                self._container = 'nxc-gdb-' + uuid.uuid4().hex
                command = [self._docker, 'run', '--pull=never', '--rm', '--name', self._container,
                           '-i', self.image, 'bash', '-lc',
                           '. /opt/devkitpro/devkita64.sh && exec aarch64-none-elf-gdb '
                           + ' '.join(flags)]
            try:
                self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                             errors='replace', bufsize=1)
                self._state = 'idle'
                self._outgoing = queue.Queue(maxsize=64)
                self._writer = threading.Thread(target=self._write_requests, args=(self.proc,),
                                                name='nxc-gdb-writer', daemon=True)
                self._writer.start()
                self._reader = threading.Thread(target=self._read, args=(self.proc,),
                                                name='nxc-gdb-reader', daemon=True)
                self._reader.start()
                output = []
                for setting in ('confirm off', 'pagination off', 'mi-async on'):
                    output.append(self._request('-gdb-set ' + setting, timeout))
                if connect:
                    output.append(self.connect(timeout))
                return '\n'.join(output)
            except (GdbError, OSError) as error:
                self.close()
                if isinstance(error, GdbError):
                    raise
                raise GdbError(f'启动 GDB 失败：{error}') from error

    def connect(self, timeout: float = 60.0) -> str:
        with self._commands:
            # target-select 的连接参数是原始文本；GDB 不会剥去 JSON 双引号。
            return self._request(f'-target-select extended-remote {self.ip}:{self.port}', timeout)

    def run(self, commands: list[str] | str, timeout: float = 60.0) -> str:
        timeout = _timeout(timeout)
        if isinstance(commands, str):
            commands = [line for line in commands.splitlines() if line.strip()]
        if not isinstance(commands, list) or not commands or any(
                not isinstance(line, str) or not line.strip() or any(c in line for c in '\r\n\0')
                for line in commands):
            raise GdbError('commands 必须是非空字符串或单行命令字符串数组')
        output = []
        with self._commands:
            deadline = time.monotonic() + timeout
            for line in commands:
                command = line.strip()
                if command in ('detach', 'disconnect'):
                    output.append(self._detach_and_disconnect(deadline - time.monotonic()))
                    continue
                elif command in ('interrupt', 'pause'):
                    command = '-exec-interrupt --all'
                elif re.fullmatch(r'attach +[1-9][0-9]*', command):
                    command = '-target-attach ' + command.split()[1]
                elif command.startswith(('target remote ', 'target extended-remote ')):
                    _, protocol, target = command.split(None, 2)
                    command = '-target-select ' + protocol + ' ' + target
                elif not command.startswith('-'):
                    command = '-interpreter-exec console ' + json.dumps(command, ensure_ascii=False)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise GdbError('GDB 批量命令超时，剩余命令未执行')
                output.append(self._request(command, remaining))
        return '\n'.join(output)

    def detach_only(self) -> str:
        with self._commands:
            return self._detach_and_disconnect(10)

    def _detach_and_disconnect(self, timeout: float) -> str:
        deadline = time.monotonic() + _timeout(timeout)
        output = self._request('-target-detach', timeout)
        with self._lock:
            # 普通 remote 会自行发出 connection-removed，不再重复断开。
            if not self.connected and not self._uncertain:
                return output
        try:
            output += '\n' + self._request('-target-disconnect', max(.001, deadline - time.monotonic()))
        except GdbError:
            with self._lock:
                self._uncertain = True
                self._state = 'unknown'
            raise
        return output

    def close(self) -> str:
        with self._lifecycle:
            process = self.proc
            if process is None:
                with self._lock:
                    self._uncertain = False
                return 'GDB 会话未启动'
            with self._lock:
                self._closing = True
            messages = []
            if process.poll() is None:
                if self.connected or self._uncertain:
                    try:
                        self._request('-exec-interrupt --all', .5, closing=True)
                    except GdbError:
                        pass
                    try:
                        self._request('-target-detach', 1, closing=True)
                        messages.append('已 detach')
                    except GdbError as error:
                        messages.append(f'detach 未确认：{error}')
                try:
                    self._request('-gdb-exit', 1, closing=True)
                except GdbError:
                    pass
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()  # 仅本服务持有的本地 GDB / Docker 客户端。
                process.wait(timeout=3)
                messages.append('已结束本服务持有的 GDB 进程')
            if self._docker and self._container:
                try:
                    cleanup = subprocess.run([self._docker, 'rm', '-f', self._container],
                                             capture_output=True, text=True, timeout=4)
                    if cleanup.returncode and 'No such container' not in cleanup.stderr:
                        messages.append('容器清理未确认：' + cleanup.stderr.strip()[:300])
                except (OSError, subprocess.TimeoutExpired) as error:
                    messages.append(f'容器清理未确认：{error}')
            with self._lock:
                # 关闭不能依赖 writer 仍存活；无消费者的满队列也必须有界清理。
                for pending in self._pending.values():
                    pending['queue'].put_nowait(GdbError('GDB 会话已关闭；未确认的命令结果未知，未重放'))
                while True:
                    try:
                        self._outgoing.get_nowait()
                    except queue.Empty:
                        break
                self._outgoing.put_nowait(None)
            if self._writer:
                self._writer.join(timeout=1)
            if self._reader:
                self._reader.join(timeout=1)
            for stream in (process.stdin, process.stdout):
                if stream:
                    try:
                        stream.close()
                    except (OSError, ValueError) as error:
                        # TextIOWrapper.close 仍会 flush；EOF 后 BrokenPipe 不能中止其余回收。
                        messages.append(f'管道关闭失败：{error}')
            with self._lock:
                self.proc = None
                self._uncertain = False
                self.connected = False
                self.attached_pid = None
                self._state = 'closed'
                self._container = None
                self._docker = None
            return 'GDB 会话已关闭' + ('（' + '；'.join(messages) + '）' if messages else '')

    def handle(self, action: str, args: dict | None = None) -> dict:
        args = args or {}
        if action == 'status':
            return self.status()
        if action == 'doctor':
            return self.doctor()
        timeout = _timeout(args.get('timeout', 60))
        if action == 'open':
            output = self.start(connect=bool(args.get('connect', True)), timeout=timeout)
        elif action == 'connect':
            output = self.connect(timeout)
        elif action == 'run':
            output = self.run(args.get('commands'), timeout)
        elif action in ('interrupt', 'pause'):
            output = self._request('-exec-interrupt --all', timeout)
        elif action == 'attach':
            pid = args.get('pid')
            if isinstance(pid, bool) or not re.fullmatch(r'[1-9][0-9]*', str(pid)):
                raise GdbError('attach 需要正整数 pid')
            with self._commands:
                output = self._request('-target-attach ' + str(pid), timeout)
        elif action == 'detach':
            output = self.detach_only()
        elif action == 'close':
            output = self.close()
        else:
            raise GdbError(f'未知 GDB action：{action}')
        return {'action': action, 'output': output, **self.status()}


_session: GdbSession | None = None
_session_lock = threading.Lock()


def get_session() -> GdbSession:
    global _session
    with _session_lock:
        if _session is None:
            _session = GdbSession()
        return _session


def handle(action: str, args: dict) -> dict:
    return get_session().handle(action, args)


def doctor() -> dict:
    return get_session().doctor()
