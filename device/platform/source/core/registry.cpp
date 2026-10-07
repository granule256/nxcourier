// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

#include "platform.hpp"

#include <cstring>

// 工具描述符被 NXC_DEFINE_TOOL 放进 `nxc_tools` 段；GNU ld 会为这类
// 名字合法的孤立段生成 __start_/__stop_ 边界符号，于是核心不需要维护任何清单。
extern "C" const nxc::Tool __start_nxc_tools[];
extern "C" const nxc::Tool __stop_nxc_tools[];

namespace nxc {
namespace reg {

int toolCount() {
    return static_cast<int>(__stop_nxc_tools - __start_nxc_tools);
}

const Tool* toolAt(int i) {
    if (i < 0 || i >= toolCount()) return nullptr;
    return &__start_nxc_tools[i];
}

const Tool* findTool(const char* name, int len) {
    const int n = toolCount();
    for (int i = 0; i < n; ++i) {
        const Tool* t = &__start_nxc_tools[i];
        if (static_cast<int>(std::strlen(t->name)) == len &&
            std::strncmp(t->name, name, static_cast<size_t>(len)) == 0) {
            return t;
        }
    }
    return nullptr;
}

}  // namespace reg

namespace host {

int toolCount() { return reg::toolCount(); }

}  // namespace host
}  // namespace nxc
