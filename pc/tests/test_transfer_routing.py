"""双通道选择、目标绑定及凭据输出回归；不连接真实设备。"""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))


class RoutingTests(unittest.TestCase):
    def setUp(self):
        from nxc_host.services.files import FileTransfers
        self.ftp = Mock()
        self.device = Mock()
        self.files = FileTransfers(self.device, self.ftp)

    def test_auto_chooses_ftp_before_mutation(self):
        self.ftp.probe.return_value = {'available': True}
        self.ftp.upload.return_value = {'verified': True}
        with patch('nxc_host.services.files.tools.tool_put') as fs:
            result = self.files.put({'local_path': '/local', 'remote_path': '/remote'})
        self.assertEqual(result['transport'], 'ftp')
        self.ftp.upload.assert_called_once_with('/local', '/remote')
        fs.assert_not_called()

    def test_auto_falls_back_only_at_probe(self):
        self.ftp.probe.return_value = {'available': False}
        with patch('nxc_host.services.files.tools.tool_put', return_value={'verified': True}) as fs:
            self.assertEqual(self.files.put({'local_path': '/l', 'remote_path': '/r'})['transport'], 'fs')
        fs.assert_called_once()
        self.ftp.upload.assert_not_called()

    def test_ftp_write_failure_never_falls_back(self):
        self.ftp.probe.return_value = {'available': True}
        self.ftp.upload.side_effect = RuntimeError('写入结果未知')
        with patch('nxc_host.services.files.tools.tool_put') as fs:
            with self.assertRaisesRegex(RuntimeError, '未知'):
                self.files.put({'local_path': '/l', 'remote_path': '/r'})
        fs.assert_not_called()

    def test_explicit_fs_never_probes_ftp(self):
        with patch('nxc_host.services.files.tools.tool_fetch', return_value={}) as fs:
            result = self.files.fetch({'transport': 'fs', 'remote_path': '/r', 'local_path': '/l'})
        self.assertEqual(result['transport'], 'fs')
        self.ftp.probe.assert_not_called()
        fs.assert_called_once()

    def test_invalid_transport_rejected_before_any_io(self):
        with self.assertRaisesRegex(ValueError, 'transport'):
            self.files.put({'transport': 'sftp', 'local_path': '/l', 'remote_path': '/r'})
        self.ftp.probe.assert_not_called()

    def test_fs_sha_uses_negotiated_device_hash(self):
        expected={'bytes':3,'sha256':'a'*64}
        self.device.call_one.side_effect=AssertionError('不应从路由层重新逐块读取文件')
        with patch('nxc_host.services.files.tools.remote_file_hash',return_value=expected) as digest:
            result=self.files.sha({'remote_path':'/r','transport':'fs'})
        self.assertEqual(result,{**expected,'remote':'/r','transport':'fs'})
        digest.assert_called_once_with(self.device,'/r')

    def test_explicit_unavailable_ftp_does_not_touch_fs(self):
        self.ftp.probe.return_value = {'available': False, 'message': 'FTPD未运行'}
        with self.assertRaisesRegex(RuntimeError, 'FTPD'):
            self.files.stat({'transport': 'ftp', 'path': '/r'})
        self.device.call_one.assert_not_called()

    def test_legacy_fs_listing_paginates_without_claiming_completeness(self):
        self.device.call_one.return_value = {'lines': [
            'name=a type=file size=1', 'name=b type=file size=2', 'count=2 truncated=1']}
        result = self.files.list({'transport': 'fs', 'path': '/', 'offset': 1, 'count': 1})
        self.assertEqual([e['name'] for e in result['entries']], ['b'])
        self.assertEqual(result['next_offset'], -1)
        self.assertTrue(result['truncated'])
        self.assertTrue(result['legacy'])


class FtpConfigTests(unittest.TestCase):
    def test_numeric_timeout_has_canonical_fingerprint(self):
        from nxc_host.config import Config, fingerprint
        self.assertEqual(fingerprint(Config(timeout=2)), fingerprint(Config(timeout=2.0)))

    def test_old_background_can_be_stopped_only_with_matching_legacy_target(self):
        import hashlib
        from nxc_host.config import Config, config_dir
        from nxc_host.client import HostClient
        config = Config()
        legacy = {k: v for k, v in config.public_dict().items() if not k.startswith('ftp_')}
        status = {'config': legacy, 'config_dir': str(config_dir()), 'config_fingerprint':
                  hashlib.sha256(json.dumps(legacy,sort_keys=True,separators=(',',':')).encode()).hexdigest()}
        client = HostClient(config)
        with self.assertRaises(RuntimeError):
            client._matching(status)
        client._matching(status, allow_legacy=True)
        legacy['timeout']=int(legacy['timeout'])
        status['config_fingerprint']=hashlib.sha256(json.dumps(legacy,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        client._matching(status, allow_legacy=True)
        status['config'] = {**legacy, 'device_ip': 'another-target'}
        with self.assertRaises(RuntimeError):
            client._matching(status, allow_legacy=True)

    def test_password_persisted_but_not_in_public_status(self):
        from nxc_host.config import Config, save_config, load_config
        from nxc_host.runtime import HostRuntime
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {'NXC_CONFIG_DIR': directory}):
            config = Config(ftp_port=1234, ftp_user='qa', ftp_password='private-test')
            save_config(config)
            self.assertEqual(load_config().ftp_password, 'private-test')
            runtime = HostRuntime(config)
            try:
                self.assertNotIn('private-test', json.dumps(runtime.status()))
                self.assertNotIn('ftp_password', runtime.status()['config'])
                self.assertEqual(runtime.status()['config']['ftp_port'], 1234)
            finally:
                runtime.close()

    def test_new_ftp_fields_validate_like_device_target(self):
        from nxc_host.config import Config
        for fields in ({'ftp_port': 0}, {'ftp_port': True}, {'ftp_user': 'a\r\nPASS secret'},
                       {'ftp_password': 'a\nNOOP'}):
            with self.subTest(fields=list(fields)), self.assertRaises(ValueError):
                Config(**fields)
