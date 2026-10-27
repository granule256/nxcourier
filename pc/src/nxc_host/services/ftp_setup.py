"""后台 FTP 检测与固定版本安装；写入结果未知时绝不自动重放。"""
from __future__ import annotations

import base64
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import threading
import uuid

from .. import tools
from ..device import parse_payload
from .ftp_package import (CATALOG, CONFIG_PATH, FLAG_PATH, MAX_ARCHIVE_BYTES,
                          MODULE_PATH, TOOLBOX_PATH, get_official_package, load_package)

MODULE, FLAG, TOOLBOX, CONFIG = ('/' + p for p in (MODULE_PATH, FLAG_PATH, TOOLBOX_PATH, CONFIG_PATH))
SELF_FLAG = '/atmosphere/contents/4200000000000012/flags/boot2.flag'
MAX_READ = 4 * 1024 * 1024
CHUNK = 381
_REQUIRED = {'fs.stat', 'fs.read', 'fs.write', 'fs.copy', 'fs.mkdir'}
_MINIMAL_CONFIG = b'[User]\nuser:=\n[Password]\npassword:=\n[Port]\nport:=5000\n[Anonymous]\nanonymous:=0\n'
# 仅匹配一行中的赋值，空用户名/密码不会吞掉下一行。
_SETTING = re.compile(rb'^([ \t]*)([A-Za-z_][A-Za-z_0-9]*)([ \t]*:=[ \t]*)([^\r\n]*)(\r\n|\n|\r|)$')


