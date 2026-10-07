// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 内置工具 · identify：回答「你是谁、什么版本、这次是第几次开机」。
//
// 这是最小、最安全的一个工具 —— 它也是 M0 的验收手段：
// 能拿到 identify 的应答，就说明「加载布局 + NPDM + 构建产物 + 网络」整条链全对。
#include "nxc_sdk.hpp"
#include "../core/platform.hpp"   // 只为读 sm 状态做诊断（不碰其他核心接口）

#include <switch.h>

namespace {

void cmdIdentify(const nxc::Args&, nxc::Reply& r) {
    r.kv("platform", nxc::kPlatformName);
    r.kv("version", nxc::kPlatformVersion);
    r.kvi("boot", nxc::host::bootCount());
    r.kvi("tools", nxc::host::toolCount());
    r.kv("heartbeat", nxc::host::heartbeatPath());
    // ★★★ 两个 sm 状态并列报出来，方便对照：
    //   flag   = 我们自己的记录（不代表 libnx 真的关掉了）
    //   active = 直接问 libnx 的会话是否活跃 ← 这个才是真凭据
    r.kvi("sm_flag", nxc::host::smIsOpen() ? 1 : 0);
    r.kvi("sm_active", nxc::host::smReallyOpen() ? 1 : 0);
    // ★★★ 这个才是有用的：**进本条命令时**（SmGuard 还没开）sm 是不是活跃的。
    //   =0 ⇒ 我们真的关掉了 ✓   =1 ⇒ **没关掉**（libnx 引用计数没归零）
    r.kvi("sm_at_entry", nxc::host::smActiveAtDispatchEntry());
    r.raw("note: 看 sm_at_entry —— =0 才是真的关掉了；sm_active 是在命令执行中读的，必然为 1");

    // 固件版本：__appInit 里读过一次用于 hosversionSet；这里再开一次会话把原始值上报。
    const Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            r.kvf("firmware", "%u.%u.%u", fw.major, fw.minor, fw.micro);
        } else {
            r.kv("firmware", "unreadable");
        }
        setsysExit();
    } else {
        r.kv("firmware", "setsys-unavailable");
    }

    const u32 hv = hosversionGet();
    r.kvf("hos", "%u.%u.%u", (hv >> 16) & 0xFFu, (hv >> 8) & 0xFFu, hv & 0xFFu);
}

const nxc::MethodInfo kInfoId = {
    nullptr, 0, "read",
    "作用：回答「你是谁、什么版本、这次是第几次开机」—— 最小、最安全的一条命令。\n"
    "返回：platform=平台名 version=平台版本 boot=开机次数 tools=已注册工具数 "
    "heartbeat=心跳文件路径 firmware=固件版本 hos=HOS 版本。\n"
    "★ 开机次数取自 SD 上的 /config/nxc/heartbeat.txt：读里面的 boot= 再加一。"
    "那个文件被删掉 / 被换到别的目录时，计数会从 1 重新开始 —— "
    "它是「文件没了」而不是「重启失败」；回包里的 heartbeat= 就是它实际读的那个路径。\n"
    "★ 还附带三个 sm 状态（诊断用）：sm_at_entry 才是真凭据 —— "
    "它在进本条命令时读、=0 表示 sm 确实已关；sm_active 是在命令执行途中读的、必然为 1；"
    "sm_flag 只是我们自己的记录。\n"
    "示例：identify.id\n"
    "注意：只读、无副作用；连不上平台时都应先试它 —— 能拿到应答就说明整条链是通的。\n"
    "返回：platform= version= boot= tools= heartbeat= sm_flag= sm_active= sm_at_entry= "
    "firmware= hos= 外加一行 # 说明。\n"
    "相关：log.info（日志放在哪）、net.info（网络状态）、tools.list（有哪些工具）\n",
};

const nxc::Method kMethods[] = {
    {"id", "上报平台版本、固件版本、开机次数、已注册工具数", cmdIdentify, &kInfoId},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_identify, "identify", "身份信息", kMethods,
    "回答「你是谁、什么版本、这次是第几次开机」。\n"
    "什么时候用它：\n"
    "  · 刚连上设备、先确认对端在不在、是什么版本\n"
    "  · 看这次开机是第几次（排查「模块有没有随开机加载」）\n"
    "  · 顺带数一下已注册的工具数\n"
    "★ 开机号存在 SD 的 /config/nxc/heartbeat.txt 里 —— 那个文件被删/换目录计数会从 1 重数。\n"
    "边界：全部只读、无副作用。\n"
    "相关工具：log（日志）、net（网络）、tools（工具清单）"
);
