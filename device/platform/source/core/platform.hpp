// 平台内部接口（只有核心自己用；工具开发者不需要看这个文件）。
#pragma once

#include "nxc_sdk.hpp"

namespace nxc {

// ---------------------------------------------------------------- sm 作用域
// ★★★ 为什么要有它（2026-10-06）：boot2 模块**不应跨睡眠持有 sm 会话** ——
//   查过两个成熟模块的源码，它们都是「开完就关、运行时再按需开」：
//     · sys-botbase  ：`__appExit` 里有 `smExit()`
//     · nx-ovlloader ：`__appInit` 结束前就 `smExit()`；运行时要开服务时再临时开、用完关
//   只有我们原来一直握着，而实测症状正是「**后台一直存在的东西**导致 omm 崩」。
//   ⇒ 现在对齐成熟做法：boot2 结束就放掉；运行时按命令开、命令结束关。
namespace host {

bool smIsOpen();
bool smOpen();     // 幂等：已开则直接返回 true
void smClose();    // 幂等

// ★★★ 直接问 libnx：sm 会话现在到底是不是活跃的 —— 这才是【真凭据】。
//   为什么必须有它：libnx 的 smInitialize/smExit 是**引用计数**的
//   （`serviceGuardExit` 里只有 refCount 归零才 `serviceClose`），
//   所以「我调了 smExit」**不等于**「会话真关了」。用户质疑过这一点，问得对。
bool smReallyOpen();

// ★★★ 进 dispatch **那一刻**（SmGuard 打开之前）sm 是否活跃 —— 这才是真问题。
//   为什么不能在命令里直接读：命令跑在 SmGuard 里，那时 sm 必然是开的，
//   读出来只会是 1，说明不了"命令之间关没关"。（我第一版就测错时刻了。）
int smActiveAtDispatchEntry();

}  // namespace host


// ---------------------------------------------------------------- 日志
// sysmodule **没有 stdout**，崩机在用户看来就是黑屏。所以日志分两条：
//   breadcrumb —— 覆盖写 SD 上的一个小文件，记录「走到第几步」。崩了看最后一行就知道停在哪。
//   ring       —— 内存里的环形缓冲，PC 连上后用 log.dump 取走，避免频繁写卡。
namespace log {

void init();

// 覆盖写「当前进度」（每个初始化步骤之前调用一次）。
void breadcrumb(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ★★★ 2026-10-06 新增：「黑匣子」。
//   与 breadcrumb 的区别（这个区别是本轮最大的教训）：
//     · breadcrumb 写的是**固定路径**、**覆盖写** ⇒ **只留最后一条**，
//       而且**下一次开机就会被冲掉** ⇒ 想知道"崩之前我们活到哪一步"根本读不到。
//     · watch 写的是 **`/config/nxc/watch_boot<开机号>.txt`** ⇒
//       ① **崩过之后再开机也不会覆盖崩前那一份**；
//       ② 里面同时记 **系统 tick（`uptime_ms`）与 RTC 墙钟（`wall_s`）**
//          —— ★ 睡眠时系统 tick 会停、而 RTC 继续走 ⇒ **两者差值一跳 = "确实睡过"的铁证**。
//   ★ 实现上为了拿墙钟会**临时开一下 sm**（`time` 服务要经 sm）再关掉；失败也不影响写 tick。
void watch(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 追加一条到内存环形缓冲。
void ring(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 把环形缓冲写进应答（最多 maxLines 行）。
void dump(Reply& reply, int maxLines);

// 本次开机是第几次（由心跳文件递增得出）。
int bootCount();

// 心跳文件的路径（也用于给 PC 侧核对）。
const char* heartbeatPath();

}  // namespace log

// ---------------------------------------------------------------- 工具注册表
// 工具描述符由 NXC_DEFINE_TOOL 放进 `nxc_tools` 段，链接器自动收集。
// 这里只负责遍历，不维护任何清单 —— 加工具不需要改核心。
namespace reg {

int toolCount();
const Tool* toolAt(int i);

// 按名字找工具（name 不要求以 '\0' 结尾，用 len 界定）。
const Tool* findTool(const char* name, int len);

}  // namespace reg

// ---------------------------------------------------------------- 协议 / 传输
namespace protocol {

// 建监听口（`socket` + `SO_REUSEADDR` + `bind(47800)` + `listen(8)`）。成功返回 fd，失败 -1。
int openListener();

// 监听 + 多客户端轮询（poll，永不返回）。
// 支持同时服务多个连接 —— MCP 网关持长连接时，命令行仍然进得来。
//
// ★★ `keepListenerOpen`：
//   · `true`  —— 长期保留监听口（= 老的 `listen` 模式，**实测会让主机睡眠崩**）
//   · `false` —— **连上第一个客户端就把监听口关掉**，只留已建立的连接；
//                客户端全断开时再把监听口开回来。
//     这是 `acceptonly` 模式的实现（见 `source/core/net.hpp` 的实测依据）：
//     要验证"元凶是处于 `listen` 状态的口，而已建立的连接是无辜的"。
void serveForever(int listenFd, bool keepListenerOpen);

}  // namespace protocol

}  // namespace nxc
