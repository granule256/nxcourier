// 内置工具 · title：主机上"有什么、在跑什么"。
//
// 依据：M1 探针实测 `ns` / `ncm` / `pmdmnt` / `pminfo` / `pmshell` 在 sysmodule 里**全部可用**
//（`pmshell` 就是"启动/结束 title"的前提）。
//
// 本轮只做**只读**部分：
//   title.list    已安装应用（application_id + 最后更新时间）
//   title.current 当前在跑什么（应用进程 id → program id）
//   title.name    读 NACP 拿应用名/作者/版本
//
// 写入类（`title.launch` / `title.terminate`，前提是 `pmshellLaunchProgram`）留到下一轮 ——
// 它们会改变主机状态，必须单独设计确认流程与验证时机。
#include "nxc_sdk.hpp"

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// NACP 缓冲较大（NacpStruct 0x4000 + 图标 0x20000），**按需 malloc、用完即还**，
// 不放进 BSS —— sysmodule 的静态内存要省着用。
constexpr size_t kControlDataBytes = 0x24000;

constexpr int kMaxRecords = 200;

// 解析 "0x0100..." 或十进制；失败返回 false。
//
// ★ 2026-10-06 晚：算法已上收到 SDK 的 `nxc::util::parseU64` —— 因为 `save` 那边也要
//   "认 0x"（它原来用 `getInt`，把 `0x…` 读成 0）。**同一套逻辑留两份，迟早漂移**，
//   所以这里只保留一个薄包装，调用点一个都不用改。
bool parseId(const char* text, u64* out) {
    return nxc::util::parseU64(text, out);
}

// ---------------------------------------------------------------- 方法
void cmdList(const nxc::Args& a, nxc::Reply& r) {
    if (R_FAILED(nsInitialize())) { r.fail(503, "ns-unavailable"); return; }

    long long count = a.getInt("count", 60);
    // ★★★ 2026-10-06 晚：越界值仍回落默认 60（`count=0 表示用默认`是常见写法），
    //   但**不再静默** —— 见函数尾部的 note_params。
    bool countAdjusted = false;
    if (count <= 0 || count > kMaxRecords) { count = 60; countAdjusted = true; }
    long long offset = a.getInt("offset", 0);
    if (offset < 0) offset = 0;

    static NsApplicationRecord records[kMaxRecords];
    s32 produced = 0;
    const Result rc = nsListApplicationRecord(records,
                                              static_cast<s32>(count),
                                              static_cast<s32>(offset),
                                              &produced);
    nsExit();

    if (R_FAILED(rc)) {
        r.kvf("rc", "0x%08X", static_cast<unsigned>(rc));
        r.fail(500, "ns-list-failed");
        return;
    }

    for (s32 i = 0; i < produced; ++i) {
        r.next();
        r.kvf("app", "0x%016llX", static_cast<unsigned long long>(records[i].application_id));
        r.kvi("last_updated", static_cast<long long>(records[i].last_updated));
    }
    r.next();
    r.kvi("count", produced);
    r.kvi("offset", offset);
    r.raw("hint: 用 title.name app=<application_id> 拿应用名");
    if (countAdjusted) {
        r.next();   // ★ 先收尾上一行（raw 行末尾没有换行），否则提示会拼到那句 hint 后面
        r.kv("note_params", "count 越界（允许 1..200），已按默认 60 执行");
    }
}

void cmdCurrent(const nxc::Args&, nxc::Reply& r) {
    if (R_FAILED(pmdmntInitialize())) { r.fail(503, "pmdmnt-unavailable"); return; }

    u64 pid = 0;
    Result rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc) || pid == 0) {
        // 没有应用在跑（例如停在主界面或相册）——这是正常状态，不是错误。
        r.kv("running", "none");
        r.kvf("rc", "0x%08X", static_cast<unsigned>(rc));
        pmdmntExit();
        return;
    }

    u64 programId = 0;
    const Result rc2 = pmdmntGetProgramId(&programId, pid);
    pmdmntExit();

    r.kv("running", "application");
    r.kvi("pid", static_cast<long long>(pid));
    if (R_SUCCEEDED(rc2)) {
        r.kvf("program_id", "0x%016llX", static_cast<unsigned long long>(programId));
    } else {
        r.kvf("program_id_rc", "0x%08X", static_cast<unsigned>(rc2));
    }
}

