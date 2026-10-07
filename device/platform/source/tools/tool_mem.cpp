// 内置工具 · mem：读写**正在运行的游戏**的内存（不冻结游戏）。
//
// ## 实现依据（照抄 sys-botbase 的成熟做法，不自己猜）
//
// 链路是**内核系统调用**，不是 `dmnt:cht`（libnx 根本没封装后者）：
//
//     pmdmntGetApplicationProcessId → svcDebugActiveProcess
//         → svcReadDebugProcessMemory / svcWriteDebugProcessMemory
//         → svcCloseHandle
//
// 这几个 SVC 都在我们 NPDM 的 syscall 表里（`svcDebugActiveProcess`=0x60、
// `svcReadDebugProcessMemory`=0x6a、`svcWriteDebugProcessMemory`=0x6b），libnx 也都有封装。
//
// ## ★★ 与 GDB 互斥（重要）
//
// `svcDebugActiveProcess` 是**独占**的：同一时刻只有一个调试者能挂到目标进程上。
// 而 PC 侧的 GDB 调试桩（TCP 22225）走的也是这套调试能力。
// ⇒ **本工具的读写与 GDB 会话不能同时进行**；同时用会互相抢句柄（实测崩机 fatal 01000000000d609）。
//
// 本工具的好处正是 GDB 做不到的那一点：**每次操作 attach → 读/写 → 立刻 detach**，
// 游戏全程照常运行，不需要"停下来看"。
#include "nxc_sdk.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr int kReadMax = 3072;  // 原始字节目；base64 后约 4096 字符
constexpr const char* kWriteToken = "I-KNOW-THIS-WRITES-MEMORY";

// 解析 "0x1234" / "1234"，支持 64 位。
//
// ★ 2026-10-06 晚：算法已上收到 SDK 的 `nxc::util::parseU64` —— 这一版**原来是自己一份**，
//   而且**没有溢出检查**（`value * base + digit` 直接算，超长输入是未定义行为）。
//   现在 `tool_title` / `tool_save` / `tool_mem` 共用同一份，只留这个薄包装，调用点不动。
bool parseU64(const char* text, u64* out) {
    return nxc::util::parseU64(text, out);
}

// RAII：构造时挂到当前应用进程，析构时立刻放手。
// ★ 只在真正要读/写的那一瞬间持有 —— 这样游戏不会被冻住。
struct Attach {
    Handle debug = 0;
    u64 pid = 0;
    Result pidRc = 1;
    Result dbgRc = 1;
    bool pmOpen = false;

    Attach() {
        if (R_FAILED(pmdmntInitialize())) return;
        pmOpen = true;
        pidRc = pmdmntGetApplicationProcessId(&pid);
        if (R_SUCCEEDED(pidRc) && pid != 0) {
            dbgRc = svcDebugActiveProcess(&debug, pid);
        }
    }
    ~Attach() {
        if (debug != 0) svcCloseHandle(debug);  // ★ 立刻放手，别长期占着调试句柄
        if (pmOpen) pmdmntExit();
    }
    bool ok() const { return debug != 0; }

    Attach(const Attach&) = delete;
    Attach& operator=(const Attach&) = delete;
};

