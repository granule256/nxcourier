// NxCourier · Copyright (C) 2026 granule256 · GPL-2.0-only
// 存档备份以 manifest.nxc + data/ + 最后写入的 COMPLETE 构成完整快照。
// 固定缓冲、串行执行；恢复先完整校验，再清理/复制/提交，失败绝不回成功。
#include "nxc_sdk.hpp"
#include "../core/sd.hpp"
#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

namespace {
#ifndef NXC_SAVE_BACKUP_ROOT
#define NXC_SAVE_BACKUP_ROOT "/config/nxc/saves"
#endif
const char* const kBackupRoot = NXC_SAVE_BACKUP_ROOT;
constexpr size_t kPath = 1536;
constexpr int kMaxDepth = 16;
constexpr int kMaxEntries = 4096;
unsigned char g_copy[32768];
unsigned char g_pathHashes[kMaxEntries][32];
bool g_pathDirectories[kMaxEntries];
char g_line[FS_MAX_PATH * 2 + 256];
char g_hexPath[FS_MAX_PATH * 2 + 1];
FsSaveDataInfo g_infos[32];
struct Stats {
    long long bytes;
    int files, dirs, entries;
};
struct ManifestEntry {
    char path[FS_MAX_PATH];
    char hash[65];
    long long size;
    bool dir;
};

bool cancelled(nxc::Reply& r) {
    if (!nxc::host::operationCancelled()) return false;
    r.fail(499, "operation-cancelled: 备份不完整或恢复未提交，请保留原备份并检查目标");
    return true;
}

bool join(const char* base, const char* path, char* out, size_t cap) {
    int n = std::snprintf(out, cap, "%s%s%s", base, (*path == '/' ? "" : "/"), path);
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool validPath(const char* path, bool relativeToSave = false) {
    if (!path || path[0] != '/' || std::strchr(path, ':') || std::strlen(path) >= (relativeToSave ? FS_MAX_PATH : kPath)) return false;
    if (std::strcmp(path,"/")==0) return relativeToSave;
    const char* p = path+1;
    while (*p) {
        const char* end = std::strchr(p,'/');
        size_t n = end ? static_cast<size_t>(end-p) : std::strlen(p);
        if (!n || (n==1 && p[0]=='.') || (n==2 && p[0]=='.' && p[1]=='.')) return false;
        if (!end) return true;
        p=end+1;
        if (!*p) return false;
    }
    return false;
}

bool backupPath(const char* path) {
    if (!validPath(path)) return false;
    size_t n=std::strlen(kBackupRoot);
    return std::strncmp(path,kBackupRoot,n)==0 && path[n]=='/' && path[n+1];
}
// EEXIST 只有确实是目录才通过；禁止宿主上的符号链接重定向备份路径。

bool ensureDirs(const char* path) {
    char tmp[kPath];
    if (!validPath(path)) return false;
    std::strcpy(tmp,path);
    size_t n=std::strlen(tmp);
    for(size_t i=1;i<=n;++i) if(tmp[i]=='/' || !tmp[i]) {
        char keep=tmp[i];
        tmp[i]=0;
        struct stat st{};
        bool ok=(::mkdir(tmp,0755)==0) || (errno==EEXIST && ::lstat(tmp,&st)==0 && S_ISDIR(st.st_mode));
        tmp[i]=keep;
        if(!ok)return false;
    }
    return true;
}

bool regular(const char* p, bool dir=false) {
    struct stat s{};
    return ::lstat(p,&s)==0 && (dir?S_ISDIR(s.st_mode):S_ISREG(s.st_mode));
}

void hashText(const unsigned char* digest, char* text) {
    static const char hex[]="0123456789abcdef";
    for(int i=0;i<32;++i) {
        text[2*i]=hex[digest[i]>>4];
        text[2*i+1]=hex[digest[i]&15];
    }
    text[64]=0;
}

void finishHash(Sha256Context& c,char* text) {
    unsigned char d[32];
    sha256ContextGetHash(&c,d);
    hashText(d,text);
}

bool hashFile(const char* path,char* hash,long long* bytes,nxc::Reply& r) {
    if(!regular(path))return false;
    FILE* f=nxc::sd::open(path,"rb");
    if(!f)return false;
    Sha256Context c;
    sha256ContextCreate(&c);
    *bytes=0;
    bool ok=true;
    for(;;) {
        if(cancelled(r)) {
            ok=false;
            break;
        }
        size_t n=std::fread(g_copy,1,sizeof(g_copy),f);
        if(n) {
            sha256ContextUpdate(&c,g_copy,n);
            *bytes+=static_cast<long long>(n);
        }
        if(n<sizeof(g_copy)) {
            if(std::ferror(f))ok=false;
            break;
        }
    }
    if(std::fclose(f)!=0)ok=false;
    if(ok)finishHash(c,hash);
    return ok;
}

bool closeOutput(FILE* f) {
    bool ok=std::fflush(f)==0;
    if(std::fclose(f)!=0)ok=false;
    return ok;
}

bool parseSpace(const char* name,FsSaveDataSpaceId* out) {
    if(!name||!strcmp(name,"user")) {
        *out=FsSaveDataSpaceId_User;
        return true;
    }
    if(!strcmp(name,"system")) {
        *out=FsSaveDataSpaceId_System;
        return true;
    }
    if(!strcmp(name,"sduser")) {
        *out=FsSaveDataSpaceId_SdUser;
        return true;
    }
    if(!strcmp(name,"sdsystem")) {
        *out=FsSaveDataSpaceId_SdSystem;
        return true;
    }
    if(!strcmp(name,"all")) {
        *out=FsSaveDataSpaceId_All;
        return true;
    }
    return false;
}

bool parseUid(const char* text,AccountUid* uid) {
    if(!text||strlen(text)!=32)return false;
    u64 v[2]={};
    for(int i=0;i<32;++i) {
        int d=nxc::codec::hexDigit(text[i]);
        if(d<0)return false;
        v[i/16]=(v[i/16]<<4)|static_cast<unsigned>(d);
    }
    uid->uid[1]=v[0];
    uid->uid[0]=v[1];
    return true;
}

bool selectSave(const nxc::Args& a,FsSaveDataInfo* out,nxc::Reply& r) {
    u64 app=0,id=0;
    AccountUid uid{};
    const bool hasId=a.has("save_id"),hasUid=a.has("uid");
    if(!nxc::util::parseU64(a.get("app"),&app)||!app) {
        r.fail(400,"app-required: 使用save.list输出的app");
        return false;
    }
    if((hasId&&(!nxc::util::parseU64(a.get("save_id"),&id)||!id)) || (hasUid&&!parseUid(a.get("uid"),&uid))) {
        r.fail(400,"save_id/uid-invalid");
        return false;
    }
    FsSaveDataSpaceId requested=FsSaveDataSpaceId_All;
    if(a.has("space")&&!parseSpace(a.get("space"),&requested)) {
        r.fail(400,"space-invalid");
        return false;
    }
    const FsSaveDataSpaceId spaces[]= {
        FsSaveDataSpaceId_User,FsSaveDataSpaceId_SdUser,FsSaveDataSpaceId_System,FsSaveDataSpaceId_SdSystem
    };
    int found=0;
    for(auto sp:spaces) {
        if(requested!=FsSaveDataSpaceId_All&&requested!=sp)continue;
        FsSaveDataInfoReader rd;
        if(R_FAILED(fsOpenSaveDataInfoReader(&rd,sp))) {
            r.fail(500,"open-info-reader-failed");
            return false;
        }
        bool ok=true;
        for(;;) {
            if(cancelled(r)) {
                ok=false;
                break;
            }
            s64 count=0;
            if(R_FAILED(fsSaveDataInfoReaderRead(&rd,g_infos,32,&count))||count<0||count>32) {
                ok=false;
                break;
            }
            if(!count)break;
            for(s64 i=0;i<count;++i) {
                auto& x=g_infos[i];
                if(x.application_id!=app||(hasId&&x.save_data_id!=id)||(hasUid&&(x.uid.uid[0]!=uid.uid[0]||x.uid.uid[1]!=uid.uid[1])))continue;
                *out=x;
                ++found;
            }
        }
        fsSaveDataInfoReaderClose(&rd);
        if(!ok) {
            if(!r.hasError())r.fail(500,"read-info-failed");
            return false;
        }
    }
    if(found!=1) {
        r.fail(found?409:404,found?"ambiguous-save: 用uid/save_id/space明确选择一份存档":"save-not-found");
        return false;
    }
    return true;
}

FsSaveDataAttribute attribute(const FsSaveDataInfo& i) {
    FsSaveDataAttribute a{};
    a.application_id=i.application_id;
    a.uid=i.uid;
    a.system_save_data_id=i.system_save_data_id;
    a.save_data_type=i.save_data_type;
    a.save_data_rank=i.save_data_rank;
    a.save_data_index=i.save_data_index;
    return a;
}

void emitInfo(nxc::Reply& r,const FsSaveDataInfo& i) {
    r.next();
    r.kvf("app","0x%016llX",(unsigned long long)i.application_id);
    r.kvf("save_id","0x%016llX",(unsigned long long)i.save_data_id);
    r.kvf("uid","%016llX%016llX",(unsigned long long)i.uid.uid[1],(unsigned long long)i.uid.uid[0]);
    r.kvi("space",i.save_data_space_id);
    r.kvi("type",i.save_data_type);
    r.kvi("size",i.size);
}

void cmdList(const nxc::Args& a, nxc::Reply& r) {
    FsSaveDataSpaceId space;
    if (!parseSpace(a.get("space"), &space)) {
        r.fail(400, "space-invalid");
        return;
    }
    const long long limit = a.getInt("count", 20);
    const long long offset = a.getInt("offset", 0);
    if (limit < 1 || limit > 200 || offset < 0) {
        r.fail(400, "pagination-invalid: offset>=0 count=1..200");
        return;
    }
    u64 app = 0;
    if (a.has("app") && (!nxc::util::parseU64(a.get("app"), &app) || !app)) {
        r.fail(400, "app-invalid");
        return;
    }
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, space))) {
        r.fail(500, "open-info-reader-failed");
        return;
    }
    int emitted = 0;
    long long matched = 0;
    bool ok = true, truncated = false;
    while (!truncated) {
        if (cancelled(r)) {
            ok = false;
            break;
        }
        s64 count = 0;
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, g_infos, 32, &count)) || count < 0 || count > 32) {
            ok = false;
            break;
        }
        if (!count) break;
        for (s64 i = 0; i < count; ++i) {
            if (app && g_infos[i].application_id != app) continue;
            if (matched++ < offset) continue;
            if (emitted >= limit) {
                truncated = true;
                break;
            }
            emitInfo(r, g_infos[i]);
            ++emitted;
        }
    }
    fsSaveDataInfoReaderClose(&reader);
    if (!ok) {
        if (!r.hasError()) r.fail(500, "read-info-failed");
        return;
    }
    r.next();
    r.kvi("count", emitted);
    r.kvi("offset", offset);
    r.kvi("next_offset", truncated ? offset + emitted : -1);
    r.kvi("truncated", truncated ? 1 : 0);
}

