"""统一后台客户端；只在执行前探测并启动，发送后的请求绝不自动重放。"""
from __future__ import annotations
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from .config import Config,config_dir,save_config


class HostClient:
    def __init__(self,config:Config):
        self.config=config
        self.url=f'http://127.0.0.1:{config.host_port}'
        self._opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def _http(self,path,body=None,timeout=3,context=None):
        data=None if body is None else json.dumps(body,ensure_ascii=False,allow_nan=False).encode()
        headers={'Content-Type':'application/json'}
        if context:
            headers.update({'X-NXC-Instance':context['instance_id'],'X-NXC-Config':context['config_fingerprint']})
        request=urllib.request.Request(self.url+path,data=data,headers=headers)
        with self._opener.open(request,timeout=timeout) as response:
            if response.status==204:return None
            raw=response.read(64*1024*1024+1)
            if len(raw)>64*1024*1024:raise RuntimeError('后台响应过大')
            return json.loads(raw)

    def status(self):
        try:data=self._http('/health')
        except urllib.error.HTTPError as exc:raise RuntimeError(f'端口{self.config.host_port}不是NXC后台（HTTP {exc.code}）') from exc
        except (urllib.error.URLError,TimeoutError,ConnectionError,OSError):
            return {'service':'nxc-host','running':False,'url':self.url}
        except (ValueError,UnicodeError) as exc:raise RuntimeError('本机端口响应无法识别，未执行任何操作') from exc
        if not isinstance(data,dict) or data.get('service')!='nxc-host':raise RuntimeError('本机端口由其他程序使用，拒绝操作')
        return data

    def _matching(self,status,*,allow_legacy=False):
        from .config import fingerprint
        expected=self.config.public_dict()
        compatible=status.get('config')==expected and status.get('config_fingerprint')==fingerprint(self.config)
        if not compatible and allow_legacy and self.config.ftp_port==5000 and self.config.ftp_user=='anonymous' and not self.config.ftp_password:
            # 仅允许停掉升级前、配置完全相同的旧后台；旧后台不能处理新传输请求。
            import hashlib
            legacy={k:v for k,v in expected.items() if not k.startswith('ftp_')}
            compatible=(status.get('config')==legacy and status.get('config_fingerprint')==
                        hashlib.sha256(json.dumps(status['config'],sort_keys=True,separators=(',',':')).encode()).hexdigest())
        if not compatible:
            raise RuntimeError('已有后台的设备或运行配置不同；请先 nxc stop，再以新配置 nxc start，未向错误目标发送请求')
        if status.get('config_dir')!=str(config_dir()):raise RuntimeError('该端口属于另一个NXC配置目录，请使用不同host-port')

    def _command(self):
        import nxc_host
        archive=getattr(nxc_host.__loader__,'archive',None)
        env=dict(os.environ)
        if archive:
            command=[sys.executable,str(Path(archive).resolve())]
        else:
            command=[sys.executable,'-m','nxc_host']
            source=str(Path(nxc_host.__file__).resolve().parent.parent)
            env['PYTHONPATH']=source+(os.pathsep+env['PYTHONPATH'] if env.get('PYTHONPATH') else '')
        env.update(NXC_CONFIG_DIR=str(config_dir()),NXC_DEVICE_IP=self.config.device_ip,
                   NXC_DEVICE_PORT=str(self.config.device_port),NXC_TIMEOUT=str(self.config.timeout),
                   NXC_HOST_PORT=str(self.config.host_port),NXC_GDB_BACKEND=self.config.gdb_backend,
                   NXC_GDB_PATH=self.config.gdb_executable or '',NXC_GDB_IMAGE=self.config.gdb_image,
                   NXC_FTP_PORT=str(self.config.ftp_port),NXC_FTP_USER=self.config.ftp_user,
                   NXC_FTP_PASSWORD=self.config.ftp_password)
        return command+['_serve'],env

    def start(self):
        current=self.status()
        if current.get('running'):
            self._matching(current);return current
        directory=config_dir();directory.mkdir(parents=True,exist_ok=True)
        lock=directory/f'start-{self.config.host_port}.lock'
        deadline=time.monotonic()+12
        acquired=False
        while time.monotonic()<deadline:
            try:
                fd=os.open(lock,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
                with os.fdopen(fd,'w') as stream:stream.write(str(os.getpid()))
                acquired=True;break
            except FileExistsError:
                try:
                    pid=int(lock.read_text());os.kill(pid,0)
                except ProcessLookupError:lock.unlink(missing_ok=True);continue
                except (ValueError,OSError):
                    try:
                        if time.time()-lock.stat().st_mtime>30:lock.unlink(missing_ok=True);continue
                    except OSError:continue
                current=self.status()
                if current.get('running'):self._matching(current);return current
                time.sleep(0.05)
        if not acquired:raise RuntimeError('另一个NXC启动操作尚未完成，请稍后查看status')
        process=None
        try:
            current=self.status()
            if current.get('running'):self._matching(current);return current
            save_config(self.config)
            command,env=self._command()
            with (directory/'service.log').open('ab') as logfile:
                process=subprocess.Popen(command,env=env,stdin=subprocess.DEVNULL,stdout=logfile,stderr=logfile,
                                         start_new_session=True)
            # 保留并回收本客户端创建的子进程，避免Popen提前析构及长驻客户端僵尸。
            threading.Thread(target=process.wait,name=f'nxc-reap-{process.pid}',daemon=True).start()
            while time.monotonic()<deadline:
                current=self.status()
                if current.get('running'):self._matching(current);return current
                if process.poll() is not None:raise RuntimeError(f'后台启动失败（退出码{process.returncode}），请查看{directory / "service.log"}')
                time.sleep(0.05)
            raise RuntimeError(f'等待后台启动超时，请查看{directory / "service.log"}')
        except BaseException:
            if process is not None and process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
            raise
        finally:lock.unlink(missing_ok=True)

    def request(self,request,autostart=True):
        current=self.start() if autostart else self.status()
        if not current.get('running'):raise RuntimeError('NXC后台未运行，请执行 nxc start')
        self._matching(current)
        try:return self._http('/rpc',request,timeout=max(300,self.config.timeout+30),context=current)
        except (OSError,ValueError,urllib.error.URLError) as exc:
            raise RuntimeError(f'后台请求中断，执行结果未知，未自动重放：{type(exc).__name__}') from exc

    def stop(self):
        current=self.status()
        if not current.get('running'):return {'running':False,'stopped':True,'url':self.url}
        self._matching(current,allow_legacy=True)
        instance=current['instance_id']
        response=self._http('/rpc',{'jsonrpc':'2.0','id':1,'method':'host.stop'},timeout=5,context=current)
        if response.get('error'):raise RuntimeError(str(response['error']))
        deadline=time.monotonic()+20;state=config_dir()/'service.json'
        while time.monotonic()<deadline:
            try:saved=json.loads(state.read_text())
            except (OSError,ValueError):saved={}
            if saved.get('instance_id')!=instance and not self.status().get('running'):
                return {'running':False,'stopped':True,'url':self.url}
            time.sleep(0.05)
        raise RuntimeError('后台仍在释放资源，请稍后检查status与service.log；未强杀其他进程')
