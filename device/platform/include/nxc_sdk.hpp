// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// NxCourier 平台 SDK —— 第三方工具开发者只需要包含这一个头文件。
//
// 设计目标：**平台核心 + 注册工具**。核心负责传输、协议、日志、分发；
// 工具负责干活。加一个工具 = 新建一个 .cpp + 用 NXC_DEFINE_TOOL 注册，**不改核心任何文件**。
//
// 协议（行文本，设备侧零依赖；PC 侧网关再翻译成 JSON / MCP）：
//   请求： <工具>.<方法> [k=v ...]\n
//   响应： OK <n>\n            ← n 行载荷
//          <k=v ...>\n          × n
//          ERR <码> <说明>\n    ← 出错时不带载荷
//
// 约定：
//   * 参数与返回值里的**值不能含空格与换行**（含空格的内容请自行 base64）。
//   * 列表型返回：`count=N` 一行，后面每行一条记录。
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nxc {

// 平台版本。工具可以拿它上报，便于 PC 侧判断对端能力。
inline constexpr const char* kPlatformName    = "nxc";
inline constexpr const char* kPlatformVersion = "0.1.0";

// ★★ 平台自己的 Title ID 与内容目录。
//   **必须和 device/platform/config.json 里的 title_id、以及 SD 卡上的目录名完全一致**，
//   否则工具会去写一个错误的位置。改 TID 时这三处一起改。
//   为什么放在这里：`fs` 工具的路径白名单要用它来允许"自我更新"（见 tool_fs.cpp）。
inline constexpr const char* kSelfTitleId    = "4200000000000012";
inline constexpr const char* kSelfContentDir = "/atmosphere/contents/4200000000000012";

// ---------------------------------------------------------------- 值的编解码
// 协议里 `k=v` 的值**不能含空格 / 制表符 / 换行**（字段之间就是用它们分列的）。
// 所以：**出方向**（设备 → PC）把这类字节转义成 `%XX`，**入方向**（PC → 设备）必须**对称地还原**。
//
// ★★ 为什么入方向的还原是必须的（2026-10-05 修）：
//   只有出方向有转义时，`fs.ls` 列出来的名字（`visible%20tinted%20rocks_3002235272`）
//   **喂回去必然失败** —— 带字面空格会被切成两段，带 `%20` 会被当成字面目录名。
//   于是含空格的路径**没有任何写法能寻址**，而失败形态长得像"文件不存在"
//   （真实案例：docs/反馈-参数值含空格无法表达-20261005.md）。
//
// ★ 兼容性代价（改完必须知道）：值里的**字面 `%` 要写成 `%25`**。
//   不含 `%` 的调用（绝大多数）行为完全不变：还原只在"`%` + 两位十六进制"时才生效。
namespace codec {

// '0'..'9' / 'a'..'f' / 'A'..'F' → 0..15；不是十六进制字符返回 -1。
inline int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 转义一个值写进 out（调用方保证 out 至少够大）。返回写入长度。
// 规则（**改动时要和 unescapeValueInPlace 一起改**）：
//   `%` → `%25`（必须先做，否则会把后面生成的 `%` 再转一遍）
//   空格 → `%20`；其它不可见字符（< 0x20、0x7F）→ `%XX`
//   ★ 多字节 UTF-8（中文等，字节 ≥ 0x80）**不转义**，所以整串仍是合法 UTF-8。
inline int escapeValue(const char* in, char* out, int cap) {
    int o = 0;
    for (const char* p = in; p != nullptr && *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '%' || c == ' ' || c < 0x20 || c == 0x7f) {
            if (o + 4 >= cap) break;
            o += std::snprintf(out + o, static_cast<size_t>(cap - o), "%%%02X", c);
        } else {
            if (o + 1 >= cap) break;
            out[o++] = static_cast<char>(c);
        }
    }
    if (o < cap) out[o] = '\0';
    return o;
}

// 就地还原 —— `escapeValue` 的逆运算。**只会变短，所以不会溢出**。
// ★ 宽容策略（很重要，别改成"严格的"）：`%` 后面不是两位十六进制时**原样保留**。
//   这样"老客户端发来的未转义值"与"新客户端发来的转义值"都能用：
//   `path=/a/b`   原样 → `/a/b`；
//   `path=/a%20b` 还原 → `/a b`；
//   `path=50%`    保留 → `50%`（末尾单 `%` 是合法的字面量）。
inline void unescapeValueInPlace(char* s) {
    if (s == nullptr) return;
    char* w = s;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p == '%') {
            const int hi = hexDigit(p[1]);
            const int lo = (hi >= 0) ? hexDigit(p[2]) : -1;
            if (lo >= 0) {
                *w++ = static_cast<char>((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        *w++ = *p;   // 未转义的字节；w 落后于 p 时这里是"就地左移"
    }
    *w = '\0';
}

}  // namespace codec

// ---------------------------------------------------------------- 容量上限
// 全部用固定缓冲，不做动态分配 —— 这是 sysmodule 内存受限下的硬要求。
inline constexpr int kMaxArgs        = 24;
inline constexpr int kArgNameBytes   = 32;
// ★ 从 512 提到 4096：这个值直接决定"一条 fs.write 能带多少字节"。
//   512 时一条只能写 381 字节 ⇒ 传个 7.7MB 的文件要 2 万条命令、约 10 分钟。
//   提到 4096 后一条能写 3060 字节，慢得不能忍的上传才有救。
inline constexpr int kArgValueBytes  = 4096;
// ★ 从 8KB 提到 32KB：这是「一帧截图要几个来回」的直接决定因素。
//   实测单次 fs.read 往返约 11.8ms，原来一次只回 3072 字节 ⇒ 一帧 190KB 要 47 个来回 ≈ 555ms。
//   提到 32KB 后一帧只要 6 次（还能凑成一批），取回耗时从约 620ms 降到约 80ms。
inline constexpr int kReplyBytes     = 32768;
// ★★★ 2026-10-06 晚：从 48 提到 256。
//
// 为什么原来的 48 是**错的**：真正的硬上限是上面那个**字节数**；48 行只是过早收紧，
// 而它把四个工具的"文档上限"全变成了空头支票：
//   · `log.dump`   —— help 写 `lines=1..60`，实测 47 过、48 挂（47 行约 7.5KB，远小于 32KB）；
//   · `title.list` —— ★ `count` **默认就是 60**，比 47 还大 ⇒ 主机上 title 超过 46 个时
//                     **不带任何参数调用都会失败**（不是"只有传大值才失败"）；
//   · `save.list` / `fs.ls` —— 上限都写 200。
// 这些都是"一条记录一行"的输出，撞的**全是行数**，与字节无关。
//
// ★ 不继续往大调、也**不动 `kReplyBytes`**：`Reply` 是**每客户端一份**的常驻对象
//   （`protocol.cpp` 的 `kMaxClients=4`），**只有字节数是内存成本**；行数上限纯粹是道
//   策略闸门 —— 放宽它一分钱内存都不花，字节数那道上限继续当兜底（真撑不下就如实回 ERR）。
inline constexpr int kReplyMaxLines  = 256;

// ---------------------------------------------------------------- 参数
struct Arg {
    char key[kArgNameBytes];
    char value[kArgValueBytes];
};

class Args {
public:
    const char* get(const char* key) const {
        for (int i = 0; i < count_; ++i) {
            if (std::strcmp(items_[i].key, key) == 0) return items_[i].value;
        }
        return nullptr;
    }

    // 取整数；缺失或解析不出时返回 fallback。
    long long getInt(const char* key, long long fallback) const {
        const char* v = get(key);
        if (v == nullptr || *v == '\0') return fallback;
        bool neg = false;
        if (*v == '-') { neg = true; ++v; }
        long long acc = 0;
        bool any = false;
        while (*v >= '0' && *v <= '9') { acc = acc * 10 + (*v - '0'); any = true; ++v; }
        if (!any) return fallback;
        return neg ? -acc : acc;
    }

    bool has(const char* key) const { return get(key) != nullptr; }
    int count() const { return count_; }
    const Arg& at(int i) const { return items_[i]; }

    // ★ 位置参数：请求里"没有 `=` 的词"（协议本来是纯 k=v，出现它通常意味着
    //   **值里有空格被切碎了** —— 这是最难查的一类输入错误：失败形态长得像"文件不存在"）。
    //   只用于在**失败时**给一句提示，不参与任何业务逻辑。
    void markPositional() { ++positional_; }
    int positionalCount() const { return positional_; }

    // 由协议层调用；超出容量则丢弃并返回 false。
    bool add(const char* key, const char* value) {
        if (count_ >= kMaxArgs) return false;
        Arg& a = items_[count_++];
        std::snprintf(a.key, sizeof(a.key), "%s", key ? key : "");
        std::snprintf(a.value, sizeof(a.value), "%s", value ? value : "");
        return true;
    }

private:
    Arg items_[kMaxArgs];
    int count_ = 0;
    int positional_ = 0;
};

// ---------------------------------------------------------------- 应答
class Reply {
public:
    void reset() {
        len_ = 0;
        lines_ = 1;
        overflow_ = false;
        errCode_ = 0;
        errMsg_ = nullptr;
        buf_[0] = '\0';
    }

    // 往**当前行**追加一个 k=v。
    void kv(const char* key, const char* value) {
        appendField(key, "%s", value ? value : "");
    }

    void kvi(const char* key, long long value) {
        appendField(key, "%lld", value);
    }

    void kvf(const char* key, const char* fmt, ...) {
        char tmp[kArgValueBytes];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);
        appendField(key, "%s", tmp);
    }

    // 结束当前行，开始下一行。当前行还是空的时不做事（这样连续调用不会产生空行）。
    void next() {
        if (len_ == 0 || buf_[len_ - 1] == '\n') return;
        if (lines_ >= kReplyMaxLines || len_ + 1 >= kReplyBytes) { overflow_ = true; return; }
        buf_[len_++] = '\n';
        buf_[len_] = '\0';
        ++lines_;
    }

    // 追加一整行**自由文本**（可含空格）。以 `#` 打头，供日志这类不需要结构化的内容用。
    // 控制字符会被替换成空格，保证一行一条不破坏协议。
    void raw(const char* text) {
        if (overflow_) return;
        if (len_ != 0 && buf_[len_ - 1] != '\n') next();
        if (overflow_) return;
        if (len_ + 2 >= kReplyBytes) { overflow_ = true; return; }
        buf_[len_++] = '#';
        buf_[len_] = '\0';
        for (const char* p = text; p && *p; ++p) {
            if (len_ + 2 >= kReplyBytes) { overflow_ = true; return; }
            char c = *p;
            buf_[len_++] = (c >= 0x20 && c != 0x7f) ? c : ' ';
            buf_[len_] = '\0';
        }
    }

    // 请求失败：协议层会改用 ERR 形式回包，不再发载荷。
    void fail(int code, const char* msg) { errCode_ = code; errMsg_ = msg; }

    // ★ 带细节的失败。为什么需要它：`fail()` 走的是 ERR 分支，**此前 kv 写下的载荷会被丢掉**
    //   （协议里 ERR 不带载荷），所以像 `rc=0x...` 这种关键诊断信息会白白消失。
    //   实测教训：`mem.read` 被 GDB 占着时只回了 "read-failed"，看不到底层 rc，只能靠猜。
    void failf(int code, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(errDetail_, sizeof(errDetail_), fmt, ap);
        va_end(ap);
        errCode_ = code;
        errMsg_ = errDetail_;
    }

    bool hasError() const { return errCode_ != 0; }
    int errCode() const { return errCode_; }
    const char* errMsg() const { return errMsg_ ? errMsg_ : "unspecified"; }
    int lineCount() const { return overflow_ ? 0 : lines_; }
    const char* body() const { return buf_; }
    bool overflowed() const { return overflow_; }

private:
    // 值里出现空格会破坏「k=v 之间用空格分列」的约定 —— 客户端会把一个值切成两半
    // （实测后果：方法说明 `help=列出 cpu/gpu/mem 的当前频率` 被截成 `help=列出`）。
    // 规则与逆运算都在上面的 `codec` 里（**入方向和出方向必须成对改**）。
    static void escapeValue(const char* in, char* out, int cap) {
        codec::escapeValue(in, out, cap);
    }

    void appendField(const char* key, const char* fmt, ...) {
        if (overflow_) return;
        char tmp[kArgValueBytes];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);

        static char esc[kArgValueBytes * 3];   // 静态：放置类缓冲别放栈上（主线程栈只有 64KB）
        escapeValue(tmp, esc, static_cast<int>(sizeof(esc)));

        const int need = std::snprintf(nullptr, 0, "%s=%s", key ? key : "", esc) + 1;
        if (len_ + need >= kReplyBytes) { overflow_ = true; return; }
        if (len_ != 0 && buf_[len_ - 1] != '\n') { buf_[len_++] = ' '; }
        len_ += std::snprintf(buf_ + len_, kReplyBytes - len_, "%s=%s", key ? key : "", esc);
    }

    char buf_[kReplyBytes];
    int len_ = 0;
    int lines_ = 1;
    int errCode_ = 0;
    const char* errMsg_ = nullptr;
    char errDetail_[256] = {0};   // ★ 错误文案的**硬上限只有 255 字节**（超了会被 snprintf 截断）：
                                  //   写 `failf` 时把最要紧的信息（口令/参数/该怎么做）放前面，
                                  //   中文一个字 3 字节 ⇒ 大概 60~70 个汉字就到头了。
                                  //   ★ 而且 `fail()` 走 ERR 分支时**之前的 `kv`/`raw` 会被丢掉**
                                  //     （协议里 ERR 不带载荷）⇒ 想传达给调用方的话必须写进这条消息里。
    bool overflow_ = false;
};

