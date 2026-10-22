"""用宿主机注入 socket 事件，执行产品原函数，验证监听故障不会进入 accept。"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'device/platform/source/core/protocol.cpp').read_text()


def function(signature):
    begin = source.index(signature)
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\{ |\{|\}', re.S)
    depth = 0
    for match in tokens.finditer(source, begin):
        token = match.group()
        if token.startswith('{'):
            depth += 1
        elif token == '}':
            depth -= 1
            if depth == 0:
                return source[begin:match.end()]
    raise AssertionError('函数范围未闭合')


harness = r'''
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <cstdarg>
#include <initializer_list>
struct Stop {};
int poll_calls, accept_calls, listener_closes, flags, socket_calls;
int poll_result = 1, poll_error = 0;
short test_events;
uint64_t slept_ns;
bool fail_nonblock;
int fake_socket(int, int, int) { ++socket_calls; return 3; }
int fake_setsockopt(int, int, int, const void*, socklen_t) { return 0; }
int fake_bind(int, const sockaddr*, socklen_t) { return 0; }
int fake_listen(int, int) { return 0; }
int fake_close(int fd) { if (fd == 3) ++listener_closes; return 0; }
int fake_fcntl(int, int cmd, ...) {
    if (cmd == F_GETFL) return flags;
    if (cmd == F_SETFL) {
        if (fail_nonblock) { errno = EINVAL; return -1; }
        va_list ap; va_start(ap, cmd); flags = va_arg(ap, int); va_end(ap);
        return 0;
    }
    return -1;
}
int fake_poll(pollfd* fds, nfds_t n, int) {
    if (poll_calls++) throw Stop{};
    if (poll_result < 0) { errno = poll_error; return -1; }
    for (nfds_t i=0; i<n; ++i) fds[i].revents = i==0 ? test_events : 0;
    return poll_result;
}
int fake_accept(int, sockaddr*, socklen_t*) { ++accept_calls; return 7; }
int fake_recv(int, void*, size_t, int) { return 0; }
void svcSleepThread(uint64_t ns) { slept_ns += ns; }
namespace nxc {
namespace log { template<class... T> void ring(const char*, T...) {} template<class... T> void watch(const char*, T...) {} }
namespace net {
constexpr unsigned short kPort=47800;
bool useReuseAddr() { return true; }
int listenerBacklog() { return 8; }
bool systemSocketService() { return false; }
int mode() { return 2; }
const char* modeName(int) { return "listen"; }
}
namespace power {
bool sleepRequested() { return false; }
void reportSocketsClosed() {}
void consumeWoke() {}
int lastState() { return 0; }
int eventCount() { return 0; }
int sleepEventCount() { return 0; }
}
namespace protocol {
constexpr int kMaxClients=4, kRequestBytes=8192;
struct Client { int fd=-1, len=0; bool dropping=false; char line[kRequestBytes]; };
Client g_clients[kMaxClients];
int activeClientCount() { int n=0; for(auto& c:g_clients) if(c.fd>=0) ++n; return n; }
bool sendAll(int, const char*, int) { return true; }
bool handleLine(int, char*, bool) { return true; }
'''

macros = '''
#define socket fake_socket
#define setsockopt fake_setsockopt
#define bind fake_bind
#define listen fake_listen
#define close fake_close
#define fcntl fake_fcntl
#define poll fake_poll
#define accept fake_accept
#define recv fake_recv
'''
tests = r'''
}}
int failures=0;
void check(bool ok, const char* message) { if(!ok) { ++failures; std::printf("FAIL: %s\n",message); } }
void reset() {
    poll_calls=accept_calls=listener_closes=flags=socket_calls=0;
    poll_result=1; poll_error=0; slept_ns=0; fail_nonblock=false;
    for(auto& c:nxc::protocol::g_clients) { c.fd=-1; c.len=0; c.dropping=false; }
}
void run(bool keep=true) { try { nxc::protocol::serveForever(3,keep); } catch(const Stop&) {} }
int main() {
    for(short events: {short(POLLERR),short(POLLHUP),short(POLLNVAL),short(POLLERR|POLLIN)}) {
        reset(); test_events=events; run();
        check(accept_calls==0,"监听错误/挂断/失效不得调用accept");
        check(listener_closes==1,"失效监听口应关闭以便后续重建");
    }
    reset(); test_events=POLLIN; run();
    check(accept_calls==1 && nxc::protocol::g_clients[0].fd==7,"正常连接仍须被接入");
    check(listener_closes==0,"listen模式接入正常客户端后保留监听");
    reset(); test_events=POLLERR; nxc::protocol::g_clients[0].fd=9; run();
    check(socket_calls==1 && nxc::protocol::g_clients[0].fd==9,"已有客户端在线时仍须恢复listen监听，不断开现有连接");
    reset(); test_events=POLLERR; nxc::protocol::g_clients[0].fd=9; run(false);
    check(socket_calls==0,"acceptonly仍须等待现有客户端全部断开后才重开监听");
    reset(); poll_result=-1; poll_error=ENETDOWN; run();
    check(slept_ns>=100000000ULL,"持续网络poll错误必须退避，不能忙循环");
    reset(); poll_result=-1; poll_error=EINTR; run();
    check(slept_ns==0,"信号中断应直接重试");
    reset(); int fd=nxc::protocol::openListener();
    check(fd==3 && (flags&O_NONBLOCK),"监听socket必须在接入前设为非阻塞");
    reset(); fail_nonblock=true; fd=nxc::protocol::openListener();
    check(fd<0 && listener_closes==1,"非阻塞设置失败必须关闭socket并报告失败");
    std::printf("listener recovery: %s (%d failures)\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory(prefix='nxc-listener-test-') as temp:
    temp = Path(temp)
    cpp = temp / 'listener.cpp'
    cpp.write_text(harness + macros + function('int openListener() {') + '\n' + function('void serveForever(int listenFd, bool keepListenerOpen) {') + '\n' + tests)
    binary = temp / 'listener-test'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', str(cpp), '-o', str(binary)], check=True)
    raise SystemExit(subprocess.run([str(binary)]).returncode)
