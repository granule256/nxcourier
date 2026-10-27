"""FTP 一站式服务离线回归：旧 fs、失败恢复、只读检测及凭据隔离。"""
import base64
import hashlib
import importlib
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))
from nxc_host.config import Config
from nxc_host.device import parse_payload
from nxc_host.services import ftp_package

MODULE = '/' + ftp_package.MODULE_PATH
FLAG = '/' + ftp_package.FLAG_PATH
TOOLBOX = '/' + ftp_package.TOOLBOX_PATH
CONFIG = '/' + ftp_package.CONFIG_PATH
SELF_FLAG = '/atmosphere/contents/4200000000000012/flags/boot2.flag'
ORIGINAL = (b'[User]\r\nuser:=\r\n\r\n[Password]\r\npassword:=\r\n'
            b'# anonymous:=1 example\r\n[Port]\r\nport:=5000\r\n'
            b'[Anonymous]\r\nanonymous:=0\r\n[Pause]\r\ndisabled:=0\r\n')
FILES = {MODULE: b'module-fixture' * 101, FLAG: b'',
         TOOLBOX: b'{"name":"sys-ftpd","tid":"420000000000000E"}', CONFIG: ORIGINAL}


def ok(*lines):
    return {'lines': list(lines)}


class FsDevice:
    def __init__(self, files=None, *, empty=False):
        self.files = {SELF_FLAG: b'', **(files or {})}
        self.dirs = {'/', '/atmosphere', '/atmosphere/contents', '/config'}
        for path in self.files:
            self.dirs.update(str(parent) for parent in Path(path).parents)
        self.empty = empty
        self.methods = {'fs.stat', 'fs.read', 'fs.copy', 'fs.write', 'fs.mkdir'}
        self.commands = []
        self.writes = []
        self.hook = None
        self.retry_flags = []

    def call_one(self, command, *, retry_safe=False):
        method, _, fields = command.partition(' ')
        params = parse_payload([fields])[0]
        self.commands.append(command)
        self.retry_flags.append((method, retry_safe))
        if method in ('fs.mkdir', 'fs.write', 'fs.copy'):
            self.writes.append((method, params))
        if self.hook:
            response = self.hook(method, params)
            if response is not None:
                return response
        if method == 'tools.list':
            return ok(*['method=' + name + ' params=' + ('path*(str),data*(base64),append(0|1=0)' +
                       (',allow_empty(0|1=0)' if self.empty else '') if name == 'fs.write' else '')
                       for name in sorted(self.methods)])
        if method not in self.methods:
            return {'error': {'code': 404, 'message': 'unknown-method'}}
        if method == 'fs.stat':
            path = params['path']
            if path in self.files:
                return ok('type=file size=' + str(len(self.files[path])))
            if path in self.dirs:
                return ok('type=dir size=0')
            return {'error': {'code': 404, 'message': 'not-found'}}
        if method == 'fs.read':
            path = params['path']
            if path not in self.files:
                return {'error': {'code': 404, 'message': 'not-found'}}
            offset, size = int(params['offset']), int(params['len'])
            data = self.files[path][offset:offset + size]
            return ok('bytes=' + str(len(data)), '#' + base64.b64encode(data).decode())
        if method == 'fs.mkdir':
            path = params['path']
            if str(Path(path).parent) not in self.dirs:
                return {'error': {'code': 500, 'message': 'missing-parent'}}
            self.dirs.add(path)
            return ok()
        if method == 'fs.write':
            path = params['path']
            data = base64.b64decode(params['data'], validate=True)
            if not data and (not self.empty or params.get('allow_empty') != '1'):
                return {'error': {'code': 400, 'message': 'empty-data'}}
            if str(Path(path).parent) not in self.dirs:
                return {'error': {'code': 500, 'message': 'missing-parent'}}
            self.files[path] = (self.files.get(path, b'') if params.get('append') == '1' else b'') + data
            return ok('bytes=' + str(len(data)))
        if method == 'fs.copy':
            if params['src'] not in self.files or str(Path(params['dst']).parent) not in self.dirs:
                return {'error': {'code': 500, 'message': 'cannot-copy'}}
            self.files[params['dst']] = self.files[params['src']]
            return ok('bytes=' + str(len(self.files[params['dst']])))
        raise AssertionError(command)