// ---------------------------------------------------------------- 工具描述
using Handler = void (*)(const Args&, Reply&);

// ---------------------------------------------------------------- 工具的自描述（2026-10-06 晚新增）
//
// **为什么要有它**：以前 AI 想知道"能不能改存档、该传什么参数"，只能逐个 `help tool=X`
// 去读一行中文；参数、示例、危险提示全散在文档里 —— 而 AI **只能通过协议说话**。
// ⇒ 让每个工具**自己持有自己的知识**（写在自己的 `tool_*.cpp` 里），核心只负责"汇总与取用"。
//
// ★ 归属原则：**每个工具的知识只写在它自己的文件里**，核心一行都不认识任何工具
//   （和"加工具不改核心"是同一条原则）。
//
// ★★ 向后兼容的关键：新字段**都带默认值** ⇒ 现有方法表 `{"id", "说明", cmdId}` 照旧编得过，
//    可以**逐个方法**慢慢补元数据，而不是一次性改 54 处。

// 一个参数：名字 / 类型 / 是否必填 / 默认值 / 说明（说明可为 nullptr）
struct Param {
    const char* name;
    const char* type;      // "str" | "int" | "base64" | "0|1" | "IP" | 枚举…
    bool        required;
    const char* def;       // 默认值（nullptr = 没有默认值）
    const char* desc;      // 一句话说明（可为 nullptr）
};

