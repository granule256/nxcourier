// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 内置工具 · title：应用查询、显式启动和结束。
// 普通应用启动只使用 libnx 公开接口；当前固件超出支持范围时明确拒绝，
// 不构造未经证实的 IApplicationManagerInterface IPC。
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
        r.failf(403, "refusing-without-confirm-token: 需要 confirm=%s", kLaunchToken);
        return;
    }
    const char* mode = a.get("mode");
    if (mode == nullptr || *mode == '\0') mode = "application";
    if (std::strcmp(mode, "application") != 0 && std::strcmp(mode, "applet") != 0 &&
        std::strcmp(mode, "program") != 0 && std::strcmp(mode, "libapplet") != 0) {
        r.fail(400, "mode-must-be-application|applet|program|libapplet");
        return;
    }

    // 保留原有显式实验接口；它不是普通应用启动的自动回退路径。
    if (std::strcmp(mode, "libapplet") == 0) {
        const long long id = a.getInt("applet", 0x0C);
        nxc::host::breadcrumb("title.launch LIBAPPLET id=0x%llX",
                              static_cast<unsigned long long>(id));
        const Result init = appletInitialize();
        if (R_FAILED(init)) { r.failf(503, "applet-unavailable rc=0x%08X", init); return; }
        AppletHolder holder{};
        const Result created = appletCreateLibraryApplet(
            &holder, static_cast<AppletId>(id), LibAppletMode_AllForeground);
        if (R_FAILED(created)) {
            appletExit();
            r.failf(500, "libapplet-create-failed rc=0x%08X", created);
            return;
        }
        const Result started = appletHolderStart(&holder);
        if (R_SUCCEEDED(started)) appletHolderRequestExitOrTerminate(&holder, 3000000000ULL);
        appletHolderClose(&holder);
        appletExit();
        if (R_FAILED(started)) { r.failf(500, "libapplet-start-failed rc=0x%08X", started); return; }
        r.kvf("applet_id", "0x%02llX", static_cast<unsigned long long>(id));
        r.kv("result", "started-and-exit-requested");
        return;
    }

    u64 appId = 0;
    if (!parseId(a.get("app"), &appId) || appId == 0) { r.fail(400, "missing-or-bad-app"); return; }
    const u8 storage = storageIdFromName(a.get("storage"));
    if (storage == 0xFF) { r.fail(400, "storage-must-be-any|sd|user|system|gc"); return; }

    if (std::strcmp(mode, "application") == 0) {
        // libnx ns.h: nsdevLaunchApplicationWithStorageIdForDevelop [1.0.0-17.0.1]。
        // nsGetApplicationManagerInterface 仅返回会话，不等于公开的启动封装。
        // appletCreateApplication 需要 SystemApplet 身份，本模块为 AppletType_None。
        const u32 version = hosversionGet();
        if (version < MAKEHOSVERSION(1, 0, 0) || version > MAKEHOSVERSION(17, 0, 1)) {
            r.fail(409, "application-launch-unsupported: libnx nsdev 支持 1.0.0..17.0.1；当前固件无已实现的普通应用启动接口。请用户判图后操作主界面");
            return;
        }
        const Result init = nsdevInitialize();
        if (R_FAILED(init)) { r.failf(503, "nsdev-unavailable rc=0x%08X", init); return; }
        nxc::host::breadcrumb("title.launch APPLICATION app=0x%llX storage=%u",
                              static_cast<unsigned long long>(appId), storage);
        u64 pid = 0;
        const Result rc = nsdevLaunchApplicationWithStorageIdForDevelop(
            &pid, appId, 0, storage, NcmStorageId_Any);
        nsdevExit();
        if (R_FAILED(rc)) { r.failf(500, "application-launch-failed rc=0x%08X", rc); return; }
        r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
        r.kvi("pid", static_cast<long long>(pid));
        r.kv("mode", "application");
        r.kv("result", "launch-requested");
        r.kv("note", "需用 title.current 和用户判图确认启动结果；此路径尚未真机验收");
        return;
    }

    nxc::host::breadcrumb("title.launch EXPERIMENTAL app=0x%llX mode=%s storage=%u",
                          static_cast<unsigned long long>(appId), mode, storage);
    if (std::strcmp(mode, "applet") == 0) {
        const Result init = appletInitialize();
        if (R_FAILED(init)) { r.failf(503, "applet-unavailable rc=0x%08X", init); return; }
        const Result rc = appletRequestLaunchApplication(appId, nullptr);
        appletExit();
        if (R_FAILED(rc)) {
            r.failf(500, "applet-launch-failed rc=0x%08X；sysmodule 非 Application/LibraryApplet；未自动回退", rc);
            return;
        }
        r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
        r.kv("mode", "applet");
        r.kv("result", "launch-requested");
        return;
    }

    // pm:shell 启动 program，不承诺可启动普通应用；原接口仅显式保留。
    const Result init = pmshellInitialize();
    if (R_FAILED(init)) { r.failf(503, "pmshell-unavailable rc=0x%08X", init); return; }
    NcmProgramLocation location{};
    location.program_id = appId;
    location.storageID = storage;
    u64 pid = 0;
    const Result rc = pmshellLaunchProgram(PmLaunchFlag_None, &location, &pid);
    pmshellExit();
    if (R_FAILED(rc)) { r.failf(500, "program-launch-failed rc=0x%08X storage=%u", rc, storage); return; }
    r.kvf("app", "0x%016llX", static_cast<unsigned long long>(appId));
    r.kvi("pid", static_cast<long long>(pid));
    r.kv("mode", "program");
    r.kv("result", "launch-requested");
}

