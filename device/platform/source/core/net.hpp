// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// 固定的网络监听参数；睡眠前必须通过PSC关闭全部socket。
#pragma once

namespace nxc {
namespace net {
constexpr unsigned short kPort = 47800;
inline constexpr int listenerBacklog() { return 8; }
inline constexpr bool useReuseAddr() { return true; }
}  // namespace net
}  // namespace nxc
