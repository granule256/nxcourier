// 内置工具 · screen：**抓一张主机画面**（让 AI 从"闭眼"变成"睁眼"）。
//
// ## 依据
//
// M1 探针「组 5」实测 `capsscInitialize()` 返回 `rc=0x00000000`，可用。
// 用的是 libnx 封装的 **JPEG 截图**：
//     capsscCaptureJpegScreenShot(u64* out_jpeg_size, void* buf, size_t buf_size,
//                                 ViLayerStack layer_stack, s64 timeout);
// ★ 顺带更正一条流传说法：抓画面**不需要**绕道 SysDVR。
//   `caps:u` / `caps:su` 确实打不开（`rc=0x00066CCE`），但 **`capssc` 可以**。
//
// ## 为什么写文件而不是直接回包
//
// 一张 720p JPEG 通常几百 KB，而协议单条应答上限是 8KB。
// 所以**存到 SD 卡**，再由 PC 侧分块取走（`fs.read` 或 MCP 的取文件工具）。
#include "nxc_sdk.hpp"
#include "../core/sd.hpp"

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kDefaultPath = "/config/nxc/screen.jpg";

// ★ 缓冲大小为什么要可调：
//   ★ **2026-10-06 更正**：这段注释原来写"静态堆只有 **0x80000 = 512KB**"——那是早期配置，
//     早已不是现状：`main.cpp` 里 `INNER_HEAP_SIZE = 0x480000`（**4.5MB**）。
//     （当年堆只有 512KB 时，一次 malloc 640KB 确实会直接 `out-of-memory`。）
//   libnx 的头文件写着官方软件用 `CAPSSC_JPEG_BUFFER_SIZE = 0x80000`(512KB)，所以默认给 512KB；
//   万一某帧特别大，可以不重编译、直接用 max_kb= 调大再试。
constexpr int kDefaultMaxKb = 512;
constexpr int kHardMaxKb    = 1536;

// ★★★ capssc 会话的生命周期 —— 2026-10-05 深夜改。
//
// 原来的写法是「开了就永远不关」（一个 static 标志记着，全代码没有一处 capsscExit()）。
//
// 为什么必须改：capssc 是「屏幕捕获」服务，在**显示 / 视频路径**上。
// 而 `omm`（Operation Mode Manager，管底座/手持模式切换与音视频输出）
// **在睡眠时会执行 `EnableAudioVisual`**，并且实测**每次睡眠都崩在同一个偏移**。
//
// 怀疑链：**我们在显示栈上留着一个从不释放的捕获会话**
//         ⇒ `omm` 重开音视频时那条路径上拿到意外状态 ⇒ 崩。
//
// 而且这本来就是异类：`nifm` / `psm` / `clkrst` / `ts` / `apm` 全都是「开→用→关」，
// 只有它常开。（`hiddbg` 常开有正当理由：虚拟手柄必须保持挂载。）
//
// ⇒ 改成 RAII：**进命令时开、出命令时关**。
//   既不留下跨睡眠的会话，也不影响命令内部的连续抓帧（作用域覆盖整条命令）。
// ★★★ A/B 测试开关（2026-10-05 深夜）：false = **整块关掉屏幕捕获**。
//   为什么要单独一个开关：为了把「capssc 相关」这个嫌疑**整体摘掉**，
//   和上面 `sd::kEnabled` 一起做 A/B —— 两个嫌疑同时关，看 omm 还崩不崩。
//   ★ 关掉之后 `screen.*` 全部返回 `capssc-unavailable`，不再碰 capssc 服务。
//   ★ 2026-10-05 A/B 结论：**capssc 关掉之后 omm 照样崩** ⇒ 已恢复为 true。
constexpr bool kScreenEnabled = true;

struct CapsscScope {
    bool ok;
    CapsscScope() : ok(kScreenEnabled && R_SUCCEEDED(capsscInitialize())) {}
    ~CapsscScope() { if (ok) capsscExit(); }
    CapsscScope(const CapsscScope&) = delete;
    CapsscScope& operator=(const CapsscScope&) = delete;
};