void cmdTerminate(const nxc::Args& a, nxc::Reply& r) {
    const char* token = a.get("confirm");
    if (token == nullptr || std::strcmp(token, kTerminateToken) != 0) {
        // ★★★ 2026-10-06 晚：原来 fail 之后的三行 raw（usage/warn/note）**都发不出去**。
        r.failf(403, "refusing-without-confirm-token: 需要 confirm=%s。"
                     "关闭后可能出现系统提示框，需用户判图后明确确认下一次按键。"
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

    // 两种终止方式都可能出现系统提示；不能根据返回值或截图大小决定按键。
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
    {"app",     "str", false, nullptr, "要启动的 title id（0x...）；仅 mode=libapplet 可省略"},
    {"storage", "str", false, "any",   "any|sd|user|system|gc；application/program 使用，patch storage 为 any"},
    {"mode",    "str", false, "application","application|applet|program|libapplet；后三者为显式实验接口"},
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
    "请求启动应用；必须 confirm=I-KNOW-THIS-LAUNCHES。\n"
    "默认 mode=application：使用 libnx nsdevLaunchApplicationWithStorageIdForDevelop，\n"
    "仅支持固件 1.0.0..17.0.1；其他版本或版本未知回 409，不猜测 IPC、不自动回退。\n"
    "该旧固件接口已交叉编译，尚未真机验收；当前 22.5 固件普通应用启动仍不支持。\n"
    "参数: app=0x...；storage=any|sd|user|system|gc，默认 any；patch storage 固定 any。\n"
    "示例: title.launch app=0x010015100B514000 confirm=I-KNOW-THIS-LAUNCHES\n"
    "原有显式实验模式保留：mode=applet 需要 Application/LibraryApplet 身份，本 sysmodule 不具备；\n"
    "mode=program 调 pm:shell，曾出现长时间阻塞，不作为普通应用启动方案；\n"
    "mode=libapplet applet=12 创建库小程序并请求退出，不证明可启动普通应用。\n"
    "返回: 请求成功不等于画面已可用，需 title.current 并由用户判图确认。\n"
    "需要主界面输入时先截图、由用户明确确认选中项及下一步，再调用 input；禁止盲按 A。",
};
const nxc::MethodInfo kInfoTerminate = {
    kTerminate, 4, "action",
    "结束一个应用/进程。★ 会丢未保存进度，必须带确认口令。\n"
    "作用: 关闭正在跑的应用或杀掉某个进程。\n"
    "参数: confirm 必填，必须逐字符等于 I-KNOW-THIS-CLOSES（缺或错回 403，消息里会告诉你口令）。\n"
    "      app / pid 二选一（pid 优先）；mode 默认 program。\n"
    "示例: title.terminate app=0x010015100B514000 confirm=I-KNOW-THIS-CLOSES\n"
    "      title.terminate pid=1234 confirm=I-KNOW-THIS-CLOSES mode=process\n"
    "注意: program 走 pmshellTerminateProgram，process 走 pmshellTerminateProcess；\n"
    "两种方式都可能出现系统提示，需用户看截图明确确认后才执行下一次输入。\n"
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
    "普通应用默认走 nsdev，仅支持 1.0.0..17.0.1，尚未真机验收；其他固件回 409。\n"
    "显式实验模式不自动回退；主界面输入必须先由用户判图明确确认。\n"
    "★ 口令：launch 要 I-KNOW-THIS-LAUNCHES；terminate 要 I-KNOW-THIS-CLOSES。\n"
    "★ 缺口令或口令错会回 403 且错误消息里会直接告诉你该带哪个。\n"
    "相关工具：input（模拟按键 含按 A 启动）\n"
);
