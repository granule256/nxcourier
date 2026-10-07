// 内置工具 · save：**存档的枚举与备份**。
//
// ## 为什么需要它
//
// `mem.write` 能直接改运行中游戏的内存 —— 改错了游戏进度就毁了，
// 而且**存档是"改内存"的下游**：改完往往要存档才生效，那时候已经没有后悔药。
// 所以备份是改内存的**安全前提**。
//
// ## 两个能力
//
// * `save.list`   —— 枚举存档（哪个 title、哪个用户、多大、在哪个空间）
// * `save.backup` —— 把某个 title 的存档**整个目录树**复制到 SD 卡上。
//
// ## 实现要点（libnx 接口）
//
// 枚举：
//     fsOpenSaveDataInfoReader(&reader, space)
//     fsSaveDataInfoReaderRead(&reader, buf, n, &total)   // 循环读
//     fsSaveDataInfoReaderClose(&reader)
// 挂载某个存档（挂上后就像个普通文件系统）：
//     fsOpenSaveDataFileSystem(&fs, space, &FsSaveDataAttribute{
//         application_id, uid, save_data_type, save_data_rank, save_data_index })
// 遍历/读取（注意根路径是 "/"）：
//     fsFsOpenDirectory / fsDirRead / fsDirClose
//     fsFsOpenFile / fsFileGetSize / fsFileRead / fsFileClose
//     fsFsClose
//
// ★ `fs` 服务在 main.cpp 里已经初始化过了（`fsInitialize()` + `fsdevMountSdmc()`），
//   这里不用重复初始化。
// ★ 大缓冲全部放静态（栈只有 64KB），别放栈上。
// ★ 只做**只读地把设备上的存档复制出来**；恢复（写回）风险高，先不做。
#include "nxc_sdk.hpp"
#include "../core/sd.hpp"

#include <dirent.h>
#include <switch.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kBackupRoot = "/config/nxc/saves";
constexpr int kCopyBuf   = 32768;   // 拷贝缓冲
constexpr int kDirBatch  = 24;      // 一次读多少目录项
constexpr int kInfoBatch = 32;      // 一次读多少存档信息
constexpr int kMaxDepth  = 8;

// ---- 静态缓冲
FsDirectoryEntry g_entries[kDirBatch];
FsSaveDataInfo   g_infos[kInfoBatch];
unsigned char    g_copy[kCopyBuf];

struct CopyStat {
    int files;
    int dirs;
    int failed;
    long long bytes;
    // ★★★ 2026-10-06 晚新增：**第一条**失败原因。
    //   动机是一次真实的"最坏失败形态"：`save.backup` 回了
    //   `OK … files=0 dirs=1 bytes=0 failed=4 result=partial`，而目标目录**根本不存在**
    //   ⇒ 零产出、却长得像成功。只报个计数没用，得把**根因那句话**带出来。
    //   只记第一条：第一条才是根因，后面的多半是它的连锁反应。
    char firstErr[96];
};
CopyStat g_stat;

void setFirstErr(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void setFirstErr(const char* fmt, ...) {
    if (g_stat.firstErr[0] != '\0') return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_stat.firstErr, sizeof(g_stat.firstErr), fmt, ap);
    va_end(ap);
}

// 逐级建目录（等价 `mkdir -p`），已存在也算成功。
//
// ★★★ 2026-10-06 晚新增 —— `save.backup` **零产出却回 OK** 的根因就在这。
//   `copyTree()` 只对**最末一级**做一次 `::mkdir`，而父目录 `/config/nxc/saves`
//   **全代码库没有任何地方创建过** ⇒ mkdir 返回 ENOENT、**返回值还被丢掉了**，
//   于是后面每个文件的 `fopen("wb")` 都因"目标路径不存在"失败，最后回一个 `partial`。
//   路径必须是绝对路径（本平台只做 SD 卡，挂载点是固定的）。
bool ensureDirChain(const char* path) {
    if (path == nullptr || path[0] != '/') return false;
    const size_t len = std::strlen(path);
    char buf[512];
    if (len == 0 || len >= sizeof(buf)) return false;
    std::memcpy(buf, path, len + 1);

    for (size_t i = 1; i <= len; ++i) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        const char keep = buf[i];
        buf[i] = '\0';
        const int rc = ::mkdir(buf, 0755);
        const bool ok = (rc == 0) || (errno == EEXIST);
        buf[i] = keep;
        if (!ok) return false;
    }
    return true;
}