void cmdName(const nxc::Args& a, nxc::Reply& r) {
    const char* text = a.get("app");
    if (text == nullptr) text = a.get("_0");

    u64 appId = 0;
    if (!parseId(text, &appId)) { r.fail(400, "app-must-be-a-title-id"); return; }

    if (R_FAILED(nsInitialize())) { r.fail(503, "ns-unavailable"); return; }

    auto* data = static_cast<NsApplicationControlData*>(std::malloc(kControlDataBytes));
    if (data == nullptr) { nsExit(); r.fail(500, "out-of-memory"); return; }

    u64 actual = 0;
    const Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, appId,
                                                  data, kControlDataBytes, &actual);
    if (R_SUCCEEDED(rc)) {
        // NsApplicationControlData 里的固定数组不保证以 '\0' 结尾，先拷进有界的本地缓冲。
        char version[17];
        std::memcpy(version, data->nacp.display_version, 16);
        version[16] = '\0';

        NacpLanguageEntry* entry = nullptr;
        if (R_SUCCEEDED(nsGetApplicationDesiredLanguage(&data->nacp, &entry)) && entry != nullptr) {
            char name[0x201];
            std::memcpy(name, entry->name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            char author[0x101];
            std::memcpy(author, entry->author, sizeof(author) - 1);
            author[sizeof(author) - 1] = '\0';
            r.kv("name", name);
            r.kv("author", author);
        } else {
            r.kv("name", "unavailable");
        }
        r.kv("version", version);
    } else {
        r.kvf("rc", "0x%08X", static_cast<unsigned>(rc));
    }

    std::free(data);
    nsExit();

    if (R_FAILED(rc)) { r.fail(404, "control-data-unavailable"); return; }
    r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
    r.kvi("control_data_bytes", static_cast<long long>(actual));
}

// ---------------------------------------------------------------- 启动 / 结束应用
constexpr const char* kLaunchToken    = "I-KNOW-THIS-LAUNCHES";
constexpr const char* kTerminateToken = "I-KNOW-THIS-CLOSES";

// 应用装在哪儿。默认给 Any —— 让系统自己找（CFW 上应用常在 SD 或 NAND，写死一个反而错）。
u8 storageIdFromName(const char* name) {
    if (name == nullptr || *name == '\0') return NcmStorageId_Any;
    if (std::strcmp(name, "sd") == 0) return NcmStorageId_SdCard;
    if (std::strcmp(name, "user") == 0 || std::strcmp(name, "nand") == 0) return NcmStorageId_BuiltInUser;
    if (std::strcmp(name, "system") == 0) return NcmStorageId_BuiltInSystem;
    if (std::strcmp(name, "gc") == 0) return NcmStorageId_GameCard;
    if (std::strcmp(name, "any") == 0) return NcmStorageId_Any;
    return 0xFF;  // 非法名字由调用方拒掉
}

