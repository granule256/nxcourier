"""宿主执行真实 cmdLaunch，服务桩只记录调用，不冒充真机启动。"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\{|\}', re.S)
    for token in tokens.finditer(source, start):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise AssertionError("产品函数不完整")


class TitleLaunchTests(unittest.TestCase):
    def test_actual_launch_handler(self):
        source = (ROOT / 'device/platform/source/tools/tool_title.cpp').read_text()
        functions = '\n'.join(function(source, x) for x in (
            'bool parseId(', 'u8 storageIdFromName(', 'void cmdLaunch('))
        harness = r'''
#include "nxc_sdk.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t; using Result=uint32_t;
#define R_FAILED(rc) ((rc)!=0)
#define R_SUCCEEDED(rc) ((rc)==0)
#define MAKEHOSVERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
constexpr u8 NcmStorageId_Any=0, NcmStorageId_SdCard=5,
 NcmStorageId_BuiltInUser=4, NcmStorageId_BuiltInSystem=3, NcmStorageId_GameCard=2;
constexpr int LibAppletMode_AllForeground=0, PmLaunchFlag_None=0;
struct AppletHolder{}; using AppletId=int;
struct NcmProgramLocation {u64 program_id; u8 storageID;};
int nsdev_init=0, nsdev_launch=0, nsdev_exit=0, pm_calls=0, applet_calls=0;
u32 version=MAKEHOSVERSION(22,5,0); Result launch_rc=0, init_rc=0;
u32 hosversionGet(){return version;}
Result nsdevInitialize(){++nsdev_init;return init_rc;}
void nsdevExit(){++nsdev_exit;}
Result nsdevLaunchApplicationWithStorageIdForDevelop(u64*p,u64 id,u32 flags,u8 storage,u8 patch){
 ++nsdev_launch; assert(id==0x123); assert(flags==0); assert(storage==5); assert(patch==0);*p=42;return launch_rc;
}
Result appletInitialize(){++applet_calls;return 0;} void appletExit(){}
Result appletCreateLibraryApplet(AppletHolder*,AppletId,int){return 1;}
Result appletHolderStart(AppletHolder*){return 1;}
void appletHolderRequestExitOrTerminate(AppletHolder*,u64){} void appletHolderClose(AppletHolder*){}
Result appletRequestLaunchApplication(u64,void*){return 0x1159;}
Result pmshellInitialize(){++pm_calls;return 0;} void pmshellExit(){}
Result pmshellLaunchProgram(int,NcmProgramLocation*,u64*){return 1;}
namespace nxc::host {void breadcrumb(const char*,...){} }
constexpr const char* kLaunchToken="I-KNOW-THIS-LAUNCHES";
'''
        checks = r'''
int main(){
 nxc::Args args; args.add("app","0x123"); args.add("storage","sd");
 nxc::Reply reply;
 cmdLaunch(args,reply); assert(reply.hasError()&&reply.errCode()==403);
 args.add("confirm",kLaunchToken);
 reply.reset();cmdLaunch(args,reply);
 assert(reply.hasError()&&reply.errCode()==409);
 assert(nsdev_init==0&&pm_calls==0&&applet_calls==0);
 version=0;reply.reset();cmdLaunch(args,reply);assert(reply.errCode()==409&&nsdev_init==0);
 version=MAKEHOSVERSION(17,0,2);reply.reset();cmdLaunch(args,reply);
 assert(reply.errCode()==409&&nsdev_init==0&&pm_calls==0&&applet_calls==0);
 version=MAKEHOSVERSION(17,0,1);reply.reset();cmdLaunch(args,reply);
 assert(!reply.hasError());assert(nsdev_init==1&&nsdev_launch==1&&nsdev_exit==1);
 assert(std::strstr(reply.body(),"pid=42")&&std::strstr(reply.body(),"launch-requested"));
 launch_rc=123;reply.reset();cmdLaunch(args,reply);assert(reply.hasError()&&reply.errCode()==500);
 assert(nsdev_exit==2&&pm_calls==0&&applet_calls==0);
 init_rc=77;reply.reset();cmdLaunch(args,reply);assert(reply.errCode()==503&&nsdev_launch==2);
 init_rc=launch_rc=0;version=MAKEHOSVERSION(1,0,0);reply.reset();cmdLaunch(args,reply);
 assert(!reply.hasError()&&nsdev_launch==3&&nsdev_exit==3);
 args.add("mode","applet");reply.reset();cmdLaunch(args,reply);
 assert(reply.errCode()==500&&applet_calls==1&&pm_calls==0&&nsdev_launch==3);
 puts("title.launch actual handler: PASS (host service stubs, no device)");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'launch.cpp'
            src.write_text(harness + functions + checks)
            binary = Path(tmp) / 'launch'
            subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-I' + str(ROOT / 'device/platform/include'),
                str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
