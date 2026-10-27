"""真实 HTTP后台与本地 FTP 双连接集成，不需要真实设备。"""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/src'))
sys.path.insert(0, str(ROOT / 'pc/tests'))
from helpers.ftp_server import FtpServer
from nxc_host.client import HostClient
from nxc_host.config import Config
from nxc_host.daemon import HostServer
from nxc_host.runtime import HostRuntime


class TransferIntegrationTests(unittest.TestCase):
    def test_http_mcp_ftp_transfer_and_target_binding(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {'NXC_CONFIG_DIR': directory}), FtpServer() as ftp:
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                host_port = listener.getsockname()[1]
            config = Config(device_ip='127.0.0.1', host_port=host_port, ftp_port=ftp.port, timeout=2)
            device = Mock()
            runtime = HostRuntime(config, device=device)
            server = HostServer(config, runtime)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            client = HostClient(config)
            def rpc(method, params=None):
                result = client.request({'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params or {}})
                self.assertNotIn('error', result, result)
                return result['result']
            try:
                content = bytes(range(256)) * 500
                ftp.files['/目标 文件.bin'] = b'old'
                receipt = rpc('host.upload', {'remote_path': '/目标 文件.bin',
                              'data': base64.b64encode(content).decode(), 'transport': 'auto'})
                self.assertEqual(receipt['transport'], 'ftp')
                self.assertTrue(receipt['verified'])
                self.assertEqual(ftp.files[receipt['backup']], b'old')
                self.assertEqual(ftp.files['/目标 文件.bin'], content)
                listing = rpc('host.files.list', {'path': '/', 'transport': 'ftp'})
                self.assertIn('目标 文件.bin', [entry['name'] for entry in listing['entries']])
                downloaded = rpc('host.download', {'remote_path': '/目标 文件.bin'})
                self.assertEqual(base64.b64decode(downloaded['data']), content)
                self.assertEqual(downloaded['sha256'], hashlib.sha256(content).hexdigest())
                local = Path(directory) / 'local.bin'
                result = rpc('tools/call', {'name': 'nxc_fetch', 'arguments': {
                    'remote_path': '/目标 文件.bin', 'local_path': str(local), 'transport': 'ftp'}})
                self.assertFalse(result.get('isError'), result)
                self.assertEqual(local.read_bytes(), content)
                tools = rpc('tools/list')['tools']
                self.assertEqual(len(tools), 8)
                for tool in tools:
                    if tool['name'] in ('nxc_put', 'nxc_fetch', 'nxc_deploy'):
                        self.assertEqual(tool['inputSchema']['properties']['transport']['enum'], ['auto', 'fs', 'ftp'])
                device.call_one.assert_not_called()
                old = client.status()
                rpc('host.configure', {'ftp_port': ftp.port, 'ftp_user': 'changed'})
                count = len(ftp.commands)
                stale = client._http('/rpc', {'jsonrpc': '2.0', 'id': 2, 'method': 'host.upload',
                                            'params': {'remote_path': '/must-not-exist', 'data': 'YQ=='}}, context=old)
                self.assertEqual(stale['error']['code'], -32009)
                self.assertEqual(len(ftp.commands), count)
                self.assertNotIn('/must-not-exist', ftp.files)
            finally:
                server.shutdown()
                server.server_close()
                runtime.close()
                thread.join(2)
                self.assertFalse(thread.is_alive())
