"""统一后台配置和共享会话；只使用设备替身。"""
import base64
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from nxc_host.config import Config, load_config, save_config
from nxc_host.runtime import HostRuntime
class Device:
    _sock=None
    def __init__(self): self.commands=[];self.closed=False
    def call_one(self,command,**kwargs):
        self.commands.append(command)
        return {'lines':['running=application pid=42']}
    def close(self):self.closed=True
class Gdb:
    def __init__(self):self.connected=False;self.actions=[];self.closed=False
    def status(self):return {'running':self.connected,'connected':self.connected,'attached_pid':None}
    def handle(self,action,args):self.actions.append((action,args));return {'action':action,'pid':args.get('pid')}
    def close(self):self.closed=True
    def doctor(self):return {'available':True}
class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory(prefix='nxc-runtime-')
        self.environment=mock.patch.dict(os.environ,{'NXC_CONFIG_DIR':self.directory.name})
        self.environment.start()
        self.d=Device();self.g=Gdb();self.ftp=mock.Mock();self.ftp.probe.return_value={'available':False}
        self.r=HostRuntime(Config(),device=self.d,gdb=self.g,ftp=self.ftp)
    def tearDown(self):
        self.r.close();self.environment.stop();self.directory.cleanup()
    def rpc(self,method,params=None):return self.r.handle({'jsonrpc':'2.0','id':1,'method':method,'params':params or {}})
    def test_status_does_not_connect_device(self):
        self.assertEqual(self.rpc('host.status')['result']['device']['port'],47800)
        self.assertFalse(self.d.commands)
    def test_attach_resolves_pid_once(self):
        out=self.rpc('tools/call',{'name':'nxc_gdb','arguments':{'action':'attach'}})
        self.assertNotIn('error',out);self.assertEqual(self.g.actions,[('open',{'connect':True}),('attach',{'action':'attach','pid':42})])
    def test_memory_conflict_rejected_before_device_call(self):
        self.g.connected=True
        out=self.rpc('tools/call',{'name':'nxc_call','arguments':{'method':'mem.read','params':{'addr':'0x10'}}})
        self.assertTrue(out['result']['isError']);self.assertFalse(self.d.commands)
    def test_all_memory_raw_routes_are_owned_by_gdb(self):
        self.g.connected=True
        for args in ({'raw':'  mem.read addr=0x10'}, {'method':'fs.ls','raw':'mem.write addr=0x10'}, {'method':'mem.info'}):
            out=self.rpc('tools/call',{'name':'nxc_call','arguments':args})
            self.assertTrue(out['result']['isError']);self.assertFalse(self.d.commands)
    def test_stale_context_never_changes_new_target(self):
        context={'instance_id':self.r.instance_id,'config_fingerprint':self.r.status()['config_fingerprint']}
        from dataclasses import replace
        self.r.config=replace(self.r.config,device_ip='127.0.0.2')
        out=self.r.handle({'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':'nxc_call','arguments':{'method':'input.seq','params':{'steps':'A:120'}}}},context=context)
        self.assertEqual(out['error']['code'],-32009);self.assertFalse(self.d.commands)
    def test_queued_request_rechecks_closed_after_lock(self):
        import threading,time
        out=[]
        with self.r._device_lock:
            thread=threading.Thread(target=lambda:out.append(self.rpc('tools/call',{'name':'nxc_call','arguments':{'method':'input.seq','params':{'steps':'A:120'}}})))
            thread.start();time.sleep(.03);self.r._closed=True
        thread.join(1)
        self.assertFalse(thread.is_alive());self.assertIn('error',out[0]);self.assertFalse(self.d.commands)
    def test_uncertain_debug_owner_blocks_memory_info(self):
        with mock.patch.object(self.g,'status',return_value={'running':True,'connected':False,'uncertain':True}):
            out=self.rpc('tools/call',{'name':'nxc_call','arguments':{'method':'mem.info'}})
        self.assertTrue(out['result']['isError']);self.assertFalse(self.d.commands)
    def test_raw_cannot_smuggle_second_command(self):
        out=self.rpc('tools/call',{'name':'nxc_call','arguments':{'raw':'hello\nmem.read addr=0x10'}})
        self.assertTrue(out['result']['isError']);self.assertFalse(self.d.commands)
    def test_http_context_required_for_target_operations(self):
        out=self.r.handle({'jsonrpc':'2.0','id':1,'method':'host.upload','params':{'remote_path':'/x','data':''}},context={})
        self.assertEqual(out['error']['code'],-32009)
    def test_close_catches_open_that_passed_guard_before_shutdown(self):
        import threading
        entered=threading.Event();release=threading.Event();first_close=threading.Event()
        class RacyGdb(Gdb):
            def handle(this,action,args):
                if action=='open':entered.set();release.wait(2);this.connected=True
                return {'action':action}
            def close(this):this.connected=False;first_close.set()
        self.r.gdb=RacyGdb()
        opened=threading.Thread(target=lambda:self.rpc('tools/call',{'name':'nxc_gdb','arguments':{'action':'open'}}))
        opened.start();self.assertTrue(entered.wait(1))
        stopped=threading.Thread(target=self.r.close);stopped.start();self.assertTrue(first_close.wait(1))
        release.set();opened.join(2);stopped.join(2)
        self.assertFalse(opened.is_alive() or stopped.is_alive());self.assertFalse(self.r.gdb.connected)
    def test_uncertain_debug_owner_prevents_switching_device(self):
        with mock.patch.object(self.g,'status',return_value={'running':False,'connected':False,'uncertain':True}):
            out=self.rpc('host.configure',{'device_ip':'127.0.0.2'})
        self.assertIn('error',out);self.assertEqual(self.r.config.device_ip,'192.168.1.50')
    def test_initialize_is_local_and_keeps_mcp_schema(self):
        out=self.rpc('initialize');self.assertIn('protocolVersion',out['result']);self.assertFalse(self.d.commands)
    def test_upload_is_verified_tool_and_temp_removed(self):
        seen=[]
        def put(device,args):
            seen.append(args['local_path']);self.assertEqual(Path(args['local_path']).read_bytes(),b'abc');return {'verified':True}
        with mock.patch('nxc_host.runtime.tools.tool_put',side_effect=put):
            out=self.rpc('host.upload',{'remote_path':'/test','data':base64.b64encode(b'abc').decode()})
        self.assertTrue(out['result']['verified']);self.assertFalse(Path(seen[0]).exists())
    def test_config_roundtrip_and_env(self):
        with tempfile.TemporaryDirectory() as d,mock.patch.dict(os.environ,{'NXC_CONFIG_DIR':d},clear=False):
            cfg=Config(device_ip='127.0.0.9',host_port=49123);save_config(cfg)
            self.assertEqual(load_config({'host_port':49123}).device_ip,'127.0.0.9')
    def test_invalid_config_and_upload_are_errors(self):
        with self.assertRaises(ValueError):Config(device_port=0)
        self.assertIn('error',self.rpc('host.upload',{'remote_path':'/x','data':'!!!'}))
    def test_close_owns_both_resources(self):
        self.r.close();self.assertTrue(self.d.closed);self.assertTrue(self.g.closed)
