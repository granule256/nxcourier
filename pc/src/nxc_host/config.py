"""后台配置及状态目录；不在导入时创建文件或连接设备。"""
from __future__ import annotations
from dataclasses import asdict, dataclass, fields
import json
import math
import os
from pathlib import Path


def config_dir() -> Path:
    return Path(os.environ.get('NXC_CONFIG_DIR', '~/.config/nxc')).expanduser().resolve()


@dataclass(frozen=True)
class Config:
    device_ip: str = '192.168.1.50'
    device_port: int = 47800
    timeout: float = 15.0
    host_port: int = 47801
    gdb_backend: str = 'auto'
    gdb_executable: str | None = None
    gdb_image: str = 'devkitpro/devkita64:latest'
    ftp_port: int = 5000
    ftp_user: str = 'anonymous'
    ftp_password: str = ''

    def __post_init__(self):
        if not isinstance(self.device_ip,str) or not self.device_ip.strip() or any(c.isspace() for c in self.device_ip):
            raise ValueError('设备地址不能为空或含空白')
        for name in ('device_port','host_port','ftp_port'):
            value=getattr(self,name)
            if isinstance(value,bool) or not isinstance(value,int) or not 1<=value<=65535:
                raise ValueError(f'{name} 必须为1..65535')
        if isinstance(self.timeout,bool) or not isinstance(self.timeout,(int,float)) or not math.isfinite(self.timeout) or not 0<self.timeout<=3600:
            raise ValueError('超时必须为0..3600秒内的正数')
        object.__setattr__(self,'timeout',float(self.timeout))
        if self.gdb_backend not in ('auto','native','docker'):
            raise ValueError('GDB后端必须为auto/native/docker')
        for name in ('gdb_executable','gdb_image'):
            value=getattr(self,name)
            if value is not None and (not isinstance(value,str) or '\x00' in value):
                raise ValueError(f'{name} 格式错误')
        for name in ('ftp_user', 'ftp_password'):
            value = getattr(self, name)
            if not isinstance(value, str) or any(c in value for c in '\r\n\x00'):
                raise ValueError(f'{name} 格式错误')

    def to_dict(self) -> dict:
        return asdict(self)

    def public_dict(self) -> dict:
        values = self.to_dict()
        values.pop('ftp_password')
        return values


def load_config(overrides: dict | None = None) -> Config:
    allowed={f.name for f in fields(Config)}
    path=config_dir()/'config.json'
    values={}
    if path.exists():
        data=json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(data,dict):raise ValueError('config.json 必须为对象')
        unknown=set(data)-allowed
        if unknown:raise ValueError('未知配置项：'+', '.join(sorted(unknown)))
        values.update(data)
    env={'device_ip':'NXC_DEVICE_IP','device_port':'NXC_DEVICE_PORT','timeout':'NXC_TIMEOUT',
         'host_port':'NXC_HOST_PORT','gdb_backend':'NXC_GDB_BACKEND','gdb_executable':'NXC_GDB_PATH','gdb_image':'NXC_GDB_IMAGE',
         'ftp_port':'NXC_FTP_PORT','ftp_user':'NXC_FTP_USER','ftp_password':'NXC_FTP_PASSWORD'}
    for key,name in env.items():
        if name not in os.environ:continue
        value=os.environ[name]
        if key in ('device_port','host_port','ftp_port'):value=int(value)
        elif key=='timeout':value=float(value)
        elif key=='gdb_executable':value=value or None
        values[key]=value
    if overrides:
        unknown=set(overrides)-allowed
        if unknown:raise ValueError('未知配置项：'+', '.join(sorted(unknown)))
        values.update({k:v for k,v in overrides.items() if v is not None})
    return Config(**values)


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True,exist_ok=True)
    import tempfile
    with tempfile.NamedTemporaryFile('w',encoding='utf-8',dir=path.parent,prefix='.nxc-',delete=False) as stream:
        temporary=Path(stream.name)
        try:
            json.dump(data,stream,ensure_ascii=False,indent=2);stream.write('\n')
        except BaseException:
            temporary.unlink(missing_ok=True);raise
    try:os.replace(temporary,path)
    finally:temporary.unlink(missing_ok=True)


def save_config(config: Config) -> None:
    write_json(config_dir()/'config.json',config.to_dict())


def fingerprint(config: Config) -> str:
    import hashlib
    return hashlib.sha256(json.dumps(config.to_dict(),sort_keys=True,separators=(',',':')).encode()).hexdigest()
