// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 内置工具 · boot：**选择下次开机进哪个系统**。
//
// ## 为什么需要它
//
// 在装了引导器（Hekate）的 CFW 上，`power.reboot` 的落点是**引导器菜单**，不是回到系统。
// 本机实测的 `/bootloader/hekate_ipl.ini`：
//     [config]
//     autoboot=0        ← 0 = 不自动启动 ⇒ 每次都停在菜单，要手动选一次
//     bootwait=1
//     autoboot_list=0
//     [虚拟系统] id=Atm-Emu   ← 第 1 条
//     [真实系统] id=Atm-Sys   ← 第 2 条
//     [正版系统] id=OFW-SYS   ← 第 3 条
//
// ⇒ **"重启后直接进虚拟系统"就是把这几个键改对**（`autoboot=1` 指第 1 条 + `bootwait` 留几秒）。
//
// ## 安全设计
//
// * **改之前自动备份**成 `hekate_ipl.ini.bak`。
// * **只动 `[config]` 段里的 autoboot / bootwait 两个键**，段落定义
//   （各系统的 pkg3/secmon/...）一个字都不碰 —— 即使写坏，引导器仍能进菜单手动选，
//   **不会真的开不了机**。
// * 写入留 `boot.set ...` 面包屑；应答里把"写了什么"原样回报，便于核对。
// * ★ 键名与语义按 **Hekate 的文档**（autoboot=0 关闭 / 1..N 指第 N 条；
//   bootwait=自动启动前等待秒数）。**本机未实测**（要重启才知道）。
#include "nxc_sdk.hpp"
#include "../core/sd.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kIniPath    = "/bootloader/hekate_ipl.ini";
constexpr const char* kBackupPath = "/bootloader/hekate_ipl.ini.bak";
constexpr int kIniMax    = 8192;
constexpr int kMaxEntries = 32;

char g_ini[kIniMax + 1];
char g_work[kIniMax + 1];

bool readIni(int* outLen) {
    FILE* f = nxc::sd::open(kIniPath, "rb");
    if (f == nullptr) return false;
    const size_t n = std::fread(g_ini, 1, kIniMax, f);
    std::fclose(f);
    g_ini[n] = '\0';
    if (outLen != nullptr) *outLen = static_cast<int>(n);
    return true;
}

// 备份（没有 rename 原语，就读出来再写过去）。
bool backupIni() {
    FILE* in = nxc::sd::open(kIniPath, "rb");
    if (in == nullptr) return false;
    FILE* out = nxc::sd::open(kBackupPath, "wb");
    if (out == nullptr) { std::fclose(in); return false; }

    unsigned char buf[4096];
    size_t n = 0;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    std::fclose(in);
    std::fclose(out);
    return ok;
}

// 这一行是不是 "<key>=" 开头（允许缩进）。
bool lineHasKey(const char* line, int len, const char* key) {
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    const int k = static_cast<int>(std::strlen(key));
    if (i + k >= len) return false;
    if (std::strncmp(line + i, key, static_cast<size_t>(k)) != 0) return false;
    return line[i + k] == '=';
}

bool isSectionHeader(const char* line, int len) {
    return len > 2 && line[0] == '[' && line[len - 1] == ']';
}

bool isConfigHeader(const char* line, int len) {
    return isSectionHeader(line, len) && len == 8 && std::strncmp(line, "[config]", 8) == 0;
}

// 读 [config] 段里某个键的值。
bool getConfig(const char* key, char* out, size_t cap) {
    bool inConfig = false;
    const char* p = g_ini;
    while (*p != '\0') {
        const char* eol = std::strchr(p, '\n');
        const int len = eol ? static_cast<int>(eol - p) : static_cast<int>(std::strlen(p));

        if (isSectionHeader(p, len)) {
            inConfig = isConfigHeader(p, len);
        } else if (inConfig && lineHasKey(p, len, key)) {
            const char* eq = std::strchr(p, '=');
            if (eq != nullptr) {
                int n = 0;
                for (const char* q = eq + 1; *q != '\0' && *q != '\n' && *q != '\r' &&
                                           static_cast<size_t>(n) + 1 < cap; ++q) {
                    out[n++] = *q;
                }
                out[n] = '\0';
                return true;
            }
        }
        if (eol == nullptr) break;
        p = eol + 1;
    }
    return false;
}

