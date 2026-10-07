// 内置工具 · net：**本机网络状态 + 关键外部依赖的连通性自检**。
//
// ## 为什么需要它
//
// 这个项目的**依赖有一部分不在我们自己手里**：
//
//   * `22225` —— Atmosphère 自带的 GDB 调试桩。**它没开，PC 侧的 GDB 就用不了。**
//   * `5000`  —— 设备上的 ftpd。★ 而且**一启动游戏它就会消失**（跑在 hbmenu 进程里）。
//   * `6666`  —— SysDVR 的串流端口。
//
// 原来这些依赖**坏了没有任何提示**：用户只会看到 GDB 抛一句
// `Connection timed out`，既不知道原因也不知道怎么修
// （需求文档 F12「设备发现/连通性自检」定义了这件事，但一直没做）。
//
// ## 为什么让**设备**来做这件事
//
// 因为**桩在不在，设备自己最清楚** —— 它可以从内部连 `127.0.0.1:22225`。
// PC 侧只能看到"22225 连不上"，分不清是"桩没开"还是"网络不通"；
// 而设备同时具备两个视角，能给出一句明确的结论。
#include "nxc_sdk.hpp"

#include <switch.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// 网络字节序转换。
// ★ 不用 `<arpa/inet.h>` 的 htons/htonl —— 那套头在这个交叉编译环境里位置不定，
//   而这台机器是小端、网络序是大端，**一个 bswap 就是全部所需**，没必要引依赖。
u16 toNet16(u16 v) { return static_cast<u16>((v >> 8) | (v << 8)); }
u32 toNet32(u32 v) { return __builtin_bswap32(v); }

struct Dep {
    const char* what;      // 人话名称
    const char* note;      // 它是干什么的 / 什么时候会没
    u16 port;
};

// 这几个是"不在我们手里、但坏了会让人以为是我们的问题"的依赖。
const Dep kDeps[] = {
    {"gdb-stub", "Atmosphere 的调试桩；没开则 PC 侧的 GDB 完全不可用", 22225},
    {"ftpd",     "设备上的 FTP；启动游戏后它会被顶掉（跑在 hbmenu 里）", 5000},
    {"sysdvr",   "SysDVR 串流；没装或没启动就没有实时画面", 6666},
};

// 试连一个 TCP 端口。**非阻塞 connect + poll**，避免卡死在没开的端口上。
// out_errno 回报失败的底层原因（ECONNREFUSED = 有人接但拒绝 / ETIMEDOUT = 没响应）。
bool probeTcp(u32 ipHostOrder, u16 port, int timeoutMs, int* outErrno) {
    *outErrno = 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *outErrno = errno; return false; }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = toNet16(port);
    addr.sin_addr.s_addr = toNet32(ipHostOrder);

    bool ok = false;
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        pollfd p;
        p.fd = fd;
        p.events = POLLOUT;
        p.revents = 0;
        if (::poll(&p, 1, timeoutMs) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) ok = true;
            else *outErrno = err;
        } else {
            *outErrno = ETIMEDOUT;
        }
    } else {
        *outErrno = errno;   // 通常是 ECONNREFUSED：立刻就知道没人听
    }
    ::close(fd);
    return ok;
}

// 把主机字节序的 IPv4 打印成点分十进制。
void ipToString(u32 ip, char* out, size_t cap) {
    std::snprintf(out, cap, "%u.%u.%u.%u",
                  (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu, (ip >> 8) & 0xFFu, ip & 0xFFu);
}

// 解析 "a.b.c.d" → 主机字节序的 u32；不合法返回 false。
bool parseIp(const char* text, u32* out) {
    if (text == nullptr) return false;
    u32 parts[4] = {0, 0, 0, 0};
    int idx = 0;
    u32 acc = 0;
    bool any = false;
    for (const char* p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            acc = acc * 10 + static_cast<u32>(*p - '0');
            any = true;
            if (acc > 255) return false;
        } else if (*p == '.' || *p == '\0') {
            if (!any || idx > 3) return false;
            parts[idx++] = acc;
            acc = 0;
            any = false;
            if (*p == '\0') break;
        } else {
            return false;
        }
    }
    if (idx != 4) return false;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

// ---------------------------------------------------------------- 方法

void cmdInfo(const nxc::Args&, nxc::Reply& r) {
    r.kv("listen_port", "47800");
    r.kvi("tools", nxc::host::toolCount());
    r.kvi("boot", nxc::host::bootCount());

    const Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        r.kvf("nifm", "unavailable rc=0x%08X", static_cast<unsigned>(rc));
        return;
    }

    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0) {
        char buf[24];
        // ★ nifm 返回的是 `struct in_addr`，**网络字节序**。
        //   实测不转就打印成 11.124.168.192（真实是 192.168.1.50）—— 正好反了。
        ipToString(__builtin_bswap32(ip), buf, sizeof(buf));
        r.kv("ip", buf);
    } else {
        r.kv("ip", "not-connected");
    }

    u32 strength = 0;
    NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
    NifmInternetConnectionType type = NifmInternetConnectionType_WiFi;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))) {
        r.kvi("wifi_strength", static_cast<long long>(strength));   // 0..3
        r.kvi("conn_status", static_cast<long long>(status));
        r.kv("conn_type", type == NifmInternetConnectionType_WiFi ? "wifi" : "ethernet");
    }
    nifmExit();
}