// 路径用十六进制编码，空格、换行、UTF-8 文件名不会破坏清单格式。

bool writeEntry(FILE* manifest,const char* path,bool dir,long long size,const char* hash,Stats& s) {
    if(++s.entries>kMaxEntries||!validPath(path,true))return false;
    const size_t n=strlen(path);
    static const char hex[]="0123456789abcdef";
    for(size_t i=0;i<n;++i) {
        unsigned char c=path[i];
        g_hexPath[2*i]=hex[c>>4];
        g_hexPath[2*i+1]=hex[c&15];
    }
    g_hexPath[2*n]=0;
    return std::fprintf(manifest,"%c %lld %s %s\n",dir?'D':'F',size,dir?"-":hash,g_hexPath)>0;
}

bool copyFileOut(FsFileSystem* fs,const char* path,const char* data,FILE* manifest,Stats& s,nxc::Reply& r) {
    FsFile in;
    if(R_FAILED(fsFsOpenFile(fs,path,FsOpenMode_Read,&in)))return false;
    s64 size=0;
    bool ok=R_SUCCEEDED(fsFileGetSize(&in,&size))&&size>=0;
    char dest[kPath];
    if(!join(data,path,dest,sizeof(dest)))ok=false;
    FILE* out=ok?nxc::sd::open(dest,"wb"):nullptr;
    if(!out)ok=false;
    Sha256Context hash;
    sha256ContextCreate(&hash);
    s64 off=0;
    while(ok&&off<size) {
        if(cancelled(r)) {
            ok=false;
            break;
        }
        u64 want=static_cast<u64>((size-off)<static_cast<s64>(sizeof(g_copy))?(size-off):sizeof(g_copy)),got=0;
        if(R_FAILED(fsFileRead(&in,off,g_copy,want,0,&got))||!got||got>want) {
            ok=false;
            break;
        }
        sha256ContextUpdate(&hash,g_copy,got);
        if(fwrite(g_copy,1,got,out)!=got) {
            ok=false;
            break;
        }
        off+=got;
    }
    if(out&&!closeOutput(out))ok=false;
    fsFileClose(&in);
    char digest[65];
    finishHash(hash,digest);
    if(ok)ok=writeEntry(manifest,path,false,size,digest,s);
    if(ok) {
        ++s.files;
        s.bytes+=size;
    }
    return ok;
}

