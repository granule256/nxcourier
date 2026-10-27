// 宿主机单元测试：2026-10-06 晚「修复批次 A」里那三个**纯算法**改动点。
//
// 为什么要有这个文件：这三处都是"回包看起来正常、其实什么都没做"型的问题，
// 光靠真机手测很容易漏（历史上已经被假成功骗过好几次）。它们又都是纯函数，
// 能在宿主机上测 —— 而且 `nxc_sdk.hpp` 是纯 C++，`#include` 拿到的
// **就是设备上跑的那一份代码**，不是抄的副本。
//
// 跑法（本机有 clang++ 即可，不需要 devkitPro）：
//     cd device/platform/tests
//     clang++ -std=c++17 -Wall -I../include test_fixes.cpp -o /tmp/nxc_fixes_test && /tmp/nxc_fixes_test
//
// 覆盖：
//   1. `util::parseU64` —— 认 `0x` 十六进制（title id 就长这样），且严格、防溢出；
//      `Args::getInt` 严格接受十进制，其他格式返回默认值并标记参数无效；title id 应使用 parseU64。
//   2. `util::b64Decode` —— 非法字符必须被拒（原来静默跳过 ⇒ 写出垃圾还回 OK）。
//   3. `Reply` 的行数上限 —— 必须能装下文档写的 `log.dump lines=60`。

#include "nxc_sdk.hpp"

#include <cstdint>
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

std::string hex64(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
    return std::string(buf);
}

// ---------------------------------------------------------------- 1. parseU64
void testParseU64() {
    std::printf("[1] util::parseU64（认 0x、严格、防溢出）\n");

    // ★ 这个常量取自真实回包：`save.list` 打印的 app=0x010015100B514000
    const uint64_t kIsaac = 0x010015100B514000ull;

    uint64_t v = 0;
    check(nxc::util::parseU64("0x010015100B514000", &v) && v == kIsaac,
          "十六进制 0x… 能解析", "得到 " + hex64(v));
    // ★ 设计选择：**不猜进制**。没有 `0x` 前缀就按十进制读，含字母的串一律拒。
    //   为什么不让它"看出来像十六进制就按十六进制读"：那样 `save.list` 打印的是 `0x…`
    //   本来就没歧义，而模糊规则会让"写错的十进制"悄悄变成另一个 id。
    check(!nxc::util::parseU64("010015100B514000", &v),
          "不写 0x 前缀时含字母的串被拒（不猜进制）", "");
    check(nxc::util::parseU64("72080752691462144", &v) && v == kIsaac,
          "十进制等价值能解析", "得到 " + hex64(v));
    check(nxc::util::parseU64("0X10", &v) && v == 16, "大写 0X 前缀", "得到 " + hex64(v));
    check(nxc::util::parseU64("0", &v) && v == 0, "单个 0", "得到 " + hex64(v));

    check(!nxc::util::parseU64(nullptr, &v), "nullptr 被拒", "");
    check(!nxc::util::parseU64("", &v), "空串被拒", "");
    check(!nxc::util::parseU64("0x", &v), "只有前缀被拒", "");
    check(!nxc::util::parseU64("0xZZ", &v), "非法十六进制字符被拒", "");
    check(!nxc::util::parseU64("12ab", &v), "十进制里混字母被拒", "");
    check(!nxc::util::parseU64("-1", &v), "负数被拒（这是无符号 id）", "");
    check(!nxc::util::parseU64(" 0x10", &v), "前导空格被拒（值里不该有空白）", "");

    check(nxc::util::parseU64("0xFFFFFFFFFFFFFFFF", &v) && v == UINT64_MAX,
          "刚好到 UINT64_MAX 可以", "得到 " + hex64(v));
    check(!nxc::util::parseU64("0x1FFFFFFFFFFFFFFFF", &v),
          "超过 UINT64_MAX 被拒（原来是无检查的溢出）", "");

    // 回归：getInt 拒绝 0x 格式并标记参数无效；十六进制 title id 使用 parseU64。
    nxc::Args a;
    a.add("app", "0x010015100B514000");
    check(a.getInt("app", -1) == -1,
          "Args::getInt 对非十进制格式返回默认值并标记无效",
          "getInt 得到 " + std::to_string(a.getInt("app", -1)));
    check(nxc::util::parseU64(a.get("app"), &v) && v == kIsaac,
          "同一个值 parseU64 能解析对", "得到 " + hex64(v));
}

