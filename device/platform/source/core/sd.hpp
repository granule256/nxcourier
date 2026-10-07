// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// SD 访问总开关 + 安全打开 —— **A/B 测试用**。
//
// ## 为什么要有"总开关"这层东西（2026-10-05 深夜的教训）
//
// 排查「睡眠后 omm 崩」时，曾把 SD 改成「按需挂载」（空闲时卸掉）。
// 结果**主机直接进不去系统**。根因：
//
//   ★★★ `fsdevUnmountAll()` 之后，**任何 `fopen` 都会在 libnx 的 `_open_r` 里
//       空指针解引用崩溃** —— 不是「返回 NULL 让调用方处理」。
//
// 所以"禁用 SD"**不能只是"不挂载"** —— 只要还有一行 `fopen` 被走到，就是崩溃。
//
// ## 这一版的写法
//
// **一个编译期开关 `kEnabled` + 一个 `open()` 包装**：
//   * 开关为 false 时，`open()` **立刻返回 nullptr，绝不落到 `fopen`**；
//   * 所有文件访问点统一改用它 ⇒ 禁用时全链路**静默降级**，不会崩。
//   * 而 `readBootCount` / `writeWholeFile` 本来就已经判了 NULL，所以无需额外改动。
//
// ## 当前状态
//
// `kEnabled = true` —— 正常态。
// ★ 2026-10-05 A/B 测试结论：**SD 关掉之后 omm 照样崩** ⇒ SD 不是元凶，已恢复。
//   这个开关留着：以后要做"禁用全部文件访问"的对照实验时直接改这里。
#pragma once

#include <cstdio>

namespace nxc {
namespace sd {

// ★★★ A/B 测试开关。false = SD 全禁用（不挂载、不读写）。
//   ★ 2026-10-06 H-D′（摘掉 SD + `fs` 会话）**实测仍然崩** ⇒ 这一条也排除了。
//     现已**恢复 true** —— 做 H-B（断网版）时**特意保留 SD/`fs`**，好让面包屑能写下来，
//     保持"只摘网络"这个单变量。
constexpr bool kEnabled = true;

// ★★★ 2026-10-06 实验 H-D′：**连 `fs` 服务会话都不开**。
//
//   为什么怀疑它：`Fs` 是 PSC 的**电源模块之一（`PscPmModuleId_Fs` = 27）**，
//   也就是"睡眠时 PSC 要挨个通知进 ReadySleep 并等 Ack"的那类角色；
//   而我们的故障正是 **SPSM 报「电源切换派发超时」**（`0x2A5`）—— 说明**有人没按时 Ack**。
//   我们**从开机把 `fs` 会话握到进程结束**，是"长期持有的资源"里**唯一还没测过**的一个：
//     · socket 配置 —— 2026-10-06 已用【libnx 默认配置】实测排除（仍崩，签名逐项一致）
//     · SD 挂载     —— A/B 已排除（不挂载仍崩）
//     · `sm` 会话   —— 已排除（实测 `sm_at_entry=0` 仍崩）
//     · boot2 位置 / NPDM 结构 —— 已排除（多个成熟模块同样配置）
//   ⇒ 剩下的就是**这个会话本身**。
//
//   ★★ 与 `kEnabled` 的约束：**`kFsSessionEnabled=false` 时必须同时 `kEnabled=false`**。
//      没有会话就挂不上 SD，而"未挂载时任何文件访问都是【崩溃】而不是返回 NULL"
//      （2026-10-05 让主机进不去系统的那次）—— 必须靠 `kEnabled` 的短路兜住。
//
//   ★★ 还有一件**必须在别处一起做的事**（见 `protocol.cpp::dispatch` 里的说明）：
//      `fs` / `save` 两个工具里有**没用 `sd::open` 包装的裸调用**
//      （`opendir`/`readdir`/`::stat`/`::mkdir`/`::remove`，以及直接要 `fs` 服务会话的
//      `fsOpenSaveDataInfoReader` / `fsOpenSaveDataFileSystem`）⇒ 光改这一行会留下地雷，
//      所以那两个工具在 SD 禁用期间被**在唯一入口挡掉**。
//   ★★ 2026-10-06 H-D′ 实测结论：**摘掉 `fs` 会话仍然崩**。证据是内存环形日志
//      `main: enter, tools=14, mask=0x0` —— `mask=0x0` 说明 `kInitFs` 和 `kInitSd`
//      都没置上，也就是"这一支真的跑起来了"，而 omm 照样在睡眠时崩。
//      ⇒ **`fs` 会话与 SD 双双排除**。现已**恢复 true**。
constexpr bool kFsSessionEnabled = true;

// 安全的文件打开：禁用时直接返回 nullptr，**绝不调用 `fopen`**。
// （为什么这个"绝不"很重要：见上面抬头 —— 未挂载时 `fopen` 是崩溃而非返回 NULL。）
inline FILE* open(const char* path, const char* mode) {
    if (!kEnabled) return nullptr;
    return std::fopen(path, mode);
}

}  // namespace sd
}  // namespace nxc
