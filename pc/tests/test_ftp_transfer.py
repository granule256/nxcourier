"""FTP 真实 socket 回归；无需设备和第三方 FTP 依赖。"""
import hashlib
import importlib
from pathlib import Path
import socket
import sys
import tempfile
import traceback
import uuid
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/src'))
sys.path.insert(0, str(ROOT / 'pc/tests'))
from helpers.ftp_server import FtpServer


class FtpTransferTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue((ROOT / 'pc/src/nxc_host/services/ftp.py').is_file(),
                        'FTP 客户端服务尚未实现')
        self.module = importlib.import_module('nxc_host.services.ftp')
        self.temp = tempfile.TemporaryDirectory(prefix='nxc FTP 中文 ')
        self.addCleanup(self.temp.cleanup)
        self.local = Path(self.temp.name) / '二进制 文件.bin'

    def client(self, server):
        client = self.module.FtpClient('127.0.0.1', server.port, timeout=2)
        self.addCleanup(client.close)
        return client

    def test_download_creates_missing_parent_directories(self):
        with FtpServer() as server:
            server.files['/file']=b'contents'
            target=Path(self.temp.name)/'new directory'/'nested'/'file.bin'
            self.client(server).download('/file',target)
            self.assertEqual(target.read_bytes(),b'contents')

    def test_binary_empty_unicode_upload_download_sha_read_and_session_reuse(self):
        with FtpServer() as server:
            server.dirs.add('/中文 目录')
            client = self.client(server)
            for payload in (bytes(range(256)) * 700, b''):
                self.local.write_bytes(payload)
                remote = '/中文 目录/文件 空格.bin'
                result = client.upload(self.local, remote)
                self.assertEqual(server.files[remote], payload)
                self.assertEqual(result['remote'], remote)
                self.assertEqual(result['bytes'], len(payload))
                self.assertEqual(result['sha256'], hashlib.sha256(payload).hexdigest())
                self.assertIs(result['verified'], True)
                self.assertEqual(result['transport'], 'ftp')
                self.assertIn('backup', result)
                self.assertIn('rollback', result)
                summary = client.sha(remote)
                self.assertEqual(summary['bytes'], len(payload))
                self.assertEqual(summary['sha256'], result['sha256'])
                self.assertEqual(client.read(remote, max_bytes=len(payload)), payload)
                output = Path(self.temp.name) / 'download.bin'
                receipt = client.download(remote, output)
                self.assertEqual(output.read_bytes(), payload)
                self.assertEqual(receipt['bytes'], len(payload))
            self.assertEqual(server.connections, 1)
            self.assertIn(('USER', 'anonymous'), server.commands)
            self.assertIn(('PASS', ''), server.commands)

    def test_list_mlsd_fallback_pagination_and_stat(self):
        for supported in (True, False):
            with self.subTest(mlsd=supported), FtpServer(mlsd=supported) as server:
                server.files.update({'/a 空格': b'abc', '/c': b'12345'})
                server.dirs.add('/b 中文')
                client = self.client(server)
                page = client.list('/', count=2)
                self.assertEqual([e['name'] for e in page['entries']], ['a 空格', 'b 中文'])
                self.assertTrue(page['truncated'])
                self.assertEqual(page['offset'], 0)
                self.assertEqual(page['next_offset'], 2)
                self.assertEqual(client.list('/', offset=2)['entries'][0]['name'], 'c')
                self.assertFalse(client.list('/', offset=2)['truncated'])
                self.assertEqual(client.list('/', offset=2)['next_offset'], -1)
                self.assertEqual(client.stat('/a 空格'), {'path': '/a 空格', 'type': 'file', 'size': 3})
                self.assertEqual(client.stat('/b 中文')['type'], 'dir')
                self.assertIsNone(client.stat('/missing', missing_ok=True))
                with self.assertRaisesRegex(RuntimeError, '不存在'):
                    client.stat('/missing')

    def test_permission_is_not_treated_as_missing(self):
        with FtpServer(mlsd=False) as server:
            server.permission_denied.add('/secret')
            client = self.client(server)
            with self.assertRaisesRegex(RuntimeError, 'FTP'):
                client.stat('/secret', missing_ok=True)
            self.local.write_bytes(b'new')
            with self.assertRaises(RuntimeError):
                client.upload(self.local, '/secret')
            self.assertFalse(any(cmd == 'STOR' for cmd, _ in server.commands))

    def test_failed_download_preserves_local_and_removes_temporary(self):
        with FtpServer() as server:
            server.files['/file'] = b'incomplete'
            server.fail_retr.add('/file')
            self.local.write_bytes(b'original local')
            with self.assertRaisesRegex(RuntimeError, 'FTP'):
                self.client(server).download('/file', self.local)
            self.assertEqual(self.local.read_bytes(), b'original local')
            self.assertEqual(list(Path(self.temp.name).iterdir()), [self.local])

    def test_226_with_short_payload_preserves_local_and_removes_temporary(self):
        with FtpServer() as server:
            server.files['/file'] = b'complete remote payload'
            server.retr_limit['/file'] = 3
            self.local.write_bytes(b'original local')
            with self.assertRaisesRegex(RuntimeError, '长度|截断'):
                self.client(server).download('/file', self.local)
            self.assertEqual(self.local.read_bytes(), b'original local')
            self.assertEqual(list(Path(self.temp.name).iterdir()), [self.local])

    def test_server_password_echo_is_absent_from_full_default_traceback(self):
        password = 'private-' + uuid.uuid4().hex
        for operation in ('list', 'upload'):
            with self.subTest(operation=operation), FtpServer() as server:
                client = self.module.FtpClient('127.0.0.1', server.port, timeout=2,
                                               user='tester', password=password)
                self.addCleanup(client.close)
                if operation == 'list':
                    server.login = False
                    server.login_error = '530 Login rejected: ' + password
                else:
                    server.permission_denied.add('/target')
                    server.permission_message = '550 Permission denied: ' + password
                    self.local.write_bytes(b'new content')
                try:
                    if operation == 'list':
                        client.list('/')
                    else:
                        client.upload(self.local, '/target')
                except RuntimeError as error:
                    rendered = ''.join(traceback.format_exception(error))
                    self.assertNotIn(password, str(error))
                    self.assertNotIn(password, rendered)
                    self.assertIn('***', rendered)
                else:
                    self.fail('服务器拒绝操作必须抛出 RuntimeError')

    def test_upload_bad_readback_preserves_original(self):
        with FtpServer() as server:
            server.files['/target'] = b'old'
            server.corrupt_upload_read = True
            self.local.write_bytes(b'new')
            with self.assertRaisesRegex(RuntimeError, '校验'):
                self.client(server).upload(self.local, '/target')
            self.assertEqual(server.files['/target'], b'old')
            self.assertFalse(any(cmd == 'RNFR' for cmd, _ in server.commands))

    def test_rename_commit_failure_restores_original_and_reports_paths(self):
        with FtpServer() as server:
            server.files['/target'] = b'old'
            server.fail_commit = True
            self.local.write_bytes(b'new')
            with self.assertRaisesRegex(RuntimeError, '已恢复.*暂存路径.*备份路径'):
                self.client(server).upload(self.local, '/target')
            self.assertEqual(server.files['/target'], b'old')
            self.assertEqual(sum(cmd == 'RNTO' for cmd, _ in server.commands), 3)

    def test_failed_restore_retains_backup_and_reports_recovery_paths(self):
        with FtpServer() as server:
            server.files['/target'] = b'old'
            server.fail_commit = server.fail_restore = True
            self.local.write_bytes(b'new')
            with self.assertRaisesRegex(RuntimeError, '恢复失败.*目标路径.*暂存路径.*备份路径'):
                self.client(server).upload(self.local, '/target')
            self.assertNotIn('/target', server.files)
            self.assertTrue(any(path.startswith('/.nxc-backup-') and data == b'old'
                                for path, data in server.files.items()))

    def test_probe_is_read_only_and_unavailable_errors_are_readable(self):
        with FtpServer() as server:
            result = self.client(server).probe()
            self.assertTrue(result['available'])
            self.assertEqual(result['transport'], 'ftp')
            self.assertFalse(any(cmd in {'STOR', 'RNFR', 'RNTO', 'DELE', 'MKD', 'RMD'}
                                 for cmd, _ in server.commands))
        with FtpServer(login=False) as server:
            result = self.client(server).probe()
            self.assertFalse(result['available'])

            self.assertIn('FTP', result['message'])
        with socket.socket() as bound:
            bound.bind(('127.0.0.1', 0))
            port = bound.getsockname()[1]
        client = self.module.FtpClient('127.0.0.1', port, timeout=.2)
        self.addCleanup(client.close)
        self.assertFalse(client.probe()['available'])
        with self.assertRaisesRegex(RuntimeError, 'FTP'):
            client.list('/')

    def test_probe_reconnects_once_after_idle_control_connection_dies(self):
        with FtpServer() as server:
            client = self.client(server)
            self.assertTrue(client.probe()['available'])
            client._ftp.sock.shutdown(socket.SHUT_RDWR)
            self.assertTrue(client.probe()['available'])
            self.assertEqual(server.connections, 2)
            self.assertFalse(any(command in {'STOR', 'RNFR', 'RNTO'} for command, _ in server.commands))

    def test_disconnect_does_not_replay_write(self):
        with FtpServer() as server:
            server.drop_stor = True
            self.local.write_bytes(b'data')
            client = self.client(server)
            with self.assertRaisesRegex(RuntimeError, 'FTP'):
                client.upload(self.local, '/target')
            self.assertEqual(sum(cmd == 'STOR' for cmd, _ in server.commands), 1)
            self.assertEqual(server.connections, 1)
            self.assertFalse(client._ftp)

    def test_first_rename_rejection_preserves_original(self):
        with FtpServer() as server:
            server.files['/target'] = b'old'
            server.fail_backup = True
            self.local.write_bytes(b'new')
            with self.assertRaisesRegex(RuntimeError, '备份路径'):
                self.client(server).upload(self.local, '/target')
            self.assertEqual(server.files['/target'], b'old')

    def test_backup_readback_failure_restores_original(self):
        with FtpServer() as server:
            server.files['/target'] = b'old'
            server.corrupt_backup_read = True
            self.local.write_bytes(b'new')
            with self.assertRaisesRegex(RuntimeError, '备份校验失败.*已恢复'):
                self.client(server).upload(self.local, '/target')
            self.assertEqual(server.files['/target'], b'old')

    def test_mlsd_permission_error_does_not_fallback_and_unknown_nlst_is_explicit(self):
        with FtpServer() as server:
            server.dirs.add('/private')
            server.permission_denied.add('/private')
            with self.assertRaises(RuntimeError):
                self.client(server).list('/private')
            self.assertFalse(any(cmd == 'NLST' for cmd, _ in server.commands))
        with FtpServer(mlsd=False) as server:
            server.files['/secret'] = b'data'
            server.permission_denied.add('/secret')
            entry = self.client(server).list('/')['entries'][0]
            self.assertEqual(entry, {'name': 'secret', 'type': 'unknown', 'size': None})

    def test_invalid_nonpath_input_is_chinese_runtime_error(self):
        with FtpServer() as server:
            client = self.client(server)
            with self.assertRaisesRegex(RuntimeError, '路径'):
                client.stat(12)

    def test_validation_and_read_limit(self):
        with FtpServer() as server:
            server.files['/file'] = b'12345'
            client = self.client(server)
            for path in ('', 'relative', 'sdmc:/file', '../x', '/a/../b', '/bad\r\nDELE x', '/bad\0x'):
                before = len(server.commands)
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    client.stat(path)
                self.assertEqual(len(server.commands), before, '非法路径不应到达服务器')
            with self.assertRaisesRegex(RuntimeError, '上限'):
                client.read('/file', max_bytes=4)
            for kwargs in ({'offset': -1}, {'count': 0}):
                with self.assertRaises(RuntimeError):
                    client.list('/', **kwargs)


if __name__ == '__main__':
    unittest.main()
