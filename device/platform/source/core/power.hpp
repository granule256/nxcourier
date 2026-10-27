// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 电源状态通知：睡眠前关闭全部socket，唤醒后恢复监听。
// 不允许跨睡眠持有socket，否则可能阻塞系统的电源切换。
//
// 必须先等PSC事件再GetRequest/Acknowledge，禁止轮询制造假应答。
// 通知线程只维护原子状态，不操作fd、SD、sm或time；网络线程负责关闭fd。
// 通知线程等待关闭确认最多900ms，同一睡眠周期不重复等待。
#pragma once

namespace nxc {
namespace power {

// 注册PSC并启动通知线程，睡眠时始终关闭socket、醒来恢复监听。
// 必须在sm会话打开时调用，且成功后才能创建第一个监听口。
bool start();

// 系统已请求进入睡眠（`ReadySleep` / `ReadySleepCritical` / `ReadyShutdown`）。
bool sleepRequested();

// 通知线程已经把"要睡了"的事件**应答完**了吗（用于主循环决定何时重开监听口）。
bool sleepAcknowledged();

// 主线程报告"我已经把所有 socket 关掉了"（通知线程据此才应答）。
void reportSocketsClosed();

// 主循环消费"刚醒来"这个事件（消费后清除）。
bool consumeWoke();

// 诊断：收到的 PSC 事件数 / 睡眠类状态数 / 最近一个状态值（放进黑匣子，便于事后复盘）。
int eventCount();
int sleepEventCount();
int lastState();

}  // namespace power
}  // namespace nxc
