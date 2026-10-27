// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

#include "platform.hpp"
#include "sd.hpp"     // ★ 需要 nxc::sd::kEnabled（实验期工具拦截）
#include "net.hpp"
#include "jobs.hpp"
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

// 参数与应答仅由串行工作线程使用，放在静态区避免挤占线程栈。
Args  g_args;
Reply g_reply;
using WorkQueue = jobs::Queue<8, kRequestBytes, kReplyBytes + 512>;
WorkQueue g_work;
std::atomic<int> g_networkOperations{0};
WorkQueue::Job* g_currentJob = nullptr; // 仅工作线程访问
Thread g_worker;
UEvent g_workReady;
alignas(0x1000) unsigned char g_workerStack[0x20000];

// ---------------------------------------------------------------- 收发
// 注意：**已经没有"一次阻塞读一行"的函数了**。
// 多客户端下不能再阻塞在对端上（一个慢客户端会卡住所有人），改成 poll + 非阻塞收 + 按行切分，
// 行缓冲由每个 ClientSlot 各自持有。见下面的 serveForever。

bool sendAll(int fd, const char* data, int len) {
    int off = 0;
    int interrupts = 0;
    while (off < len) {
        const int n = ::send(fd, data + off, len - off, 0);
        if (n > 0) {
            off += n;
            interrupts = 0;
            continue;
        }
        // 快速应答不等待慢客户端；完整工具应答由 flushResponses 分步发送。
        if (n < 0 && errno == EINTR && ++interrupts < 3) continue;
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
            // C 字符串不能承载嵌入 NUL，不能把路径后半段静默丢掉。
            for (const char* v = eq + 1; *v; ++v)
                if (v[0] == '%' && v[1] == '0' && v[2] == '0') args.reject();
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

    // 禁用SD时，统一拒绝需要挂载和fs会话的工具，避免底层裸调用崩溃。
    if (!nxc::sd::kEnabled &&
        (std::strcmp(tool->name, "fs") == 0 || std::strcmp(tool->name, "save") == 0)) {
        reply.fail(503, "tool-disabled-sd-off");
        return;
    }

    const char* sub = dot + 1;
    for (int i = 0; i < tool->methodCount; ++i) {
        if (std::strcmp(tool->methods[i].name, sub) == 0) {
            const Method& m = tool->methods[i];
            if (m.info != nullptr) {
                for (int j = 0; j < m.info->paramCount; ++j) {
                    const Param& p = m.info->params[j];
                    if (p.name == nullptr || p.type == nullptr || args.get(p.name) == nullptr) continue;
                    if (std::strcmp(p.type, "int") == 0 || std::strcmp(p.type, "0|1") == 0) {
                        const auto v = args.getInt(p.name, 0);
                        if (args.invalid() || (std::strcmp(p.type, "0|1") == 0 && v != 0 && v != 1)) {
                            reply.failf(400, "invalid-integer: %s", p.name); return;
                        }
                    }
                }
            }
            if (host::operationCancelled()) { reply.fail(409, "operation-cancelled"); return; }
            m.handler(args, reply);
            if (args.invalid()) reply.fail(400, "invalid-parameters");
            if (reply.overflowed()) reply.fail(500, "reply-too-large");
            return;
        }
    }
    reply.fail(404, "no-such-method");
}

// ---------------------------------------------------------------- 回包
void encodeReply(WorkQueue::Job& job, Reply& reply) {
    int n = 0;
    if (reply.hasError()) {
        n = std::snprintf(job.output, sizeof(job.output), "ERR %d %s\n", reply.errCode(), reply.errMsg());
    } else {
        const bool body = reply.body()[0] != '\0';
        n = std::snprintf(job.output, sizeof(job.output), "OK %d\n%s%s",
                          body ? reply.lineCount() : 0, reply.body(), body ? "\n" : "");
    }
    if (n < 0 || n >= static_cast<int>(sizeof(job.output))) {
        std::strcpy(job.output, "ERR 500 reply-too-large\n");
        n = static_cast<int>(std::strlen(job.output));
    }
    job.outputBytes = n;
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
    uint64_t generation = 0;
    bool readClosed = false;
};

ClientSlot g_clients[kMaxClients];

// 工作线程处理完整请求，不接触socket。
void processLine(char* line, bool tooLong) {
    const char* method = "";
    parseRequest(line, method, g_args);

    g_reply.reset();
    if (tooLong) {
        g_reply.fail(413, "request-too-long");
    } else if (g_args.invalid()) {
        g_reply.fail(400, "invalid-or-too-large-parameters");
    } else if (std::strcmp(method, "__TOO_LONG__") == 0) {
        g_reply.fail(413, "request-too-long");
    } else if (*method == '\0') {
        g_reply.fail(400, "empty-request");
    } else {
        dispatch(method, g_args, g_reply);
        if (g_args.invalid()) g_reply.fail(400, "invalid-parameters");

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

    log::ring("req %s", method);
}

// 系统服务和工具状态仅由这个工作线程串行访问；网络线程随时可以收口。
void commandWorker(void*) {
    for (;;) {
        WorkQueue::Job* job = g_work.take();
        if (job == nullptr) {
            // 持久心跳由唯一访问SD/sm的工作线程写，网络线程始终不做磁盘IO。
            const Result rc = waitSingle(waiterForUEvent(&g_workReady), 30000000000ULL);
            if (R_FAILED(rc) && !power::sleepRequested())
                log::watch("worker idle (psc_events=%d, psc_sleep=%d)", power::eventCount(), power::sleepEventCount());
            continue;
        }
        g_currentJob = job;
        if (job->cancelled.load(std::memory_order_acquire) || power::sleepRequested()) {
            g_reply.reset(); g_reply.fail(409, "operation-cancelled");
        } else {
            processLine(job->input, job->tooLong);
        }
        encodeReply(*job, g_reply);
        g_currentJob = nullptr;
        g_work.finish(*job);
    }
}

bool startWorker() {
    ueventCreate(&g_workReady, true);
    const Result rc = threadCreate(&g_worker, commandWorker, nullptr, g_workerStack, sizeof(g_workerStack), 0x2C, -2);
    if (R_FAILED(rc)) { log::ring("worker create failed rc=0x%X", rc); return false; }
    const Result started = threadStart(&g_worker);
    if (R_FAILED(started)) { threadClose(&g_worker); log::ring("worker start failed rc=0x%X", started); return false; }
    return true;
}

void closeClient(int slot) {
    auto& c = g_clients[slot];
    g_work.cancelClient(slot, c.generation);
    if (c.fd >= 0) ::close(c.fd);
    c.fd = -1; c.len = 0; c.dropping = false; c.readClosed = false;
}

void flushResponses() {
    g_work.discardDisconnected([](int slot, uint64_t generation) {
        return g_clients[slot].fd >= 0 && g_clients[slot].generation == generation;
    });
    for (int i = 0; i < kMaxClients; ++i) {
        auto& c = g_clients[i];
        if (c.fd < 0) continue;
        auto* j = g_work.completeFor(i, c.generation);
        if (j == nullptr) { if (c.readClosed && !g_work.hasClient(i,c.generation)) closeClient(i); continue; }
        const int sent = ::send(c.fd, j->output + j->sent, j->outputBytes - j->sent, 0);
        if (sent > 0) {
            j->sent += sent;
            if (j->sent == j->outputBytes) g_work.retire(*j);
        } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            closeClient(i);
        }
    }
}

bool submitLine(int slot, char* line, bool tooLong) {
    auto& c = g_clients[slot];
    // hello 在独立连接上不经过工作队列，可在耗时操作期间检查平台是否存活。
    if (!tooLong && !g_work.hasClient(slot, c.generation) && std::strcmp(line, "hello") == 0) {
        char out[192];
        const int n = std::snprintf(out, sizeof(out), "OK 4\nplatform=%s\nproto=1\nboot=%d\ntools=%d\n",
                                    kPlatformName, log::bootCount(), reg::toolCount());
        return sendAll(c.fd, out, n);
    }
    if (!tooLong && !g_work.hasClient(slot, c.generation) && std::strcmp(line, "cancel") == 0) {
        g_work.cancelAll();
        static const char answer[] = "OK 1\ncancel_requested=1\n";
        return sendAll(c.fd, answer, sizeof(answer)-1);
    }
    auto* job = g_work.submit(slot, c.generation, line, tooLong);
    if (job == nullptr) return false; // 关闭过载连接，不发送乱序错误帧。
    ueventSignal(&g_workReady);
    return true;
}

// 当前还开着的客户端数，用于运行状态日志。
int activeClientCount() {
    int n = 0;
    for (int i = 0; i < kMaxClients; ++i) {
        if (g_clients[i].fd >= 0) ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------- 监听口
// ★ 注意：**必须定义在 `nxc::protocol` 这一层**（不能在匿名命名空间里另起一份）——
//   否则和 `platform.hpp` 里那份声明撞名，调用点会报 ambiguous（编译期就炸了，这是好事）。
// 建监听口：socket + SO_REUSEADDR + bind(47800) + listen。成功返回 fd，失败返回 -1。
// 唤醒或监听异常后复用同一个入口恢复。
int openListener() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // 网络状态可能在 poll 返回与 accept 之间变化；监听口不能阻塞整个服务循环。
    // 设置失败时不继续使用阻塞 socket，交给调用方稍后重试。
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log::ring("net: listener O_NONBLOCK failed errno=%d", errno);
        ::close(fd);
        return -1;
    }
    const int actualFlags = ::fcntl(fd, F_GETFL, 0);
    if (actualFlags < 0 || !(actualFlags & O_NONBLOCK)) {
        log::ring("net: listener O_NONBLOCK not retained flags=%d errno=%d", actualFlags, errno);
        ::close(fd);
        return -1;
    }

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
    // 保持多客户端监听。
    const int backlog = nxc::net::listenerBacklog();
    if (::listen(fd, backlog) != 0) {
        log::ring("net: listen failed");
        ::close(fd);
        return -1;
    }
    log::ring("net: listener fd=%d svc=bsd:u backlog=%d reuseaddr=%d",
              fd, backlog, nxc::net::useReuseAddr() ? 1 : 0);
    return fd;
}

void serveForever(int listenFd) {
    pollfd fds[kMaxClients + 1];
    int slots[kMaxClients + 1];
    if (!startWorker()) { if (listenFd >= 0) ::close(listenFd); return; }
    int listener = listenFd;          // -1 ⇒ 现在没有监听口
    log::ring("net: serveForever listen=%d", listener);

    for (;;) {
        // 睡眠前关闭全部socket后再确认PSC；fd仅由网络线程操作。
        if (nxc::power::sleepRequested()) {
            g_work.cancelAll();
            if (listener >= 0) { ::close(listener); listener = -1; }
            for (int i = 0; i < kMaxClients; ++i) closeClient(i);
            // net.probe 自有socket由工作线程在50ms轮询内关闭；确认后才报告。
            for (int wait=0; g_networkOperations.load(std::memory_order_acquire)>0 && wait<300; wait+=5)
                svcSleepThread(5000000ULL);
            // ★ 关干净了，通知线程这才去 Ack（它最多等 900ms）。
            if (g_networkOperations.load(std::memory_order_acquire)==0) nxc::power::reportSocketsClosed();
            log::ring("power: 全部 socket 已关闭（睡眠），已回报通知线程");

            while (nxc::power::sleepRequested()) svcSleepThread(50000000ULL);   // 50ms 一轮，醒来反应快
            nxc::power::consumeWoke();

            // ★ 醒来重开监听口。开不上不致命 —— 下一轮循环会用同一条重开逻辑再试。
            listener = openListener();
            log::ring("power: 醒来，listener fd=%d", listener);
            log::ring("PSC 醒来 state=%d → 重开监听口 fd=%d",
                       nxc::power::lastState(), listener);
            continue;
        }

        // 包括唤醒和异常恢复，已有客户端在线时也继续恢复监听。
        if (listener < 0) {
            listener = openListener();
            log::ring("net: listener reopened fd=%d", listener);
        }

        flushResponses();
        int n = 0;
        if (listener >= 0) {
            fds[n].fd = listener;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            slots[n] = -1;
            ++n;
        }

        for (int i = 0; i < kMaxClients; ++i) {
            if (g_clients[i].fd < 0 || g_clients[i].readClosed) continue;
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
            svcSleepThread(100000000ULL);
            continue;
        }

        // 空闲100ms内响应睡眠；有工作时5ms检查应答，避免每个上传分块额外等100ms。
        const int pollMs = g_work.hasWork() ? 5 : 100;
        const int ready = ::poll(fds, static_cast<nfds_t>(n), pollMs);
        if (ready == 0) {
            static int s_servingTick = 0;
            if (++s_servingTick % 300 == 0) {   // ~30 秒一条
                log::ring("serving tick=%d (clients=%d, listener=%d, psc_events=%d, psc_sleep=%d)",
                           s_servingTick / 300,
                           activeClientCount(), listener >= 0 ? 1 : 0,
                           nxc::power::eventCount(), nxc::power::sleepEventCount());
            }
            continue;
        }
        if (ready < 0) {
            const int pollError = errno;
            if (pollError != EINTR) {
                log::ring("net: poll failed errno=%d, retry after 100ms", pollError);
                svcSleepThread(100000000ULL);
            }
            continue;
        }

        for (int k = 0; k < n; ++k) {
            if ((fds[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) continue;

            // ---- 新连接
            if (slots[k] < 0) {
                // 错误/挂断不是“有连接可接收”。先释放失效监听口，下轮重建。
                // 即使同时带 POLLIN，也不能在已失效的监听口上尝试 accept。
                if (fds[k].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    log::ring("net: listener events=0x%X, close and retry", fds[k].revents);
                    ::close(listener);
                    listener = -1;
                    svcSleepThread(100000000ULL);
                    continue;
                }
                if (!(fds[k].revents & POLLIN)) continue;
                const int fd = ::accept(listener, nullptr, nullptr);
                if (fd < 0) {
                    const int acceptError = errno;
                    if (acceptError != EAGAIN && acceptError != EWOULDBLOCK && acceptError != EINTR) {
                        log::ring("net: accept failed errno=%d, retry after 100ms", acceptError);
                        svcSleepThread(100000000ULL);
                    }
                    continue;
                }

                // 接入后立即验证非阻塞，包含满载拒绝分支。
                const int clientFlags = ::fcntl(fd, F_GETFL, 0);
                if (clientFlags < 0 || ::fcntl(fd, F_SETFL, clientFlags | O_NONBLOCK) < 0) { ::close(fd); continue; }
                const int actualClientFlags = ::fcntl(fd,F_GETFL,0);
                if (actualClientFlags < 0 || !(actualClientFlags & O_NONBLOCK)) { ::close(fd); continue; }
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
                ++g_clients[slot].generation;
                g_clients[slot].fd = fd;
                g_clients[slot].len = 0;
                g_clients[slot].dropping = false;
                g_clients[slot].readClosed = false;

                log::ring("client connected slot=%d", slot);

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
            if (fds[k].revents & (POLLERR | POLLNVAL)) { closeClient(slot); continue; }
            char buf[256];
            bool closed = false;

            const int got = ::recv(g_clients[slot].fd, buf, sizeof(buf), 0);
            if (got == 0) {
                // EOF 可能只是客户端关闭写方向；先发完此前请求的应答。
                g_clients[slot].readClosed = true;
                closed = !g_work.hasClient(slot, g_clients[slot].generation);
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
                        if (!submitLine(slot, g_clients[slot].line, tooLong)) {
                            closed = true;
                        }
                    } else if (c == '\0') {
                        g_clients[slot].dropping = true;
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
                closeClient(slot);
            }
        }
    }
}

}  // namespace protocol
}  // namespace nxc

namespace nxc::host {
bool beginNetworkOperation() {
    protocol::g_networkOperations.fetch_add(1, std::memory_order_acq_rel);
    if (operationCancelled()) { protocol::g_networkOperations.fetch_sub(1, std::memory_order_release); return false; }
    return true;
}
void endNetworkOperation() { protocol::g_networkOperations.fetch_sub(1, std::memory_order_release); }
bool operationCancelled() {
    return power::sleepRequested() || (protocol::g_currentJob != nullptr &&
        protocol::g_currentJob->cancelled.load(std::memory_order_acquire));
}
bool sleepMilliseconds(unsigned long long ms) {
    while (ms > 0) {
        if (operationCancelled()) return false;
        const auto part = ms > 20 ? 20 : ms;
        svcSleepThread(part * 1000000ULL); ms -= part;
    }
    return !operationCancelled();
}
}
