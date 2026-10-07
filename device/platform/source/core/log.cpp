#include "platform.hpp"
#include "sd.hpp"

#include <switch.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace nxc {
namespace log {

namespace {

constexpr const char* kDir            = "/config/nxc";
constexpr const char* kBreadcrumbPath = "/config/nxc/boot_progress.txt";
constexpr const char* kHeartbeatPath  = "/config/nxc/heartbeat.txt";

// 内存环形日志：不写卡，PC 连上后用 log.dump 取走。
constexpr int kRingLines     = 128;
constexpr int kRingLineBytes = 160;

char s_ring[kRingLines][kRingLineBytes];
int  s_ringHead  = 0;
int  s_ringCount = 0;
int  s_boot      = 0;

// Tegra X1 的 system tick 是 19.2 MHz。这里只是给日志加个相对时间，不追求绝对精度。
long long uptimeMs() {
    return static_cast<long long>(svcGetSystemTick() / 19200ULL);
}

void writeWholeFile(const char* path, const char* text) {
    FILE* f = nxc::sd::open(path, "wb");
    if (f == nullptr) return;
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);  // 立刻关：启动路径上不做重 IO，也不长时间占着文件
}

int readBootCount() {
    FILE* f = nxc::sd::open(kHeartbeatPath, "rb");
    if (f == nullptr) return 0;
    char buf[256];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';

    const char* p = std::strstr(buf, "boot=");
    if (p == nullptr) return 0;
    p += 5;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
    return v;
}

void ringLine(const char* text) {
    std::snprintf(s_ring[s_ringHead], kRingLineBytes, "%s", text);
    s_ringHead = (s_ringHead + 1) % kRingLines;
    if (s_ringCount < kRingLines) ++s_ringCount;
}

}  // namespace

void init() {
    static bool done = false;
    if (done) return;  // 幂等：__appInit 里调一次就够了，重复调用不该让开机次数乱跳
    done = true;

    ::mkdir(kDir, 0777);  // 已存在会失败，无所谓

    s_boot = readBootCount() + 1;

    char text[192];
    std::snprintf(text, sizeof(text), "boot=%d uptime_ms=%lld\n", s_boot, uptimeMs());
    writeWholeFile(kHeartbeatPath, text);

    ringLine("platform boot");
}

void breadcrumb(const char* fmt, ...) {
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char text[288];
    std::snprintf(text, sizeof(text), "boot=%d uptime_ms=%lld\nstep=%s\n",
                  s_boot, uptimeMs(), body);
    writeWholeFile(kBreadcrumbPath, text);  // 覆盖写：崩了以后最后这一行就是凶手

    char line[kRingLineBytes];
    // 显式限长：body 最长可到 191 字节，直接拼会超过环形行宽（也触发 -Wformat-truncation）。
    std::snprintf(line, sizeof(line), "[%lldms] %.120s", uptimeMs(), body);
    ringLine(line);
}