void cmdProbe(const nxc::Args& a, nxc::Reply& r) {
    const char* host = a.get("host");
    // ★★★ 2026-10-06 晚：**缺 host 不再静默用 `127.0.0.1`**。
    //   原来的写法让"漏了参数"看起来像"探测了一个真实地址"（回包里 host=127.0.0.1 很容易被跳过），
    //   而"想探本机服务"是一个**需要写清楚**的意图 —— `net.deps` 就是显式用 127.0.0.1 的。
    if (host == nullptr || *host == '\0') {
        r.fail(400, "missing-host: 需要 host=<点分 IP>（要探本机服务请显式写 host=127.0.0.1）");
        return;
    }

    u32 ip = 0;
    if (!parseIp(host, &ip)) {
        r.fail(400, "host-must-be-dotted-quad-ipv4");
        return;
    }

    long long port = a.getInt("port", 0);
    if (port <= 0 || port > 65535) { r.fail(400, "port-out-of-range"); return; }

    long long timeoutMs = a.getInt("timeout_ms", 800);
    if (timeoutMs < 50) timeoutMs = 50;
    if (timeoutMs > 10000) timeoutMs = 10000;

    int err = 0;
    const bool ok = probeTcp(ip, static_cast<u16>(port), static_cast<int>(timeoutMs), &err);

    r.kv("host", host);
    r.kvi("port", port);
    r.kvi("open", ok ? 1 : 0);
    r.kvi("errno", err);
    r.kv("errno_name", err == 0 ? "-"
                        : (err == ECONNREFUSED ? "ECONNREFUSED（没人监听）"
                        : (err == ETIMEDOUT ? "ETIMEDOUT（无响应）"
                        : (err == ENOBUFS ? "ENOBUFS（本平台开不出 socket，不是目标的问题）"
                                          : "其他"))));
    r.kvi("timeout_ms", timeoutMs);
}

void cmdDeps(const nxc::Args& a, nxc::Reply& r) {
    long long timeoutMs = a.getInt("timeout_ms", 700);
    if (timeoutMs < 50) timeoutMs = 50;
    if (timeoutMs > 10000) timeoutMs = 10000;

    nxc::host::breadcrumb("net.deps timeout=%lldms", timeoutMs);

    int up = 0;
    for (const Dep& dep : kDeps) {
        int err = 0;
        const bool ok = probeTcp(0x7F000001u /* 127.0.0.1（主机字节序） */,
                                 dep.port, static_cast<int>(timeoutMs), &err);
        if (ok) ++up;
        r.next();
        r.kv("name", dep.what);
        r.kvi("port", dep.port);
        r.kv("reachable", ok ? "yes" : "no");
        r.kv("note", dep.note);
        if (!ok) {
            r.kv("why", err == ECONNREFUSED ? "本机没人监听这个端口"
                                          : (err == ETIMEDOUT ? "超时未响应" : "见 errno"));
            r.kvi("errno", err);
        }
    }

    r.next();
    r.kvi("reachable_count", up);
    r.kvi("checked_count", static_cast<long long>(sizeof(kDeps) / sizeof(kDeps[0])));
    r.raw("★ 从设备内部测的 127.0.0.1 —— 所以结论是「本机有没有在跑这个服务」，与网络无关");
    r.raw("gdb-stub 没开时：PC 侧 GDB 会直接超时。开启办法见 使用说明书 的「六 外部依赖」一节");
    r.raw("ftpd 不在是正常的：它跑在 hbmenu 进程里，一启动游戏就没了");
}

// ---------------------------------------------------------------- 自描述元数据（2026-10-06 晚）
// ★ 本工具的全部知识都写在这里 —— tools.list / tools.doc 只是把这里的内容汇总出去。
const nxc::Param kProbe[] = {
    {"host",       "IP",  true,  nullptr, "点分十进制 IPv4 必填；探本机服务请显式写 127.0.0.1"},
    {"port",       "int", true,  nullptr, "TCP 端口 1..65535"},
    {"timeout_ms", "int", false, "800",   "连接超时毫秒 50..10000"},
};
const nxc::Param kDepsTimeout[] = {
    {"timeout_ms", "int", false, "700", "每个端口试连的超时毫秒 50..10000"},
};