// ---------------------------------------------------------------- 2. base64 解码
void testB64Strict() {
    std::printf("[2] util::b64Decode（非法字符必须被拒）\n");

    unsigned char out[64];
    std::memset(out, 0, sizeof(out));

    // "hello" 的 base64
    const int n = nxc::util::b64Decode("aGVsbG8=", out, sizeof(out));
    check(n == 5 && std::memcmp(out, "hello", 5) == 0, "正常 base64 解出 hello",
          "长度 " + std::to_string(n));

    check(nxc::util::b64Decode("!!!bad", out, sizeof(out)) < 0,
          "data=!!!bad 被拒（原来会静默写出 2 字节垃圾并回 OK）", "");
    check(nxc::util::b64Decode("aGVs bG8=", out, sizeof(out)) < 0,
          "值里夹空格被拒（协议本就不允许空白）", "");
    check(nxc::util::b64Decode("aGVsbG8=AAAA", out, sizeof(out)) < 0,
          "填充符之后又出现数据被拒", "");
    check(nxc::util::b64Decode("a", out, sizeof(out)) < 0,
          "长度不合法被拒", "");

    // 容量上限：解码结果超过 cap ⇒ -1（不是悄悄截断）
    unsigned char tiny[2];
    check(nxc::util::b64Decode("aGVsbG8=", tiny, sizeof(tiny)) < 0,
          "超过 cap 被拒", "");

    // 编解码往返（覆盖 0..8 字节这些会对齐到填充的边界）
    bool roundOk = true;
    for (int len = 0; len <= 8; ++len) {
        unsigned char src[8];
        for (int i = 0; i < len; ++i) src[i] = static_cast<unsigned char>(i * 37 + 1);
        char enc[32];
        const int en = nxc::util::b64Encode(src, len, enc);
        unsigned char dec[16];
        const int dn = nxc::util::b64Decode(enc, dec, sizeof(dec));
        if (dn != len || std::memcmp(src, dec, static_cast<size_t>(len)) != 0) {
            roundOk = false;
            std::printf("      len=%d enc=[%s] 往返失败 (en=%d dn=%d)\n", len, enc, en, dn);
        }
    }
    check(roundOk, "0..8 字节编解码往返一致", "");
}

// ---------------------------------------------------------------- 3. 应答行数上限
void testReplyLineLimit() {
    std::printf("[3] Reply 行数上限（要装得下 log.dump lines=60）\n");

    check(nxc::kReplyMaxLines > 60,
          "kReplyMaxLines 大于 60（否则 log.dump 的文档上限就是空头支票）",
          "当前 " + std::to_string(nxc::kReplyMaxLines));
    check(nxc::kReplyBytes == 32768,
          "字节上限保持 32768 不变（它是真正的内存成本，别顺手调大）",
          "当前 " + std::to_string(nxc::kReplyBytes));

    // 真实形态：第一行是汇总行（count/boot），之后 60 行日志。
    nxc::Reply r;
    r.reset();
    r.kv("count", "60");
    for (int i = 0; i < 60; ++i) {
        char line[96];
        std::snprintf(line, sizeof(line), "log line %d —— 一行普通日志", i);
        r.raw(line);
    }
    check(!r.overflowed(), "60 条日志 + 1 行汇总不溢出",
          "overflowed=1，lines=" + std::to_string(r.lineCount()));
    check(r.lineCount() == 61, "行数统计正确", std::to_string(r.lineCount()));

    // `title.list` 的默认 count=60 也是同一形态（每条记录一行）
    nxc::Reply t;
    t.reset();
    t.kv("probe", "1");
    for (int i = 0; i < 60; ++i) {
        char line[96];
        std::snprintf(line, sizeof(line), "app=0x%016llX last_updated=%d",
                      static_cast<unsigned long long>(0x010015100B514000ull + i), i);
        t.raw(line);
    }
    check(!t.overflowed(), "title.list 默认 60 条不溢出", "");
}

}  // namespace

int main() {
    testParseU64();
    testB64Strict();
    testReplyLineLimit();
    std::printf("\n%d/%d 通过\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
