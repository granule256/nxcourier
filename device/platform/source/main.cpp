// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// NxCourier 平台 · 设备侧入口（sysmodule）
//
// 设计要点（都有出处，别随手改）：
//   * `__nx_applet_type = AppletType_None` —— sysmodule 不是相册小程序。
//   * ★★ **`__appInit` 里绝不允许 abort，也绝不允许阻塞。**
//     本模块靠 `flags/boot2.flag` 在 **boot2 阶段**被启动；在 boot2 阶段里
//     `diagAbortWithResult` 或"钉住不返回"都可能让**开机失败**。
//     所以每一步失败只记录状态，由 `main` 决定怎么安全收场（正常 return 退出进程）。
//   * ★ **socket 初始化推迟到 main 里做，并且失败要重试** —— boot2 阶段 `bsd:u` 未必就绪。
//   * ★★ **`sm` 不跨睡眠持有**：boot2 阶段用完就 `smClose()`；运行时按需开、用完关
//     （`SmGuard`，见 `host::smOpen/smClose` 与 `__appInit` 末尾的说明）。
//     ★ 这条注释以前写的是「全程不调 smExit()」，是**反的**（依据是一条没核实的注释），
//       2026-10-06 已改正 —— 教训：注释里的"事实"必须核实才能当依据。
//   * ★ socket 配置：**首选 libnx 默认配置**（`socketInitialize(NULL)`），失败才退到一套小缓冲。
//     ★ 这条 2026-10-06 反过来了：以前**只有**小缓冲（理由：默认要 ~2.25MB transfer memory，
//       想省内存）。但对照发现同样常驻监听 socket 的 sys-botbase 用的是**默认配置**，
//       而我们的故障是"睡眠时电源切换派发超时" ⇒ 把默认配置提到首选来验证（实验 H-A）。
//   * 不用 `std::thread` —— 单线程跑 accept 循环（开工单 §4.9）。

#include "platform.hpp"
#include "sd.hpp"
#include "net.hpp"
#include "power.hpp"   // ★ 方案 A：PSC 电源通知（睡眠时把 socket 全关掉）

#include <switch.h>
// ★ 备注：曾经为了"照抄 SysDVR 自带 socket 内存池"引过 `<switch/services/bsd.h>`；
//   那条路因 libnx **不导出** `bsdInitialize`/`bsdExit` 而放弃（见下面的注释），
//   所以这个 include 也撤掉了。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------- libnx 胶水
u32 __nx_applet_type     = AppletType_None;  // sysmodule 绝不能用 applet
u32 __nx_fs_num_sessions = 1;                // sysmodule 一个 FS 会话就够
// ★★ 另外两个"最小化 fs 资源占用"的开关 —— 抄自 nx-ovlloader（它源码里明确写了
//   "Minimize fs resource usage"）。控制台期减少 fs.mitm 侧的资源占用。
u32  __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd = false;

// 堆大小由**我们自己划**（不是"sysmodule 内存小"，而是必须显式划一块）。
// ★★★ 2026-10-06：0x100000(1MB) → 0x480000(4.5MB)。
//   为什么：实验 H-A 想把 socket 换成 libnx 默认配置，但**首轮就失败了** ——
//   默认配置要算出一块 **2.25MB** 的 transfer memory
//   （(0x40000+0x40000+0x2400+0xA500) 页对齐 × sb_efficiency(4) = 0x234000），
//   而我们只有 1MB 堆，所以要不到 ⇒ **当轮回退到小配置，实验等于没做**。
//   ⇒ 对齐参照实现 sys-botbase（它 HEAP_SIZE = 4.5MB，正是为了装下默认配置）。
//   ★ 单变量提醒：这一改同时动了"内存占用"这条嫌疑（旧嫌疑 5），
//     所以若崩不再出现，要再单独验证是"配置"还是"堆"起的作用（把堆退回 1MB 复测）。
#define INNER_HEAP_SIZE 0x480000

