"""标准库被动 FTP 客户端；流式校验与可恢复的两步重命名提交。"""
from __future__ import annotations

import ftplib
import hashlib
import math
import os
from pathlib import Path
import posixpath
import re
import tempfile
import threading
import uuid

_BLOCK = 64 * 1024
_UNSUPPORTED = {'500', '501', '502', '504'}


class FtpClient:
    def __init__(self, ip, port=5000, timeout=15, user='anonymous', password=''):
        if not isinstance(ip, str) or not ip or any(c in ip for c in '\r\n\0'):
            raise RuntimeError('FTP 目标地址无效')
        try:
            port, timeout = int(port), float(timeout)
        except (ValueError, TypeError) as error:
            raise RuntimeError('FTP 端口或超时无效') from error
        if not 1 <= port <= 65535 or not math.isfinite(timeout) or timeout <= 0:
            raise RuntimeError('FTP 端口须为 1–65535，超时须为正数')
        if any(not isinstance(value, str) or any(c in value for c in '\r\n\0')
               for value in (user, password)):
            raise RuntimeError('FTP 登录信息无效')
        self.ip, self.port, self.timeout = ip, port, timeout
        self.user, self.password = user, password
        self._ftp = None
        self._mlsd = None
        self._lock = threading.RLock()

    @staticmethod
    def _path(path):
        try:
            path = os.fspath(path)
        except TypeError as error:
            raise RuntimeError('FTP 目标路径无效') from error
        if (not isinstance(path, str) or not path.startswith('/') or any(c in path for c in '\r\n\0')
                or '..' in path.split('/') or '\\' in path or path.startswith('-')):
            raise RuntimeError('FTP 目标路径无效，不能包含控制字符或上级目录')
        return path

    @staticmethod
    def _missing(error):
        # 550 也可能是权限问题；只识别服务器明确说明不存在的回复。
        return (str(error).startswith('550') and bool(re.search(
            r'no such file|file not found|path not found|does not exist|not exist',
            str(error), re.IGNORECASE)))

    def _connect(self):
        if self._ftp is None:
            ftp = ftplib.FTP(timeout=self.timeout, encoding='utf-8')
            self._ftp = ftp
            ftp.connect(self.ip, self.port)
            reply = ftp.sendcmd('USER ' + self.user)
            if reply.startswith('3'):
                # ftplib.login 会将匿名空密码改写为 anonymous@；这里保持配置原值。
                reply = ftp.sendcmd('PASS ' + self.password)
            if not reply.startswith('2'):
                raise RuntimeError('FTP 登录未成功')
            ftp.set_pasv(True)
            ftp.voidcmd('TYPE I')
            self._mlsd = None
        return self._ftp

    def _message(self, error):
        detail = str(error) or type(error).__name__
        if self.password:
            detail = detail.replace(self.password, '***')
        return detail

    def _run(self, action, callback):
        with self._lock:
            try:
                return callback(self._connect())
            except Exception as error:
                self.close()
                raise RuntimeError(f'FTP {action}失败：{self._message(error)}') from None

    def close(self):
        with self._lock:
            if self._ftp is not None:
                self._ftp.close()
                self._ftp = None
            self._mlsd = None

    def probe(self):
        with self._lock:
            # FTPD可能在空闲后关闭控制连接；只读预检允许重新建连一次。
            # 文件传输本身仍只有一次尝试，任何写入失败都不会走这里重放。
            attempts = 2 if self._ftp is not None else 1
            for attempt in range(attempts):
                try:
                    self._run('探测', lambda ftp: ftp.pwd())
                    return {'available': True, 'transport': 'ftp', 'ip': self.ip, 'port': self.port}
                except RuntimeError as error:
                    if attempt + 1 == attempts:
                        return {'available': False, 'transport': 'ftp', 'message': str(error),
                                'ip': self.ip, 'port': self.port}

    def _stat(self, ftp, path, missing_ok=False):
        ftp.voidcmd('TYPE I')
        try:
            size = ftp.size(path)
            if size is not None:
                return {'path': path, 'type': 'file', 'size': size}
        except ftplib.error_perm as error:
            if self._missing(error):
                if missing_ok:
                    return None
                raise RuntimeError(f'路径不存在：{path}') from error
            size_error = error
        else:
            size_error = RuntimeError('服务器未提供文件大小')
        previous = ftp.pwd()
        try:
            ftp.cwd(path)
        except ftplib.error_perm:
            raise size_error
        ftp.cwd(previous)
        return {'path': path, 'type': 'dir', 'size': 0}

    def stat(self, path, missing_ok=False):
        path = self._path(path)
        return self._run('读取属性', lambda ftp: self._stat(ftp, path, missing_ok))

    def _entries(self, ftp, path):
        if self._mlsd is not False:
            try:
                entries = []
                for name, facts in ftp.mlsd(path):
                    kind = facts.get('type', 'unknown')
                    if kind in {'cdir', 'pdir'} or name in {'.', '..'}:
                        continue
                    size = facts.get('size')
                    entries.append({'name': name, 'type': kind if kind in {'dir', 'file'} else 'unknown',
                                    'size': int(size) if size is not None else (0 if kind == 'dir' else None)})
                self._mlsd = True
                return entries
            except ftplib.error_perm as error:
                if str(error)[:3] not in _UNSUPPORTED:
                    raise
                self._mlsd = False
        entries = []
        for entry in ftp.nlst(path):
            name = posixpath.basename(entry.rstrip('/'))
            if name in {'', '.', '..'}:
                continue
            target = posixpath.join(path, name)
            try:
                info = self._stat(ftp, target)
            except ftplib.error_perm:
                info = {'type': 'unknown', 'size': None}
            entries.append({'name': name, 'type': info['type'], 'size': info['size']})
        return entries

    def list(self, path, offset=0, count=50):
        path = self._path(path)
        if (not isinstance(offset, int) or isinstance(offset, bool) or offset < 0
                or not isinstance(count, int) or isinstance(count, bool) or count <= 0):
            raise RuntimeError('FTP 分页 offset 须为非负整数，count 须为正整数')

        def operation(ftp):
            entries = sorted(self._entries(ftp, path), key=lambda item: item['name'])
            page = entries[offset:offset + count]
            truncated = offset + len(page) < len(entries)
            return {'entries': page, 'offset': offset,
                    'next_offset': offset + len(page) if truncated else -1,
                    'truncated': truncated}
        return self._run('列目录', operation)

    @staticmethod
    def _sha(ftp, path):
        digest = hashlib.sha256()
        length = 0

        def collect(data):
            nonlocal length
            length += len(data)
            digest.update(data)
        ftp.retrbinary('RETR ' + path, collect, blocksize=_BLOCK)
        return {'bytes': length, 'sha256': digest.hexdigest()}

    def sha(self, remote_path):
        path = self._path(remote_path)
        return self._run('计算摘要', lambda ftp: self._sha(ftp, path))

    def read(self, remote_path, max_bytes):
        path = self._path(remote_path)
        if not isinstance(max_bytes, int) or isinstance(max_bytes, bool) or max_bytes < 0:
            raise RuntimeError('FTP 读取上限须为非负整数')

        def operation(ftp):
            result = bytearray()

            def collect(data):
                if len(result) + len(data) > max_bytes:
                    raise RuntimeError(f'文件超过读取上限 {max_bytes} 字节')
                result.extend(data)
            ftp.retrbinary('RETR ' + path, collect, blocksize=_BLOCK)
            return bytes(result)
        return self._run('读取文件', operation)

    def download(self, remote_path, local_path):
        remote = self._path(remote_path)

        def operation(ftp):
            local = Path(local_path)
            local.parent.mkdir(parents=True, exist_ok=True)
            ftp.voidcmd('TYPE I')
            expected_size = ftp.size(remote)
            if expected_size is None or expected_size < 0:
                raise RuntimeError('服务器未提供有效 SIZE，无法校验下载长度')
            temporary = None
            try:
                digest = hashlib.sha256()
                length = 0
                with tempfile.NamedTemporaryFile(dir=local.parent, prefix='.nxc-download-', delete=False) as output:
                    temporary = Path(output.name)

                    def collect(data):
                        nonlocal length
                        output.write(data)
                        digest.update(data)
                        length += len(data)
                    ftp.retrbinary('RETR ' + remote, collect, blocksize=_BLOCK)
                    if length != expected_size:
                        raise RuntimeError(f'下载长度不一致，可能发生截断：预期 {expected_size} 字节，收到 {length} 字节')
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, local)
                temporary = None
                return {'remote': remote, 'local': str(local), 'bytes': length,
                        'sha256': digest.hexdigest(), 'transport': 'ftp'}
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
        return self._run('下载', operation)

    def upload(self, local_path, remote_path):
        remote = self._path(remote_path)
        if remote.rstrip('/') in {'', '.', '/'} or remote.endswith('/'):
            raise RuntimeError('FTP 上传目标必须为文件路径')
        parent = posixpath.dirname(remote)
        token = uuid.uuid4().hex
        temporary = posixpath.join(parent, '.nxc-upload-' + token)
        backup = posixpath.join(parent, '.nxc-backup-' + token)

        def operation(ftp):
            backup_started = False
            old = None
            try:
                old_stat = self._stat(ftp, remote, missing_ok=True)
                if old_stat is not None and old_stat['type'] != 'file':
                    raise RuntimeError('上传目标不是普通文件')
                if old_stat is not None:
                    old = self._sha(ftp, remote)
                digest = hashlib.sha256()
                length = 0
                with Path(local_path).open('rb') as source:
                    def collect(data):
                        nonlocal length
                        digest.update(data)
                        length += len(data)
                    ftp.storbinary('STOR ' + temporary, source, blocksize=_BLOCK, callback=collect)
                expected = {'bytes': length, 'sha256': digest.hexdigest()}
                if self._sha(ftp, temporary) != expected:
                    raise RuntimeError('暂存文件回读校验失败，未提交目标')
                if old is not None:
                    backup_started = True
                    ftp.rename(remote, backup)
                    if self._sha(ftp, backup) != old:
                        raise RuntimeError('原目标备份校验失败')
                ftp.rename(temporary, remote)
                return {'remote': remote, **expected, 'verified': True,
                        'backup': backup if old is not None else None,
                        'rollback': backup if old is not None else None,
                        'transport': 'ftp'}
            except Exception as error:
                recovery = '原目标尚未移动；请检查目标状态'
                if backup_started:
                    try:
                        ftp.rename(backup, remote)
                        if self._sha(ftp, remote) != old:
                            raise RuntimeError('恢复后摘要不匹配')
                        recovery = '已恢复原目标'
                    except Exception as restore_error:
                        recovery = '恢复失败，目标状态无法确认：' + self._message(restore_error)
                raise RuntimeError(
                    f'{self._message(error)}；{recovery}；目标路径：{remote}；'
                    f'暂存路径：{temporary}；备份路径：{backup if old is not None else "无（原目标不存在）"}'
                ) from None
        return self._run('上传', operation)
