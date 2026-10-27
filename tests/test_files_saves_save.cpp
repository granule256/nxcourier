#include <unistd.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include "test_files_saves_switch.hpp"
#include "nxc_sdk.hpp"
static char test_backup_root[1024];
#define NXC_SAVE_BACKUP_ROOT test_backup_root
#undef NXC_TOOL_SECTION
#define NXC_TOOL_SECTION
#include "../device/platform/source/tools/tool_save.cpp"
static bool mock_cancelled=false;
namespace nxc::host {void log(const char*,...){} void breadcrumb(const char*,...){} int bootCount(){return 7;} bool operationCancelled(){return mock_cancelled;} }
std::string slurp(const std::string&p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
void invoke(const char* method,nxc::Args&a,nxc::Reply&r){r.reset();for(auto&m:kMethods)if(!strcmp(m.name,method)){m.handler(a,r);return;}r.fail(404,"missing-method");}
void expect(bool ok,const nxc::Reply&r){if(!ok){std::cerr<<"unexpected reply code="<<r.errCode()<<" msg="<<r.errMsg()<<" body="<<r.body()<<"\n";abort();}}
int main(int argc,char**argv){
 snprintf(test_backup_root,sizeof(test_backup_root),"%s/backups",argv[1]);mock_save_root=std::string(argv[1])+"/source";
 std::filesystem::create_directories(mock_save_root+"/nested");std::ofstream(mock_save_root+"/nested/a")<<"payload";std::ofstream(mock_save_root+"/empty");
 FsSaveDataInfo one{};one.application_id=1;one.save_data_id=10;one.save_data_space_id=1;one.uid.uid[0]=123;
 mock_infos={one,one};mock_infos[1].uid.uid[0]=456;mock_infos[1].save_data_id=11;nxc::Reply r;
 {nxc::Args a;a.add("app","1");a.add("offset","1");a.add("count","1");invoke("list",a,r);expect(!r.hasError(),r);assert(std::string(r.body()).find("save_id=0x000000000000000B")!=std::string::npos);}
 {nxc::Args a;a.add("app","1");invoke("backup",a,r);expect(r.errCode()==409,r);}
 std::string backup=std::string(test_backup_root)+"/chosen";
 auto backupArgs=[&](){nxc::Args a;a.add("app","1");a.add("save_id","10");a.add("to",backup.c_str());return a;};
 {auto a=backupArgs();mock_process_check_fail=true;invoke("backup",a,r);mock_process_check_fail=false;expect(r.errCode()==500,r);assert(!std::filesystem::exists(backup));}
 {auto a=backupArgs();mock_running=true;invoke("backup",a,r);mock_running=false;expect(r.errCode()==409,r);assert(!std::filesystem::exists(backup));}
 {auto a=backupArgs();invoke("backup",a,r);expect(!r.hasError(),r);assert(slurp(backup+"/data/nested/a")=="payload");assert(std::filesystem::exists(backup+"/COMPLETE"));}
 {auto a=backupArgs();invoke("backup",a,r);expect(r.errCode()==409,r);assert(slurp(backup+"/data/nested/a")=="payload");}
 std::string defaults[2];for(int i=0;i<2;++i){nxc::Args a;a.add("app","1");a.add("save_id","10");invoke("backup",a,r);expect(!r.hasError(),r);std::string body=r.body();auto p=body.find("to=");assert(p!=body.npos);defaults[i]=body.substr(p+3,body.find_first_of(" \n",p+3)-(p+3));}assert(defaults[0]!=defaults[1]);
 auto verifyArgs=[&](){nxc::Args a;a.add("from",backup.c_str());return a;};
 {auto a=verifyArgs();invoke("verify",a,r);expect(!r.hasError(),r);}
 auto restoreArgs=[&](){auto a=verifyArgs();a.add("app","1");a.add("save_id","10");a.add("confirm","RESTORE_SAVE");return a;};
 auto noWrites=[&](nxc::Args&a,int code){mock_writes=0;invoke("restore",a,r);expect(r.errCode()==code,r);assert(mock_writes==0);};
 // 清单哈希正确但目录条目乱序也必须在恢复写入前拒绝。
 auto originalManifest=slurp(backup+"/manifest.nxc");
 auto installManifest=[&](const std::string& content){
  std::ofstream(backup+"/manifest.nxc")<<content;char hash[65];long long size=0;nxc::Reply check;check.reset();
  assert(hashFile((backup+"/manifest.nxc").c_str(),hash,&size,check));std::ofstream(backup+"/COMPLETE")<<hash<<"\n";
 };
 std::vector<std::string> manifestLines;{std::istringstream in(originalManifest);std::string line;while(std::getline(in,line))manifestLines.push_back(line);}
 size_t nestedDir=0,nestedFile=0;for(size_t i=1;i<manifestLines.size();++i){auto path=manifestLines[i].substr(manifestLines[i].find_last_of(' ')+1);if(path=="2f6e6573746564")nestedDir=i;if(path=="2f6e65737465642f61")nestedFile=i;}assert(nestedDir&&nestedFile);
 std::ofstream(mock_save_root+"/must-remain")<<"current-state";
 for(bool rootLast:{false,true}){
  auto reordered=manifestLines;std::swap(reordered[rootLast?1:nestedDir],reordered[rootLast?nestedDir:nestedFile]);std::string content;for(auto& line:reordered)content+=line+"\n";installManifest(content);
  auto a=restoreArgs();noWrites(a,422);assert(slurp(mock_save_root+"/must-remain")=="current-state");assert(slurp(mock_save_root+"/nested/a")=="payload");installManifest(originalManifest);
 }
 {auto a=verifyArgs();a.add("app","1");noWrites(a,403);}
 {auto a=restoreArgs();mock_running=true;noWrites(a,409);mock_running=false;}
 {auto a=verifyArgs();a.add("app","1");a.add("save_id","11");a.add("uid","000000000000000000000000000001C8");a.add("confirm","RESTORE_SAVE");noWrites(a,409);}
 std::ofstream(backup+"/data/nested/a")<<"damaged";
 {auto a=verifyArgs();invoke("verify",a,r);expect(r.hasError(),r);auto b=restoreArgs();noWrites(b,422);}
 std::ofstream(backup+"/data/nested/a")<<"payload";
 std::ofstream(backup+"/data/extra")<<"unlisted";
 {auto a=restoreArgs();noWrites(a,422);}std::filesystem::remove(backup+"/data/extra");
 std::filesystem::rename(backup+"/COMPLETE",backup+"/INCOMPLETE");
 {auto a=restoreArgs();noWrites(a,422);}std::filesystem::rename(backup+"/INCOMPLETE",backup+"/COMPLETE");
 {auto a=restoreArgs();mock_cancelled=true;noWrites(a,499);mock_cancelled=false;}
 std::ofstream(mock_save_root+"/old")<<"obsolete";std::ofstream(mock_save_root+"/nested/a")<<"changed";
 {auto a=restoreArgs();invoke("restore",a,r);expect(!r.hasError(),r);assert(slurp(mock_save_root+"/nested/a")=="payload");assert(!std::filesystem::exists(mock_save_root+"/old"));assert(mock_commits==1);}
 {auto a=restoreArgs();mock_commit_fail=true;invoke("restore",a,r);expect(r.errCode()==500,r);mock_commit_fail=false;}
 {nxc::Args a;a.add("app","1");a.add("save_id","10");std::string fail=std::string(test_backup_root)+"/broken";a.add("to",fail.c_str());mock_read_fail=true;invoke("backup",a,r);expect(r.hasError(),r);assert(!std::filesystem::exists(fail+"/COMPLETE"));mock_read_fail=false;}
 // SdUser=4 曾被清单校验的 space>3 拒绝；真实枚举下跑完备份→验证→恢复。
 const FsSaveDataSpaceId supportedSpaces[] = {FsSaveDataSpaceId_SdUser, FsSaveDataSpaceId_User, FsSaveDataSpaceId_System, FsSaveDataSpaceId_SdSystem};
 for(auto space:supportedSpaces){
  FsSaveDataInfo info=one;info.application_id=2;info.save_data_id=20;info.save_data_space_id=space;
  info.save_data_type=FsSaveDataType_SystemBcat;info.save_data_rank=FsSaveDataRank_Secondary;info.save_data_index=65535;mock_infos={info};
  std::string roundtrip=std::string(test_backup_root)+"/space-"+std::to_string(space);
  nxc::Args b;b.add("app","2");b.add("save_id","20");b.add("to",roundtrip.c_str());invoke("backup",b,r);expect(!r.hasError(),r);
  nxc::Args v;v.add("from",roundtrip.c_str());invoke("verify",v,r);expect(!r.hasError(),r);
  nxc::Args restore;restore.add("app","2");restore.add("save_id","20");restore.add("from",roundtrip.c_str());restore.add("confirm","RESTORE_SAVE");
  invoke("restore",restore,r);expect(!r.hasError(),r);assert(slurp(mock_save_root+"/nested/a")=="payload");
 }
 auto validHeader=[&](unsigned long long space,unsigned long long type,unsigned long long rank,unsigned long long index){
  FILE* f=tmpfile();assert(f);fprintf(f,"NXC_SAVE_V1 0000000000000001 000000000000000A 0000000000000000000000000000007B %llu %llu %llu %llu 0000000000000000\n",space,type,rank,index);rewind(f);FsSaveDataInfo parsed{};bool ok=readHeader(f,parsed);fclose(f);return ok;
 };
 for(unsigned type=FsSaveDataType_System;type<=FsSaveDataType_SystemBcat;++type)assert(validHeader(FsSaveDataSpaceId_User,type,FsSaveDataRank_Primary,0));
 assert(validHeader(FsSaveDataSpaceId_SdUser,FsSaveDataType_Account,FsSaveDataRank_Secondary,65535));
 for(unsigned space:{3u,100u,101u,255u})assert(!validHeader(space,FsSaveDataType_Account,FsSaveDataRank_Primary,0));
 assert(!validHeader(FsSaveDataSpaceId_User,7,FsSaveDataRank_Primary,0));
 assert(!validHeader(FsSaveDataSpaceId_User,FsSaveDataType_Account,2,0));
 assert(!validHeader(FsSaveDataSpaceId_User,FsSaveDataType_Account,FsSaveDataRank_Primary,65536));
 assert(!validHeader(FsSaveDataSpaceId_User,FsSaveDataType_Account,FsSaveDataRank_Primary,4294967296ULL));
 std::cout<<"save actual handlers: PASS (ambiguity, selectors, unique/no-overwrite backup, SHA256/identity/tree verification, restore guards, commit/read failures, cancellation)\n";
}
