"""执行设备真实启动函数，验证无模式文件启动及PSC失败时禁止监听。"""
from pathlib import Path
import ast
import subprocess
import tempfile
import unittest

from test_core_behavior import function


ROOT = Path(__file__).resolve().parents[1]


class DeviceStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        main = (ROOT / 'device/platform/source/main.cpp').read_text()
        harness = r'''
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include "net.hpp"
using u64=uint64_t; using Result=int;
#define R_SUCCEEDED(rc) ((rc)==0)
constexpr unsigned kInitSd=1,kInitSocket=2;
unsigned g_init=kInitSd;
bool sm=false,pscOk=true,failSockets=false;
int configReads=0,listenCalls=0,serveCalls=0,socketCalls=0,sleeps=0,pscCalls=0;
const char* netText=nullptr; const char* pscText=nullptr;
struct Stop {};
void svcSleepThread(u64) { if(++sleeps>65)throw Stop{}; }
struct SocketInitConfig {};
SocketInitConfig makeSmallSocketConfig(){return {};}

Result socketInitialize(const SocketInitConfig*){++socketCalls;return failSockets?-1:0;}
Result socketGetLastResult(){return 0;}
namespace nxc {
namespace sd {
constexpr bool kEnabled=true;
FILE* open(const char* path,const char*) {
    ++configReads;
    const char* text=std::strstr(path,"net_mode")?netText:pscText;
    if(!text)return nullptr;
    FILE* f=std::tmpfile();std::fputs(text,f);std::rewind(f);return f;
}
}
namespace log {
template<class...T>void breadcrumb(const char*,T...){}
template<class...T>void ring(const char*,T...){}
template<class...T>void watch(const char*,T...){}
}
namespace reg { int toolCount(){return 14;} }
namespace host {
bool smOpen(){sm=true;return true;}
void smClose(){sm=false;}
bool smIsOpen(){return sm;}
}
namespace power {
bool start(){++pscCalls;return pscOk;}
}
namespace protocol {
int openListener(){if(!pscCalls||!pscOk||sm)std::abort();++listenCalls;return 7;}
void serveForever(int fd){if(fd!=7)std::abort();++serveCalls;}
}
}
'''
        checks = r'''
int main(int argc,char** argv){
    int scenario=argc>1?std::atoi(argv[1]):0;
    if(scenario==1){netText="off";pscText="observe";}
    if(scenario==2){netText="broken";pscText="broken";}
    if(scenario==3)pscOk=false;
    if(scenario==4)failSockets=true;
    try{runDevice(0,nullptr);}catch(const Stop&){}
    if(configReads!=0){std::fprintf(stderr,"启动仍依赖模式文件：读取%d次\n",configReads);return 1;}
    if(scenario<3 && (listenCalls!=1||serveCalls!=1||pscCalls!=1))return 2;
    if(scenario==3 && (listenCalls||serveCalls||pscCalls!=1))return 3;
    if(scenario==4 && (listenCalls||serveCalls||pscCalls||socketCalls!=120||sm))return 4;
    if(sm)return 5;
    return 0;
}
'''
        source = Path(cls.temp.name) / 'startup.cpp'
        source.write_text(harness + function(main, 'int main(').replace(
            'int main(', 'int runDevice(', 1) + checks)
        cls.binary = Path(cls.temp.name) / 'startup'
        subprocess.run(['c++', '-std=c++17', '-I'+str(ROOT / 'device/platform/source/core'),
                        str(source), '-o', str(cls.binary)], check=True)

    def run_scenario(self, scenario):
        subprocess.run([str(self.binary), str(scenario)], check=True, timeout=5)

    def test_without_mode_files(self):
        self.run_scenario(0)

    def test_legacy_disabled_modes_are_ignored(self):
        self.run_scenario(1)

    def test_corrupt_legacy_modes_are_ignored(self):
        self.run_scenario(2)

    def test_failed_psc_never_opens_listener(self):
        self.run_scenario(3)

    def test_socket_initialization_failure_is_bounded(self):
        self.run_scenario(4)

    def test_sleep_cleanup_acknowledgement_and_next_cycle(self):
        power = (ROOT / 'device/platform/source/core/power.cpp').read_text()
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
using u32=uint32_t; using Result=int;
#define R_FAILED(rc) ((rc)!=0)
enum PscPmState {PscPmState_Awake,PscPmState_ReadyAwaken,PscPmState_ReadySleep,
 PscPmState_ReadySleepCritical,PscPmState_ReadyAwakenCritical,PscPmState_ReadyShutdown};
