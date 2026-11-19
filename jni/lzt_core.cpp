// ============================================================
// lzt_core.cpp — settingsframework 公共基础层实现
// 实现说明见 lzt_core.h 头注释。
// ============================================================

#include "lzt_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <ctime>
#include <android/log.h>

namespace lzt_core {

// ---- libPVZ2.so 稳定基址 ----
static std::atomic<uintptr_t> g_base{0};

void set_base(uintptr_t base) { g_base.store(base, std::memory_order_release); }
uintptr_t base() { return g_base.load(std::memory_order_acquire); }
std::atomic<uintptr_t>& base_ref() { return g_base; }

// ---- 崩溃诊断阶段号 ----
static volatile std::sig_atomic_t g_stage = 0;

void set_stage(int stage) { g_stage = stage; }
int stage() { return g_stage; }
volatile std::sig_atomic_t& stage_ref() { return g_stage; }

// ---- 崩溃信号诊断 ----
static FILE *g_logFile = nullptr;   // 本地日志文件句柄（log_init 打开）
static int g_crashFd = -1;          // 崩溃专用 fd（dup 自 g_logFile，绕过 stdio 锁）

// 对外标识：logcat tag 与本地日志文件名（与 so 模块名一致）
static const char *const kLogTag = "settingsframework";
static const char *const kLogFileName = "settingsframework.log";

static void crash_signal_handler(int signal_number, siginfo_t *info, void *) {
    char buffer[256];
    int length = snprintf(buffer, sizeof(buffer),
                          "NATIVE_CRASH signal=%d fault=%p stage=%d base=0x%lx\n",
                          signal_number, info ? info->si_addr : nullptr,
                          g_stage, static_cast<unsigned long>(g_base.load(
                              std::memory_order_acquire)));
    // logcat 保底输出（不经过 stdio，无锁竞争）
    __android_log_write(ANDROID_LOG_FATAL, kLogTag, buffer);
    // 文件写入走独立 fd 的 POSIX write：
    // 历史教训——若崩溃发生在其他线程 log_write 持有 g_logFile 的 stdio 锁
    // 期间，handler 内 fwrite/fprintf 同一 FILE 会永久死锁，NATIVE_CRASH
    // 整行丢失（v48 视角 Tab 崩溃定位时实测）。dup fd + write 绕开 stdio。
    if (length > 0 && g_crashFd >= 0) {
        // v7.6.5：先 seek 到末尾再写。g_crashFd 与 g_logFile 共享偏移,
        // 若崩溃打断了一次未刷净的 stdio 写,共享偏移可能落后于文件尾,
        // NATIVE_CRASH 行会被错位写入甚至混入空洞(真机日志尾部空白填充
        // 实证),导致崩溃记录丢失。
        lseek(g_crashFd, 0, SEEK_END);
        ssize_t rc = write(g_crashFd, buffer, static_cast<size_t>(length));
        (void)rc;
    }
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

// v7.10.1：崩溃处理器保活线程。游戏引擎可能在 so 装载之后安装自己的
// 信号处理器(覆盖我们的),固定时刻的重注册会再次被覆盖;每 5 秒重注册
// 一次、持续 10 分钟,保证测试窗口内 NATIVE_CRASH 必然可见。重复注册
// 同一 handler 无副作用;信号到达时内核按当前注册处理,原子安全。
void start_crash_handler_keepalive() {
    std::thread([]() {
        for (int i = 0; i < 120; ++i) {   // 120 次 × 5 秒 = 10 分钟
            install_crash_diagnostics();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }).detach();
}

void install_crash_diagnostics() {
    // 备用信号栈：栈溢出型 SIGSEGV 发生时，默认 handler 需要在已损坏的
    // 栈上运行而二次崩溃，导致 NATIVE_CRASH 日志缺失。挂接独立备用栈后
    // 任何崩溃都能留下 stage 现场再转发默认处理。
    static char altstack[SIGSTKSZ * 2];
    stack_t ss = {};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction action = {};
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = crash_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
}

bool memory_range_accessible(uintptr_t address, size_t size, bool writable) {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size) return false;

    uintptr_t end = address + size;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long start = 0;
        unsigned long mapped_end = 0;
        char perms[5] = {0};
        if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &mapped_end, perms) != 3) {
            continue;
        }
        if (address >= (uintptr_t)start && end <= (uintptr_t)mapped_end &&
            perms[0] == 'r' && (!writable || perms[1] == 'w')) {
            return true;
        }
    }
    return false;
}

// ---- 进程信息 ----
// 函数内静态：首次调用时读取并缓存（C++11 起线程安全；亦规避静态初始化顺序问题）
static const std::string& cached_package_name() {
    static const std::string name = []() -> std::string {
        std::ifstream cmdline("/proc/self/cmdline");
        std::string pkg;   // cmdline 以 \0 分隔，取首段
        std::getline(cmdline, pkg, '\0');
        return pkg;
    }();
    return name;
}

const char* package_name() { return cached_package_name().c_str(); }

bool files_dir(char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return false;
    const char* pkg = package_name();
    if (pkg == nullptr || pkg[0] == '\0') return false;
    const int n = snprintf(out, outSize, "/sdcard/Android/data/%s/files", pkg);
    return n > 0 && static_cast<size_t>(n) < outSize;
}