// 是否存在该键（只看 [config] 段）。
bool keyExistsInConfig(const char* key) {
    char scratch[8];
    return getConfig(key, scratch, sizeof(scratch));
}

// 把 [config] 段里的 key 改成 value；不存在就紧跟在 [config] 行之后插入。
// ★ 原地改 g_ini（用 g_work 当草稿），所以可以连续调用改多个键。
void setConfig(const char* key, const char* value, bool* found) {
    const bool exists = keyExistsInConfig(key);
    *found = exists;

    int o = 0;
    bool inConfig = false;
    bool replaced = false;
    bool inserted = false;

    const char* p = g_ini;
    while (*p != '\0') {
        const char* eol = std::strchr(p, '\n');
        const int len = eol ? static_cast<int>(eol - p) : static_cast<int>(std::strlen(p));
        const bool hasNewline = (eol != nullptr);

        if (isSectionHeader(p, len)) {
            inConfig = isConfigHeader(p, len);
        }

        if (exists && inConfig && !replaced && !isSectionHeader(p, len) &&
            lineHasKey(p, len, key)) {
            o += std::snprintf(g_work + o, static_cast<size_t>(kIniMax - o), "%s=%s", key, value);
            if (hasNewline) g_work[o++] = '\n';
            replaced = true;
        } else {
            if (o + len + 2 < kIniMax) {
                std::memcpy(g_work + o, p, static_cast<size_t>(len));
                o += len;
                if (hasNewline) g_work[o++] = '\n';
            }
            // 键不存在：紧跟在 [config] 那一行之后补一行。
            if (!exists && !inserted && isConfigHeader(p, len)) {
                o += std::snprintf(g_work + o, static_cast<size_t>(kIniMax - o), "%s=%s\n", key, value);
                inserted = true;
            }
        }

        if (!hasNewline) break;
        p = eol + 1;
    }
    g_work[o] = '\0';
    std::memcpy(g_ini, g_work, static_cast<size_t>(o) + 1);
}

// ---------------------------------------------------------------- 方法
void cmdList(const nxc::Args&, nxc::Reply& r) {
    if (!readIni(nullptr)) { r.fail(404, "cannot-read-hekate_ipl.ini"); return; }

    char autoBoot[16] = "0";
    getConfig("autoboot", autoBoot, sizeof(autoBoot));
    const long long current = std::strtoll(autoBoot, nullptr, 10);

    int index = 0;
    const char* p = g_ini;
    while (*p != '\0' && index < kMaxEntries) {
        const char* eol = std::strchr(p, '\n');
        const int len = eol ? static_cast<int>(eol - p) : static_cast<int>(std::strlen(p));

        if (isSectionHeader(p, len) && !isConfigHeader(p, len)) {
            ++index;
            char name[96];
            int n = 0;
            for (int i = 1; i < len - 1 && n < static_cast<int>(sizeof(name)) - 1; ++i) {
                name[n++] = p[i];
            }
            name[n] = '\0';

            r.next();
            r.kvi("index", index);
            r.kv("name", name);
            r.kvi("selected", (current == index) ? 1 : 0);
        }
        if (eol == nullptr) break;
        p = eol + 1;
    }

    r.next();
    r.kvi("count", index);
    r.kvi("autoboot", current);
    r.raw("hint: boot.set target=<index 或名字> 让下次开机直接进那个系统");
}

void cmdStatus(const nxc::Args&, nxc::Reply& r) {
    int len = 0;
    if (!readIni(&len)) { r.fail(404, "cannot-read-hekate_ipl.ini"); return; }

    char v[32];
    if (getConfig("autoboot", v, sizeof(v))) r.kv("autoboot", v);
    if (getConfig("bootwait", v, sizeof(v))) r.kv("bootwait", v);
    if (getConfig("autoboot_list", v, sizeof(v))) r.kv("autoboot_list", v);
    r.kvi("ini_bytes", len);
    r.kv("ini_path", kIniPath);
    r.raw("note: autoboot=0 表示不自动启动（每次停在引导器菜单）；=N 表示自动启动第 N 条");
    r.raw("note: bootwait 是自动启动前的等待秒数，留几秒就能在菜单里按 VOL- 取消");
}