namespace nxc {
std::atomic<bool> g_sleeping{false},g_socketsClosed{false},g_acked{false},g_woke{false};
int g_events=0,g_sleepEvents=0,g_lastState=-1,g_module=0,g_waiter=0;
int step=0,acks=0,closeWaits=0; struct Stop {};
namespace log {template<class...T>void ring(const char*,T...) {}}
void svcSleepThread(uint64_t){assert(g_sleeping);g_socketsClosed=true;++closeWaits;}
int waitSingle(int,uint64_t){
 if(step==2)assert(g_sleeping && g_socketsClosed && closeWaits==1);
 if(step==3)assert(!g_sleeping && !g_socketsClosed && g_woke);
 if(step==4)throw Stop{};
 return 0;
}
int pscPmModuleGetRequest(int*,PscPmState* state,u32* flags){
 PscPmState states[]={PscPmState_ReadySleep,PscPmState_ReadySleepCritical,
                     PscPmState_Awake,PscPmState_ReadySleep};
 *state=states[step++];*flags=0;return 0;
}
int pscPmModuleAcknowledge(int*,PscPmState state){
 if(state!=PscPmState_Awake)assert(g_sleeping && g_socketsClosed);
 ++acks;return 0;
}
'''
        program = harness + '\n'.join(function(power, signature) for signature in (
            'const char* stateName(', 'bool isSleepState(', 'bool isWakeState(',
            'bool waitSocketsClosed(', 'void notificationThread(')) + r'''
}
int main(){
 using namespace nxc;
 try{notificationThread(nullptr);}catch(const Stop&){}
 assert(acks==4 && closeWaits==2 && g_sleepEvents==3 && g_events==4 && g_acked);
}
'''
        source = Path(self.temp.name) / 'sleep.cpp'
        binary = Path(self.temp.name) / 'sleep'
        source.write_text(program)
        subprocess.run(['c++', '-std=c++17', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)

    def test_network_closes_all_sockets_before_reporting_and_reopens_on_wake(self):
        tree = ast.parse((ROOT / 'device/platform/tests/test_listener_recovery.py').read_text())
        values = {target.id: item.value.value for item in tree.body
                  if isinstance(item, ast.Assign) and isinstance(item.value, ast.Constant)
                  and isinstance(item.value.value, str) for target in item.targets
                  if isinstance(target, ast.Name)}
        harness = values['harness'].replace(
            'bool sleepRequested() { return false; }',
            'int checks=0; bool sleepRequested() { return checks++<2; }').replace(
            'void reportSocketsClosed() {}',
            'void reportSocketsClosed() { assert(listener_closes==1 && clientClosed && workerClosed); ++reported; }').replace(
            'void consumeWoke() {}', 'void consumeWoke() { ++wakes; }').replace(
            'if (fd == 3) ++listener_closes;',
            'if (fd == 3) ++listener_closes; if(fd==9)clientClosed=true;').replace(
            'void svcSleepThread(uint64_t ns) { slept_ns += ns; }',
            'void svcSleepThread(uint64_t ns) { slept_ns += ns; if(ns==5000000ULL) finishNetworkOperation(); }')
        protocol = (ROOT / 'device/platform/source/core/protocol.cpp').read_text()
        program = ('#include <cassert>\nbool clientClosed=false,workerClosed=false;\n'
                   'int reported=0,wakes=0; void finishNetworkOperation();\n' + harness + values['macros']
                   + function(protocol, 'int openListener(') + function(protocol, 'void serveForever(') + r'''
}}
void finishNetworkOperation(){nxc::protocol::g_networkOperations=0;workerClosed=true;}
int main(){
 nxc::protocol::g_clients[0].fd=9;
 nxc::protocol::g_networkOperations=1;
 poll_result=0;
 try{nxc::protocol::serveForever(3);}catch(const Stop&){}
 assert(reported==1 && wakes==1 && socket_calls==1 && accept_calls==0);
 assert(nxc::protocol::g_clients[0].fd==-1);
}
''')
        source = Path(self.temp.name) / 'network-sleep.cpp'
        binary = Path(self.temp.name) / 'network-sleep'
        source.write_text(program)
        subprocess.run(['c++', '-std=c++17', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5)
