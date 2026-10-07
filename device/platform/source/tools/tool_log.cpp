// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 内置工具 · log：把内存环形日志取走。
//
// ★ 这是 sysmodule 唯一的"打印"手段：没有 stdout，日志只能存在内存里等 PC 来取。
// 崩溃时环形日志（在 RAM 里）会丢，但面包屑文件（在 SD 上）不会 —— 两者互补。
#include "nxc_sdk.hpp"

namespace {

void cmdDump(const nxc::Args& a, nxc::Reply& r) {
    long long lines = a.getInt("lines", 40);
    // ★ 60 是本方法**自己定的**取值上限（help 也写 1..60），与应答容量无关。
    //   原来这里的注释写"一整条应答装不下更多" —— 那是行数上限只有 48 时的实情，
    //   2026-10-06 晚把上限提到 256 之后这句已经不成立（60 行完全装得下）。
    //   ★ 越界/0 仍按老规矩**回落默认 40**（改成报错会让 `lines=0 表示用默认` 这种常见
    //     写法直接失败），但**不再静默** —— 见下面的 note_params。
    bool adjusted = false;
    if (lines <= 0 || lines > 60) { lines = 40; adjusted = true; }

    nxc::host::dumpLog(r, static_cast<int>(lines));

    // ★★★ 2026-10-06 晚：越界值"静默换成默认"这种病 ——
    //   "我按你要的做了"和"我偷偷换了参数"原来长得一模一样（`lines=999` 实际按 40 跑）。
    //   ★ `r.next()` 必须放在前面：`dumpLog` 最后一行是 `#…` 自由文本、**没有换行结尾**，
    //     直接补 kv 会把提示语拼到那行日志的屁股后面。
    if (adjusted) {
        r.next();
        r.kv("note_params", "lines 越界（允许 1..60），已按默认 40 执行");
    }
}

void cmdInfo(const nxc::Args&, nxc::Reply& r) {
    r.kv("heartbeat", nxc::host::heartbeatPath());
    r.kvi("boot", nxc::host::bootCount());
    r.kv("note", "环形日志在内存里，崩溃即丢；面包屑文件在SD上，崩溃后仍可读");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
// ★ 本工具的全部知识都写在这里 —— tools.list / tools.doc 只是把这里的内容汇总出去。
const nxc::Param kDump[] = {
    {"lines", "int", false, "40", "取多少行 1..60；越界回落 40 并在回包里告警"},
};

const nxc::MethodInfo kInfoDump = {
    kDump, 1, "read",
    "作用：取走内存环形日志 —— sysmodule 没有 stdout，这是它唯一的「打印」出口。\n"
    "参数说明：lines 可选，默认 40、范围 1..60；越界 / 0 会【回落 40】并在回包里加一行 note_params 告警"
    "（不像老版本那样静默换默认）。\n"
    "★ 环形日志在【内存】里、崩机即丢；持久的是 SD 上的面包屑 boot_progress.txt 与黑匣子 watch_boot<N>.txt。\n"
    "示例：log.dump\n"
    "      log.dump lines=20\n"
    "注意：只读、无副作用。\n"
    "返回：count= boot= 然后每行一条 # 日志。\n"
    "相关：log.info（日志位置说明） probe.results（SD 上的持久结果）\n",
};
const nxc::MethodInfo kInfoInfo = {
    nullptr, 0, "read",
    "作用：说明各类日志 / 持久记录分别放在哪里、有什么区别。\n"
    "返回：heartbeat=心跳文件路径 boot=开机次数 与一行 # 说明。\n"
    "★ 这些位置：\n"
    "  · 内存环形日志 —— 不落盘、崩机即丢，用 log.dump 取。\n"
    "  · 面包屑 /config/nxc/boot_progress.txt —— 覆盖写、崩后留下的最后一步。\n"
    "  · 心跳 /config/nxc/heartbeat.txt —— 存开机次数（boot=）；文件没了计数会从 1 重数。\n"
    "  · 黑匣子 /config/nxc/watch_boot<N>.txt —— 文件名带开机号、崩过不覆盖。\n"
    "示例：log.info\n"
    "注意：只读、无副作用。\n"
    "返回：heartbeat= boot= note=\n"
    "相关：log.dump identify.id\n",
};

const nxc::Method kMethods[] = {
    {"dump", "取走内存环形日志（lines=1..60，默认40）", cmdDump, &kInfoDump},
    {"info", "说明日志存放位置与两者区别", cmdInfo, &kInfoInfo},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_log, "log", "日志读取", kMethods,
    "取走平台的内存环形日志、并说明各类记录存在哪。\n"
    "什么时候用它：\n"
    "  · 行为不对时先 log.dump 看平台自己说了什么\n"
    "  · 想确认崩溃前最后一步 —— 看 SD 上 boot_progress.txt 的最后一行\n"
    "边界：全部只读、无副作用。\n"
    "★ 内存环形日志崩机即丢；要活过崩溃只能靠 SD 上的面包屑与黑匣子。\n"
    "相关工具：identify（身份） probe（能力探针）"
);
