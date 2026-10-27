// 宿主机单元测试：值的编解码（**入方向与出方向必须对称**）。
//
// 为什么能在宿主机跑：`nxc_sdk.hpp` 是**纯 C++**（不依赖 libnx），所以这里直接
// `#include` 拿的是**设备上跑的那一份代码**，不是抄一份副本 —— 测的就是真东西。
//
// 跑法（本机有 clang++ 即可，不需要 devkitPro）：
//     cd device/platform/tests
//     clang++ -std=c++17 -Wall -I../include test_codec.cpp -o /tmp/nxc_codec_test && /tmp/nxc_codec_test
//
// 背景：见 docs/反馈-参数值含空格无法表达-20261005.md ——
// 出方向把空格转义成 %20，入方向却没有还原，导致卡上"带空格的目录名"列得出来、喂不回去。

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
    std::printf("  ✗ %s  —— %s\n", what, detail.c_str());
}

void expectEq(const std::string& got, const char* want, const char* what) {
    check(got == want, what, "期望 [" + std::string(want) + "]，实际 [" + got + "]");
}

// 走真正的 escapeValue（经由 Reply 的私有包装：用 kv 写进去再看 body）
std::string escapeViaReply(const char* value) {
    nxc::Reply r;
    r.reset();
    r.kv("k", value);
    const char* body = r.body();
    const char* eq = std::strchr(body, '=');
    return eq ? std::string(eq + 1) : std::string();
}

std::string unescapeCopy(const std::string& in) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", in.c_str());
    nxc::codec::unescapeValueInPlace(buf);
    return std::string(buf);
}

}  // namespace

int main() {
    std::printf("== 出方向：escapeValue（设备 → PC）==\n");
    expectEq(escapeViaReply(""), "", "空串不动");
    expectEq(escapeViaReply("abc"), "abc", "普通串不动");
    expectEq(escapeViaReply("/a b/c"), "/a%20b/c", "空格 → %20");
    expectEq(escapeViaReply("100%"), "100%25", "% → %25");
    expectEq(escapeViaReply("a b%c"), "a%20b%25c", "空格与 % 混合");
    expectEq(escapeViaReply("a\tb"), "a%09b", "制表符 → %09");
    expectEq(escapeViaReply(std::string("a\x01" "b").c_str()), "a%01b", "控制字符 → %01");
    expectEq(escapeViaReply(std::string("a\x7f" "b").c_str()), "a%7Fb", "0x7F → %7F");
    expectEq(escapeViaReply("中文"), "中文", "多字节 UTF-8 不转义");

    std::printf("== 入方向：unescapeValueInPlace（PC → 设备）==\n");
    expectEq(unescapeCopy(""), "", "空串不动");
    expectEq(unescapeCopy("/a/b"), "/a/b", "无反斜杠的普通路径不动");
    expectEq(unescapeCopy("/a%20b/c"), "/a b/c", "%20 → 空格");
    expectEq(unescapeCopy("100%25"), "100%", "%25 → %");
    expectEq(unescapeCopy("a%09b"), "a\tb", "%09 → 制表符");
    expectEq(unescapeCopy("a%2fb"), "a/b", "小写十六进制也认");
    expectEq(unescapeCopy("50%"), "50%", "★ 末尾单个 % 原样保留");
    expectEq(unescapeCopy("a%zz"), "a%zz", "★ % 后不是十六进制则原样保留");
    expectEq(unescapeCopy("a%2"), "a%2", "★ % 后只有一位十六进制则原样保留");
    expectEq(unescapeCopy("中文%20名"), "中文 名", "UTF-8 与转义混排");

    std::printf("== 往返：自然值 → 转义 → 还原，必须回到原值 ==\n");
    const char* kRoundTrip[] = {
        "",
        "abc",
        ("/atmosphere/contents/010021C000B6A000/romfs/isaac_mods/mods/"
         "visible tinted rocks_3002235272"),
        "/romfs/rp_patch/resources/gfx/ui/main menu/gamemenu.anm2",
        "/switch/nxc-recovery/nxc-platform-14tools.nsp",
        "100% done",
        "a%20b",              // ★ 字面 %20 也要能往返（转义后是 %2520）
        "%",
        "%%%",
        "tab\there",
        "new\nline",
        "中文 目录/子 目录.xml",
        "a=b",                // 值里本来就可以有 '='（切词只按空格，'=' 只切第一个）
    };
    for (const char* s : kRoundTrip) {
        check(unescapeCopy(escapeViaReply(s)) == s, "往返一致",
              "原值 [" + std::string(s) + "] 转义后 [" + escapeViaReply(s) + "]");
    }

    std::printf("== 协议形状：转义后的值里不能再有空格/换行 ==\n");
    for (const char* s : kRoundTrip) {
        const std::string enc = escapeViaReply(s);
        check(enc.find(' ') == std::string::npos, "转义后不含空格", enc);
        check(enc.find('\n') == std::string::npos, "转义后不含换行", enc);
        check(enc.find('\t') == std::string::npos, "转义后不含制表符", enc);
    }

    std::printf("== Args：位置参数计数与容量 ==\n");
    {
        nxc::Args a;
        check(a.positionalCount() == 0, "初始为 0", "");
        a.markPositional();
        a.markPositional();
        check(a.positionalCount() == 2, "计数正确", "");
        check(a.add("path", "/a b"), "add 成功", "");
        check(std::strcmp(a.get("path"), "/a b") == 0, "值原样保存（解码在调用方做）", "");
        check(a.getInt("missing", 7) == 7, "getInt 缺省值", "");
        check(a.add("neg", "-12") && a.getInt("neg", 0) == -12, "getInt 负数", "");

        int added = 2;
        while (a.add(("k" + std::to_string(added)).c_str(), "v")) ++added;
        check(added == nxc::kMaxArgs, "超出 kMaxArgs 后 add 返回 false",
              "加进去了 " + std::to_string(added) + " 个，kMaxArgs=" + std::to_string(nxc::kMaxArgs));
    }

    std::printf("\n%s  共 %d 项，失败 %d 项\n",
                g_failed == 0 ? "全部通过 ✓" : "有失败 ✗", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