// ★★ 走 sys-botbase 验证过的那条路：`capssc` 的**调试专用命令 1204**。
//
// 为什么不用 libnx 的 `capsscCaptureJpegScreenShot`：实测它在 sysmodule 里一律返回
// `rc=0x000668CE`（换 layer、换缓冲大小都一样），而 `caps:u`/`caps:su` 同样是
// `rc=0x00066CCE` —— 同一族限制，大概是需要更高的调用者权限。
// sys-botbase 作为常驻 sysmodule 能用截图，靠的就是下面这条 1204：
//     serviceDispatchInOut(capsscGetServiceSession(), 1204, in, out_size,
//         .buffer_attrs = { HipcMapTransferAllowsNonSecure | HipcMapAlias | Out },
//         .buffers = { { buffer, buffer_size } });
// 其中 in = { u32 0, u64 超时(ns) }，出参是实际大小。
Result captureForDebug(void* buffer, size_t buffer_size, u64* out_size) {
    struct { u32 a; u64 b; } in = { 0, 10000000000ULL };   // 超时 10 秒
    return serviceDispatchInOut(capsscGetServiceSession(), 1204, in, *out_size,
        .buffer_attrs = { SfBufferAttr_HipcMapTransferAllowsNonSecure |
                          SfBufferAttr_HipcMapAlias |
                          SfBufferAttr_Out },
        .buffers = { { buffer, buffer_size } });
}

bool parseLayer(const char* name, ViLayerStack* out) {
    if (name == nullptr || *name == '\0') { *out = ViLayerStack_Lcd; return true; }
    if (std::strcmp(name, "lcd") == 0)       { *out = ViLayerStack_Lcd; return true; }
    if (std::strcmp(name, "default") == 0)   { *out = ViLayerStack_Default; return true; }
    if (std::strcmp(name, "screenshot") == 0){ *out = ViLayerStack_Screenshot; return true; }
    if (std::strcmp(name, "recording") == 0) { *out = ViLayerStack_Recording; return true; }
    if (std::strcmp(name, "lastframe") == 0) { *out = ViLayerStack_LastFrame; return true; }
    return false;
}

void cmdInfo(const nxc::Args&, nxc::Reply& r) {
    CapsscScope cap;   // 开了就关，不留到命令之外
    r.kv("capssc", cap.ok ? "available" : "unavailable");
    r.kv("default_path", kDefaultPath);
    r.raw("layer 可选：lcd（默认，LCD 上实际显示的）|default|screenshot|recording|lastframe");
    r.raw("note: 出的是 JPEG；存到 SD 卡后用 fs.read 或 MCP 的取文件工具拿回 PC");
    r.kvi("default_max_kb", kDefaultMaxKb);
    r.raw("note: 抓的是「那一刻」的画面，游戏在动的话每次结果都会不同");
    r.raw("note: 缓冲默认 512KB（libnx 官方口径 CAPSSC_JPEG_BUFFER_SIZE）；max_kb= 只能往大调 —— 它改的是抓帧缓冲容量，不是输出大小");
    r.raw("wait_change: 设备端连抓帧、逐字节比对，画面一变就回 —— 操作后不用再盲等固定秒数");
    r.raw("note: capssc 会话「进命令时开、出命令时关」，不会跨命令/跨睡眠持有");
}

