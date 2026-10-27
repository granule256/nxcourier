"""统一 CLI 的无设备回归。"""
import base64
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace, ModuleType
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))


class FakeClient:
    def __init__(self, config):
        self.config = config
        self.calls = []
        self.reply = {'jsonrpc': '2.0', 'id': 1, 'result': {'content': [
            {'type': 'text', 'text': '{"ok":true}'}]}}

    def request(self, request, autostart=True):
        self.calls.append(request)
        return self.reply

    def start(self):
        self.calls.append('start')
        return {'running': True, 'port': 47801}

    def status(self):
        self.calls.append('status')
        return {'running': False}

    def stop(self):
        self.calls.append('stop')
        return {'running': False}


class CliTests(unittest.TestCase):
    def setUp(self):
        from nxc_host import cli
        self.cli = cli
        self.config = SimpleNamespace(host_port=47801)
        self.client = FakeClient(self.config)
        self.overrides = None
        self.out, self.err = io.StringIO(), io.StringIO()

    def run_cli(self, *args):
        def loader(overrides):
            self.overrides = overrides
            return self.config
        with contextlib.redirect_stdout(self.out), contextlib.redirect_stderr(self.err):
            return self.cli.main(list(args), client_factory=lambda config: self.client,
                                 config_loader=loader)

    def test_call_preserves_natural_values_and_global_flags_after_command(self):
        self.assertEqual(self.run_cli('call', 'fs.ls', 'path=/a b/100%', '--ip',
                                      '127.0.0.2', '--port', '5000', '--json'), 0)
        self.assertEqual(self.overrides, {'device_ip': '127.0.0.2', 'device_port': 5000})
        self.assertEqual(self.client.calls[0]['params'], {'name': 'nxc_call',
            'arguments': {'method': 'fs.ls', 'params': {'path': '/a b/100%'}}})
        self.assertEqual(json.loads(self.out.getvalue()), {'ok': True})

    def test_raw_is_not_encoded(self):
        self.assertEqual(self.run_cli('call', '--raw', 'fs.ls path=/a%20b'), 0)
        self.assertEqual(self.client.calls[0]['params']['arguments'],
                         {'raw': 'fs.ls path=/a%20b'})

    def test_file_transport_is_forwarded_to_shared_background(self):
        self.assertEqual(self.run_cli('put', '/tmp/a', '/remote', '--transport', 'ftp'), 0)
        self.assertEqual(self.client.calls[0]['params']['arguments']['transport'], 'ftp')
        self.client.calls.clear()
        self.assertEqual(self.run_cli('ftp', 'status'), 0)
        self.assertEqual(self.client.calls[0]['method'], 'host.ftp.status')

    def test_status_does_not_start_and_web_opens_shared_port(self):
        self.assertEqual(self.run_cli('status'), 0)
        self.assertEqual(self.client.calls, ['status'])
        self.client.calls.clear()
        with patch('webbrowser.open') as opened:
            self.assertEqual(self.run_cli('web'), 0)
        opened.assert_called_once_with('http://127.0.0.1:47801/')
        self.assertEqual(self.client.calls, ['start'])

    def test_rpc_and_tool_error_return_failure(self):
        for response in ({'error': {'code': -1, 'message': '失败'}},
                         {'result': {'isError': True, 'content': [
                             {'type': 'text', 'text': '失败'}]}}):
            self.client.reply = response
            self.assertEqual(self.run_cli('caps'), 1)
        self.assertIn('失败', self.err.getvalue())

    def test_images_written_without_base64_output(self):
        encoded = base64.b64encode(b'fake png').decode()
        self.client.reply = {'result': {'content': [
            {'type': 'image', 'mimeType': 'image/png', 'data': encoded},
            {'type': 'text', 'text': '截图完成'}]}}
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'nested' / 'screen.png'
            self.assertEqual(self.run_cli('see', '--output', str(target), '--json'), 0)
            self.assertEqual(target.read_bytes(), b'fake png')
        self.assertNotIn(encoded, self.out.getvalue())
        self.assertIn('截图完成', self.out.getvalue())

    def test_default_image_folder_is_user_downloads(self):
        self.client.reply = {'result': {'content': [
            {'type': 'image', 'mimeType': 'image/jpeg', 'data': 'eA=='}]}}
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(self.cli, '_downloads_dir', return_value=Path(directory)):
                self.assertEqual(self.run_cli('step', 'A:100', '--wait', 'change'), 0)
            self.assertEqual([p.read_bytes() for p in Path(directory).iterdir()], [b'x'])
        self.assertEqual(self.client.calls[0]['params']['arguments'],
                         {'steps': 'A:100', 'wait': 'change'})

    def test_routes_and_local_file_paths(self):
        cases = [
            (['caps'], 'tools/call', {'name': 'nxc_caps', 'arguments': {}}),
            (['doctor'], 'host.doctor', {}),
            (['sha', '/switch/a'], 'host.sha', {'remote_path': '/switch/a'}),
                        (['gdb', 'attach', '--pid', '20'], 'tools/call',
             {'name': 'nxc_gdb', 'arguments': {'action': 'attach', 'pid': 20}}),
            (['gdb', 'interrupt'], 'tools/call',
             {'name': 'nxc_gdb', 'arguments': {'action': 'interrupt'}}),
            (['gdb', 'open', '--no-connect'], 'tools/call',
             {'name': 'nxc_gdb', 'arguments': {'action': 'open', 'connect': False}}),
            (['gdb', 'run', 'info threads', 'bt'], 'tools/call',
             {'name': 'nxc_gdb', 'arguments': {'action': 'run',
                                            'commands': ['info threads', 'bt']}}),
        ]
        for args, method, params in cases:
            with self.subTest(args=args):
                self.assertEqual(self.run_cli(*args), 0)
                self.assertEqual(self.client.calls[-1]['method'], method)
                self.assertEqual(self.client.calls[-1]['params'], params)
        for command, args, expected in [
            ('put', ['a.bin', '/switch/a'], {'local_path': str(Path('a.bin').resolve()),
                                           'remote_path': '/switch/a'}),
            ('fetch', ['/switch/a', 'a.bin'], {'remote_path': '/switch/a',
                                              'local_path': str(Path('a.bin').resolve())}),
            ('deploy', ['a.bin'], {'local_path': str(Path('a.bin').resolve())}),
        ]:
            self.assertEqual(self.run_cli(command, *args), 0)
            self.assertEqual(self.client.calls[-1]['params'],
                             {'name': 'nxc_' + command, 'arguments': expected})

    def test_boot_once_and_polling_do_not_reboot(self):
        responses = iter([{'ready': True, 'boot': 3}, {'ready': True, 'boot': 4}])
        def request(message, autostart=True):
            self.client.calls.append(message)
            return {'result': next(responses)}
        self.client.request = request
        with patch.object(self.cli.time, 'sleep') as sleep:
            self.assertEqual(self.run_cli('boot', '--max-wait', '8'), 0)
        sleep.assert_called_once()
        self.assertEqual([c['method'] for c in self.client.calls], ['host.boot', 'host.boot'])
        self.assertTrue(all(c['params'] == {} for c in self.client.calls))
        self.client.request = lambda *a, **k: {'result': {'ready': True, 'boot': 3}}
        self.assertEqual(self.run_cli('boot', '--once', '--expect-boot', '3'), 0)
        self.assertEqual(self.run_cli('boot', '--once', '--expect-boot', '4'), 1)
        self.assertEqual(self.run_cli('boot', '--max-wait', '0'), 1)

    def test_fetch_default_targets_invoking_directory(self):
        self.assertEqual(self.run_cli('fetch', '/switch/a.bin'), 0)
        self.assertEqual(self.client.calls[-1]['params']['arguments'],
                         {'remote_path': '/switch/a.bin',
                          'local_path': str(Path('a.bin').resolve())})

    def test_raw_with_method_only_sends_raw(self):
        self.assertEqual(self.run_cli('call', 'fs.ls', '--raw', 'fs.ls path=/a%20b'), 0)
        self.assertEqual(self.client.calls[-1]['params']['arguments'],
                         {'raw': 'fs.ls path=/a%20b'})

    def test_foreground_and_internal_server_use_config_without_starting_client(self):
        daemon = ModuleType('nxc_host.daemon')
        seen = []
        daemon.serve = lambda config: seen.append(config)
        with patch.dict(sys.modules, {'nxc_host.daemon': daemon}):
            self.assertEqual(self.run_cli('start', '--foreground'), 0)
            self.assertEqual(self.run_cli('_serve'), 0)
        self.assertEqual(seen, [self.config, self.config])
        self.assertEqual(self.client.calls, [])

    def test_mcp_command_uses_bridge(self):
        with patch('nxc_host.mcp.run_stdio', return_value=0) as bridge:
            self.assertEqual(self.run_cli('mcp'), 0)
        bridge.assert_called_once_with(self.client)

    def test_invalid_image_fails_without_printing_base64(self):
        self.client.reply = {'result': {'content': [
            {'type': 'image', 'mimeType': 'image/png', 'data': 'BROKEN BASE64'}]}}
        self.assertEqual(self.run_cli('see'), 1)
        self.assertNotIn('BROKEN BASE64', self.out.getvalue())

    def test_nonfinite_boot_wait_rejected(self):
        for value in ('nan', 'inf', '-1'):
            with self.assertRaises(SystemExit) as caught:
                self.run_cli('boot', '--max-wait', value)
            self.assertEqual(caught.exception.code, 2)

    def test_version_has_no_host_side_effect(self):
        self.assertEqual(self.run_cli('--version'), 0)
        self.assertEqual(self.client.calls, [])
        self.assertIsNone(self.overrides)

    def test_host_exception_is_chinese_diagnostic(self):
        self.client.request = lambda *a, **k: (_ for _ in ()).throw(RuntimeError('服务不可达'))
        self.assertEqual(self.run_cli('caps'), 1)
        self.assertIn('服务不可达', self.err.getvalue())


if __name__ == '__main__':
    unittest.main()
