"""真实HTTP路径验收安装向导，设备为明确的旧版fs替身，无真实设备写入。"""
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'pc/src'))
sys.path.insert(0,str(ROOT/'pc/tests'))
from test_ftp_setup import FsDevice,Ftp,FILES,MODULE,FLAG,CONFIG
from nxc_host.config import Config
from nxc_host.client import HostClient
from nxc_host.daemon import HostServer
from nxc_host.runtime import HostRuntime
from nxc_host.services import ftp_package,ftp_setup


class SetupHttpTests(unittest.TestCase):
    def test_legacy_install_is_pending_until_rechecked_and_target_switch_invalidates(self):
        payload=io.BytesIO()
        with zipfile.ZipFile(payload,'w') as archive:
            for path,data in FILES.items():archive.writestr(path[1:],data)
        data=payload.getvalue()
        catalog={**ftp_package.CATALOG,'sha256':hashlib.sha256(data).hexdigest(),
                 'module_sha256':hashlib.sha256(FILES[MODULE]).hexdigest(),
                 'files_sha256':{path[1:]:hashlib.sha256(value).hexdigest() for path,value in FILES.items()}}
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ,{'NXC_CONFIG_DIR':directory}), \
                patch.object(ftp_package,'CATALOG',catalog),patch.object(ftp_setup,'CATALOG',catalog):
            with socket.socket() as listener:
                listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
            device=FsDevice();device.close=lambda:None
            ftp=Ftp(device,hot=False)
            cfg=Config(device_ip='127.0.0.1',host_port=port,timeout=2)
            runtime=HostRuntime(cfg,device=device,ftp=ftp)
            server=HostServer(cfg,runtime);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            client=HostClient(cfg)
            def rpc(method,args=None):
                reply=client.request({'jsonrpc':'2.0','id':1,'method':method,'params':args or {}})
                self.assertNotIn('error',reply,reply);return reply['result']
            try:
                self.assertEqual(rpc('host.ftp.setup.status')['state'],'missing')
                self.assertFalse(device.writes)
                prepared=rpc('host.ftp.setup.prepare',{'source':'local','data':base64.b64encode(data).decode()})
                self.assertFalse(device.writes)
                installed=rpc('host.ftp.setup.install',{'package_id':prepared['package_id']})
                self.assertEqual(installed['state'],'pending_activation')
                self.assertTrue(installed['module']['boot_enabled'])
                self.assertEqual(device.files[MODULE],FILES[MODULE])
                self.assertEqual(device.files[FLAG],b'')
                self.assertIn(b'anonymous:=1',device.files[CONFIG])
                # 模拟用户已重启；检测不负责“启动”FTP，不伪造就绪。
                ftp.available=True
                self.assertEqual(rpc('host.ftp.setup.status')['state'],'ready')
                count=len(device.writes)
                self.assertEqual(rpc('host.ftp.setup.install',{'package_id':prepared['package_id']})['state'],'ready')
                self.assertEqual(len(device.writes),count)
                with runtime._device_lock:
                    result=[]
                    worker=threading.Thread(target=lambda:result.append(rpc('host.ftp.setup.progress')))
                    worker.start();worker.join(1)
                    self.assertFalse(worker.is_alive())
                    self.assertTrue(result)
                with __import__('urllib.request',fromlist=['urlopen']).urlopen(client.url+'/assets/ftp-setup.js') as response:
                    self.assertIn(b'initFtpSetup',response.read())
                old=client.status();rpc('host.configure',{'device_ip':'127.0.0.2'})
                stale=client._http('/rpc',{'jsonrpc':'2.0','id':2,'method':'host.ftp.setup.install',
                                         'params':{'package_id':prepared['package_id']}},context=old)
                self.assertEqual(stale['error']['code'],-32009)
                self.assertEqual(len(device.writes),count)
            finally:
                server.shutdown();server.server_close();runtime.close();thread.join(2)
                self.assertFalse(thread.is_alive())