// FNV-1a 64 —— 用来判断"这一帧和上一帧是不是同一张图"。
// 选它的理由：够快（190KB 也就几十微秒）、零依赖、确定性。
// 这里做的是**变更检测**不是密码学用途，不需要更强的哈希。
u64 fnv1a64(const unsigned char* data, u64 len) {
    u64 hash = 1469598103934665603ULL;
    for (u64 i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// 抓一帧到 buf。先走 1204（sys-botbase 验证过的调试命令），不行再退回 libnx 的 JPEG 接口。
// 抽出来是为了让 wait_change 能复用 —— 它要连抓好几帧。
Result captureInto(unsigned char* buf, size_t cap, u64* out_size,
                   long long timeoutMs, ViLayerStack layer, const char** out_via) {
    u64 size = 0;
    Result rc = captureForDebug(buf, cap, &size);
    *out_via = "capssc:1204";
    if (R_FAILED(rc)) {
        size = 0;
        rc = capsscCaptureJpegScreenShot(&size, buf, cap, layer, timeoutMs * 1000000LL);
        *out_via = "libnx-jpeg";
        if (R_FAILED(rc)) return rc;
    }
    // 诊断用哨兵值：让调用方分得清"服务返回失败"和"返回了但大小不合理"
    if (size == 0 || size > cap) return static_cast<Result>(0x80000000u | 0xEu);
    *out_size = size;
    return 0;
}

void cmdCapture(const nxc::Args& a, nxc::Reply& r) {
    CapsscScope cap;
    if (!cap.ok) { r.fail(503, "capssc-unavailable"); return; }

    const char* path = a.get("path");
    if (path == nullptr || *path == '\0') path = kDefaultPath;

    ViLayerStack layer = ViLayerStack_Lcd;
    if (!parseLayer(a.get("layer"), &layer)) { r.fail(400, "layer-invalid"); return; }

    long long timeoutMs = a.getInt("timeout_ms", 3000);
    if (timeoutMs <= 0 || timeoutMs > 30000) timeoutMs = 3000;

    // JPEG 缓冲按需 malloc（几百 KB，不放进 BSS）。
    //
    // ★★★ 2026-10-06 晚：`max_kb` 的语义被澄清，并且"越界静默回落"改成"明确报错"。
    //
    //   两件事要分清（原来混着，导致它看起来"被忽略"）：
    //     ① 它是**抓帧缓冲的容量上限**，**不是"输出大小上限"** —— capssc 编出多大就写多大
    //        （实测一帧 187KB），调它**改不了输出文件的字节数**；
    //     ② 它有**硬下限**：capssc 少于 512KB 会直接失败（libnx 的
    //        `CAPSSC_JPEG_BUFFER_SIZE = 0x80000`）⇒ `max_kb` 只能往**大**调，不能往小调。
    //   ⇒ 传 `max_kb=8` 时，旧代码走"越界就回落默认值"，**既不报错也不生效**，
    //     只能靠回包里的 `buffer_kb` 反推（实测就是这么被记成"参数被忽略"的）。
    //     现在：显式给了越界值就报 400，让"没生效"看得见。
    long long maxKb = a.getInt("max_kb", kDefaultMaxKb);
    if (a.get("max_kb") != nullptr && (maxKb < 512 || maxKb > kHardMaxKb)) {
        r.failf(400, "max-kb-out-of-range: 允许 512..%d（capssc 硬性要 ≥512KB；这个参数只影响"
                     "抓帧缓冲容量，调大也改不了输出大小）", kHardMaxKb);
        return;
    }
    if (maxKb < 512 || maxKb > kHardMaxKb) maxKb = kDefaultMaxKb;
    const size_t bufBytes = static_cast<size_t>(maxKb) * 1024;

    auto* buf = static_cast<unsigned char*>(std::malloc(bufBytes));
    if (buf == nullptr) {
        // ★ 2026-10-06 更正：这里原来写"静态堆只有 512KB"（那是早期配置，会误导排查方向）。
        //   堆实际大小以 `main.cpp` 的 `INNER_HEAP_SIZE`（当前 4.5MB）为准。
        r.failf(500, "out-of-memory: 申请 %lldKB 失败（静态堆 INNER_HEAP_SIZE 见 main.cpp）", maxKb);
        return;
    }

    nxc::host::breadcrumb("screen.capture layer=%d -> %s", static_cast<int>(layer), path);

    const char* via = "";
    u64 jpegSize = 0;
    const Result rc = captureInto(buf, bufBytes, &jpegSize, timeoutMs, layer, &via);
    if (R_FAILED(rc)) {
        std::free(buf);
        r.failf(500, "capture-failed rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }

    char magic[16];
    std::snprintf(magic, sizeof(magic), "%02X%02X%02X%02X", buf[0], buf[1], buf[2], buf[3]);

    FILE* f = nxc::sd::open(path, "wb");
    if (f == nullptr) {
        std::free(buf);
        r.fail(500, "cannot-open-for-write");
        return;
    }
    const size_t written = std::fwrite(buf, 1, static_cast<size_t>(jpegSize), f);
    std::fclose(f);
    std::free(buf);

    if (written != jpegSize) {
        r.failf(500, "short-write %llu/%llu", static_cast<unsigned long long>(written),
                static_cast<unsigned long long>(jpegSize));
        return;
    }

    r.kv("path", path);
    r.kvi("bytes", static_cast<long long>(jpegSize));
    r.kv("via", via);
    r.kv("magic", magic);          // JPEG 应为 FFD8FFE0 / FFD8FFE1
    r.kvi("layer", static_cast<long long>(layer));
    r.kvi("buffer_kb", maxKb);
    r.raw("next: 用 fs.read path=<上面的 path> 分块取回，或让 PC 侧取文件工具自动拉回");
}

// ★★★ 等画面变化 —— "边看边控制"最关键的一块。
//
// 原来的做法（PC 侧）是"按一个键 → **盲等**几秒 → 抓一张图"：等短了不够，等长了白等，
// 而且永远不知道中间发生了什么。这个方法是**事件驱动**的：设备端连着抓帧，
// 画面一变就立刻回 —— 把"操作之后要等多久"从一个猜测变成一条事实。
//
// ## ★★ 为什么用哈希，而不是保存上一帧的像素
//
// 两个原因，第二个是硬约束：
//
// 1. **JPEG 编码是确定性的** —— 同样的画面编出来的字节完全一样。所以"哈希相同"
//    就等价于"画面相同"，不存在压缩噪声导致误报这回事。
// 2. ★ **capssc 要求缓冲至少 512KB**（0x80000）。实测给 256KB 会直接返回
//    `rc=0x000668CE`、`size=0` —— 这不是"图太大装不下"，是服务端的**硬性下限**。
//    ⇒ 若改成"存两块像素逐字节比对"，光缓冲就得 1MB。
//    ★ 2026-10-06 注记：这里原来写"正好撑爆我们的静态堆"——那是堆只有 512KB 时的实情；
//      现在堆是 4.5MB（见 main.cpp）。**但"只保留哈希"仍然照旧保留**：一个 u64 比一块
//      512KB 缓冲省得多，而且"JPEG 编码确定性"这条理由本来就够。
//
// 所以：**只保留上一帧的哈希（一个 u64），缓冲只用一块。**
//
// ## 关于 min_changed_bytes
//
// 哈希是"变了/没变"的二值判断，没有"变了多少"这个中间量。
// 但**尺寸变化**是个有用的信号：`size_changed=1` 说明帧长度都变了，通常是较大的画面变化。
void cmdWaitChange(const nxc::Args& a, nxc::Reply& r) {
    // capssc 的服务会话必须先初始化（漏了这句就是抓帧必失败）。
    CapsscScope cap;
    if (!cap.ok) { r.fail(503, "capssc-unavailable"); return; }

    long long timeoutMs = a.getInt("timeout_ms", 5000);
    if (timeoutMs <= 0 || timeoutMs > 60000) timeoutMs = 5000;
    long long intervalMs = a.getInt("interval_ms", 200);
    if (intervalMs < 30) intervalMs = 30;
    if (intervalMs > 2000) intervalMs = 2000;

    // ★ 默认必须给足 512KB —— capssc 的硬性下限。
    // ★ 显式传入越界值就报错（与 cmdCapture 一致）：原来静默回落默认值 ⇒ "没生效"看不出来。
    long long maxKb = a.getInt("max_kb", kDefaultMaxKb);
    if (a.get("max_kb") != nullptr && (maxKb < 512 || maxKb > kHardMaxKb)) {
        r.failf(400, "max-kb-out-of-range: 允许 512..%d（capssc 硬性要 ≥512KB）", kHardMaxKb);
        return;
    }
    if (maxKb < 512 || maxKb > kHardMaxKb) maxKb = kDefaultMaxKb;
    const size_t bufBytes = static_cast<size_t>(maxKb) * 1024;

    ViLayerStack layer = ViLayerStack_Lcd;
    if (!parseLayer(a.get("layer"), &layer)) { r.fail(400, "layer-invalid"); return; }

    auto* buf = static_cast<unsigned char*>(std::malloc(bufBytes));
    if (buf == nullptr) {
        r.failf(500, "out-of-memory: 需要 %lldKB（静态堆 INNER_HEAP_SIZE 见 main.cpp）", maxKb);
        return;
    }

    const char* via = "";
    u64 baselineSize = 0;
    const Result rcBase = captureInto(buf, bufBytes, &baselineSize, 3000, layer, &via);
    if (R_FAILED(rcBase)) {
        std::free(buf);
        r.failf(500, "baseline-capture-failed rc=0x%08X size=%llu cap=%zu",
                static_cast<unsigned>(rcBase),
                static_cast<unsigned long long>(baselineSize), bufBytes);
        return;
    }
    const u64 baselineHash = fnv1a64(buf, baselineSize);

    nxc::host::breadcrumb("screen.wait_change timeout=%lldms interval=%lldms", timeoutMs, intervalMs);

    const u64 startTick = armGetSystemTick();
    int frames = 0;
    long long elapsedMs = 0;
    bool changed = false;
    u64 lastSize = baselineSize;
    u64 lastHash = baselineHash;

    for (;;) {
        svcSleepThread(static_cast<u64>(intervalMs) * 1000000ULL);

        u64 size = 0;
        if (R_FAILED(captureInto(buf, bufBytes, &size, 3000, layer, &via))) break;
        ++frames;
        lastSize = size;
        lastHash = fnv1a64(buf, size);
        elapsedMs = static_cast<long long>(armTicksToNs(armGetSystemTick() - startTick) / 1000000ULL);

        if (lastHash != baselineHash) { changed = true; break; }
        if (elapsedMs >= timeoutMs) break;
    }

    std::free(buf);

    nxc::host::log("screen.wait_change: changed=%d frames=%d elapsed=%lldms",
                   changed ? 1 : 0, frames, elapsedMs);

    r.kvi("changed", changed ? 1 : 0);
    r.kvi("frames", frames);
    r.kvi("elapsed_ms", elapsedMs);
    r.kvi("interval_ms", intervalMs);
    r.kvf("baseline_hash", "0x%016llX", static_cast<unsigned long long>(baselineHash));
    r.kvf("last_hash", "0x%016llX", static_cast<unsigned long long>(lastHash));
    r.kvi("baseline_bytes", static_cast<long long>(baselineSize));
    r.kvi("last_bytes", static_cast<long long>(lastSize));
    r.kvi("size_changed", lastSize != baselineSize ? 1 : 0);
    r.raw("changed=1 表示画面确实变了（JPEG 确定性 ⇒ 哈希不同即画面不同）");
    r.raw("changed=0 表示超时内画面没动；下次可以把 timeout_ms 调大");
    if (frames == 0) r.raw("warn: frames=0，一帧都没抓到 —— capssc 或内存有问题");
}

// ★★★ 等画面**稳定** —— 补上 wait_change 的短板。
//
// ## 为什么需要它（实测教训）
//
// `wait_change` 是"等它变"。但真机实测发现：**主界面本身一直在动**
// （图标动画、正在游玩指示、预览动画），所以空闲时它也会立刻返回 changed=1 ——
// "空闲"在这种界面上根本不是个有意义的状态。
//
// 而操作之后真正想知道的是：**"还在加载，还是已经到下一屏了？"**
// 那是"画面**稳定下来**"这个事件。所以需要反过来等：
// **连续 N 帧哈希都一样 ⇒ 画面停住了 ⇒ 回。**
//
// 典型用法：
//     input.seq steps=A:120        # 按 A
//     screen.wait_stable           # 等这一屏稳定 ⇒ 加载完了
//
// 注意：**动画循环的界面（例如游戏内）永远不会稳定**，那种情况用 wait_change。
void cmdWaitStable(const nxc::Args& a, nxc::Reply& r) {
    CapsscScope cap;
    if (!cap.ok) { r.fail(503, "capssc-unavailable"); return; }

    long long timeoutMs = a.getInt("timeout_ms", 10000);
    if (timeoutMs <= 0 || timeoutMs > 60000) timeoutMs = 10000;
    long long intervalMs = a.getInt("interval_ms", 200);
    if (intervalMs < 30) intervalMs = 30;
    if (intervalMs > 2000) intervalMs = 2000;
    long long needFrames = a.getInt("stable_frames", 3);
    if (needFrames < 2) needFrames = 2;
    if (needFrames > 30) needFrames = 30;

    // ★ 显式传入越界值就报错（与 cmdCapture 一致）：原来静默回落默认值 ⇒ "没生效"看不出来。
    long long maxKb = a.getInt("max_kb", kDefaultMaxKb);
    if (a.get("max_kb") != nullptr && (maxKb < 512 || maxKb > kHardMaxKb)) {
        r.failf(400, "max-kb-out-of-range: 允许 512..%d（capssc 硬性要 ≥512KB）", kHardMaxKb);
        return;
    }
    if (maxKb < 512 || maxKb > kHardMaxKb) maxKb = kDefaultMaxKb;
    const size_t bufBytes = static_cast<size_t>(maxKb) * 1024;

    ViLayerStack layer = ViLayerStack_Lcd;
    if (!parseLayer(a.get("layer"), &layer)) { r.fail(400, "layer-invalid"); return; }

    auto* buf = static_cast<unsigned char*>(std::malloc(bufBytes));
    if (buf == nullptr) { r.failf(500, "out-of-memory: 需要 %lldKB", maxKb); return; }

    nxc::host::breadcrumb("screen.wait_stable timeout=%lldms interval=%lldms need=%lld",
                          timeoutMs, intervalMs, needFrames);

    const u64 startTick = armGetSystemTick();
    int frames = 0;
    int held = 0;
    long long elapsedMs = 0;
    bool stable = false;
    u64 prevHash = 0;
    bool havePrev = false;
    u64 lastSize = 0;
    u64 stableHash = 0;

    for (;;) {
        u64 size = 0;
        const char* via = "";
        if (R_FAILED(captureInto(buf, bufBytes, &size, 3000, layer, &via))) break;
        ++frames;
        lastSize = size;
        const u64 hash = fnv1a64(buf, size);
        elapsedMs = static_cast<long long>(armTicksToNs(armGetSystemTick() - startTick) / 1000000ULL);

        if (havePrev && hash == prevHash) {
            ++held;
            stableHash = hash;
            if (held + 1 >= needFrames) { stable = true; break; }
        } else {
            held = 0;
        }
        prevHash = hash;
        havePrev = true;

        if (elapsedMs >= timeoutMs) break;
        svcSleepThread(static_cast<u64>(intervalMs) * 1000000ULL);
    }

    std::free(buf);

    nxc::host::log("screen.wait_stable: stable=%d frames=%d held=%d elapsed=%lldms",
                   stable ? 1 : 0, frames, held, elapsedMs);

    r.kvi("stable", stable ? 1 : 0);
    r.kvi("frames", frames);
    r.kvi("held_frames", held + (stable ? 1 : 0));
    r.kvi("stable_frames", needFrames);
    r.kvi("elapsed_ms", elapsedMs);
    r.kvi("interval_ms", intervalMs);
    r.kvf("hash", "0x%016llX", static_cast<unsigned long long>(stableHash));
    r.kvi("bytes", static_cast<long long>(lastSize));
    r.raw("stable=1 表示连续 stable_frames 帧完全一样 ⇒ 画面停住了（加载/过渡已结束）");
    r.raw("stable=0 表示超时前它一直没停 —— 可能界面本来就在循环动画，那种情况用 wait_change");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
//
// ★★ 规矩：**这个工具的全部知识都写在本文件里** —— 核心不认识 `screen` 是什么，
//    `tools.list` / `tools.doc` 只是把这里的内容汇总出去。**AI 就是靠这些字学会用它。**
namespace {

const nxc::Param kLayerParam = {
    "layer", "lcd|default|screenshot|recording|lastframe", false, "lcd",
    "抓哪一层，默认 lcd（LCD 上实际显示的）",
};
const nxc::Param kMaxKbParam = {
    "max_kb", "int", false, "512",
    "抓帧缓冲容量 KB（允许 512..1536；★只能调大，改不了输出大小）",
};

const nxc::Param kCaptureParams[] = {
    {"path",       "str", false, "/config/nxc/screen.jpg", "输出 JPEG 的路径（SD 卡上）"},
    kLayerParam,
    {"timeout_ms", "int", false, "3000", "单次抓帧超时毫秒（允许 1..30000；越界回落 3000）"},
    kMaxKbParam,
};
const nxc::Param kWaitChangeParams[] = {
    {"timeout_ms",  "int", false, "5000", "最长等待毫秒（允许 1..60000；越界回落 5000）"},
    {"interval_ms", "int", false, "200",  "抓帧间隔毫秒（会被钳到 30..2000）"},
    kLayerParam,
    kMaxKbParam,
};
const nxc::Param kWaitStableParams[] = {
    {"timeout_ms",    "int", false, "10000", "最长等待毫秒（允许 1..60000；越界回落 10000）"},
    {"interval_ms",   "int", false, "200",   "抓帧间隔毫秒（会被钳到 30..2000）"},
    {"stable_frames", "int", false, "3",     "连续多少帧一样算稳定（会被钳到 2..30）"},
    kLayerParam,
    kMaxKbParam,
};

const nxc::MethodInfo kInfoInfo = {
    nullptr, 0, "read",
    "看屏幕捕获能力与默认参数。\n"
    "作用: 确认 capssc 是否可用、默认保存路径、缓冲默认大小、可选 layer。\n"
    "参数: 无。\n"
    "示例: screen.info\n"
    "注意:\n"
    "  ★ capssc 会话是「进命令时开、出命令时关」，不跨命令／睡眠持有。\n"
    "  ★ 抓的是「那一刻」的画面，游戏在动时每次结果都不同。\n"
    "返回:\n"
    "  capssc=available|unavailable default_path= default_max_kb=，并附 layer 与说明。\n"
    "相关: screen.capture screen.wait_change screen.wait_stable\n",
};
const nxc::MethodInfo kInfoCapture = {
    kCaptureParams, 4, "write",
    "抓一张画面，出 JPEG 存到 SD 卡（AI 从「闭眼」变「睁眼」）。\n"
    "作用: 截当前画面一张存成 jpg，之后用 fs.read 或 PC 侧工具取回。\n"
    "参数:\n"
    "  path       = 输出路径，默认 /config/nxc/screen.jpg。\n"
    "  layer      = 抓哪一层，默认 lcd；可选 default|screenshot|recording|lastframe。\n"
    "  timeout_ms = 单次抓帧超时毫秒，默认 3000；越界（≤0 或 >30000）回落 3000。\n"
    "  max_kb     = 抓帧缓冲容量 KB，默认 512，允许 512..1536。\n"
    "示例:\n"
    "  screen.capture\n"
    "  screen.capture path=/config/nxc/shot.jpg layer=lcd\n"
    "注意:\n"
    "  ★★ max_kb 是「抓帧缓冲容量」而不是「输出大小」——capssc 编多大就写多大，\n"
    "     调它改不了输出文件的字节数。\n"
    "  ★★ capssc 硬性要求缓冲 ≥512KB ⇒ max_kb 只能调大、调不了更小；\n"
    "     显式传越界值会回 400（不再静默回落）。\n"
    "  ★ 出的是 JPEG（回包 magic 应是 FFD8FFE0／FFD8FFE1）。\n"
    "  ★★ layer 与 timeout_ms **只在回退路上生效**：主路是 capssc 的调试命令 1204，\n"
    "     它忽略 layer、且超时固定在 10 秒（这是实测的；别以为传了就一定按你给的来）。\n"
    "返回:\n"
    "  path= bytes= via= magic= layer= buffer_kb=。\n"
    "相关: screen.info screen.wait_change fs.read\n",
};
const nxc::MethodInfo kInfoWaitChange = {
    kWaitChangeParams, 4, "read",
    "等画面变化（事件驱动）：设备端连抓帧、逐字节比对，画面一变立刻回。\n"
    "作用: 操作后不用再盲等固定秒数 —— 把「要等多久」变成一条事实。\n"
    "参数:\n"
    "  timeout_ms  = 最长等待毫秒，默认 5000；越界（≤0 或 >60000）回落 5000。\n"
    "  interval_ms = 抓帧间隔毫秒，默认 200；会被钳到 30..2000。\n"
    "  layer       = 抓哪一层，默认 lcd。\n"
    "  max_kb      = 抓帧缓冲容量 KB，默认 512，允许 512..1536（capssc 硬性要 ≥512）。\n"
    "示例:\n"
    "  screen.wait_change\n"
    "  （先 input.seq steps=A:120，再 screen.wait_change 等画面真的变）\n"
    "注意:\n"
    "  ★ 主界面本身一直在动（图标动画等）⇒ 空闲时也会立刻回 changed=1。\n"
    "    想在空闲时判断「到下一屏没」请用 screen.wait_stable。\n"
    "  ★ JPEG 编码是确定性的 ⇒ 哈希不同即画面不同。\n"
    "返回:\n"
    "  changed=0|1 frames= elapsed_ms= interval_ms= baseline_hash= last_hash=\n"
    "  baseline_bytes= last_bytes= size_changed=。\n"
    "相关: screen.wait_stable screen.capture\n",
};
const nxc::MethodInfo kInfoWaitStable = {
    kWaitStableParams, 5, "read",
    "等画面稳定：连续 N 帧完全一样 ⇒ 画面停住了 ⇒ 回。\n"
    "作用: 判断「加载完了没／到下一屏了没」—— 补 wait_change 的短板。\n"
    "参数:\n"
    "  timeout_ms    = 最长等待毫秒，默认 10000；越界（≤0 或 >60000）回落 10000。\n"
    "  interval_ms   = 抓帧间隔毫秒，默认 200；会被钳到 30..2000。\n"
    "  stable_frames = 连续多少帧一样算稳定，默认 3；会被钳到 2..30。\n"
    "  layer         = 抓哪一层，默认 lcd。\n"
    "  max_kb        = 抓帧缓冲容量 KB，默认 512，允许 512..1536（capssc 硬性要 ≥512）。\n"
    "示例:\n"
    "  screen.wait_stable\n"
    "  screen.wait_stable stable_frames=5 timeout_ms=20000\n"
    "注意:\n"
    "  ★ 动画循环的界面（例如游戏内）永远不会稳定 —— 那种情况用 screen.wait_change。\n"
    "返回:\n"
    "  stable=0|1 frames= held_frames= stable_frames= elapsed_ms= interval_ms=\n"
    "  hash= bytes=。\n"
    "相关: screen.wait_change screen.capture\n",
};

}  // namespace

const nxc::Method kMethods[] = {
    {"info",    "看 capssc 是否可用 / 默认路径 / 可选 layer", cmdInfo,    &kInfoInfo},
    {"capture", "★ 抓一张画面：可选 path=（默认 /config/nxc/screen.jpg）layer= timeout_ms= max_kb=（默认512，只能调大；改不了输出大小）", cmdCapture, &kInfoCapture},
    {"wait_change", "★★ 等画面变化（事件驱动）：可选 timeout_ms=5000 interval_ms=200 —— 画面一变立刻回", cmdWaitChange, &kInfoWaitChange},
    {"wait_stable", "★★ 等画面稳定：可选 timeout_ms=10000 interval_ms=200 stable_frames=3 —— 连 N 帧一样就算稳定（判断「加载完了没」）", cmdWaitStable, &kInfoWaitStable},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_screen, "screen",
    "抓主机画面（JPEG 存到 SD 卡）。AI 从「闭眼」变「睁眼」", kMethods,
    "抓主机画面（JPEG 存到 SD 卡），以及等画面变化／稳定。\n"
    "什么时候用它：\n"
    "  · 想知道屏幕现在是什么样（AI 睁眼）\n"
    "  · 操作之后确认画面真的变了／加载完了\n"
    "工作流：screen.capture 抓一张 → fs.read 取回；或 input.seq 操作后 screen.wait_change。\n"
    "边界：\n"
    "  · 画面存 SD 卡，单条应答塞不下，得用 fs.read 分块取回\n"
    "  · max_kb 是抓帧缓冲容量而不是输出大小，capssc 硬性要 ≥512KB ⇒ 只能调大\n"
    "  · capssc 会话「进命令时开、出命令时关」，不跨命令／睡眠持有\n"
    "相关工具：fs（取回 jpg）、mem（读写内存）、input（模拟操作）\n"
);
