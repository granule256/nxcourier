// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 内置工具 · input：**给主机模拟按键 / 摇杆**（让 AI 从"能看"变成"能动"）。
//
// ## ★ 第一版用错通道了（实测教训，值得写下来）
//
// 最初我用的是 `hiddbgSetDebugPadAutoPilotState()`（DebugPad）。
// 调用全部返回成功，但**在游戏里完全没有反应** —— 因为
// **DebugPad 是系统里一个独立的输入设备**，游戏读的是正常的 `HidNpad`，根本看不到它。
// ⇒ **接口返回"成功"不等于输入被游戏收到。** 这条教训比代码值钱。
//
// ## 现在的做法：Hdls 虚拟手柄（照抄 sys-botbase，它实测能用）
//
// 调用顺序（顺序不能改）：
//     hiddbgInitialize()
//     workmem = aligned_alloc(0x1000, 0x1000)              // 工作缓冲，4KB
//     hiddbgAttachHdlsWorkBuffer(&sessionId, workmem, 0x1000)
//     hiddbgAttachHdlsVirtualDevice(&handle, &deviceInfo)
//     hiddbgSetHdlsState(handle, &state)                   // 之后每次改状态都调它
//
// deviceInfo：`deviceType = HidDeviceType_FullKey3`（当成 标准手柄）、
//             `npadInterfaceType = HidNpadInterfaceType_Bluetooth`。
// ★ 不需要额外的 npad 分配调用（sys-botbase 也没调）。
//
// ★ 摇杆取值范围：`HidAnalogStickState` 是 s32，**中心是 0**，满偏约 ±32767。
//   （注意这与 DebugPad 那套 0..255 / 128 中位**不是一回事**，别混。）
#include "nxc_sdk.hpp"

#include <switch.h>

#include <cstdlib>
#include <cstring>
#include <strings.h>   // strcasecmp（在全局命名空间，不在 std::）

