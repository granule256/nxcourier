// 内置工具 · probe：能力探针 —— 首期真正的主角。
//
// 为什么要有它：整个项目的价值 = sysmodule 的能力矩阵有多长，而这个矩阵**现在是坏数据**
// （开工单判"做不了"的几个能力，已被 sys-clk 这类现成 sysmodule 反驳）。所以先把它实测出来。
//
// ★ 崩溃定位机制（sysmodule 没有 stdout，崩了就是黑屏）：
//   * 每次尝试**之前**，先往 SD 上的 probe_results.txt **追加**一行 "… try"；
//   * 崩机后这行就是文件最后一行 ⇒ 凶手就是它；
//   * 结果文件是**追加**的，所以崩溃后重启仍然留着历史，不会像内存日志那样丢。
//
// 为什么把服务分四组：组 4 是"预期会崩"的（nv / grcd / usbHs 等），必须放最后，
// 免得它们把前面几组的结论一起带走。
#include "nxc_sdk.hpp"
#include "../core/sd.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kResultPath = "/config/nxc/probe_results.txt";
constexpr int kMaxResultBytes = 4096;
// ★ 单次最多回几行（2026-10-06 晚随分页一起加）。这份结果文件是**追加**写的、会一直长大，
//   所以"取回"必须能翻页，而不是赌它永远装得进一条应答。
constexpr int kMaxResultLines = 200;

struct Item {
    const char* group;   // "1".."4"
    const char* name;
    Result (*init)();
    void (*deinit)();
    bool knownFatal;     // ★ 已实测「一调就死」的服务：默认拒绝跑，必须显式 force=1
};

// 组 1 基础：几乎肯定全通（多数已在开工单里实测过）。
// 组 2 性能与传感器：sys-clk 是现成 sysmodule 且做到了，所以这里预期通。
// 组 3 系统与内容：逐个试，结论未知。
// 组 4 高风险：预期崩 —— 崩了正好把"做不到"这件事钉死。
// 注意：fs 与 socket **不在这里** —— 它们已经被核心初始化并正在使用，
// 探测它们等于把自己的传输层拆掉。
const Item kItems[] = {
    {"1", "spl",          splInitialize,       splExit,       false},
    {"1", "set",          setInitialize,       setExit,       false},
    {"1", "setsys",       setsysInitialize,    setsysExit,    false},

    {"2", "clkrst",       clkrstInitialize,    clkrstExit,    false},
    {"2", "pcv",          pcvInitialize,       pcvExit,       false},
    {"2", "apm",          apmInitialize,       apmExit,       false},
    {"2", "tc",           tcInitialize,        tcExit,        false},
    {"2", "i2c",          i2cInitialize,       i2cExit,       false},
    {"2", "bpc",          bpcInitialize,       bpcExit,       false},
    {"2", "psm",          psmInitialize,       psmExit,       false},
    {"2", "pminfo",       pminfoInitialize,    pminfoExit,    false},
    {"2", "pscm",         pscmInitialize,      pscmExit,      false},
    {"2", "fan",          fanInitialize,       fanExit,       false},
    {"2", "gpio",         gpioInitialize,      gpioExit,      false},

    {"3", "ns",           nsInitialize,        nsExit,        false},
    {"3", "pmshell",      pmshellInitialize,   pmshellExit,   false},
    {"3", "pmdmnt",       pmdmntInitialize,    pmdmntExit,    false},
    {"3", "lbl",          lblInitialize,       lblExit,       false},
    {"3", "ldrshell",     ldrShellInitialize,  ldrShellExit,  false},
    {"3", "ldrdmnt",      ldrDmntInitialize,   ldrDmntExit,   false},
    {"3", "ldrro",        ldrRoInitialize,     ldrRoExit,     false},
    {"3", "ro1",          ro1Initialize,       ro1Exit,       false},
    {"3", "ncm",          ncmInitialize,       ncmExit,       false},
    {"3", "capsu",        capsuInitialize,     capsuExit,     false},
    {"3", "capssu",       capssuInitialize,    capssuExit,    false},
    {"3", "pctl",         pctlInitialize,      pctlExit,      false},
    {"3", "miiimg",       miiimgInitialize,    miiimgExit,    false},
    {"3", "spsm",         spsmInitialize,      spsmExit,      false},

    {"4", "nv",           nvInitialize,        nvExit,        true},   // ★★ 实测：一调就死
    {"4", "grcd",         grcdInitialize,      grcdExit,      false},
    {"4", "hidsys",       hidsysInitialize,    hidsysExit,    false},
    {"4", "usbHs",        usbHsInitialize,     usbHsExit,     false},
    {"4", "mmu",          mmuInitialize,       mmuExit,       false},

    // 组 5：★「输入」与「画面」两条最有价值的路 —— 通了就能各做一个工具。
    //   hiddbg → 虚拟手柄 / 触摸 / 键盘 / 鼠标模拟（AI 从"能看"变"能动"）
    //   capssc → 系统截图服务（JPEG + 原始画面流）（让 AI"睁眼"）
    {"5", "hiddbg",       hiddbgInitialize,    hiddbgExit,    false},
    {"5", "capssc",       capsscInitialize,    capsscExit,    false},
    {"5", "time",         timeInitialize,      timeExit,      false},
};