bool parseSpace(const char* name, FsSaveDataSpaceId* out) {
    if (name == nullptr || *name == '\0') { *out = FsSaveDataSpaceId_User; return true; }
    if (std::strcmp(name, "user") == 0)     { *out = FsSaveDataSpaceId_User; return true; }
    if (std::strcmp(name, "system") == 0)   { *out = FsSaveDataSpaceId_System; return true; }
    if (std::strcmp(name, "sduser") == 0)   { *out = FsSaveDataSpaceId_SdUser; return true; }
    if (std::strcmp(name, "sdsystem") == 0) { *out = FsSaveDataSpaceId_SdSystem; return true; }
    if (std::strcmp(name, "all") == 0)      { *out = FsSaveDataSpaceId_All; return true; }
    return false;
}

// 把挂载点内的路径拼出来。挂载后的根是 "/"，别拼成 "//x"。
// ★ 返回 false 表示**被截断了** —— 调用方必须放弃这个文件。
//   对复制工具来说，路径被悄悄截断就等于"复制了另一个文件"，比报错危险得多。
bool joinFsPath(const char* base, const char* name, char* out, size_t cap) {
    int n = 0;
    if (std::strcmp(base, "/") == 0) n = std::snprintf(out, cap, "/%s", name);
    else                             n = std::snprintf(out, cap, "%s/%s", base, name);
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool joinSdPath(const char* base, const char* name, char* out, size_t cap) {
    const int n = std::snprintf(out, cap, "%s/%s", base, name);
    return n > 0 && static_cast<size_t>(n) < cap;
}

// 在指定空间里找某个 title 的存档。找到第一个就返回。
bool findSave(FsSaveDataSpaceId space, u64 appId, FsSaveDataInfo* out) {
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, space))) return false;

    bool found = false;
    for (;;) {
        s64 total = 0;
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, g_infos, kInfoBatch, &total))) break;
        if (total <= 0) break;
        for (s64 i = 0; i < total; ++i) {
            if (g_infos[i].application_id == appId) { *out = g_infos[i]; found = true; break; }
        }
        if (found) break;
        if (total < kInfoBatch) break;   // 已经是最后一批
    }
    fsSaveDataInfoReaderClose(&reader);
    return found;
}

// 把一个存档里的文件复制到 SD。
void copyOneFile(FsFileSystem* fs, const char* src, const char* dst) {
    FsFile f;
    if (R_FAILED(fsFsOpenFile(fs, src, FsOpenMode_Read, &f))) {
        setFirstErr("open-src %s", src);
        ++g_stat.failed;
        return;
    }

    s64 size = 0;
    fsFileGetSize(&f, &size);

    FILE* out = nxc::sd::open(dst, "wb");
    if (out == nullptr) {
        nxc::host::log("save: 打不开目标 %s", dst);
        setFirstErr("open-dst %s", dst);      // ★ 这条就是原来那个"零产出"的答案
        fsFileClose(&f);
        ++g_stat.failed;
        return;
    }

    s64 off = 0;
    bool ok = true;
    while (off < size) {
        const s64 want = (size - off < kCopyBuf) ? (size - off) : kCopyBuf;
        u64 got = 0;
        if (R_FAILED(fsFileRead(&f, off, g_copy, static_cast<u64>(want), 0, &got)) || got == 0) {
            ok = false;
            break;
        }
        if (std::fwrite(g_copy, 1, static_cast<size_t>(got), out) != static_cast<size_t>(got)) {
            ok = false;
            break;
        }
        off += static_cast<s64>(got);
    }
    std::fclose(out);
    fsFileClose(&f);

    if (ok) { ++g_stat.files; g_stat.bytes += size; }
    else    {
        ++g_stat.failed;
        setFirstErr("copy-body %s", src);
        nxc::host::log("save: 复制失败 %s", src);
    }
}