extern "C" void __libnx_initheap(void) {
    // ★ 从 512KB 提到 1MB：capssc 官方建议的 JPEG 缓冲就是 0x80000(512KB)，
//   原来的 512KB 堆连这一个缓冲都放不下（malloc 直接失败）。
static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

namespace {

// ★ 监听端口不在这里了 —— 挪到 `core/net.hpp` 的 `nxc::net::kPort`，
//   因为 `protocol.cpp` 在"客户端全断开要重开监听口"时也要用它（逻辑只留一份）。

// 初始化进度位：每一步成功就置一位；main 靠它判断能走到哪。
enum InitBit : u32 {
    kInitSm     = 1u << 0,
    kInitFs     = 1u << 1,
    kInitSd     = 1u << 2,
    kInitSocket = 1u << 3,
};

u32 g_init = 0;

// ★★★ 这是**降级用**的小配置（不是首选）—— 2026-10-06 实验 H-A 改。
//
//   原来的历史：这一组曾是**唯一**的配置，注释写着"已知可用，别随手放大"，
//   因为实测把缓冲放大 + `sb_efficiency` 提到 4 + 会话数提到 3 之后
//   `socketInitialize()` 会直接失败，而当时的代码在这种情况下**静默 return 0 退出**
//   ⇒ 整个模块不监听端口了（系统其他部分全正常，看起来像"我们挂了"）。
//
//   ★ 但 2026-10-06 的排查发现另一件事：**同样是"常驻监听 socket 的 boot2 模块"的
//     sys-botbase，用的是【默认配置】**，只有我们用了这套非常规小配置
//     （`tcp_*_buf_max_size` 从 0x40000 压到 0x8000、`sb_efficiency` 4→1、会话数 1）。
//     而我们的故障恰好是**睡眠时 PSC 电源切换派发超时**（`0x2A5` = Spsm）。
//   ⇒ 现在改成：**首选默认配置（`socketInitialize(NULL)`），失败才退到这一套**。
SocketInitConfig makeSmallSocketConfig() {
    SocketInitConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.tcp_tx_buf_size     = 0x4000;
    cfg.tcp_rx_buf_size     = 0x4000;
    cfg.tcp_tx_buf_max_size = 0x8000;
    cfg.tcp_rx_buf_max_size = 0x8000;
    cfg.udp_tx_buf_size     = 0x1200;
    cfg.udp_rx_buf_size     = 0x5280;
    cfg.sb_efficiency       = 1;
    cfg.num_bsd_sessions    = 1;
    cfg.bsd_service_type    = BsdServiceType_User;  // bsd:u 就够，不必碰系统权限
    return cfg;
}


}  // namespace

// ---------------------------------------------------------------- 服务初始化
extern "C" void __appInit(void) {
    // ★★ 这里曾经加过「开机先等 20 秒」，**已撤掉**（2026-10-06）。
    //   撤掉的两个理由：
    //     ① **依据不成立**：sys-botbase 确实有这么一行，但它在那边
    //        **连注释都没有**，提交历史也查不到说明 ⇒ 我不知道它为什么那么写。
    //        把推断当依据用，和今晚那条错注释是同一类毛病。
    //     ② **有真实副作用**：延迟 20 秒 ⇒ 端口从"约 35 秒"推到"约 55 秒"才出现，
    //        而启动画面约 47 秒就出来了 ⇒ **会错过「按 3 次」的窗口**。
    //   ⇒ 若以后要试延迟，**单独做一轮、带明确假设**，别塞进别的改动里。

    // ★ 这一段是 boot2 阶段跑的：只许"记录结果"，不许 abort / 不许卡住。
    if (R_SUCCEEDED(smInitialize())) {
        g_init |= kInitSm;
    }

    if (g_init & kInitSm) {
        // 取固件版本，好让 libnx 内部按版本走对分支。取不到也不影响后续。
        Result rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            }
            setsysExit();
        }
    }

    // ★★★ 2026-10-06 实验 H-D′：`fs` 会话可以**整体不开**（见 `sd.hpp` 的说明）。
    //   关掉时：不 `fsInitialize()` ⇒ 我们不持有任何 `fs` 服务会话。
    //   ★ 配套要求（已做）：`sd::kEnabled` 必须同时为 false，且 `protocol.cpp::dispatch`
    //     要把 `fs`/`save` 两个工具挡掉 —— 那两处有裸 devoptab / 裸 fs 服务调用。
    if (nxc::sd::kFsSessionEnabled && (g_init & kInitSm) && R_SUCCEEDED(fsInitialize())) {
        g_init |= kInitFs;
    }
    // ★★★ 这里曾经改成过「按需挂载」（2026-10-05 深夜），**已作废并回退**。
    //   回退原因（记下来免得有人再试）：`fsdevUnmountAll()` 之后，
    //   **任何 `fopen` 都会在 libnx 的 `_open_r` 里空指针解引用崩溃** ——
    //   不是「返回 NULL 让调用方处理」。那次改动直接让主机进不去系统
    //   （boot2 阶段崩 ⇒ 大气层报错 ⇒ 卡在错误屏）。
    //   ⇒ 「按需卸载 SD」这条路不要走：它的失败模式是**崩溃**，不是可控失败。
    // ★ 只有"SD 开关打开"时才挂载。开关关闭 = 全程不挂载、不读写（A/B 测试用）。
    if (nxc::sd::kEnabled && (g_init & kInitFs) && R_SUCCEEDED(fsdevMountSdmc())) {
        g_init |= kInitSd;
    }
    if (g_init & kInitSd) {
        nxc::log::init();
        nxc::log::breadcrumb("init: sd mounted, mask=0x%X", g_init);
    }

    // socket 不在这里初始化：boot2 阶段 bsd:u 未必就绪，留给 main 带重试地做。

    // ★★★ 这里【要】把 sm 关掉（2026-10-06 改）。
    //
    // ★★ 原来这里是"故意不关"，注释写的是「参照 sys-botbase 的做法」——
    //    **那条注释是错的**：查过两个成熟 boot2 模块的源码，它们都关：
    //      · sys-botbase   ：`__appExit` 里有 `smExit()`
    //      · nx-ovlloader  ：**在 `__appInit` 结束前就 `smExit()`**（main.c:139），
    //                        运行时要开服务时再临时 `smInitialize()` → 用完 `smExit()`（:154/:186）
    //    ⇒ 只有我们一直握着 sm 不放，而实测症状正是"**后台一直存在的东西**导致 omm 崩"。
    //    ⇒ 现在对齐成熟做法：**boot2 结束就放掉 sm；运行时按需开、用完关**（见 host::smOpen/smClose）。
    nxc::host::smClose();
}