const nxc::MethodInfo kInfoInfo = {
    nullptr, 0, "read",
    "作用：报本机网络状态 —— 本机 IP、连接类型、WiFi 信号强度、监听端口（走 nifm）。\n"
    "返回：listen_port=我们的监听端口 tools=已注册工具数 boot=开机次数；"
    "ip=<点分 IP>（未联网时为 not-connected）；查到连接状态时再给 "
    "wifi_strength=0..3 conn_status=<数值> conn_type=wifi|ethernet。\n"
    "★ IP 由 nifm 给出、已做过字节序修正 —— 回包里的就是能直接用的地址（实测不转会打成反的）。\n"
    "★ nifm 会话开不起来时回 nifm=unavailable rc=0x…，其余字段仍尽量给。\n"
    "示例：net.info\n"
    "注意：只读、无副作用。\n"
    "返回：listen_port= tools= boot= ip=（以及 wifi_strength= conn_status= conn_type=）\n"
    "相关：net.deps（外部依赖自检） net.probe（试连某端口）\n",
};
const nxc::MethodInfo kInfoProbe = {
    kProbe, 3, "read",
    "作用：从设备侧试连一个 TCP 端口，回它开不开以及失败原因。\n"
    "参数说明：\n"
    "  host   必填，点分十进制 IPv4（如 192.168.1.50）。"
    "★ 缺了会回 400 missing-host —— 不再默认 127.0.0.1；要探本机服务请显式写 host=127.0.0.1。\n"
    "  port   必填，1..65535 的 TCP 端口。\n"
    "  timeout_ms  可选，默认 800，范围 50..10000（越界自动夹到边界）。\n"
    "★ 用非阻塞 connect + poll，不会卡死在没人监听的端口上。\n"
    "★ errno_name 帮你读：ECONNREFUSED=有人接但拒绝（没人监听）；ETIMEDOUT=无响应；"
    "ENOBUFS=本平台开不出 socket（不是目标的问题）。\n"
    "示例：net.probe host=127.0.0.1 port=47800\n"
    "      net.probe host=192.168.1.50 port=22225 timeout_ms=1500\n"
    "注意：只读、无副作用；只发起一次连接、不发送任何数据。\n"
    "返回：host= port= open=0|1 errno=<数值> errno_name= timeout_ms=\n"
    "相关：net.info（本机状态） net.deps（一次自检三个外部依赖）\n",
};
const nxc::MethodInfo kInfoDeps = {
    kDepsTimeout, 1, "read",
    "作用：一次性自检关键外部依赖的连通性 —— 22225 GDB 桩 / 5000 FTP / 6666 SysDVR。\n"
    "★ 探测方式是【从设备内部】去连 127.0.0.1 —— 所以结论是「本机有没有在跑这个服务」，"
    "与 PC 能不能连上无关（PC 连不上也可能只是网络问题）。\n"
    "★ ftpd 不在是正常的：它跑在相册 / hbmenu 进程里，一进游戏就没了。\n"
    "★ gdb-stub 没开时 PC 侧 GDB 会直接超时；开启办法见使用说明书的「外部依赖」一节。\n"
    "参数说明：timeout_ms 可选，默认 700，范围 50..10000。\n"
    "示例：net.deps\n"
    "注意：只读、无副作用。\n"
    "返回：每个依赖一行 name= port= reachable=yes|no（没通时附 why= errno=），"
    "末行 reachable_count= checked_count=。\n"
    "相关：net.info net.probe 以及 log.dump（看启动期有没有相关报错）\n",
};

const nxc::Method kMethods[] = {
    {"info",  "本机网络状态：IP / 连接类型 / WiFi 信号强度 / 监听端口", cmdInfo,  &kInfoInfo},
    {"probe", "试连一个 TCP 端口：host=<点分IP> port=<n> [timeout_ms=800]", cmdProbe, &kInfoProbe},
    {"deps",  "★ 一次性自检关键外部依赖（22225 GDB / 5000 FTP / 6666 SysDVR）—— 补上需求 F12", cmdDeps, &kInfoDeps},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_net, "net",
    "本机网络状态与外部依赖连通性自检",
    kMethods,
    "看设备的网络：本机 IP 与连接类型、以及几个「不在我们手里」的外部依赖是否在跑。\n"
    "什么时候用它：\n"
    "  · 先 net.info 拿到设备 IP（PC 侧要连它）\n"
    "  · GDB / FTP / 串流连不上时用 net.deps 一次看清是「服务没开」还是「网络不通」\n"
    "  · 想单独试某个端口用 net.probe\n"
    "★ net.deps 是【从设备内部】连 127.0.0.1 —— 结论只说明本机有没有在跑那个服务。\n"
    "★ ftpd 不在是正常的（一进游戏就没）。\n"
    "边界：只读、无副作用。\n"
    "相关工具：identify（版本/开机次数）、log（日志）、probe（能力探针）"
);