bool backupTree(FsFileSystem* fs,const char* path,const char* data,FILE* manifest,Stats& s,nxc::Reply& r,int depth) {
    if(depth>kMaxDepth||cancelled(r))return false;
    char target[kPath];
    if(!join(data,path,target,sizeof(target)))return false;
    if(strcmp(path,"/")&&::mkdir(target,0755)!=0)return false;
    if(!writeEntry(manifest,path,true,0,"-",s))return false;
    ++s.dirs;
    FsDir dir;
    if(R_FAILED(fsFsOpenDirectory(fs,path,FsDirOpenMode_ReadDirs|FsDirOpenMode_ReadFiles,&dir)))return false;
    bool ok=true;
    for(;;) {
        if(cancelled(r)) {
            ok=false;
            break;
        }
        FsDirectoryEntry e{};
        s64 n=0;
        if(R_FAILED(fsDirRead(&dir,&n,1,&e))||n<0||n>1) {
            ok=false;
            break;
        }
        if(!n)break;
        if(!memchr(e.name,0,sizeof(e.name))||strchr(e.name,'/')||!strcmp(e.name,".")||!strcmp(e.name,"..")) {
            ok=false;
            break;
        }
        char child[FS_MAX_PATH];
        int len=snprintf(child,sizeof(child),"%s%s%s",path,strcmp(path,"/")?"/":"",e.name);
        if(len<0||size_t(len)>=sizeof(child)) {
            ok=false;
            break;
        }
        if(e.type==FsDirEntryType_Dir)ok=backupTree(fs,child,data,manifest,s,r,depth+1);
        else if(e.type==FsDirEntryType_File)ok=copyFileOut(fs,child,data,manifest,s,r);
        else ok=false;
        if(!ok)break;
    }
    fsDirClose(&dir);
    return ok;
}

