// ================================================================
// NxCourier · Copyright (C) 2026 granule256
//
// 本文件是 NxCourier 的一部分，以 GNU 通用公共许可证第 2 版
//（GPL-2.0-only）发布，不带任何担保。
// 完整条款见仓库根目录的 LICENSE。
// ================================================================

// NxCourier 救生艇 —— 平台自己挂了的时候，用它把平台救回来。
//
// ## 为什么需要它（真实事故）
//
// 2026-10-05：我改大了 socket 缓冲配置，`socketInitialize()` 再也成功不了，
// 而模块的 main 在这种情况下会 `return 0` **静默退出** ⇒ 47800 端口永远不出现。
//
// 于是出现了一个**死锁**：
//   * 自我更新（`nxc_deploy`）走 47800 ⇒ 端口死了就进不去
//   * 设备上的 FTP 也不是常驻的（跑在 hbmenu 进程里）
//   * ⇒ **只能靠人去 hbmenu 开 FTP，或者把卡拔下来**
//
// **"能重启 / 能恢复" 这个能力，不该只放在那个会坏掉的盒子里。**
//
// ## 它是什么
//
// 一个**普通的 NRO 自制软件**（不是 sysmodule），放在 /switch/ 下，
// 从 hbmenu 点开就能用。它**不依赖平台、不依赖 47800、不依赖 FTP**。
//
// 它只做一件事：把 `/switch/nxc-recovery/` 里的恢复镜像写回平台模块目录，
// 逐字节校验，然后重启。
//
// ## 用法
//
//   1. 把一份已知可用的模块文件放到 `/switch/nxc-recovery/`（可以放多份，带日期命名）
//   2. 平台挂了的时候：hbmenu → 启动本程序
//   3. ↑/↓ 选镜像，按 A → 写回 + 校验 + 重启
//
// ★ 建议平时就**留一份已知可用**的镜像在那儿，别等出事才找。
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

namespace {

constexpr const char* kSrcDir  = "/switch/nxc-recovery";
constexpr const char* kDstPath = "/atmosphere/contents/4200000000000012/exefs.nsp";
constexpr int kMaxEntries = 32;
constexpr size_t kBufSize = 64 * 1024;

struct Entry {
    char name[256];
    long long size;
};

Entry g_entries[kMaxEntries];
int   g_count = 0;
unsigned char* g_buf = nullptr;

// 扫描目录，收 *.nsp（恢复镜像就是这个）
void scanDir() {
    g_count = 0;
    DIR* dir = ::opendir(kSrcDir);
    if (dir == nullptr) return;

    for (dirent* e = ::readdir(dir); e != nullptr; e = ::readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        const size_t len = std::strlen(e->d_name);
        if (len < 5) continue;
        if (std::strcmp(e->d_name + len - 4, ".nsp") != 0) continue;
        if (g_count >= kMaxEntries) break;

        char full[512];
        std::snprintf(full, sizeof(full), "%s/%s", kSrcDir, e->d_name);
        struct stat st;
        if (::stat(full, &st) != 0) continue;

        std::snprintf(g_entries[g_count].name, sizeof(g_entries[g_count].name), "%s", e->d_name);
        g_entries[g_count].size = static_cast<long long>(st.st_size);
        ++g_count;
    }
    ::closedir(dir);
}

// 复制 + 逐字节校验。返回 0 成功；否则返回"第几个块出错"之类的负值。
// ★ 校验方式选**整文件逐字节比对**而不是哈希：不需要引入 sha256 实现，
//   强度一样（能发现任何一位的差异），而且能顺便验证"读得回来"。
int restore(const char* srcName, char* errBuf, size_t errCap) {
    char src[512];
    std::snprintf(src, sizeof(src), "%s/%s", kSrcDir, srcName);

    FILE* in = std::fopen(src, "rb");
    if (in == nullptr) {
        std::snprintf(errBuf, errCap, "打不开源文件");
        return -1;
    }
    FILE* out = std::fopen(kDstPath, "wb");
    if (out == nullptr) {
        std::fclose(in);
        std::snprintf(errBuf, errCap, "打不开目标路径（目录不存在？）");
        return -2;
    }

    long long total = 0;
    size_t n = 0;
    while ((n = std::fread(g_buf, 1, kBufSize, in)) > 0) {
        if (std::fwrite(g_buf, 1, n, out) != n) {
            std::fclose(in);
            std::fclose(out);
            std::snprintf(errBuf, errCap, "写到第 %lld 字节时失败（卡满？）", total);
            return -3;
        }
        total += static_cast<long long>(n);
    }
    const bool readErr = std::ferror(in) != 0;
    std::fclose(in);
    std::fclose(out);
    if (readErr) {
        std::snprintf(errBuf, errCap, "读源文件出错");
        return -4;
    }

    // ★ 读回逐字节比对 —— 没有这一步就等于"没验证"
    FILE* a = std::fopen(src, "rb");
    FILE* b = std::fopen(kDstPath, "rb");
    if (a == nullptr || b == nullptr) {
        if (a) std::fclose(a);
        if (b) std::fclose(b);
        std::snprintf(errBuf, errCap, "校验时打不开文件");
        return -5;
    }
    long long off = 0;
    int rc = 0;
    for (;;) {
        const size_t na = std::fread(g_buf, 1, kBufSize, a);
        unsigned char* cmp = static_cast<unsigned char*>(std::malloc(kBufSize));
        if (cmp == nullptr) { rc = -6; break; }
        const size_t nb = std::fread(cmp, 1, kBufSize, b);
        const bool same = (na == nb) && (na == 0 || std::memcmp(g_buf, cmp, na) == 0);
        std::free(cmp);
        if (!same) {
            std::snprintf(errBuf, errCap, "校验不一致（约在第 %lld 字节）", off);
            rc = -7;
            break;
        }
        if (na == 0) break;   // 两边同时到结尾
        off += static_cast<long long>(na);
    }
    std::fclose(a);
    std::fclose(b);
    if (rc != 0) return rc;

    std::snprintf(errBuf, errCap, "共 %lld 字节，逐字节一致", total);
    return 0;
}

void draw(int sel, const char* status) {
    consoleClear();
    std::printf("NxCourier 救生艇\n");
    std::printf("平台（47800）挂了的时候用它把模块写回去\n");
    std::printf("------------------------------------------------\n\n");

    if (g_count == 0) {
        std::printf("  %s 里没有找到任何 .nsp 恢复镜像。\n\n", kSrcDir);
        std::printf("  请从电脑把一份已知可用的 exefs.nsp 放进去，命名成 .nsp 即可。\n");
    } else {
        std::printf("  找到 %d 份镜像（↑/↓ 选择）：\n\n", g_count);
        for (int i = 0; i < g_count; ++i) {
            std::printf("   %s %-34s %8lld 字节\n",
                        (i == sel ? ">" : " "), g_entries[i].name, g_entries[i].size);
        }
    }

    std::printf("\n  目标：%s\n", kDstPath);
    std::printf("------------------------------------------------\n");
    if (status != nullptr && status[0] != '\0') {
        std::printf("  %s\n", status);
    } else {
        std::printf("  A = 写回并重启    + = 退出\n");
    }
    consoleUpdate(nullptr);
}

}  // namespace

