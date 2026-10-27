"""固定官方 sys-ftpd 安装包来源；只在 PC 解析，不解压到文件系统。"""
from __future__ import annotations

import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import ssl
import stat
import tempfile
import urllib.parse
import urllib.request
import zipfile


MAX_ARCHIVE_BYTES = 4 * 1024 * 1024
MAX_UNPACKED_BYTES = 8 * 1024 * 1024
MODULE_PATH = 'atmosphere/contents/420000000000000E/exefs.nsp'
FLAG_PATH = 'atmosphere/contents/420000000000000E/flags/boot2.flag'
TOOLBOX_PATH = 'atmosphere/contents/420000000000000E/toolbox.json'
CONFIG_PATH = 'config/sys-ftpd/config.ini'

# 来源：根目录 README.md 及官方 1.0.5 ZIP。产品不携带 ZIP；本地重包仍须逐项一致。
CATALOG = {
    'version': '1.0.5',
    'url': 'https://github.com/cathery/sys-ftpd/releases/download/1.0.5/sys-ftpd-1.0.5.zip',
    'release_url': 'https://github.com/cathery/sys-ftpd/releases/tag/1.0.5',
    'size': 130642,
    'sha256': '004e157224f105902b0952e9bdcb0872f5e23c1962985a4bc14aa62f1921febc',
    'module_sha256': '9ce6611814892442ecf01a6a7245dd9dbdd0a368e93a9036700be90e0e1412c4',
    'files_sha256': {
        MODULE_PATH: '9ce6611814892442ecf01a6a7245dd9dbdd0a368e93a9036700be90e0e1412c4',
        FLAG_PATH: 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
        TOOLBOX_PATH: '539c4ae164c3deebc3d8ef474731e0a757e3f6bbf89f491e686ad15a86d79fd1',
        CONFIG_PATH: 'bd9ea171e86d40340fa522f0e93efdf75a0809c4854d283f2aa1cd480d59c6f7',
    },
}


def load_package(data: bytes, *, catalog: dict | None = None,
                 source: str = 'local') -> dict:
    """核验四文件并返回绝对设备路径。official 还必须匹配固定 ZIP 摘要。

    sha256 始终是输入 ZIP 的真实摘要，不以官方摘要代替本地重包摘要。
    catalog 仅供离线测试注入；产品调用省略此参数，使用固定 CATALOG。
    """
    catalog = CATALOG if catalog is None else catalog
    if source not in ('local', 'official'):
        raise RuntimeError('FTP 安装包来源无效')
    if not isinstance(data, bytes):
        raise RuntimeError('FTP 安装包必须为 ZIP 字节数据')
    if len(data) > MAX_ARCHIVE_BYTES:
        raise RuntimeError('FTP 安装包超过 4MiB 上限')
    digest = hashlib.sha256(data).hexdigest()
    if source == 'official' and digest != catalog['sha256']:
        raise RuntimeError('官方 FTP 安装包 ZIP 摘要不匹配')
    expected = catalog['files_sha256']
    directories = {str(parent) + '/' for path in expected
                   for parent in PurePosixPath(path).parents if str(parent) != '.'}
    files = {}
    seen = set()
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            infos = archive.infolist()
            if sum(info.file_size for info in infos) > MAX_UNPACKED_BYTES:
                raise RuntimeError('FTP 安装包解压总量超过 8MiB 上限')
            if len(infos) > 64:
                raise RuntimeError('FTP 安装包文件数量超过上限')
            for info in infos:
                name = info.filename
                parts = name.rstrip('/').split('/')
                if (info.orig_filename != name or not name or name.startswith('/')
                        or '\\' in name or ':' in name
                        or any(part in ('', '.', '..') for part in parts)
                        or any(ord(char) < 32 for char in name)):
                    raise RuntimeError('FTP 安装包包含非法路径')
                if name in seen:
                    raise RuntimeError('FTP 安装包包含重复路径')
                seen.add(name)
                mode = stat.S_IFMT(info.external_attr >> 16)
                if mode == stat.S_IFLNK:
                    raise RuntimeError('FTP 安装包不允许符号链接')
                if mode not in (0, stat.S_IFDIR if info.is_dir() else stat.S_IFREG):
                    raise RuntimeError('FTP 安装包包含不支持的文件类型')
                if info.is_dir():
                    if name not in directories or info.file_size:
                        raise RuntimeError('FTP 安装包包含未知目录路径')
                    continue
                if name not in expected:
                    raise RuntimeError('FTP 安装包包含未知文件')
                if info.flag_bits & 1:
                    raise RuntimeError('FTP 安装包不允许加密文件')
                if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    raise RuntimeError('FTP 安装包压缩格式不支持')
                with archive.open(info) as entry:
                    content = entry.read(MAX_UNPACKED_BYTES + 1)
                if len(content) > MAX_UNPACKED_BYTES:
                    raise RuntimeError('FTP 安装包解压总量超过 8MiB 上限')
                if hashlib.sha256(content).hexdigest() != expected[name]:
                    raise RuntimeError('FTP 安装包文件摘要不匹配：' + name)
                files['/' + name] = content
    except (zipfile.BadZipFile, zipfile.LargeZipFile, EOFError, NotImplementedError) as error:
        raise RuntimeError('FTP 安装包 ZIP 损坏或格式无效') from error
    if set(files) != {'/' + name for name in expected}:
        raise RuntimeError('FTP 安装包缺少固定版本所需的四个文件')
    return {
        'package_id': 'sys-ftpd-' + catalog['version'] + '-' + digest,
        'version': catalog['version'], 'sha256': digest, 'source': source,
        'files': files,
    }