bool writeHeader(FILE* f,const FsSaveDataInfo& i) {
    return fprintf(f,"NXC_SAVE_V1 %016llX %016llX %016llX%016llX %u %u %u %u %016llX\n",(unsigned long long)i.application_id,(unsigned long long)i.save_data_id,(unsigned long long)i.uid.uid[1],(unsigned long long)i.uid.uid[0],unsigned(i.save_data_space_id),unsigned(i.save_data_type),unsigned(i.save_data_rank),unsigned(i.save_data_index),(unsigned long long)i.system_save_data_id)>0;
}

bool appStopped(u64 app, nxc::Reply& r);

void cmdBackup(const nxc::Args& a,nxc::Reply& r) {
    FsSaveDataInfo info{};
    if(!selectSave(a,&info,r))return;
    if (!appStopped(info.application_id, r)) return;
    char root[kPath];
    const char* to=a.get("to");
    if(to) {
        if(!backupPath(to)) {
            r.fail(400,"to-must-be-new-directory-under-backup-root");
            return;
        }
        strcpy(root,to);
    }
    else {
        int n=snprintf(root,sizeof(root),"%s/%016llX-%016llX-%016llX-%016llX",kBackupRoot,(unsigned long long)info.application_id,(unsigned long long)info.save_data_id,(unsigned long long)info.uid.uid[0],(unsigned long long)armGetSystemTick());
        if(n<0||size_t(n)>=sizeof(root)) {
            r.fail(400,"path-too-long");
            return;
        }
    }
    char parent[kPath];
    strcpy(parent,root);
    *strrchr(parent,'/')=0;
    if(!ensureDirs(parent)) {
        r.fail(500,"backup-parent-unavailable");
        return;
    }
    if(::mkdir(root,0755)!=0) {
        r.fail(errno==EEXIST?409:500,"backup-directory-must-not-exist: 不覆盖已有备份");
        return;
    }
    char data[kPath],manifest[kPath],complete[kPath];
    if(!join(root,"data",data,sizeof(data))||!join(root,"manifest.nxc",manifest,sizeof(manifest))||!join(root,"COMPLETE",complete,sizeof(complete))||::mkdir(data,0755)!=0) {
        r.fail(500,"create-backup-layout-failed");
        return;
    }
    FsFileSystem fs;
    auto attr=attribute(info);
    if(R_FAILED(fsOpenReadOnlySaveDataFileSystem(&fs,static_cast<FsSaveDataSpaceId>(info.save_data_space_id),&attr))) {
        r.fail(500,"mount-readonly-failed");
        return;
    }
    FILE* out=nxc::sd::open(manifest,"wb");
    Stats s{};
    bool ok=out&&writeHeader(out,info)&&backupTree(&fs,"/",data,out,s,r,0);
    if(out&&!closeOutput(out))ok=false;
    fsFsClose(&fs);
    char digest[65];
    long long size=0;
    if(ok)ok=hashFile(manifest,digest,&size,r);
    if(ok) {
        FILE* mark=nxc::sd::open(complete,"wb");
        ok=mark!=nullptr;
        if(mark) {
            if(fprintf(mark,"%s\n",digest)!=65)ok=false;
            if(!closeOutput(mark))ok=false;
        }
        if(!ok)::remove(complete);
    }
    if(!ok) {
        if(!r.hasError())r.fail(500,"backup-incomplete: 未生成有效完成标志，禁止恢复");
        return;
    }
    emitInfo(r,info);
    r.kv("to",root);
    r.kvi("files",s.files);
    r.kvi("dirs",s.dirs);
    r.kvi("bytes",s.bytes);
    r.kv("result","ok");
}
// 一行至多为 2*FS_MAX_PATH + 哈希；超长、截断、尾随字段均拒绝。

bool line(FILE* f) {
    if(!fgets(g_line,sizeof(g_line),f))return false;
    size_t n=strlen(g_line);
    return n&&g_line[n-1]=='\n';
}