int main(int, char**) {
    consoleInit(nullptr);
    fsInitialize();
    fsdevMountSdmc();
    padInitializeDefault(nullptr);

    g_buf = static_cast<unsigned char*>(std::malloc(kBufSize));
    if (g_buf == nullptr) {
        draw(0, "内存不足，退出");
        svcSleepThread(2000000000ULL);
        return 1;
    }

    scanDir();

    char status[160] = {0};
    int sel = 0;
    bool done = false;

    draw(sel, status);
    while (appletMainLoop()) {
        padUpdate(nullptr);
        const u64 down = padGetButtonsDown(nullptr);

        if (down & HidNpadButton_Plus) break;

        if (g_count > 0) {
            if (down & HidNpadButton_Down) { sel = (sel + 1) % g_count; status[0] = '\0'; }
            if (down & HidNpadButton_Up)   { sel = (sel + g_count - 1) % g_count; status[0] = '\0'; }
            if ((down & HidNpadButton_A) && !done) {
                std::snprintf(status, sizeof(status), "正在写回 %s …", g_entries[sel].name);
                draw(sel, status);

                char msg[160] = {0};
                const int r = restore(g_entries[sel].name, msg, sizeof(msg));
                if (r == 0) {
                    std::snprintf(status, sizeof(status), "★ 成功：%s", msg);
                    done = true;
                } else {
                    std::snprintf(status, sizeof(status), "★ 失败(%d)：%s", r, msg);
                }
                draw(sel, status);

                if (r == 0) {
                    // 写成功了才重启 —— 失败就别重启，留着现状更好排查
                    svcSleepThread(1500000000ULL);
                    if (R_SUCCEEDED(bpcInitialize())) {
                        bpcRebootSystem();
                        bpcExit();
                    }
                    std::snprintf(status, sizeof(status),
                                  "★ 已写回，但重启失败 —— 请手动重启主机");
                    draw(sel, status);
                }
            }
        }
        svcSleepThread(50000000ULL);   // 50ms
    }

    std::free(g_buf);
    fsdevUnmountAll();
    fsExit();
    consoleExit(nullptr);
    return 0;
}