// ---------------------------------------------------------------- sm 作用域
// ★★★ 为什么要有这三个函数：boot2 模块**不应跨睡眠持有 sm 会话**（见 __appInit 末尾的说明）。
//   但运行时开服务又必须经 sm ⇒ 改成"按需开、用完关"。
//   实现放在这里是因为 g_init 是 main.cpp 的 static。
namespace nxc {
namespace host {

bool smIsOpen() { return (g_init & kInitSm) != 0; }

// ★★★ 直接查 libnx 的 sm 会话是否活跃 —— **不是**看我们自己的标志位。
//   `serviceIsActive()` 检查的是 Service 结构里的 handle 是否有效。
bool smReallyOpen() {
    return serviceIsActive(smGetServiceSession());
}

bool smOpen() {
    if (g_init & kInitSm) return true;             // 已经开着，幂等
    if (R_FAILED(smInitialize())) return false;
    g_init |= kInitSm;
    return true;
}

void smClose() {
    if (!(g_init & kInitSm)) return;               // 已经关着，幂等
    smExit();
    g_init &= ~static_cast<unsigned>(kInitSm);
}

}  // namespace host
}  // namespace nxc

extern "C" void __appExit(void) {
    if (g_init & kInitSocket) socketExit();
    if (g_init & kInitSd) fsdevUnmountAll();
    if (g_init & kInitFs) fsExit();
    if (g_init & kInitSm) smExit();
}