// 只接受工具实际支持的空间；枚举值不连续，不能用 space<=某数 判断。
bool supportedSaveSpace(u64 space) {
    switch (space) {
        case FsSaveDataSpaceId_User:
        case FsSaveDataSpaceId_System:
        case FsSaveDataSpaceId_SdUser:
        case FsSaveDataSpaceId_SdSystem:
            return true;
        default:
            return false;
    }
}

bool supportedSaveType(u64 type) {
    switch (type) {
        case FsSaveDataType_System:
        case FsSaveDataType_Account:
        case FsSaveDataType_Bcat:
        case FsSaveDataType_Device:
        case FsSaveDataType_Temporary:
        case FsSaveDataType_Cache:
        case FsSaveDataType_SystemBcat:
            return true;
        default:
            return false;
    }
}

bool readHeader(FILE* f, FsSaveDataInfo& info) {
    if (!line(f)) return false;
    unsigned long long app, id, hi, lo, sys;
    char spaceText[21], typeText[21], rankText[21], indexText[21];
    u64 space = 0, type = 0, rank = 0, index = 0;
    int end = 0;
    const int fields = sscanf(g_line,
        "NXC_SAVE_V1 %16llx %16llx %16llx%16llx %20s %20s %20s %20s %16llx%n",
        &app, &id, &hi, &lo, spaceText, typeText, rankText, indexText, &sys, &end);
    if (fields != 9 || strcmp(g_line + end, "\n") || !app || !id) return false;
    // 先用有溢出检查的解析器读取，再检查真实枚举和 u16 边界，避免 scanf %u 截断。
    if (!nxc::util::parseU64(spaceText, &space) || !supportedSaveSpace(space) ||
        !nxc::util::parseU64(typeText, &type) || !supportedSaveType(type) ||
        !nxc::util::parseU64(rankText, &rank) ||
        (rank != FsSaveDataRank_Primary && rank != FsSaveDataRank_Secondary) ||
        !nxc::util::parseU64(indexText, &index) || index > UINT16_MAX) return false;
    info = {};
    info.application_id = app;
    info.save_data_id = id;
    info.uid.uid[1] = hi;
    info.uid.uid[0] = lo;
    info.system_save_data_id = sys;
    info.save_data_space_id = static_cast<u8>(space);
    info.save_data_type = static_cast<u8>(type);
    info.save_data_rank = static_cast<u8>(rank);
    info.save_data_index = static_cast<unsigned short>(index);
    return true;
}

bool readEntry(FILE* f,ManifestEntry& e) {
    if(!line(f))return false;
    char kind=0;
    int end=0;
    long long size=-1;
    if(sscanf(g_line,"%c %lld %64s %1538s%n",&kind,&size,e.hash,g_hexPath,&end)!=4||strcmp(g_line+end,"\n")||(kind!='D'&&kind!='F')||size<0)return false;
    const size_t n=strlen(g_hexPath);
    if(!n||n%2||n/2>=sizeof(e.path))return false;
    for(size_t j=0;j<n;j+=2) {
        int hi=nxc::codec::hexDigit(g_hexPath[j]),lo=nxc::codec::hexDigit(g_hexPath[j+1]);
        if(hi<0||lo<0||(hi==0&&lo==0))return false;
        e.path[j/2]=static_cast<char>((hi<<4)|lo);
    }
    e.path[n/2]=0;
    e.dir=kind=='D';
    e.size=size;
    if(!validPath(e.path,true))return false;
    if(e.dir)return size==0&&!strcmp(e.hash,"-");
    if(strlen(e.hash)!=64)return false;
    for(int j=0;j<64;++j)if(nxc::codec::hexDigit(e.hash[j])<0)return false;
    return true;
}
// 检查实际树的类型、深度、数量；清单中的每条路径先经去重且逐项存在验证，数量相等即无漏列/多余文件。

bool countTree(const char* base,int depth,int& count,nxc::Reply& r) {
    if(depth>kMaxDepth||cancelled(r)||!regular(base,true)||++count>kMaxEntries)return false;
    DIR* d=opendir(base);
    if(!d)return false;
    bool ok=true;
    for(;;) {
        errno=0;
        dirent* e=readdir(d);
        if(!e) {
            if(errno)ok=false;
            break;
        }
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char child[kPath];
        if(!join(base,e->d_name,child,sizeof(child))) {
            ok=false;
            break;
        }
        struct stat st{};
        if(lstat(child,&st)!=0) {
            ok=false;
            break;
        }
        if(S_ISDIR(st.st_mode))ok=countTree(child,depth+1,count,r);
        else if(S_ISREG(st.st_mode))ok=(++count<=kMaxEntries);
        else ok=false;
        if(!ok||cancelled(r)) {
            ok=false;
            break;
        }
    }
    if(closedir(d)!=0)ok=false;
    return ok;
}

