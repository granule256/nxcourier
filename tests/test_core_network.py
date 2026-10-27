"""执行真实网络循环、应答发送和队列，系统调用仅用可控桩。"""
from pathlib import Path
import ast
import re
import subprocess
import tempfile
import unittest
class CoreNetwork(unittest.TestCase):
    def test_real_network_queue_and_eof(self):
        with tempfile.TemporaryDirectory() as temp:

            root = Path(__file__).resolve().parents[1]
            proto = (root / 'device/platform/source/core/protocol.cpp').read_text()

            def function(signature):
                start = proto.index(signature)
                depth = 0
                for match in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\{|\}', proto[start:], re.S):
                    token = match.group()
                    if token == '{':
                        depth += 1
                    elif token == '}':
                        depth -= 1
                        if depth == 0:
                            return proto[start:start + match.end()] + '\n'
                raise AssertionError(signature)

            values = {}
            tree = ast.parse((root / 'device/platform/tests/test_listener_recovery.py').read_text())
            for item in tree.body:
                if isinstance(item, ast.Assign) and isinstance(item.value, ast.Constant) and isinstance(item.value.value, str):
                    for target in item.targets:
                        if isinstance(target, ast.Name):
                            values[target.id] = item.value.value

            harness = values['harness'].replace(
                'void svcSleepThread(uint64_t ns) { slept_ns += ns; }',
                'void svcSleepThread(uint64_t ns) { slept_ns += ns; throw Stop{}; }')
            harness = harness.replace(
                'struct Work { void cancelAll() {} bool hasClient(int,uint64_t){return false;} bool hasWork(){return false;} } g_work;',
                'using WorkQueue=nxc::jobs::Queue<8,kRequestBytes,1024>; WorkQueue g_work; int g_workReady=0; const char* kPlatformName="nxc";')
            harness = harness.replace(
                'void closeClient(int slot) { if(g_clients[slot].fd>=0) fake_close(g_clients[slot].fd); g_clients[slot].fd=-1; }',
                'void closeClient(int slot);')
            harness = harness.replace('bool submitLine(int, char*, bool) { return true; }', 'bool submitLine(int, char*, bool);')
            harness = harness.replace('namespace protocol {', 'namespace reg { int toolCount(){return 14;} }\nnamespace protocol {')
            harness = harness.replace('void flushResponses() {}', 'void flushResponses();')
            harness = harness.replace('namespace log {', 'namespace log { int bootCount(){return 1;}')
            # 监听口始终存在，EOF应投递给已有客户端而非监听fd。
            harness = harness.replace('i==0 ? test_events : 0', 'fds[i].fd==7 ? test_events : 0')
            checks = r'''
            }}
            int main() {
                using namespace nxc::protocol;
                g_clients[0].fd=7; g_clients[0].generation=1;
                auto* j=g_work.submit(0,1,"input.seq steps=A:10000",false);
                g_work.take(); test_events=POLLIN;
                try { serveForever(-1); } catch(const Stop&) {}
                printf("after_EOF readClosed=%d fd=%d cancelled=%d state=%d\n",
                       g_clients[0].readClosed,g_clients[0].fd,int(j->cancelled.load()),int(j->state.load()));
                assert(g_clients[0].readClosed && g_clients[0].fd==7 && !j->cancelled.load());
                std::strcpy(j->output,"reply\n");j->outputBytes=6;
                g_work.finish(*j);
                for(int i=0;i<4;++i)flushResponses();
                assert(wire=="reply\n" && g_clients[0].fd==-1);
                g_clients[0].fd=7;g_clients[0].readClosed=false;
                int accepted=0; char line[]="fs.stat path=/x";
                for(int i=0;i<8;++i) accepted+=submitLine(0,line,false);
                g_clients[1].fd=8; g_clients[1].generation=1;
                assert(accepted==2 && submitLine(1,line,false));
                auto* first=g_work.take();std::strcpy(first->output,"ONE");first->outputBytes=3;g_work.finish(*first);
    auto* second=g_work.take();std::strcpy(second->output,"TWO");second->outputBytes=3;g_work.finish(*second);
    wire.clear();flushResponses();flushResponses();assert(wire=="ONETWO");
    g_work.cancelAll();while(auto* pending=g_work.take()){g_work.finish(*pending);g_work.retire(*pending);}
                auto* stale=g_work.submit(0,1,"stale",false);assert(stale);g_work.take();
                std::strcpy(stale->output,"OLD");stale->outputBytes=3;g_work.finish(*stale);
                ++g_clients[0].generation;wire.clear();flushResponses();assert(wire.empty());
                auto* running=g_work.submit(0,g_clients[0].generation,"input.seq",false);assert(running);g_work.take();
                char cancel[]="cancel";assert(submitLine(1,cancel,false));assert(running->cancelled.load());
                puts("network EOF, ordered partial output, connection generation, client quota and cancel: PASS");
            }
            '''
            source = '#include <cassert>\n#include <string>\n#include <sys/socket.h>\nstd::string wire; int fake_send(int,const void* b,size_t n,int){size_t part=n>3?3:n;wire.append(static_cast<const char*>(b),part);return static_cast<int>(part); }\n#define send fake_send\n#include "' + str(root / 'device/platform/source/core/jobs.hpp') + '"\nvoid ueventSignal(int*){}\n'
            source += harness + values['macros']
            for signature in ('void closeClient(', 'void flushResponses(', 'bool submitLine(', 'int openListener(', 'void serveForever('):
                source += function(signature)
            source += checks
            cpp = Path(temp) / 'core-network.cpp'
            cpp.write_text(source)
            subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(Path(temp) / 'core-network')], check=True)
            subprocess.run([str(Path(temp) / 'core-network')], check=True, timeout=10)
