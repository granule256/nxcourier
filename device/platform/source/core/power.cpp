// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

#include "power.hpp"
#include <atomic>
#include "platform.hpp"

#include <switch.h>

namespace nxc {
namespace power {

namespace {

// ★ 全部是"标志位"，**没有任何 fd**（fd 只由主线程碰，见 power.hpp 的纪律 2）。
std::atomic<bool> g_sleeping      = false;   // 系统已请求睡眠
std::atomic<bool> g_socketsClosed = false;   // 主线程已确认关完
std::atomic<bool> g_acked         = false;   // 通知线程已应答睡眠事件
std::atomic<bool> g_woke          = false;   // 刚醒来，待主循环消费
std::atomic<int>  g_events        = 0;       // 收到的 PSC 事件总数
std::atomic<int>  g_sleepEvents   = 0;       // 其中"睡眠类"状态数
std::atomic<int>  g_lastState     = -1;      // 最近一个 PscPmState

PscPmModule g_module;
Waiter      g_waiter;                    // ★ 照抄 sys-con：等事件用它，不轮询 GetRequest
Thread      g_thread;
// ★ 栈必须页对齐 —— 第一次编译成 `u8 g_stack[0x2000]`（没有 alignas），
//   `threadCreate` 直接失败 ⇒ **整条通知线程没起来、收口等于没做**。
alignas(0x1000) u8 g_stack[0x2000];

// ★ 依赖列表：照抄 sys-con 的 `{Fs}`。
//
// ★★★ **但这里不只是"照抄" —— `{Fs}` 恰好也是【唯一正确】的选择，理由是机制性的：**
//   PSC 的 `dependencies` 语义是"我想在这几个模块**完成转换之后**才被通知"
//   ⇒ 若我们把 **`Nifm`（38）或 `WlanSockets`（25）** 写进 deps，
//     就是"等 Nifm/WlanSockets 睡完再通知我"。
//   ★ 而我们的假设是：**正是我们握着的 socket 挡住了 Nifm/WlanSockets 的 Ack**
//     ⇒ 那样我们会**永远等不到通知** ⇒ 也就**永远没机会去关 socket** ⇒ **死锁**。
//   ⇒ 反之 `{Fs}` 把我们挂在一个**与网络无关**的模块后面 ⇒ 我们一定能被叫醒 ⇒
//     关掉 socket ⇒ 反过来把 Nifm/WlanSockets 解开。**顺序上是"自救"而不是"等救"。**
//   ★ 佐证：系统里另一个电源模块（PmModuleId 0x5250）用的 deps 是 `{Fs, Nifm}` —— 它不需要
//     "先关 socket 去救 Nifm"，所以那样的 deps 对它没问题；**我们的场景不同。**
constexpr const u32 kDeps[] = { static_cast<u32>(PscPmModuleId_Fs) };

// ★ module_id 用 126：★ 已核实**未占用** —— libnx 的 `PscPmModuleId` 枚举从 102 直接跳到
//   127（`Spsm`），中间 102..126 全是自定义空间；sys-con 也正是用的 126。
constexpr PscPmModuleId kModuleId = static_cast<PscPmModuleId>(126);

const char* stateName(PscPmState s) {
    switch (s) {
        case PscPmState_Awake:               return "Awake";
        case PscPmState_ReadyAwaken:         return "ReadyAwaken";
        case PscPmState_ReadySleep:          return "ReadySleep";
        case PscPmState_ReadySleepCritical:  return "ReadySleepCritical";
        case PscPmState_ReadyAwakenCritical: return "ReadyAwakenCritical";
        case PscPmState_ReadyShutdown:       return "ReadyShutdown";
    }
    return "?";
}

bool isSleepState(PscPmState s) {
    return s == PscPmState_ReadySleep || s == PscPmState_ReadySleepCritical
        || s == PscPmState_ReadyShutdown;
}
bool isWakeState(PscPmState s) {
    return s == PscPmState_Awake || s == PscPmState_ReadyAwaken
        || s == PscPmState_ReadyAwakenCritical;
}

// 等主线程把 socket 关掉。
// ★★ 上限刻意压到 **900ms**：PSC 的派发超时是秒级，我们**不能**让"关 socket"把 Ack
//   拖过那个超时 —— 那等于我们自己在制造 `0x2A5`。
//   （上一版是 3000ms，正好是"把自己的修复变成故障"的写法。）
bool waitSocketsClosed(int maxMs) {
    for (int i = 0; i < maxMs; i += 5) {
        if (g_socketsClosed) return true;
        svcSleepThread(5000000ULL);   // 5ms
    }
    return false;
}

void notificationThread(void*) {
    // ★★★ 这条线程**只写内存环形日志**，绝不碰 SD / `sm` / `time` —— 见 power.hpp 纪律 3。
    log::ring("power: 通知线程已启动（waitSingle 等事件，不轮询 GetRequest）");

    for (;;) {
        // ★★★ 照抄 sys-con 的第一步：**先等事件**。
        //   这就是"注册 PSC 卡开机"的真凶所在：libnx 的 `pscPmModuleGetRequest`
        //   **自己不阻塞**（只发 IPC cmd 1 就返回），所以上一版 100ms 轮询它、
        //   拿到 state=0 就盲目 `Acknowledge` ⇒ 往 PSC 状态机灌假应答 ⇒ 卡 logo。
        //   `waitSingle` 才是"真的有待处理请求"这个前提。
        if (R_FAILED(waitSingle(g_waiter, UINT64_MAX))) {
            svcSleepThread(100000000ULL);   // 100ms，别把 CPU 烧了
            continue;
        }

        PscPmState state = PscPmState_Awake;
        u32 flags = 0;
        const Result rcReq = pscPmModuleGetRequest(&g_module, &state, &flags);
        if (R_FAILED(rcReq)) {
            // ★ 拿不到请求就**不应答** —— 盲目 Ack 是上一版的病根。
            log::ring("power: GetRequest 失败 rc=0x%08X（不应答，等下一个事件）",
                      static_cast<unsigned>(rcReq));
            continue;
        }

        ++g_events;
        g_lastState = static_cast<int>(state);
        log::ring("power: 收到 state=%d(%s) flags=0x%X",
                  static_cast<int>(state), stateName(state), static_cast<unsigned>(flags));

        if (isSleepState(state)) {
            ++g_sleepEvents;
            g_sleeping = true;
            // 一个睡眠周期只等一次关连接确认，后续状态不重复等待。
            if (!g_socketsClosed && !waitSocketsClosed(900)) {
                log::ring("power: 等 socket 关闭超时(900ms)，仍然应答");
            }
        } else if (isWakeState(state)) {
            g_sleeping      = false;
            g_socketsClosed = false;
            g_acked         = false;
            g_woke          = true;
        }

        const Result rcAck = pscPmModuleAcknowledge(&g_module, state);
        if (R_FAILED(rcAck)) {
            log::ring("power: Ack 失败 rc=0x%08X state=%d(%s)",
                      static_cast<unsigned>(rcAck), static_cast<int>(state), stateName(state));
        } else if (isSleepState(state)) {
            g_acked = true;   // 诊断用：确认"睡眠事件已经应答过"
        }
    }
}

}  // namespace

bool start() {
    if (R_FAILED(pscmInitialize())) {
        log::ring("power: pscmInitialize 失败 —— 没有电源通知（睡眠时不收口）");
        return false;
    }

    const Result rc = pscmGetPmModule(&g_module, kModuleId, kDeps, 1, /*autoclear=*/true);
    if (R_FAILED(rc)) {
        log::ring("power: pscmGetPmModule 失败 rc=0x%08X（id=%d）",
                  static_cast<unsigned>(rc), static_cast<int>(kModuleId));
        pscmExit();
        return false;
    }

    // ★★★ 照抄 sys-con：事件用 `Waiter` 等，而不是轮询 IPC。
    g_waiter = waiterForEvent(&g_module.event);

    // ★ 每一步都把 rc 记下来 —— 上次 `threadCreate` 失败只留了一句"失败"，没有 rc。
    //   ★ 优先级/核也照抄 sys-con（`0x2C, -2`：任意核）。
    const Result rcThread = threadCreate(&g_thread, notificationThread, nullptr, g_stack,
                                         sizeof(g_stack), 0x2C, -2);
    if (R_FAILED(rcThread)) {
        log::ring("power: threadCreate 失败 rc=0x%08X（stack=%p size=%u）",
                  static_cast<unsigned>(rcThread), static_cast<void*>(g_stack),
                  static_cast<unsigned>(sizeof(g_stack)));
        pscPmModuleFinalize(&g_module);
        pscPmModuleClose(&g_module);
        pscmExit();
        return false;
    }
    const Result rcStart = threadStart(&g_thread);
    if (R_FAILED(rcStart)) {
        log::ring("power: threadStart 失败 rc=0x%08X", static_cast<unsigned>(rcStart));
        threadClose(&g_thread);
        pscPmModuleFinalize(&g_module);
        pscPmModuleClose(&g_module);
        pscmExit();
        return false;
    }

    log::ring("power: PSC 已注册（id=%d deps={Fs} autoclear=1）",
              static_cast<int>(kModuleId));
    return true;
}

bool sleepRequested()    { return g_sleeping; }
bool sleepAcknowledged() { return g_acked; }
void reportSocketsClosed() { g_socketsClosed = true; }

bool consumeWoke() {
    return g_woke.exchange(false, std::memory_order_acq_rel);
}

int eventCount()      { return g_events; }
int sleepEventCount() { return g_sleepEvents; }
int lastState()       { return g_lastState; }

}  // namespace power
}  // namespace nxc