// 恢复按清单顺序建目录，因此 verify 必须先证明每条路径的父目录已就绪。
bool manifestOrderValid(const ManifestEntry& entry, int previousCount) {
    if (previousCount == 0) return entry.dir && std::strcmp(entry.path, "/") == 0;
    if (std::strcmp(entry.path, "/") == 0) return false;
    const char* slash = std::strrchr(entry.path, '/');
    if (!slash) return false;
    const size_t len = slash == entry.path ? 1 : static_cast<size_t>(slash - entry.path);
    char parent[FS_MAX_PATH];
    if (len >= sizeof(parent)) return false;
    std::memcpy(parent, entry.path, len);
    parent[len] = 0;
    Sha256Context hash;
    sha256ContextCreate(&hash);
    sha256ContextUpdate(&hash, parent, len);
    unsigned char digest[32];
    sha256ContextGetHash(&hash, digest);
    for (int i = 0; i < previousCount; ++i) {
        if (g_pathDirectories[i] && std::memcmp(g_pathHashes[i], digest, sizeof(digest)) == 0) return true;
    }
    return false;
}

bool verifyBackup(const char* root,FsSaveDataInfo& info,Stats& s,nxc::Reply& r) {
    if(!backupPath(root)||!regular(root,true))return false;
    char path[kPath],data[kPath],manifest[kPath],hash[65],expected[66];
    long long bytes=0;
    if(!join(root,"COMPLETE",path,sizeof(path))||!regular(path))return false;
    FILE* mark=nxc::sd::open(path,"rb");
    if(!mark)return false;
    bool ok=fread(expected,1,sizeof(expected),mark)==65;
    if(fclose(mark)!=0)ok=false;
    if(!ok||expected[64]!='\n')return false;
    expected[64]=0;
    if(!join(root,"manifest.nxc",manifest,sizeof(manifest))||!join(root,"data",data,sizeof(data))||!hashFile(manifest,hash,&bytes,r)||strcmp(hash,expected))return false;
    FILE* f=nxc::sd::open(manifest,"rb");
    if(!f)return false;
    ok=readHeader(f,info);
    s={};
    while(ok) {
        if(cancelled(r)) {
            ok=false;
            break;
        }
        int ch=fgetc(f);
        if(ch==EOF) {
            if(ferror(f))ok=false;
            break;
        }
        ungetc(ch,f);
        ManifestEntry e{};
        if(s.entries>=kMaxEntries||!readEntry(f,e)||!manifestOrderValid(e,s.entries)) {
            ok=false;
            break;
        }
        Sha256Context c;
        sha256ContextCreate(&c);
        sha256ContextUpdate(&c,e.path,strlen(e.path));
        unsigned char digest[32];
        sha256ContextGetHash(&c,digest);
        for(int j=0;j<s.entries;++j)if(!memcmp(g_pathHashes[j],digest,32)) {
            ok=false;
            break;
        }
        if(!ok)break;
        g_pathDirectories[s.entries] = e.dir;
        memcpy(g_pathHashes[s.entries++],digest,32);
        if(!join(data,e.path,path,sizeof(path))) {
            ok=false;
            break;
        }
        if(e.dir) {
            if(!regular(path,true)) {
                ok=false;
                break;
            }
            ++s.dirs;
        }
        else {
            long long n=0;
            if(!hashFile(path,hash,&n,r)||n!=e.size||strcmp(hash,e.hash)) {
                ok=false;
                break;
            }
            ++s.files;
            if(s.bytes>LLONG_MAX-n) {
                ok=false;
                break;
            }
            s.bytes+=n;
        }
    }
    if(fclose(f)!=0)ok=false;
    int actual=0;
    if(ok)ok=countTree(data,0,actual,r)&&actual==s.entries;
    return ok;
}

void cmdVerify(const nxc::Args& a,nxc::Reply& r) {
    FsSaveDataInfo i{};
    Stats s{};
    if(!verifyBackup(a.get("from"),i,s,r)) {
        if(!r.hasError())r.fail(422,"backup-invalid: 完成标志/清单/身份/文件长度或SHA256不匹配");
        return;
    }
    emitInfo(r,i);
    r.kvi("files",s.files);
    r.kvi("dirs",s.dirs);
    r.kvi("bytes",s.bytes);
    r.kv("result","verified");
}

bool sameIdentity(const FsSaveDataInfo& a,const FsSaveDataInfo& b) {
    return a.application_id==b.application_id&&a.save_data_id==b.save_data_id&&a.uid.uid[0]==b.uid.uid[0]&&a.uid.uid[1]==b.uid.uid[1]&&a.save_data_space_id==b.save_data_space_id&&a.save_data_type==b.save_data_type&&a.save_data_rank==b.save_data_rank&&a.save_data_index==b.save_data_index&&a.system_save_data_id==b.system_save_data_id;
}

