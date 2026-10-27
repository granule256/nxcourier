// 宿主机单元测试：工具自描述的"参数摘要"格式化（`nxc::info::paramSummary`）。
//
// 为什么值得单独测：这一段字符串是**设备侧压给 AI 看的参数契约** ——
// `tools.list` 每个方法的 `params=` 就是它。它错一位，AI 就会传错参数。
// 而 `nxc_sdk.hpp` 是纯 C++（不依赖 libnx）⇒ 这里测的就是**设备上跑的那一份代码**。
//
// 跑法：
//     cd device/platform/tests
//     clang++ -std=c++17 -Wall -I../include test_toolinfo.cpp -o /tmp/nxc_info_test && /tmp/nxc_info_test

#include "nxc_sdk.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failed = 0;
int g_total = 0;

void check(bool ok, const char* what, const std::string& detail) {
    ++g_total;
    if (ok) return;
    ++g_failed;
    std::printf("  x %s  —— %s\n", what, detail.c_str());
}

std::string summarize(const nxc::MethodInfo* mi) {
    char buf[256];
    nxc::info::paramSummary(mi, buf, sizeof(buf));
    return std::string(buf);
}

void testEmpty() {
    std::printf("[1] 没有元数据时\n");
    check(summarize(nullptr).empty(), "info=nullptr ⇒ 空串", summarize(nullptr));
    const nxc::MethodInfo noParams = {nullptr, 0, "read", "…"};
    check(summarize(&noParams).empty(), "没有参数 ⇒ 空串", summarize(&noParams));
}

void testRequiredAndDefault() {
    std::printf("[2] 必填 / 选填 / 有默认值\n");
    const nxc::Param ps[] = {
        {"path", "str", true, nullptr, "绝对路径"},
        {"len", "int", false, "24000", "最多读多少字节"},
        {"append", "0|1", false, "0", "1=追加"},
    };
    const nxc::MethodInfo mi = {ps, 3, "read", "…"};
    // ★ 必填带 `*`；有默认值的写成 `名字(类型=默认值)`
    check(summarize(&mi) == "path*(str),len(int=24000),append(0|1=0)",
          "摘要形如 path*(str),len(int=24000),append(0|1=0)", summarize(&mi));
    // ★ 值里**不能有空格** —— 否则会被协议转义成 %20，人读起来就花了
    check(summarize(&mi).find(' ') == std::string::npos, "值里不含空格（不会被 %20 转义）",
          summarize(&mi));
}

void testSensitive() {
    std::printf("[3] 边界与截断\n");
    const nxc::Param ps[] = {
        {"a", "str", true, nullptr, nullptr},
        {"b", "str", true, nullptr, nullptr},
        {"c", "str", true, nullptr, nullptr},
    };
    const nxc::MethodInfo mi = {ps, 3, "read", "…"};
    char tiny[10];
    const int n = nxc::info::paramSummary(&mi, tiny, sizeof(tiny));
    check(n == static_cast<int>(sizeof(tiny)) - 1, "cap 不够时返回 cap-1", std::to_string(n));
    check(tiny[sizeof(tiny) - 1] == '\0', "cap 不够时仍然以 NUL 结尾",
          std::string("结尾字节=") + std::to_string(static_cast<int>(tiny[sizeof(tiny) - 1])));
    check(summarize(&mi).rfind("a*(str)", 0) == 0, "截断发生在末尾、开头仍然完整",
          summarize(&mi));

    char zero[1];
    check(nxc::info::paramSummary(&mi, zero, 1) == 0, "cap=1 时不越界、返回 0", "");
}

void testMethodDefaults() {
    std::printf("[4] ★ 向后兼容：老写法的方法表仍然成立\n");
    // 关键保证：`Method` 新加的 `info` 字段**带默认值**，所以只写三项的旧写法编得过、
    // 而且 info 就是 nullptr（`tools.list` 会因此不输出 risk/params）。
    const nxc::Method m = {"old", "老写法", nullptr};
    check(m.info == nullptr, "三项初始化的 Method，info 默认 nullptr", "");
    const nxc::Method withInfo = {"new", "新写法", nullptr, nullptr};
    check(withInfo.info == nullptr, "显式写第四项也行", "");
    // Tool 的 doc 同理
    const nxc::Tool t = {"t", "工具", nullptr, 0};
    check(t.doc == nullptr, "三项初始化的 Tool，doc 默认 nullptr", "");
}

}  // namespace

int main() {
    testEmpty();
    testRequiredAndDefault();
    testSensitive();
    testMethodDefaults();
    std::printf("\n%d/%d 通过\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
