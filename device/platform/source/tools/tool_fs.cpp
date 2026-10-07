// 内置工具 · fs：SD 卡文件操作。
//
// ★ 安全边界（首期唯一允许"写"的地方之一，另一处是 clk.set）：
//   * 只允许 /config/nxc 与 /switch 两个根，其余一律 403。
//   * 路径里出现 ".." 直接拒绝（防穿越）。
//   * 读是分块的（offset/len），二进制走 base64，避免把大文件塞进一条应答。
#include "nxc_sdk.hpp"
#include "../core/sd.hpp"

#include <dirent.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kMaxListEntries = 200;
// 一次最多读这么多原始字节；base64 后 32000 字符，正好塞进 32KB 应答缓冲。
// 写路径也复用这个常量当解码上限（请求里的 data 值受 `kArgValueBytes = 4096` 限制
// —— 2026-10-06 更正：这里原来写 512，是常量提到 4096 之前的旧值）。
constexpr int kReadChunk      = 24000;

// ---------------------------------------------------------------- 路径门禁（已放宽为"整卡"）
bool underRoot(const char* path, const char* root) {
    const size_t n = std::strlen(root);
    if (std::strncmp(path, root, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}

// ★ 写进自己的模块目录 = **自我更新**（不用 FTP 也能换 exefs.nsp）。
//   风险只落在我们自己身上：写坏了下次开机模块加载失败，拔卡删目录即可复原。
//   所以这里不留确认口令，但写入前一定留面包屑，把"写了什么、多大"记下来。
bool isSelfUpdate(const char* path) {
    return underRoot(path, nxc::kSelfContentDir);
}

// ★ `/bootloader` 是引导器（Hekate 等）的配置目录。
//   为什么需要它：在 CFW 上 `power.reboot` 的落点是**引导器菜单**，不是回到系统；
//   要做到"重启后直接进指定系统"，就得能读写引导器的配置（如 hekate_ipl.ini 的 autoboot）。
//   ★ 这个根**比较敏感**：写坏了可能影响开机。所以只放开读+写，且写入会留面包屑；
//     真写坏了也仍有引导器菜单可手动选，属于可恢复。
bool isBootLoaderConfig(const char* path) {
    return underRoot(path, "/bootloader");
}

// ★★★ 2026-10-06 21:45 · 按用户要求：**路径白名单已删除**（连带"根以外要带口令"那一套）。
//   现在 `fs.*` 对**整张 SD 卡**直接可读写，**不需要任何 confirm 口令**。
//
//   ★ 只保留一条：**路径必须是绝对路径**（以 `/` 开头）。
//     理由不是安全，而是**正确性**：设备侧的当前工作目录不可预期
//     （`sd::open` 走的是"挂载点 + 绝对路径"），相对路径会随调用时序变化，
//     属于"看不出错在哪"的那一类问题。
//
//   ★ 为什么连 `..` 的拒绝也去掉了：它原本的唯一作用是"**防止穿出白名单**"。
//     白名单既然没了，它只会挡住合法写法（例如 `/switch/../config/nxc/x`）。
//     而 SD 根之上没有东西可穿 —— 路径真写错时操作系统自己会报错。
//
//   ★★ 仍然保留的两处**写入留痕**（在 `cmdWrite` 里，不属于门禁）：
//     写自己的模块目录 = 自我更新；写 `/bootloader` = 影响下次开机落点。
//     这两类写入会写面包屑，方便事后回溯"是谁改的"。
bool gate(const nxc::Args&, nxc::Reply& r, const char* path) {
    if (path == nullptr || path[0] != '/') {
        r.fail(400, "path-must-be-absolute");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- base64
// ★★★ 2026-10-06 晚：**这里原来有一份自己的 base64 编/解码**，与 SDK 的
//   `nxc::util::b64Encode/b64Decode` 是**两份同样逻辑**（典型的两份实现漂移源）。
//   现在统一用 SDK 那份（本文件里的调用点已改成 `nxc::util::…`）。
//   顺带修掉那个真 bug：解码器遇到非法字符原来是 `continue`（**跳过**）⇒
//   `data=!!!bad` 会静默写出 2 字节垃圾并回 `OK bytes=2`（最坏的一种失败形态）。
//   严格版遇非法字符直接返回 -1，调用方必须当失败处理。

// ---------------------------------------------------------------- 命令
void cmdRoots(const nxc::Args&, nxc::Reply& r) {
    // ★ 2026-10-06：**白名单已删** —— 现在整张 SD 卡都能读写，下面这些只是"常用起点"。
    r.kv("sd_root", "/");
    r.kv("0", "/config/nxc");
    r.kv("1", "/switch");
    r.kv("2", nxc::kSelfContentDir);
    r.kv("3", "/bootloader");
    r.kvi("count", 4);
    r.raw("note: ★★ 白名单已删除 —— 任意**绝对路径**都可读写，不需要 confirm 口令");
    r.raw("note: SD 卡的根就是 /；下面 4 个只是常用起点");
    r.raw("note: 根 2 是平台自己的模块目录 —— 写它是自我更新；写坏了下次开机加载失败，拔卡删目录即可复原");
    r.raw("note: 根 3 是引导器配置 —— 写它会影响下次开机的落点，改前先 fs.copy 备份");
    r.raw("note: ★ 范围仅限 **SD 卡**；主机内部储存（NAND / 系统分区）不在此工具范围内");
}

void cmdLs(const nxc::Args& a, nxc::Reply& r) {
    const char* path = a.get("path");
    if (!gate(a, r, path)) return;

    DIR* dir = opendir(path);
    if (dir == nullptr) { r.fail(404, "cannot-open-dir"); return; }

    int n = 0;
    bool truncated = false;
    for (dirent* e = readdir(dir); e != nullptr; e = readdir(dir)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        if (n >= kMaxListEntries) { truncated = true; break; }

        char full[512];
        std::snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        const bool ok = (::stat(full, &st) == 0);

        r.next();
        r.kv("name", e->d_name);
        r.kv("type", ok ? (S_ISDIR(st.st_mode) ? "dir" : "file") : "?");
        r.kvi("size", ok ? static_cast<long long>(st.st_size) : 0);
        ++n;
    }
    closedir(dir);

    r.next();
    r.kvi("count", n);
    r.kvi("truncated", truncated ? 1 : 0);
}

void cmdStat(const nxc::Args& a, nxc::Reply& r) {
    const char* path = a.get("path");
    if (!gate(a, r, path)) return;
    struct stat st;
    if (::stat(path, &st) != 0) { r.fail(404, "not-found"); return; }
    r.kv("path", path);
    r.kv("type", S_ISDIR(st.st_mode) ? "dir" : "file");
    r.kvi("size", static_cast<long long>(st.st_size));
}

void cmdRead(const nxc::Args& a, nxc::Reply& r) {
    const char* path = a.get("path");
    if (!gate(a, r, path)) return;

    long long offset = a.getInt("offset", 0);
    long long len    = a.getInt("len", kReadChunk);
    if (offset < 0) offset = 0;
    if (len <= 0 || len > kReadChunk) len = kReadChunk;

    FILE* f = nxc::sd::open(path, "rb");
    if (f == nullptr) { r.fail(404, "cannot-open-file"); return; }
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(f);
        r.fail(400, "bad-offset");
        return;
    }

    // 静态缓冲：不用栈（栈只有 64KB），也不做动态分配。
    static unsigned char raw[kReadChunk];
    static char b64[kReadChunk * 4 / 3 + 8];

    const int n = static_cast<int>(std::fread(raw, 1, static_cast<size_t>(len), f));
    std::fclose(f);
    if (n < 0) { r.fail(500, "read-failed"); return; }

    nxc::util::b64Encode(raw, n, b64);
    r.kvi("bytes", n);
    r.kvi("offset", offset);
    r.kvi("eof", n < len ? 1 : 0);
    r.raw(b64);
}

void cmdWrite(const nxc::Args& a, nxc::Reply& r) {
    const char* path = a.get("path");
    if (!gate(a, r, path)) return;

    const char* data = a.get("data");
    if (data == nullptr) { r.fail(400, "missing-data"); return; }
    const bool append = a.getInt("append", 0) != 0;

    static unsigned char raw[kReadChunk];
    // ★ 严格版：非法字符 / `=` 填充之后又有数据 / 超出 cap，一律返回 -1。
    const int n = nxc::util::b64Decode(data, raw, kReadChunk);
    if (n < 0) {
        // ★★★ 2026-10-06 晚：这里原来只在"超出 cap"时报错，**非法字符会被解码器静默跳过**
        //   ⇒ `data=!!!bad` 回 `OK bytes=2`，写出 2 字节垃圾。
        //   现在解码器严格化，这一条同时覆盖"数据不是合法 base64"与"载荷过大"。
        r.fail(400, "bad-or-too-large-base64: data 必须是合法 base64，且解码后不超过 24000 字节");
        return;
    }
    // ★★ 2026-10-06 晚：**空载荷要被拒**。`data=`（空串）或 `data===` 会解出 0 字节，
    //   然后建/截断出一个**空文件**并回 `OK bytes=0` —— 又是一次"回 OK 但没做事"。
    //   `mem.write` 一直有这道守卫，`fs` 侧原来漏了。
    if (n == 0) {
        r.fail(400, "empty-data: data 解出来是 0 字节（不接受空载荷；要清空文件请 fs.rm 后重建）");
        return;
    }

    // 写之前先留面包屑：写卡是这套工具里少数几个有副作用的动作。
    // ★ 自我更新 / 引导器配置这两种写入单独记一条更醒目的 —— 出问题时要靠它回溯。
    if (isSelfUpdate(path)) {
        nxc::host::breadcrumb("fs.write SELF-UPDATE %s bytes=%d append=%d", path, n, append ? 1 : 0);
        nxc::host::log("fs.write self-update: %s (+%d bytes)", path, n);
    } else if (isBootLoaderConfig(path)) {
        nxc::host::breadcrumb("fs.write BOOTLOADER-CONFIG %s bytes=%d append=%d", path, n, append ? 1 : 0);
        nxc::host::log("fs.write bootloader-config: %s (+%d bytes)", path, n);
    } else {
        nxc::host::breadcrumb("fs.write %s bytes=%d", path, n);
    }

    FILE* f = nxc::sd::open(path, append ? "ab" : "wb");
    if (f == nullptr) { r.fail(500, "cannot-open-for-write"); return; }
    const size_t written = std::fwrite(raw, 1, static_cast<size_t>(n), f);
    std::fclose(f);

    // ★★★ 2026-10-06 晚：**短写要报错**。原来无条件回 `bytes=<实际写入数>`，
    //   磁盘满/写失败时只回一个小数字，调用方不看就以为写成功了（同类的假成功）。
    if (written != static_cast<size_t>(n)) {
        r.failf(500, "short-write: 只写进 %lld/%d 字节（磁盘满？）",
                static_cast<long long>(written), n);
        return;
    }

    // ★ 只回一行：回包行数越多，一批请求能塞的条数越少（应答有行数上限）。
    //   原来回 3 行 ⇒ 一批最多 10 条；现在回 1 行 ⇒ 一批能塞 20 条，上传快一倍。
    r.kvi("bytes", static_cast<long long>(written));
}

void cmdMkdir(const nxc::Args& a, nxc::Reply& r) {
    const char* path = a.get("path");
    if (!gate(a, r, path)) return;
    const int rc = ::mkdir(path, 0777);
    // ★★★ 2026-10-06 晚：失败要回 ERR，不再只写进 `result=see-rc`。
    //   ★ 但"目录已存在"（EEXIST）**不算失败** —— 这个方法本来就常被当幂等的"确保目录在"
    //     用（PC 侧脚本就是那么用的），把 EEXIST 变成错误会把正常用法打坏。
    if (rc != 0 && errno != EEXIST) {
        r.failf(500, "mkdir-failed path=%s rc=%d", path, rc);
        return;
    }
    r.kv("path", path);
    r.kv("result", rc == 0 ? "created" : "already-exists");
}

void cmdRm(const nxc::Args& a, nxc::Reply& r) {
    const char* path = a.get("path");
    if (!gate(a, r, path)) return;
    nxc::host::breadcrumb("fs.rm %s", path);
    int rc = ::remove(path);
    if (rc != 0) rc = ::rmdir(path);  // 目标是目录时换 rmdir
    // ★★★ 2026-10-06 晚：失败回 ERR，不再只写 `result=see-rc`。
    //   原来的形态是 `OK path=… rc=-1 result=see-rc` —— **回的是 OK**，
    //   调用方不看 `rc` 就会把"没删掉"当成"删掉了"（删不掉的常见原因：
    //   路径不存在、目录非空、或文件被别的程序占着）。
    if (rc != 0) {
        r.failf(500, "remove-failed path=%s rc=%d（路径不存在／目录非空／被占用）", path, rc);
        return;
    }
    r.kv("path", path);
    r.kv("result", "removed");
}

// 复制文件（白名单内）。★ 主要用途：**自我更新之前先把旧的 exefs.nsp 备份一份**。
// 没有 rename 原语，所以就是"读出来 → 写过去"，分块做以免撞上缓冲上限。
void cmdCopy(const nxc::Args& a, nxc::Reply& r) {
    const char* src = a.get("src");
    const char* dst = a.get("dst");
    if (!gate(a, r, src)) return;
    if (!gate(a, r, dst)) return;

    FILE* in = nxc::sd::open(src, "rb");
    if (in == nullptr) { r.fail(404, "cannot-open-src"); return; }
    FILE* out = nxc::sd::open(dst, "wb");
    if (out == nullptr) { std::fclose(in); r.fail(500, "cannot-open-dst"); return; }

    nxc::host::breadcrumb("fs.copy %s -> %s", src, dst);

    static unsigned char buf[4096];
    long long total = 0;
    bool ok = true;
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
        total += static_cast<long long>(n);
    }
    if (std::ferror(in)) ok = false;

    std::fclose(in);
    std::fclose(out);

    if (!ok) {
        r.failf(500, "copy-failed after %lld bytes", total);
        return;
    }
    r.kv("src", src);
    r.kv("dst", dst);
    r.kvi("bytes", total);
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
//
// ★★ 规矩：**这个工具的全部知识都写在本文件里** —— 核心不认识 `fs` 是什么，
//    `tools.list` / `tools.doc` 只是把这里的内容汇总出去。**AI 就是靠这些字学会用它。**
//    ⇒ 新增/改动方法时，把参数表与教程一起改（别只改实现）。
// ★ 参数表可以多个方法共用（例如下面的 `kPath`）—— 省得抄来抄去、也不会互相写歪。
namespace {

const nxc::Param kPath[] = {
    {"path", "str", true, nullptr, "绝对路径，必须 `/` 开头（如 /config/nxc/a.txt）"},
};
const nxc::Param kSrcDst[] = {
    {"src", "str", true, nullptr, "源文件绝对路径"},
    {"dst", "str", true, nullptr, "目标绝对路径（父目录必须已存在）"},
};
const nxc::Param kRead[] = {
    {"path",   "str", true,  nullptr, "文件绝对路径"},
    {"offset", "int", false, "0",     "从第几字节开始读"},
    {"len",    "int", false, "24000", "最多读多少原始字节（上限 24000）"},
};
const nxc::Param kWrite[] = {
    {"path",   "str",    true,  nullptr, "文件绝对路径"},
    {"data",   "base64", true,  nullptr, "内容（base64）。非法 base64 会被拒，不会静默写垃圾"},
    {"append", "0|1",    false, "0",     "1 = 追加到文件尾；0 = 覆盖"},
};

const nxc::MethodInfo kInfoRoots = {
    nullptr, 0, "read",
    "列出这个平台允许访问的根。\n"
    "★ 2026-10-06 起白名单已删除 ⇒ 现在就是「整张 SD 卡、任意绝对路径」。\n"
    "★ 但**碰不到主机内部储存**（NAND / 系统分区）：fs 只挂了 SD。\n"
    "示例: fs.roots\n"
    "返回: roots=<路径列表>",
};
const nxc::MethodInfo kInfoLs = {
    kPath, 1, "read",
    "列一个目录（不含子目录），每条给 name / type(file|dir) / size。\n"
    "★ 硬上限 200 条；被截断时回包里有 truncated=1（**不是静默截断**）。\n"
    "★ 名字里带空格会被转义成 %20，PC 客户端会自动还原；用 nc 手打时要自己写 %20。\n"
    "示例: fs.ls path=/config/nxc\n"
    "返回: 每行 name=/type=/size=，末行 count= truncated=",
};
const nxc::MethodInfo kInfoStat = {
    kPath, 1, "read",
    "看一个路径存在不存在、是文件还是目录、多大。\n"
    "★ 判断'卡上有没有这个文件'用它最快，比 ls 整个目录便宜。\n"
    "示例: fs.stat path=/atmosphere/contents/4200000000000012/exefs.nsp\n"
    "返回: path= type= size=",
};
const nxc::MethodInfo kInfoRead = {
    kRead, 3, "read",
    "分块读文件，内容以 base64 放在一行 `#` 自由文本里。\n"
    "★ 单块上限 24000 原始字节（base64 后 32000 字符，正好塞进 32KB 应答）。\n"
    "★ 读法：按**实际收到的字节数**推进 offset，**读到 bytes=0 才算到头** ——\n"
    "  不要拿'少于请求量'当 EOF（老客户端在这上面踩过：152KB 的文件只取回 3072 字节）。\n"
    "★ 想算远端文件的哈希：不必整份取回本地，分块读 + 边读边算即可\n"
    "  （现成工具：`pc/nxc_remote_sha.py`）。\n"
    "示例: fs.read path=/config/nxc/heartbeat.txt offset=0 len=64\n"
    "返回: bytes= offset= eof=，然后一行 #<base64>",
};
const nxc::MethodInfo kInfoWrite = {
    kWrite, 3, "write",
    "往 SD 卡写文件（内容走 base64）。\n"
    "★ 单次值上限 4096 字节 ⇒ 大文件必须分块（配合 append=1 一段段追加）。\n"
    "★ 非法 base64 会被**拒绝**（400），不会像老版本那样静默写出垃圾；空载荷也会被拒。\n"
    "★ 短写会报错（磁盘满了不会假装成功）。\n"
    "★ 写入留痕：写自己的模块目录 = 自我更新；写 /bootloader = 影响下次开机落点 ——\n"
    "  这两类会单独写一条更醒目的面包屑，便于事后回溯'是谁改的'。\n"
    "示例: fs.write path=/config/nxc/x.txt data=aGVsbG8=\n"
    "      fs.write path=/config/nxc/log.txt data=YQ== append=1\n"
    "返回: bytes=<实际写入字节数>",
};
const nxc::MethodInfo kInfoCopy = {
    kSrcDst, 2, "write",
    "复制文件（内部就是读一段写一段，4096 字节一块）。\n"
    "★ 主要用途：**给平台自己升级之前，先把旧的 exefs.nsp 备份一份**。\n"
    "★ 目标父目录必须已存在（先用 fs.mkdir）。\n"
    "示例: fs.copy src=/atmosphere/contents/4200000000000012/exefs.nsp dst=/config/nxc/_prev.nsp\n"
    "返回: src= dst= bytes=",
};
const nxc::MethodInfo kInfoMkdir = {
    kPath, 1, "write",
    "建一级目录（**不是** mkdir -p：父目录不存在会失败）。\n"
    "★ 目录已存在**算成功**（result=already-exists）—— 这样它可以被当幂等的'确保目录在'用。\n"
    "★ 真失败会回 ERR（不像老版本只把 rc 塞在 OK 里）。\n"
    "示例: fs.mkdir path=/config/nxc/docs\n"
    "返回: path= result=created|already-exists",
};
const nxc::MethodInfo kInfoRm = {
    kPath, 1, "write",
    "删文件或空目录。\n"
    "★ 失败会回 ERR 并带 rc（路径不存在／目录非空／被占用）—— 老版本只回 OK + rc=-1，\n"
    "  看起来像删掉了。\n"
    "★ 目录非空不会被递归删（安全考虑：先用 fs.ls 看清再逐个删）。\n"
    "示例: fs.rm path=/config/nxc/_tmp.txt\n"
    "返回: path= result=removed",
};

}  // namespace

const nxc::Method kMethods[] = {
    {"roots", "列出允许访问的根（现在等于'整张 SD 卡'）", cmdRoots, &kInfoRoots},
    {"ls",    "列目录（最多 200 条，被截断会明确标出）",    cmdLs,    &kInfoLs},
    {"stat",  "看一个路径是否存在/类型/大小",               cmdStat,  &kInfoStat},
    {"read",  "分块读文件（返回 base64；读到 0 字节才算完）", cmdRead,  &kInfoRead},
    {"write", "写文件（base64；非法内容会被拒）",            cmdWrite, &kInfoWrite},
    {"copy",  "复制文件（平台自我更新前备份用）",            cmdCopy,  &kInfoCopy},
    {"mkdir", "建一级目录（已存在算成功）",                  cmdMkdir, &kInfoMkdir},
    {"rm",    "删文件/空目录（失败会回 ERR）",               cmdRm,    &kInfoRm},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_fs, "fs", "SD 卡文件（整卡可读写；仅 SD，不含主机内部储存）", kMethods,
    "管 SD 卡上的文件：读、写、列、删、复制、建目录。\n"
    "什么时候用它：\n"
    "  · 看/改卡上的配置与模组文件（/config、/atmosphere、/switch…）\n"
    "  · 部署与备份：先 fs.copy 备份旧模块，再写新的（配合 nxc_deploy 更省事）\n"
    "  · 取回产物：fs.read 分块读回本地（PC 侧 nxc_fetch 会自动拼）\n"
    "边界：\n"
    "  · **只有 SD 卡**，碰不到主机内部储存（NAND / 系统分区）\n"
    "  · 路径必须是绝对路径（设备侧当前工作目录不可预期）\n"
    "  · 单次写值上限 4096 字节 ⇒ 大文件分块 + append=1\n"
    "相关工具：save（存档备份）、log（日志）、probe（结果文件）"
);