// 一个方法的"教程"。★ `doc` 是正文，**长度不限**（作用/示例/注意/返回/相关都写在这里）。
struct MethodInfo {
    const Param* params;
    int          paramCount;
    const char*  risk;     // "read" | "write" | "action" | "danger"（nullptr = 未标注）
    const char*  doc;      // 教程正文（多行字符串，`\n` 分行）
};

struct Method {
    const char* name;   // 方法名，调用时拼成 "<工具>.<方法>"
    const char* help;   // 一句话说明
    Handler handler;
    // ★ 新字段带默认值 ⇒ 老写法 `{"id", "…", cmdId}` 不用改就能编过
    const MethodInfo* info = nullptr;
};

struct Tool {
    const char* name;   // 工具名，同时作为命名空间前缀
    const char* help;   // 一句话说明
    const Method* methods;
    int methodCount;
    const char* doc = nullptr;   // ★ 工具级教程（"什么时候用它/和别的工具怎么配合"，长度不限）
};

namespace info {

// 把参数表压成一行、可直接塞进 `k=v` 的摘要：
//     `path*(str),data*(base64),append(0|1=0)`      （`*` = 必填）
// ★ 用**逗号**分隔、值里不含空格 —— 这样不会触发协议的空格转义（`%20`），人能直读。
// 返回写入长度（不含结尾 NUL）；cap 不够时会截断并返回实际长度。
inline int paramSummary(const MethodInfo* mi, char* out, int cap) {
    if (out == nullptr || cap <= 0) return 0;
    out[0] = '\0';
    if (mi == nullptr || mi->params == nullptr) return 0;
    int used = 0;
    for (int i = 0; i < mi->paramCount; ++i) {
        const Param& p = mi->params[i];
        char one[96];
        if (p.def != nullptr && *p.def != '\0') {
            std::snprintf(one, sizeof(one), "%s%s(%s=%s)", used ? "," : "",
                          p.name ? p.name : "?", p.type ? p.type : "?", p.def);
        } else {
            std::snprintf(one, sizeof(one), "%s%s%s(%s)", used ? "," : "",
                          p.name ? p.name : "?", p.required ? "*" : "",
                          p.type ? p.type : "?");
        }
        const int n = std::snprintf(out + used, static_cast<size_t>(cap - used), "%s", one);
        if (n <= 0 || used + n >= cap) { out[cap - 1] = '\0'; return cap - 1; }
        used += n;
    }
    return used;
}

}  // namespace info

