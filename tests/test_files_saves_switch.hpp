#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <filesystem>
#include <vector>
#include <string>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <cassert>
#include <cerrno>
#endif
using u64=uint64_t;using u32=uint32_t;using u8=uint8_t;using s64=int64_t;using s32=int32_t;using Result=unsigned;
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
#define FS_MAX_PATH 769
struct AccountUid{u64 uid[2];};
// 与固定 devkitPro 镜像 libnx/services/fs.h 保持数值一致；不能为桩重新编号。
enum FsSaveDataSpaceId {
    FsSaveDataSpaceId_System=0, FsSaveDataSpaceId_User=1, FsSaveDataSpaceId_SdSystem=2,
    FsSaveDataSpaceId_Temporary=3, FsSaveDataSpaceId_SdUser=4,
    FsSaveDataSpaceId_ProperSystem=100, FsSaveDataSpaceId_SafeMode=101, FsSaveDataSpaceId_All=-1,
};
enum FsSaveDataType {
    FsSaveDataType_System=0, FsSaveDataType_Account=1, FsSaveDataType_Bcat=2,
    FsSaveDataType_Device=3, FsSaveDataType_Temporary=4, FsSaveDataType_Cache=5,
    FsSaveDataType_SystemBcat=6,
};
enum FsSaveDataRank { FsSaveDataRank_Primary=0, FsSaveDataRank_Secondary=1 };
enum FsDirEntryType{FsDirEntryType_Dir=0,FsDirEntryType_File=1};
enum{FsOpenMode_Read=1,FsOpenMode_Write=2,FsDirOpenMode_ReadDirs=1,FsDirOpenMode_ReadFiles=2,FsWriteOption_Flush=1};
struct FsSaveDataInfo{u64 save_data_id;u8 save_data_space_id,save_data_type;AccountUid uid;u64 system_save_data_id,application_id,size;unsigned short save_data_index;u8 save_data_rank;};
struct FsSaveDataAttribute{u64 application_id;AccountUid uid;u64 system_save_data_id;u8 save_data_type,save_data_rank;unsigned short save_data_index;};
struct FsSaveDataInfoReader{size_t offset;int space;};struct FsFileSystem{int unused;};struct FsFile{FILE*f;};struct FsDir{DIR*d;std::string path;};struct FsDirectoryEntry{char name[FS_MAX_PATH];int type;s64 file_size;};
inline std::vector<FsSaveDataInfo> mock_infos;inline std::string mock_save_root;inline bool mock_running=false,mock_commit_fail=false,mock_read_fail=false;inline int mock_writes=0,mock_commits=0;inline bool mock_process_check_fail=false;
inline Result fsOpenSaveDataInfoReader(FsSaveDataInfoReader*r,FsSaveDataSpaceId s){*r={0,s};return 0;}
inline Result fsSaveDataInfoReaderRead(FsSaveDataInfoReader*r,FsSaveDataInfo*b,size_t n,s64*out){*out=0;while(r->offset<mock_infos.size()&&size_t(*out)<n){auto i=mock_infos[r->offset++];if(i.save_data_space_id==r->space||r->space==FsSaveDataSpaceId_All)b[(*out)++]=i;}return 0;}
inline void fsSaveDataInfoReaderClose(FsSaveDataInfoReader*){}
inline Result fsOpenSaveDataFileSystem(FsFileSystem*,FsSaveDataSpaceId,const FsSaveDataAttribute*){return 0;}
inline Result fsOpenReadOnlySaveDataFileSystem(FsFileSystem*,FsSaveDataSpaceId,const FsSaveDataAttribute*){return 0;}
inline void fsFsClose(FsFileSystem*){}
inline std::string realSave(const char*p){return mock_save_root+p;}
inline Result fsFsOpenFile(FsFileSystem*,const char*p,u32 mode,FsFile*f){f->f=fopen(realSave(p).c_str(),mode==FsOpenMode_Read?"rb":"r+b");return f->f?0:1;}
inline Result fsFileGetSize(FsFile*f,s64*n){auto pos=ftell(f->f);fseek(f->f,0,SEEK_END);*n=ftell(f->f);fseek(f->f,pos,SEEK_SET);return 0;}
inline Result fsFileRead(FsFile*f,s64 off,void*b,u64 n,u32,u64*got){if(mock_read_fail)return 1;fseek(f->f,off,SEEK_SET);*got=fread(b,1,n,f->f);return ferror(f->f)?1:0;}
inline Result fsFileWrite(FsFile*f,s64 off,const void*b,u64 n,u32){++mock_writes;fseek(f->f,off,SEEK_SET);return fwrite(b,1,n,f->f)==n?0:1;}
inline Result fsFileFlush(FsFile*f){return fflush(f->f)==0?0:1;}
inline void fsFileClose(FsFile*f){fclose(f->f);}
inline Result fsFsOpenDirectory(FsFileSystem*,const char*p,u32,FsDir*d){d->path=realSave(p);d->d=opendir(d->path.c_str());return d->d?0:1;}
inline Result fsDirRead(FsDir*d,s64*out,size_t n,FsDirectoryEntry*b){*out=0;while(size_t(*out)<n){auto*e=readdir(d->d);if(!e)break;if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;auto&v=b[(*out)++];strcpy(v.name,e->d_name);struct stat st{};stat((d->path+"/"+v.name).c_str(),&st);v.type=S_ISDIR(st.st_mode)?FsDirEntryType_Dir:FsDirEntryType_File;v.file_size=st.st_size;}return 0;}
inline void fsDirClose(FsDir*d){closedir(d->d);}
inline Result fsFsCreateDirectory(FsFileSystem*,const char*p){++mock_writes;return mkdir(realSave(p).c_str(),0755)==0?0:1;}
inline Result fsFsCreateFile(FsFileSystem*,const char*p,s64 n,u32){++mock_writes;auto*f=fopen(realSave(p).c_str(),"wb");if(!f)return 1;int rc=ftruncate(fileno(f),n);fclose(f);return rc==0?0:1;}
inline Result fsFsDeleteFile(FsFileSystem*,const char*p){++mock_writes;return remove(realSave(p).c_str())==0?0:1;}
inline Result fsFsCleanDirectoryRecursively(FsFileSystem*,const char*){++mock_writes;for(auto&e:std::filesystem::directory_iterator(mock_save_root))std::filesystem::remove_all(e.path());return 0;}
inline Result fsFsCommit(FsFileSystem*){++mock_commits;return mock_commit_fail?1:0;}
inline Result pmdmntInitialize(){return mock_process_check_fail?1:0;}inline void pmdmntExit(){}
inline Result svcGetProcessList(s32*n,u64*p,u32){*n=mock_running?1:0;if(*n)p[0]=42;return 0;}
inline Result pmdmntGetProgramId(u64*a,u64){*a=mock_infos[0].application_id;return 0;}
inline u64 armGetSystemTick(){static u64 tick=100;return ++tick;}
#ifdef __APPLE__
using Sha256Context=CC_SHA256_CTX;
inline void sha256ContextCreate(Sha256Context*c){CC_SHA256_Init(c);}
inline void sha256ContextUpdate(Sha256Context*c,const void*b,size_t n){CC_SHA256_Update(c,b,n);}
inline void sha256ContextGetHash(Sha256Context*c,void*b){CC_SHA256_Final((unsigned char*)b,c);}
#else
// Linux 用测试入口已有的 Python hashlib 作独立参考，不要求额外安装密码库头文件。
struct Sha256Context { std::string bytes; };
inline void sha256ContextCreate(Sha256Context* c) { c->bytes.clear(); }
inline void sha256ContextUpdate(Sha256Context* c,const void* b,size_t n) { c->bytes.append(static_cast<const char*>(b),n); }
inline void sha256ContextGetHash(Sha256Context* c,void* out) {
    int input[2], output[2];
    assert(pipe(input)==0 && pipe(output)==0);
    pid_t child=fork(); assert(child>=0);
    if(child==0) {
        dup2(input[0],STDIN_FILENO); dup2(output[1],STDOUT_FILENO);
        close(input[0]);close(input[1]);close(output[0]);close(output[1]);
        execlp("python3","python3","-c","import hashlib,sys;sys.stdout.buffer.write(hashlib.sha256(sys.stdin.buffer.read()).digest())",static_cast<char*>(nullptr));
        _exit(127);
    }
    close(input[0]);close(output[1]);
    size_t offset=0;
    while(offset<c->bytes.size()) { ssize_t n=write(input[1],c->bytes.data()+offset,c->bytes.size()-offset); if(n<0&&errno==EINTR)continue;assert(n>0);offset+=static_cast<size_t>(n); }
    close(input[1]);offset=0;
    while(offset<32) { ssize_t n=read(output[0],static_cast<char*>(out)+offset,32-offset);if(n<0&&errno==EINTR)continue;assert(n>0);offset+=static_cast<size_t>(n); }
    close(output[0]);int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
#endif