// 递归复制整个目录。
void copyTree(FsFileSystem* fs, const char* fsPath, const char* sdPath, int depth) {
    if (depth > kMaxDepth) return;

    // ★★★ 2026-10-06 晚：`dirs` 原来**无条件 ++**，于是"一个目录都没建成"也会报 `dirs=1`
    //   —— 实测那次 `files=0 dirs=1 bytes=0 failed=4` 里的 `dirs=1` 就是这么来的，
    //   它把读的人往"目录建好了、只是文件没复制成"的方向带偏。
    //   现在：**只有真的建成、或本来就存在，才计数**；建不出来就当场记失败并放弃这一棵子树。
    const int mk = ::mkdir(sdPath, 0755);
    if (mk != 0 && errno != EEXIST) {
        ++g_stat.failed;
        setFirstErr("mkdir %s rc=%d", sdPath, mk);
        nxc::host::log("save: 建目录失败 %s rc=%d", sdPath, mk);
        return;
    }
    ++g_stat.dirs;

    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(fs, fsPath,
                                   FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir))) {
        nxc::host::log("save: 打不开目录 %s", fsPath);
        return;
    }

    for (;;) {
        s64 total = 0;
        if (R_FAILED(fsDirRead(&dir, &total, kDirBatch, g_entries))) break;
        if (total <= 0) break;

        for (s64 i = 0; i < total; ++i) {
            const char* name = g_entries[i].name;
            char childFs[FS_MAX_PATH];
            char childSd[FS_MAX_PATH + 256];
            const bool okFs = joinFsPath(fsPath, name, childFs, sizeof(childFs));
            const bool okSd = joinSdPath(sdPath, name, childSd, sizeof(childSd));
            if (!okFs || !okSd) {
                // 路径太长被截断了 —— 宁可记一笔失败，也不要静默复制错文件。
                ++g_stat.failed;
                setFirstErr("path-too-long %.40s", name);
                nxc::host::log("save: 路径过长，跳过 %.40s", name);
                continue;
            }

            if (g_entries[i].type == FsDirEntryType_Dir) {
                copyTree(fs, childFs, childSd, depth + 1);
            } else if (g_entries[i].type == FsDirEntryType_File) {
                copyOneFile(fs, childFs, childSd);
            }
        }
        if (total < kDirBatch) break;
    }
    fsDirClose(&dir);
}

void emitInfo(nxc::Reply& r, const FsSaveDataInfo& info) {
    r.next();
    r.kvf("app", "0x%016llX", static_cast<unsigned long long>(info.application_id));
    r.kvf("save_id", "0x%016llX", static_cast<unsigned long long>(info.save_data_id));
    r.kvi("space", info.save_data_space_id);
    r.kvi("type", info.save_data_type);
    r.kvi("size", static_cast<long long>(info.size));
    r.kvf("uid", "%016llX%016llX",
          static_cast<unsigned long long>(info.uid.uid[1]),
          static_cast<unsigned long long>(info.uid.uid[0]));
}