// ---------------------------------------------------------------- 方法
void cmdInfo(const nxc::Args&, nxc::Reply& r) {
    Attach at;
    if (R_FAILED(at.pidRc)) {
        // 最常见的原因：主界面上没有应用在跑（不是错误，是状态）。
        r.failf(503, "no-application-running pid_rc=0x%08X", static_cast<unsigned>(at.pidRc));
        return;
    }
    r.kvi("pid", static_cast<long long>(at.pid));

    // program id
    {
        Attach probe;  // 这里只要 pmdmnt，不需要复用 at
        u64 programId = 0;
        if (R_SUCCEEDED(probe.pidRc) && R_SUCCEEDED(pmdmntGetProgramId(&programId, at.pid))) {
            r.kvf("program_id", "0x%016llX", static_cast<unsigned long long>(programId));
        }
    }

    // 模块基址（sys-botbase 的经验：拿到两个模块时，[1] 才是主 NSO）
    if (R_SUCCEEDED(ldrDmntInitialize())) {
        LoaderModuleInfo modules[4];
        s32 count = 0;
        if (R_SUCCEEDED(ldrDmntGetProcessModuleInfo(at.pid, modules, 4, &count))) {
            for (s32 i = 0; i < count; ++i) {
                r.next();
                r.kvi("module", i);
                r.kvf("base", "0x%016llX", static_cast<unsigned long long>(modules[i].base_address));
                r.kvf("size", "0x%llX", static_cast<unsigned long long>(modules[i].size));
            }
            r.next();
            r.kvi("module_count", count);
            if (count > 0) {
                const int idx = (count > 1) ? 1 : 0;
                r.kvf("main_base", "0x%016llX",
                      static_cast<unsigned long long>(modules[idx].base_address));
                r.kvi("main_module_index", idx);
            }
        } else {
            r.kv("module_info", "unavailable");
        }
        ldrDmntExit();
    }

    // 堆基址
    if (at.ok()) {
        u64 heap = 0;
        if (R_SUCCEEDED(svcGetInfo(&heap, InfoType_HeapRegionAddress, at.debug, 0))) {
            r.kvf("heap_base", "0x%016llX", static_cast<unsigned long long>(heap));
        }
    }

    r.kv("attach", at.ok() ? "ok" : "failed");
    if (!at.ok()) {
        r.kvf("attach_rc", "0x%08X", static_cast<unsigned>(at.dbgRc));
        // ★★★ 2026-10-06 晚：把"这份回包里少了什么"点出来。
        //   抢不到调试句柄时（典型原因：PC 侧 GDB 桩 22225 占着**同一个**调试句柄，两者互斥），
        //   上面的 heap_base 压根不会输出，而回包仍然是 OK ⇒ 看起来像"这个游戏没有堆"。
        //   ★ 保持 OK 是有意的：pid / program_id / 模块基址**都是有效结果**，
        //     整条判失败会把已经拿到的信息一起丢掉（与 probe.group 同一个道理）。
        //     要修的不是成败，而是"部分性没说出口"。
        r.kv("missing", "heap_base");
        r.raw("why: 抢不到调试句柄 ⇒ heap_base 取不到；要用 mem.read / mem.write 请先断开 PC 侧 GDB 会话");
    }
    r.raw("note: 读写与 PC 侧 GDB 会话互斥（抢同一个调试句柄），不能同时进行");
}

