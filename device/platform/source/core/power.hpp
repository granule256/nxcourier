// 电源状态通知 —— 让模块在"系统要睡了"的时候**先把 socket 全关掉**。
//
// ## 为什么需要它（2026-10-06 实测结论）
//
// 一整天的对照实验把规律钉死了：
//   · `bdsonly`（只建 bsd 会话，**没有任何 socket**）          ⇒ **不崩**
//   · `listen`（长期监听口）                                   ⇒ 崩
//   · `acceptonly`（监听口已 close，**只留一条已建立的连接**）  ⇒ **也崩**
//   ⇒ ★★★ **睡眠那一刻，只要我们【有任何】打开的 socket，`omm` 就会崩**（`0x2A5` = SPSM 的
//     「PSC 派发电源切换超时」）。**不是 `listen` 状态的问题，是"有 socket"这件事本身。**
//
// ## ★★★ 2026-10-06 第二轮：`0x2A5` 不是"没人 Ack"，而是"被我们挡住了"
//
// 我们**从不注册 PmModule**，而 `0x2A5` 是"某个 PmModule 没按时 Ack"。
// 睡眠时要拆网络栈（`WlanSockets`/`Nifm`/`Socket` 都是 PmModule，都要 Ack），
// 而我们**从开机把一个 bsd 客户端 + socket 握到死** ⇒ 那个拆除流程被拖住 ⇒ 超时 ⇒
// `omm`（PmModule 18、天然站在睡眠流程中间）吃到该错误后崩掉。
//
// ## ★★★ 所以修法：自己进入"电源确认圈"，睡之前把 socket 收干净
//
// 这**不是**我们的原创做法：两台独立参照都是这么干的 ——
//   · `sys-con`（`source/Sysmodule/source/psc_module.cpp`）：注册 `PscPmModuleId(126)`、
//     deps `{Fs}`、`autoclear=true`，睡眠状态里干活、然后 `Acknowledge`。
//   · **`MissionControl`**：boot2 模块，初始化服务清单里就有 `pscm`
//     ⇒ ★ **证明"boot2 模块在开机期注册 PSC"这件事本身是安全的。**
//
// ## ★★★ 上一版为什么会"卡开机"：**我们把假应答灌进了 PSC 的状态机**（已修）
//
// 读了 libnx 的 `nx/source/services/psc.c` 才看清：
//   `pscPmModuleGetRequest()` **自己不阻塞** —— 它只是发一条 IPC（cmd 1）就返回。
//   而上一版写成「100ms 轮询 `GetRequest`，成功就 `Acknowledge`」
//   ⇒ 在没有待处理请求时也会拿到一个值（`state=0=Awake`）并**盲目应答**
//   ⇒ 开机期往 PSC 状态机里灌假应答 ⇒ **开机时卡在 logo**。
// ⇒ 现在**一字不差地照抄 sys-con 的等待方式**：
//   `waiterForEvent(&module.event)` + `waitSingle(waiter, UINT64_MAX)` **先等到事件**，
//   再 `GetRequest`、再干活、再 `Acknowledge`。
//
// ## 两条必须守住的设计纪律
//
// 1. ★★★ **应答必须等"真的关完了"** —— PSC 等的是 Ack，如果**先应答再关**，
//    系统会在我们还没关的时候继续往下走 ⇒ 等于没修。
//    ⇒ 握手：通知线程置 `sleeping` 并**等主线程回 `socketsClosed`**（上限 900ms，超时也照答）。
// 2. ★★★ **fd 只能由主线程关** —— 跨线程关 fd 会和主线程的 `poll` 竞争。
//    ⇒ 本文件**绝不碰任何 fd**，只维护标志位。
// 3. ★★★ **通知线程绝不碰 SD / `sm` / `time`** —— 它只调 `log::ring()`（纯内存）。
//    为什么：`log::watch()` 会临时开关 `sm` 与 `time`，而这条线程与主线程并发
//    ⇒ 会把主线程正在用的 sm 关掉（这个坑 2026-10-06 已经咬过一次，见 `log.cpp` 的注释）。
//
// ## 代价与副作用
//
// * 多一条线程（我们原来刻意单线程）。★ 只用标志位与 PSC 调用。
// * **睡眠期间 PC 侧会断**（那时没有任何 socket）⇒ 醒来重新监听，PC 侧要重连。
#pragma once

namespace nxc {
namespace power {

// ★★★ 三档，从 SD 上 `/config/nxc/psc_mode.txt` 读（改一行字 + 重启即切换，不用重编）。
enum class Mode {
    Off = 0,       // 不注册 PSC（**缺省，安全优先**）
    Observe = 1,   // 注册 + 记录每次状态 + 立刻 Ack，**但不动 socket**
                   //   ⇒ 隔离实验：只验证"注册 PSC 本身会不会卡开机/卡睡眠"
    Close = 2,     // 注册 + 睡眠时**关掉全部 socket**、醒来重开（真正的修法）
};

inline Mode g_mode = Mode::Off;

const char* modeName(Mode m);

// 解析 `/config/nxc/psc_mode.txt` 的内容（大小写不敏感、容忍空白）。不认识返回 false。
bool parseMode(const char* text, Mode* out);
inline Mode mode() { return g_mode; }

// 注册 PSC 模块并起通知线程。成功返回 true。
// ★ 调用时机：**要在 `sm` 开着的时候调**（`pscmInitialize` 要经 `sm:`）。
//   ★★ 现在改到**监听口建好之后、进 `serveForever` 之前**调：
//      ① 对齐 MissionControl/sys-con 的"开机期注册"（已证明安全）；
//      ② 顺手避开最早的开机窗口 —— 万一注册有问题，也不至于卡在 logo。
bool start(Mode m);

// 系统已请求进入睡眠（`ReadySleep` / `ReadySleepCritical` / `ReadyShutdown`）。
bool sleepRequested();

// 通知线程已经把"要睡了"的事件**应答完**了吗（用于主循环决定何时重开监听口）。
bool sleepAcknowledged();

// 主线程报告"我已经把所有 socket 关掉了"（通知线程据此才应答）。
void reportSocketsClosed();

// 主循环消费"刚醒来"这个事件（消费后清除）。
bool consumeWoke();

// 诊断：收到的 PSC 事件数 / 睡眠类状态数 / 最近一个状态值（放进黑匣子，便于事后复盘）。
int eventCount();
int sleepEventCount();
int lastState();

}  // namespace power
}  // namespace nxc