// ★★★ 2026-10-06 新增「黑匣子」（详见 platform.hpp 上的说明）。
//   与 breadcrumb 的两点关键区别：
//     ① 文件名带**开机号** ⇒ 崩过之后再开机**不会覆盖**崩前那一份；
//     ② 同时记 **系统 tick** 与 **RTC 墙钟** ⇒ 睡眠时 tick 停、RTC 继续走，
//        **两者差值一跳就是"确实睡过"的铁证**。
void watch(const char* fmt, ...) {
    if (!nxc::sd::kEnabled) return;   // 没 SD 写不了 —— 静默跳过（这是预期路径，不是错）

    char body[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    // 墙钟：`time` 服务要经 `sm:`，所以临时开一下 sm、拿完关掉。
    // ★ 拿不到就写 0 —— **绝不能因为拿不到时间就不写这一条**（tick 本身也有用）。
    //
    // ★★ 2026-10-06 修正：第一版只取了 `TimeType_LocalSystemClock` 并按秒打印，
    //    结果打出来是 `wall_s=1`（≈1.x 秒）—— **那不是绝对时间，拿它做不了睡眠检测**。
    //    ⇒ 现在**两个时钟都按原始纳秒值记下来**（不自己换算），
    //      让 PC 侧一眼看出哪个是"一直往前走的绝对钟"、哪个一跳就是睡过。
    // ★★ 注意：libnx 的 `timeGetCurrentTime` 要的是 `u64*`（本平台 `u64` = `unsigned long`），
    //   所以这里必须用 `u64` 而不是 `unsigned long long`（后者编译不过，编译器会报
    //   "invalid conversion from 'long long unsigned int*' to 'u64*'"）。
    //
    // ★★★ 2026-10-06 又一个修正（这次是致命的，值得记）：**绝不能无条件 `smClose()`！**
    //   踩坑经过：`main()` 跑 socket 重试循环时会**借开** `sm`（`socketInitialize` 要经 `sm:`），
    //   而循环里的失败路径会调 `watch()` —— 它当时无条件 `smClose()` ⇒ **把调用方正在用的
    //   `sm` 会话关掉了** ⇒ 之后每一次 `socketInitialize`（含降级重试）都失败
    //   ⇒ 60 秒后模块**静默退出、47800 完全不出现** —— 而现象看起来像"不崩了"（**假阳性**）。
    //   ★ 这个 bug 只在"默认配置失败"时才触发，所以恰好只在 `likedmnt2` 模式下咬人。
    //   ⇒ 现在改成：**只在"我们自己开出来的"时候才关**。
    const bool smWasAlreadyOpen = nxc::host::smIsOpen();
    if (!smWasAlreadyOpen) nxc::host::smOpen();

    u64 wallUserNs = 0, wallLocalNs = 0;
    if (R_SUCCEEDED(timeInitialize())) {
        timeGetCurrentTime(TimeType_UserSystemClock,  &wallUserNs);
        timeGetCurrentTime(TimeType_LocalSystemClock, &wallLocalNs);
        timeExit();
    }

    if (!smWasAlreadyOpen) nxc::host::smClose();

    char path[128];
    std::snprintf(path, sizeof(path), "/config/nxc/watch_boot%d.txt", s_boot);

    char text[352];
    std::snprintf(text, sizeof(text),
                  "boot=%d uptime_ms=%lld wall_user_ns=%llu wall_local_ns=%llu\nstep=%s\n",
                  s_boot, uptimeMs(),
                  static_cast<unsigned long long>(wallUserNs),
                  static_cast<unsigned long long>(wallLocalNs), body);
    writeWholeFile(path, text);   // 覆盖写：每次刷新，崩后留下最后一条
}

void ring(const char* fmt, ...) {
    char body[kRingLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    ringLine(body);
}

void dump(Reply& reply, int maxLines) {
    if (maxLines < 0) maxLines = 0;
    if (maxLines > s_ringCount) maxLines = s_ringCount;
    if (maxLines > kRingLines) maxLines = kRingLines;

    reply.kvi("count", maxLines);
    reply.kvi("boot", s_boot);

    int idx = (s_ringHead - maxLines + kRingLines * 2) % kRingLines;
    for (int i = 0; i < maxLines; ++i) {
        reply.raw(s_ring[idx]);
        idx = (idx + 1) % kRingLines;
    }
}

int bootCount() { return s_boot; }

const char* heartbeatPath() { return kHeartbeatPath; }

}  // namespace log

// ---------------------------------------------------------------- 暴露给工具的服务
// 工具只通过 nxc::host 用平台能力，不碰核心内部实现。
namespace host {

void log(const char* fmt, ...) {
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    log::ring("%s", body);
}

void breadcrumb(const char* fmt, ...) {
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    log::breadcrumb("%s", body);
}

int bootCount() { return log::bootCount(); }
const char* heartbeatPath() { return log::heartbeatPath(); }
void dumpLog(Reply& reply, int maxLines) { log::dump(reply, maxLines); }

}  // namespace host
}  // namespace nxc
