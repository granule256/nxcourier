"""后台唯一拥有的设备连接、GDB会话和工具调度。"""
from __future__ import annotations
from dataclasses import replace
import base64
import hashlib
import os
from pathlib import Path, PurePosixPath
import tempfile
import threading
import time
import uuid
from . import __version__, tools
from .config import Config, config_dir, fingerprint
from .device import Device, parse_payload
from .services.gdb import GdbSession
from .services.ftp import FtpClient
from .services.files import FileTransfers

MAX_FILE_BYTES=8*1024*1024

class TargetChanged(RuntimeError):
    pass


class HostRuntime:
    def __init__(self,config: Config,*,device=None,gdb=None,ftp=None):
        self.config=config
        self.instance_id=uuid.uuid4().hex
        self.device=device if device is not None else Device(config.device_ip,config.device_port,config.timeout)
        self.gdb=gdb if gdb is not None else GdbSession(config.device_ip,backend=config.gdb_backend,
                    executable=config.gdb_executable,image=config.gdb_image)
        self.ftp=ftp if ftp is not None else self._new_ftp(config)
        self.files=FileTransfers(self.device,self.ftp)
        self._setup=None
        self.started_at=time.time()
        self._device_lock=threading.RLock()
        self._target_lock=threading.RLock()
        self._closed=False
        self._lifecycle_lock=threading.RLock()
        self.stop_callback=None

    def status(self) -> dict:
        return {'service':'nxc-host','version':__version__,'instance_id':self.instance_id,'config_fingerprint':fingerprint(self.config),'pid':os.getpid(),'started_at':self.started_at,
                'running':not self._closed,'url':f'http://127.0.0.1:{self.config.host_port}',
                'config':self.config.public_dict(),
                'device':{'ip':self.config.device_ip,'port':self.config.device_port,
                          'connected':getattr(self.device,'_sock',None) is not None},
                'gdb':self.gdb.status()}

    @staticmethod
    def _new_ftp(config):
        return FtpClient(config.device_ip,port=config.ftp_port,timeout=config.timeout,
                         user=config.ftp_user,password=config.ftp_password)

    def _setup_service(self):
        with self._target_lock:
            if self._setup is None:
                from .services.ftp_setup import FtpSetup
                self._setup=FtpSetup(self.device,self.ftp,self.config,config_dir()/'cache')
            return self._setup

    def _gdb(self,action,args):
        args=dict(args)
        if action=='attach' and args.get('pid') is None:
            current=self.device.call_one('title.current',retry_safe=True)
            if 'error' in current:raise RuntimeError(f"无法查询当前应用：{current['error']}")
            rows=parse_payload(current.get('lines',[]))
            row=next((r for r in rows if isinstance(r,dict) and r.get('running')=='application'),None)
            if row is None or int(row.get('pid',0))<=0:raise RuntimeError('当前没有可附加的应用')
            args['pid']=int(row['pid'])
        if action=='attach':
            status=self.gdb.status()
            if not status.get('running'):self.gdb.handle('open',{'connect':True})
            elif not status.get('connected'):self.gdb.handle('connect',{})
        return self.gdb.handle(action,args)

    def _check_context(self,context):
        if self._closed:raise RuntimeError('后台正在退出，未执行排队请求')
        if context is not None and (context.get('instance_id')!=self.instance_id or context.get('config_fingerprint')!=fingerprint(self.config)):
            raise TargetChanged('目标配置或后台实例已变化，操作未执行；请刷新概览确认新目标，CLI请重新读取配置')

    def _tool_request(self,request,context=None):
        params=request.get('params') or {}
        name=params.get('name');args=params.get('arguments') or {}
        if not isinstance(args,dict):raise ValueError('arguments 必须为对象')
        if name=='nxc_gdb' and args.get('action') in ('status','doctor','interrupt','pause'):
            # 中断不能等在正在执行的GDB命令/设备调用之后。
            with self._target_lock:
                self._check_context(context)
                selected_gdb=self.gdb
            payload=selected_gdb.handle(str(args.get('action')),args)
            return tools.rpc_result(request.get('id'),tools.text_content(__import__('json').dumps(payload,ensure_ascii=False)))
        with self._device_lock:
            self._check_context(context)
            if name in ('nxc_put','nxc_fetch','nxc_deploy'):
                try:
                    payload=self.files.fetch(args) if name=='nxc_fetch' else self.files.put(args,deploy=name=='nxc_deploy')
                    content=tools.text_content(__import__('json').dumps(payload,ensure_ascii=False))
                except Exception as exc:
                    content={'isError':True,'content':[{'type':'text','text':f'文件传输失败：{exc}'}]}
                return tools.rpc_result(request.get('id'),content)
            if name=='nxc_call':
                effective=str(args['raw']).strip() if args.get('raw') else str(args.get('method') or '')
                method=effective.split()[0] if effective.split() else ''
                gdb_status=self.gdb.status()
                if method in ('power.reboot','power.shutdown') and any(gdb_status.get(key) for key in ('running','connected','uncertain')):
                    return tools.rpc_result(request.get('id'),{'isError':True,'content':[{'type':'text','text':'GDB会话仍在运行或占用调试资源，请先关闭后再重启或关机。'}]})
                if method.startswith('mem.') and (gdb_status.get('connected') or gdb_status.get('uncertain')):
                    return tools.rpc_result(request.get('id'),{'isError':True,'content':[{'type':'text','text':'GDB会话已连接，请先detach/close后使用独立mem读写，避免调试资源争用。'}]})
            return tools.handle(request,self.device,gdb_handler=self._gdb)

    def _upload(self,args):
        remote=args.get('remote_path');data=args.get('data')
        if not isinstance(remote,str) or not remote.startswith('/'):raise ValueError('remote_path 必须为设备绝对路径')
        if not isinstance(data,str) or len(data)>MAX_FILE_BYTES*4//3+8:raise ValueError('上传内容超过8MiB或格式错误')
        raw=base64.b64decode(data,validate=True)
        if len(raw)>MAX_FILE_BYTES:raise ValueError('Web上传最多8MiB，请使用nxc put传较大文件')
        with tempfile.TemporaryDirectory(prefix='nxc-upload-') as directory:
            local=Path(directory)/'upload';local.write_bytes(raw)
            return self.files.put({'local_path':str(local),'remote_path':remote,'transport':args.get('transport','auto')})

    def _download(self,args):
        remote=args.get('remote_path')
        if not isinstance(remote,str) or not remote.startswith('/'):raise ValueError('remote_path 必须为设备绝对路径')
        data,channel=self.files.read(args,MAX_FILE_BYTES)
        return {'name':PurePosixPath(remote).name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
                'data':base64.b64encode(data).decode(),'transport':channel}

    def _boot(self,args):
        # 轮询由CLI在请求之间等待，不长时间占住后台设备调度锁。
        out=self.device.call_one('identify.id',retry_safe=True)
        if 'error' in out:raise RuntimeError(f"设备未就绪：{out['error']}")
        records=parse_payload(out.get('lines',[]))
        boot=next((int(row['boot']) for row in records if isinstance(row,dict) and 'boot' in row),None)
        return {'records':records,'boot':boot,'ready':True}

    def _configure(self,args,context=None):
        from .config import save_config
        unknown=set(args)-{'device_ip','device_port','timeout','ftp_port','ftp_user','ftp_password'}
        if unknown:raise ValueError('这里只允许更改设备地址、端口、超时和FTP设置')
        with self._device_lock:
            with self._target_lock:
                self._check_context(context)
                owner=self.gdb.status()
                if owner.get('running') or owner.get('connected') or owner.get('uncertain'):
                    raise RuntimeError('请先关闭GDB会话并确认资源已释放，再更换目标设备')
                updated=replace(self.config,**args)
                new_device=Device(updated.device_ip,updated.device_port,updated.timeout)
                new_gdb=GdbSession(updated.device_ip,backend=updated.gdb_backend,
                                  executable=updated.gdb_executable,image=updated.gdb_image)
                new_ftp=self._new_ftp(updated)
                save_config(updated)
                self.device.close();self.gdb.close();self.ftp.close()
                self.config=updated;self.device=new_device;self.gdb=new_gdb;self.ftp=new_ftp
                self.files=FileTransfers(new_device,new_ftp)
                self._setup=None
                return self.status()

    def handle(self,request:dict,*,context=None):
        if not isinstance(request,dict):return tools.rpc_error(None,-32600,'请求必须为JSON对象')
        req_id=request.get('id');method=request.get('method');args=request.get('params',{})
        if args is None:args={}
        if request.get('jsonrpc')!='2.0' or not isinstance(method,str) or not method or not isinstance(args,dict) or isinstance(req_id,(dict,list,bool)):return tools.rpc_error(req_id,-32600,'method/params格式错误')
        try:
            if self._closed:raise RuntimeError('后台正在退出')
            if method=='host.status':result=self.status()
            elif method=='host.ftp.setup.progress':
                with self._target_lock:
                    self._check_context(context)
                    setup=self._setup_service()
                result=setup.progress()
            elif method in ('host.ftp.setup.status','host.ftp.setup.prepare','host.ftp.setup.install','host.ftp.setup.configure'):
                with self._device_lock:
                    self._check_context(context)
                    setup=self._setup_service()
                    if method.endswith('.status'):result=setup.status()
                    elif method.endswith('.prepare'):result=setup.prepare(args)
                    elif method.endswith('.configure'):result=setup.configure_anonymous()
                    else:
                        anonymous=args.get('anonymous',True)
                        if type(anonymous) is not bool:raise ValueError('anonymous 必须是布尔值')
                        package_id=args.get('package_id')
                        if not isinstance(package_id,str) or not package_id:raise ValueError('请先准备并校验安装包')
                        result=setup.install(package_id,anonymous=anonymous)
            elif method=='host.configure':result=self._configure(args,context)
            elif method=='host.doctor':
                result={'service':self.status(),'gdb':self.gdb.doctor(),
                        'python':{'version':__import__('sys').version.split()[0]},'config_dir':str(config_dir())}
            elif method=='host.stop':
                self._check_context(context)
                result={'stopping':True,'pid':os.getpid()}
                if self.stop_callback:self.stop_callback()
            elif method=='host.logs':
                lines=max(1,min(int(args.get('lines',80)),300));path=config_dir()/'service.log'
                if path.exists():
                    with path.open('rb') as stream:
                        stream.seek(0,2);stream.seek(max(0,stream.tell()-1024*1024));text=stream.read().decode('utf-8','replace')
                    result={'lines':text.splitlines()[-lines:]}
                else:result={'lines':[]}
            elif method in ('host.upload','host.download','host.sha','host.boot','host.ftp.status','host.files.list','host.files.stat'):
                with self._device_lock:
                    self._check_context(context)
                    if method=='host.upload':result=self._upload(args)
                    elif method=='host.download':result=self._download(args)
                    elif method=='host.boot':result=self._boot(args)
                    elif method=='host.ftp.status':result=self.ftp.probe()
                    elif method=='host.files.list':result=self.files.list(args)
                    elif method=='host.files.stat':result=self.files.stat(args)
                    else:result=self.files.sha(args)
            elif method=='tools/call':
                response=self._tool_request(request,context)
                return None if req_id is None else response
            else:
                response=tools.handle(request,self.device,gdb_handler=self._gdb)
                return None if req_id is None else response
            return None if req_id is None else tools.rpc_result(req_id,result)
        except TargetChanged as exc:
            return None if req_id is None else tools.rpc_error(req_id,-32009,str(exc))
        except Exception as exc:
            return None if req_id is None else tools.rpc_error(req_id,-32000,f'{type(exc).__name__}: {exc}')

    def close(self):
        with self._lifecycle_lock:
            if self._closed:return
            self._closed=True
            try:self.gdb.close()
            finally:
                # 先中断GDB以释放活动命令，再等串行调用退出；补收已通过检查但刚启动的GDB。
                with self._device_lock:
                    try:self.gdb.close()
                    finally:
                        try:self.ftp.close()
                        finally:self.device.close()
