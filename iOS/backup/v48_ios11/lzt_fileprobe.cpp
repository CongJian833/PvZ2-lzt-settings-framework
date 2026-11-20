/*
 * lzt_fileprobe.cpp — 文件访问探针（用于定位「谁读取了 CDN 资源」）
 *
 * 为什么单独成文件：主实现文件已相当庞大，逐行插入容易破坏结构；
 * 本模块自成一体，主文件只需调用 lzt_fileprobe_install() 一行。
 *
 * 手段：用 fishhook 式符号重绑定（只改数据段符号指针，不改代码页，
 *       因此不会像 inline hook libc 那样崩溃）。
 *
 * 覆盖的调用：open / openat / fopen / stat / stat64 / lstat / access
 *   —— fs_usage 实测游戏对目标文件执行的是 stat64 + open，
 *      因此这些是重点；其余一并挂上以覆盖不同代码路径。
 *
 * 输出：每次命中「关心路径」时打印路径 + 调用栈（相对主程序基址的偏移），
 *       据此可直接定位读取 LawnStrings / CDN 文件的函数。
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include "lzt_backtrace_shim.h"   /* minos < 11 时把 backtrace 换成空实现 */

#include "lzt_core.h"

#ifdef __APPLE__

extern "C" int lzt_rebind(const char* symbol, void* replacement, void** orig_out);

namespace {

std::atomic<int> g_hitCount{0};
std::atomic<int> g_anyCount{0};

bool path_interesting(const char* p) {
    if (p == nullptr) return false;
    return strstr(p, "LawnStrings") || strstr(p, ".rton") || strstr(p, ".rsb")
        || strstr(p, "CDN") || strstr(p, "No_Backup") || strstr(p, "LanguageFlag")
        || strstr(p, "properties/") || strstr(p, "locales/");
}

// 去重表：同一条路径只抓一次调用栈（避免高频重入拖慢启动）
constexpr int kMaxSeen = 64;
constexpr size_t kSeenLen = 160;
char g_seen[kMaxSeen][kSeenLen];
std::atomic<int> g_seenCount{0};

bool mark_seen(const char* p) {
    const int n = g_seenCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxSeen; ++i) {
        if (strncmp(g_seen[i], p, kSeenLen - 1) == 0) return true;
    }
    if (n >= kMaxSeen) return true;
    strncpy(g_seen[n], p, kSeenLen - 1);
    g_seen[n][kSeenLen - 1] = 0;
    g_seenCount.store(n + 1, std::memory_order_release);
    return false;
}

// 只在「关心路径且首次见到」时抓栈；其它路径只记一行（限 60 条），
// 用于确认探针是否真的被调用（零捕获时分辨「探针失效」与「确实没读」）。
std::atomic<int> g_otherSeen{0};

bool probe_should_dump(const char* p) {
    if (p == nullptr) return false;
    static __thread int inProbe = 0;
    if (inProbe) return false;
    inProbe = 1;
    bool result = false;
    if (path_interesting(p)) {
        result = !mark_seen(p);
    } else if (g_otherSeen.fetch_add(1) < 60) {
        lzt_core::log_write("FILESEE %s", p);
    }
    inProbe = 0;
    return result;
}

void dump_backtrace(const char* tag, const char* path) {
    static thread_local int inDump = 0;
    if (inDump) return;            // 重入保护：backtrace 自身可能触发 IO
    inDump = 1;
    void* frames[24];
    const int n = backtrace(frames, 24);
    const uintptr_t base = lzt_core::base();
    const uintptr_t lo = base, hi = base + 0x40000000u;
    char line[1024];
    int off = snprintf(line, sizeof(line), "%s %s bt(%d):", tag, path ? path : "(null)", n);
    for (int i = 0; i < n && off < 900; ++i) {
        const uintptr_t a = (uintptr_t)frames[i];
        if (a >= lo && a < hi)
            off += snprintf(line + off, sizeof(line) - off, " +%lx", (unsigned long)(a - base));
        else
            off += snprintf(line + off, sizeof(line) - off, " [sys %lx]", (unsigned long)a);
    }
    lzt_core::log_write("%s", line);
    inDump = 0;
}

