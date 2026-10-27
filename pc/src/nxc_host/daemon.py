"""单一回环HTTP后台：Web、CLI和MCP共用同一个HostRuntime。"""
from __future__ import annotations
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.resources import files
import json
import signal
import threading
import uuid
from .config import Config, config_dir, write_json
from .runtime import HostRuntime

MAX_BODY=16*1024*1024


class HostServer(ThreadingHTTPServer):
    daemon_threads=True
    allow_reuse_address=True
    def __init__(self,config:Config,runtime=None):
        self.config=config
        self.runtime=runtime if runtime is not None else HostRuntime(config)
        self.instance_id=self.runtime.instance_id
        super().__init__(('127.0.0.1',config.host_port),Handler)
        self.runtime.stop_callback=self.request_stop

    def request_stop(self):
        threading.Thread(target=self.shutdown,name='nxc-stop',daemon=True).start()


class Handler(BaseHTTPRequestHandler):
    server:HostServer
    protocol_version='HTTP/1.1'
    def log_message(self,fmt,*args):
        # 不记录RPC参数/文件内容，只记录请求状态。
        import sys
        print('[nxc-host]',fmt%args,file=sys.stderr,flush=True)

    def _send(self,status,data,content_type='application/json; charset=utf-8'):
        if not isinstance(data,bytes):data=json.dumps(data,ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type',content_type)
        self.send_header('Content-Length',str(len(data)))
        self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('Connection','close')
        self.end_headers();self.close_connection=True
        try:self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError):pass

    def do_GET(self):
        path=self.path.split('?',1)[0]
        if path=='/health':
            data=self.server.runtime.status()
            data.update(instance_id=self.server.instance_id,config_dir=str(config_dir()))
            self._send(200,data);return
        assets={'/':('index.html','text/html; charset=utf-8'),'/assets/app.js':('app.js','text/javascript; charset=utf-8'),
                '/assets/app.css':('app.css','text/css; charset=utf-8'),
                '/assets/ftp-setup.js':('ftp-setup.js','text/javascript; charset=utf-8')}
        if path not in assets:self._send(404,{'error':'not-found'});return
        name,mime=assets[path]
        try:body=files('nxc_host').joinpath('web',name).read_bytes()
        except OSError:self._send(404,{'error':'Web资源未安装'});return
        self._send(200,body,mime)

    def do_POST(self):
        if self.path!='/rpc':self._send(404,{'error':'not-found'});return
        if self.headers.get('Content-Type','').split(';',1)[0].strip()!='application/json':
            self._send(415,{'error':'Content-Type必须为application/json'});return
        origin=self.headers.get('Origin')
        allowed={f'http://127.0.0.1:{self.server.config.host_port}',f'http://localhost:{self.server.config.host_port}'}
        if origin is not None and origin not in allowed:self._send(403,{'error':'不接受跨站控制请求'});return
        try:length=int(self.headers.get('Content-Length','-1'))
        except ValueError:length=-1
        if length<0 or length>MAX_BODY:self._send(413,{'error':'请求正文大小不合法，最大16MiB'});return
        self.connection.settimeout(15)
        try:
            raw=self.rfile.read(length)
            if len(raw)!=length:raise ValueError('请求正文未完整收到')
            request=json.loads(raw,parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
        except (ValueError,UnicodeError):
            self._send(200,{'jsonrpc':'2.0','id':None,'error':{'code':-32700,'message':'JSON解析失败'}});return
        response=self.server.runtime.handle(request,context={'instance_id':self.headers.get('X-NXC-Instance'),'config_fingerprint':self.headers.get('X-NXC-Config')})
        if response is None:self._send(204,b'')
        else:self._send(200,response)


def serve(config:Config) -> int:
    server=HostServer(config)
    state=config_dir()/'service.json'
    write_json(state,{'pid':__import__('os').getpid(),'port':config.host_port,'instance_id':server.instance_id})
    previous={}
    def stopping(signum,frame):server.request_stop()
    if threading.current_thread() is threading.main_thread():
        for signum in (signal.SIGTERM,signal.SIGINT):previous[signum]=signal.signal(signum,stopping)
    try:
        print(f'NXC后台已启动：http://127.0.0.1:{config.host_port}',file=__import__('sys').stderr,flush=True)
        server.serve_forever(poll_interval=0.1)
    finally:
        server.server_close()
        try:server.runtime.close()
        finally:
            try:
                if json.loads(state.read_text()).get('instance_id')==server.instance_id:state.unlink(missing_ok=True)
            except (OSError,ValueError):pass
            for signum,handler in previous.items():signal.signal(signum,handler)
    return 0