// 列出正在跑的进程（pid + program_id）—— 决定"关哪个"之前先看它。
void cmdProcesses(const nxc::Args&, nxc::Reply& r) {
    if (R_FAILED(pmdmntInitialize())) { r.fail(503, "pmdmnt-unavailable"); return; }

    constexpr u32 kMax = 64;
    u64 pids[kMax] = {0};
    u32 count = 0;
    const Result rc = pmdmntGetJitDebugProcessIdList(&count, pids, kMax);
    if (R_FAILED(rc)) {
        pmdmntExit();
        r.failf(500, "process-list-failed rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }
    if (count > kMax) count = kMax;

    for (u32 i = 0; i < count; ++i) {
        r.next();
        r.kvi("pid", static_cast<long long>(pids[i]));
        u64 programId = 0;
        if (R_SUCCEEDED(pmdmntGetProgramId(&programId, pids[i]))) {
            r.kvf("program_id", "0x%016llX", static_cast<unsigned long long>(programId));
        }
    }
    pmdmntExit();

    r.next();
    r.kvi("count", count);
    r.raw("hint: title.terminate pid=<上面的 pid> 可以结束某个进程");
}

void cmdLaunch(const nxc::Args& a, nxc::Reply& r) {
    const char* token = a.get("confirm");
    if (token == nullptr || std::strcmp(token, kLaunchToken) != 0) {
        // ★★★ 2026-10-06 晚：原来 `fail` 之后的 `raw("usage: …")` **发不出去**（ERR 不带载荷）
        //   ⇒ 调用方只知道被拒、不知道该带哪个口令。折进消息里。
        //   ★ 另外把「启动游戏这条路实测走不通、替代是用 input 按 A」也一并告诉它 ——
        //     否则 AI 会反复试一条已经验证过不通的路。
        //   ★ 消息长度要控制在 `errDetail_[256]` 以内（超了会被截断），所以这里只留
        //     「口令 + 替代做法」，完整用法让调用方去看 help title。
        r.failf(403, "refusing-without-confirm-token: 需要 confirm=%s。"
                     "★ 实测「启动游戏」走不通（applet 被拒 0x1159、program 会挂住）；"
                     "替代：用 input 在主界面按 A",
                kLaunchToken);
        return;
    }

    u64 appId = 0;
    if (!parseId(a.get("app"), &appId)) { r.fail(400, "missing-or-bad-app"); return; }

    const u8 storage = storageIdFromName(a.get("storage"));
    if (storage == 0xFF) { r.fail(400, "storage-must-be-any|sd|user|system|gc"); return; }

    const char* mode = a.get("mode");
    if (mode == nullptr || *mode == '\0') mode = "applet";

    // ★★★ 2026-10-06 晚：**mode 必须走白名单**。
    //   原来只判断 "applet" / "libapplet"，**其余任何值（包括写错的）**都会落到最后那条
    //   `pmshellLaunchProgram` 兜底路 —— 而那条路实测**会把请求挂住 60 秒以上**
    //   （代码注释里自己写着的）。⇒ 一个拼错的参数就能把整个平台拖住 60 秒，
    //   绝不能让这种失败形态存在。拼错就当场拒绝，并把正确取值告诉调用方。
    if (std::strcmp(mode, "applet") != 0 && std::strcmp(mode, "program") != 0 &&
        std::strcmp(mode, "libapplet") != 0) {
        r.failf(400, "bad-mode: mode 只接受 applet|program|libapplet（当前=%s）。"
                     "★ applet 实测被拒 0x1159；program 实测会挂住 60 秒以上，慎用",
                mode);
        return;
    }

    // ★★★ 实验路径：**sysmodule 到底能不能启动 library applet？**
    //
    // 为什么这条最重要：`appletRequestLaunchApplication`（启动游戏）的注释明确写着
    // "Only available with AppletType_*Application, or AppletType_LibraryApplet"
    // —— 我们的 sysmodule 两者都不是，所以实测被拒（rc=0x1159）。
    //
    // 但 `appletCreateLibraryApplet` 的注释**没有这条限制**。
    // 如果 sysmodule 能启动 library applet，那么：
    //     sysmodule → 启动一个库小程序（它有 applet 身份）→ 由它去调 appletRequestLaunchApplication
    // ⇒ **"启动游戏"这条死角就有解了**（而且不用模拟输入、不用模拟按键）。
    //
    // 这条路径只做"创建 + 启动 + 请求退出"，把两个 rc 原样报出来：
    //   * rc 是权限类错误 ⇒ 这条路堵死，如实上报
    //   * rc 是"找不到那个 applet"之类 ⇒ **说明权限过了**，只是需要我们自己提供一个库小程序
    if (std::strcmp(mode, "libapplet") == 0) {
        long long id = a.getInt("applet", 0x0C);   // 默认 0x0C = controller（手柄设置，无害可见）
        nxc::host::breadcrumb("title.launch LIBAPPLET id=0x%llX",
                              static_cast<unsigned long long>(id));
        if (R_FAILED(appletInitialize())) {
            r.fail(503, "applet-unavailable");
            return;
        }
        AppletHolder holder;
        const Result rcCreate = appletCreateLibraryApplet(
            &holder, static_cast<AppletId>(id), LibAppletMode_AllForeground);
        if (R_FAILED(rcCreate)) {
            appletExit();
            // ★★★ 2026-10-06 晚：**失败回 ERR**。原来回的是 `OK` + `result=创建失败`，
            //   把"这条路走不通"写进了成功回包（调用方不看字段就会以为 applet 能启动）。
            //   读法（权限类 = 堵死 / 找不到 applet = 权限过了）一并折进消息里 —— 原来那句
            //   `raw` 提示在 OK 分支还能看到，改成 ERR 之后 raw 会被丢掉。
            r.failf(500, "libapplet-create-failed applet_id=0x%02llX create_rc=0x%08X"
                         "（权限类错误 ⇒ 这条路堵死；找不到 applet 之类 ⇒ 权限这关过了）",
                    static_cast<unsigned long long>(id), static_cast<unsigned>(rcCreate));
            return;
        }
        const Result rcStart = appletHolderStart(&holder);
        if (R_FAILED(rcStart)) {
            appletHolderClose(&holder);
            appletExit();
            r.failf(500, "libapplet-start-failed applet_id=0x%02llX start_rc=0x%08X",
                    static_cast<unsigned long long>(id), static_cast<unsigned>(rcStart));
            return;
        }
        // 让它自己退，别留在屏幕上
        appletHolderRequestExitOrTerminate(&holder, 3000000000ULL);
        appletHolderClose(&holder);
        appletExit();
        r.kvf("applet_id", "0x%02llX", static_cast<unsigned long long>(id));
        r.kv("result", "★ 成功了 —— sysmodule 能启动 library applet");
        return;
    }

    // ★★ 实测教训：启动"应用"要走 **applet 管理器**，不能走 `pm:shell`。
    //
    // 第一版只用了 `pmshellLaunchProgram`，真机上**每个 storage 值都失败**：
    //     sd → 0x00000408 (fs)   system → 0x00000408   gc → 0x00020A05 (ncm)
    //     user → 0xCA47D802      any → 0x00001805
    // 那一族接口是给"启动普通程序"用的，不是给"启动游戏"用的。
    //
    // 正道是 `appletRequestLaunchApplication(app_id, NULL)` ——
    // HOME 菜单点一个游戏时走的就是它（它把请求交给 applet 管理器去真正启动）。
    nxc::host::breadcrumb("title.launch app=0x%llX mode=%s storage=%u",
                          static_cast<unsigned long long>(appId), mode, storage);

    Result rcApplet = 0;
    Result rcProgram = 0;
    bool appletTried = false;

    if (std::strcmp(mode, "applet") == 0) {
        appletTried = true;
        if (R_SUCCEEDED(appletInitialize())) {
            rcApplet = appletRequestLaunchApplication(appId, nullptr);
            appletExit();
        } else {
            rcApplet = -1;
        }
        if (R_SUCCEEDED(rcApplet)) {
            r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
            r.kv("mode", "applet");
            r.kv("result", "launch-requested");
            r.raw("note: 这是「请求启动」，游戏是异步起来的 —— 稍等一下再用 title.current 确认");
            return;
        }
        // ★★ 这里**故意不自动回退**。
        //   实测：applet 路径返回 0x1159 之后自动去试 `pmshellLaunchProgram`，
        //   那个调用**把请求挂住了 60 秒以上**（PC 侧 timeout 断连）。
        //   **"挂住"比"报错"糟得多** —— 调用方既拿不到结果也不知道发生了什么事。
        //   要试 program 路径，请**显式**传 mode=program。
        nxc::host::log("title.launch: applet 路径失败 rc=0x%08X",
                       static_cast<unsigned>(rcApplet));
        // ★★★ 2026-10-06 晚：原来这里 failf 之后跟着三行 `raw("note: …")` ——
        //   **ERR 不带载荷 ⇒ 那三行（含"已验证可行的替代"）一句也发不出去**，
        //   调用方只看到一句 `applet-launch-failed rc=0x1159`，然后会**反复去试一条
        //   已经验证过不通的路**。现在把这些折进错误消息里。
        r.failf(500, "applet-launch-failed rc=0x%08X（applet 管理器拒绝了。想试 pm:shell 那条路请"
                     "把 mode 显式设成 program，但实测它会挂住）"
                     "★ 已验证可行的替代：用 input 在主界面按 A 启动",
                static_cast<unsigned>(rcApplet));
        return;
    }

    // 兜底：老的 program 路径（实测对应用不管用，但对"普通程序"可能有用）
    if (R_FAILED(pmshellInitialize())) {
        r.failf(503, "pmshell-unavailable applet_rc=0x%08X", static_cast<unsigned>(rcApplet));
        return;
    }

    NcmProgramLocation location;
    std::memset(&location, 0, sizeof(location));
    location.program_id = appId;
    location.storageID = storage;

    u64 pid = 0;
    rcProgram = pmshellLaunchProgram(PmLaunchFlag_None, &location, &pid);
    pmshellExit();

    if (R_FAILED(rcProgram)) {
        r.failf(500, "launch-failed applet_rc=0x%08X program_rc=0x%08X program_tried=%d storage=%u",
                static_cast<unsigned>(rcApplet), static_cast<unsigned>(rcProgram),
                appletTried ? 1 : 0, storage);
        return;
    }

    r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
    r.kv("mode", "program");
    r.kvi("storage", storage);
    r.kvi("pid", static_cast<long long>(pid));
    r.kv("result", "launched");
}

void cmdTerminate(const nxc::Args& a, nxc::Reply& r) {
    const char* token = a.get("confirm");
    if (token == nullptr || std::strcmp(token, kTerminateToken) != 0) {
        // ★★★ 2026-10-06 晚：原来 fail 之后的三行 raw（usage/warn/note）**都发不出去**。
        r.failf(403, "refusing-without-confirm-token: 需要 confirm=%s。"
                     "mode=program 不弹错误框；mode=process 会弹「发生错误，软件已关闭」。"
                     "★ 会丢未保存进度",
                kTerminateToken);
        return;
    }

    if (R_FAILED(pmshellInitialize())) { r.fail(503, "pmshell-unavailable"); return; }

    // 先把目标归一化成 pid。
    u64 pid = 0;
    u64 appId = 0;
    if (a.get("pid") != nullptr) {
        if (!parseId(a.get("pid"), &pid)) {
            pmshellExit();
            r.fail(400, "bad-pid");
            return;
        }
    } else {
        if (!parseId(a.get("app"), &appId)) {
            pmshellExit();
            r.fail(400, "need-app-or-pid");
            return;
        }
        const Result rc0 = pmshellGetProcessId(&pid, appId);
        if (R_FAILED(rc0) || pid == 0) {
            pmshellExit();
            r.failf(404, "no-such-running-process rc=0x%08X", static_cast<unsigned>(rc0));
            return;
        }
    }

    // ★ 两种终止方式，实测区别很大：
    //   mode=program（默认）—— `pmshellTerminateProgram(program_id)`：
    //       这是"关闭程序"那条路，**不给游戏留错误框**。
    //   mode=process      —— `pmshellTerminateProcess(pid)`：直接杀进程。
    //       ★ 实测副作用：屏幕上会弹系统提示「发生错误，软件已关闭」需要手动确认。
    //   （用 pid 发起时要先把 program id 反查出来 —— `pmdmntGetProgramId`。）
    const char* mode = a.get("mode");
    if (mode == nullptr || *mode == '\0') mode = "program";

    Result rc;
    if (std::strcmp(mode, "program") == 0) {
        u64 programId = appId;
        if (programId == 0) {
            if (R_FAILED(pmdmntInitialize())) { pmshellExit(); r.fail(503, "pmdmnt-unavailable"); return; }
            const Result rcProg = pmdmntGetProgramId(&programId, pid);
            pmdmntExit();
            if (R_FAILED(rcProg)) {
                pmshellExit();
                r.failf(500, "cannot-resolve-program-id rc=0x%08X", static_cast<unsigned>(rcProg));
                return;
            }
        }
        nxc::host::breadcrumb("title.terminate PROGRAM prog=0x%llX pid=%llu",
                              static_cast<unsigned long long>(programId),
                              static_cast<unsigned long long>(pid));
        rc = pmshellTerminateProgram(programId);
    } else if (std::strcmp(mode, "process") == 0) {
        nxc::host::breadcrumb("title.terminate PROCESS pid=%llu",
                              static_cast<unsigned long long>(pid));
        rc = pmshellTerminateProcess(pid);
    } else {
        pmshellExit();
        r.fail(400, "mode-must-be-program-or-process");
        return;
    }
    pmshellExit();

    if (R_FAILED(rc)) {
        r.failf(500, "terminate-failed mode=%s rc=0x%08X", mode, static_cast<unsigned>(rc));
        return;
    }

    r.kv("mode", mode);
    r.kvi("pid", static_cast<long long>(pid));
    if (appId != 0) r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
    r.kv("result", "terminated");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
//
// ★ 归属原则：本工具的全部知识都写在本文件里；`tools.list` / `tools.doc` 只负责汇总。
//   **AI 就是靠这些字学会用它** —— 改方法时把参数表与教程一起改，别只改实现。
const nxc::Param kList[] = {
    {"count",  "int", false, "60", "最多返回多少条（1..200）；越界回落 60 并在末尾回一行 note_params"},
    {"offset", "int", false, "0",  "从第几条开始列（>=0；为负会归 0）"},
};
const nxc::Param kName[] = {
    {"app", "str", true, nullptr, "应用 title id，支持 0x 十六进制（如 0x010015100B514000）"},
};
const nxc::Param kLaunch[] = {
    {"confirm", "str", true,  nullptr, "确认口令：必须逐字符等于 I-KNOW-THIS-LAUNCHES"},
    {"app",     "str", false, nullptr, "要启动的 title id（0x...）；mode=applet|program 时必填"},
    {"storage", "str", false, "any",   "any|sd|user|system|gc；仅 mode=program 用到"},
    {"mode",    "str", false, "applet","applet|program|libapplet；默认 applet"},
    {"applet",  "int", false, "12",    "仅 mode=libapplet：库小程序 id（十进制）；默认 12 即 0x0C（手柄设置）"},
};
const nxc::Param kTerminate[] = {
    {"confirm", "str", true,  nullptr, "确认口令：必须逐字符等于 I-KNOW-THIS-CLOSES"},
    {"app",     "str", false, nullptr, "要结束的应用 title id（0x...）；与 pid 二选一"},
    {"pid",     "str", false, nullptr, "要结束的进程 pid；给了它就优先用（与 app 二选一）"},
    {"mode",    "str", false, "program","program|process；默认 program"},
};

const nxc::MethodInfo kInfoList = {
    kList, 2, "read",
    "列出已安装应用（application_id + 最后更新时间）。\n"
    "作用: 知道主机上装了哪些应用，拿到 title id 去 title.name / title.launch。\n"
    "参数: count 默认 60、上限 200；offset 默认 0。\n"
    "示例: title.list\n"
    "      title.list count=200 offset=0\n"
    "★ 注意: count 越界（<=0 或 >200）会回落 60，并在末尾多回一行 note_params 说明（不再静默）。\n"
    "注意: offset 为负会归 0。\n"
    "返回: 每行 app=0x... last_updated=...；末行 count= offset=，并带一行 hint。\n"
    "相关: title.name（把 id 换成人能读的名字）、title.launch、title.processes",
};
const nxc::MethodInfo kInfoCurrent = {
    nullptr, 0, "read",
    "看当前有没有应用在跑，并给出它的进程 id 与 program id。\n"
    "作用: 判断「现在能不能安全地做某事」（例如启动前先看有没有在跑）。\n"
    "参数: 无。\n"
    "示例: title.current\n"
    "★ 注意: 停在主界面/相册时没有应用在跑，这是**正常状态**，会回 running=none（不是错误）。\n"
    "返回: running=application|none；在跑时给 pid= 与 program_id=0x...（拿不到 program id 时给 program_id_rc=0x...）。\n"
    "相关: title.processes（列全部进程）、title.list、title.name",
};
const nxc::MethodInfo kInfoName = {
    kName, 1, "read",
    "读一个应用的 NACP：应用名 / 作者 / 版本。\n"
    "作用: 把 title.list 里那串十六进制 id 变成人能读的名字。\n"
    "参数: app = 应用 title id，**支持 0x 十六进制**（title.list 打印的就是 0x...，可原样粘贴）。\n"
    "示例: title.name app=0x010015100B514000\n"
    "注意: 只接受单个 title id；拿不到控制数据会回 404 control-data-unavailable 并带 rc。\n"
    "注意: 名字按系统语言选取（nsGetApplicationDesiredLanguage）。\n"
    "返回: name= author= version=；失败时 404。\n"
    "相关: title.list（拿 id）、title.launch",
};
const nxc::MethodInfo kInfoProcesses = {
    nullptr, 0, "read",
    "列出当前正在跑的进程（pid + program_id）。\n"
    "作用: 决定「关哪个」之前先看它——它给的就是 title.terminate 需要的 pid。\n"
    "参数: 无。\n"
    "示例: title.processes\n"
    "注意: 走 pmdmnt 的进程列表；上限 64 条。\n"
    "返回: 每行 pid= program_id=0x...；末行 count=，并带一行 hint。\n"
    "相关: title.current、title.terminate",
};
const nxc::MethodInfo kInfoLaunch = {
    kLaunch, 5, "action",
    "启动应用 / 库小程序。★ 会改变主机状态，必须带确认口令。\n"
    "作用: 让主机把某个 title 跑起来（或启动一个库小程序）。\n"
    "参数: confirm 必填，必须逐字符等于 I-KNOW-THIS-LAUNCHES（缺或错回 403，错误消息里会直接告诉你该带哪个）。\n"
    "      app = title id（0x...），mode=applet|program 时必填。\n"
    "      mode 默认 applet；storage 默认 any（仅 program 用到）；applet 默认 12（仅 libapplet 用到，只认十进制）。\n"
    "示例: title.launch app=0x010015100B514000 confirm=I-KNOW-THIS-LAUNCHES\n"
    "      title.launch app=0x010015100B514000 confirm=I-KNOW-THIS-LAUNCHES mode=program storage=sd\n"
    "      title.launch mode=libapplet applet=12 confirm=I-KNOW-THIS-LAUNCHES\n"
    "★★ 注意: 「启动游戏」这条路**实测走不通**——\n"
    "      · mode=applet：appletRequestLaunchApplication 被拒，rc=0x1159（sysmodule 不是 Application/LibraryApplet 身份）。\n"
    "      · mode=program：pmshellLaunchProgram 各 storage 都失败，而且**会挂住 60 秒以上**（PC 侧 timeout 断连）。\n"
    "      所以代码**故意不做自动回退**（applet 失败后自动去试 program 正是挂住的来源）；要试 program 请显式传 mode=program。\n"
    "★★ 已验证可行的替代: 用 input 在主界面按 A 启动游戏（先用 title.current 确认停在主界面）。\n"
    "注意: mode=libapplet 可用来启动一个「库小程序」（如 0x0C 手柄设置），成功说明 sysmodule 能启动库小程序。\n"
    "返回: applet 模式回 result=launch-requested；program 模式回 result=launched pid=；libapplet 成功回 result=成功。\n"
    "相关: title.current（确认是否起来了）、title.list、input（按 A 启动的替代）",
};
const nxc::MethodInfo kInfoTerminate = {
    kTerminate, 4, "action",
    "结束一个应用/进程。★ 会丢未保存进度，必须带确认口令。\n"
    "作用: 关闭正在跑的应用或杀掉某个进程。\n"
    "参数: confirm 必填，必须逐字符等于 I-KNOW-THIS-CLOSES（缺或错回 403，消息里会告诉你口令）。\n"
    "      app / pid 二选一（pid 优先）；mode 默认 program。\n"
    "示例: title.terminate app=0x010015100B514000 confirm=I-KNOW-THIS-CLOSES\n"
    "      title.terminate pid=1234 confirm=I-KNOW-THIS-CLOSES mode=process\n"
    "★ 注意: mode=program（默认）走 pmshellTerminateProgram，**不弹错误框**；\n"
    "      mode=process 走 pmshellTerminateProcess，屏幕上会弹「发生错误，软件已关闭」需手动确认。\n"
    "注意: 只给 app 时内部会先用 pmshellGetProcessId 反查 pid；没在跑会回 404 no-such-running-process。\n"
    "返回: mode= pid= result=terminated（给了 app 时还回 app=0x...）。\n"
    "相关: title.processes（拿 pid）、title.current",
};

const nxc::Method kMethods[] = {
    {"list",      "列已安装应用：可选 count=（默认60，上限200）offset=", cmdList, &kInfoList},
    {"current",   "当前在跑什么（应用进程 id → program id；没在跑就返回 running=none）", cmdCurrent, &kInfoCurrent},
    {"name",      "读应用名/作者/版本：app=<title id，支持 0x 十六进制>", cmdName, &kInfoName},
    {"processes", "列出正在跑的进程（pid + program_id），决定关哪个之前先看它", cmdProcesses, &kInfoProcesses},
    {"launch",    "★ 启动应用：app=<0x...> [storage=any|sd|user|system|gc] confirm=I-KNOW-THIS-LAUNCHES", cmdLaunch, &kInfoLaunch},
    {"terminate", "★ 结束应用：app=<0x...> 或 pid=<n> [mode=program|process] confirm=I-KNOW-THIS-CLOSES", cmdTerminate, &kInfoTerminate},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_title, "title",
    "已安装应用、当前在跑什么。含启动 / 结束应用（都要确认口令）", kMethods,
    "查主机上有什么应用、现在在跑什么。\n"
    "什么时候用它：\n"
    "  · title.list 列出已安装应用拿 title id，再用 title.name 换成人能读的名字\n"
    "  · title.current / title.processes 看现在在跑什么（决定能不能安全操作前先看）\n"
    "  · title.launch / title.terminate 启动或关闭应用（都要确认口令 且会改主机状态）\n"
    "★ 启动游戏这条路实测走不通：applet 被拒 0x1159、program 会挂住 60 秒以上（所以不自动回退）。\n"
    "★ 已验证可行的替代：用 input 在主界面按 A 启动。\n"
    "★ 口令：launch 要 I-KNOW-THIS-LAUNCHES；terminate 要 I-KNOW-THIS-CLOSES。\n"
    "★ 缺口令或口令错会回 403 且错误消息里会直接告诉你该带哪个。\n"
    "相关工具：input（模拟按键 含按 A 启动）\n"
);