// ---------------------------------------------------------------- 方法
void cmdList(const nxc::Args& a, nxc::Reply& r) {
    FsSaveDataSpaceId space = FsSaveDataSpaceId_User;
    if (!parseSpace(a.get("space"), &space)) { r.fail(400, "space-invalid"); return; }

    long long want = a.getInt("count", 20);
    // ★★★ 2026-10-06 晚：越界值仍回落默认 20，但**不再静默**（见函数尾部 note_params）。
    bool wantAdjusted = false;
    if (want <= 0 || want > 200) { want = 20; wantAdjusted = true; }

    // ★★★ 2026-10-06 晚：`app` 过滤改用 `util::parseU64`，并且**解析不出来就报错**。
    //   原来用 `getInt` ⇒ `app=0x…` 被读成 0，而 0 在下面的判断里是"**不过滤**"
    //   ⇒ 它把**全部存档**列了出来。看起来"参数生效了"，其实过滤条件被静默丢掉了 ——
    //   所以 `save.list app=` 这个功能**从来没有工作过**（报告里"save.list 能接受 0x"是假象）。
    u64 filterApp = 0;
    const char* filterText = a.get("app");
    if (filterText != nullptr && *filterText != '\0') {
        if (!nxc::util::parseU64(filterText, &filterApp) || filterApp == 0) {
            r.failf(400, "bad-app: 解析不出 title id: %.64s", filterText);
            return;
        }
    }

    FsSaveDataInfoReader reader;
    const Result rc = fsOpenSaveDataInfoReader(&reader, space);
    if (R_FAILED(rc)) {
        r.failf(500, "open-info-reader-failed rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }

    int emitted = 0;
    long long seen = 0;
    for (;;) {
        s64 total = 0;
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, g_infos, kInfoBatch, &total))) break;
        if (total <= 0) break;
        for (s64 i = 0; i < total && emitted < want; ++i) {
            ++seen;
            if (filterApp != 0 && g_infos[i].application_id != filterApp) continue;
            emitInfo(r, g_infos[i]);
            ++emitted;
        }
        if (total < kInfoBatch) break;
        if (emitted >= want) break;
    }
    fsSaveDataInfoReaderClose(&reader);

    r.next();
    r.kvi("count", emitted);
    r.kvi("scanned", seen);
    r.raw("note: space = user(默认) | system | sduser | sdsystem | all");
    r.raw("note: 用 save.backup app=<app 里的 0x...> 把某个存档整棵目录树备份到 SD");
    if (wantAdjusted) {
        r.next();   // ★ 先收尾上一行（raw 行末尾没有换行），否则提示会拼到上一条 note 后面
        r.kv("note_params", "count 越界（允许 1..200），已按默认 20 执行");
    }
}