void cmdRead(const nxc::Args& a, nxc::Reply& r) {
    u64 addr = 0;
    if (!parseU64(a.get("addr"), &addr)) { r.fail(400, "missing-or-bad-addr"); return; }
    long long size = a.getInt("size", 256);
    bool sizeAdjusted = false;
    if (size <= 0 || size > kReadMax) { size = 256; sizeAdjusted = true; }

    Attach at;
    if (!at.ok()) {
        r.failf(503, "attach-failed pid_rc=0x%08X dbg_rc=0x%08X",
                static_cast<unsigned>(at.pidRc), static_cast<unsigned>(at.dbgRc));
        return;
    }

    static unsigned char raw[kReadMax];
    static char b64[kReadMax * 4 / 3 + 8];

    const Result rc = svcReadDebugProcessMemory(raw, at.debug, addr, static_cast<u64>(size));
    if (R_FAILED(rc)) {
        // ★ 带上 rc —— 被 GDB 占着调试句柄时走的就是这个分支，rc 能区分原因。
        r.failf(500, "read-failed rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }

    nxc::util::b64Encode(raw, static_cast<int>(size), b64);
    r.kvf("addr", "0x%016llX", static_cast<unsigned long long>(addr));
    r.kvi("bytes", size);
    r.raw(b64);
    // ★★★ 2026-10-06 晚：**越界不再静默**。原来 `size=10000` 会被悄悄改成 256 去读，
    //   调用方看到 `bytes=256` 却不知道自己要的 10KB 没读到 —— 这是同一类"没生效却看不出来"的病
    //   （`log.dump` / `title.list` / `save.list` 已经先修了，这里补齐一致）。
    if (sizeAdjusted) {
        r.next();
        r.kv("note_params", "size 越界（允许 1..3072），已按默认 256 执行");
    }
}

// 按类型读一个数 —— 比让 AI 自己 base64 解码直观得多。
void cmdPeek(const nxc::Args& a, nxc::Reply& r) {
    u64 addr = 0;
    if (!parseU64(a.get("addr"), &addr)) { r.fail(400, "missing-or-bad-addr"); return; }

    const char* type = a.get("type");
    if (type == nullptr) type = "u32";

    int width = 0;
    if (std::strcmp(type, "u8") == 0 || std::strcmp(type, "s8") == 0) width = 1;
    else if (std::strcmp(type, "u16") == 0 || std::strcmp(type, "s16") == 0) width = 2;
    else if (std::strcmp(type, "u32") == 0 || std::strcmp(type, "s32") == 0 ||
             std::strcmp(type, "f32") == 0) width = 4;
    else if (std::strcmp(type, "u64") == 0 || std::strcmp(type, "s64") == 0 ||
             std::strcmp(type, "f64") == 0) width = 8;
    else { r.fail(400, "type-must-be-u8|u16|u32|u64|s8|s16|s32|s64|f32|f64"); return; }

    Attach at;
    if (!at.ok()) {
        r.failf(503, "attach-failed pid_rc=0x%08X dbg_rc=0x%08X",
                static_cast<unsigned>(at.pidRc), static_cast<unsigned>(at.dbgRc));
        return;
    }

    unsigned char buf[8] = {0};
    const Result rc = svcReadDebugProcessMemory(buf, at.debug, addr, static_cast<u64>(width));
    if (R_FAILED(rc)) {
        r.failf(500, "read-failed rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }

    u64 raw = 0;
    std::memcpy(&raw, buf, static_cast<size_t>(width));

    r.kvf("addr", "0x%016llX", static_cast<unsigned long long>(addr));
    r.kv("type", type);

    if (std::strcmp(type, "f32") == 0) {
        float f; std::memcpy(&f, buf, 4);
        r.kvf("value", "%.6g", static_cast<double>(f));
    } else if (std::strcmp(type, "f64") == 0) {
        double d; std::memcpy(&d, buf, 8);
        r.kvf("value", "%.17g", d);
    } else if (width == 1 && type[0] == 's') {
        r.kvi("value", static_cast<s8>(raw));
    } else if (width == 2 && type[0] == 's') {
        r.kvi("value", static_cast<s16>(raw));
    } else if (width == 4 && type[0] == 's') {
        r.kvi("value", static_cast<s32>(raw));
    } else if (width == 8 && type[0] == 's') {
        r.kvi("value", static_cast<s64>(raw));
    } else {
        r.kvf("value", "0x%llX", static_cast<unsigned long long>(raw));
        r.kvf("value_dec", "%llu", static_cast<unsigned long long>(raw));
    }
}

void cmdWrite(const nxc::Args& a, nxc::Reply& r) {
    const char* token = a.get("confirm");
    if (token == nullptr || std::strcmp(token, kWriteToken) != 0) {
        // ★★★ 2026-10-06 晚：原来 `fail` 之后的 `raw("usage: …")` / `raw("warn: …")`
        //   **发不出去**（ERR 不带载荷）⇒ 折进错误消息里，让调用方知道该带什么口令、
        //   以及这一步的后果。
        r.failf(403, "refusing-without-confirm-token: 需要 confirm=%s，用法 mem.write addr=<0x...> "
                     "data=<base64>（★ 会直接改动运行中游戏的内存，可能让游戏崩溃或损坏存档）",
                kWriteToken);
        return;
    }

    u64 addr = 0;
    if (!parseU64(a.get("addr"), &addr)) { r.fail(400, "missing-or-bad-addr"); return; }
    const char* data = a.get("data");
    if (data == nullptr) { r.fail(400, "missing-data"); return; }

    static unsigned char raw[kReadMax];
    const int n = nxc::util::b64Decode(data, raw, kReadMax);
    if (n < 0) { r.fail(413, "payload-too-large"); return; }
    if (n == 0) { r.fail(400, "empty-data"); return; }

    // 写内存是有副作用的：先留面包屑，出事后能看出是谁写的。
    nxc::host::breadcrumb("mem.write addr=0x%llX bytes=%d",
                          static_cast<unsigned long long>(addr), n);

    Attach at;
    if (!at.ok()) {
        r.failf(503, "attach-failed pid_rc=0x%08X dbg_rc=0x%08X",
                static_cast<unsigned>(at.pidRc), static_cast<unsigned>(at.dbgRc));
        return;
    }

    const Result rc = svcWriteDebugProcessMemory(at.debug, raw, addr, static_cast<u64>(n));
    if (R_FAILED(rc)) {
        r.failf(500, "write-failed rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }

    r.kvf("addr", "0x%016llX", static_cast<unsigned long long>(addr));
    r.kvi("bytes", n);
    r.kv("result", "written");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
//
// ★★ 规矩：**这个工具的全部知识都写在本文件里** —— 核心不认识 `mem` 是什么，
//    `tools.list` / `tools.doc` 只是把这里的内容汇总出去。**AI 就是靠这些字学会用它。**
namespace {

const nxc::Param kReadParams[] = {
    {"addr", "hex", true,  nullptr, "起始地址（十六进制，认 0x，如 0x...）"},
    {"size", "int", false, "256",   "读多少原始字节（上限 3072；越界回落 256）"},
};
const nxc::Param kPeekParams[] = {
    {"addr", "hex", true,  nullptr, "要读的地址（十六进制，认 0x）"},
    {"type", "u8|u16|u32|u64|s8|s16|s32|s64|f32|f64", false, "u32",
     "按什么类型解释这几个字节，默认 u32"},
};
const nxc::Param kWriteParams[] = {
    {"addr",    "hex",    true, nullptr, "目标地址（十六进制，认 0x）"},
    {"data",    "base64", true, nullptr, "要写入的字节（base64；空载荷／非法 base64 会被拒）"},
    {"confirm", "str",    true, nullptr, "口令，必须等于 I-KNOW-THIS-WRITES-MEMORY"},
};

const nxc::MethodInfo kInfoInfo = {
    nullptr, 0, "read",
    "读当前运行游戏进程的元数据。\n"
    "作用: 拿到 pid / program_id / 模块基址 / 堆基址。\n"
    "      其中 main_base 是后面 mem.read／mem.peek／mem.write 的第一步。\n"
    "参数: 无。\n"
    "示例: mem.info\n"
    "注意:\n"
    "  ★★ 与 PC 侧 GDB 会话互斥 —— 两者抢同一个调试句柄，同时用会崩机。\n"
    "  ★ 抢不到调试句柄时 heap_base 不会输出，回包里会有 missing=heap_base，\n"
    "    但整条仍是 OK（pid / program_id / 模块基址都有效）。\n"
    "  ★ 主界面上没有应用在跑时回 503 no-application-running（是状态，不是坏了）。\n"
    "返回:\n"
    "  pid= program_id=；每个模块一行 module=<i> base= size= 再 module_count=；\n"
    "  还有 main_base= main_module_index= heap_base=（可能缺）；\n"
    "  attach=ok|failed（失败时加 attach_rc= missing=）。\n"
    "相关: mem.read mem.peek mem.write\n",
};
const nxc::MethodInfo kInfoRead = {
    kReadParams, 2, "read",
    "读游戏内存的一段原始字节（内容以 base64 回）。\n"
    "作用: 取原始字节流，自己解码／搜索。只想看一个数用 mem.peek 更直观。\n"
    "参数:\n"
    "  addr = 必填，起始地址（十六进制，认 0x）。\n"
    "  size = 要读的字节数，默认 256；越界（≤0 或 >3072）回落 256。\n"
    "示例:\n"
    "  mem.read addr=0x... size=256\n"
    "注意:\n"
    "  ★★ 与 PC 侧 GDB 会话互斥（抢同一个调试句柄），同时用会崩机。\n"
    "  ★ 单次上限 3072 字节；要读更长就分段读。\n"
    "  ★ 不冻结游戏：attach → 读 → 立刻 detach。\n"
    "返回:\n"
    "  addr= bytes=，然后一行 #<base64>。\n"
    "相关: mem.info mem.peek mem.write\n",
};
const nxc::MethodInfo kInfoPeek = {
    kPeekParams, 2, "read",
    "按类型读一个数（比 mem.read 的 base64 直观）。\n"
    "作用: 快速读一个 u8／u16／u32／u64／s8..s64／f32／f64。\n"
    "参数:\n"
    "  addr = 必填，地址（十六进制，认 0x）。\n"
    "  type = 数据类型，默认 u32；取值 u8|u16|u32|u64|s8|s16|s32|s64|f32|f64。\n"
    "示例:\n"
    "  mem.peek addr=0x... type=f32\n"
    "  mem.peek addr=0x... type=u32\n"
    "注意:\n"
    "  ★★ 与 PC 侧 GDB 会话互斥（抢同一个调试句柄），同时用会崩机。\n"
    "  ★ type 写错回 400 并列出允许的取值。\n"
    "返回:\n"
    "  addr= type= value=；无符号整型还会多给 value_dec=（十进制）。\n"
    "相关: mem.info mem.read mem.write\n",
};
const nxc::MethodInfo kInfoWrite = {
    kWriteParams, 3, "action",
    "★★ 直接写运行中游戏的内存 —— 有副作用，可能崩游戏／坏存档。\n"
    "作用: 改内存里的数值（例如金币、血量）。\n"
    "参数:\n"
    "  addr    = 必填，目标地址（十六进制，认 0x）。\n"
    "  data    = 必填，要写入的字节（base64）。空载荷／非法 base64 会被拒。\n"
    "  confirm = 必填口令 I-KNOW-THIS-WRITES-MEMORY，缺了回 403。\n"
    "示例:\n"
    "  mem.write addr=0x... data=<base64> confirm=I-KNOW-THIS-WRITES-MEMORY\n"
    "注意:\n"
    "  ★★ 会改动运行中游戏的内存，可能让游戏崩溃或损坏存档 —— 动手前先 save.backup。\n"
    "  ★★ 与 PC 侧 GDB 会话互斥（抢同一个调试句柄），同时用会崩机。\n"
    "  ★ 单次上限 3072 字节（解码后）；写前会写一条面包屑便于回溯。\n"
    "  ★ 不冻结游戏：attach → 写 → 立刻 detach。\n"
    "返回:\n"
    "  addr= bytes= result=written。\n"
    "相关: mem.read mem.peek save.backup\n",
};

}  // namespace

const nxc::Method kMethods[] = {
    {"info",  "读游戏进程元数据：pid / title id / 模块基址 / 堆基址（先调它拿 main_base）", cmdInfo,  &kInfoInfo},
    {"read",  "读游戏内存字节：addr=0x... [size=（默认256，上限3072）]（返回 base64）", cmdRead,  &kInfoRead},
    {"peek",  "按类型读一个数：addr=0x... [type=u8|u16|u32|u64|s8|s16|s32|s64|f32|f64]", cmdPeek, &kInfoPeek},
    {"write", "写游戏内存：addr=0x... data=<base64> confirm=I-KNOW-THIS-WRITES-MEMORY", cmdWrite, &kInfoWrite},
};

}  // namespace

// ★ 冲突提示写在工具说明里，AI 一读 nxc_caps 就能看到。
NXC_DEFINE_TOOL_EX(nxc_tool_mem, "mem",
    "读/写运行中进程的内存（不冻结进程）。★ 与 PC 侧 GDB 会话互斥：两者抢同一个调试句柄，不能同时用",
    kMethods,
    "读写正在运行的游戏内存（不冻结游戏）。\n"
    "什么时候用它：\n"
    "  · 查数值地址、改金币／血量之类的内存值。\n"
    "  · 先 mem.info 拿 main_base，再按偏移到目标地址读写。\n"
    "工作流：mem.info →（mem.peek／mem.read 定位与确认）→ mem.write（先 save.backup）。\n"
    "边界：\n"
    "  · ★★ 与 PC 侧 GDB 会话互斥：两者抢同一个调试句柄，不能同时用，同时用会崩机。\n"
    "  · 单次读写上限 3072 字节；只操作当前前台应用进程。\n"
    "  · 改内存有风险：可能崩游戏／损坏存档，动手前先备份。\n"
    "相关工具：save（备份）、screen（看画面）、input（模拟操作）\n"
);