def _tls_context() -> ssl.SSLContext:
    """保留平台信任与证书验证，并补充已存在的常见系统 CA 包。"""
    context = ssl.create_default_context()
    for candidate in ('/etc/ssl/cert.pem', '/etc/ssl/certs/ca-certificates.crt',
                      '/etc/pki/tls/certs/ca-bundle.crt'):
        if Path(candidate).is_file():
            try:
                context.load_verify_locations(cafile=candidate)
            except (OSError, ssl.SSLError):
                # 某个系统路径不可读时保留默认 CA，不降低验证级别。
                continue
    return context


class _HTTPSOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme.lower() != 'https':
            raise RuntimeError('FTP 安装包仅允许 HTTPS 下载与重定向')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _download(url: str) -> bytes:
    if urllib.parse.urlsplit(url).scheme.lower() != 'https':
        raise RuntimeError('FTP 安装包仅允许 HTTPS 下载')
    opener = urllib.request.build_opener(
        urllib.request.HTTPSHandler(context=_tls_context()), _HTTPSOnlyRedirect())
    request = urllib.request.Request(url, headers={'User-Agent': 'NXC FTP setup'})
    with opener.open(request, timeout=30) as response:
        length = response.headers.get('Content-Length')
        if length and int(length) > MAX_ARCHIVE_BYTES:
            raise RuntimeError('FTP 安装包下载超过 4MiB 上限')
        data = response.read(MAX_ARCHIVE_BYTES + 1)
        if len(data) > MAX_ARCHIVE_BYTES:
            raise RuntimeError('FTP 安装包下载超过 4MiB 上限')
    return data


def get_official_package(cache_dir: Path, *, catalog: dict | None = None,
                         download=None) -> dict:
    """复核缓存或有界下载；校验成功后以同目录临时文件原子替换缓存。"""
    catalog = CATALOG if catalog is None else catalog
    download = _download if download is None else download
    cache_dir = Path(cache_dir)
    cached = cache_dir / ('sys-ftpd-' + catalog['version'] + '.zip')
    if cached.is_file():
        try:
            with cached.open('rb') as handle:
                return load_package(handle.read(MAX_ARCHIVE_BYTES + 1),
                                    catalog=catalog, source='official')
        except (OSError, RuntimeError):
            pass  # 缓存不完整或已被更改时，重新准备并完整校验。
    temporary = None
    try:
        data = download(catalog['url'])
        package = load_package(data, catalog=catalog, source='official')
        cache_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(prefix='.sys-ftpd-', suffix='.tmp',
                                         dir=cache_dir, delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, cached)
        return package
    except Exception as error:
        raise RuntimeError('官方 FTP 安装包准备失败：' + str(error)) from error
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