class Ftp:
    def __init__(self, device, *, hot=True, available=False, listing=True, message='Connection refused'):
        self.device = device
        self.hot = hot
        self.available = available
        self.listing = listing
        self.closed = 0
        self.message = message

    def close(self):
        self.closed += 1
        if self.hot and b'anonymous:=1' in self.device.files.get(CONFIG, b''):
            self.available = True

    def probe(self):
        return {'available': self.available,
                'message': '' if self.available else self.message}

    def list(self, path, offset=0, count=50):
        if not self.listing:
            raise RuntimeError('目录读取失败')
        return {'entries': []}


class FtpSetupTests(unittest.TestCase):
    def setUp(self):
        implementation = Path(__file__).resolve().parents[1] / 'src/nxc_host/services/ftp_setup.py'
        self.assertTrue(implementation.exists(), '后台 FTP 安装检测服务尚未实现')
        self.module = importlib.import_module('nxc_host.services.ftp_setup')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w') as archive:
            for path, data in FILES.items():
                archive.writestr(path[1:], data)
        self.zip = output.getvalue()
        catalog = dict(ftp_package.CATALOG, sha256=hashlib.sha256(self.zip).hexdigest(),
                       module_sha256=hashlib.sha256(FILES[MODULE]).hexdigest(),
                       files_sha256={path[1:]: hashlib.sha256(data).hexdigest() for path, data in FILES.items()})
        for module in (self.module, ftp_package):
            patcher = patch.object(module, 'CATALOG', catalog)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.device = FsDevice()
        self.ftp = Ftp(self.device)
        self.setup = self.module.FtpSetup(self.device, self.ftp, Config(), Path(self.temp.name))

    def prepared(self):
        return self.setup.prepare({'source': 'local', 'data': base64.b64encode(self.zip).decode()})

    def installed(self, *, anonymous=False, port=5000):
        self.ftp.message = 'FTP 登录失败：430 Invalid username or password'
        self.device.files.update(FILES)
        self.device.files[CONFIG] = ORIGINAL.replace(b'port:=5000', b'port:=' + str(port).encode())
        if anonymous:
            self.device.files[CONFIG] = self.device.files[CONFIG].replace(b'anonymous:=0', b'anonymous:=1')
        for path in FILES:
            self.device.dirs.update(str(parent) for parent in Path(path).parents)

    def assert_no_writes(self):
        self.assertEqual(self.device.writes, [])

    def test_missing_and_unknown_fs_are_distinct(self):
        self.assertEqual(self.setup.status()['state'], 'missing')
        self.device.hook = lambda method, p: {'error': {'code': 403, 'message': 'denied'}} if method == 'fs.stat' else None
        self.assertIn(self.setup.status()['state'], ('error', 'unreachable'))
        self.assert_no_writes()

    def test_ready_requires_directory_validation_and_never_writes(self):
        self.installed(anonymous=True)
        self.ftp.available = True
        result = self.setup.status()
        self.assertEqual(result['state'], 'ready')
        self.assertEqual(result['module']['known_version'], '1.0.5')
        self.ftp.listing = False
        self.assertNotEqual(self.setup.status()['state'], 'ready')
        self.assert_no_writes()

    def test_pending_auth_conflict_and_port_mismatch(self):
        self.installed()
        self.assertEqual(self.setup.status()['state'], 'auth_required')
        self.ftp.probe = lambda: {'available': False, 'message': 'Connection refused'}
        self.assertEqual(self.setup.status()['state'], 'pending_activation')
        self.device.files[CONFIG] = ORIGINAL.replace(b'5000', b'6000')
        result = self.setup.status()
        self.assertFalse(result['configuration']['port_matches'])
        self.assertIn('6000', result['message'])
        self.device.files[MODULE] = b'unknown'
        self.assertEqual(self.setup.status()['state'], 'conflict')
        self.assert_no_writes()

    def test_toolbox_unknown_is_conflict(self):
        self.installed()
        self.device.files[TOOLBOX] = b'{"name":"other-service"}'
        self.assertEqual(self.setup.status()['state'], 'conflict')

    def test_empty_credentials_not_matched_across_lines_and_values_never_returned(self):
        self.installed()
        result = self.setup.status()
        self.assertFalse(result['configuration']['credentials_set'])
        self.device.files[CONFIG] = ORIGINAL.replace(b'user:=\r\n', b'user:=secret-user\r\n').replace(b'password:=\r\n', b'password:=secret-password\r\n')
        result = self.setup.status()
        self.assertTrue(result['configuration']['credentials_set'])
        self.assertNotIn('secret-user', json.dumps(result))
        self.assertNotIn('secret-password', json.dumps(result))

    def test_prepare_local_manifest_has_no_bytes_or_device_commands(self):
        result = self.prepared()
        self.assertEqual(result['version'], '1.0.5')
        self.assertEqual({item['path'] for item in result['files']}, set(FILES))
        self.assertTrue(all('sha256' in item and 'bytes' in item for item in result['files']))
        json.dumps(result)
        self.assertEqual(self.device.commands, [])
        self.assertEqual(self.setup.progress()['phase'], 'prepared')

    def test_prepare_official_delegates_to_verified_package_provider(self):
        package = ftp_package.load_package(self.zip)
        with patch.object(self.module, 'get_official_package', return_value=dict(package, source='official')) as download:
            result = self.setup.prepare({'source': 'official'})
        download.assert_called_once_with(Path(self.temp.name))
        self.assertEqual(result['source'], 'official')

    def test_prepare_rejects_invalid_or_oversized_base64(self):
        for data in ('!!', '', 'A' * (6 * 1024 * 1024)):
            with self.subTest(length=len(data)), self.assertRaises((RuntimeError, ValueError)):
                self.setup.prepare({'source': 'local', 'data': data})
        self.assertEqual(self.device.commands, [])

    def test_legacy_install_381_byte_stage_readback_copy_and_flag_last(self):
        self.setup = self.module.FtpSetup(self.device, self.ftp, Config(ftp_port=6000), Path(self.temp.name))
        result = self.setup.install(self.prepared()['package_id'])
        self.assertEqual(result['state'], 'pending_activation')
        self.assertEqual(self.device.files[MODULE], FILES[MODULE])
        self.assertIn(b'port:=6000', self.device.files[CONFIG])
        self.assertIn(b'anonymous:=1', self.device.files[CONFIG])
        self.assertEqual(self.device.files[FLAG], b'')
        last_method, last = self.device.writes[-1]
        self.assertEqual((last_method, last['dst']), ('fs.copy', FLAG))
        self.assertEqual(last['src'], SELF_FLAG)
        for method, params in self.device.writes:
            if method == 'fs.write':
                self.assertLessEqual(len(base64.b64decode(params['data'])), 381)
                self.assertNotIn(params['path'], (MODULE, TOOLBOX, CONFIG))
        self.assertTrue(all(not retry for method, retry in self.device.retry_flags if method in ('fs.write', 'fs.copy', 'fs.mkdir')))
        self.assertTrue(result['receipt']['verified'])

    def test_modern_empty_flag_uses_allow_empty_last(self):
        self.device.empty = True
        self.device.files.pop(SELF_FLAG)
        result = self.setup.install(self.prepared()['package_id'])
        self.assertEqual(result['state'], 'pending_activation')
        method, params = self.device.writes[-1]
        self.assertEqual((method, params['path'], params['allow_empty']), ('fs.write', FLAG, '1'))

    def test_empty_flag_missing_or_nonempty_rejected_before_any_write(self):
        for content in (None, b'nonempty'):
            with self.subTest(content=content):
                if content is None:
                    self.device.files.pop(SELF_FLAG, None)
                else:
                    self.device.files[SELF_FLAG] = content
                result = self.setup.install(self.prepared()['package_id'])
                self.assertEqual(result['state'], 'error')
                self.assertIn('空', result['message'])
                self.assert_no_writes()

    def test_required_primitive_missing_rejected_before_any_write(self):
        self.device.methods.remove('fs.copy')
        result = self.setup.install(self.prepared()['package_id'])
        self.assertEqual(result['state'], 'error')
        self.assert_no_writes()

    def test_existing_module_never_overwritten_even_pending_or_conflicting(self):
        package = self.prepared()['package_id']
        for content in (FILES[MODULE], b'unknown'):
            self.installed()
            self.device.files[MODULE] = content
            result = self.setup.install(package)
            self.assertIn(result['state'], ('auth_required', 'conflict'))
            self.assert_no_writes()

    def test_ready_install_is_noop(self):
        self.installed(anonymous=True)
        self.ftp.available = True
        result = self.setup.install(self.prepared()['package_id'])
        self.assertEqual(result['state'], 'ready')
        self.assert_no_writes()

    def test_changed_prepared_package_and_foreign_instance_rejected_without_writes(self):
        package = self.prepared()['package_id']
        other = self.module.FtpSetup(self.device, self.ftp, Config(), Path(self.temp.name))
        with self.assertRaises((RuntimeError, ValueError)):
            other.install(package)
        self.assert_no_writes()

    def test_malformed_or_duplicate_configuration_rejected_before_writes(self):
        for config in (b'[Anonymous]\nanonymous:=0\nanonymous:=1\nport:=5000\n', b'anonymous=0\nport:=5000\n', b'anonymous:=0\nport:=garbage\n'):
            with self.subTest(config=config):
                self.device.files[CONFIG] = config
                result = self.setup.install(self.prepared()['package_id'])
                self.assertEqual(result['state'], 'error')
                self.assert_no_writes()

    def test_configure_430_backups_preserves_bytes_and_verifies_hot_reload(self):
        self.installed()
        result = self.setup.configure_anonymous()
        self.assertEqual(result['state'], 'ready')
        expected = ORIGINAL.replace(b'anonymous:=0', b'anonymous:=1')
        self.assertEqual(self.device.files[CONFIG], expected)
        self.assertEqual(self.device.files[result['backup']], ORIGINAL)
        self.assertNotIn(b'port:=6000', self.device.files[CONFIG])
        self.assertGreater(self.ftp.closed, 0)
        self.assertTrue(result['receipt']['verified'])

    def test_configure_already_anonymous_is_noop_and_no_hot_read_lie(self):
        self.installed(anonymous=True)
        self.ftp.available = True
        self.assertEqual(self.setup.configure_anonymous()['state'], 'ready')
        self.assert_no_writes()
        self.ftp.available = False
        self.ftp.hot = False
        self.assertNotEqual(self.setup.configure_anonymous()['state'], 'ready')
        self.assert_no_writes()

    def test_ready_anonymous_noop_reuses_the_verified_connection(self):
        self.installed(anonymous=True)
        self.ftp.available=True
        result=self.setup.configure_anonymous()
        self.assertEqual(result['state'],'ready')
        self.assertEqual(result['receipt']['phase'],'noop')
        self.assertEqual(self.ftp.closed,0)
        self.assert_no_writes()

    def test_configure_does_not_write_unrecognized_module_and_repairs_missing_config(self):
        self.installed()
        self.device.files[MODULE] = b'unknown'
        self.assertEqual(self.setup.configure_anonymous()['state'], 'conflict')
        self.assert_no_writes()
        self.device.files[MODULE] = FILES[MODULE]
        self.device.files.pop(CONFIG)
        self.assertTrue(self.setup.status()['can_configure'])
        repaired = self.setup.configure_anonymous()
        self.assertEqual(repaired['state'], 'ready')
        self.assertIsNone(repaired['backup'])
        self.assertIn(b'anonymous:=1', self.device.files[CONFIG])
        self.assertIn(b'port:=5000', self.device.files[CONFIG])

    def test_failed_backup_verification_does_not_touch_original_config(self):
        self.installed()
        def corrupt(method, params):
            if method == 'fs.read' and 'backup-' in params.get('path', ''):
                return ok('bytes=3', '#' + base64.b64encode(b'bad').decode())
        self.device.hook = corrupt
        result = self.setup.configure_anonymous()
        self.assertEqual(result['state'], 'error')
        self.assertEqual(self.device.files[CONFIG], ORIGINAL)
        self.assertFalse(any(m == 'fs.copy' and p['dst'] == CONFIG for m, p in self.device.writes))

    def test_failed_config_commit_restores_verified_original_without_replaying_commit(self):
        self.installed()
        calls = []
        def fail_commit(method, params):
            if method == 'fs.copy' and params['dst'] == CONFIG and 'stage-' in params['src']:
                calls.append(params.copy())
                self.device.files[CONFIG] = b'partial'
                raise ConnectionResetError('disconnect after partial write')
        self.device.hook = fail_commit
        result = self.setup.configure_anonymous()
        self.assertEqual(result['state'], 'error')
        self.assertEqual(self.device.files[CONFIG], ORIGINAL)
        self.assertEqual(len(calls), 1)
        self.assertEqual(result['receipt']['recovery'], 'restored')
        self.assertIn('commit', result['receipt']['phase'])

    def test_restore_failure_reports_unknown_and_backup(self):
        self.installed()
        def fail(method, params):
            if method == 'fs.copy' and params['dst'] == CONFIG:
                raise ConnectionResetError('unknown write outcome')
        self.device.hook = fail
        result = self.setup.configure_anonymous()
        self.assertEqual(result['receipt']['recovery'], 'unknown')
        self.assertEqual(self.device.files[result['backup']], ORIGINAL)
        self.assertIn('unknown', result['message'])

    def test_stage_disconnect_not_retried_and_no_flag_written(self):
        def fail(method, params):
            if method == 'fs.write':
                raise ConnectionResetError('lost response')
        self.device.hook = fail
        result = self.setup.install(self.prepared()['package_id'])
        self.assertEqual(result['state'], 'error')
        self.assertEqual(len([m for m, _ in self.device.writes if m == 'fs.write']), 1)
        self.assertNotIn(FLAG, self.device.files)
        self.assertTrue(result['paths'])

    def test_progress_reads_memory_without_device_or_operation_lock(self):
        prepared = self.prepared()
        before = list(self.device.commands)
        snapshot = self.setup.progress()
        snapshot['phase'] = 'changed externally'
        result = []
        worker = threading.Thread(target=lambda: result.append(self.setup.progress()))
        worker.start()
        worker.join(1)
        self.assertFalse(worker.is_alive())
        self.assertEqual(result[0]['phase'], 'prepared')
        self.assertEqual(self.device.commands, before)
        self.assertTrue(prepared['package_id'])

    def test_frontend_ftp_without_known_module_is_conflict_and_blocks_install(self):
        self.ftp.available = True
        result = self.setup.status()
        self.assertEqual(result['state'], 'conflict')
        self.assertTrue(result['ftp']['available'])
        self.assertEqual(self.setup.install(self.prepared()['package_id'])['state'], 'conflict')
        self.assert_no_writes()

    def test_frontend_ftp_auth_rejection_without_module_blocks_install_without_writes(self):
        for code in ('430', '530'):
            with self.subTest(code=code):
                self.ftp.message = 'FTP 登录失败：' + code + ' Invalid username or password'
                result = self.setup.status()
                self.assertEqual(result['state'], 'conflict')
                self.assertFalse(result['module']['installed'])
                self.assertFalse(result['ftp']['available'])
                result = self.setup.install(self.prepared()['package_id'])
                self.assertEqual(result['state'], 'conflict')
                self.assert_no_writes()

    def test_recognized_backend_auth_rejection_remains_auth_required(self):
        self.installed()
        for code in ('430', '530'):
            with self.subTest(code=code):
                self.ftp.message = 'FTP 登录失败：' + code + ' Invalid username or password'
                self.assertEqual(self.setup.status()['state'], 'auth_required')
                self.assert_no_writes()

    def test_account_mode_without_credentials_rejected_before_writes(self):
        result = self.setup.install(self.prepared()['package_id'], anonymous=False)
        self.assertEqual(result['state'], 'error')
        self.assert_no_writes()

    def test_account_mode_preserves_complete_existing_credentials(self):
        self.device.files[CONFIG] = ORIGINAL.replace(b'user:=\r\n', b'user:=username\r\n').replace(b'password:=\r\n', b'password:=password-value\r\n')
        self.device.dirs.add('/config/sys-ftpd')
        result = self.setup.install(self.prepared()['package_id'], anonymous=False)
        self.assertEqual(result['state'], 'pending_activation')
        self.assertIn(b'anonymous:=0', self.device.files[CONFIG])
        self.assertIn(b'user:=username', self.device.files[CONFIG])
        self.assertNotIn('password-value', json.dumps(result))

    def test_bootflag_missing_is_explicit_pending_state(self):
        self.installed()
        self.device.files.pop(FLAG)
        result = self.setup.status()
        self.assertEqual(result['state'], 'pending_activation')
        self.assertIn('boot2.flag', result['message'])
        self.assert_no_writes()

    def test_target_change_and_mutated_prepared_content_rejected_without_writes(self):
        package_id = self.prepared()['package_id']
        self.setup.config = Config(device_ip='127.0.0.2')
        with self.assertRaises((RuntimeError, ValueError)):
            self.setup.install(package_id)
        self.assert_no_writes()
        self.setup = self.module.FtpSetup(self.device, self.ftp, Config(), Path(self.temp.name))
        package_id = self.prepared()['package_id']
        self.setup._packages[package_id]['files'][MODULE] = b'tampered'
        with self.assertRaises((RuntimeError, ValueError)):
            self.setup.install(package_id)
        self.assert_no_writes()

    def test_configure_repairs_bootflag_last_without_touching_module(self):
        self.installed(anonymous=True)
        self.device.files.pop(FLAG)
        result = self.setup.configure_anonymous()
        self.assertEqual(result['state'], 'ready')
        self.assertEqual(self.device.files[FLAG], b'')
        self.assertEqual(self.device.writes[-1][1]['dst'], FLAG)
        self.assertFalse(any(p.get('dst') == MODULE or p.get('path') == MODULE for _, p in self.device.writes))

    def test_configure_missing_flag_requires_empty_source_before_config_writes(self):
        self.installed()
        self.device.files.pop(FLAG)
        self.device.files.pop(SELF_FLAG)
        result = self.setup.configure_anonymous()
        self.assertEqual(result['state'], 'error')
        self.assert_no_writes()

    def test_status_requires_declared_read_primitives_and_never_assumes_missing(self):
        self.device.methods.remove('fs.stat')
        result = self.setup.status()
        self.assertEqual(result['state'], 'error')
        self.assertIsNone(result['module']['installed'])
        self.assert_no_writes()

    def test_status_supports_readonly_fs_without_write_primitives(self):
        self.device.methods = {'fs.stat', 'fs.read'}
        self.installed(anonymous=True)
        self.ftp.available = True
        self.assertEqual(self.setup.status()['state'], 'ready')
        self.assertTrue(all(retry for method, retry in self.device.retry_flags))
        self.assert_no_writes()

    def test_install_preserves_and_backups_existing_configuration(self):
        original = ORIGINAL + b'# keep custom bytes \xff\r\nother:=12\r\n'
        self.device.files[CONFIG] = original
        self.device.dirs.add('/config/sys-ftpd')
        result = self.setup.install(self.prepared()['package_id'])
        self.assertEqual(result['state'], 'pending_activation')
        self.assertEqual(self.device.files[result['backup']], original)
        self.assertEqual(self.device.files[CONFIG], original.replace(b'anonymous:=0', b'anonymous:=1'))
        self.assertEqual(self.device.writes[-1][1]['dst'], FLAG)

    def test_install_config_commit_failure_restores_original_and_never_writes_flag(self):
        self.device.files[CONFIG] = ORIGINAL
        self.device.dirs.add('/config/sys-ftpd')
        def fail_commit(method, params):
            if method == 'fs.copy' and params['dst'] == CONFIG and 'stage-' in params['src']:
                self.device.files[CONFIG] = b'partial'
                raise ConnectionResetError('partial config write')
        self.device.hook = fail_commit
        result = self.setup.install(self.prepared()['package_id'])
        self.assertEqual(result['state'], 'error')
        self.assertEqual(result['receipt']['recovery'], 'restored')
        self.assertEqual(self.device.files[CONFIG], ORIGINAL)
        self.assertNotIn(FLAG, self.device.files)
        self.assertEqual(self.device.files[MODULE], FILES[MODULE])

    def test_configuration_without_hot_reload_does_not_claim_ready(self):
        self.installed()
        self.ftp.hot = False
        result = self.setup.configure_anonymous()
        self.assertEqual(result['state'], 'auth_required')
        self.assertTrue(result['receipt']['verified'])
        self.assertFalse(result['ftp']['available'])

    def test_existing_configuration_port_preserved_during_anonymous_repair(self):
        self.installed(port=6000)
        result = self.setup.configure_anonymous()
        self.assertEqual(result['configuration']['port'], 6000)
        self.assertFalse(result['configuration']['port_matches'])
        self.assertIn(b'port:=6000', self.device.files[CONFIG])
        self.assertEqual(self.setup.config.ftp_port, 5000)

    def test_progress_remains_available_during_a_blocked_device_write(self):
        prepared = self.prepared()['package_id']
        entered, release = threading.Event(), threading.Event()
        results = []
        def block(method, params):
            if method == 'fs.write':
                entered.set()
                if not release.wait(2):
                    raise RuntimeError('test timeout')
        self.device.hook = block
        worker = threading.Thread(target=lambda: results.append(self.setup.install(prepared)))
        worker.start()
        try:
            self.assertTrue(entered.wait(1))
            progress_results = []
            reader = threading.Thread(target=lambda: progress_results.append(self.setup.progress()))
            reader.start()
            reader.join(0.5)
            self.assertFalse(reader.is_alive())
            self.assertTrue(progress_results[0]['phase'].startswith('stage_'))
        finally:
            release.set()
            worker.join(2)
        self.assertEqual(results[0]['state'], 'pending_activation')

    def test_read_limit_blocks_oversize_remote_without_read(self):
        self.installed()
        def huge(method, params):
            if method == 'fs.stat' and params['path'] == MODULE:
                return ok('type=file size=4194305')
        self.device.hook = huge
        result = self.setup.status()
        self.assertEqual(result['state'], 'error')
        self.assertFalse(any(c.startswith('fs.read path=' + MODULE) for c in self.device.commands))


if __name__ == '__main__':
    unittest.main()
