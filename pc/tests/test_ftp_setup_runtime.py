"""后台FTP向导RPC生命周期与绑定回归，不访问真实设备。"""
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from nxc_host.config import Config
from nxc_host.runtime import HostRuntime


class SetupRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.env=patch.dict(os.environ,{'NXC_CONFIG_DIR':self.temp.name})
        self.env.start()
        self.runtime=HostRuntime(Config(),device=Mock(),gdb=Mock(),ftp=Mock())
        self.setup=Mock()
        self.runtime._setup=self.setup
        self.addCleanup(self.runtime.close)
        self.addCleanup(self.env.stop)
        self.addCleanup(self.temp.cleanup)

    def rpc(self,method,params=None,context=None):
        return self.runtime.handle({'jsonrpc':'2.0','id':1,'method':method,'params':params or {}},context=context)

    def test_status_and_install_are_distinct_and_use_prepared_package(self):
        self.setup.status.return_value={'state':'missing'}
        self.setup.install.return_value={'state':'pending_activation'}
        self.assertEqual(self.rpc('host.ftp.setup.status')['result']['state'],'missing')
        self.setup.install.assert_not_called()
        self.assertEqual(self.rpc('host.ftp.setup.install',{'package_id':'a'*64})['result']['state'],'pending_activation')
        self.setup.install.assert_called_once_with('a'*64,anonymous=True)

    def test_stale_context_cannot_prepare_install_or_configure(self):
        for method in ('status','prepare','install','configure'):
            reply=self.rpc('host.ftp.setup.'+method,{'package_id':'a'*64},context={})
            self.assertEqual(reply['error']['code'],-32009)
        self.assertFalse(self.setup.mock_calls)

    def test_progress_does_not_wait_for_long_install_lock(self):
        self.setup.progress.return_value={'phase':'writing','completed':12,'total':40}
        results=[]
        with self.runtime._device_lock:
            worker=threading.Thread(target=lambda:results.append(self.rpc('host.ftp.setup.progress')))
            worker.start();worker.join(1)
            self.assertFalse(worker.is_alive(),'进度查询不能排在安装设备锁之后')
        self.assertEqual(results[0]['result']['completed'],12)

    def test_prepare_and_configure_route_without_implicit_install_or_restart(self):
        self.setup.prepare.return_value={'package_id':'b'*64}
        self.setup.configure_anonymous.return_value={'state':'ready'}
        args={'source':'local','data':'UEs='}
        self.rpc('host.ftp.setup.prepare',args)
        self.setup.prepare.assert_called_once_with(args)
        self.rpc('host.ftp.setup.configure')
        self.setup.configure_anonymous.assert_called_once_with()
        self.setup.install.assert_not_called()
        self.runtime.device.call_one.assert_not_called()

    def test_target_switch_discards_prepared_packages(self):
        self.runtime.gdb.status.return_value={'running':False,'connected':False,'uncertain':False}
        self.assertIn('result',self.rpc('host.configure',{'device_ip':'127.0.0.2'}))
        self.assertIsNone(self.runtime._setup)

    def test_closed_runtime_rejects_install_and_nonboolean_anonymous(self):
        reply=self.rpc('host.ftp.setup.install',{'package_id':'a'*64,'anonymous':'false'})
        self.assertIn('error',reply)
        self.setup.install.assert_not_called()
        self.runtime.close()
        self.assertIn('error',self.rpc('host.ftp.setup.install',{'package_id':'a'*64}))

    def test_reboot_checks_current_gdb_owner_in_background(self):
        self.runtime.device.call_one.return_value={'lines':['rebooting=1']}
        for owner in ({'running':True},{'connected':True},{'uncertain':True}):
            self.runtime.gdb.status.return_value=owner
            for args in ({'method':'power.reboot','params':{'confirm':'I-KNOW-THIS-REBOOTS'}},
                         {'raw':'power.shutdown confirm=I-KNOW-THIS-SHUTS-DOWN'}):
                reply=self.rpc('tools/call',{'name':'nxc_call','arguments':args})
                self.assertTrue(reply['result'].get('isError'),reply)
                self.runtime.device.call_one.assert_not_called()
