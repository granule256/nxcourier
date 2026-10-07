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

// ★★★ 曾经试过、**已放弃**：静态 socket 内存池（照抄 dmnt.gen2 的 `g_socket_memory` /
//   SysDVR 的 `TmemBackingBuffer`）—— 那块 2.25MB 的静态数组已经删掉了。
//   放弃原因：libnx **不导出** `bsdInitialize`/`bsdExit`（链接期 `nm` 实测）⇒ 走不通；
//   而且 `sys-botbase` 用 libnx 原路（内核分配）也不崩 ⇒ 内存池来源不是关键。详见下面的注释。

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

// ★★★ 2026-10-06 新增（`likedmnt2` 模式）：**照抄 Atmosphère 自己的 standalone GDB 桩
//   （`dmnt.gen2` TCP 模式）的 socket 配置**。动机：那个桩在 `0.0.0.0:22225`
//   **长期 listen、零睡眠处理**（`dmnt2_main.cpp` 全文件无 pscm、主线程 `SleepThread(1天)`），
//   却在我们机器上**跨整个崩溃循环一直不崩** ⇒ 我们和它的差异才是真凶。
//   已知差异（逐行核过 `stratosphere/dmnt.gen2/` 与 `libraries/libstratosphere/`）：
//     ① socket 配置 = `socket::SystemConfigLightDefault` ⇒ `m_system = true`
//        ⇒ `service_type = (1<<1)` ⇒ **`bsd:s`**（我们是 libnx 默认的 `bsd:u`）
//     ② `Listen(fd, 0)` ⇒ **backlog = 0**（我们原来 8）
//     ③ **不设 `SO_REUSEADDR`**（我们原来设了）
//     ④ `sb_efficiency = 2`、并发会话 = 2（libnx 默认是 4 / 3）
//   ★★★ **一个必须知道的坑**（libnx `bsd.c` 的 `_bsdInitialize` 源码）：
//     回退到 `bsd:u` **只在 `service_type` 里带了 `BsdServiceType_User` 时才发生**。
//     这里**故意只传 `BsdServiceType_System`** ⇒ 若拿不到 `bsd:s` 权限，`socketInitialize`
//     会**直接失败**（不会静默退到 bsd:u）。这正是我们要的：**失败可见**，
//     好过"以为在用 bsd:s、其实在用 bsd:u"（H-A 就栽在"实验静默空跑"上）。
SocketInitConfig makeSystemServiceConfig() {
    SocketInitConfig cfg = *socketGetDefaultInitConfig();   // 不手抄默认值，避免抄错
    cfg.bsd_service_type = BsdServiceType_System;           // bsd:s（只传 System ⇒ 失败可见）
    cfg.sb_efficiency    = static_cast<u32>(nxc::net::sbEfficiency());
    cfg.num_bsd_sessions = static_cast<u32>(nxc::net::bsdSessionCount());
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

    // ---------------------------------------------------------------- 网络模式
    // ★★★ 2026-10-06：**网络层已实测是崩因**（完全不建网时模块活了 192 秒、崩溃报告没增加），
    //   现在要把它**切开看内部**。模式从 SD 上 `/config/nxc/net_mode.txt` 读
    //   （改一行字 + 重启即可，不用重编）：
    //     `off`      完全不建网（已知**不崩**，保底模式）
    //     `bdsonly`  只 `socketInitialize`（建 `bsd` 会话 + transfer memory），
    //                **不 listen、不建任何 socket** —— 与 `off` 的唯一差别就是"持有一个 bsd 客户端"
    //     `listen`   完整（已知**会崩**）
    //   ★ 读不到 / 值不认识 ⇒ 用 `kDefaultMode`（= `off`）—— **安全优先**：
    //     免得模式文件写坏就把主机拖进崩溃循环。
    nxc::net::Mode netMode = nxc::net::kDefaultMode;
    {
        FILE* f = nxc::sd::open("/config/nxc/net_mode.txt", "rb");
        if (f != nullptr) {
            char buf[64];
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            buf[n] = '\0';
            nxc::net::Mode parsed{};
            int delay = 0;
            if (nxc::net::parseMode(buf, &parsed, &delay)) {
                netMode = parsed;
                nxc::net::g_listenDelaySec = delay;
            } else {
                nxc::log::ring("net_mode.txt 的内容不认识，保留缺省 %s",
                               nxc::net::modeName(netMode));
            }
        } else {
            nxc::log::ring("net_mode.txt 读不到，用缺省 %s", nxc::net::modeName(netMode));
        }
    }
    nxc::net::setMode(netMode);
    nxc::log::breadcrumb("main: net mode=%s", nxc::net::modeName(netMode));
    nxc::log::watch("net mode=%s (start)", nxc::net::modeName(netMode));

    // ---------------------------------------------------------------- PSC 模式
    // ★★★ 2026-10-06：PSC 收口**重新启用**（上一次因"卡开机"被停用，原因已查明并修好）。
    //   真凶不是"注册太早"，而是**我们把假应答灌进了 PSC 状态机**：
    //   libnx 的 `pscPmModuleGetRequest()` **自己不阻塞**，上一版 100ms 轮询它、
    //   拿到 `state=0` 就盲目 `Acknowledge`。现在照抄 sys-con 用 `waitSingle` 先等事件。
    //   ★ 独立佐证：**`MissionControl` 也是 boot2 模块、也在开机期注册 `pscm`**
    //     ⇒ "boot2 + 开机期注册 PSC"本身是安全的。
    //   ★ 同样从 SD 读（改一行字 + 重启即切换，不用重编）：
    //        `off`     不注册（缺省，安全优先）
    //        `observe` 注册 + 记录状态 + 立刻 Ack，**但不动 socket**（隔离实验）
    //        `close`   注册 + 睡眠时关掉全部 socket、醒来重开（真正的修法）
    nxc::power::Mode pscMode = nxc::power::Mode::Off;
    {
        FILE* f = nxc::sd::open("/config/nxc/psc_mode.txt", "rb");
        if (f != nullptr) {
            char buf[64];
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            buf[n] = '\0';
            nxc::power::Mode parsed{};
            if (nxc::power::parseMode(buf, &parsed)) {
                pscMode = parsed;
            } else {
                nxc::log::ring("psc_mode.txt 的内容不认识，保留缺省 %s",
                               nxc::power::modeName(pscMode));
            }
        } else {
            nxc::log::ring("psc_mode.txt 读不到，用缺省 %s", nxc::power::modeName(pscMode));
        }
    }
    nxc::log::watch("psc mode=%s (start)", nxc::power::modeName(pscMode));

    // ★★★ 判据必须是"这个模式要不要建监听口"，**绝不能**写成 `!= Mode::Listen`！
    //   踩坑（2026-10-06，本文件第三次因为"分支条件没跟着改"而白烧一轮）：
    //   我后来加了 `AcceptOnly` / `LikeDmnt2` 两个模式，但这个条件没同步改
    //   ⇒ 它们**掉进了"断网空转"分支** ⇒ 根本没建监听口 ⇒ 47800 直接拒绝，
    //     而现象看起来像"不崩了"（**假阳性**）。
    //   ★ 抓出真相的是黑匣子：`watch_boot68.txt` 里写着 `step=idle tick=3 (mode=likedmnt2)`。
    //   ⇒ 现在统一用 `nxc::net::listenEnabled()`（它已把 Listen / AcceptOnly / LikeDmnt2 都算上）
    //     ⇒ 以后**加模式只需要改 `net.hpp` 那一处**，这里不会再漏。
    if (!nxc::net::listenEnabled()) {
        // ★ 这两支都**不 listen** ⇒ 没有 47800 ⇒ 读数只能靠 SD 上的 watch 文件
        //   （每 30 秒一条，含系统 tick 与 RTC 墙钟）+ 用户的 ftpd 拉崩溃报告。
        // ★★ 先明确宣告"这一支不会建监听口" —— 免得再被误读成"不崩了"。
        nxc::log::watch("NET OFF: no listener will be created (mode=%s) — 47800 不会出现",
                        nxc::net::modeName(nxc::net::mode()));
        if (netMode == nxc::net::Mode::BsdOnly) {
            // 只把 `bsd` 会话 + transfer memory 建起来，**不建 socket、不 listen**。
            // ⇒ 这一支与 `off` 的唯一差别是"持有一个 bsd 客户端"，
            //   正好回答"元凶是 `bsd` 会话还是监听口"。
            if (nxc::host::smOpen()) {
                const Result rc = socketInitialize(nullptr);   // 默认配置（= 参照模块那套）
                nxc::log::watch("bdsonly: socketInitialize rc=0x%08X bsd=0x%08X",
                                static_cast<unsigned>(rc),
                                static_cast<unsigned>(socketGetLastResult()));
                if (R_SUCCEEDED(rc)) g_init |= kInitSocket;
                nxc::host::smClose();   // 用完就把 sm 还掉（睡眠时不持有它）
            } else {
                nxc::log::watch("bdsonly: sm unavailable for socketInitialize");
            }
        }

        for (int tick = 0; ; ++tick) {
            svcSleepThread(30000000000ULL);   // 30s
            // ★ 黑匣子：每 30 秒一条，含**系统 tick 与 RTC 墙钟**。
            //   睡眠时系统 tick 会停、而 RTC 继续走 ⇒ 两者撕开后的**差值一跳
            //   就是"确实睡过"的铁证**；文件名带开机号 ⇒ 崩过之后再开机也不会
            //   把崩前那一条冲掉。
            nxc::log::watch("idle tick=%d (mode=%s)", tick, nxc::net::modeName(nxc::net::mode()));
        }
    }

    // ★★ 可选的"**socket 初始化延迟**"（模式文件里第二个数字，如 `likedmnt2 20`）。
    //   ★★ 为什么把它从"建监听口之前"挪到"**socket 初始化之前**"：
    //     上一轮我延迟的是**建监听口**，实测无效（而且被用户点破：那次睡在口建出来之前，
    //     等于没口，测了个寂寞）。而 `sys-botbase` 里那行**没有注释、当年被放弃不抄**的
    //     `svcSleepThread(20 秒)` 延迟的是**整个 socket 初始化** —— **两件事不一样**。
    //   ★ 而且这次有**代码级理由**：Atmosphère 自己的注释写的是
    //     「**socket 驱动**需要 WiFi，而 WiFi 要等系统完全起来」——
    //     ★ 它说的是 **socket 初始化**，不是"监听"。我们现在是开机 **11.5 秒**就把 bsd
    //     客户端注册进去了（`socketInitialize` 第 1 次尝试就成功），而系统"完全起来"
    //     （端口可连）约在 35 秒 ⇒ 很可能就是**注册得太早**留下的坏状态。
    if (nxc::net::listenDelaySec() > 0) {
        nxc::log::watch("socket-init delay: 等 %d 秒再 socketInitialize（mode=%s）",
                        nxc::net::listenDelaySec(), nxc::net::modeName(nxc::net::mode()));
        svcSleepThread(static_cast<u64>(nxc::net::listenDelaySec()) * 1000000000ULL);
    }

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
    SocketInitConfig sysCfg   = makeSystemServiceConfig();   // ★ likedmnt2 模式用（bsd:s）
    const bool useSysService  = nxc::net::systemSocketService();
    bool usingFallback = false;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        // ★★★ `likesysdvr`（照抄 SysDVR）：**绕开 `socketInitialize`，直接 `bsdInitialize`**，
        //   并把 socket 内存池指向我们自己的静态数组。
        //   为什么要绕：libnx 的 `SocketInitConfig` **没有**内存池字段，`tmem_buffer` 只在
        //   `BsdInitConfig`（`bsd.h`）里 ⇒ 想自己给内存池就只能直接调 `bsdInitialize`。
        //   其余模式照旧（`likedmnt2` 用 `bsd:s`、其它用 libnx 默认配置）。
        // ★★★ 「自带 socket 内存池」这条路：**原本被我误判为"已放弃"，2026-10-06 已纠正。**
        //   ★ 当时写的"放弃原因"是「`bsdInitialize`/`bsdExit` 在 libnx 里是内部符号、没有导出」。
        //   ★★ 实测推翻（`nm` `devkitpro/devkita64` 里的 `libnx.a`）：
        //      `T bsdExit` / `T bsdInitialize` —— **两个都是导出的**，可以直接调用。
        //   ⇒ 路线是通的，但**代价明确**：直接调 `bsdInitialize` 不会注册 libnx 的 `"soc"`
        //     devoptab ⇒ `::poll`/`::send`/`::recv`/`::accept` 全失效，约 30 个调用点要换成
        //     `bsdPoll`/`bsdSend`/...（= 照抄 libstratosphere 的 `socket_api.os.horizon.cpp`）。
        //   ⇒ 详见 `core/net.hpp` 里 `likesysdvr` 那条注释；**留作 PSC 之后的下一个候选。**
        const Result rcDefault = useSysService ? socketInitialize(&sysCfg)
                                               : socketInitialize(nullptr);
        if (R_SUCCEEDED(rcDefault)) {
            nxc::log::ring("socket: DEFAULT config OK (attempt %d, svc=%s)", attempt,
                           useSysService ? "bsd:s" : "bsd:u");
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
        // ★★ 同一件事**再写一份到"黑匣子"**（per-boot 文件，不会被下次开机冲掉）。
        //   为什么必须双写：`likedmnt2` 模式的默认配置**故意只传 `BsdServiceType_System`**
        //   —— 拿不到 `bsd:s` 时会直接失败、然后当轮降级到小配置（bsd:u）**照常工作**。
        //   若只写环形日志，一旦以后崩机重启就再也看不到"这次其实没用上 bsd:s"，
        //   就会把"没崩"错读成"bsd:s 有效"（H-A 正是栽在这种静默空跑上）。
        nxc::log::watch("socket DEFAULT FAILED attempt=%d rc=0x%08X bsd=0x%08X mode=%s svc=%s",
                        attempt, static_cast<unsigned>(rcDefault),
                        static_cast<unsigned>(socketGetLastResult()),
                        nxc::net::modeName(nxc::net::mode()),
                        useSysService ? "bsd:s" : "bsd:u");

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

    // ★★★★ 2026-10-06 15:17 曾在此处**停用** PSC 电源通知（方案 A），因为它导致
    //   **开机卡在 logo**（第 3 类事故）。★ 16:xx 查明真凶并修好，**现已恢复**，
    //   但**调用点挪到了监听口建好之后**（见下面 `serveForever` 之前那段）。
    //
    //   ★★ 当时的两条归因，现在都不是原因（记下来免得再绕回去）：
    //     ① "注册得太早（早于系统初始化）" ⇒ **错**。`MissionControl`、`sys-con`
    //        都是 boot2 模块、都在开机期注册 `pscm`，都不卡开机。
    //     ② "handler 里做了阻塞动作（等主线程关 socket，最多 3 秒）" ⇒ 只对了一半：
    //        阻塞的**上限**确实要压小（现在 900ms），但真正卡死的是另一件事 ——
    //        ★★★ **libnx 的 `pscPmModuleGetRequest()` 自己不阻塞**（只发 IPC 就返回），
    //        而上一版是"100ms 轮询 GetRequest，成功就 Acknowledge"
    //        ⇒ 没有待处理请求时也会拿到 `state=0`(`Awake`) 并**盲目应答**
    //        ⇒ **往 PSC 状态机里灌假应答** ⇒ 开机的电源状态确认永远走不完 ⇒ 卡 logo。
    //        ⇒ 修法：照抄 `sys-con` —— `waiterForEvent()` + `waitSingle()` **先等事件**，
    //          再 `GetRequest`、再干活、再 `Acknowledge`（见 `core/power.cpp`）。
    nxc::log::ring("main: PSC 收口将在监听口建好后注册（psc_mode=%s）",
                   nxc::power::modeName(pscMode));

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
    // ★★ 同一个读数**再写进"黑匣子"**（per-boot 文件，崩机重启也不会被冲掉）：
    //   一次看清"这次到底用了哪个配置、哪个 bsd 服务、backlog 多少" ——
    //   免得又出现"以为在测 A、其实跑的是 B"。
    //   ★ `svc` 必须写**实际生效的那个**：一旦降级到小配置，那实际就是 `bsd:u`
    //     （只写 `useSysService` 会谎报成 bsd:s，正是这种谎报让上一轮白烧）。
    nxc::log::watch("socket ready via %s (mode=%s effective_svc=%s backlog=%d reuseaddr=%d)",
                    usingFallback ? "SMALL-fallback" : "DEFAULT",
                    nxc::net::modeName(nxc::net::mode()),
                    usingFallback ? "bsd:u" : (useSysService ? "bsd:s" : "bsd:u"),
                    nxc::net::listenerBacklog(),
                    nxc::net::useReuseAddr() ? 1 : 0);

    // ★ 监听口统一由 `protocol::openListener()` 建（抽出去是为了让 `acceptonly` 模式
    //   能在"客户端全断开"时把它开回来 —— 逻辑只有一份，不会两边走偏）。
    int listenFd = nxc::protocol::openListener();
    if (listenFd < 0) {
        nxc::log::breadcrumb("net: openListener failed, exit");
        return 0;
    }
    nxc::log::breadcrumb("net: listening on %u (keepListenerOpen=%d)",
                         nxc::net::kPort, nxc::net::keepListenerOpen() ? 1 : 0);
    nxc::log::watch("listening on %u (mode=%s keepOpen=%d)", nxc::net::kPort,
                    nxc::net::modeName(nxc::net::mode()),
                    nxc::net::keepListenerOpen() ? 1 : 0);

    // ---------------------------------------------------------------- PSC 收口
    // ★★★ 注册时机：**监听口建好之后、进服务循环之前**。
    //   ① 对齐 `MissionControl` / `sys-con` 的"开机期注册"（已被证明安全）；
    //   ② 顺手避开最早的开机窗口 —— 万一注册本身有问题，也不至于卡在 logo，
    //      而是在"端口已经能用"之后才发生（有 47800 可救、可远程改回 `off`）。
    // ★ `pscmInitialize()` 要经 `sm:` ⇒ **临时借一下、注册完立刻还**
    //   （守"睡眠时不持有 sm"这条纪律；PSC 之后走的是自己的域会话，不再需要 sm）。
    if (pscMode != nxc::power::Mode::Off) {
        // ★★★ 保险：**等开机彻底完成之后再注册**（2026-10-06 晚加）。
        //   为什么值得多等这 30 秒：这个模块有**两次"卡在 logo"**的前科，
        //   而那种事故的恢复只能靠**拔卡改文件**（远程全断）。
        //   ⇒ 把注册推到开机完成（实测约 35 秒）**之后**，就**从结构上取消了
        //     "卡开机"这个失败模式** —— 万一注册有问题，最坏也只影响睡眠，
        //     而睡眠出问题时 `47800`/FTP 仍然活着 ⇒ **能远程改回去，不用拔卡**。
        //   ★ 只要注册成功，它就在下一次睡眠之前（约 150 秒）从容就位，不影响修法本身。
        //   ★ 代价（诚实记下）：比 `sys-con`/`MissionControl` 的"开机期注册"晚；
        //     万一 PSC 不接受"开机后注册"，我们**会看见**（收不到任何 PSC 状态），
        //     而"看不见的失败"才是真正危险的 —— 这条是**可见的**。
        constexpr u64 kPscRegisterDelayMs = 30000;
        nxc::log::watch("psc: 等 %llu 秒再注册（确保开机已完成，避免「卡开机」这类事故）",
                        static_cast<unsigned long long>(kPscRegisterDelayMs / 1000));
        svcSleepThread(static_cast<u64>(kPscRegisterDelayMs) * 1000000ULL);

        if (nxc::host::smOpen()) {
            const bool ok = nxc::power::start(pscMode);
            nxc::log::watch("psc: 注册%s（mode=%s，id=126 deps={Fs} autoclear=1）",
                            ok ? "成功" : "**失败**", nxc::power::modeName(pscMode));
            nxc::host::smClose();   // 用完就还掉
        } else {
            nxc::log::watch("psc: sm 借不到，注册不了（mode=%s）",
                            nxc::power::modeName(pscMode));
        }
    }

    nxc::protocol::serveForever(listenFd, nxc::net::keepListenerOpen());
    return 0;
}