namespace {

constexpr size_t kWorkMemSize = 0x1000;
constexpr s32 kStickMax = 32767;

// 虚拟手柄"一次性附加、长期持有"（和 sys-botbase 一样），所以状态放静态。
bool g_ready = false;
void* g_workmem = nullptr;
HiddbgHdlsSessionId g_session;
HiddbgHdlsHandle g_handle;
HiddbgHdlsState g_state;

// 附加虚拟手柄（幂等）。outStage 用来回报卡在哪一步 —— 只有一个返回值时定位不了问题。
Result ensurePad(const char** outStage) {
    *outStage = "already-ready";
    if (g_ready) return 0;

    Result rc = hiddbgInitialize();
    if (R_FAILED(rc)) { *outStage = "hiddbgInitialize"; return rc; }

    *outStage = "aligned_alloc";
    g_workmem = std::aligned_alloc(0x1000, kWorkMemSize);
    if (g_workmem == nullptr) { hiddbgExit(); return -1; }

    HiddbgHdlsDeviceInfo device;
    std::memset(&device, 0, sizeof(device));
    device.deviceType         = HidDeviceType_FullKey3;   // 当成 标准手柄
    device.npadInterfaceType  = HidNpadInterfaceType_Bluetooth;
    device.singleColorBody    = RGBA8_MAXALPHA(255, 255, 255);
    device.singleColorButtons = RGBA8_MAXALPHA(0, 0, 0);
    device.colorLeftGrip      = RGBA8_MAXALPHA(230, 255, 0);
    device.colorRightGrip     = RGBA8_MAXALPHA(0, 40, 20);

    *outStage = "AttachHdlsWorkBuffer";
    rc = hiddbgAttachHdlsWorkBuffer(&g_session, g_workmem, kWorkMemSize);
    if (R_FAILED(rc)) { std::free(g_workmem); g_workmem = nullptr; hiddbgExit(); return rc; }

    *outStage = "AttachHdlsVirtualDevice";
    rc = hiddbgAttachHdlsVirtualDevice(&g_handle, &device);
    if (R_FAILED(rc)) {
        hiddbgReleaseHdlsWorkBuffer(g_session);
        std::free(g_workmem); g_workmem = nullptr;
        hiddbgExit();
        return rc;
    }

    std::memset(&g_state, 0, sizeof(g_state));
    g_state.battery_level = 4;   // 满电
    hiddbgSetHdlsState(g_handle, &g_state);

    g_ready = true;
    *outStage = "ok";
    return 0;
}

// 键名 → 位。
bool buttonBit(const char* name, u64* out) {
    struct Entry { const char* name; u64 bit; };
    static const Entry kTable[] = {
        {"A", HidNpadButton_A},          {"B", HidNpadButton_B},
        {"X", HidNpadButton_X},          {"Y", HidNpadButton_Y},
        {"L", HidNpadButton_L},          {"R", HidNpadButton_R},
        {"ZL", HidNpadButton_ZL},        {"ZR", HidNpadButton_ZR},
        {"PLUS", HidNpadButton_Plus},    {"START", HidNpadButton_Plus},
        {"MINUS", HidNpadButton_Minus},  {"SELECT", HidNpadButton_Minus},
        {"DUP", HidNpadButton_Up},       {"UP", HidNpadButton_Up},
        {"DDOWN", HidNpadButton_Down},   {"DOWN", HidNpadButton_Down},
        {"DLEFT", HidNpadButton_Left},   {"LEFT", HidNpadButton_Left},
        {"DRIGHT", HidNpadButton_Right},{"RIGHT", HidNpadButton_Right},
        {"LSTICK", HidNpadButton_StickL}, {"RSTICK", HidNpadButton_StickR},
    };
    for (const Entry& e : kTable) {
        if (strcasecmp(name, e.name) == 0) { *out = e.bit; return true; }
    }
    // 系统键走另一套枚举（按 sys-botbase 的用法）
    if (strcasecmp(name, "HOME") == 0)    { *out = HiddbgNpadButton_Home; return true; }
    if (strcasecmp(name, "CAPTURE") == 0) { *out = HiddbgNpadButton_Capture; return true; }
    return false;
}

// 解析 "A,B,ZL" / "A+B" / "A B"。
bool parseButtons(const char* text, u64* outButtons, char* badName, size_t badCap) {
    if (text == nullptr || *text == '\0') { *outButtons = 0; return true; }

    char token[24];
    int n = 0;
    u64 bits = 0;
    for (const char* p = text;; ++p) {
        const char c = *p;
        const bool sep = (c == ',' || c == '+' || c == ' ' || c == '\0');
        if (sep) {
            if (n > 0) {
                token[n] = '\0';
                u64 bit = 0;
                if (!buttonBit(token, &bit)) { std::snprintf(badName, badCap, "%s", token); return false; }
                bits |= bit;
                n = 0;
            }
            if (c == '\0') break;
            continue;
        }
        if (n < static_cast<int>(sizeof(token)) - 1) token[n++] = c;
    }
    *outButtons = bits;
    return true;
}

bool applyState(u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry) {
    g_state.buttons = buttons;
    g_state.analog_stick_l.x = lx;
    g_state.analog_stick_l.y = ly;
    g_state.analog_stick_r.x = rx;
    g_state.analog_stick_r.y = ry;
    return R_SUCCEEDED(hiddbgSetHdlsState(g_handle, &g_state));
}

// ---------------------------------------------------------------- 方法
void cmdStatus(const nxc::Args&, nxc::Reply& r) {
    // ★★★ 2026-10-06 晚：这个方法**不再自动附加**虚拟手柄。
    //
    //   原来这里是 `if (!g_ready) rc = ensurePad(&stage);` —— 一个语义上"只读"的状态查询，
    //   **顺手把手柄挂上了**。后果是一次很误导的实测：`input.detach` 明明生效了
    //   （`cmdDetach` 真的 detach 并把 `g_ready` 置了 false），紧接着 `input.status`
    //   又把它挂回去 ⇒ 永远报 `virtual_pad=attached`，看起来像"detach 不生效"。
    //
    //   ★ 需要"用时自动挂载"的是 press / seq / release，它们各自会调 `ensurePad`，
    //     不依赖 status 去挂。
    r.kv("virtual_pad", g_ready ? "attached" : "not-attached");
    r.kv("stage", g_ready ? "attached" : "idle");
    r.raw("note: status 是只读的，不会自动附加；press / seq / release 会在需要时自动附加");
    r.kvi("stick_center", 0);
    r.kvi("stick_max", kStickMax);
    r.raw("buttons: A B X Y L R ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT LSTICK RSTICK HOME CAPTURE");
    r.raw("sticks: lx ly rx ry ，中心 0，满偏约 32767（与 DebugPad 的 0..255 不是一回事）");
    r.raw("note: press 会自动松开（按下→保持→松开）；input.detach 可拔掉虚拟手柄");
}

void cmdPress(const nxc::Args& a, nxc::Reply& r) {
    const char* stage = "";
    const Result rcInit = ensurePad(&stage);
    if (R_FAILED(rcInit)) {
        r.failf(503, "attach-failed stage=%s rc=0x%08X", stage, static_cast<unsigned>(rcInit));
        return;
    }

    u64 buttons = 0;
    char bad[24] = {0};
    const char* spec = a.get("buttons");
    if (spec == nullptr) spec = a.get("keys");
    if (!parseButtons(spec, &buttons, bad, sizeof(bad))) {
        r.failf(400, "unknown-button:%s", bad);
        return;
    }

    long long holdMs = a.getInt("hold_ms", 120);
    if (holdMs < 0 || holdMs > 10000) { r.fail(400, "hold_ms-out-of-range-0..10000"); return; }

    const s32 lx = static_cast<s32>(a.getInt("lx", 0));
    const s32 ly = static_cast<s32>(a.getInt("ly", 0));
    const s32 rx = static_cast<s32>(a.getInt("rx", 0));
    const s32 ry = static_cast<s32>(a.getInt("ry", 0));

    if (buttons == 0 && lx == 0 && ly == 0 && rx == 0 && ry == 0) {
        r.fail(400, "nothing-to-press");
        return;
    }

    nxc::host::breadcrumb("input.press buttons=0x%llX hold=%lldms",
                          static_cast<unsigned long long>(buttons), holdMs);

    if (!applyState(buttons, lx, ly, rx, ry)) {
        r.fail(500, "SetHdlsState-failed");
        return;
    }
    if (holdMs > 0) svcSleepThread(static_cast<u64>(holdMs) * 1000000ULL);

    // ★ 无论如何都回到"不按"的状态 —— 不留下按死的键。
    applyState(0, 0, 0, 0, 0);

    r.kvf("buttons", "0x%llX", buttons);
    r.kvi("hold_ms", holdMs);
    r.kvi("lx", lx); r.kvi("ly", ly); r.kvi("rx", rx); r.kvi("ry", ry);
    r.kv("result", "pressed-and-released");
}

void cmdRelease(const nxc::Args&, nxc::Reply& r) {
    const char* stage = "";
    if (R_FAILED(ensurePad(&stage))) { r.failf(503, "attach-failed stage=%s", stage); return; }
    nxc::host::breadcrumb("input.release");
    applyState(0, 0, 0, 0, 0);
    r.kv("result", "released");
}

void cmdDetach(const nxc::Args&, nxc::Reply& r) {
    if (!g_ready) { r.kv("result", "was-not-attached"); return; }
    nxc::host::breadcrumb("input.detach");
    hiddbgDetachHdlsVirtualDevice(g_handle);
    hiddbgReleaseHdlsWorkBuffer(g_session);
    hiddbgExit();
    std::free(g_workmem);
    g_workmem = nullptr;
    g_ready = false;
    r.kv("result", "detached");
}

// ★★ 一次请求执行一串动作 —— 走菜单不用再"一次按键一次往返"。
//
// 语法：`steps=名字:毫秒;名字:毫秒;...`，用 **分号** 分隔（因为按键名之间用逗号 `A,B`）。
//   * `WAIT:300`      → 单纯等 300ms
//   * `DRIGHT:150`    → 按右方向键 150ms 后松开
//   * `A+B:120`       → 同时按 A 和 B
//
// 为什么需要它：走一个菜单往往要按 5 次键，每次往返约 1.5 秒（PC 侧还要看结果）；
// 放在设备端连着跑，往返只剩一次。
//
// ★ 仍然遵守"按完必松开"：每一步都是 按下→保持→松开，不会留下按死的键。
void cmdSeq(const nxc::Args& a, nxc::Reply& r) {
    const char* stage = "";
    const Result rcInit = ensurePad(&stage);
    if (R_FAILED(rcInit)) {
        r.failf(503, "attach-failed stage=%s rc=0x%08X", stage, static_cast<unsigned>(rcInit));
        return;
    }

    const char* spec = a.get("steps");
    if (spec == nullptr || *spec == '\0') {
        // ★★★ 2026-10-06 晚：原来 `fail` 之后的 `raw("usage: …")` **发不出去**（ERR 不带载荷）
        //   ⇒ 折进错误消息（语法是必须告诉调用方的东西）。
        r.failf(400, "missing-steps: 需要 steps=<动作串>，形如 steps=DRIGHT:150;WAIT:300;A:120"
                     "（分号分隔；WAIT:n 为纯等待）");
        return;
    }

    long long defaultHold = a.getInt("default_hold_ms", 120);
    bool holdAdjusted = false;
    if (defaultHold < 0 || defaultHold > 5000) { defaultHold = 120; holdAdjusted = true; }

    nxc::host::breadcrumb("input.seq %s", spec);

    const u64 startTick = armGetSystemTick();
    int stepCount = 0;
    int failed = 0;

    const char* p = spec;
    while (*p != '\0') {
        // 取一段（到分号或结尾）
        char step[80];
        int n = 0;
        while (*p != '\0' && *p != ';') {
            if (n < static_cast<int>(sizeof(step)) - 1) step[n++] = *p;
            ++p;
        }
        step[n] = '\0';
        if (*p == ';') ++p;
        if (n == 0) continue;

        // 拆成 名字:毫秒
        char name[48];
        int m = 0;
        const char* q2 = step;
        while (*q2 != '\0' && *q2 != ':') {
            if (m < static_cast<int>(sizeof(name)) - 1) name[m++] = *q2;
            ++q2;
        }
        name[m] = '\0';
        long long ms = defaultHold;
        if (*q2 == ':') {
            ms = 0;
            for (const char* d = q2 + 1; *d >= '0' && *d <= '9'; ++d) ms = ms * 10 + (*d - '0');
        }
        if (ms < 0) ms = 0;
        if (ms > 10000) ms = 10000;

        ++stepCount;
        if (strcasecmp(name, "WAIT") == 0) {
            if (ms > 0) svcSleepThread(static_cast<u64>(ms) * 1000000ULL);
            continue;
        }

        u64 buttons = 0;
        char bad[24] = {0};
        if (!parseButtons(name, &buttons, bad, sizeof(bad))) {
            ++failed;
            nxc::host::log("input.seq: 不认识的键 %s", bad);
            continue;
        }
        if (buttons == 0) continue;

        if (!applyState(buttons, 0, 0, 0, 0)) { ++failed; continue; }
        if (ms > 0) svcSleepThread(static_cast<u64>(ms) * 1000000ULL);
        applyState(0, 0, 0, 0, 0);      // 每步都松开，不留按死的键
    }

    const long long elapsed =
        static_cast<long long>(armTicksToNs(armGetSystemTick() - startTick) / 1000000ULL);

    // ★★★ 2026-10-06 晚：**有步骤失败就回错误**，不再回 `OK steps=1 failed=1`。
    //   原来的形态要求调用方自己去读 `failed=`；实测 `steps=zzz` 回的是 OK ⇒
    //   "按键没执行"看起来像"执行了"。现在把计数折进错误消息，信息一点不少。
    if (failed > 0) {
        r.failf(400, "seq-had-failures steps=%lld failed=%lld elapsed_ms=%lld"
                     "（名字不认识的键会被跳过，可用 input.status 看可用键名）",
                static_cast<long long>(stepCount), static_cast<long long>(failed), elapsed);
        return;
    }

    r.kvi("steps", stepCount);
    r.kvi("failed", 0);
    r.kvi("elapsed_ms", elapsed);
    if (holdAdjusted) {   // ★ 与其他方法一致：越界不静默
        r.next();
        r.kv("note_params", "default_hold_ms 越界（允许 0..5000），已按默认 120 执行");
    }
    r.raw("note: 每步都是「按下→保持→松开」，不会留按死的键");
    r.raw("tip: 跑完接一次 screen.wait_change，就知道这串动作有没有生效");
}

// ---------------------------------------------------------------- 自描述元数据
//
// ★★ 规矩：**这个工具的全部知识都写在本文件里** —— 核心不认识 `input` 是什么，
//    `tools.list` / `tools.doc` 只是把这里的内容汇总出去。**AI 就是靠这些字学会用它。**
//    ⇒ 新增/改动方法时，把参数表与教程一起改（别只改实现）。
namespace {

const nxc::Param kPress[] = {
    {"buttons",  "str", false, nullptr, "要按的键：逗号/加号/空格分隔（如 A,B,ZL 或 A+B）；别名 keys=。留空则只推摇杆"},
    {"lx",       "int", false, "0",     "左摇杆 X，s32：中心 0、满偏约 ±32767"},
    {"ly",       "int", false, "0",     "左摇杆 Y，同 lx"},
    {"rx",       "int", false, "0",     "右摇杆 X，同 lx"},
    {"ry",       "int", false, "0",     "右摇杆 Y，同 lx"},
    {"hold_ms",  "int", false, "120",   "按住多少毫秒（0..10000），到点自动松开"},
};
const nxc::Param kSeq[] = {
    {"steps",           "str", true,  nullptr, "动作串：名字:毫秒;名字:毫秒…（分号分隔；WAIT:n 为纯等待）"},
    {"default_hold_ms", "int", false, "120",   "某步省了 :毫秒 时的默认按住时长（0..5000，越界回落 120）"},
};

const nxc::MethodInfo kInfoStatus = {
    nullptr, 0, "read",
    "看虚拟手柄当前是否已附加，并列出可用键名与摇杆取值约定。\n"
    "作用: 查这个「虚拟 标准手柄」挂着没有（attached / not-attached）。\n"
    "★ 刻意设计: status 是**只读**的 —— 它**不会**顺手把手柄挂上。以前会，导致刚 input.detach\n"
    "  完再查状态又变成 attached，看起来像 detach 没生效。真正会按需自动挂载的是 press / seq / release。\n"
    "参数: 无。\n"
    "示例: input.status\n"
    "返回: virtual_pad=attached|not-attached 与 stage=，另附可用键名、stick_center=0、stick_max=32767。\n"
    "相关: press（会自动挂载）、detach（拔手柄）。\n",
};
const nxc::MethodInfo kInfoPress = {
    kPress, 6, "action",
    "模拟一次按键 / 推一次摇杆（走 hiddbg 的 Hdls 虚拟手柄，主机把它当一个虚拟 标准手柄）。\n"
    "作用: 让 AI 真的「按一下」主机 —— 按下 → 保持 hold_ms → 自动松开，不会留下按死的键。\n"
    "★ 需要时**自动挂载**虚拟手柄（press / seq / release 都会；status 不会）。\n"
    "参数:\n"
    "  buttons  要按的键，逗号/加号/空格分隔，如 A,B,ZL 或 A+B；别名 keys=。\n"
    "           可用键: A B X Y L R ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT LSTICK RSTICK HOME CAPTURE\n"
    "           （UP/DOWN/LEFT/RIGHT/START/SELECT 是别名）。\n"
    "  lx ly rx ry  摇杆，s32：**中心 0、满偏约 ±32767**（不是 0..255 那套）。\n"
    "  hold_ms  按住时长（0..10000，默认 120）。\n"
    "★ buttons 与四个摇杆都为空 → 回 400 nothing-to-press（不做事就不回 OK）。\n"
    "示例: input.press buttons=A\n"
    "      input.press buttons=DRIGHT hold_ms=200\n"
    "      input.press lx=32767 ly=0\n"
    "返回: buttons=0x.. hold_ms= lx= ly= rx= ry= result=pressed-and-released\n"
    "注意: 属于 action（真的动主机输入）；按住 hold_ms 期间会阻塞这么久。\n"
    "相关: seq（一串动作一次往返）、release（清空）、detach（拔手柄）。\n",
};
const nxc::MethodInfo kInfoRelease = {
    nullptr, 0, "action",
    "把按钮与摇杆全部归零（虚拟手柄保持挂着）。\n"
    "作用: 清掉「按着」的状态 —— 比如上一步推着摇杆，想让角色停下。\n"
    "★ 会按需自动挂载；手柄本来没挂也会先挂上再清零。\n"
    "参数: 无。\n"
    "示例: input.release\n"
    "返回: result=released\n"
    "注意: 属于 action（真的动主机输入状态）。\n"
    "相关: press、seq、detach。\n",
};
const nxc::MethodInfo kInfoSeq = {
    kSeq, 2, "action",
    "一次请求执行一串按键 / 等待动作（省掉「一次按键一次往返」）。\n"
    "作用: 走菜单这类要连按多次的场景，在设备端一口气跑完，往返只剩一次。\n"
    "语法: steps=名字:毫秒;名字:毫秒;…  用**分号**分隔（因为键名之间本来就用逗号）。\n"
    "  · WAIT:300     纯等待 300ms\n"
    "  · DRIGHT:150   按右方向键 150ms 后松开\n"
    "  · A+B:120      同时按 A 和 B\n"
    "某步省掉 :毫秒 时用 default_hold_ms（默认 120，合法范围 0..5000，越界回落 120）。\n"
    "每步都是「按下 → 保持 → 松开」，不会留按死的键。\n"
    "★ 有步骤失败（名字不认识 或 SetHdlsState 失败）会回 **ERR**，并把 steps= / failed= / elapsed_ms= 折进错误消息。\n"
    "  以前是回 OK + failed=1，调用方不看 failed= 就会把「没按」当成「按了」。\n"
    "示例: input.seq steps=DRIGHT:150;WAIT:300;A:120\n"
    "返回: steps= failed=0 elapsed_ms=\n"
    "注意: 属于 action；单个动作的时长上限 10000ms。\n"
    "相关: press（单次）、status（看可用键名）。\n",
};
const nxc::MethodInfo kInfoDetach = {
    nullptr, 0, "action",
    "拔掉虚拟手柄并释放工作缓冲。\n"
    "作用: 彻底收摊 —— 主机不再看到这个虚拟 标准手柄（用完不想让它一直挂着时）。\n"
    "参数: 无。\n"
    "示例: input.detach\n"
    "返回: result=detached；本来就没挂则 result=was-not-attached。\n"
    "★ detach 之后 input.status 会如实报 not-attached（不会被偷偷挂回去）；\n"
    "  但下次 press / seq / release 又会自动把它挂上。\n"
    "注意: 属于 action。\n"
    "相关: status、release。\n",
};

}  // namespace

const nxc::Method kMethods[] = {
    {"status",  "看虚拟手柄是否已附加 / 可用按键名 / 摇杆取值", cmdStatus,  &kInfoStatus},
    {"press",   "★ 模拟按键/摇杆：buttons=A,B,ZL [lx=] [ly=] [rx=] [ry=] [hold_ms=120]", cmdPress, &kInfoPress},
    {"release", "清空按钮与摇杆（保持虚拟手柄）", cmdRelease, &kInfoRelease},
    {"seq",     "★★ 一次执行一串动作：steps=DRIGHT:150;WAIT:300;A:120 （分号分隔；WAIT:n 为纯等待）", cmdSeq, &kInfoSeq},
    {"detach",  "拔掉虚拟手柄并释放工作缓冲", cmdDetach, &kInfoDetach},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_input, "input",
                "给主机模拟按键 / 摇杆（虚拟 标准手柄）。按完自动松开，不会卡键",
                kMethods,
    "给主机模拟按键 / 摇杆 —— 让 AI 从「能看」变成「能动」。\n"
    "实现: 走 hiddbg 的 Hdls **虚拟手柄**，主机把它当成一个虚拟 标准手柄。\n"
    "  · press / seq / release 在需要时**自动挂载**\n"
    "  · status 是**只读**的、**不会**自动挂载（这是刻意修的：以前查状态会偷偷把手柄挂回去，\n"
    "    导致 detach 看起来不生效）\n"
    "  · detach 拔掉手柄\n"
    "摇杆量纲: lx/ly/rx/ry 是 s32，**中心 0、满偏约 ±32767**（不是 0..255 那套）。\n"
    "什么时候用它:\n"
    "  · 走菜单/确认框：input.seq 一串动作一次往返\n"
    "  · 单次按键或推摇杆：input.press\n"
    "  · 判有没有生效：跑完接一个 screen.wait_change\n"
    "边界: 会真的动主机输入（press/release/seq/detach 都是 action）；每步按完必松开，不留按死的键。\n"
    "相关工具: screen（看画面）、title（前台标题）"
);
