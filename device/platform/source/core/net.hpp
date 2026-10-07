// 网络（socket / bsd）模式开关 —— 对照实验用。
//
// ## ★★★ 2026-10-06 的结论（实测）
//
// **H-B「断网版」不崩了**：完全不建网络时，模块活了 192 秒、按 30 秒打点、崩溃报告
// 计数没有增加（20 → 20）。⇒ **网络层（`bsd` 会话 / 监听 socket）就是这根刺。**
//
// 之前被逐个排除的（全部有实测证据，别再查）：
//   capssc 会话 · SD 挂载 · `fs` 服务会话 · `sm` 会话 · 轮询周期唤醒 ·
//   boot2 位置 · NPDM 结构（全文件逐字节比过）· 自定义 SocketInitConfig（用默认配置实测）·
//   堆大小（1MB→4.5MB 无变化）
//
// ## ★★★ 2026-10-06 第二轮定案：元凶是【处于 `listen` 状态的 socket】
//
// 三档模式的实测结果：
//   `off`      （完全不调 `socketInitialize`）                        ⇒ **不崩**
//   `bdsonly`  （`socketInitialize` 建了 `bsd` 会话 + transfer memory，
//               **但不建任何 socket、不 listen**）                    ⇒ **不崩**
//   `listen`   （`socket()` + `bind` + `listen`，长期持有监听口）      ⇒ **睡眠一次必崩**
// ⇒ **`bsd` 会话与 transfer memory 都无罪；有罪的是"一个处在 `listen` 状态的 socket 被握到睡眠"。**
//
// ## 所以现在要分辨的是【监听口】与【已建立的连接】
//
// | 模式 | 做什么 | 想区分什么 |
// | --- | --- | --- |
// | `acceptonly` | `socket()+bind+listen`，**连上一个客户端之后就把监听口关掉**，只留已建立的连接；<br>客户端全断开时再把监听口开回来 | ★ 若它也**不崩** ⇒ 元凶是**"处于 listen 状态的"那个口**，<br>**已建立的连接是无辜的** ⇒ **修法成立**：连上就收监听口（MCP 长连接天然就是这个形态，平台照样可用）<br>若它崩 ⇒ 元凶是**"任何打开的 socket"** ⇒ 那就必须**在睡眠时把所有连接都关掉**（需要 PSC 通知，MCP 会断） |
//
// ★ `off` 是**缺省**模式（安全优先）：模式文件读不到或值不认识，就回退到它，
//   免得把主机拖进崩溃循环。
#pragma once

#include <cstring>