// 统计用：确认 hook 是否真的被调用（记录前若干次，不论路径）
void note_any(const char* tag, const char* path) {
    (void)tag; (void)path;   // 静音：原为每次文件访问都写日志，噪声大且自身会走 open
}

/* ---------------- open ---------------- */
int (*o_open)(const char*, int, ...) = nullptr;
int h_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    note_any("open", path);
    if (probe_should_dump(path)) {
        dump_backtrace("FILEOPEN", path);
    }
    return o_open ? o_open(path, flags, mode) : -1;
}

/* ---------------- openat ---------------- */
int (*o_openat)(int, const char*, int, ...) = nullptr;
int h_openat(int fd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    note_any("openat", path);
    if (probe_should_dump(path)) {
        dump_backtrace("FILEOPENAT", path);
    }
    return o_openat ? o_openat(fd, path, flags, mode) : -1;
}

/* ---------------- fopen ---------------- */
FILE* (*o_fopen)(const char*, const char*) = nullptr;
FILE* h_fopen(const char* path, const char* mode) {
    note_any("fopen", path);
    if (probe_should_dump(path)) {
        dump_backtrace("FILEFOPEN", path);
    }
    return o_fopen ? o_fopen(path, mode) : nullptr;
}

/* ---------------- stat / stat64 / lstat ---------------- */
int (*o_stat)(const char*, struct stat*) = nullptr;
int h_stat(const char* path, struct stat* st) {
    note_any("stat", path);
    if (probe_should_dump(path)) {
        dump_backtrace("FILESTAT", path);
    }
    return o_stat ? o_stat(path, st) : -1;
}

int (*o_stat64)(const char*, struct stat*) = nullptr;
int h_stat64(const char* path, struct stat* st) {
    note_any("stat64", path);
    if (probe_should_dump(path)) {
        dump_backtrace("FILESTAT64", path);
    }
    return o_stat64 ? o_stat64(path, st) : -1;
}

int (*o_lstat)(const char*, struct stat*) = nullptr;
int h_lstat(const char* path, struct stat* st) {
    note_any("lstat", path);
    if (probe_should_dump(path)) {
        dump_backtrace("FILELSTAT", path);
    }
    return o_lstat ? o_lstat(path, st) : -1;
}

/* ---------------- access ---------------- */
int (*o_access)(const char*, int) = nullptr;
int h_access(const char* path, int mode) {
    note_any("access", path);
    if (probe_should_dump(path)) {
        dump_backtrace("FILEACCESS", path);
    }
    return o_access ? o_access(path, mode) : -1;
}

} // namespace

// 安装全部文件访问钩子（幂等）。返回替换成功的槽总数。
extern "C" int lzt_fileprobe_install(void) {
    if (o_open != nullptr || o_stat != nullptr) return 0;   // 幂等
    int total = 0;
    total += lzt_rebind("open",   (void*)h_open,   (void**)&o_open);
    total += lzt_rebind("openat", (void*)h_openat, (void**)&o_openat);
    total += lzt_rebind("fopen",  (void*)h_fopen,  (void**)&o_fopen);
    total += lzt_rebind("stat",   (void*)h_stat,   (void**)&o_stat);
    total += lzt_rebind("stat64", (void*)h_stat64, (void**)&o_stat64);
    total += lzt_rebind("lstat",  (void*)h_lstat,  (void**)&o_lstat);
    total += lzt_rebind("access", (void*)h_access, (void**)&o_access);
    lzt_core::log_write("fileprobe: installed slots=%d (open=%p stat=%p stat64=%p)",
                        total, (void*)o_open, (void*)o_stat, (void*)o_stat64);
    return total;
}

#endif // __APPLE__