bool appStopped(u64 app,nxc::Reply& r) {
    if(R_FAILED(pmdmntInitialize())) {
        r.fail(500,"cannot-check-running-app");
        return false;
    }
    static u64 pids[256];
    s32 n=0;
    bool ok=R_SUCCEEDED(svcGetProcessList(&n,pids,256))&&n>=0&&n<=256;
    for(s32 j=0;ok&&j<n;++j) {
        u64 id=0;
        if(R_FAILED(pmdmntGetProgramId(&id,pids[j]))) {
            ok=false;
            break;
        }
        if(id==app) {
            pmdmntExit();
            r.fail(409,"target-app-running: 先完全关闭应用再恢复");
            return false;
        }
    }
    pmdmntExit();
    if(!ok)r.fail(500,"cannot-check-running-app");
    return ok;
}

bool restoreFile(FsFileSystem* fs,const char* data,const ManifestEntry& e,nxc::Reply& r) {
    char src[kPath];
    if(!join(data,e.path,src,sizeof(src)))return false;
    FILE* in=nxc::sd::open(src,"rb");
    if(!in)return false;
    bool ok=R_SUCCEEDED(fsFsCreateFile(fs,e.path,e.size,0));
    FsFile out;
    bool opened=ok&&R_SUCCEEDED(fsFsOpenFile(fs,e.path,FsOpenMode_Write,&out));
    if(!opened)ok=false;
    long long off=0;
    Sha256Context hash;
    sha256ContextCreate(&hash);
    while(ok&&off<e.size) {
        if(cancelled(r)) {
            ok=false;
            break;
        }
        size_t want=static_cast<size_t>((e.size-off)<static_cast<long long>(sizeof(g_copy))?(e.size-off):sizeof(g_copy));
        size_t got=fread(g_copy,1,want,in);
        if(got!=want||R_FAILED(fsFileWrite(&out,off,g_copy,got,0))) {
            ok=false;
            break;
        }
        sha256ContextUpdate(&hash,g_copy,got);
        off+=got;
    }
    if(ok&&(fgetc(in)!=EOF||ferror(in)))ok=false;
    char digest[65];
    finishHash(hash,digest);
    if(strcmp(digest,e.hash))ok=false;
    if(fclose(in)!=0)ok=false;
    if(opened) {
        if(R_FAILED(fsFileFlush(&out)))ok=false;
        fsFileClose(&out);
    }
    return ok;
}

void cmdRestore(const nxc::Args& a,nxc::Reply& r) {
    if(!a.get("confirm")||strcmp(a.get("confirm"),"RESTORE_SAVE")) {
        r.fail(403,"confirm-required: confirm=RESTORE_SAVE，将完整替换目标存档");
        return;
    }
    FsSaveDataInfo target{},backup{};
    Stats s{};
    if(!selectSave(a,&target,r))return;
    const char* from=a.get("from");
    if(!verifyBackup(from,backup,s,r)) {
        if(!r.hasError())r.fail(422,"backup-invalid: 恢复前完整校验失败，未写目标");
        return;
    }
    if(!sameIdentity(target,backup)) {
        r.fail(409,"backup-target-identity-mismatch: app/save_id/uid/space/type/rank/index必须完全一致");
        return;
    }
    if(!appStopped(target.application_id,r)||cancelled(r))return;
    char manifest[kPath],data[kPath];
    if(!join(from,"manifest.nxc",manifest,sizeof(manifest))||!join(from,"data",data,sizeof(data))) {
        r.fail(400,"path-too-long");
        return;
    }
    FILE* f=nxc::sd::open(manifest,"rb");
    if(!f) {
        r.fail(500,"open-manifest-failed");
        return;
    }
    FsSaveDataInfo header{};
    bool ok=readHeader(f,header)&&sameIdentity(header,backup);
    FsFileSystem fs;
    auto attr=attribute(target);
    bool mounted=ok&&R_SUCCEEDED(fsOpenSaveDataFileSystem(&fs,static_cast<FsSaveDataSpaceId>(target.save_data_space_id),&attr));
    if(!mounted)ok=false;
    // 至此清单与所有文件已完整验证，且应用已关闭；从这里才允许目标写入。
    if(ok)ok=R_SUCCEEDED(fsFsCleanDirectoryRecursively(&fs,"/"));
    for(int j=0;ok&&j<s.entries;++j) {
        if(cancelled(r)) {
            ok=false;
            break;
        }
        ManifestEntry e{};
        if(!readEntry(f,e)) {
            ok=false;
            break;
        }
        if(e.dir) {
            if(strcmp(e.path,"/"))ok=R_SUCCEEDED(fsFsCreateDirectory(&fs,e.path));
        }
        else ok=restoreFile(&fs,data,e,r);
    }
    if(fclose(f)!=0)ok=false;
    if(ok&&cancelled(r))ok=false;
    if(ok)ok=R_SUCCEEDED(fsFsCommit(&fs));
    if(mounted)fsFsClose(&fs);
    if(!ok) {
        if(!r.hasError())r.fail(500,"restore-failed-or-commit-failed: 不能确认恢复成功，保留备份并检查目标存档");
        return;
    }
    emitInfo(r,target);
    r.kvi("files",s.files);
    r.kvi("bytes",s.bytes);
    r.kv("result","restored");
}