// ---------------------------------------------------------------- 平台给工具的服务
// 工具**只依赖这一小块能力**，不依赖核心实现细节 —— 这样核心重构不会打断第三方工具。
namespace host {

// 追加一条到内存环形日志（PC 侧可用 log.dump 取走）。sysmodule 没有 stdout，这是唯一的"打印"。
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 覆盖写「当前进度」到 SD。**崩了就靠它定位**：调用点一定要写在危险动作**之前**。
void breadcrumb(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 本次开机是第几次。
int bootCount();

// 已注册的工具数。
int toolCount();

// 心跳文件路径。
const char* heartbeatPath();

// 把内存环形日志写进应答（最多 maxLines 行，每行以 '#' 打头，可含空格）。
void dumpLog(Reply& reply, int maxLines);

}  // namespace host

// ---------------------------------------------------------------- 通用编解码
// 协议规定 `k=v` 的值不能含空格，所以二进制一律走 base64。
// 这两条是工具层反复要用的，放在 SDK 里免得每个工具各写一份。
namespace util {

// ---------------------------------------------------------------- 整数（含 0x）解析
// ★★★ 为什么必须有它（2026-10-06 晚，一次"工具读不进自己打印的格式"）：
//
//   `Args::getInt()` 是**手写的、只认十进制**：`0x010015100B514000` 会读成 **0**
//   （读掉开头的 `0` 就停在 `x` 上）。而 `save.list` / `title.list` 打印 title id 用的
//   **恰恰是 `0x%016llX`** ⇒ 把工具自己的输出原样喂回去，就被解析成 0：
//     · `save.backup app=0x…` ⇒ 走 `appArg == 0` 分支，报 `missing-app`（**明确报错，还好**）；
//     · `save.list   app=0x…` ⇒ 更坏 —— 在它那里 `0` 是"**不过滤**"的意思，
//       于是它**列出了全部存档**，看起来像"参数生效了"（**假成功**，比报错危险得多）。
//   ⇒ 凡"用户会从别处复制过来的 id"，一律用这个函数，不要用 `getInt`。
//
// 支持 `0x`/`0X` 前缀的十六进制与纯十进制；失败（空串、非法字符、溢出）返回 false。
//
// ★ 类型用 `uint64_t`（`<cstdint>` 已包含）而**不是** libnx 的 `u64` —— 这个头文件
//   刻意不依赖 `<switch.h>`，而 `u64` 只是 `<switch.h>` 里的一个 typedef。
//   两者是**同一个类型**，所以调用方传 `u64*` 完全对得上。
inline bool parseU64(const char* text, uint64_t* out) {
    if (text == nullptr || out == nullptr || *text == '\0') return false;
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }

