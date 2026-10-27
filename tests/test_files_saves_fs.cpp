#include <cassert>
#include <fstream>
#include <string>
#include <filesystem>
#include <iostream>
#include "nxc_sdk.hpp"
#undef NXC_TOOL_SECTION
#define NXC_TOOL_SECTION
#include <unistd.h>
#include <switch.h>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
static bool mock_close_fail=false,mock_rename_fail=false,mock_cancelled=false,mock_zero_ino=false,mock_read_error=false;
namespace std {size_t testFread(void* b,size_t size,size_t count,FILE* f){return mock_read_error?0:std::fread(b,size,count,f);} int testFerror(FILE* f){return mock_read_error?1:std::ferror(f);} int testFclose(FILE* f){int rc=std::fclose(f);return mock_close_fail?EOF:rc;} }
static int mock_rename_call=0, mock_fail_rename_call=0;
static bool mock_fail_rollback=false, mock_horizon_rename=true;
int testRename(const char* src,const char* dst){
 ++mock_rename_call;
 if(mock_rename_fail||mock_rename_call==mock_fail_rename_call||(mock_fail_rollback&&mock_rename_call==3)){errno=EACCES;return -1;}
 struct stat st{};if(mock_horizon_rename&&::lstat(dst,&st)==0){errno=EEXIST;return -1;}
 return ::rename(src,dst);
}
int testStat(const char* p,struct stat* s){int rc=::stat(p,s);if(mock_zero_ino)s->st_ino=0;return rc;}
#define stat(...) testStat(__VA_ARGS__)
#define fread testFread
#define ferror testFerror
#define fclose testFclose
#define rename testRename
#include "../device/platform/source/tools/tool_fs.cpp"
#undef stat
#undef fread
#undef ferror
#undef fclose
#undef rename
namespace nxc::host {void log(const char*,...){} void breadcrumb(const char*,...){} bool operationCancelled(){return mock_cancelled;} }
std::string read(const std::string&p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
void call(const char* name,nxc::Args&a,nxc::Reply&r){r.reset();for(auto&m:kMethods)if(std::string(m.name)==name){m.handler(a,r);return;}r.fail(404,"no-method");}
int main(int argc,char**argv){std::string base=argv[1],p=base+"/file";std::ofstream(p)<<"original";nxc::Reply r;
 // 新方法必须提供正确的自描述元数据，不借用stat教程。
 for(const auto& method:kMethods)if(std::string(method.name)=="transfer_info"||std::string(method.name)=="sha256"){
  assert(method.info&&std::string(method.info->risk)=="read");assert(std::string(method.info->doc).find(std::string("fs.")+method.name)!=std::string::npos);
 }
 // 新方法必须直接运行产品处理函数；摘要向量由独立标准值校验。
 {nxc::Args a;call("transfer_info",a,r);assert(!r.hasError());std::string b=r.body();assert(b.find("write_chunk=3060")!=b.npos);assert(b.find("read_chunk=24000")!=b.npos);assert(b.find("max_inflight=2")!=b.npos);assert(b.find("sha256=1")!=b.npos);}
 const char* vectors[]={"", "abc"};
 const char* hashes[]={"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
 for(int i=0;i<2;++i){std::ofstream(p)<<vectors[i];nxc::Args a;a.add("path",p.c_str());call("sha256",a,r);assert(!r.hasError());assert(std::string(r.body()).find(hashes[i])!=std::string::npos);}
 {std::ofstream f(p);for(int i=0;i<1000000;++i)f<<'a';}
 {nxc::Args a;a.add("path",p.c_str());call("sha256",a,r);assert(!r.hasError());assert(std::string(r.body()).find("bytes=1000000")!=std::string::npos);assert(std::string(r.body()).find("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")!=std::string::npos);}
 for(int fault=0;fault<3;++fault){nxc::Args a;a.add("path",p.c_str());mock_cancelled=fault==0;mock_read_error=fault==1;mock_close_fail=fault==2;call("sha256",a,r);mock_cancelled=mock_read_error=mock_close_fail=false;assert(r.errCode()==(fault==0?499:500));}
 {nxc::Args a;a.add("path",base.c_str());call("sha256",a,r);assert(r.hasError());}
 std::ofstream(p)<<"original";
 {nxc::Args a;a.add("path",p.c_str());a.add("data","");a.add("allow_empty","1");call("write",a,r);assert(!r.hasError());assert(read(p).empty());}
 std::ofstream(p)<<"original";
 {nxc::Args a;a.add("path",p.c_str());mock_read_error=true;call("read",a,r);mock_read_error=false;assert(r.errCode()==500);}
 {nxc::Args a;a.add("path",p.c_str());mock_close_fail=true;call("read",a,r);mock_close_fail=false;assert(r.errCode()==500);}
 {nxc::Args a;a.add("path",p.c_str());a.add("data","");call("write",a,r);assert(r.errCode()==400);assert(read(p)=="original");}
 {nxc::Args a;a.add("path",p.c_str());a.add("data","");a.add("allow_empty","1");a.add("append","1");call("write",a,r);assert(r.errCode()==400);assert(read(p)=="original");}
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/copy").c_str());mock_close_fail=true;call("copy",a,r);mock_close_fail=false;assert(r.errCode()==500);}
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/cancelled").c_str());mock_cancelled=true;call("copy",a,r);mock_cancelled=false;assert(r.errCode()==499);}
 std::ofstream(base+"/zero_ino")<<"previous";
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/zero_ino").c_str());mock_zero_ino=true;call("copy",a,r);mock_zero_ino=false;assert(!r.hasError());assert(read(base+"/zero_ino")=="original");}
 std::ofstream(base+"/target")<<"keep";
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/target").c_str());a.add("replace","0");call("rename",a,r);assert(r.errCode()==409);assert(read(base+"/target")=="keep");}
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/target").c_str());a.add("rollback",(base+"/rb-first").c_str());mock_rename_fail=true;call("rename",a,r);mock_rename_fail=false;assert(r.errCode()==500);assert(read(base+"/target")=="keep");assert(read(p)=="original");}
 // Horizon rename不覆盖已存在目标：必须先保留旧目标到显式rollback路径。
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/target").c_str());call("rename",a,r);assert(r.errCode()==400);assert(read(base+"/target")=="keep");}
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/target").c_str());a.add("rollback",(base+"/rb-second").c_str());mock_rename_call=0;mock_fail_rename_call=2;call("rename",a,r);mock_fail_rename_call=0;assert(r.errCode()==500);assert(std::string(r.errMsg()).find("original-restored")!=std::string::npos);assert(read(base+"/target")=="keep");assert(!std::filesystem::exists(base+"/rb-second"));assert(read(p)=="original");}
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/target").c_str());a.add("rollback",(base+"/rb-failed").c_str());mock_rename_call=0;mock_fail_rename_call=2;mock_fail_rollback=true;call("rename",a,r);mock_fail_rename_call=0;mock_fail_rollback=false;assert(r.errCode()==500);assert(std::string(r.errMsg()).find("commit-and-rollback-failed")!=std::string::npos);assert(!std::filesystem::exists(base+"/target"));assert(read(base+"/rb-failed")=="keep");assert(read(p)=="original");}
 std::filesystem::rename(base+"/rb-failed",base+"/target");
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/target").c_str());a.add("rollback",(base+"/rb-success").c_str());mock_rename_call=0;call("rename",a,r);assert(!r.hasError());assert(read(base+"/target")=="original");assert(read(base+"/rb-success")=="keep");assert(std::string(r.body()).find("rollback=")!=std::string::npos);}
 std::ofstream(p)<<"original";
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",p.c_str());call("copy",a,r);assert(r.hasError());assert(read(p)=="original");}
 {nxc::Args a;a.add("src",p.c_str());a.add("dst",(base+"/new").c_str());a.add("replace","1");call("rename",a,r);assert(!r.hasError());assert(read(base+"/new")=="original");}
 // 长UTF-8文件名和长父目录仍是合法SD路径，不应受ERR文本容量的人工限制。
 for(bool longParent:{false,true}) {
  std::string dir=longParent?base+"/"+std::string(216,'d'):base;std::filesystem::create_directories(dir);
  std::string name="live";if(!longParent){name.clear();for(int i=0;i<72;++i)name+="测";}
  std::string stage=dir+"/long-stage",dst=dir+"/"+name,rollback=dir+"/long-rb";
  std::ofstream(stage)<<"next";std::ofstream(dst)<<"before";
  nxc::Args a;a.add("src",stage.c_str());a.add("dst",dst.c_str());a.add("rollback",rollback.c_str());call("rename",a,r);
  assert(!r.hasError());assert(read(dst)=="next");assert(read(rollback)=="before");
 }
 for(int i=0;i<5;i++)std::ofstream(base+"/p"+std::to_string(i))<<"x";
 {nxc::Args a;a.add("path",base.c_str());a.add("offset","2");a.add("count","2");call("ls",a,r);assert(!r.hasError());std::string body=r.body();assert(body.find("count=2")!=body.npos);assert(body.find("next_offset=4")!=body.npos);assert(body.find("truncated=1")!=body.npos);}
 std::cout<<"fs actual handlers: PASS\n";
}
