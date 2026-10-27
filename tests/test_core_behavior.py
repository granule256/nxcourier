"""宿主执行产品解析和环形日志函数；系统调用仅用线程同步桩替代。"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def function(source, signature):
    begin=source.index(signature); depth=0
    for match in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\{|\}',source[begin:],re.S):
        token=match.group()
        if token=='{': depth+=1
        elif token=='}':
            depth-=1
            if not depth: return source[begin:begin+match.end()]
    raise AssertionError(signature)
class CoreBehavior(unittest.TestCase):
    def test_real_parser_and_concurrent_ring(self):
        protocol=(ROOT/'device/platform/source/core/protocol.cpp').read_text()
        log=(ROOT/'device/platform/source/core/log.cpp').read_text()
        program=r'''
#include "nxc_sdk.hpp"
#include <cassert>
#include <thread>
#include <mutex>
#include <string>
using Mutex=std::mutex;
void mutexLock(Mutex* m){ m->lock(); }
void mutexUnlock(Mutex* m){ m->unlock(); }
using namespace nxc;
'''+function(protocol,'void parseRequest(')+r'''
constexpr int kRingLines=64,kRingLineBytes=160;
char s_ring[kRingLines][kRingLineBytes];
Mutex s_ringMutex;
int s_ringHead=0,s_ringCount=0,s_boot=1;
'''+function(log,'void ringLine(')+function(log,'void dump(')+r'''
int main(){
 static Args a; const char* method=nullptr;
 char tooLong[5000]; std::strcpy(tooLong,"fs.write data="); std::memset(tooLong+14,'x',4500);tooLong[4514]='\0';
 parseRequest(tooLong,method,a);assert(a.invalid());
 char duplicate[]="fs.read len=1 len=2";parseRequest(duplicate,method,a);assert(a.invalid());
 char nul[]="fs.rm path=/a%00/b";parseRequest(nul,method,a);assert(a.invalid());
 char good[]="fs.stat path=/a%20b";parseRequest(good,method,a);assert(!a.invalid()&&std::strcmp(a.get("path"),"/a b")==0);
 std::thread writer([]{for(int i=0;i<10000;++i) ringLine("one");});
 std::thread other([]{for(int i=0;i<10000;++i) ringLine("two");});
 for(int i=0;i<10000;++i){ Reply r;r.reset();dump(r,60);assert(!r.overflowed());}
 writer.join();other.join();assert(s_ringCount==64);puts("real parser and concurrent ring: PASS");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'core.cpp';exe=Path(tmp)/'core';src.write_text(program)
            subprocess.run(['c++','-std=c++17','-pthread','-I'+str(ROOT/'device/platform/include'),str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
