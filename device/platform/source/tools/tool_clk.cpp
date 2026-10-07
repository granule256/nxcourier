// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 内置工具 · clk：CPU / GPU / MEM 频率设置与读取。
//
// ★ 安全设计（这是本项目里两个"写"操作之一，另一个是 fs.write）：
//   * `clk.set` **必须先读到原值**并存起来；读不到就拒绝设置。
//   * 提供 `clk.restore` 还原原值；`clk.restore all=1` 一次全还原。
//   * 原值只存在内存里，模块重启就没了 —— 所以 help 里会提醒。
//
// 依据：M1 探针在真机上实测 `clkrstInitialize()` 返回 rc=0，可用
// （开工单曾断言"clkrst 在 sysmodule 里崩 ⇒ 时钟做不了"，已被推翻）。
#include "nxc_sdk.hpp"

#include <switch.h>

#include <cstring>

namespace {

struct Core {
    const char* name;
    PcvModuleId module;
    u32 originalHz;
    bool haveOriginal;
};

Core g_cores[] = {
    {"cpu", PcvModuleId_CpuBus, 0, false},
    {"gpu", PcvModuleId_GPU,    0, false},
    {"mem", PcvModuleId_EMC,    0, false},
};
constexpr int kCoreCount = static_cast<int>(sizeof(g_cores) / sizeof(g_cores[0]));

bool ensureService() {
    static bool ready = false;
    if (ready) return true;
    if (R_FAILED(clkrstInitialize())) return false;
    ready = true;  // 常驻：不退出，避免后续每次调用都要重新开
    return true;
}

Core* findCore(const char* name) {
    if (name == nullptr) return nullptr;
    for (int i = 0; i < kCoreCount; ++i) {
        if (std::strcmp(g_cores[i].name, name) == 0) return &g_cores[i];
    }
    return nullptr;
}

bool readHz(PcvModuleId module, u32* out) {
    if (!ensureService()) return false;
    ClkrstSession session;
    if (R_FAILED(clkrstOpenSession(&session, module, 0))) return false;
    const Result rc = clkrstGetClockRate(&session, out);
    clkrstCloseSession(&session);
    return R_SUCCEEDED(rc);
}

bool writeHz(PcvModuleId module, u32 hz) {
    if (!ensureService()) return false;
    ClkrstSession session;
    if (R_FAILED(clkrstOpenSession(&session, module, 0))) return false;
    const Result rc = clkrstSetClockRate(&session, hz);
    clkrstCloseSession(&session);
    return R_SUCCEEDED(rc);
}

// ---------------------------------------------------------------- 方法
void cmdList(const nxc::Args&, nxc::Reply& r) {
    for (int i = 0; i < kCoreCount; ++i) {
        Core& c = g_cores[i];
        u32 hz = 0;
        r.next();
        r.kv("core", c.name);
        if (readHz(c.module, &hz)) r.kvi("hz", hz); else r.kv("hz", "unavailable");
        if (c.haveOriginal) r.kvi("original_hz", c.originalHz);
        // 频率以 MHz 显示更直观
        if (hz != 0) r.kvf("mhz", "%.0f", hz / 1000000.0);
    }
    r.next();
    r.raw("note: set 之前会先记住原值；restore 可还原。原值只存内存，模块重启即丢");
}

void cmdGet(const nxc::Args& a, nxc::Reply& r) {
    Core* c = findCore(a.get("core"));
    if (c == nullptr) { r.fail(400, "core-must-be-cpu-gpu-mem"); return; }
    u32 hz = 0;
    r.kv("core", c->name);
    if (readHz(c->module, &hz)) {
        r.kvi("hz", hz);
        r.kvf("mhz", "%.0f", hz / 1000000.0);
    } else {
        r.kv("hz", "unavailable");
    }
}

void cmdSet(const nxc::Args& a, nxc::Reply& r) {
    Core* c = findCore(a.get("core"));
    if (c == nullptr) { r.fail(400, "core-must-be-cpu-gpu-mem"); return; }

    const long long hz = a.getInt("hz", -1);
    if (hz <= 0) { r.fail(400, "missing-or-bad-hz"); return; }
    // 低于 76MHz 或高于 2.4GHz 都明显是笔误，直接拒掉，比"设进去再说"安全。
    if (hz < 76000000LL || hz > 2400000000LL) { r.fail(400, "hz-out-of-sane-range"); return; }

    // ★ 先记原值；读不到就不允许写。
    if (!c->haveOriginal) {
        u32 original = 0;
        if (!readHz(c->module, &original)) {
            r.fail(409, "cannot-read-original-refusing-to-set");
            return;
        }
        c->originalHz = original;
        c->haveOriginal = true;
    }

    nxc::host::breadcrumb("clk.set %s hz=%lld", c->name, hz);
    if (!writeHz(c->module, static_cast<u32>(hz))) {
        r.fail(500, "set-failed");
        return;
    }

    u32 now = 0;
    r.kv("core", c->name);
    r.kvi("requested_hz", hz);
    r.kvi("original_hz", c->originalHz);
    if (readHz(c->module, &now)) r.kvi("actual_hz", now);
}

void cmdRestore(const nxc::Args& a, nxc::Reply& r) {
    const bool all = a.getInt("all", 0) != 0;
    Core* only = all ? nullptr : findCore(a.get("core"));
    if (!all && only == nullptr) { r.fail(400, "need-core-or-all=1"); return; }

    int restored = 0;
    for (int i = 0; i < kCoreCount; ++i) {
        Core& c = g_cores[i];
        if (!all && &c != only) continue;
        if (!c.haveOriginal) continue;
        if (!writeHz(c.module, c.originalHz)) continue;
        ++restored;
        r.next();
        r.kv("core", c.name);
        r.kvi("restored_hz", c.originalHz);
    }
    r.next();
    r.kvi("restored", restored);
    if (restored == 0) r.raw("note: 没有可还原的原值（从未 set 过，或模块重启过）");
}

// ---------------------------------------------------------------- 自描述元数据
//
// ★★ 规矩：**这个工具的全部知识都写在本文件里** —— 核心不认识 `clk` 是什么，
//    `tools.list` / `tools.doc` 只是把这里的内容汇总出去。**AI 就是靠这些字学会用它。**
//    ⇒ 新增/改动方法时，把参数表与教程一起改（别只改实现）。
// ★ risk: list/get 只读（read）；set/restore 会真的改设备频率（action）。
namespace {

const nxc::Param kCore[] = {
    {"core", "str", true, nullptr, "核心：cpu | gpu | mem（其它值回 400 core-must-be-cpu-gpu-mem）"},
};
const nxc::Param kSet[] = {
    {"core", "str", true,  nullptr, "核心：cpu | gpu | mem"},
    {"hz",   "int", false, "-1",    "目标频率（Hz）；必须 >0，且落在 76MHz..2.4GHz（内部默认 -1 = 视为没给 → 回 400）"},
};
const nxc::Param kRestore[] = {
    {"core", "str", false, nullptr, "要还原的核心 cpu|gpu|mem；给了 all=1 时可省"},
    {"all",  "0|1", false, "0",     "1 = 一次还原全部已记录原值的核心"},
};

const nxc::MethodInfo kInfoList = {
    nullptr, 0, "read",
    "列出 cpu / gpu / mem 的当前频率与已记录的原值。\n"
    "作用: 一眼看清三个核心现在跑多少、以及 set 时记住的原值是多少。\n"
    "★ 频率读的是 clkrst 的**请求值**。\n"
    "★ original_hz 只存在**本模块内存**里 —— 模块重启就没了（那时看不到这一行）。\n"
    "参数: 无。\n"
    "示例: clk.list\n"
    "返回: 每个核心一行 core= hz=（可能带 mhz= original_hz=），末行一句 note。\n"
    "相关: clk.get、clk.set、clk.restore。\n",
};
const nxc::MethodInfo kInfoGet = {
    kCore, 1, "read",
    "读一个核心（cpu/gpu/mem）的当前频率。\n"
    "作用: 只要一个核心的频率时用它，比 clk.list 便宜。\n"
    "参数:\n"
    "  core  cpu | gpu | mem（必填；写其它值回 400 core-must-be-cpu-gpu-mem）\n"
    "示例: clk.get core=cpu\n"
    "返回: core= hz=（读不到时 hz=unavailable），另附 mhz=。\n"
    "相关: clk.list（三个一起看）、clk.set。\n",
};
const nxc::MethodInfo kInfoSet = {
    kSet, 2, "action",
    "设置 CPU / GPU / MEM 频率。\n"
    "作用: 超频 / 降频。★ 这是会改设备状态的 action。\n"
    "★ 安全设计: set **必须先读到原值**并存下来；**读不到原值就直接拒设**\n"
    "  （409 cannot-read-original-refusing-to-set）—— 这样 clk.restore 才一定还原得回去。\n"
    "★ 原值只存在**内存**里 —— 模块重启就丢（丢了之后 restore 无事可做）。\n"
    "参数:\n"
    "  core  cpu | gpu | mem（必填）\n"
    "  hz    目标频率，单位 Hz，必须 >0；合理范围 **76MHz..2.4GHz**\n"
    "        （写小了/写大了都回 400 hz-out-of-sane-range）。\n"
    "示例: clk.set core=cpu hz=1785000000\n"
    "返回: core= requested_hz= original_hz= actual_hz=（actual 是设完再读回的请求值）。\n"
    "注意: 改频率属于 action；用完记得 clk.restore（否则一直生效到模块重启）。\n"
    "相关: clk.restore、tele.freq（看请求值）、tele.temp（超频后看温度）。\n",
};
const nxc::MethodInfo kInfoRestore = {
    kRestore, 2, "action",
    "把之前 set 记住的原值还原回去。\n"
    "作用: 超频/降频用完，恢复原来的频率。\n"
    "参数:\n"
    "  core  只还原这一个核心（cpu|gpu|mem）\n"
    "  all   1 = 一次还原全部已记录的核心（给了 all=1 可省 core）\n"
    "★ 两个都不给会回 400 need-core-or-all=1。\n"
    "★ 只还原**有记录原值**的核心；没 set 过、或模块重启过（内存丢了）的会被跳过。\n"
    "示例: clk.restore core=cpu\n"
    "      clk.restore all=1\n"
    "返回: 每个被还原的核心一行 core= restored_hz=，末行 restored=<还原了几个>。\n"
    "注意: 属于 action；还原值同样读的是 clkrst 请求值。\n"
    "相关: clk.set、clk.list（看还有没有原值）。\n",
};

}  // namespace

const nxc::Method kMethods[] = {
    {"list",    "列出 cpu/gpu/mem 的当前频率与已记录的原值", cmdList, &kInfoList},
    {"get",     "读一个核心的频率：core=cpu|gpu|mem", cmdGet, &kInfoGet},
    {"set",     "设频率：core=cpu|gpu|mem hz=<赫兹>（先记原值，读不到原值则拒设）", cmdSet, &kInfoSet},
    {"restore", "还原原值：core=… 或 all=1", cmdRestore, &kInfoRestore},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_clk, "clk", "频率设置与读取", kMethods,
    "CPU / GPU / MEM 频率的设置与读取。\n"
    "安全设计（本项目两个「写」操作之一，另一个是 fs.write）:\n"
    "  · set 必须先读到原值并存起来 —— 读不到原值就**拒设**（409）\n"
    "  · restore 还原原值；all=1 一次全还原\n"
    "  · ★ 原值只存在**内存**里 —— 模块重启就丢（丢了之后 restore 无事可做）\n"
    "  · set 的 hz 有合理范围校验：76MHz..2.4GHz，越界回 400\n"
    "什么时候用它: 需要超频/降频跑负载、跑完再还原时。\n"
    "约定: 频率都是 clkrst 的**请求值**（不是硬件瞬时值）；改频率属于 action（会改设备状态）。\n"
    "相关工具: tele（看频率/温度/性能模式）、power（电源）"
);
