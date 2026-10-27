"""真实按键handler的预检查与释放错误路径，不操作主机。"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_core_behavior import function, ROOT
class InputHandlers(unittest.TestCase):
 def test_press_preflight_release_and_cancel(self):
  text=(ROOT/'device/platform/source/tools/tool_input.cpp').read_text()
  code=r'''
#include "nxc_sdk.hpp"
#include <cassert>
using u64=uint64_t;using Result=int;
#define R_FAILED(x) ((x)!=0)
int attachments=0,applied=0;bool failRelease=false,cancel=false;u64 last=99;
Result ensurePad(const char**){++attachments;return 0;}
bool parseButtons(const char* s,u64* b,char*,size_t){*b=1;return s && std::strcmp(s,"A")==0;}
bool applyState(u64 b,long long,long long,long long,long long){++applied;last=b;return !(failRelease&&!b);}
namespace nxc::host{void breadcrumb(const char*,...){}bool sleepMilliseconds(unsigned long long){return !cancel;}}
'''+function(text,'void cmdPress(')+function(text,'void cmdRelease(')+r'''
int main(){nxc::Reply r;static nxc::Args a;
a.add("buttons","A");a.add("hold_ms","10001");r.reset();cmdPress(a,r);assert(r.errCode()==400&&attachments==0&&applied==0);
a=nxc::Args{};a.add("buttons","A");a.add("lx","4294967296");r.reset();cmdPress(a,r);assert(r.errCode()==400&&attachments==0);
a=nxc::Args{};a.add("buttons","A");cancel=true;r.reset();cmdPress(a,r);assert(r.errCode()==409&&applied==2&&last==0);
cancel=false;failRelease=true;r.reset();cmdRelease(a,r);assert(r.errCode()==500);puts("input actual handlers: PASS");}
'''
  with tempfile.TemporaryDirectory() as tmp:
   src=Path(tmp)/'input.cpp';exe=Path(tmp)/'input';src.write_text(code)
   subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'device/platform/include'),str(src),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