    const uint64_t kMax = static_cast<uint64_t>(-1);
    uint64_t value = 0;
    bool any = false;
    for (const char* p = text; *p != '\0'; ++p) {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else return false;
        // 溢出检查：原来 `value * base + digit` 直接算，超长输入是**未定义行为**。
        if (value > (kMax - static_cast<uint64_t>(digit)) / static_cast<uint64_t>(base)) return false;
        value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
        any = true;
    }
    if (!any) return false;
    *out = value;
    return true;
}

inline const char* b64Table() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

// 编码 n 个字节到 out（调用方保证 out 至少 n*4/3+8 字节），返回写入长度。
inline int b64Encode(const unsigned char* in, int n, char* out) {
    const char* table = b64Table();
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        const int rem = n - i;
        const unsigned v = (static_cast<unsigned>(in[i]) << 16) |
                           (rem > 1 ? (static_cast<unsigned>(in[i + 1]) << 8) : 0u) |
                           (rem > 2 ? static_cast<unsigned>(in[i + 2]) : 0u);
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        out[o++] = rem > 1 ? table[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? table[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

inline int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// 解码到 out；**遇到任何非法字符、或超出 cap，一律返回 -1**（调用方必须当失败处理）。
//
// ★★★ 2026-10-06 晚改：原来非法字符是 `continue`（**跳过**），后果是最坏的一种失败形态：
//   `fs.write data=!!!bad` 静默写出 2 字节垃圾（`!!!` 被跳过、`bad` 三个合法字符 = 18 bit = 2 字节），
//   而回包是 `OK bytes=2` —— 看不出任何异常。同一份宽松逻辑当时在 `tool_fs.cpp` 里还有一份拷贝，
//   现在两份都统一到这里（`mem.write` 也走这条路 ⇒ 一起修好）。
//
// ★ 这带来一个行为变化：**base64 里夹空白/换行会被拒**。这是有意的 ——
//   协议规定 `k=v` 的值不能含空白（要写 `%20`），我们自己的 `b64Encode` 也不产空白，
//   所以"带空白"只能意味着数据坏了，正是该拒的情况。
inline int b64Decode(const char* in, unsigned char* out, int cap) {
    if (in == nullptr || out == nullptr) return -1;
    int o = 0, buf = 0, bits = 0;
    bool padded = false;
    for (const char* p = in; *p != '\0'; ++p) {
        if (*p == '=') { padded = true; continue; }   // 填充只允许出现在结尾
        const int v = b64Value(*p);
        if (v < 0) return -1;                          // ★ 非法字符 = 坏数据，不再静默跳过
        if (padded) return -1;                         // ★ 填充之后又出现数据，同样是坏数据
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = static_cast<unsigned char>((buf >> bits) & 0xFF);
        }
    }
    // ★ 剩下的位数 ≥6 ⇒ 说明总字符数是 4k+1，**不可能是合法的 base64**
    //   （单字符 `a` 就是这种）。不加这一条，`data=a` 会"成功"解出 0 字节、
    //   回 `OK bytes=0` 并写出一个空文件 —— 又是一次"回 OK 但没做事"。
    if (bits >= 6) return -1;
    return o;
}

}  // namespace util

}  // namespace nxc

// ---------------------------------------------------------------- 注册宏
// 用法（一个工具一个 .cpp，**不需要改核心**）：
//
//   #include "nxc_sdk.hpp"
//   namespace {
//   void cmd_echo(const nxc::Args& a, nxc::Reply& r) { r.kv("echo", a.get("text")); }
//   const nxc::Method kMethods[] = { {"echo", "回显一段文本", cmd_echo} };
//   }
//   NXC_DEFINE_TOOL(nxc_tool_echo, "echo", "示例工具", kMethods);
//
// 关键点：描述符被放进 `nxc_tools` 段，由链接器自动收集，
// 核心在启动时遍历段内全部工具 —— 所以**加工具不需要在核心的注册表里加行**。
#define NXC_TOOL_SECTION __attribute__((used, section("nxc_tools")))

// 带**工具级教程**的写法（`tool_doc` 是一段多行文字，长度不限）
#define NXC_DEFINE_TOOL_EX(var, tool_name, tool_help, method_array, tool_doc)    \
    extern const nxc::Tool var;                                                  \
    const nxc::Tool var NXC_TOOL_SECTION = {                                     \
        tool_name, tool_help, method_array,                                       \
        static_cast<int>(sizeof(method_array) / sizeof((method_array)[0])),       \
        tool_doc                                                                  \
    }

// 不带工具级教程（等价于 tool_doc = nullptr）—— 老写法，一个字都不用改
#define NXC_DEFINE_TOOL(var, tool_name, tool_help, method_array)                 \
    NXC_DEFINE_TOOL_EX(var, tool_name, tool_help, method_array, nullptr)