class FtpSetup:
    def __init__(self, device, ftp, config, cache_dir):
        self.device, self.ftp, self.config = device, ftp, config
        self.cache_dir = Path(cache_dir)
        self._packages = {}
        self._binding = self._target()
        self._lock = threading.RLock()
        self._progress_lock = threading.Lock()
        self._progress = {'phase': 'idle', 'message': '尚未开始', 'completed': 0, 'total': 0}
        self._secrets = set()

    def _target(self):
        return (self.config.device_ip, self.config.device_port, self.config.ftp_port,
                self.config.ftp_user, self.config.ftp_password)

    def _message(self, value):
        text = str(value)
        secrets = self._secrets | {self.config.ftp_user, self.config.ftp_password}
        for secret in sorted((item for item in secrets if item and item != 'anonymous'), key=len, reverse=True):
            text = text.replace(secret, '***')
        return text

    def _advance(self, phase, message, *, completed=None, total=None, paths=None):
        with self._progress_lock:
            self._progress.update(phase=phase, message=self._message(message))
            if completed is not None:
                self._progress['completed'] = completed
            if total is not None:
                self._progress['total'] = total
            if paths is not None:
                self._progress['paths'] = list(paths)

    def progress(self):
        # 与设备操作锁独立，长安装期间也能立即读取，调用者不能改动内部状态。
        with self._progress_lock:
            return copy.deepcopy(self._progress)

    def _query(self, method, params=None, *, missing_ok=False):
        response = self.device.call_one(tools._device_line(method, params or {}), retry_safe=True)
        if missing_ok and response.get('error', {}).get('code') == 404:
            return None
        return tools._checked(response, method)

    def _stat(self, path):
        response = self._query('fs.stat', {'path': path}, missing_ok=True)
        if response is None:
            return None
        rows = [row for row in parse_payload(response.get('lines', [])) if isinstance(row, dict)]
        fields = {key: value for row in rows for key, value in row.items()}
        if fields.get('type') not in ('file', 'dir') or not re.fullmatch(r'[0-9]+', fields.get('size', '')):
            raise RuntimeError('fs.stat 响应缺少有效文件类型或大小：' + path)
        return {'type': fields['type'], 'size': int(fields['size'])}

    def _read(self, path, stat=None):
        stat = self._stat(path) if stat is None else stat
        if stat is None:
            return None
        if stat['type'] != 'file':
            raise RuntimeError('预期普通文件：' + path)
        size = stat['size']
        if size > MAX_READ:
            raise RuntimeError('读取文件超过 4MiB 上限：' + path)
        data = bytearray()
        while len(data) < size:
            want = min(24000, size - len(data))
            piece = tools._read_once(self.device, path, len(data), want)
            if not piece or len(piece) > want:
                raise RuntimeError('文件读取长度异常：' + path)
            data.extend(piece)
        after = self._stat(path)
        if after != stat:
            raise RuntimeError('读取期间文件大小或类型变化：' + path)
        return bytes(data)

    def _parse_config(self, data):
        values, matches = {}, {}
        for index, line in enumerate(data.splitlines(keepends=True)):
            stripped = line.strip()
            if not stripped or stripped.startswith((b'#', b';')) or (stripped.startswith(b'[') and stripped.endswith(b']')):
                continue
            match = _SETTING.fullmatch(line)
            if not match:
                raise RuntimeError('FTP 配置存在无法解析的赋值；未改动原文件')
            key = match[2].decode('ascii')
            if key in ('anonymous', 'port', 'user', 'password'):
                if key in values:
                    raise RuntimeError('FTP 配置存在重复的 ' + key + ' 项；未改动原文件')
                values[key] = match[4].strip(b' \t')
                matches[key] = (index, match)
        for key in ('user', 'password'):
            if values.get(key):
                self._secrets.add(values[key].decode('utf-8', errors='replace'))
        if values.get('anonymous') not in (b'0', b'1'):
            raise RuntimeError('FTP 配置缺少有效的 anonymous:=0 或 anonymous:=1 项')
        port = values.get('port', b'')
        if not re.fullmatch(rb'[0-9]+', port) or not 1 <= int(port) <= 65535:
            raise RuntimeError('FTP 配置缺少有效的 port:=端口 项')
        return {'anonymous': values['anonymous'] == b'1', 'port': int(port),
                'credentials_set': bool(values.get('user') and values.get('password'))}, matches

    def _updated_config(self, original, *, anonymous, port=None):
        _, matches = self._parse_config(original)
        lines = original.splitlines(keepends=True)
        changes = {'anonymous': b'1' if anonymous else b'0'}
        if port is not None:
            changes['port'] = str(port).encode('ascii')
        for key, value in changes.items():
            index, match = matches[key]
            # 保留同一行值两侧空格和原始换行，其他行完全逐字节保留。
            old = match[4]
            trailing = old[len(old.rstrip(b' \t')):]
            lines[index] = match[1] + match[2] + match[3] + value + trailing + match[5]
        updated = b''.join(lines)
        self._parse_config(updated)
        return updated

    def _base(self):
        return {'state': 'error', 'message': '',
                'module': {'installed': None, 'known_version': None, 'boot_enabled': None, 'configured': False},
                'configuration': {'exists': None, 'anonymous': None, 'port': None,
                                  'credentials_set': False, 'expected_port': self.config.ftp_port,
                                  'port_matches': None},
                'ftp': {'available': False, 'message': '尚未验证'},
                'catalog': copy.deepcopy(CATALOG), 'can_configure': False}

    def _inspect(self, result):
        module = self._read(MODULE)
        result['module']['installed'] = module is not None
        if module is not None:
            known = hashlib.sha256(module).hexdigest() == CATALOG['module_sha256']
            result['module']['known_version'] = CATALOG['version'] if known else None
        toolbox = self._read(TOOLBOX)
        valid_toolbox = True
        if toolbox is not None:
            try:
                info = json.loads(toolbox)
                valid_toolbox = (isinstance(info, dict) and info.get('name') in ('sys-ftpd', 'sys-ftpd-light')
                                 and info.get('tid', '420000000000000E').upper() == '420000000000000E')
            except (ValueError, TypeError, AttributeError):
                valid_toolbox = False
        flag = self._read(FLAG)
        if flag is not None and flag != b'':
            raise RuntimeError('FTP boot2.flag 不是空文件；拒绝修改')
        result['module']['boot_enabled'] = flag is not None
        raw = self._read(CONFIG)
        result['configuration']['exists'] = raw is not None
        if raw is not None:
            parsed, _ = self._parse_config(raw)
            result['configuration'].update(parsed, port_matches=parsed['port'] == self.config.ftp_port)
            result['module']['configured'] = parsed['anonymous'] or parsed['credentials_set']
        conflict = (module is not None and not result['module']['known_version']) or not valid_toolbox
        result['can_configure'] = bool(module is not None and result['module']['known_version'] and
                                       valid_toolbox)
        return {'module': module, 'toolbox': toolbox, 'flag': flag, 'config': raw, 'conflict': conflict}

    def _ftp_status(self):
        try:
            probe = self.ftp.probe()
            available = bool(probe.get('available'))
            if available:
                listing = self.ftp.list('/', offset=0, count=1)
                if not isinstance(listing, dict) or not isinstance(listing.get('entries'), list):
                    raise RuntimeError('FTP 目录验证响应无效')
            return {'available': available,
                    'message': self._message(probe.get('message') or ('登录与目录验证通过' if available else 'FTP 不可用'))}
        except Exception as error:
            return {'available': False, 'message': self._message(error)}

    def _status(self):
        result = self._base()
        try:
            self._capabilities({'fs.stat', 'fs.read'})
            scanned = self._inspect(result)
        except Exception as error:
            result.update(state='unreachable' if isinstance(error, OSError) or '执行结果未知' in str(error) else 'error',
                          message='NXC 只读检测失败：' + self._message(error))
            return result, None
        result['ftp'] = self._ftp_status()
        installed = result['module']['installed']
        cfg = result['configuration']
        auth_required = bool(re.search(r'\b(?:430|530)\b', result['ftp']['message']))
        if scanned['conflict']:
            result.update(state='conflict', message='同 ID 模块摘要或 toolbox 名称未知；不会覆盖已有文件')
        elif not installed and (result['ftp']['available'] or auth_required):
            result.update(state='conflict', message='FTP 端口已响应，但未识别后台模块，服务来源未确认；请退出前台 FTP 后重新检测')
        elif installed and not result['module']['boot_enabled']:
            result.update(state='pending_activation', message='后台模块缺少 boot2.flag，可配置匿名并启用后台后重启验证')
        elif installed and not cfg['exists']:
            result.update(state='auth_required', message='已识别后台模块但缺少 config.ini，可配置匿名并启用后台')
        elif cfg['port_matches'] is False:
            result.update(state='pending_activation' if installed else 'missing',
                          message=f"设备配置端口 {cfg['port']} 与 PC 端口 {self.config.ftp_port} 不一致，请核对目标端口")
        elif result['ftp']['available']:
            result.update(state='ready', message='FTP 登录与目录验证通过，无需重复安装')
        elif not installed:
            result.update(state='missing', message='未找到后台 FTP 模块，可准备固定版本安装包')
        elif auth_required:
            result.update(state='auth_required', message='FTP 需要登录配置；可启用匿名后重新验证')
        else:
            result.update(state='pending_activation', message='模块文件已存在，FTP 尚未可用；请检查暂停状态或重启后重新检测')
        return result, scanned

    def status(self):
        with self._lock:
            return self._status()[0]

    def prepare(self, args):
        with self._lock:
            source = args.get('source', 'official')
            self._advance('preparing', '正在验证安装包', completed=0, total=0, paths=[])
            try:
                if source == 'official':
                    package = get_official_package(self.cache_dir)
                elif source == 'local':
                    encoded = args.get('data')
                    if not isinstance(encoded, str) or not encoded or len(encoded) > ((MAX_ARCHIVE_BYTES + 2) // 3) * 4:
                        raise ValueError('本地安装包 base64 缺失或超过 4MiB 上限')
                    try:
                        data = base64.b64decode(encoded, validate=True)
                    except (ValueError, UnicodeError) as error:
                        raise ValueError('本地安装包 base64 无效') from error
                    package = load_package(data, source='local')
                else:
                    raise ValueError('安装包来源必须是 official 或 local')
                self._validate_package(package)
                self._packages[package['package_id']] = copy.deepcopy(package)
                result = {key: package[key] for key in ('package_id', 'version', 'source', 'sha256')}
                result['files'] = [{'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                                   for path, data in package['files'].items()]
                self._advance('prepared', '安装包已在 PC 验证，尚未写入设备', completed=0,
                              total=sum(item['bytes'] for item in result['files']))
                return result
            except Exception as error:
                self._advance('error', error)
                raise

    def _validate_package(self, package):
        expected = {'/' + path: digest for path, digest in CATALOG['files_sha256'].items()}
        if (package.get('version') != CATALOG['version'] or package.get('source') not in ('official', 'local')
                or not re.fullmatch(r'[a-f0-9]{64}', package.get('sha256', ''))
                or set(package.get('files', {})) != set(expected)
                or package.get('package_id') != 'sys-ftpd-' + CATALOG['version'] + '-' + package['sha256']):
            raise ValueError('已准备安装包不再匹配固定版本，请重新准备')
        for path, data in package['files'].items():
            if not isinstance(data, bytes) or hashlib.sha256(data).hexdigest() != expected[path]:
                raise ValueError('已准备安装包内容摘要不匹配，请重新准备')

    def _capabilities(self, required=None):
        rows = parse_payload(self._query('tools.list', {'tool': 'fs'}).get('lines', []))
        methods = {row['method']: row for row in rows if isinstance(row, dict) and 'method' in row}
        missing = (_REQUIRED if required is None else required) - set(methods)
        if missing:
            raise RuntimeError('设备缺少文件操作前提：' + ', '.join(sorted(missing)))
        return bool(re.search(r'(?:^|,)allow_empty\(', methods.get('fs.write', {}).get('params', '')))

    def _flag_capability(self):
        allow_empty = self._capabilities()
        if not allow_empty and self._read(SELF_FLAG) != b'':
            raise RuntimeError('设备无法创建空启动标记，且 NXC 自身空 boot2.flag 不存在或非空；写入前拒绝')
        return allow_empty

    def _write_flag(self, allow_empty, receipt):
        receipt['phase'] = 'boot_flag'
        if allow_empty:
            self._mutate('fs.write', {'path': FLAG, 'data': '', 'append': 0, 'allow_empty': 1}, receipt)
        else:
            self._verify(SELF_FLAG, b'')
            self._mutate('fs.copy', {'src': SELF_FLAG, 'dst': FLAG}, receipt)
        self._verify(FLAG, b'')

    def _new_path(self, path, suffix, token):
        return path + '.nxc-' + suffix + '-' + token

    def _preflight(self, targets, *, extra_paths=()):
        # 先检查所有目录及唯一暂存/备份路径。返回后才允许第一次 mkdir/write。
        directories = {str(parent) for path in list(targets) + list(extra_paths) for parent in PurePosixPath(path).parents if str(parent) != '/'}
        missing_dirs = []
        for path in sorted(directories, key=lambda item: (item.count('/'), item)):
            stat = self._stat(path)
            if stat is None:
                missing_dirs.append(path)
            elif stat['type'] != 'dir':
                raise RuntimeError('目标父路径不是目录：' + path)
        for path in targets:
            if self._stat(path) is not None:
                raise RuntimeError('唯一暂存或备份路径已经存在：' + path)
        return missing_dirs

    def _mutate(self, method, params, receipt):
        path = params.get('dst') or params.get('path')
        if path not in receipt['paths']:
            receipt['paths'].append(path)
        self._advance(receipt['phase'], '正在执行 ' + receipt['phase'], paths=receipt['paths'])
        # 路径在发出前记入回执：断线也可能已写入，不能当作未执行。
        response = self.device.call_one(tools._device_line(method, params), retry_safe=False)
        return tools._checked(response, method)

    def _verify(self, path, expected):
        if self._read(path) != expected:
            raise RuntimeError('文件完整回读校验失败：' + path)

    def _stage(self, path, content, receipt):
        for offset in range(0, max(1, len(content)), CHUNK):
            piece = content[offset:offset + CHUNK]
            result = self._mutate('fs.write', {'path': path, 'data': base64.b64encode(piece).decode('ascii'),
                                  'append': int(offset != 0), **({'allow_empty': 1} if not piece else {})}, receipt)
            rows = parse_payload(result.get('lines', []))
            counts = [row['bytes'] for row in rows if isinstance(row, dict) and 'bytes' in row]
            if counts != [str(len(piece))]:
                raise RuntimeError('fs.write 未确认完整写入字节数')
            with self._progress_lock:
                self._progress['completed'] += len(piece)
        self._verify(path, content)

    def _backup(self, original, backup, receipt):
        receipt['phase'] = 'config_backup'
        self._verify(CONFIG, original)
        self._mutate('fs.copy', {'src': CONFIG, 'dst': backup}, receipt)
        self._verify(backup, original)
        receipt['backup_verified'] = True

    def _commit_config(self, stage, updated, original, backup, receipt):
        receipt['phase'] = 'config_commit'
        if original is None:
            if self._stat(CONFIG) is not None:
                raise RuntimeError('配置目标已出现；拒绝覆盖')
        else:
            self._verify(CONFIG, original)
        receipt['config_touched'] = True
        self._mutate('fs.copy', {'src': stage, 'dst': CONFIG}, receipt)
        self._verify(CONFIG, updated)

    def _failure(self, error, receipt, original=None):
        phase = receipt['phase']
        receipt['verified'] = False
        receipt['recovery'] = 'not_needed'
        if receipt.get('config_touched'):
            receipt['recovery'] = 'unknown'
        if receipt.get('config_touched') and original is not None:
            if receipt.get('backup_verified'):
                try:
                    receipt['phase'] = 'config_restore'
                    self._verify(receipt['backup'], original)
                    self._mutate('fs.copy', {'src': receipt['backup'], 'dst': CONFIG}, receipt)
                    self._verify(CONFIG, original)
                    receipt['recovery'] = 'restored'
                except Exception:
                    # 不再尝试第二次恢复，也不自动重放原先失败的提交。
                    pass
        receipt['phase'] = phase
        result = self._base()
        result.update(state='error', message=f"FTP 操作失败，阶段 {phase}：{self._message(error)}；配置恢复 {receipt['recovery']}")
        self._advance('error', result['message'], paths=receipt['paths'])
        return self._with_receipt(result, receipt)

    @staticmethod
    def _with_receipt(result, receipt):
        return {**result, 'receipt': copy.deepcopy(receipt), 'backup': receipt.get('backup'),
                'paths': list(receipt['paths'])}

    def install(self, package_id, anonymous=True):
        with self._lock:
            if type(anonymous) is not bool:
                raise ValueError('anonymous 必须是布尔值')
            if self._target() != self._binding:
                self._packages.clear()
                raise ValueError('设备目标已变化，请重新创建安装向导并准备安装包')
            package = self._packages.get(package_id)
            if package is None:
                raise ValueError('安装包不属于当前设备向导，请重新准备')
            self._validate_package(package)
            self._advance('preflight', '正在检查所有安装前提', completed=0, total=0, paths=[])
            result, scanned = self._status()
            receipt = {'phase': 'preflight', 'paths': [], 'backup': None, 'verified': False}
            if scanned is None:
                return self._with_receipt(result, receipt)
            if result['module']['installed'] or result['state'] in ('ready', 'conflict'):
                receipt.update(phase='noop', verified=result['state'] == 'ready')
                self._advance('noop', result['message'], paths=[])
                return self._with_receipt(result, receipt)
            original = scanned['config']
            try:
                allow_empty = self._flag_capability()
                if not anonymous and not self._parse_config(original if original is not None else package['files'][CONFIG])[0]['credentials_set']:
                    raise RuntimeError('账号模式需要已配置完整 user/password 凭据；写入前拒绝')
                updated = self._updated_config(original if original is not None else package['files'][CONFIG],
                                               anonymous=anonymous, port=self.config.ftp_port)
                token = uuid.uuid4().hex
                stages = {path: self._new_path(path, 'stage', token) for path in (MODULE, TOOLBOX, CONFIG)}
                backup = self._new_path(CONFIG, 'backup', token) if original is not None and updated != original else None
                receipt['backup'] = backup
                targets = list(stages.values()) + ([backup] if backup else [])
                directories = self._preflight(targets, extra_paths=[FLAG])
                # 预检与提交前都必须确认模块仍不存在。
                if self._stat(MODULE) is not None:
                    raise RuntimeError('模块已出现，拒绝覆盖')
                if scanned['toolbox'] is not None and scanned['toolbox'] != package['files'][TOOLBOX]:
                    raise RuntimeError('已有 toolbox 内容不同，拒绝覆盖')
                total = len(package['files'][MODULE]) + len(package['files'][TOOLBOX]) + len(updated)
                self._advance('mkdir', '所有前提已检查，准备固定目录', total=total)
                receipt['phase'] = 'mkdir'
                for directory in directories:
                    self._mutate('fs.mkdir', {'path': directory}, receipt)
                for path, content in ((MODULE, package['files'][MODULE]), (TOOLBOX, package['files'][TOOLBOX]), (CONFIG, updated)):
                    receipt['phase'] = 'stage_' + PurePosixPath(path).name
                    self._stage(stages[path], content, receipt)
                if backup:
                    self._backup(original, backup, receipt)
                for path in (MODULE, TOOLBOX):
                    if path == TOOLBOX and scanned['toolbox'] is not None:
                        self._verify(path, package['files'][path])
                        continue
                    receipt['phase'] = 'commit_' + PurePosixPath(path).name
                    if self._stat(path) is not None:
                        raise RuntimeError('目标已存在，拒绝覆盖：' + path)
                    self._mutate('fs.copy', {'src': stages[path], 'dst': path}, receipt)
                    self._verify(path, package['files'][path])
                if updated != original:
                    self._commit_config(stages[CONFIG], updated, original, backup, receipt)
                self._write_flag(allow_empty, receipt)
                receipt.update(phase='installed', verified=True, recovery='not_needed')
                final = self._base()
                final['module'].update(installed=True, known_version=CATALOG['version'], boot_enabled=True,
                                       configured=anonymous or self._parse_config(updated)[0]['credentials_set'])
                final['configuration'].update(self._parse_config(updated)[0], exists=True, port_matches=True)
                final.update(state='pending_activation', message='文件已安装并回读校验，待重启后重新检测 FTP', can_configure=True)
                self._advance('installed', final['message'], paths=receipt['paths'])
                return self._with_receipt(final, receipt)
            except Exception as error:
                return self._failure(error, receipt, original)

    def configure_anonymous(self):
        with self._lock:
            self._advance('preflight', '正在检查匿名配置前提', completed=0, total=0, paths=[])
            result, scanned = self._status()
            receipt = {'phase': 'preflight', 'paths': [], 'backup': None, 'verified': False}
            if scanned is None or not result['can_configure']:
                return self._with_receipt(result, receipt)
            original = scanned['config']
            try:
                template = original if original is not None else _MINIMAL_CONFIG
                updated = self._updated_config(template, anonymous=True,
                                               port=self.config.ftp_port if original is None else None)
                needs_config = updated != original
                needs_flag = scanned['flag'] is None
                if not needs_config and not needs_flag and result['state'] == 'ready':
                    # 已经用当前配置登录并验证目录；无修改时复用连接，避免后台FTP频繁重连。
                    receipt.update(phase='noop', verified=True, recovery='not_needed')
                    self._advance('noop', '后台 FTP 已就绪，配置无需修改', paths=[])
                    return self._with_receipt(result, receipt)
                allow_empty = None
                directories = []
                stage = backup = None
                if needs_flag:
                    allow_empty = self._flag_capability()
                elif needs_config:
                    self._capabilities()
                if needs_config:
                    token = uuid.uuid4().hex
                    stage = self._new_path(CONFIG, 'stage', token)
                    backup = self._new_path(CONFIG, 'backup', token) if original is not None else None
                    receipt['backup'] = backup
                    directories = self._preflight([stage] + ([backup] if backup else []),
                                                  extra_paths=[FLAG] if needs_flag else [])
                elif needs_flag:
                    directories = self._preflight([], extra_paths=[FLAG])
                # 所有能力、空文件来源、配置、目录和唯一辅助路径检查均先于首次写入。
                receipt['phase'] = 'mkdir'
                for directory in directories:
                    self._mutate('fs.mkdir', {'path': directory}, receipt)
                if needs_config:
                    self._advance('config_stage', '备份并准备匿名配置', total=len(updated))
                    receipt['phase'] = 'config_stage'
                    self._stage(stage, updated, receipt)
                    if backup:
                        self._backup(original, backup, receipt)
                    self._commit_config(stage, updated, original, backup, receipt)
                if needs_flag:
                    self._write_flag(allow_empty, receipt)
                # 已为匿名时同样断开旧会话，重新登录并验证目录，写入成功不等于服务热读生效。
                receipt['phase'] = 'verify_ftp'
                self.ftp.close()
                result, _ = self._status()
                receipt.update(phase='configured' if needs_config or needs_flag else 'noop',
                               verified=True, recovery='not_needed')
                if result['state'] != 'ready':
                    result['message'] += '；匿名配置已确认，若未热读请重新加载或重启后检测'
                self._advance(receipt['phase'], result['message'], paths=receipt['paths'])
                return self._with_receipt(result, receipt)
            except Exception as error:
                return self._failure(error, receipt, original)