void cmdSet(const nxc::Args& a, nxc::Reply& r) {
    const char* token = a.get("confirm");
    if (token == nullptr || std::strcmp(token, "I-KNOW-THIS-CHANGES-BOOT") != 0) {
        // ★★★ 2026-10-06 晚：原来 `fail` 之后的 `raw("usage: …")` **发不出去**
        //   （ERR 不带载荷）⇒ 调用方只知道"被拒了"，不知道**该带哪个口令**。折进消息里。
        r.failf(403, "refusing-without-confirm-token: 需要 confirm=I-KNOW-THIS-CHANGES-BOOT；"
                     "用法 boot.set target=<index 或名字> [wait=<秒>]"
                     "（会自动备份成 hekate_ipl.ini.bak，且只改 [config] 段的两个键）");
        return;
    }

    if (!readIni(nullptr)) { r.fail(404, "cannot-read-hekate_ipl.ini"); return; }

    // ---- target：先当序号，不行再当条目名
    const char* target = a.get("target");
    if (target == nullptr) target = a.get("_0");
    if (target == nullptr) { r.fail(400, "missing-target"); return; }

    long long index = std::strtoll(target, nullptr, 10);
    if (index <= 0) {
        int i = 0;
        const char* p = g_ini;
        while (*p != '\0' && i < kMaxEntries) {
            const char* eol = std::strchr(p, '\n');
            const int len = eol ? static_cast<int>(eol - p) : static_cast<int>(std::strlen(p));
            if (isSectionHeader(p, len) && !isConfigHeader(p, len)) {
                ++i;
                const int nameLen = len - 2;
                if (static_cast<int>(std::strlen(target)) == nameLen &&
                    std::strncmp(p + 1, target, static_cast<size_t>(nameLen)) == 0) {
                    index = i;
                    break;
                }
            }
            if (eol == nullptr) break;
            p = eol + 1;
        }
    }
    if (index <= 0) { r.fail(404, "no-such-entry"); return; }

    long long wait = a.getInt("wait", 3);
    if (wait < 0) wait = 0;
    if (wait > 30) wait = 30;

    const bool backedUp = backupIni();

    nxc::host::breadcrumb("boot.set autoboot=%lld bootwait=%lld backup=%d",
                          index, wait, backedUp ? 1 : 0);

    char value[32];
    bool foundAuto = false;
    bool foundWait = false;

    std::snprintf(value, sizeof(value), "%lld", index);
    setConfig("autoboot", value, &foundAuto);

    std::snprintf(value, sizeof(value), "%lld", wait);
    setConfig("bootwait", value, &foundWait);

    FILE* f = nxc::sd::open(kIniPath, "wb");
    if (f == nullptr) { r.fail(500, "cannot-write-ini"); return; }
    const size_t written = std::fwrite(g_ini, 1, std::strlen(g_ini), f);
    std::fclose(f);

    r.kvi("autoboot", index);
    r.kvi("bootwait", wait);
    r.kvi("autoboot_key_existed", foundAuto ? 1 : 0);
    r.kvi("bootwait_key_existed", foundWait ? 1 : 0);
    r.kvi("bytes_written", static_cast<long long>(written));
    r.kvi("backup_ok", backedUp ? 1 : 0);
    r.kv("backup_path", kBackupPath);
    r.raw("note: 下次开机引导器会自动启动该条目；bootwait 秒内可在菜单按 VOL- 取消");
    r.raw("note: 想恢复成「每次停菜单」，把 autoboot 设回 0（boot.set 暂不支持 0，可直接 fs.write 改）");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
//
// ★ 归属原则：本工具的全部知识都写在本文件里；`tools.list` / `tools.doc` 只负责汇总。
//   **AI 就是靠这些字学会用它** —— 改方法时把参数表与教程一起改，别只改实现。
const nxc::Param kSet[] = {
    {"confirm", "str", true,  nullptr, "确认口令：必须逐字符等于 I-KNOW-THIS-CHANGES-BOOT"},
    {"target",  "str", true,  nullptr, "启动条目：序号（从 1 数）或 [名字] 里的字（如 虚拟系统）"},
    {"wait",    "int", false, "3",     "自动启动前等待秒数（0..30；越界夹到边界）"},
};

const nxc::MethodInfo kInfoList = {
    nullptr, 0, "read",
    "列出引导器（Hekate）的启动条目，并标出当前 autoboot 指向哪条。\n"
    "作用: 知道开机菜单里有哪几项、现在默认进哪项。\n"
    "参数: 无。\n"
    "示例: boot.list\n"
    "注意: 读的是 /bootloader/hekate_ipl.ini，只解析 [config] 之外的 [名字] 段落。\n"
    "返回: 每行 index= name= selected=0|1；末行 count= autoboot=，并带一行 hint。\n"
    "相关: boot.status、boot.set",
};
const nxc::MethodInfo kInfoStatus = {
    nullptr, 0, "read",
    "读引导器当前的关键配置：autoboot / bootwait / autoboot_list。\n"
    "作用: 确认「下次开机到底进哪个系统、等几秒」。\n"
    "参数: 无。\n"
    "示例: boot.status\n"
    "注意: autoboot=0 表示不自动启动（每次停在菜单）；=N 表示自动启动第 N 条。\n"
    "注意: bootwait 是自动启动前的等待秒数，留几秒就能在菜单按 VOL- 取消。\n"
    "返回: autoboot= bootwait= autoboot_list= ini_bytes= ini_path=。\n"
    "相关: boot.list、boot.set",
};
const nxc::MethodInfo kInfoSet = {
    kSet, 3, "action",
    "设「下次开机自动进哪个系统」。★ 会写 SD 卡且留下**永久副作用**。\n"
    "作用: 解决「在 Hekate 上重启会停在菜单、需要人工选一项」——把它改成自动进指定系统。\n"
    "参数: confirm 必填，必须逐字符等于 I-KNOW-THIS-CHANGES-BOOT（缺或错回 403，消息里会告诉你口令）。\n"
    "      target 必填：序号（从 1 数）或条目名（方括号里的字，如 虚拟系统）。\n"
    "      wait 默认 3 秒（0..30，越界夹到边界）。\n"
    "示例: boot.set target=虚拟系统 wait=3 confirm=I-KNOW-THIS-CHANGES-BOOT\n"
    "      boot.set target=1 confirm=I-KNOW-THIS-CHANGES-BOOT\n"
    "★★ 注意: 动的是 /bootloader/hekate_ipl.ini（改前自动备份成 hekate_ipl.ini.bak）。\n"
    "★★ 注意: 这是**永久副作用**——改完以后每次开机落点都变了，直到你改回去。\n"
    "★★ 注意: 本机 reboot_payload.bin 就是 Hekate 本体。autoboot=0 时重启会停在 Hekate 菜单、需要人工选一项。\n"
    "注意: 只改 [config] 段的 autoboot / bootwait 两个键，段落定义一个字都不碰（写坏也能进菜单手动选）。\n"
    "注意: 想恢复「每次停菜单」= 把 autoboot 设回 0；boot.set 暂不支持 0，可直接用 fs.write 改。\n"
    "返回: autoboot= bootwait= bytes_written= backup_ok= backup_path= 等。\n"
    "相关: boot.list（先看有哪些条目）、boot.status、fs（直接改 ini）",
};

const nxc::Method kMethods[] = {
    {"list",   "列出启动条目（[config] 之外的 [名字]），并标出当前 autoboot 指向哪条", cmdList, &kInfoList},
    {"status", "读引导器当前的 autoboot / bootwait / autoboot_list", cmdStatus, &kInfoStatus},
    {"set",    "★ 设下次开机自动进的系统：target=<index 或名字> [wait=秒] confirm=I-KNOW-THIS-CHANGES-BOOT", cmdSet, &kInfoSet},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_boot, "boot",
    "引导器启动项：看/设下次开机进哪个系统（解决「重启落到菜单」）", kMethods,
    "管引导器（Hekate）的启动项：看有哪些条目、设下次开机自动进哪个。\n"
    "什么时候用它：\n"
    "  · 重启总是停在引导器菜单 → boot.set 指一个条目（如「虚拟系统」）让下次直接进\n"
    "  · 改之前先用 boot.list / boot.status 看清现状\n"
    "★ 动的是 /bootloader/hekate_ipl.ini（改前自动备份 .bak 且只改 [config] 的 autoboot / bootwait 两个键）。\n"
    "★ 本机 reboot_payload.bin 就是 Hekate 本体 ⇒ autoboot=0 时重启会停在 Hekate 菜单、需要人工选一项。\n"
    "★ boot.set 会留下永久副作用（影响以后每次开机落点）—— 用之前要想清楚。\n"
    "★ 口令：boot.set 要 I-KNOW-THIS-CHANGES-BOOT（缺或错回 403）。\n"
    "相关工具：power（重启/关机）、fs（直接读写 bootloader 配置）\n"
);
