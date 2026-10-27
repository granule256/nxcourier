"""执行真实probeTcp，检查非阻塞失效与取消路径释放socket。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_core_behavior import function,ROOT
class NetProbe(unittest.TestCase):
 def test_actual_probe_cleanup_and_cancel(self):
  text=(ROOT/'device/platform/source/tools/tool_net.cpp').read_text()
  source=r'''
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
using u32=uint32_t;using u16=uint16_t;
int closes=0,connects=0,polls=0,active=0;bool failFlags=false,cancel=false;
namespace nxc::host {bool operationCancelled(){return cancel;}bool beginNetworkOperation(){++active;return true;}void endNetworkOperation(){--active;}}
u16 toNet16(u16 p){return p;}u32 toNet32(u32 p){return p;}
int fake_socket(int,int,int){return 3;}
int fake_fcntl(int,int cmd,...){if(failFlags){errno=EINVAL;return -1;}return cmd==F_GETFL?O_NONBLOCK:0;}
int fake_close(int){++closes;return 0;}
int fake_connect(int,const sockaddr*,socklen_t){++connects;errno=EINPROGRESS;return -1;}
int fake_poll(pollfd*,nfds_t,int timeout){assert(timeout<=50);++polls;cancel=true;return 0;}
int fake_getsockopt(int,int,int,void*,socklen_t*){return 0;}
#define socket fake_socket
#define fcntl fake_fcntl
#define close fake_close
#define connect fake_connect
#define poll fake_poll
#define getsockopt fake_getsockopt
'''+function(text,'bool probeTcp(')+r'''
int main(){int error=0;failFlags=true;assert(!probeTcp(0,1,10000,&error));assert(closes==1&&connects==0&&active==0);
failFlags=false;assert(!probeTcp(0,1,10000,&error));assert(closes==2&&polls==1&&error==ECANCELED&&active==0);puts("probe nonblocking and cancellation cleanup: PASS");}
'''
  with tempfile.TemporaryDirectory() as tmp:
   cpp=Path(tmp)/'net.cpp';exe=Path(tmp)/'net';cpp.write_text(source)
   subprocess.run(['c++','-std=c++17',str(cpp),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
