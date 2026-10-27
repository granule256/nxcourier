"""固定 sys-ftpd 包解析与缓存回归；不连接网络或设备。"""
import hashlib
import importlib
import io
from pathlib import Path, PureWindowsPath
import ssl
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/src'))
MODULE = 'atmosphere/contents/420000000000000E/exefs.nsp'
FLAG = 'atmosphere/contents/420000000000000E/flags/boot2.flag'
TOOLBOX = 'atmosphere/contents/420000000000000E/toolbox.json'
CONFIG = 'config/sys-ftpd/config.ini'
FILES = {MODULE: b'module-fixture', FLAG: b'', TOOLBOX: b'{"name":"sys-ftpd"}',
         CONFIG: b'[Network]\nport=5000\n'}


def archive(files=None, *, compression=zipfile.ZIP_DEFLATED, extras=()):
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', compression=compression) as handle:
        for path, content in (FILES if files is None else files).items():
            handle.writestr(path, content)
        for path, content in extras:
            handle.writestr(path, content)
    return output.getvalue()


class FtpPackageTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue((ROOT / 'pc/src/nxc_host/services/ftp_package.py').is_file(),
                        '固定 FTP 安装包来源尚未实现')
        self.module = importlib.import_module('nxc_host.services.ftp_package')
        self.data = archive()
        self.catalog = dict(self.module.CATALOG,
                            sha256=hashlib.sha256(self.data).hexdigest(),
                            module_sha256=hashlib.sha256(FILES[MODULE]).hexdigest(),
                            files_sha256={path: hashlib.sha256(data).hexdigest()
                                          for path, data in FILES.items()})
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache = Path(self.temp.name) / 'cache'

    def load(self, data=None, **kwargs):
        return self.module.load_package(self.data if data is None else data,
                                        catalog=self.catalog, **kwargs)

    def test_catalog_is_pinned_to_verified_canonical_release(self):
        catalog = self.module.CATALOG
        self.assertEqual(catalog['version'], '1.0.5')
        self.assertEqual(catalog['url'], 'https://github.com/cathery/sys-ftpd/releases/download/1.0.5/sys-ftpd-1.0.5.zip')
        self.assertEqual(catalog['release_url'], 'https://github.com/cathery/sys-ftpd/releases/tag/1.0.5')
        self.assertEqual(catalog['sha256'], '004e157224f105902b0952e9bdcb0872f5e23c1962985a4bc14aa62f1921febc')
        self.assertEqual(catalog['module_sha256'], '9ce6611814892442ecf01a6a7245dd9dbdd0a368e93a9036700be90e0e1412c4')
        self.assertEqual(catalog['files_sha256'][CONFIG], 'bd9ea171e86d40340fa522f0e93efdf75a0809c4854d283f2aa1cd480d59c6f7')
        self.assertEqual(catalog['files_sha256'][TOOLBOX], '539c4ae164c3deebc3d8ef474731e0a757e3f6bbf89f491e686ad15a86d79fd1')

    def test_real_official_fixture_when_present(self):
        fixture = ROOT / 'tmp/ftp-setup-work/sys-ftpd-1.0.5.zip'
        if not fixture.exists():
            self.skipTest('可选官方原包未放入工作目录，离线合成包仍完整覆盖解析')
        package = self.module.load_package(fixture.read_bytes(), source='official')
        self.assertEqual(package['sha256'], self.module.CATALOG['sha256'])
        self.assertEqual(set(package['files']), {'/' + path for path in FILES})
        self.assertEqual(len(package['files']['/' + MODULE]), 173082)

    def test_four_files_map_to_absolute_device_paths(self):
        package = self.load()
        self.assertEqual(package['files'], {'/' + path: data for path, data in FILES.items()})
        self.assertEqual(package['source'], 'local')
        self.assertEqual(package['version'], '1.0.5')
        self.assertEqual(package['sha256'], hashlib.sha256(self.data).hexdigest())
        self.assertTrue(package['package_id'])
        self.assertEqual(package['package_id'], self.load()['package_id'])

    def test_zip_directories_use_posix_paths_on_windows_hosts_too(self):
        data = archive(extras=[('atmosphere/', b''), ('atmosphere/contents/', b'')])
        with patch.object(self.module, 'Path', PureWindowsPath):
            try:
                package = self.load(data)
            except RuntimeError as error:
                self.fail('合法 ZIP 目录不应随主机系统改变：' + str(error))
        self.assertEqual(package['files'], {'/' + path: data for path, data in FILES.items()})

    def test_local_repack_uses_content_hashes_official_requires_archive_hash(self):
        repacked = archive(compression=zipfile.ZIP_STORED)
        package = self.load(repacked)
        self.assertNotEqual(package['sha256'], self.catalog['sha256'])
        self.assertEqual(package['source'], 'local')
        with self.assertRaisesRegex(RuntimeError, '摘要'):
            self.load(repacked, source='official')

    def test_wrong_content_for_each_of_four_files_is_rejected(self):
        for path in FILES:
            with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, '摘要'):
                self.load(archive(dict(FILES, **{path: b'changed'})))

    def test_missing_file_and_unknown_file_rejected(self):
        for path in FILES:
            files = dict(FILES)
            del files[path]
            with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, '缺少'):
                self.load(archive(files))
        with self.assertRaisesRegex(RuntimeError, '文件'):
            self.load(archive(extras=[('unknown', b'x')]))

    def test_unsafe_duplicate_and_noncanonical_paths_rejected(self):
        for path in ('../escape', '/absolute', 'C:/absolute', 'a/../escape',
                     'a\\escape', './' + MODULE, 'a//b'):
            with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, '路径'):
                self.load(archive(extras=[(path, b'x')]))
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            duplicate = archive(extras=[(MODULE, FILES[MODULE])])
        with self.assertRaisesRegex(RuntimeError, '重复'):
            self.load(duplicate)

    def test_symlink_rejected_even_for_expected_file(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w') as handle:
            for path, data in FILES.items():
                info = zipfile.ZipInfo(path)
                info.create_system = 3
                info.external_attr = ((stat.S_IFLNK | 0o777) if path == MODULE
                                      else (stat.S_IFREG | 0o644)) << 16
                handle.writestr(info, data)
        with self.assertRaisesRegex(RuntimeError, '符号链接|类型'):
            self.load(output.getvalue())

    def test_compressed_and_uncompressed_limits(self):
        with self.assertRaisesRegex(RuntimeError, '上限'):
            self.load(b'x' * (4 * 1024 * 1024 + 1))
        expanded = archive({MODULE: b'x' * (8 * 1024 * 1024 + 1)})
        self.assertLess(len(expanded), 4 * 1024 * 1024)
        with self.assertRaisesRegex(RuntimeError, '上限'):
            self.load(expanded)

    def test_malformed_and_unknown_source_rejected(self):
        for data in (b'', b'not a zip'):
            with self.subTest(data=data), self.assertRaises(RuntimeError):
                self.load(data)
        with self.assertRaisesRegex(RuntimeError, '来源'):
            self.load(source='unverified')

    def test_official_download_atomically_cached_and_revalidated(self):
        calls = []
        def download(url):
            calls.append(url)
            return self.data
        first = self.module.get_official_package(self.cache, catalog=self.catalog, download=download)
        self.assertEqual(first['source'], 'official')
        cached = list(self.cache.iterdir())
        self.assertEqual(len(cached), 1)
        self.assertEqual(cached[0].read_bytes(), self.data)
        self.assertEqual(self.module.get_official_package(self.cache, catalog=self.catalog,
                                                        download=download), first)
        self.assertEqual(calls, [self.catalog['url']])
        cached[0].write_bytes(b'partial')
        self.module.get_official_package(self.cache, catalog=self.catalog, download=download)
        self.assertEqual(len(calls), 2)
        self.assertEqual(cached[0].read_bytes(), self.data)

    def test_failed_download_or_validation_never_creates_valid_cache(self):
        def broken(url):
            raise OSError('offline')
        for download in (broken, lambda url: b'partial',
                         lambda url: archive(dict(FILES, **{MODULE: b'bad'}))):
            with self.subTest(download=download), self.assertRaises(RuntimeError):
                self.module.get_official_package(self.cache, catalog=self.catalog, download=download)
            self.assertFalse(self.cache.exists() and list(self.cache.iterdir()))

    def test_cache_replace_failure_removes_temporary_file(self):
        with patch.object(self.module.os, 'replace', side_effect=OSError('disk failure')):
            with self.assertRaises(RuntimeError):
                self.module.get_official_package(self.cache, catalog=self.catalog,
                                                 download=lambda url: self.data)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_tls_keeps_verification_and_loads_existing_system_ca(self):
        context = ssl.create_default_context()
        with patch.object(self.module.ssl, 'create_default_context', return_value=context), \
             patch.object(self.module.Path, 'is_file', return_value=True), \
             patch.object(context, 'load_verify_locations') as load:
            result = self.module._tls_context()
        self.assertIs(result, context)
        self.assertTrue(result.check_hostname)
        self.assertEqual(result.verify_mode, ssl.CERT_REQUIRED)
        self.assertIn('/etc/ssl/cert.pem', [call.kwargs['cafile'] for call in load.call_args_list])

    def test_downloader_bounds_response_and_uses_verified_https(self):
        response = io.BytesIO(b'x' * (4 * 1024 * 1024 + 1))
        response.headers = {}
        with patch.object(self.module.urllib.request, 'build_opener') as build:
            build.return_value.open.return_value = response
            with self.assertRaisesRegex(RuntimeError, '上限'):
                self.module._download(self.catalog['url'])
            handlers = build.call_args.args
            https = next(h for h in handlers if isinstance(h, self.module.urllib.request.HTTPSHandler))
            self.assertTrue(https._context.check_hostname)
            self.assertEqual(https._context.verify_mode, ssl.CERT_REQUIRED)
            self.assertGreater(build.return_value.open.call_args.kwargs['timeout'], 0)
        with self.assertRaisesRegex(RuntimeError, 'HTTPS'):
            self.module._download('http://example.invalid/package.zip')

    def test_https_redirect_cannot_downgrade(self):
        handler = self.module._HTTPSOnlyRedirect()
        request = self.module.urllib.request.Request('https://example.invalid/package.zip')
        with self.assertRaisesRegex(RuntimeError, 'HTTPS'):
            handler.redirect_request(request, None, 302, 'found', {}, 'http://example.invalid/file')


if __name__ == '__main__':
    unittest.main()