void cmdBackup(const nxc::Args& a, nxc::Reply& r) {
    const char* appText = a.get("app");
    u64 appId = 0;
    // ★★★ 2026-10-06 晚：这里改用 `util::parseU64`（认 `0x`），**不再用 `getInt`**。
    //   `save.list` 打印 title id 用的就是 `0x%016llX`，而 `getInt` 只认十进制 ⇒
    //   会把 `0x010015100B514000` 读成 0，撞进下面的 `appArg == 0` 分支报 `missing-app`
    //   —— **工具自己打印的格式，工具自己读不进去**（实测就是这么翻的车）。
    if (appText == nullptr || *appText == '\0') {
        // ★ 提示语折进错误消息里：`fail()` 走 ERR 分支时，**之前 raw 写的提示会被丢掉**
        //   （协议里 ERR 不带载荷）。原来那句 `hint:` 从来没到达过客户端。
        r.fail(400, "missing-app: 需要 app=<title id，如 0x010021C000B6A000>（可用 save.list 查看）");
        return;
    }
    if (!nxc::util::parseU64(appText, &appId) || appId == 0) {
        r.failf(400, "bad-app: 解析不出 title id: %.64s（形如 0x010021C000B6A000）", appText);
        return;
    }

    // 在几个空间里找。用户存档通常在 user；但 CFW 上也可能落在 sduser。
    static const FsSaveDataSpaceId kSearch[] = {
        FsSaveDataSpaceId_User, FsSaveDataSpaceId_SdUser,
        FsSaveDataSpaceId_System, FsSaveDataSpaceId_SdSystem,
    };
    FsSaveDataInfo info;
    bool found = false;
    for (FsSaveDataSpaceId sp : kSearch) {
        if (findSave(sp, appId, &info)) { found = true; break; }
    }
    if (!found) { r.fail(404, "no-save-for-that-app"); return; }

    // 目标目录：默认 /config/nxc/saves/<app 的 16 位十六进制>
    char dstRoot[512];
    const char* to = a.get("to");
    if (to != nullptr && *to != '\0') {
        std::snprintf(dstRoot, sizeof(dstRoot), "%s", to);
    } else {
        std::snprintf(dstRoot, sizeof(dstRoot), "%s/%016llX", kBackupRoot,
                      static_cast<unsigned long long>(appId));
    }
    if (std::strncmp(dstRoot, kBackupRoot, std::strlen(kBackupRoot)) != 0) {
        r.fail(403, "to-must-be-under-/config/nxc/saves");
        return;
    }

    FsSaveDataAttribute attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.application_id   = info.application_id;
    attr.uid              = info.uid;
    attr.save_data_type   = info.save_data_type;
    attr.save_data_rank   = info.save_data_rank;
    attr.save_data_index  = info.save_data_index;

    FsFileSystem fs;
    const Result rc = fsOpenSaveDataFileSystem(&fs, static_cast<FsSaveDataSpaceId>(info.save_data_space_id), &attr);
    if (R_FAILED(rc)) {
        r.failf(500, "mount-failed rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }

    // ★★★ 2026-10-06 晚：**先把父目录链建出来**。
    //   原来这里直接 `copyTree()`，而 `copyTree` 只 mkdir 最末一级 ⇒ 因为
    //   `/config/nxc/saves` 从没被创建过，一级都建不成 ⇒ 后面每个文件都写不进去
    //   ⇒ 最后回 `OK … files=0 failed=4`（**零产出却回成功**，最坏的一种失败形态）。
    if (!ensureDirChain(kBackupRoot)) {
        fsFsClose(&fs);
        r.failf(500, "backup-root-unavailable: 建不出 %s", kBackupRoot);
        return;
    }

    nxc::host::breadcrumb("save.backup app=0x%llX -> %s",
                          static_cast<unsigned long long>(appId), dstRoot);

    g_stat = CopyStat{};
    copyTree(&fs, "/", dstRoot, 0);
    fsFsClose(&fs);

    // ★★★ 2026-10-06 晚：**一个文件都没复制出来 = 失败**，不再回"看起来成功"的 partial。
    //   要求：调用方一眼就能判死，并且能看到**根因那句话**（firstErr）。
    if (g_stat.files == 0) {
        r.failf(500, "backup-produced-nothing files=0 dirs=%d failed=%d first=%.60s",
                g_stat.dirs, g_stat.failed,
                g_stat.firstErr[0] ? g_stat.firstErr : "(无)");
        return;
    }

    r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
    r.kv("to", dstRoot);
    r.kvi("files", g_stat.files);
    r.kvi("dirs", g_stat.dirs);
    r.kvi("bytes", g_stat.bytes);
    r.kvi("failed", g_stat.failed);
    r.kv("result", g_stat.failed == 0 ? "ok" : "partial");
    if (g_stat.failed != 0) {
        // ★ 把第一条原因也带出来 —— 只给个 `failed=4` 等于让人去猜（原来连猜的线索都没有）。
        r.kv("first", g_stat.firstErr[0] ? g_stat.firstErr : "(未记录)");
        r.raw("★ 有失败：上面 first= 是第一条原因；failed= 非 0 就别当成功");
    }
    r.raw("next: 用 fs.ls / fs.read 把备份取走；或让 PC 侧的取文件工具拉回本地");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
//
// ★★ 规矩：**这个工具的全部知识都写在本文件里** —— 核心不认识 `save` 是什么，
//    `tools.list` / `tools.doc` 只是把这里的内容汇总出去。**AI 就是靠这些字学会用它。**
//    ⇒ 新增/改动方法时，把参数表与教程一起改（别只改实现）。
namespace {

const nxc::Param kListParams[] = {
    {"space", "user|system|sduser|sdsystem|all", false, "user",
     "存档空间：user(默认) | system | sduser | sdsystem | all"},
    {"app",   "title-id(0x...)", false, nullptr,
     "只列这个 title 的存档（十六进制、认 0x；值就用本方法输出里的 app=）"},
    {"count", "int", false, "20",
     "最多几条（允许 1..200；越界回落默认 20 并在回包里提示）"},
};
const nxc::MethodInfo kInfoList = {
    kListParams, 3, "read",
    "枚举设备上的存档（只读）。\n"
    "作用: 看有哪些存档、属于哪个 title／用户、多大、在哪个空间。\n"
    "参数:\n"
    "  space = user(默认)|system|sduser|sdsystem|all —— 存档所在空间。\n"
    "  app   = title id 过滤，十六进制并认 0x（值就用本方法输出里的 app=）。\n"
    "          ★ 解析不出会直接报 400，不会静默变成「不过滤」。\n"
    "  count = 最多几条，默认 20，允许 1..200；越界回落 20 并在回包里提示。\n"
    "示例:\n"
    "  save.list\n"
    "  save.list space=all count=50\n"
    "  save.list app=0x010015100B514000\n"
    "注意:\n"
    "  ★ app 支持 0x 十六进制 —— 直接把本方法打印的 app= 复制回来即可。\n"
    "  ★ 只列不读：要看内容或做备份，请用 save.backup。\n"
    "返回:\n"
    "  每个存档一行 app= save_id= space= type= size= uid=；\n"
    "  末行 count=<实际列出数> scanned=<扫描到的总数>，并附 note 提示。\n"
    "相关: save.backup\n",
};
const nxc::Param kBackupParams[] = {
    {"app", "title-id(0x...)", true, nullptr,
     "要备份的 title id（十六进制、认 0x，如 0x010021C000B6A000）"},
    {"to",  "str", false, nullptr,
     "目标目录，必须位于 /config/nxc/saves 之下；不填则默认 /config/nxc/saves/<app 的16位十六进制>"},
};
const nxc::MethodInfo kInfoBackup = {
    kBackupParams, 2, "write",
    "★ 把一个 title 的存档整棵目录树复制到 SD 卡（只读存档、只往 SD 写）。\n"
    "作用: 在改内存／改存档之前，先留一份可回退的备份。\n"
    "参数:\n"
    "  app = 必填，要备份的 title id，十六进制并认 0x。\n"
    "        值可直接用 save.list 输出里的 app=（形如 0x010021C000B6A000）。\n"
    "  to  = 可选，目标目录，必须位于 /config/nxc/saves 之下（否则 403）。\n"
    "        不填时默认 /config/nxc/saves/<app 的 16 位十六进制>。\n"
    "示例:\n"
    "  save.backup app=0x010021C000B6A000\n"
    "  save.backup app=0x010021C000B6A000 to=/config/nxc/saves/botw_before_edit\n"
    "注意:\n"
    "  ★ 一个文件都没复制出来就回 ERROR（不再「零产出却回 OK」）。\n"
    "  ★ 部分失败时 result=partial 并带 first=<第一条原因>，别把 partial 当成功。\n"
    "  ★ 整个存档目录树会复制到 /config/nxc/saves/<app 十六进制>/ 下。\n"
    "  ★ 只做「备份出来」，不做「写回存档」（恢复风险高，暂不支持）。\n"
    "返回:\n"
    "  app= to= files= dirs= bytes= failed= result=ok|partial（有失败时加 first=）。\n"
    "相关: save.list mem.write fs.read\n",
};

}  // namespace

const nxc::Method kMethods[] = {
    {"list",   "枚举存档：可选 space=user|system|sduser|sdsystem|all，app=<title id>，count=<最多几条，默认20，上限200>", cmdList,   &kInfoList},
    {"backup", "★ 把某个 title 的存档整棵目录树备份到 SD：app=<title id> [to=/config/nxc/saves/<app>]", cmdBackup, &kInfoBackup},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_save, "save",
    "存档枚举与备份（改内存前的安全前提）", kMethods,
    "管存档：枚举（save.list）与把整棵存档目录树备份到 SD（save.backup）。\n"
    "什么时候用它：\n"
    "  · 动内存之前先备份 —— 改内存的下游往往就是存档，改坏了没有后悔药。\n"
    "  · 想知道某个游戏有没有存档、在哪个空间、多大。\n"
    "工作流：先 save.list 找到 app → 再 save.backup 备份 → 之后才 mem.write。\n"
    "边界：\n"
    "  · 只做只读地把存档复制出来，不做写回／恢复。\n"
    "  · 只写 SD 卡上的 /config/nxc/saves；不会改动设备上的存档本身。\n"
    "相关工具：mem（改内存前先备份）、fs（把备份取回本地）、title（列 title）\n"
);