// ---------------------------------------------------------------- 主循环
int main(int /*argc*/, char** /*argv*/) {
    // ★★ 一个教训换来的面包屑，别删。
    //
    //   2026-10-05 晚，为了排查「睡眠后 omm 崩」把 SD 改成「按需挂载」，
    //   结果模块**每次开机直接静默退出**（那个改动让 kInitSd 永远不再置上，
    //   于是下面这个条件恒真）—— 端口永不出现，而 22225/6666 照样通，
    //   现象与「系统没起来」极像。更晚那次改得更糟，直接让主机进不了系统。
    //
    //   ★ 本文件其它 5 处 return 前**本来就有** breadcrumb，**只有这一处没有**。
    //     ⇒ 现在补上了：**main 里每一条退出路径都必须留痕。**
    if (!(g_init & kInitSd)) {
        if (nxc::sd::kEnabled) {
            // 期望挂载却没挂上 ⇒ 异常，安静退出（**留痕再退**，别再变成静默退出）
            nxc::log::breadcrumb("main: WARN sd mount failed, mask=0x%X (quiet exit)", g_init);
            return 0;
        }
        // ★ SD 被显式禁用 ⇒ 这是**预期状态**，继续监听。
        //   （这条 breadcrumb 写不进去 —— 没 SD —— 但也不会崩，
        //     因为 log 走的是 sd::open，禁用时直接返回 nullptr。）
        nxc::log::breadcrumb("main: sd disabled (test mode), continue");
    }

    nxc::log::breadcrumb("main: enter, tools=%d, mask=0x%X",
                         nxc::reg::toolCount(), g_init);

    // 正常监听和睡眠收口是固定行为，不再读取实验模式文件。
    nxc::log::watch("network: listen with PSC socket cleanup");

    // ★ socket 的初始化需要 sm（要经 sm: 拿 bsd 端口）—— 所以这里临时开一下。
    //   开完立刻关：**睡眠时我们不持有 sm**（这就是本次改动的目标）。
    const bool smForSocket = nxc::host::smOpen();
    if (!smForSocket) {
        nxc::log::breadcrumb("main: WARN sm unavailable for socket (quiet exit)");
        return 0;
    }

    // socket 可能要等一会儿才可用（boot2 阶段网络栈还没起来）。
    // 重试有上限：一直起不来就退出，绝不无限占着开机流程。
    // ★ 2026-10-06：实测上一次开机（boot=61）**一次就成功**、没有重试记录，
    //   所以这个 60 秒预算只是保险，正常路径不会真的等。
    constexpr int kMaxAttempts = 60;          // 60 × 1s = 最多等 60 秒
    // ★★★ 实验 H-A（2026-10-06，见 `docs/分析-omm崩溃-0x2A5属于Spsm电源切换超时-20261006.md` §六）：
    //   **每一轮先试【默认配置】，失败再试我们那套小配置。**
    //   为什么这个结构（而不是"先试 60 轮默认、再试 60 轮小配置"）：
    //     ① 总等待预算与改之前**完全一致**（还是最多 60 秒），不会把开机拖长、不会错过
    //        「重启后按 3 次」的窗口；
    //     ② bsd 没就绪时两种配置都是立刻失败 ⇒ 行为等价于原来；
    //     ③ bsd 就绪但默认配置要不到内存时 ⇒ 当轮就降级成功，不白等。
    //   ★ 安全性：`socketInitialize(NULL)` 失败**不会** abort、也不会卡住 —— 它只是返回错误，
    //     我们照旧往下走 ⇒ 最坏情况是"退回小配置"，与改之前一模一样。
    SocketInitConfig smallCfg = makeSmallSocketConfig();
    bool usingFallback = false;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const Result rcDefault = socketInitialize(nullptr);
        if (R_SUCCEEDED(rcDefault)) {
            nxc::log::ring("socket: DEFAULT config OK (attempt %d, svc=bsd:u)", attempt);
            g_init |= kInitSocket;
            break;
        }
        // ★★★ 这条是 2026-10-06 补的关键读数 —— 上一次实验（H-A）就是栽在这里：
        //   默认配置失败、小配置当轮成功 ⇒ 循环 `break` 掉 ⇒ 失败原因**一个字节都没留下**，
        //   于是"到底为什么失败"只能靠猜。
        //   ★ 写【内存环形日志】而不是面包屑：面包屑是"覆盖写、只留最后一条"，
        //     会被后面的 `net: listening` 冲掉；环形日志能用 `log.dump` 完整取走。
        nxc::log::ring("socket: DEFAULT FAILED attempt=%d rc=0x%08X bsd=0x%08X",
                       attempt, static_cast<unsigned>(rcDefault),
                       static_cast<unsigned>(socketGetLastResult()));
        nxc::log::watch("socket DEFAULT FAILED attempt=%d rc=0x%08X bsd=0x%08X svc=bsd:u",
                        attempt, static_cast<unsigned>(rcDefault),
                        static_cast<unsigned>(socketGetLastResult()));

        const Result rcSmall = socketInitialize(&smallCfg);
        if (R_SUCCEEDED(rcSmall)) {
            nxc::log::ring("socket: SMALL fallback OK (attempt %d)", attempt);
            g_init |= kInitSocket;
            usingFallback = true;
            break;
        }
        nxc::log::ring("socket: SMALL also FAILED rc=0x%08X bsd=0x%08X",
                       static_cast<unsigned>(rcSmall),
                       static_cast<unsigned>(socketGetLastResult()));

        if (attempt == 1 || attempt % 10 == 0) {
            nxc::log::breadcrumb("main: socket not ready, attempt=%d/%d rc_default=0x%08X rc_small=0x%08X",
                                 attempt, kMaxAttempts,
                                 static_cast<unsigned>(rcDefault), static_cast<unsigned>(rcSmall));
        }
        svcSleepThread(1000000000ULL);  // 1s
    }

    // ★ 无论 socket 成不成，先把为它借用的 sm 还掉（下面失败时自己会 return）。
    //   放在这里而不是上面：socketInitialize 自己可能要重试很久。
    if (nxc::host::smIsOpen()) nxc::host::smClose();

    if (!(g_init & kInitSocket)) {
        nxc::log::breadcrumb("main: socket unavailable after retries, exit");
        return 0;
    }
    // ★ 这一行是实验 H-A 的**读数点**：从 SD 上的 boot_progress.txt 就能看出
    //   到底用的是默认配置还是降级的小配置 —— 不必猜。
    nxc::log::breadcrumb("main: socket ready via %s (sm 已归还)",
                         usingFallback ? "SMALL-fallback" : "DEFAULT");
    nxc::log::watch("socket ready via %s (svc=bsd:u backlog=%d reuseaddr=%d)",
                    usingFallback ? "SMALL-fallback" : "DEFAULT",
                    nxc::net::listenerBacklog(), nxc::net::useReuseAddr() ? 1 : 0);

    // 维持原有注册等待，先启动PSC通知再创建监听口。
    constexpr u64 kPscRegisterDelayMs = 30000;
    svcSleepThread(kPscRegisterDelayMs * 1000000ULL);
    bool pscReady = false;
    if (nxc::host::smOpen()) {
        pscReady = nxc::power::start();
        nxc::host::smClose();
    }
    if (!pscReady) {
        nxc::log::breadcrumb("network disabled: PSC registration failed");
        return 0;
    }
    // 首次监听失败也走服务循环重试，不因一次临时网络错误退出模块。
    const int listenFd = nxc::protocol::openListener();
    nxc::protocol::serveForever(listenFd);
    return 0;
}
