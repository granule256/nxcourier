// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

#include "platform.hpp"
#include "sd.hpp"     // ★ 需要 nxc::sd::kEnabled（实验期工具拦截）
#include "net.hpp"    // ★ 需要 nxc::net::kListenEnabled（同理）
#include "power.hpp"  // ★ 需要 nxc::power::*（睡眠时收口，见 core/power.hpp）

#include <switch.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

namespace nxc {

// ★★★ 记录「进 dispatch 那一刻」sm 是否真的活跃（**在 SmGuard 打开之前**）。
//   这是回答"命令之间 sm 关没关"的唯一正确时刻 ——
//   命令执行中读必然为 1（那时 SmGuard 开着），说明不了问题。
//
// ★ 注意命名空间位置：必须放在 nxc::host（**不是** nxc::protocol::<匿名>）里，
//   否则外面链接不到（我第一版就放错了，链接报 undefined reference）。
static int s_smActiveAtEntry = -1;

namespace host {
int smActiveAtDispatchEntry() { return s_smActiveAtEntry; }
}  // namespace host

namespace protocol {

namespace {

// ★ 跟着 kArgValueBytes 一起放大：一条 fs.write 现在可能 4KB 以上，
//   还按 1024 就会被当成"超长行"整条丢掉。每个客户端各持一份（kMaxClients=4 → 32KB）。
constexpr int kRequestBytes = 8192;

// Args(≈13KB) 与 Reply(≈8KB) 加起来超过 16KB 的默认线程栈，所以放成静态的。
// 服务端是**单线程**的（多客户端靠 poll 轮询，见 serveForever），因此不需要加锁。
Args  g_args;
Reply g_reply;

// ---------------------------------------------------------------- 收发
// 注意：**已经没有"一次阻塞读一行"的函数了**。
// 多客户端下不能再阻塞在对端上（一个慢客户端会卡住所有人），改成 poll + 非阻塞收 + 按行切分，
// 行缓冲由每个 ClientSlot 各自持有。见下面的 serveForever。

bool sendAll(int fd, const char* data, int len) {
    int off = 0;
    int stalls = 0;
    while (off < len) {
        const int n = ::send(fd, data + off, len - off, 0);
        if (n > 0) {
            off += n;
            stalls = 0;
            continue;
        }
        // ★ socket 现在是非阻塞的（见 serveForever 里的 O_NONBLOCK），
        //   发送缓冲满时会返回 EAGAIN。这里短暂让出 CPU 再试，别因为非阻塞就把回包丢掉。
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stalls > 300) return false;          // 约 300ms 还发不出去，认输
            svcSleepThread(1000000ULL);                 // 1ms
            continue;
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 解析
// 以空格切词：第一个词是方法名，其余是 k=v；没有 '=' 的词按位置记为 _0/_1/...
// ★ 值的**入方向解码**在这里做（`codec::unescapeValueInPlace`）—— 与出方向的
//   `Reply::escapeValue` 严格对称。少了这一步，设备自己回包里的 `%20` 就喂不回去
//   （见 docs/反馈-参数值含空格无法表达-20261005.md）。
// ★ 解码放在**任何工具看到路径之前**，所以白名单/`..` 检查拿到的都是最终字节 ——
//   顺序反过来（先检查后解码）就能被 `%2E%2E` 绕过。
void parseRequest(char* line, const char*& method, Args& args) {
    args = Args{};

    char* save = nullptr;
    char* tok = ::strtok_r(line, " \t", &save);
    if (tok == nullptr) { method = ""; return; }
    method = tok;

    int positional = 0;
    char keyBuf[16];
    while ((tok = ::strtok_r(nullptr, " \t", &save)) != nullptr) {
        char* eq = std::strchr(tok, '=');
        if (eq != nullptr) {
            *eq = '\0';
            codec::unescapeValueInPlace(eq + 1);
            args.add(tok, eq + 1);
        } else {
            std::snprintf(keyBuf, sizeof(keyBuf), "_%d", positional++);
            args.markPositional();
            args.add(keyBuf, tok);
        }
    }
}

// ---------------------------------------------------------------- 能力发现（2026-10-06 晚新增）
//
// `tools.list` / `tools.doc` —— 让 AI **自己发现**能力，而不是靠人告诉它、也不是把工具清单
// 写死在 PC 侧。放在核心的理由：它们是**关于"工具"本身的元命令**，必须永远存在。
//
// ★ 核心在这里只做"汇总与取用"：它**不认识** fs/tele/mem 是什么 —— 全部知识来自
//   每个工具自己文件里的 `MethodInfo` / `Tool::doc`（归属原则见 nxc_sdk.hpp）。
namespace {

// 把一个方法写成一行（结构化 k=v）。参数用逗号分隔 ⇒ 值里没有空格 ⇒ 不会被转义成 %20，
// 人和 AI 都能直读。
void emitMethodLine(Reply& reply, const Tool* t, const Method& m) {
    char full[48];
    std::snprintf(full, sizeof(full), "%s.%s", t->name, m.name);
    char params[256];
    info::paramSummary(m.info, params, sizeof(params));

    reply.next();
    reply.kv("method", full);
    reply.kv("help", m.help);
    if (m.info != nullptr && m.info->risk != nullptr) reply.kv("risk", m.info->risk);
    if (params[0] != '\0') reply.kv("params", params);
}

// 教程正文的拼装缓冲。★ 静态：不占栈（主线程栈只有 64KB），一次只有一条命令在用。
//   ★ 每行**不带** `#` 前缀 —— 发出去时由 `Reply::raw()` 统一加，避免出现 `##`。
char g_docBuf[12288];
int  g_docLen = 0;

void docPut(const char* text) {
    if (g_docLen >= static_cast<int>(sizeof(g_docBuf)) - 1) return;
    const int n = std::snprintf(g_docBuf + g_docLen,
                                static_cast<size_t>(sizeof(g_docBuf) - g_docLen), "%s\n",
                                text ? text : "");
    if (n > 0) g_docLen += n;
    if (g_docLen > static_cast<int>(sizeof(g_docBuf)) - 1) g_docLen = sizeof(g_docBuf) - 1;
}

// 把一行自由文本加进教程（超长截断，并留一句说明）
void docPutLine(const char* prefix, const char* body) {
    char line[352];
    const int n = std::snprintf(line, sizeof(line), "%s%.300s", prefix ? prefix : "",
                                body ? body : "");
    if (n >= static_cast<int>(sizeof(line))) docPut("…（这一行太长，已截断）");
    docPut(line);
}

// 单条方法的完整教程：帮助 → 参数表 → 作者写的正文
void buildMethodDoc(const Tool* t, const Method& m) {
    char head[192];
    std::snprintf(head, sizeof(head), "name=%s.%s  tool=%s  risk=%s", t->name, m.name, t->name,
                  (m.info != nullptr && m.info->risk != nullptr) ? m.info->risk : "未标注");
    docPut(head);
    docPutLine("帮助  ", m.help);
    if (m.info == nullptr) {
        docPut("★ 这个方法还没有教程（工具作者可以在自己的文件里补 MethodInfo）");
        return;
    }
    if (m.info->params != nullptr && m.info->paramCount > 0) {
        docPut("参数");
        for (int i = 0; i < m.info->paramCount; ++i) {
            const Param& p = m.info->params[i];
            char one[224];
            std::snprintf(one, sizeof(one), "  %s%s  %s%s%s%s", p.name ? p.name : "?",
                          p.required ? "（必填）" : "", p.type ? p.type : "?",
                          p.def ? "  默认=" : "", p.def ? p.def : "", "");
            docPut(one);
            if (p.desc != nullptr && *p.desc != '\0') docPutLine("      ", p.desc);
        }
    }
    docPut("教程");
    const char* p = m.info->doc;
    while (p != nullptr && *p != '\0') {
        const char* nl = std::strchr(p, '\n');
        int len = nl ? static_cast<int>(nl - p) : static_cast<int>(std::strlen(p));
        if (len > 300) len = 300;          // ★ 单行上限；上面 docPutLine 会提示截断语义
        char body[320];
        std::memcpy(body, p, static_cast<size_t>(len));
        body[len] = '\0';
        docPut(body);
        if (nl == nullptr) break;
        p = nl + 1;
    }
}

}  // namespace

// ---------------------------------------------------------------- 核心内建
// 这几条由核心直接处理，不属于任何工具（所以它们永远存在）。
bool handleBuiltin(const char* method, const Args& args, Reply& reply) {
    if (std::strcmp(method, "hello") == 0) {
        reply.kv("platform", "nxc");
        reply.kv("proto", "1");
        reply.kvi("boot", log::bootCount());
        reply.kvf("tools", "%d", reg::toolCount());
        return true;
    }

    if (std::strcmp(method, "caps") == 0) {
        const int n = reg::toolCount();
        reply.kvi("count", n);
        for (int i = 0; i < n; ++i) {
            const Tool* t = reg::toolAt(i);
            reply.next();
            reply.kv("tool", t->name);
            reply.kvi("methods", t->methodCount);
            reply.kv("help", t->help);
        }
        return true;
    }

    // ★★ 能力发现之一：**一条命令拿到全部工具 + 每个方法的使用摘要**
    //   这就是"直接列表发现"：不用逐个 help，也不用把工具清单写死在 PC 侧。
    if (std::strcmp(method, "tools.list") == 0) {
        const char* only = args.get("tool");
        if (only == nullptr) only = args.get("_0");
        const Tool* onlyTool = nullptr;
        if (only != nullptr && *only != '\0') {
            onlyTool = reg::findTool(only, static_cast<int>(std::strlen(only)));
            if (onlyTool == nullptr) {
                reply.failf(404, "no-such-tool: %s（不带参数调 tools.list 看全部工具）", only);
                return true;
            }
        }
        const int n = reg::toolCount();
        int toolN = 0, methodN = 0;
        for (int i = 0; i < n; ++i) {
            const Tool* t = reg::toolAt(i);
            if (onlyTool != nullptr && t != onlyTool) continue;
            ++toolN;
            methodN += t->methodCount;
        }
        reply.kvi("count", toolN);
        reply.kvi("methods", methodN);
        for (int i = 0; i < n; ++i) {
            const Tool* t = reg::toolAt(i);
            if (onlyTool != nullptr && t != onlyTool) continue;
            reply.next();
            reply.kv("tool", t->name);
            reply.kvi("methods", t->methodCount);
            reply.kv("help", t->help);
            if (t->doc != nullptr) reply.kv("tool_doc", "1");
            for (int j = 0; j < t->methodCount; ++j) emitMethodLine(reply, t, t->methods[j]);
        }
        reply.next();
        reply.raw("risk: read=只读 / write=会写 / action=会改设备状态（多为需口令的动作）/ danger=可能搞死设备");
        reply.raw("hint: tools.doc name=<工具[.方法]> 看完整教程（参数/示例/注意/返回/相关）");
        return true;
    }

    // ★★ 能力发现之二：取某个工具/方法的**完整教程**（分页）
    if (std::strcmp(method, "tools.doc") == 0) {
        const char* name = args.get("name");
        if (name == nullptr) name = args.get("_0");
        if (name == nullptr || *name == '\0') {
            reply.fail(400, "missing-name: 需要 name=<工具> 或 name=<工具.方法>（不带参数调 tools.list 看全部）");
            return true;
        }
        long long offset = args.getInt("offset", 0);
        if (offset < 0) offset = 0;
        long long want = args.getInt("lines", 60);
        if (want <= 0 || want > 200) want = 60;

        char toolName[32] = {0};
        const char* dot = std::strchr(name, '.');
        const int toolLen = dot ? static_cast<int>(dot - name) : static_cast<int>(std::strlen(name));
        if (toolLen <= 0 || toolLen >= static_cast<int>(sizeof(toolName))) {
            reply.fail(400, "bad-name: 工具名太长或为空");
            return true;
        }
        std::snprintf(toolName, sizeof(toolName), "%.*s", toolLen, name);
        const Tool* t = reg::findTool(toolName, toolLen);
        if (t == nullptr) {
            reply.failf(404, "no-such-tool: %s（不带参数调 tools.list 看全部工具）", toolName);
            return true;
        }

        g_docLen = 0;
        if (dot != nullptr) {
            const char* sub = dot + 1;
            const Method* m = nullptr;
            for (int j = 0; j < t->methodCount; ++j) {
                if (std::strcmp(t->methods[j].name, sub) == 0) { m = &t->methods[j]; break; }
            }
            if (m == nullptr) {
                reply.failf(404, "no-such-method: %s（用 tools.list tool=%s 看它有哪些方法）", name, t->name);
                return true;
            }
            buildMethodDoc(t, *m);
        } else {
            char head[128];
            std::snprintf(head, sizeof(head), "name=%s  methods=%d", t->name, t->methodCount);
            docPut(head);
            docPutLine("帮助  ", t->help);
            if (t->doc != nullptr) docPut(t->doc);
            docPut("方法清单（要单个方法的教程：tools.doc name=<工具.方法>）");
            for (int j = 0; j < t->methodCount; ++j) {
                char one[224];
                std::snprintf(one, sizeof(one), "  %s.%s  %s", t->name, t->methods[j].name,
                              t->methods[j].help ? t->methods[j].help : "");
                docPut(one);
            }
        }

        int total = 0;
        for (int i = 0; i < g_docLen; ++i) {
            if (g_docBuf[i] == '\n') ++total;
        }
        reply.kvi("total_lines", total);
        reply.kvi("offset", offset);
        int idx = 0, shown = 0;
        const char* p = g_docBuf;
        while (p != nullptr && *p != '\0') {
            const char* nl = std::strchr(p, '\n');
            if (nl == nullptr) break;
            if (idx >= offset) {
                if (shown >= want) break;
                int len = static_cast<int>(nl - p);
                if (len > 300) len = 300;
                char body[320];
                std::memcpy(body, p, static_cast<size_t>(len));
                body[len] = '\0';
                reply.raw(body);
                ++shown;
            }
            p = nl + 1;
            ++idx;
        }
        reply.kvi("shown", shown);
        reply.kvi("next_offset", offset + shown);
        reply.kvi("eof", (offset + shown) >= total ? 1 : 0);
        return true;
    }

    if (std::strcmp(method, "help") == 0) {
        const char* toolName = args.get("tool");
        if (toolName == nullptr) toolName = args.get("_0");
        if (toolName == nullptr) {
            // 不带参数：给一句用法，别让人猜。
            reply.kv("usage", "help tool=<工具名>");
            reply.kv("list", "用 caps 列出全部工具");
            return true;
        }
        const Tool* t = reg::findTool(toolName, static_cast<int>(std::strlen(toolName)));
        if (t == nullptr) {
            reply.fail(404, "no-such-tool");
            return true;
        }
        reply.kv("tool", t->name);
        reply.kv("help", t->help);
        reply.kvi("count", t->methodCount);
        for (int i = 0; i < t->methodCount; ++i) {
            reply.next();
            reply.kv("method", t->methods[i].name);
            reply.kv("help", t->methods[i].help);
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------- 分发
// ★ sm 的作用域守卫：进命令开、出命令关（幂等，失败也不拦命令 ——
//   不需要 sm 的命令照常能跑，需要 sm 的会让它自己的 xxxInitialize 报错）。
namespace {
struct SmGuard {
    bool opened;
    SmGuard() : opened(host::smOpen()) {}
    ~SmGuard() { if (opened) host::smClose(); }
};
}  // namespace

void dispatch(const char* method, const Args& args, Reply& reply) {
    s_smActiveAtEntry = serviceIsActive(smGetServiceSession()) ? 1 : 0;

    // ★★★ 按需开 sm：**睡眠时我们不持有它**（这是本次改动的目标）。
    SmGuard smGuard;

    if (handleBuiltin(method, args, reply)) return;

    const char* dot = std::strchr(method, '.');
    if (dot == nullptr) {
        reply.fail(400, "method-must-be-tool.method");
        return;
    }

    const Tool* tool = reg::findTool(method, static_cast<int>(dot - method));
    if (tool == nullptr) {
        reply.fail(404, "no-such-tool");
        return;
    }

    // ★★★ 2026-10-06：**被实验摘掉的能力，对应工具必须在唯一入口一起挡掉。**
    //
    //   为什么必须在**唯一入口**挡，而不是逐个补安全包装：
    //     · `fs.ls`  用的是**裸 `opendir` / `readdir` / `::stat`**；
    //       `fs.mkdir` / `fs.rm` 用**裸 `::mkdir` / `::remove`**；
    //     · `save.*` 直接调 **`fsOpenSaveDataInfoReader` / `fsOpenSaveDataFileSystem`**
    //       —— 那需要 `fs` 服务会话；
    //     · `net.info` / `net.probe` / `net.deps` 直接调 **`::socket()` / `::connect()`**
    //       —— 那需要 `socketInitialize` 建好的 bsd 子系统。
    //   ★ 这些在**能力被摘掉时不是"返回错误"，而是【崩溃】**
    //     （2026-10-05 就是这个形态把主机搞到进不去系统，要靠拔卡救）。
    //   ⇒ 逐个补包装 = 漏一个就是地雷；在唯一入口一刀切 ⇒ 任何漏网调用点都够不着。
    if (!nxc::net::networkUp() && std::strcmp(tool->name, "net") == 0) {
        reply.fail(503, "tool-disabled-net-off");
        return;
    }
    if (!nxc::sd::kEnabled &&
        (std::strcmp(tool->name, "fs") == 0 || std::strcmp(tool->name, "save") == 0)) {
        reply.fail(503, "tool-disabled-sd-off");
        return;
    }

    const char* sub = dot + 1;
    for (int i = 0; i < tool->methodCount; ++i) {
        if (std::strcmp(tool->methods[i].name, sub) == 0) {
            tool->methods[i].handler(args, reply);
            if (reply.overflowed()) reply.fail(500, "reply-too-large");
            return;
        }
    }
    reply.fail(404, "no-such-method");
}

// ---------------------------------------------------------------- 回包
bool sendReply(int fd, Reply& reply) {
    // ★★★ 2026-10-06 晚：这个缓冲从 64 提到 384，并且**把长度夹住**。
    //
    //   原来 64 字节装不下 `ERR 400 missing-app: 需要 app=<title id，如 0x010021C000B6A000>\n`
    //   （实测这条是 68 字节），于是同时犯两个错：
    //     ① 只写进 63 字节 + NUL ⇒ 结尾的 UTF-8 汉字被切一半，PC 侧显示成乱码；
    //     ② ★★ `snprintf` 返回的是「**本该写多少**」(68)，而代码把它当发送长度用了 ⇒
    //        `sendAll` 从 64 字节的数组里**多读 4 字节栈垃圾**发出去（越界读），
    //        并且第 67 字节的 `\n` 根本不在缓冲里 ⇒ **错误行结尾丢掉换行**
    //        （客户端要靠后续/超时才收帧，表现为莫名的卡顿）。
    //
    //   夹取长度之后：超长文案依旧会被截断（可接受的降级），但**绝不越界读、也一定有换行**。
    char header[384];
    constexpr int kHeaderCap = static_cast<int>(sizeof(header));

    if (reply.hasError()) {
        int n = std::snprintf(header, sizeof(header), "ERR %d %s\n",
                              reply.errCode(), reply.errMsg());
        if (n < 0) return false;
        if (n > kHeaderCap - 1) n = kHeaderCap - 1;
        return sendAll(fd, header, n);
    }

    const bool hasBody = (reply.body()[0] != '\0');
    const int lines = hasBody ? reply.lineCount() : 0;
    const int n = std::snprintf(header, sizeof(header), "OK %d\n", lines);
    if (!sendAll(fd, header, n)) return false;
    if (!hasBody) return true;

    if (!sendAll(fd, reply.body(), static_cast<int>(std::strlen(reply.body())))) return false;
    return sendAll(fd, "\n", 1);
}

}  // namespace

namespace {

// ---------------------------------------------------------------- 多客户端
// ★ 为什么必须支持多客户端：原来的写法是「accept 一个 → 阻塞在 serveClient 里读到断开」，
//   于是**先连上的客户端独占平台**。而 MCP 网关持的是长连接 —— 它一连上，
//   命令行/脚本就再也挤不进来（实测表现：TCP 能建立，一发数据就被 RST）。
//   现在改成 poll() 轮询，多个客户端各自带一个行缓冲，互不阻塞。
constexpr int kMaxClients = 4;

struct ClientSlot {
    int fd = -1;
    char line[kRequestBytes];
    int len = 0;
    bool dropping = false;
};

ClientSlot g_clients[kMaxClients];

// 处理一条完整的请求行；返回 false 表示这条连接该关掉了。
bool handleLine(int fd, char* line, bool tooLong) {
    const char* method = "";
    parseRequest(line, method, g_args);

    g_reply.reset();
    if (tooLong) {
        g_reply.fail(413, "request-too-long");
    } else if (std::strcmp(method, "__TOO_LONG__") == 0) {
        g_reply.fail(413, "request-too-long");
    } else if (*method == '\0') {
        g_reply.fail(400, "empty-request");
    } else {
        dispatch(method, g_args, g_reply);

        // ★ 诊断增强（2026-10-05，来自一次真实的误判）：
        //   请求里出现「没有 `=` 的词」时，最可能的原因就是**值里有空格** ——
        //   典型的是一位带空格的目录名（卡上真的有：`visible tinted rocks_3002235272`），
        //   或者人工手打的整串 `confirm=I-KNOW-...`（漏了下划线之类）被切成两半。
        //   ★ 为什么必须提示：这类失败原来长得像"文件不存在"（`ERR 404 not-found`），
        //     会让人得出"卡上根本没这个文件"的错误结论（反馈单就是被这个形态绊的）。
        //   ★ 只在**已经失败**时补这一句：成功回包一个字都不变。
        //   ★ 触发条件再加一层"同时还有 k=v"：`help probe` 这种**本来就合法**的
        //     位置参数用法（没带 `=` 的词）不该被唠叨；而"值被空格切碎"必然留下一个
        //     带 `=` 的前缀（`path=/a`）＋后面的碎片。
        //   ★ 注意 `%%20`：`failf` 走的是 printf 格式串，写 `%20` 会被当成格式说明符。
        if (g_reply.hasError() && g_args.positionalCount() > 0 &&
            g_args.count() > g_args.positionalCount()) {
            char keep[192];
            std::snprintf(keep, sizeof(keep), "%s", g_reply.errMsg());
            g_reply.failf(g_reply.errCode(),
                          "%s (hint: 请求里有 %d 个没带 '=' 的词 —— 值里的空格要写成 %%20)",
                          keep, g_args.positionalCount());
        }
    }

    if (!sendReply(fd, g_reply)) return false;
    log::ring("req %s", method);
    return true;
}

// 当前还开着的客户端数（`acceptonly` 用它判断"能不能把监听口开回来"）。
int activeClientCount() {
    int n = 0;
    for (int i = 0; i < kMaxClients; ++i) {
        if (g_clients[i].fd >= 0) ++n;
    }
    return n;
}

// ★ 备注（2026-10-06）：这里曾经有一层 `transportPoll()` 转发 —— 为了在"照抄 SysDVR 自带
//   socket 内存池"的模式下改用 `bsdPoll`（因为那个模式绕开 `socketInitialize`，
//   libnx 的 `::poll` 会因没有 "soc" devoptab 而失败）。
//   ★ 那条路已放弃（libnx **不导出** `bsdInitialize`/`bsdExit`）⇒ 这层转发也撤掉了，
//     一律用 libnx 的 `::poll`。

}  // namespace

// ---------------------------------------------------------------- 监听口
// ★ 注意：**必须定义在 `nxc::protocol` 这一层**（不能在匿名命名空间里另起一份）——
//   否则和 `platform.hpp` 里那份声明撞名，调用点会报 ambiguous（编译期就炸了，这是好事）。
// 建监听口：socket + SO_REUSEADDR + bind(47800) + listen。成功返回 fd，失败返回 -1。
// ★ 抽出来是为了让 `acceptonly` 模式能在"客户端全断开"时**把监听口开回来**。
int openListener() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // ★ 照抄验证（`likedmnt2` 模式）：Atmosphère 的 dmnt.gen2 **不设** `SO_REUSEADDR`。
    //   其余模式保持原样（我们一直设着）。
    if (nxc::net::useReuseAddr()) {
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(nxc::net::kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log::ring("net: bind %u failed (端口被占用？)", nxc::net::kPort);
        ::close(fd);
        return -1;
    }
    // backlog：`likedmnt2` 模式照抄 dmnt.gen2 的 1，其余给足 8（多客户端：
    // MCP 长连接 + 命令行要能同时挤进来）。
    const int backlog = nxc::net::listenerBacklog();
    if (::listen(fd, backlog) != 0) {
        log::ring("net: listen failed");
        ::close(fd);
        return -1;
    }
    log::ring("net: listener fd=%d mode=%s svc=%s backlog=%d reuseaddr=%d",
              fd, nxc::net::modeName(nxc::net::mode()),
              nxc::net::systemSocketService() ? "bsd:s" : "bsd:u",
              backlog, nxc::net::useReuseAddr() ? 1 : 0);
    return fd;
}

void serveForever(int listenFd, bool keepListenerOpen) {
    pollfd fds[kMaxClients + 1];
    int slots[kMaxClients + 1];
    int listener = listenFd;          // -1 ⇒ 现在没有监听口
    log::ring("net: serveForever listen=%d keepOpen=%d", listener, keepListenerOpen ? 1 : 0);

    for (;;) {
        // ★★★ 2026-10-06 方案 A（照抄 sys-con 的 PSC 做法）：系统要睡了 ⇒ **把全部 socket 关掉**。
        //   为什么非要这样：一整天的对照实验把规律钉死了 ——
        //     `bdsonly`（没有任何 socket）**不崩**；`listen`（监听口）**崩**；
        //     `acceptonly`（监听口已关、**只留一条已建立的连接**）**也崩**
        //     ⇒ ★ **睡眠那一刻只要有【任何】打开的 socket 就崩**（`0x2A5` = SPSM「派发超时」）。
        //   ★ 纪律 1：**关干净之后才让通知线程去应答**（应答早了 = 等于没关）。
        //   ★ 纪律 2：**fd 只在这一处（主线程）关**，通知线程只碰标志位（见 `core/power.hpp`）。
        if (nxc::power::sleepRequested()) {
            // ★★★ 先写黑匣子（**由主线程写 SD**：通知线程不许碰文件，见 power.hpp 纪律 3）。
            //   为什么必须在关 socket **之前**写：这一段就是"我们确实收到了 ReadySleep"
            //   的唯一幸存证据 —— 如果关了 socket 之后崩机，环形日志会随进程一起消失，
            //   只剩这个按开机号命名的文件。
            log::watch("PSC 睡眠 state=%d → 先记一笔再关 socket（events=%d）",
                       nxc::power::lastState(), nxc::power::eventCount());

            if (listener >= 0) { ::close(listener); listener = -1; }
            for (int i = 0; i < kMaxClients; ++i) {
                if (g_clients[i].fd >= 0) { ::close(g_clients[i].fd); g_clients[i].fd = -1; }
                g_clients[i].len = 0;
                g_clients[i].dropping = false;
            }
            // ★ 关干净了，通知线程这才去 Ack（它最多等 900ms）。
            nxc::power::reportSocketsClosed();
            log::ring("power: 全部 socket 已关闭（睡眠），已回报通知线程");

            while (nxc::power::sleepRequested()) svcSleepThread(50000000ULL);   // 50ms 一轮，醒来反应快
            nxc::power::consumeWoke();

            // ★ 醒来重开监听口。开不上不致命 —— 下一轮循环会用同一条重开逻辑再试。
            listener = openListener();
            log::ring("power: 醒来，listener fd=%d", listener);
            log::watch("PSC 醒来 state=%d → 重开监听口 fd=%d（mode=%s）",
                       nxc::power::lastState(), listener, nxc::net::modeName(nxc::net::mode()));
            continue;
        }

        // ★★★ 监听口不在就开回来 —— 两种情况都走这里：
        //   · `acceptonly` 模式：连上第一个客户端时把监听口收掉了（客户端全断就该开回来）；
        //   · **PSC 收口**：睡眠时关掉了，醒来重开（`openListener` 在 `continue` 之前已经试过一次，
        //     这里是不成功时的重试，也是 `keepListenerOpen=true` 各模式唯一的重开路径）。
        //   ★ 原来这个条件里带了 `!keepListenerOpen` ⇒ **`listen` 模式下醒来若第一次重开失败，
        //     就再也不会重开**（端口永久消失、看起来像"模块挂了"）。现在去掉那个限制。
        if (listener < 0 && activeClientCount() == 0) {
            listener = openListener();
            log::ring("net: listener reopened fd=%d", listener);
        }

        int n = 0;
        if (listener >= 0) {
            fds[n].fd = listener;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            slots[n] = -1;
            ++n;
        }

        for (int i = 0; i < kMaxClients; ++i) {
            if (g_clients[i].fd < 0) continue;
            fds[n].fd = g_clients[i].fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            slots[n] = i;
            ++n;
        }

        // ★ 一个可轮询的对象都没有（既没监听口也没客户端，而且刚才重开监听口还失败了）
        //   ⇒ **绝不能拿 n=0 去调 `poll(..., -1)`**：那会**永久阻塞**，模块从此不再响应。
        //   歇一秒重来（下一轮会再试着重开监听口）。
        if (n == 0) {
            svcSleepThread(1000000000ULL);
            continue;
        }

        // ★★ 2026-10-06：把 poll 超时从 `-1`（永久阻塞）改成 **30 秒**，并在每次超时时写一条
        //   **黑匣子心跳**（`log::watch`）。为什么值得：
        //     · 老文档里"我们死在睡眠前/中/后"一直答不出来 —— 因为没有"我们活着"的记录；
        //     · 心跳里同时有**系统 tick 与墙钟** ⇒ 睡眠时 tick 停、墙钟继续走，
        //       **两者差值一跳就是"确实睡过"的铁证**，而且**崩机重启也不会冲掉**（文件名带开机号）。
        //   ★ 会不会因此改变崩溃行为？不会 —— **"周期唤醒"这条早就被排除过**：
        //     D 轮把 poll 改成永久阻塞之后**仍然崩**（见 docs/改动日志-平台.md）。
        //     而且现在的崩因是"持有监听口"本身，与我们醒不醒无关。
        // ★★ 超时从 30 秒 → 250ms → **100ms**：为了**尽快看到"要睡了"的标志**。
        //   为什么还要再压：通知线程收到 `ReadySleep` 后**最多只等 900ms** 才 Ack
        //   （见 power.hpp 纪律 1 —— 绝不能让 PSC 等我们等到超时）。
        //   ⇒ 主线程必须在这个预算内响应，100ms 的轮询周期留足了余量。
        //   ★ 这**不引入新变量**："周期唤醒"这条早就被排除过（D 轮改成永久阻塞后仍然崩）。
        //   ★ 心跳仍按 ~30 秒的节奏写：100ms × 300。
        const int pollMs = 100;
        const int ready = ::poll(fds, static_cast<nfds_t>(n), pollMs);
        if (ready == 0) {
            static int s_servingTick = 0;
            if (++s_servingTick % 300 == 0) {   // ~30 秒一条
                log::watch("serving tick=%d (mode=%s, clients=%d, listener=%d, psc_events=%d, psc_sleep=%d)",
                           s_servingTick / 300, nxc::net::modeName(nxc::net::mode()),
                           activeClientCount(), listener >= 0 ? 1 : 0,
                           nxc::power::eventCount(), nxc::power::sleepEventCount());
            }
            continue;
        }
        if (ready < 0) continue;

        for (int k = 0; k < n; ++k) {
            if ((fds[k].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;

            // ---- 新连接
            if (slots[k] < 0) {
                const int fd = ::accept(listener, nullptr, nullptr);
                if (fd < 0) continue;

                int slot = -1;
                for (int i = 0; i < kMaxClients; ++i) {
                    if (g_clients[i].fd < 0) { slot = i; break; }
                }
                if (slot < 0) {
                    // 满了就明确拒绝 —— 让对端立刻拿到原因，而不是干等超时。
                    static const char kBusy[] = "ERR 503 server-busy\n";
                    sendAll(fd, kBusy, static_cast<int>(sizeof(kBusy) - 1));
                    ::close(fd);
                    continue;
                }
                g_clients[slot].fd = fd;
                g_clients[slot].len = 0;
                g_clients[slot].dropping = false;

                // 顺手试着设成非阻塞 —— 但**不依赖它**。
                //   实测 libnx 上 fcntl(O_NONBLOCK) 与 MSG_DONTWAIT 都不可靠，
                //   所以读取侧改成"poll 说可读才读、每次只读一块"（见下面的读分支），
                //   即使这里没生效也不会阻塞。
                const int flags = ::fcntl(fd, F_GETFL, 0);
                if (flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                    log::ring("note: O_NONBLOCK not supported, relying on poll");
                }

                log::ring("client connected slot=%d", slot);

                // ★★★ `acceptonly` 模式：**连上第一个客户端就把监听口收掉**。
                //   要验证的假设是"元凶是处于 `listen` 状态的那个口，而已建立的连接是无辜的"
                //   ⇒ 若这一版不崩，修法就成立（MCP 的长连接天然就是这个形态：
                //     它连上之后就一直握着，很少需要重连）。
                if (!keepListenerOpen) {
                    ::close(listener);
                    listener = -1;
                    log::ring("net: listener closed (acceptonly) — 只留已建立连接");
                }
                continue;
            }

            // ---- 已有连接可读
            //
            // ★★ 这里**只能读一块，绝不能在 recv 上再套循环**。
            //   踩过的坑：想"一次把已到达的数据读干净"，于是写成 `for(;;) { recv(...); }` 直到 EAGAIN。
            //   但那要求 socket 真的是非阻塞的 —— 而实测 libnx 上 **`fcntl(O_NONBLOCK)` 与
            //   `MSG_DONTWAIT` 都不可靠**，socket 实际仍是阻塞的。于是第二次 recv 把整个服务卡住，
            //   再也回不到 accept（现象：MCP 一连上，命令行就再也进不来）。
            //   现在的做法最稳：**poll 说可读才读，每次只读一块**。没读完的数据会立刻让 poll 再次就绪。
            const int slot = slots[k];
            char buf[256];
            bool closed = false;

            const int got = ::recv(g_clients[slot].fd, buf, sizeof(buf), 0);
            if (got == 0) {
                closed = true;                                  // 对端正常关闭
            } else if (got < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) closed = true;
            } else {
                for (int i = 0; i < got && !closed; ++i) {
                    const char c = buf[i];
                    if (c == '\n') {
                        g_clients[slot].line[g_clients[slot].len] = '\0';
                        const bool tooLong = g_clients[slot].dropping;
                        g_clients[slot].len = 0;
                        g_clients[slot].dropping = false;
                        if (!handleLine(g_clients[slot].fd, g_clients[slot].line, tooLong)) {
                            closed = true;
                        }
                    } else if (c == '\r') {
                        // 忽略，容忍 CRLF
                    } else if (g_clients[slot].len + 1 >= kRequestBytes) {
                        g_clients[slot].dropping = true;  // 超长：丢掉多余，但把这一行读完
                    } else {
                        g_clients[slot].line[g_clients[slot].len++] = c;
                    }
                }
            }

            if (closed) {
                log::ring("client gone slot=%d", slot);
                ::close(g_clients[slot].fd);
                g_clients[slot].fd = -1;
                g_clients[slot].len = 0;
                g_clients[slot].dropping = false;
            }
        }
    }
}

}  // namespace protocol
}  // namespace nxc