const nxc::Param kListParams[] = {
    {"space", "user|system|sduser|sdsystem|all", false, "user", "存档空间"},
    {"app", "title-id", false, nullptr, "可选应用ID过滤"},
    {"count", "int", false, "20", "1..200"},
    {"offset", "int", false, "0", "跳过过滤后的存档条数，目录变化请重新枚举"},
};
const nxc::Param kBackupParams[] = {
    {"app", "title-id", true, nullptr, "应用ID，认0x"},
    {"uid", "hex32", false, nullptr, "save.list输出的32位用户ID"},
    {"save_id", "id", false, nullptr, "save.list输出的存档ID"},
    {"space", "str", false, "all", "可选空间选择"},
    {"to", "str", false, nullptr, "必须是/config/nxc/saves下不存在的新目录；默认独立唯一目录"},
};
const nxc::Param kVerifyParams[] = {
    {"from", "str", true, nullptr, "备份目录，必须含manifest.nxc、data、COMPLETE"},
};
const nxc::Param kRestoreParams[] = {
    {"app", "title-id", true, nullptr, "目标应用ID"},
    {"uid", "hex32", false, nullptr, "目标用户ID"},
    {"save_id", "id", false, nullptr, "目标存档ID"},
    {"space", "str", false, "all", "目标空间"},
    {"from", "str", true, nullptr, "完整备份目录"},
    {"confirm", "str", true, nullptr, "必须为RESTORE_SAVE；将完整替换目标存档"},
};
const nxc::MethodInfo kInfoList = {
    kListParams, 4, "read",
    "枚举存档。save.list [space=all] [app=0x...] [offset=0] [count=20]。\n"
    "输出app/save_id/uid/space/type/size，供明确选择；末行count/offset/next_offset/truncated。",
};
const nxc::MethodInfo kInfoBackup = {
    kBackupParams, 5, "write",
    "save.backup app=0x... [uid=<32位十六进制>] [save_id=0x...] [space=user] "
    "[to=/config/nxc/saves/new]\n"
    "多匹配409，必须细化选择；默认新建唯一目录，显式目录存在也409，绝不覆盖。\n"
    "data保存目录树，manifest.nxc记录身份及每条路径/长度/SHA256，COMPLETE最后写入。\n"
    "先关闭目标应用再备份；正在运行或无法确认关闭时拒绝创建备份目录。\n"
    "任意失败回ERR，未完成备份不可恢复。空存档合法；上限4096条、16层。",
};
const nxc::MethodInfo kInfoVerify = {
    kVerifyParams, 1, "read",
    "save.verify from=/config/nxc/saves/<backup>\n"
    "验证完成标志与清单哈希、全部文件路径/长度/SHA256和完整目录树；\n"
    "拒绝多余文件、重复路径、符号链接；根目录必须首条出现，父目录必须先于子项。返回result=verified。",
};
const nxc::MethodInfo kInfoRestore = {
    kRestoreParams, 6, "write",
    "save.restore app=0x... [uid=...] [save_id=0x...] "
    "from=/config/nxc/saves/<backup> confirm=RESTORE_SAVE\n"
    "警告：完整替换目标存档，删除目标额外文件。\n"
    "先完整验证备份和app/save_id/uid/space/type/rank/index身份，拒绝正在运行的目标应用，\n"
    "写后检查flush和commit。任意失败回ERR且不能认定恢复成功；保留原备份。\n"
    "建议恢复前再备份目标；提交失败或恢复中断须检查目标。\n"
    "本实现仅经宿主测试与交叉构建，尚未真机验收。",
};
const nxc::Method kMethods[] = {
    {"list", "枚举存档及精确选择身份", cmdList, &kInfoList},
    {"backup", "独立目录备份、清单与SHA256", cmdBackup, &kInfoBackup},
    {"verify", "验证完整备份与全部文件", cmdVerify, &kInfoVerify},
    {"restore", "确认口令后校验并恢复同身份存档", cmdRestore, &kInfoRestore},
};
}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_save, "save", "存档枚举、备份、完整性验证与恢复", kMethods,
    "先save.list取得app/uid/save_id，再save.backup到独立新目录；save.verify验证完整性。\n"
    "save.restore需要confirm=RESTORE_SAVE、相同存档身份及关闭目标应用，写前验证全部文件，写后检查commit。\n"
    "备份最多4096条目录/文件、16层；不覆盖旧备份，只有COMPLETE有效才可恢复。"
);