// 游戏的外部资源目录（与 iOS 侧同义，路径各随平台）。
bool cdn_dir(char* out, size_t outSize) {
    char files[512];
    if (!files_dir(files, sizeof(files))) return false;
    const int n = snprintf(out, outSize, "%s/No_backup", files);
    return n > 0 && static_cast<size_t>(n) < outSize;
}

// ---- 日志归档（对齐 iOS 的每次启动归档策略）----
// 上一份 settingsframework.log 改名为 settingsframework-<mtime>.log；
// 只保留最近 kLogArchiveKeep 份，更早的删除，防止无限增长。
static constexpr int kLogArchiveKeep = 5;

static void archive_previous_log(const char* dir, const char* logPath) {
    struct stat st;
    if (stat(logPath, &st) != 0) return;        // 不存在：首次启动，无需归档

    char stamp[32];
    {
        struct tm tmv;
        time_t mtime = st.st_mtime;
        localtime_r(&mtime, &tmv);
        snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    }

    // 归档名：<去掉 .log 的主名>-<stamp>.log
    char base[256];
    snprintf(base, sizeof(base), "%s", logPath);
    if (char* dot = strrchr(base, '.')) *dot = '\0';

    char arc[512];
    snprintf(arc, sizeof(arc), "%s-%s.log", base, stamp);
    remove(arc);                    // 秒级撞名则覆盖旧归档
    rename(logPath, arc);           // 失败则后续 "w" 直接截断，不影响功能

    // 清理超额归档：按文件名排序（=时间序），保留最新 kLogArchiveKeep 份。
    // 目录扫描失败直接跳过——归档是便利功能，绝不能影响日志本身可用。
    char prefix[300];
    snprintf(prefix, sizeof(prefix), "%s-", base);
    const size_t prefixLen = strlen(prefix);

    DIR* d = opendir(dir);
    if (d == nullptr) return;
    std::string arcs[64];
    int count = 0;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (strncmp(e->d_name, prefix, prefixLen) != 0) continue;
        const size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".log") != 0) continue;
        if (count < 64) arcs[count++] = e->d_name;
    }
    closedir(d);

    if (count <= kLogArchiveKeep) return;
    // 选择排序出最小的 (count - keep) 个并删除
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (arcs[j] < arcs[i]) { std::string t = arcs[i]; arcs[i] = arcs[j]; arcs[j] = t; }
        }
    }
    char victim[512];
    for (int i = 0; i < count - kLogArchiveKeep; ++i) {
        snprintf(victim, sizeof(victim), "%s/%s", dir, arcs[i].c_str());
        remove(victim);
    }
}

// ---- 日志 ----
void log_init() {
    // 包名 / files 目录走公共工具（/proc/self/cmdline 读取全工程仅此一处）
    char dir[256] = {0};
    if (!files_dir(dir, sizeof(dir))) {
        snprintf(dir, sizeof(dir), "/sdcard/Android/data/unknown/files");
    }
    const std::string logPath = std::string(dir) + "/" + kLogFileName;
    mkdir(dir, 0777);   // 确保目录存在

    // 每次启动归档上一份日志，再以 "w" 重建当前日志（对齐 iOS lzt_core_ios.mm
    // 的【v14 日志策略】）。归档理由：取日志时"哪份是最新"必须直观，
    // 否则只能靠手动删除来保证拿到的是本次运行的输出。
    // 归档名含 mtime（yyyyMMdd-HHmmss，字典序即时间序），只保留最近 5 份。
    archive_previous_log(dir, logPath.c_str());

    g_logFile = fopen(logPath.c_str(), "w");   // "w" 模式每次启动覆盖旧日志
    if (!g_logFile) {
        log_early("WARN: cannot open log file: %s", logPath.c_str());
    }
    if (g_logFile) {
        // 崩溃 handler 专用 fd：绕过 stdio 锁（见 crash_signal_handler 注释）。
        // dup 共享内核文件偏移，log_write 每次已 fflush，写入位置始终正确。
        g_crashFd = dup(fileno(g_logFile));
        // BOOT 行：单份日志被单独发送时自带启动时刻，便于与操作时间对齐
        // （对齐 iOS 的 "===== BOOT <stamp> ====="）。
        {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            struct tm tmv;
            const time_t sec = static_cast<time_t>(tv.tv_sec);
            localtime_r(&sec, &tmv);
            char stamp[64];
            snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                     static_cast<int>(tv.tv_usec / 1000));
            fprintf(g_logFile, "===== BOOT %s =====\n", stamp);
        }
        fprintf(g_logFile, "=== settingsframework Log ===\n");
        fprintf(g_logFile, "package: %s\n", package_name());
        fprintf(g_logFile, "log path: %s\n", logPath.c_str());
        fprintf(g_logFile, "arch: %s\n",
#           ifdef __aarch64__
                "arm64-v8a"
#           elif defined(__arm__)
                "armeabi-v7a"
#           else
                "unknown"
#           endif
        );
        fflush(g_logFile);
    }
}

void log_write_v(const char *fmt, va_list ap) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", buf);
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", buf);
        fflush(g_logFile);   // 立即刷新，防止崩溃丢失日志
    }
}

void log_early(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", buf);
}

void log_write(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write_v(fmt, ap);
    va_end(ap);
}

} // namespace lzt_core