const int kItemCount = static_cast<int>(sizeof(kItems) / sizeof(kItems[0]));

const Item* findItem(const char* name) {
    if (name == nullptr) return nullptr;
    for (int i = 0; i < kItemCount; ++i) {
        if (std::strcmp(kItems[i].name, name) == 0) return &kItems[i];
    }
    return nullptr;
}

// 追加一行到结果文件；同时更新面包屑（崩了就停在最后那条 try 上）。
void record(const char* line) {
    FILE* f = nxc::sd::open(kResultPath, "ab");
    if (f != nullptr) {
        std::fputs(line, f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    nxc::host::log("%s", line);
}

// 试一个服务。**返回它的 rc**，让调用方自己决定"失败要不要当成失败"：
//   `probe.try`（单独试一个）⇒ 失败就回 ERR；`probe.group`（跑一整组）⇒ 失败是**预期数据**，
//   必须继续跑完并把每条都带回来（那时候回 ERR 会把整组结果一起丢掉）。
//   ★ 返回值 0 表示"不需要当成失败"——包括 `knownFatal` 被主动拒跑那种情况（那是设计如此）。
Result runOne(const Item& it, nxc::Reply& r, bool force) {
    // ★ 已知致命的服务默认拒跑：2026-10-05 实测 `nvInitialize()` 会把整个模块直接搞死
    //   （连上去只会得到"连接被重置"，之后必须重启主机才能恢复）。别让后来人再踩一遍。
    if (it.knownFatal && !force) {
        r.kv("svc", it.name);
        r.kv("group", it.group);
        r.kv("result", "refused");
        r.raw("why: KNOWN-FATAL —— 实测该服务会直接杀死模块，重启才能恢复");
        r.raw("override: 确要再试，加 force=1（后果自负）");
        return 0;
    }

    char line[192];

    // ★ 先落"我要试它了"，再真的去试 —— 顺序反过来就失去定位能力。
    std::snprintf(line, sizeof(line), "boot=%d group=%s svc=%s try",
                  nxc::host::bootCount(), it.group, it.name);
    nxc::host::breadcrumb("probe: %s", line);
    record(line);

    const Result rc = it.init();

    if (R_SUCCEEDED(rc)) {
        it.deinit();  // 试通就立刻收掉，不要长期占着别人的会话
        std::snprintf(line, sizeof(line), "boot=%d group=%s svc=%s OK",
                      nxc::host::bootCount(), it.group, it.name);
    } else {
        std::snprintf(line, sizeof(line), "boot=%d group=%s svc=%s FAIL rc=0x%08X",
                      nxc::host::bootCount(), it.group, it.name,
                      static_cast<unsigned>(rc));
    }
    record(line);

    r.kv("svc", it.name);
    r.kv("group", it.group);
    r.kvf("rc", "0x%08X", static_cast<unsigned>(rc));
    r.kv("result", R_SUCCEEDED(rc) ? "ok" : "fail");
    return rc;
}

void cmdList(const nxc::Args& a, nxc::Reply& r) {
    const char* only = a.get("group");
    int n = 0;
    for (int i = 0; i < kItemCount; ++i) {
        if (only != nullptr && std::strcmp(only, kItems[i].group) != 0) continue;
        r.next();
        r.kv("group", kItems[i].group);
        r.kv("svc", kItems[i].name);
        if (kItems[i].knownFatal) r.kv("KNOWN_FATAL", "1");
        ++n;
    }
    r.next();
    r.kvi("count", n);
    r.raw("hint: 用 probe.group n=<组号> 或 probe.try svc=<名字> 真跑一遍");
}

void cmdTry(const nxc::Args& a, nxc::Reply& r) {
    const char* svc = a.get("svc");
    if (svc == nullptr) svc = a.get("_0");
    const Item* it = findItem(svc);
    if (it == nullptr) { r.fail(404, "no-such-service"); return; }
    // ★★★ 2026-10-06 晚：`probe.try` 是"就试这一个" ⇒ **失败要回 ERR**。
    //   原来回的是 `OK rc=0x… result=fail`，调用方不看字段就会以为"服务能开"。
    //   ★ 注意与 `probe.group` 的区别（见 runOne 的注释）：组跑不能因为一条失败就整组报废。
    const Result rc = runOne(*it, r, a.getInt("force", 0) != 0);
    if (R_FAILED(rc)) {
        r.failf(500, "svc-init-failed svc=%s rc=0x%08X", it->name, static_cast<unsigned>(rc));
    }
}

// 整组跑：组内按顺序试；崩了就靠结果文件的最后一行定位。
void cmdGroup(const nxc::Args& a, nxc::Reply& r) {
    const char* g = a.get("n");
    if (g == nullptr) g = a.get("_0");
    if (g == nullptr) { r.fail(400, "missing-group"); return; }
    const bool force = a.getInt("force", 0) != 0;

    nxc::host::breadcrumb("probe: group %s begin", g);
    int n = 0;
    for (int i = 0; i < kItemCount; ++i) {
        if (std::strcmp(g, kItems[i].group) != 0) continue;
        r.next();
        (void)runOne(kItems[i], r, force);  // ★ 组跑：单条失败是**预期数据**，不回 ERR（见 runOne 注释）
        ++n;
    }
    r.next();
    r.kvi("count", n);
    r.raw("note: 若本次没回包，看 SD 上 probe_results.txt 的最后一行");
}

// ★★★ 2026-10-06 晚：**加分页**（`offset=` / `lines=`）。
//
//   原来是一次把整个结果文件按行塞进一条应答，实测直接回 `ERR 500 reply-too-large`：
//   文件只有 1958 字节（**远小于** 32KB 的应答上限）却还是失败 —— 因为它撞的是**行数**上限
//   （那时是 48 行，而这份文件约 60+ 行）。★ 顺带纠正一个当时的误判：这不是"字节超了"。
//
//   行数上限虽然已从 48 放宽到 256，但这份文件是**一直追加**的（每次 probe 都往上写），
//   迟早还会撑爆 —— 所以这里必须能翻页，不能靠"上限调大"拖。
//   用法：`probe.results` 取第一页；之后照回包里的 `next_offset` 往后取，`eof=1` 表示到底了。
void cmdResults(const nxc::Args& a, nxc::Reply& r) {
    long long offset = a.getInt("offset", 0);
    if (offset < 0) offset = 0;
    long long want = a.getInt("lines", 40);
    bool wantAdjusted = false;
    if (want <= 0 || want > kMaxResultLines) { want = 40; wantAdjusted = true; }

    FILE* f = nxc::sd::open(kResultPath, "rb");
    if (f == nullptr) { r.fail(404, "no-results-yet"); return; }

    static char buf[kMaxResultBytes + 1];
    const size_t n = std::fread(buf, 1, kMaxResultBytes, f);
    std::fclose(f);
    buf[n] = '\0';

    // 先把总行数数出来（文件本来就不大，一次读进来最简单）。
    long long total = 0;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n') ++total;
    }
    if (n > 0 && buf[n - 1] != '\n') ++total;   // 最后一行没有换行符，也算一行

    r.kvi("lines", total);        // ★ 老字段，语义不变（文件总行数）
    r.kvi("total_lines", total);  // ★ 新名字，同一个值，写清楚免得误读
    r.kvi("offset", offset);
    r.kvi("lines_asked", want);
    r.kv("path", kResultPath);

    if (offset >= total) {
        r.kvi("shown", 0);
        r.kvi("next_offset", total);
        r.kvi("eof", 1);
        r.raw("note: offset 已到文件末尾；结果文件是累积的，越靠后越新");
        return;
    }

    long long idx = 0, shown = 0;
    char* save = nullptr;
    for (char* line = ::strtok_r(buf, "\n", &save); line != nullptr;
         line = ::strtok_r(nullptr, "\n", &save), ++idx) {
        if (idx < offset) continue;
        if (shown >= want) break;
        r.raw(line);
        ++shown;
    }

    r.kvi("shown", shown);
    r.kvi("next_offset", offset + shown);
    r.kvi("eof", (offset + shown) >= total ? 1 : 0);
    // ★ 与 log.dump/title.list/save.list 一致：越界不静默
    if (wantAdjusted) {
        r.next();
        r.kv("note_params", "lines 越界（允许 1..200），已按默认 40 执行");
    }
    r.raw("note: 结果文件是累积追加的；继续取下一段用 offset=<上面的 next_offset>");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
// ★ 本工具的全部知识都写在这里 —— tools.list / tools.doc 只是把这里的内容汇总出去。
const nxc::Param kListGroup[] = {
    {"group", "str", false, nullptr, "只列某一组 1|2|3|4|5；缺省列全部"},
};
const nxc::Param kTry[] = {
    {"svc",   "str", true,  nullptr, "服务名（如 spl / pcv / nv）。也可用位置参数：probe.try spl"},
    {"force", "0|1", false, "0",     "1=强制试 KNOWN-FATAL 服务（如 nv，实测会弄死模块）"},
};
const nxc::Param kGroup[] = {
    {"n",     "str", true,  nullptr, "组号 1|2|3|4|5。也可用位置参数：probe.group 2"},
    {"force", "0|1", false, "0",     "1=连组内 KNOWN-FATAL 服务也试（后果自负）"},
};
const nxc::Param kResults[] = {
    {"offset", "int", false, "0",  "从第几行开始取（默认从头）"},
    {"lines",  "int", false, "40", "本页最多回几行（1..200，越界回落 40）"},
};

const nxc::MethodInfo kInfoList = {
    kListGroup, 1, "read",
    "作用：列出可探的服务清单 —— 只列不跑，安全。\n"
    "参数说明：group 可选，1..5 只列某一组；缺省列全部。组 4 里有已知致命的 nv（会带 KNOWN_FATAL=1）。\n"
    "五组大致分工：组 1 基础、组 2 性能与传感器、组 3 系统与内容、组 4 高风险（含 nv）、"
    "组 5 输入与画面（hiddbg / capssc）。\n"
    "示例：probe.list\n"
    "      probe.list group=4\n"
    "注意：只读、无副作用。真正去试要用 probe.try / probe.group（那才是 danger）。\n"
    "返回：每行 group= svc=（致命服务多一个 KNOWN_FATAL=1）末行 count= 与一行 # 提示。\n"
    "相关：probe.try probe.group probe.results\n",
};
const nxc::MethodInfo kInfoTry = {
    kTry, 2, "danger",
    "作用：真去调用一个服务的 Initialize()，看它能不能开起来。\n"
    "★ danger 级：会开 / 关服务会话，个别服务可能把整个模块搞死（实测 nv 就是）。\n"
    "参数说明：\n"
    "  svc    必填，服务名（probe.list 给的那个）。也可写成位置参数 probe.try spl。\n"
    "  force  可选，默认 0。对 KNOWN-FATAL 服务（如 nv）默认【拒跑】、回 result=refused 与原因；"
    "要真试必须显式 force=1（后果自负）。\n"
    "★ 崩溃定位：试之前会先往 SD 的 probe_results.txt 追一行「… try」；"
    "崩了以后那行就是文件最后一行 —— 凶手就是它。结果文件是追加的、重启不丢。\n"
    "示例：probe.try svc=spl\n"
    "      probe.try svc=nv force=1\n"
    "注意：单独试一个、失败会回 ERR（不像整组跑那样把失败当数据）；先 probe.list 看清楚再试。\n"
    "返回：svc= group= rc=0x… result=ok|fail|refused\n"
    "相关：probe.list probe.group probe.results\n",
};
const nxc::MethodInfo kInfoGroup = {
    kGroup, 2, "danger",
    "作用：按组跑一整组服务探针。\n"
    "★ danger 级：会逐个开关服务会话，组 4 含已知致命的 nv。\n"
    "参数说明：\n"
    "  n      必填，组号 1|2|3|4|5。也可写成位置参数 probe.group 2。\n"
    "  force  可选，默认 0；=1 表示连组内 KNOWN-FATAL 服务也试。\n"
    "★ 与 probe.try 的关键区别：整组跑时【单条失败是预期数据】、不回 ERR，"
    "必须把每条都带回来（不然一条失败会把整组结论一起丢掉）。\n"
    "★ 若本次没回包，就去看 SD 上 probe_results.txt 的最后一行，它指向凶手。\n"
    "示例：probe.group n=2\n"
    "      probe.group 5\n"
    "注意：组 4 / 组 5 可能崩机、需要重启主机 —— 跑之前先想清楚。\n"
    "返回：每条一行 svc= group= rc= result=，末行 count= 与一行 # 提示。\n"
    "相关：probe.list probe.try probe.results\n",
};
const nxc::MethodInfo kInfoResults = {
    kResults, 2, "read",
    "作用：读回 SD 上的 probe_results.txt（累积结果），带分页。\n"
    "★ 结果文件是【一直追加】的 —— 每次 probe 都往上写，所以必须能翻页，不能赌它装得进一条应答。\n"
    "参数说明：\n"
    "  offset 可选，从第几行开始取（默认 0）。\n"
    "  lines  可选，本页最多回几行，默认 40、上限 200（越界回落 40）。\n"
    "用法：先取第一页，再拿回包里的 next_offset 当下一次的 offset，直到 eof=1。\n"
    "示例：probe.results\n"
    "      probe.results offset=40 lines=80\n"
    "注意：只读、无副作用；文件不存在时回 404 no-results-yet（说明还没跑过 probe）。\n"
    "返回：lines=（文件总行数）total_lines= offset= lines_asked= path= "
    "shown= next_offset= eof= 以及每行一条结果。\n"
    "相关：probe.try probe.group\n",
};

const nxc::Method kMethods[] = {
    {"list",    "列出要试的服务：可选 group=1|2|3|4|5", cmdList,    &kInfoList},
    {"try",     "试一个服务：svc=<名字> [force=1 覆盖 KNOWN-FATAL 保护]", cmdTry, &kInfoTry},
    {"group",   "跑一整组：n=1|2|3|4|5（组4含已知致命的 nv，默认跳过；组5=输入与画面）", cmdGroup, &kInfoGroup},
    {"results", "读回 SD 上的累积结果文件（分页）：可选 offset=<行号> lines=<最多几行，默认40，上限200>", cmdResults, &kInfoResults},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_probe, "probe", "能力探针（M1 主角）", kMethods,
    "实测 sysmodule 到底能开哪些系统服务 —— 组 5 的输入（hiddbg）与画面（capssc）是两条最有价值的路。\n"
    "什么时候用它：\n"
    "  · 评估某能力做不做得起来（先 probe.list 看清单、再 probe.group 跑一组）\n"
    "  · 追崩溃：结果文件是追加的、崩了就停在最后一行 try 上\n"
    "★ nv（GPU 驱动）是禁区：实测一调就把模块搞死、必须重启主机 —— 默认拒跑，要试得显式 force=1。\n"
    "★ 组 4 全是高风险、组 5 是输入与画面 —— 跑之前先想清楚，崩了要重启主机。\n"
    "★ 结果读回用 probe.results（分页），文件在 SD 的 /config/nxc/probe_results.txt。\n"
    "相关工具：log（日志）、net（网络）"
);
