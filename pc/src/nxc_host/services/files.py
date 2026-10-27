"""后台共用的文件通道选择；只在传输前探测，开始后失败绝不切换通道。"""
from __future__ import annotations

from pathlib import Path

from .. import tools
from ..device import parse_payload


class FileTransfers:
    def __init__(self, device, ftp):
        self.device = device
        self.ftp = ftp

    def select(self, transport='auto'):
        if transport not in ('auto', 'fs', 'ftp'):
            raise ValueError('transport 必须是 auto、fs 或 ftp')
        if transport == 'fs':
            return 'fs'
        status = self.ftp.probe()
        if status.get('available'):
            return 'ftp'
        if transport == 'ftp':
            raise RuntimeError(status.get('message') or 'FTP 不可用，请确认 Switch 的 FTPD 已运行')
        return 'fs'

    def put(self, args, *, deploy=False):
        channel = self.select(args.get('transport', 'auto'))
        if channel == 'ftp':
            remote = args.get('remote_path') or (tools.DEFAULT_REMOTE_MODULE if deploy else None)
            if not args.get('local_path') or not remote:
                raise ValueError('缺少本地文件或设备路径')
            if args.get('verify', True) is not True:
                raise ValueError('发布前必须校验，不能禁用 verify')
            result = self.ftp.upload(args['local_path'], remote)
        else:
            result = (tools.tool_deploy if deploy else tools.tool_put)(self.device, args)
        return {**result, 'transport': channel}

    def fetch(self, args):
        channel = self.select(args.get('transport', 'auto'))
        if channel == 'ftp':
            remote = args.get('remote_path')
            if not remote:
                raise ValueError('缺少设备路径')
            local = args.get('local_path') or str(Path(remote).name)
            result = self.ftp.download(remote, local)
        else:
            result = tools.tool_fetch(self.device, args)
        return {**result, 'transport': channel}

    def stat(self, args):
        channel = self.select(args.get('transport', 'auto'))
        path = args.get('path') or args.get('remote_path')
        info = self.ftp.stat(path) if channel == 'ftp' else tools._remote_stat(self.device, path)
        return {**info, 'transport': channel}

    def list(self, args):
        channel = self.select(args.get('transport', 'auto'))
        path = args.get('path', '/')
        offset, count = int(args.get('offset', 0)), int(args.get('count', 50))
        if offset < 0 or not 1 <= count <= 200:
            raise ValueError('目录分页要求 offset>=0 且 count=1..200')
        if channel == 'ftp':
            return {**self.ftp.list(path, offset=offset, count=count), 'transport': channel}
        result = tools._checked(self.device.call_one(tools._device_line('fs.ls', {
            'path': path, 'offset': offset, 'count': count}), retry_safe=True), '列出设备目录')
        rows = parse_payload(result.get('lines', []))
        entries = [row for row in rows if isinstance(row, dict) and 'name' in row]
        meta = next((row for row in rows if isinstance(row, dict) and 'next_offset' in row), None)
        if meta is not None:
            return {'entries': entries, 'offset': offset, 'next_offset': int(meta['next_offset']),
                    'truncated': str(meta.get('truncated', '0')) == '1', 'transport': channel, 'legacy': False}
        # 旧设备忽略 offset/count，返回至多200项；本地分页并显式保留截断事实。
        truncated = any(isinstance(row, dict) and str(row.get('truncated', '0')) == '1' for row in rows)
        return {'entries': entries[offset:offset+count], 'offset': offset,
                'next_offset': offset+count if offset+count < len(entries) else -1,
                'truncated': truncated, 'legacy': True, 'transport': channel}

    def sha(self, args):
        channel = self.select(args.get('transport', 'auto'))
        remote = args.get('remote_path')
        if not isinstance(remote, str) or not remote.startswith('/'):
            raise ValueError('remote_path 必须为设备绝对路径')
        if channel == 'ftp':
            return {**self.ftp.sha(remote), 'transport': channel}
        return {'remote': remote, **tools.remote_file_hash(self.device, remote), 'transport': channel}

    def read(self, args, max_bytes):
        channel = self.select(args.get('transport', 'auto'))
        remote = args.get('remote_path')
        info = self.ftp.stat(remote) if channel == 'ftp' else tools._remote_stat(self.device, remote)
        if info.get('type') != 'file':
            raise ValueError('请选择普通文件')
        size = int(info['size'])
        if size > max_bytes:
            raise ValueError('Web下载最多8MiB，请使用nxc fetch传较大文件')
        if channel == 'ftp':
            data = self.ftp.read(remote, max_bytes=max_bytes)
        else:
            body = bytearray()
            while len(body) <= max_bytes:
                piece = tools._read_once(self.device, remote, len(body), min(24000, max_bytes-len(body)+1))
                if not piece:
                    break
                body.extend(piece)
            data = bytes(body)
        if len(data) != size or len(data) > max_bytes:
            raise RuntimeError('读取大小变化或文件不完整，请重新读取')
        return data, channel
