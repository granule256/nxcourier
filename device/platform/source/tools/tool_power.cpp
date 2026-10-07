// 内置工具 · power：电源（重启 / 关机 / 按键状态）。
//
// 两条实现路径（用 `via=` 选）：
//   * `bpc`  —— `bpcRebootSystem()` / `bpcShutdownSystem()`。M1 探针实测 `bpc` 可用。
//     （开工单曾断言"真正的 reboot 要 spsmShutdown，在 sysmodule 里做不到"，而 `bpc` 自带这两个。）
//   * `spsm` —— `spsmShutdown(bool reboot)`。★ 开工单说它"端口能开、但客户端一连就 reset"，
//     所以这条路径**同时是 spsm 的客户端级验证** —— 只测 init 不算数，必须真调一次。
//
// ★★ 安全设计：重启/关机是**不可撤销**的动作，所以必须显式带上确认口令，
//    而且口令写死、必须逐字符匹配 —— 防止模型或脚本"顺手"调用。
#include "nxc_sdk.hpp"

#include <switch.h>

#include <cstring>

namespace {

constexpr const char* kRebootToken   = "I-KNOW-THIS-REBOOTS";
constexpr const char* kShutdownToken = "I-KNOW-THIS-SHUTS-DOWN";

bool ensureBpc() {
    static bool ready = false;
    if (ready) return true;
    if (R_FAILED(bpcInitialize())) return false;
    ready = true;
    return true;
}

bool ensureSpsm() {
    static bool ready = false;
    if (ready) return true;
    if (R_FAILED(spsmInitialize())) return false;
    ready = true;
    return true;
}

// ---------------------------------------------------------------- 方法
void cmdStatus(const nxc::Args&, nxc::Reply& r) {
    if (!ensureBpc()) { r.fail(503, "bpc-unavailable"); return; }

    bool pushed = false;
    if (R_SUCCEEDED(bpcGetPowerButton(&pushed))) r.kvi("power_button", pushed ? 1 : 0);
    else r.kv("power_button", "unavailable");

    BpcSleepButtonState sleep = BpcSleepButtonState_Released;
    if (R_SUCCEEDED(bpcGetSleepButtonState(&sleep))) r.kvi("sleep_button", static_cast<int>(sleep));
    else r.kv("sleep_button", "unavailable");

    r.raw("note: 这两个是只读状态；重启/关机要用 power.reboot / power.shutdown 且必须带确认口令");
}

// reboot=true 重启，false 关机。via 可选 bpc（默认）或 spsm。
void doPower(bool reboot, const nxc::Args& a, nxc::Reply& r) {
    const char* token = a.get("confirm");
    const char* expect = reboot ? kRebootToken : kShutdownToken;
    if (token == nullptr || std::strcmp(token, expect) != 0) {
        // ★★★ 2026-10-06 晚：这几句原来是 `r.fail(...)` + `r.raw("usage: …")` ——
        //   **ERR 不带载荷 ⇒ 那行提示根本发不出去**。调用方（尤其是 AI）只能拿到一句
        //   `refusing-without-confirm-token`，不知道**该带哪个口令**，只能去翻文档。
        //   现在把用法折进错误消息里（`failf`）。
        r.failf(403, "refusing-without-confirm-token: 要%s请带 confirm=%s，可选 via=bpc|spsm",
                reboot ? "重启" : "关机", expect);
        return;
    }

    const char* via = a.get("via");
    if (via == nullptr || *via == '\0') via = "bpc";

    // 先写面包屑：重启之后我们能在 SD 上看到"是谁按的、走的哪条路"。
    nxc::host::breadcrumb("power: %s requested via %s", reboot ? "REBOOT" : "SHUTDOWN", via);
    nxc::host::log("power.%s via=%s", reboot ? "reboot" : "shutdown", via);

    Result rc;
    if (std::strcmp(via, "bpc") == 0) {
        if (!ensureBpc()) { r.fail(503, "bpc-unavailable"); return; }
        rc = reboot ? bpcRebootSystem() : bpcShutdownSystem();
    } else if (std::strcmp(via, "spsm") == 0) {
        if (!ensureSpsm()) { r.fail(503, "spsm-unavailable"); return; }
        rc = spsmShutdown(reboot);  // ★ 这就是 spsm 的客户端级调用
    } else {
        r.fail(400, "via-must-be-bpc-or-spsm");
        return;
    }

    // ★★★ 2026-10-06 晚：走到了这里说明**请求没有被接受**（正常的话机器已经在重启/关机）。
    //   原来回的是 `OK rc=0x…`，把"没接受"写在了 OK 里 —— 改成失败时回 ERR。
    if (R_FAILED(rc)) {
        r.failf(500, "power-request-refused via=%s rc=0x%08X", via, static_cast<unsigned>(rc));
        return;
    }
    r.kv("via", via);
    r.kvf("rc", "0x%08X", static_cast<unsigned>(rc));
    r.kv("result", "accepted-but-still-alive");
    r.raw("note: 还收到这条响应 = 请求没被执行（正常的话机器已经在重启/关机了）");
}

void cmdReboot(const nxc::Args& a, nxc::Reply& r)   { doPower(true, a, r); }
void cmdShutdown(const nxc::Args& a, nxc::Reply& r) { doPower(false, a, r); }

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
//
// ★ 归属原则：本工具的全部知识都写在本文件里；`tools.list` / `tools.doc` 只负责汇总。
//   **AI 就是靠这些字学会用它** —— 改方法时把参数表与教程一起改，别只改实现。
const nxc::Param kReboot[] = {
    {"confirm", "str", true,  nullptr, "确认口令：必须逐字符等于 I-KNOW-THIS-REBOOTS"},
    {"via",     "str", false, "bpc",   "bpc（默认）| spsm"},
};
const nxc::Param kShutdown[] = {
    {"confirm", "str", true,  nullptr, "确认口令：必须逐字符等于 I-KNOW-THIS-SHUTS-DOWN"},
    {"via",     "str", false, "bpc",   "bpc（默认）| spsm"},
};

const nxc::MethodInfo kInfoStatus = {
    nullptr, 0, "read",
    "读电源键 / 睡眠键的状态。\n"
    "作用: 只读地看一眼机器的按键状态。\n"
    "参数: 无。\n"
    "示例: power.status\n"
    "注意: sleep_button 在这个固件上取不到，会回 unavailable（不是错误）。\n"
    "返回: power_button=0|1|unavailable sleep_button=<值>|unavailable。\n"
    "相关: power.reboot、power.shutdown（都要口令）",
};
const nxc::MethodInfo kInfoReboot = {
    kReboot, 2, "action",
    "重启主机。★ 不可撤销，必须带确认口令。\n"
    "作用: 重启。在 Hekate 上落点是引导器菜单（配合 boot.set 决定进哪个系统）。\n"
    "参数: confirm 必填，必须逐字符等于 I-KNOW-THIS-REBOOTS（缺或错回 403，消息里会告诉你口令）。\n"
    "      via 默认 bpc（bpcRebootSystem）；可选 spsm（spsmShutdown(true)）。\n"
    "示例: power.reboot confirm=I-KNOW-THIS-REBOOTS\n"
    "      power.reboot confirm=I-KNOW-THIS-REBOOTS via=spsm\n"
    "★★ 注意: 请求超时是**正常**的——机器会在回包之前就重启。PC 侧别把 timeout 当失败。\n"
    "注意: 若还收到响应且 result=accepted-but-still-alive，说明请求没被执行（正常应该已断连）。\n"
    "注意: via=spsm 同时也是 spsm 客户端可用性的验证（只 init 不算数，要真调一次）。\n"
    "返回: 通常收不到（机器已重启）；收得到则 via= rc= result=accepted-but-still-alive。\n"
    "相关: boot.set（设重启后进哪个系统）、power.shutdown",
};
const nxc::MethodInfo kInfoShutdown = {
    kShutdown, 2, "action",
    "关机。★ 不可撤销，必须带确认口令。\n"
    "作用: 关闭主机（不是睡眠）。\n"
    "参数: confirm 必填，必须逐字符等于 I-KNOW-THIS-SHUTS-DOWN（缺或错回 403，消息里会告诉你口令）。\n"
    "      via 默认 bpc（bpcShutdownSystem）；可选 spsm（spsmShutdown(false)）。\n"
    "示例: power.shutdown confirm=I-KNOW-THIS-SHUTS-DOWN\n"
    "      power.shutdown confirm=I-KNOW-THIS-SHUTS-DOWN via=spsm\n"
    "★★ 注意: 请求超时是**正常**的——机器会在回包之前就关机。PC 侧别把 timeout 当失败。\n"
    "注意: 若还收到响应且 result=accepted-but-still-alive，说明请求没被执行。\n"
    "返回: 通常收不到（机器已关机）；收得到则 via= rc= result=accepted-but-still-alive。\n"
    "相关: power.reboot、power.status",
};

const nxc::Method kMethods[] = {
    {"status",   "读电源键 / 睡眠键状态（只读）", cmdStatus, &kInfoStatus},
    {"reboot",   "重启主机 —— 必须带 confirm=I-KNOW-THIS-REBOOTS [via=bpc|spsm]", cmdReboot, &kInfoReboot},
    {"shutdown", "关机 —— 必须带 confirm=I-KNOW-THIS-SHUTS-DOWN [via=bpc|spsm]", cmdShutdown, &kInfoShutdown},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_power, "power",
    "电源（重启/关机需确认口令）", kMethods,
    "电源控制：读按键状态、重启、关机。\n"
    "什么时候用它：\n"
    "  · power.status 只读地看电源键 / 睡眠键状态\n"
    "  · power.reboot / power.shutdown 是**不可撤销**的动作 必须带确认口令\n"
    "★ 口令：reboot 要 I-KNOW-THIS-REBOOTS；shutdown 要 I-KNOW-THIS-SHUTS-DOWN。\n"
    "★ 缺口令或口令错会回 403 且错误消息里会直接告诉你该带哪个。\n"
    "★ via 默认 bpc；也可选 spsm。\n"
    "★ 请求超时是正常的——机器在回包之前就重启/关机了。\n"
    "★ power.status 的 sleep_button 在这个固件上取不到。\n"
    "相关工具：boot（重启后进哪个系统）\n"
);