namespace nxc {
namespace net {

// ★ 监听端口放这里，因为 `protocol.cpp` 在"客户端全断开要重开监听口"时也要用它。
constexpr unsigned short kPort = 47800;

enum class Mode {
    Off = 0,        // 完全不建网（不崩）
    BsdOnly = 1,    // 只 socketInitialize，不建 socket、不 listen（不崩）
    Listen = 2,     // 完整：长期持有监听口（**会崩**）
    AcceptOnly = 3, // 连上就把监听口收掉，只留已建立连接（分辨"监听口"vs"连接"）
    // ★★★ 2026-10-06 新增：**1:1 照抄 Atmosphère 自己的 standalone GDB 桩（`dmnt.gen2` TCP 模式）
    //   的监听口参数**。动机是一条硬反例：那个桩在 `0.0.0.0:22225` **长期 listen**、
    //   **零睡眠处理**（`dmnt2_main.cpp` 里只有 `sm::Initialize` + `pmdmntInitialize`，
    //   主线程 `SleepThread(1 天)`，全文件没有 PSC / Suspend），
    //   而它在我们机器上**跨整个崩溃循环一直都在、完全不崩** ⇒
    //   **"长期持有监听口"本身不必然致崩**。所以我们和它的差异才是真凶。
    //   已知差异（按可疑度）：① 服务类型（它 `bsd:s`，我们 `bsd:u`）
    //                       ② backlog（它 1，我们 8）
    //                       ③ 它不设 `SO_REUSEADDR`，我们设了
    //   这一版把三条**一起**照抄过来（先看"照抄能否不崩"，成立再逐条二分）。
    LikeDmnt2 = 4,
    // ★★★ 2026-10-06 新增：**照抄 SysDVR 的 socket 初始化方式**。
    //
    //   发现（把两条独立参照并排看出来的共同点）：
    //     · Atmosphère 的 `dmnt.gen2`（22225，**不崩**）→ 静态数组 `g_socket_memory` 当 socket 内存池
    //     · **`SysDVR`（6666，就装在这台机器上；我们模块被禁用那段时间它一直开着也没崩）**
    //       → 直接调 bsd API + 静态数组 `TmemBackingBuffer` 当内存池
    //     · **我们** → 用 libnx 的 `socketInitialize`，socket 内存池由**内核**替我们分配（`tmemCreate`）
    //   ⇒ **两个能跑的参照都自己给内存池，只有我们让内核代劳。** 这是唯一一个
    //     "两条独立参照共有、而我们没有"的具体差异。
    //
    //   ★ 技术上：libnx 的 `SocketInitConfig`（socket.h）**根本没有**内存池字段；
    //     `tmem_buffer` 在 **`BsdInitConfig`（bsd.h）** 里 ⇒ **必须绕开 `socketInitialize`、
    //     直接调 `bsdInitialize`** —— 这正是 SysDVR 干的事（它直接调 bsd API）。
    //   ★ 代价：`bsdInitialize` 不注册 libnx 的 `"soc"` devoptab，而 libnx 的 `::poll`
    //     强制要求 fd 属于 `"soc"` ⇒ **本模式下 `::poll` 会失败**，必须改用 `bsdPoll`
    //     （已在 `protocol.cpp` 做 `transportPoll()` 转发），并且 `net` 工具会被挡掉。
    //   ★ 本档位**除"内存池来源"外，其余参数与 `likedmnt2` 完全一致** ⇒ 让"内存池从哪来"
    //     成为**唯一变量**。
    LikeSysDvr = 5,
    // ★★★ 2026-10-06 15:17 新增：**一字不改地照抄 `sys-botbase` 的写法**（用户要求"直接照抄"）。
    //
    //   ★ 为什么要单独一档：`sys-botbase` 的做法 = `socketInitializeDefault()`（= `bsd:u` +
    //     libnx 默认缓冲）+ `setsockopt(SO_REUSEADDR)` + `bind(INADDR_ANY)` + **`listen(fd, 3)`**
    //     + 阻塞 `poll(..., -1)` + **完全不注册 PSC**。
    //     ⇒ 这**几乎就是我们的 `listen` 模式**（差别只有 backlog 8 vs 3），而它**崩了**。
    //   ★★ **但它有一行我们【从来没有真正测过】**：**在 `socketInitialize` 之前
    //     `svcSleepThread(20 秒)`**（它源码里那行**没有注释、原因不明**的等待）。
    //     我 2026-10-06 15:00 把它部署过，但**三分钟后自己用下一个测试覆盖了** ⇒ 从未跑过。
    //   ⇒ 本档位 = 它的参数（backlog 3）+ 模式文件里那个延迟数字（`likeSysBotBase 20`）。
    //   ★ 这一档**没有** `likedmnt2` 那些花样（不用 `bsd:s`、照设 `SO_REUSEADDR`）—— 就是照抄。
    LikeSysBotBase = 6,
};

// 缺省模式：**安全优先**。这样"忘了放模式文件"或"模式文件读坏"都不会把主机拖进崩溃循环。
constexpr Mode kDefaultMode = Mode::Off;

inline Mode g_mode = kDefaultMode;

inline void setMode(Mode m) { g_mode = m; }
inline Mode mode() { return g_mode; }

// 需要建 socket + bind + listen 的模式（`Listen` / `AcceptOnly` / `LikeDmnt2` 都要）
inline bool listenEnabled() {
    // ★ 这里**不能**调 `isCopiedMode()`（它定义在本函数之后）—— 显式列全，避免依赖声明顺序。
    return g_mode == Mode::Listen || g_mode == Mode::AcceptOnly
        || g_mode == Mode::LikeDmnt2 || g_mode == Mode::LikeSysDvr
        || g_mode == Mode::LikeSysBotBase;
}
inline bool networkUp() { return g_mode != Mode::Off; }              // 会碰 bsd 服务
// ★ 是否长期保留监听口：只有 `AcceptOnly` 是"连上就收掉"，其余都是长期持有。
inline bool keepListenerOpen() { return g_mode != Mode::AcceptOnly && g_mode != Mode::Off
                                        && g_mode != Mode::BsdOnly; }

// ★ 建监听口前的延迟（秒）。**这是下一个待测的关键变量**：
//   Atmosphère 的 `dmnt.gen2` 是**运行期由专用线程懒建**监听口（`InitializeByTcp()` 只做
//   `socket::Initialize`，listen 发生在其后的 GDB 线程里），而我们是**开机 `main()` 里就建**。
//   ★ 这条正对应 `sys-botbase` 里那行**没有注释、原因不明、当年被放弃不抄**的
//     `svcSleepThread(20 秒)` —— 两处线索指向同一件事：**建口太早可能才是问题**。
//   用法：模式文件里可以带第二个数字，例如 `likedmnt2 45` ⇒ 开机后等 45 秒再建口。
inline int g_listenDelaySec = 0;
inline int listenDelaySec() { return g_listenDelaySec; }

// ★ 两条"照抄"路线的**共同**参数（`likedmnt2` 与 `likesysdvr` 完全一致 —— 刻意如此，
//   这样两者的差别只剩"socket 内存池从哪来"）。
//   ★ 来源（逐行核过）：`stratosphere/dmnt.gen2/source/dmnt2_gdb_server.cpp`
//     的 `GdbServerThreadFunction` 里是 `transport::Listen(fd, 0)` ⇒ **backlog = 0**；
//     它也不设 `SO_REUSEADDR`；socket 配置是 `socket::SystemConfigLightDefault`
//     ⇒ `m_system = true` ⇒ `service_type = (1<<1)` ⇒ **`bsd:s`**。
inline bool isCopiedMode() {
    return g_mode == Mode::LikeDmnt2 || g_mode == Mode::LikeSysDvr;
}
inline bool systemSocketService() { return isCopiedMode(); }   // 用 bsd:s 而不是 bsd:u
inline int  listenerBacklog()     {
    if (isCopiedMode()) return 0;                          // 照抄 dmnt.gen2 / SysDVR 的 0
    if (g_mode == Mode::LikeSysBotBase) return 3;           // ★ 照抄 sys-botbase 的 3
    return 8;
}
inline bool useReuseAddr()        { return !isCopiedMode(); }
inline int  bsdSessionCount()     { return isCopiedMode() ? 2 : 3; }
inline int  sbEfficiency()        { return isCopiedMode() ? 2 : 4; }
// ★★★ `likesysdvr`：**这条路原本被误判为"走不通"，2026-10-06 已纠正 —— 其实走得通。**
//
//   ★ 当时写的理由（**错的，已推翻**）：「`bsdInitialize` / `bsdExit` 在 libnx 里
//     没有导出 ⇒ 必须自己重写整个 bsd 客户端层」。
//   ★★ 纠正（2026-10-06 16:0x，在 `devkitpro/devkita64` 里直接 nm 实测）：
//     ```
//     $ aarch64-none-elf-nm /opt/devkitpro/libnx/lib/libnx.a | grep -iE 'bsd(Initialize|Exit)'
//     0000000000000000 T bsdExit
//     0000000000000000 T bsdInitialize
//     ```
//     ⇒ **两个都是导出的全局符号**，可以直接 `#include <switch/services/bsd.h>` 后调用。
//     ★ 教训：一句"某符号没导出"就否掉一整条路线，代价是白丢半天 —— **证据要当场贴出来**。
//   ★ 这条路线**仍然值得做**，因为它是"两个同机参照共有、而独我们没有"的**唯一**具体差异：
//     · `dmnt.gen2`（22225，不崩）→ `ams::socket` 内部把静态数组 `g_socket_memory`
//       作为 `BsdInitConfig::tmem_buffer` 传给 `bsdInitialize`
//     · `SysDVR`（6666）      → 同理，用自己的 `TmemBackingBuffer`
//     · **我们** → 走 `socketInitialize` ⇒ 内存池由 `tmemCreate` 从**堆里新分配**
//   ★ 代价（为什么还没做）：直接调 `bsdInitialize` **不会注册 libnx 的 `"soc"` devoptab**
//     ⇒ `::poll` / `::send` / `::recv` / `::accept` 全部失效，必须整条换成
//     `bsdPoll` / `bsdSend` / `bsdRecv` / `bsdAccept`（照抄 libstratosphere 的
//     `socket_api.os.horizon.cpp`）⇒ 大约 30 个调用点要改。
//     ⇒ **留作 PSC 收口之后的下一个候选**（PSC 那条有我们自己更强的证据支持，先做）。
inline bool useStaticTmem() { return false; }
inline bool needBsdPoll()   { return false; }

inline const char* modeName(Mode m) {
    switch (m) {
        case Mode::Off:        return "off";
        case Mode::BsdOnly:    return "bdsonly";
        case Mode::Listen:     return "listen";
        case Mode::AcceptOnly: return "acceptonly";
        case Mode::LikeDmnt2:  return "likedmnt2";
        case Mode::LikeSysDvr: return "likesysdvr";
        case Mode::LikeSysBotBase: return "likesysbotbase";
    }
    return "?";
}

// 解析模式文件内容（大小写不敏感、容忍空白/换行）。不认识的值返回 false（保留缺省）。
// ★ `outDelay`（可选）：解析模式名后面跟的**第一个数字**当"建口延迟秒数"，
//   例如文件里写 `likedmnt2 45` ⇒ 模式 likedmnt2 + 开机后等 45 秒再建监听口。
inline bool parseMode(const char* text, Mode* out, int* outDelay = nullptr) {
    if (text == nullptr || out == nullptr) return false;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;

    struct Entry { const char* name; Mode m; };
    static const Entry kEntries[] = {
        {"off", Mode::Off}, {"bdsonly", Mode::BsdOnly},
        {"listen", Mode::Listen}, {"acceptonly", Mode::AcceptOnly},
        {"likedmnt2", Mode::LikeDmnt2},
        {"likesysdvr", Mode::LikeSysDvr},
        {"likesysbotbase", Mode::LikeSysBotBase},
    };
    for (const Entry& e : kEntries) {
        const size_t n = std::strlen(e.name);
        if (std::strncmp(text, e.name, n) != 0) continue;
        // 后面必须跟着空白/换行/结束，避免 "listen_x" 被当成 listen
        const char c = text[n];
        if (c == '\0' || c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (outDelay != nullptr) {
                int v = 0;
                bool any = false;
                for (const char* p = text + n; *p != '\0'; ++p) {
                    if (*p >= '0' && *p <= '9') {
                        v = v * 10 + (*p - '0');
                        any = true;
                        if (v > 600) { v = 600; break; }   // 上限 10 分钟，别把自己坑死
                    } else if (any) {
                        break;
                    }
                }
                *outDelay = any ? v : 0;
            }
            *out = e.m;
            return true;
        }
    }
    return false;
}

}  // namespace net
}  // namespace nxc
