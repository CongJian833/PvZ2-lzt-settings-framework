/*
 * lzt_core_ios.mm — iOS 平台基础层（对应 Android 侧 lzt_core）
 *
 * 职责与 Android 完全对齐（接口签名见 lzt_core.h）：
 *   - 主程序稳定基址：进程级单例，applyHooks 阶段写入，各模块经此换算游戏地址
 *   - 崩溃诊断阶段号：关键序列前推进 stage，崩溃时随日志输出用于定位
 *   - 双通道日志：沙盒文件 + os_log
 *   - 进程内存可读性检查（iOS 用 mach vm_region，替代 Android 的 /proc/self/maps）
 *   - 包名与 files 目录（替代 Android 的 /proc/self/cmdline 与 /sdcard 路径）
 *
 * iOS 与 Android 的差异（本文件集中处理）：
 *   · 基址：iOS 主程序是 PvZ2.app/PvZ2（可执行文件本身），
 *     越狱环境下 systemhook.dylib 会抢占 dyld 镜像 0，故按 mainBundle
 *     可执行文件路径精确匹配，而不是取 image[0]。
 *   · 内存检查：mach vm_region_64（iOS 无 /proc/self/maps）。
 *   · 日志：写沙盒 Documents，另转 os_log。
 *   · 文件目录：NSSearchPathForDirectoriesInDomains。
 */

#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>   // fps_now_ns 高精度时钟
#include <execinfo.h>        // backtrace（抓 setFrameInterval 调用者）
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <os/log.h>
#include <sys/stat.h>
#include <sys/ucontext.h>   // 崩溃现场寄存器（PC/LR/FP）；不引 ucontext.h，它要求 _XOPEN_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>   // 日志递归锁（多线程并发写同一 FILE*）
#include <sys/time.h>  // gettimeofday（启动分隔行时间戳）
#include <time.h>      // localtime_r
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdint>

#include "lzt_core.h"
#include "settings/settings_framework.h"   // ios_try_rebuild_dirty（延迟重建用）
#include <dispatch/dispatch.h>               // dispatch_async（主队列延后任务）
#include <atomic>
#include <link.h>   // 本项目的 iOS 垫片（iOS/port/include/link.h）：dl_iterate_phdr

namespace lzt_core {

// ============================================================
// 进程级单例
// ============================================================
static std::atomic<uintptr_t> g_base{0};
static volatile std::sig_atomic_t g_stage = 0;

void set_base(uintptr_t base) { g_base.store(base, std::memory_order_release); }
uintptr_t base() { return g_base.load(std::memory_order_acquire); }
std::atomic<uintptr_t>& base_ref() { return g_base; }

void set_stage(int stage) { g_stage = stage; }
int stage() { return (int)g_stage; }
volatile std::sig_atomic_t& stage_ref() { return g_stage; }

// ============================================================
// 日志：沙盒文件 + os_log
// ============================================================
static const char *kLogTag = "settingsframework";
static FILE *g_logFile = nullptr;
static os_log_t g_osLog = nullptr;
static char g_logPath[512] = {0};

// 日志锁：多线程（主线程 + 各诊断/看门狗线程）会并发写同一个 FILE*。
// 不加锁时 fprintf/fflush 交错，且一旦某线程在读野指针处崩溃于持内部锁状态，
// 其余线程会永久阻塞在该锁上——实机表现为「随机卡在 EA 标志、必须重启」。
// 递归锁：log_open_locked 内部可能再触发 log_write。
// Darwin 提供 PTHREAD_RECURSIVE_MUTEX_INITIALIZER 的静态初值，此处用它避免运行期初始化竞态。
static pthread_mutex_t g_logMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

struct LogLockGuard {
    LogLockGuard()  { pthread_mutex_lock(&g_logMutex); }
    ~LogLockGuard() { pthread_mutex_unlock(&g_logMutex); }
};

static void log_open_locked() {
    if (g_logFile) return;
    g_osLog = os_log_create("com.scode.lztsettingsframework", "default");
    @autoreleasepool {
        NSString *dir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                             NSUserDomainMask, YES) firstObject];
        if (!dir) return;
        NSString *p = [dir stringByAppendingPathComponent:@"lztsettingsframework.log"];
        strncpy(g_logPath, [p UTF8String], sizeof(g_logPath) - 1);

        /* 【v14 日志策略】每次启动一个独立文件。
         *
         * 旧策略的问题（实测）：追加模式把多次启动写进同一文件，越滚越大；
         * 朋友取日志时经常发错——要么发了旧的（分不清哪次是出问题的），
         * 要么漏发（不知道该发哪个）。根因是"哪个是最新"不直观。
         *
         * 新策略：
         *   1) 当前日志固定名为 lztsettingsframework.log（每次启动清空重建）；
         *   2) 启动时把上一次的日志归档为 lztsettingsframework-<mtime>.log，
         *      文件名含时间戳（yyyyMMdd-HHmmss，字典序即时间序）；
         *   3) 归档只保留最近 5 个，更早的自动删除，防止无限增长。
         *
         * 于是"要发哪个"永远只有一个答案：lztsettingsframework.log。
         * 若卡死发生在更早的启动，可按时间戳在同目录找到对应归档。 */
        NSFileManager *fm = [NSFileManager defaultManager];

        // 清理旧版轮转遗留（原先的 4MB 轮转产物 lztsettingsframework.log.1）
        [fm removeItemAtPath:[p stringByAppendingString:@".1"] error:nil];

        if ([fm fileExistsAtPath:p]) {
            NSDictionary *oldAttrs = [fm attributesOfItemAtPath:p error:nil];
            NSDate *mtime = oldAttrs ? oldAttrs[NSFileModificationDate] : nil;
            if (mtime == nil) mtime = [NSDate date];
            NSDateFormatter *df = [[NSDateFormatter alloc] init];
            [df setLocale:[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"]];
            [df setDateFormat:@"yyyyMMdd-HHmmss"];
            NSString *stamp = [df stringFromDate:mtime];
            NSString *arc = [dir stringByAppendingPathComponent:
                             [NSString stringWithFormat:@"lztsettingsframework-%@.log", stamp]];
            [fm removeItemAtPath:arc error:nil];          // 秒级撞名则覆盖
            [fm moveItemAtPath:p toPath:arc error:nil];   // 失败则下面 "w" 直接截断
        }

        // 归档超额清理：按文件名（=时间顺序）保留最新 5 个
        {
            NSArray *entries = [fm contentsOfDirectoryAtPath:dir error:nil];
            NSMutableArray *arcs = [NSMutableArray array];
            for (NSString *name in entries) {
                if ([name hasPrefix:@"lztsettingsframework-"] &&
                    [name hasSuffix:@".log"]) {
                    [arcs addObject:name];
                }
            }
            if (arcs.count > 5) {
                [arcs sortUsingSelector:@selector(compare:)];
                const NSUInteger drop = arcs.count - 5;
                for (NSUInteger i = 0; i < drop; ++i) {
                    [fm removeItemAtPath:[dir stringByAppendingPathComponent:arcs[i]]
                                   error:nil];
                }
            }
        }

        g_logFile = fopen(g_logPath, "w");
        if (g_logFile) {
            /* 新文件每次启动重建，BOOT 行仍保留：便于与操作时间对齐，
             * 也让单文件被单独发送时自带启动时刻。 */
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            struct tm tmv;
            time_t sec = (time_t)tv.tv_sec;
            localtime_r(&sec, &tmv);
            char stamp[64];
            snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                     (int)(tv.tv_usec / 1000));
            fprintf(g_logFile, "\n===== BOOT %s =====\n", stamp);
            fflush(g_logFile);
        }
    }
}

void log_init() {
    LogLockGuard guard;
    log_open_locked();
}

void log_write_v(const char *fmt, va_list ap) {
    LogLockGuard guard;
    log_open_locked();
    char body[2048];
    vsnprintf(body, sizeof(body), fmt, ap);
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", body);
        fflush(g_logFile);   // 立即落盘，防崩溃丢日志
    }
    if (g_osLog) {
        os_log_with_type(g_osLog, OS_LOG_TYPE_DEFAULT, "%{public}s", body);
    }
}

void log_write(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write_v(fmt, ap);
    va_end(ap);
}

// ============================================================
// 崩溃诊断
// ============================================================
// 崩溃现场寄存器与调用栈（崩溃时进程状态已不可信，只做只读读取并严格限界，
// 目的是把「在哪条链路上崩」一次抓清，避免反复往返真机却拿不到线索）。
static void crash_format_backtrace(void *uctx, char *out, size_t cap) {
    const uintptr_t base = (uintptr_t)g_base.load();
    uintptr_t pc = 0, lr = 0, fp = 0, sp = 0;
    if (uctx != nullptr) {
        ucontext_t *uc = reinterpret_cast<ucontext_t *>(uctx);
        pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
        lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
        fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
        sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
    }
    int off = snprintf(out, cap,
                       "NATIVE_CRASH_PC pc=%p(+0x%lx) lr=%p(+0x%lx) fp=%p sp=%p bt:",
                       (void *)pc, (unsigned long)(pc - base),
                       (void *)lr, (unsigned long)(lr - base),
                       (void *)fp, (void *)sp);
    int steps = 0;
    while (steps < 24 && fp != 0 && off > 0 && off < (int)cap - 48) {
        if ((fp & 0x7) != 0) break;                    // 必须 8 字节对齐
        const uintptr_t *f = reinterpret_cast<const uintptr_t *>(fp);
        const uintptr_t nfp = f[0];
        const uintptr_t nlr = f[1];
        off += snprintf(out + off, cap - (size_t)off, " +0x%lx",
                        (unsigned long)(nlr - base));
        ++steps;
        if (nfp <= fp) break;                          // 必须向高地址推进
        fp = nfp;
    }
    if (off > 0 && off < (int)cap - 2) { out[off] = '\n'; out[off + 1] = 0; }
}

static void crash_handler(int sig, siginfo_t *info, void *uctx) {
    // 崩溃信息必须写【独立且追加】的文件：
    // 主日志每次启动以 "w" 打开会截断，一旦崩溃后重启，现场就没了
    // （实测为此多花了一轮排查）。这里单独写 lztsettingsframework_crash.log。
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "NATIVE_CRASH sig=%d addr=%p stage=%d base=0x%lx\n",
                     sig, info ? info->si_addr : nullptr, (int)g_stage,
                     (unsigned long)g_base.load());
    if (g_logPath[0]) {
        // 由 "lztsettingsframework.log" 推出崩溃日志文件名
        char crashPath[520];
        const char* dot = strstr(g_logPath, ".log");
        if (dot != nullptr) {
            const size_t prefix = (size_t)(dot - g_logPath);
            snprintf(crashPath, sizeof(crashPath), "%.*s_crash.log", (int)prefix, g_logPath);
        } else {
            snprintf(crashPath, sizeof(crashPath), "%s.crash", g_logPath);
        }
        int fd = open(crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t r = write(fd, buf, (size_t)n);
            (void)r;
            char bt[1024];
            crash_format_backtrace(uctx, bt, sizeof(bt));
            r = write(fd, bt, strlen(bt));
            (void)r;
            close(fd);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_crash_diagnostics() {
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
}

void start_crash_handler_keepalive() {
    // iOS 上游戏同样可能在启动后重装信号处理器，这里保持与 Android 一致的语义：
    // 由调用方在若干时机重复调用 install_crash_diagnostics() 即可（本函数为空实现）。
}

// ============================================================
// 内存可读性检查（iOS：mach vm_region_64）
// ============================================================
bool memory_range_accessible(uintptr_t address, size_t size, bool writable) {
    if (address == 0 || size == 0) return false;

    vm_address_t region = (vm_address_t)address;
    vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;

    kern_return_t kr = vm_region_64(mach_task_self(), &region, &regionSize,
                                    VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&info, &count, &object);
    if (kr != KERN_SUCCESS) return false;

    // 必须是同一段映射内的完整区间
    if (address < region) return false;
    if ((address + size) > (region + regionSize)) return false;

    if ((info.protection & VM_PROT_READ) == 0) return false;
    if (writable && (info.protection & VM_PROT_WRITE) == 0) return false;
    return true;
}

// ============================================================
// 进程信息
// ============================================================
const char *package_name() {
    static char s_buf[256] = {0};
    static bool s_done = false;
    if (!s_done) {
        s_done = true;
        @autoreleasepool {
            NSString *bid = [[NSBundle mainBundle] bundleIdentifier];
            if (bid) strncpy(s_buf, [bid UTF8String], sizeof(s_buf) - 1);
        }
    }
    return s_buf;
}

bool files_dir(char *out, size_t outSize) {
    if (!out || outSize == 0) return false;
    @autoreleasepool {
        NSString *d = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                           NSUserDomainMask, YES) firstObject];
        if (!d) return false;
        strncpy(out, [d UTF8String], outSize - 1);
        out[outSize - 1] = 0;
        return true;
    }
}

// 游戏的外部资源目录。
// 注意：该目录已被重定向到 Documents/No_Backup——
// 游戏的资源路径对象在 0x1c32040 处构造，其 Application Support 成员会在
// 返回后被改写为 Documents（见 lzt_main_ios.cpp 的 NBPATH 处理）。
// 本模块必须与该重定向保持一致，否则会出现「游戏读新位置、模块读旧位置」。
bool cdn_dir(char *out, size_t outSize) {
    if (!out || outSize < 2) return false;
    out[0] = 0;
    @autoreleasepool {
        /* 该函数会被语言表预热轮询调用，对每一步取到的对象与字符串都做
         * 空值/长度校验，避免向栈上的定长缓冲越界写：
         *   · 实机曾出现一次 NATIVE_CRASH sig=10(SIGBUS) stage=221，
         *     stage 221 正是本函数被调用处（resource_file.cpp）。
         *   · 崩溃地址为页对齐值 0x16b548000，高于 sp 约 6.9KB，
         *     更像访问未映射页而非解引用空指针——真实根因尚未确证。
         * 因此这里只做「取不到就失败」的防御，不假设具体成因：
         * 任何一步拿不到有效值都返回 false，由调用方走"资源不可用"分支。 */
        NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                            NSUserDomainMask, YES);
        if (dirs == nil || dirs.count == 0) return false;
        NSString *base = [dirs objectAtIndex:0];
        if (base == nil) return false;
        NSString *p = [base stringByAppendingPathComponent:@"No_Backup"];
        if (p == nil) return false;
        const char *utf8 = [p UTF8String];
        if (utf8 == nullptr) return false;
        const size_t n = strlen(utf8);
        if (n == 0 || n >= outSize) return false;   /* 放不下就拒绝，不截断 */
        memcpy(out, utf8, n);
        out[n] = 0;
        return true;
    }
}

// ============================================================
// 主程序基址解析（供 applyHooks 调用）
// ============================================================
// 越狱环境下 dyld 镜像 0 常被 systemhook.dylib 占据，因此不能取 image[0]。
// 按主 bundle 可执行文件路径在镜像表里精确匹配，再退化为路径以 "/PvZ2" 结尾。
static int main_image_index() {
    static int s_idx = -2;                 /* -2 = 未解析；-1 = 上次失败（可重试） */
    if (s_idx >= 0) return s_idx;          /* 只在【成功】后缓存 */

    /* 失败不缓存：dyld 加载本库时主镜像可能尚未出现在 _dyld_image_count 列表里，
     * 若把这次的 -1 缓存住，之后无论主镜像何时就绪都永远返回失败，
     * 进而让 wait_and_start 空转 300×100ms=30s 后 abort —— 表现为「EA 界面
     * 停留极久、偶发永久卡住」，且随设备启动速度变化（越快越容易踩中）。 */
    s_idx = -1;

    uint32_t n = _dyld_image_count();
    @autoreleasepool {
        NSString *exec = [[NSBundle mainBundle] executablePath];
        const char *want = exec ? [exec UTF8String] : nullptr;
        if (want) {
            for (uint32_t i = 0; i < n; ++i) {
                const char *name = _dyld_get_image_name(i);
                if (name && strcmp(name, want) == 0) { s_idx = (int)i; return s_idx; }
            }
        }
        for (uint32_t i = 0; i < n; ++i) {
            const char *name = _dyld_get_image_name(i);
            if (!name) continue;
            size_t len = strlen(name);
            if (len >= 5 && strcmp(name + len - 5, "/PvZ2") == 0) {
                s_idx = (int)i;
                return s_idx;
            }
        }
    }
    return s_idx;
}

uintptr_t main_image_slide() {
    int idx = main_image_index();
    if (idx < 0) return 0;
    return (uintptr_t)_dyld_get_image_vmaddr_slide(idx);
}

const char *main_image_path() {
    int idx = main_image_index();
    if (idx < 0) return "";
    const char *p = _dyld_get_image_name(idx);
    return p ? p : "";
}

// 主程序 __TEXT 的运行时基址（= slide + 0x100000000）
uintptr_t main_text_base() {
    uintptr_t slide = main_image_slide();
    if (slide == 0) return 0;
    return slide + 0x100000000ULL;
}

} // namespace lzt_core

// 主程序镜像索引（供 fishhook 复用同一套判定，避免两处索引不一致）。
extern "C" int lzt_ios_main_index_public(void) {
    return lzt_core::main_image_index();
}

// ============================================================
// 资源目录重定向：Library/Application Support/No_Backup → Documents/No_Backup
//
// 为什么用符号链接而不是代码层 patch：
//   1) Application Support 下除 No_Backup 外还有若干 SDK 目录（Google /
//      GoogleMobileAds / com.crashlytics / com.vungle / gad / vungle 等），
//      重定向父目录会牵连它们；
//   2) No_Backup 的路径构建点不止一处（实测至少 0x101c31030 与 0x101c32040），
//      逐个挂钩既繁琐又容易漏掉间接拼接的路径。
//   符号链接在文件系统层生效，位于游戏代码之下：无论游戏用哪条代码路径、
//   还是第三方 SDK 或直接文件 IO，访问该目录都会落到 Documents/No_Backup。
//
// 安全设计：迁移与建链之间若失败会【回滚】，绝不留下「目录被搬走但链接没建上」
// 的状态（那会让游戏看到空目录）。任何一步失败都保持原状，游戏照常运行。
// ============================================================
extern "C" void lzt_ios_redirect_no_backup_dir() {
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *home = NSHomeDirectory();
        NSString *oldPath = [[home stringByAppendingPathComponent:@"Library"]
                                stringByAppendingPathComponent:@"Application Support"];
        oldPath = [oldPath stringByAppendingPathComponent:@"No_Backup"];
        NSString *newPath = [[home stringByAppendingPathComponent:@"Documents"]
                                stringByAppendingPathComponent:@"No_Backup"];

        // ---- 快路径：已完成配置时只读检查，立即返回 ----
        // 绝大多数启动（以及每次启动后的复检）都走这里：一次 lstat + 一次
        // readlink，无任何写操作。避免每次启动都重复做目录 I/O。
        NSDictionary *attrs = [fm attributesOfItemAtPath:oldPath error:nil];
        if (attrs && [attrs[NSFileType] isEqualToString:NSFileTypeSymbolicLink]) {
            NSString *dest = [fm destinationOfSymbolicLinkAtPath:oldPath error:nil];
            if ([dest isEqualToString:newPath]) return;      // 已就绪
        }

        // ---- 慢路径：需要（重新）建立链接 ----
        // 这里绝不使用「contentsOfDirectoryAtPath(oldPath) + moveItem」的组合：
        // oldPath 若是指向 newPath 的链接，该组合会透过链接读到 newPath 自身，
        // 把自己移到自己身上，随后 removeItemAtPath 删除目标目录——
        // 表现为「用得越久越容易出问题」的数据损坏。改为：只在 oldPath 是
        // 【真实目录】时才迁移，且只搬 newPath 里尚不存在的顶层项。
        const BOOL oldIsLink =
            attrs && [attrs[NSFileType] isEqualToString:NSFileTypeSymbolicLink];
        const BOOL oldExists = !oldIsLink && [fm fileExistsAtPath:oldPath];
        const BOOL newExists = [fm fileExistsAtPath:newPath];
        BOOL moved = NO;

        if (oldIsLink) {
            // 指向别处（或已失效）的链接：先移除，再按常规流程重建
            if (![fm removeItemAtPath:oldPath error:nil]) {
                lzt_core::log_write("no_backup redirect: 移除旧链接失败，保持原状");
                return;
            }
        }

        if (oldExists && !newExists) {
            // 首次迁移：数据在旧位置而新位置没有 → 整体改名（同容器内原子完成）
            NSError *err = nil;
            if ([fm moveItemAtPath:oldPath toPath:newPath error:&err]) {
                moved = YES;
            } else {
                lzt_core::log_write("no_backup redirect: 迁移失败，保持原状: %s",
                                    [[err localizedDescription] UTF8String]);
                return;
            }
        } else if (oldExists && newExists) {
            // 两边都有：只把新目录缺少的顶层项从旧目录补过去，再移除旧目录。
            // 此处 oldPath 已确认为真实目录（oldIsLink 分支已排除链接情形）。
            NSArray *items = [fm contentsOfDirectoryAtPath:oldPath error:nil];
            for (NSString *it in items) {
                NSString *dst = [newPath stringByAppendingPathComponent:it];
                if ([fm fileExistsAtPath:dst]) continue;
                [fm moveItemAtPath:[oldPath stringByAppendingPathComponent:it]
                            toPath:dst error:nil];
            }
            [fm removeItemAtPath:oldPath error:nil];
            moved = YES;
        } else if (!newExists) {
            // 4) 两边都没有：新建目标目录
            [fm createDirectoryAtPath:newPath withIntermediateDirectories:YES
                           attributes:nil error:nil];
        }

        // 5) 建链接；失败则回滚刚才的迁移
        NSError *err = nil;
        if (![fm createSymbolicLinkAtPath:oldPath withDestinationPath:newPath error:&err]) {
            lzt_core::log_write("no_backup redirect: 建链接失败，回滚: %s",
                                [[err localizedDescription] UTF8String]);
            if (moved) {
                [fm moveItemAtPath:newPath toPath:oldPath error:nil];
                lzt_core::log_write("no_backup redirect: 已回滚到原位置");
            }
            return;
        }
        lzt_core::log_write("no_backup redirect: %s -> %s (symbolic link)",
                            [oldPath UTF8String], [newPath UTF8String]);
    }
}
// ============================================================
// 确保 iCloud 备份排除属性生效（文档《No_Backup 迁移至 Documents》§3.4 验收项）
//
// 为什么需要独立函数：上面的 redirect 函数在第 1 步「已是正确符号链接 → 完成」
// 会直接 return，导致后续代码在已迁移的环境下永不执行。
//
// 游戏自身对 No_Backup/Backup 设置排除属性的路径是
//   0x1c32040 → 0x1c366ec / 0x1c36890 → setxattr("com.apple.MobileBackup", "\x01", 1)
// 但那两处调用带条件分支（0x1c9ea54 的返回值须 ==5 或 >=6），
// 本机实测未走该分支 —— 结果两个目录都没有排除属性，
// 与「No_Backup = 不参与 iCloud 备份」的语义相反。
// 这里显式补设，保证语义成立。
// ============================================================
extern "C" void lzt_ios_ensure_backup_exclusion() {
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *home = NSHomeDirectory();
        NSString *docs = [home stringByAppendingPathComponent:@"Documents"];
        NSArray<NSString *> *dirs = @[
            [docs stringByAppendingPathComponent:@"No_Backup"],
            [docs stringByAppendingPathComponent:@"Backup"],
        ];
        for (NSString *dir in dirs) {
            BOOL isDir = NO;
            if (![fm fileExistsAtPath:dir isDirectory:&isDir]) {
                [fm createDirectoryAtPath:dir withIntermediateDirectories:YES
                               attributes:nil error:nil];
            }
            NSError *e = nil;
            NSURL *u = [NSURL fileURLWithPath:dir];
            const BOOL ok = [u setResourceValue:@YES
                                         forKey:NSURLIsExcludedFromBackupKey
                                          error:&e];
            lzt_core::log_write("icloud exclude: %s -> %s%s",
                                [dir UTF8String], ok ? "ok" : "FAIL",
                                ok ? "" : (e ? [[e localizedDescription] UTF8String] : ""));
        }
    }
}

// ============================================================
// link.h 垫片实现：dl_iterate_phdr（供共享源码按名字找游戏库）
//
// 安卓侧遍历 loaded so 并匹配 "libPVZ2.so"；iOS 的游戏代码在主程序里，
// 因此除按真实路径报告每个镜像外，还会把【主程序镜像】以 "libPVZ2.so"
// 这个名字再报告一次，使共享源码里的 strstr 匹配无需任何改动。
// dlpi_addr 语义与安卓一致：镜像的加载偏移（load bias）。
// ============================================================
extern "C" int dl_iterate_phdr(lzt_dl_iterate_cb callback, void *data) {
    if (callback == nullptr) return 0;
    const int mainIdx = lzt_core::main_image_index();
    const uint32_t n = _dyld_image_count();

    for (uint32_t i = 0; i < n; ++i) {
        struct dl_phdr_info info;
        // 关键：dlpi_addr 必须与「库基址」同义（游戏地址 = dlpi_addr + 偏移）。
        // 安卓的 dlpi_addr 就是 so 的加载基址；iOS 主可执行文件的 __TEXT 位于
        // vmaddr 0x100000000，因此这里必须给【已加 slide 的镜像起始地址】
        // （即 mach 头所在地址），而不是 _dyld_get_image_vmaddr_slide。
        // 早期版本误用 slide，导致 base+OFF_G_DisplayInfo 少算 0x100000000，
        // 读到未映射地址 → 启动即崩。
        const struct mach_header* hdr = _dyld_get_image_header(i);
        info.dlpi_addr = reinterpret_cast<uintptr_t>(hdr);
        info.dlpi_name = _dyld_get_image_name(i);
        if (callback(&info, sizeof(info), data) != 0) return 1;

        // 主程序镜像：额外以安卓侧的库名报告一次（见文件头说明）
        if (mainIdx >= 0 && static_cast<int>(i) == mainIdx) {
            struct dl_phdr_info alias = info;
            alias.dlpi_name = "libPVZ2.so";
            if (callback(&alias, sizeof(alias), data) != 0) return 1;
        }
    }
    return 0;
}

// ============================================================
// 兼容垫片符号（供 iOS/port/include/android/log.h 转接）
//
// Android 侧源码（如 And64InlineHook.cpp）写的是 __android_log_print，
// 在 iOS 上由垫片头转到这里，最终汇入 lzt_core 的统一日志（沙盒文件 + os_log）。
// ============================================================
extern "C" int lzt_ios_log_vwrite(int prio, const char *tag, const char *fmt, va_list ap);

// 安排一次「当前事件处理结束后」的页面重建（主队列延后任务）。
// 调用方：settings_framework 的 dispatch 命中控件之后。
// 之所以必须延后：那一刻仍在事件处理调用栈内、控件正被使用，立即重建会 UAF。
extern "C" void lzt_ios_defer_rebuild(uintptr_t page) {
    static std::atomic<uintptr_t> pending{0};
    if (page == 0) return;
    if (pending.exchange(page) == page) return;   // 同一页合并为一次
    dispatch_async(dispatch_get_main_queue(), ^{
        const uintptr_t p = pending.exchange(0);
        if (p != 0) {
            lzt_core::log_write("[SFW] deferred rebuild: page=%p", (void*)p);
            lzt_settings::ios_try_rebuild_dirty(p);
        }
    });
}

// 屏幕宽高比（横屏：长边/短边）。UIScreen 的 bounds 为竖屏朝向，
// 而游戏为横屏，故取长边除以短边。
// 屏幕宽（像素）。横屏游戏下取长边——对齐合理性检查用它做上界。
extern "C" int lzt_ios_screen_width_px() {
    @autoreleasepool {
        CGRect b = [[UIScreen mainScreen] bounds];
        CGFloat s = [[UIScreen mainScreen] scale];
        int w = (int)(b.size.width * s);
        int h = (int)(b.size.height * s);
        return w > h ? w : h;
    }
}

extern "C" float lzt_ios_screen_aspect_ratio() {
    @autoreleasepool {
        CGRect b = [[UIScreen mainScreen] bounds];
        CGFloat s = [[UIScreen mainScreen] scale];
        int w = (int)(b.size.width * s);
        int h = (int)(b.size.height * s);
        if (w <= 0 || h <= 0) return 0.0f;
        int lo = w < h ? w : h;
        int hi = w < h ? h : w;
        return (float)hi / (float)lo;
    }
}

extern "C" int lzt_ios_log_write(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    char body[2048];
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    lzt_core::log_write("[%s] %s", tag ? tag : "-", body);
    return 0;
}

extern "C" int lzt_ios_log_vwrite(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    char body[2048];
    vsnprintf(body, sizeof(body), fmt, ap);
    lzt_core::log_write("[%s] %s", tag ? tag : "-", body);
    return 0;
}

// ============================================================
// 帧率诊断探针（第一轮：只测量，零改动）
//
// 依据 metalprobe 实测（CAMetalLayer 实例方法 63 个）：
//   · nextDrawable                 每帧必调（拿 drawable 才能渲染）
//   · allowsNextDrawableTimeout    drawable 等待超时开关
//   · setDrawableTimeoutSeconds:  超时时长（与帧率强相关）
//   · presentsWithTransaction       呈现事务模式
//   · maximumDrawableCount          drawable 缓冲深度
// present 本身在 CAMetalDrawable 上（present / presentWithOptions:）。
//
// 用 method_setImplementation 而非 inline hook：nextDrawable 实现位于
// dyld 共享缓存（QuartzCore），inline 补丁需 20 字节远跳，实测启动即崩
// （FINDINGS §20.6 已记录同类教训）。swizzle 只改运行时方法表指向。
//
// 测量目标：
//   ① 游戏实际帧间隔（预期 ~33.3ms = 30fps）
//   ② 屏幕刷新基准（runloop observer，粗参考）
//   ③ layer 配置（判断是否走「非事务 + 超时等待」这条会被限帧的路）
// ============================================================
namespace {

std::atomic<uint64_t> g_fpsLastFrameNs{0};
std::atomic<uint64_t> g_fpsLastDispNs{0};
std::atomic<bool>      g_fpsActive{false};

// 2 秒窗口统计
std::atomic<uint64_t> g_fpsWinFrames{0};
std::atomic<uint64_t> g_fpsWinSumNs{0};
std::atomic<uint64_t> g_fpsWinMinNs{UINT64_MAX};
std::atomic<uint64_t> g_fpsWinMaxNs{0};
std::atomic<uint64_t> g_fpsWinDisp{0};
std::atomic<uint64_t> g_fpsWinDispSumNs{0};

// 原 IMP（swizzle 前保存）
id (*g_fpsOrigNextDrawable)(id, SEL) = nullptr;

std::atomic<uintptr_t> g_fpsProbeLayer{0};
std::atomic<int>       g_fpsCfgDumps{0};

uint64_t fps_now_ns() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    return static_cast<uint64_t>(mach_absolute_time()) * tb.numer / tb.denom;
}

void fps_record(uint64_t now, std::atomic<uint64_t>& winCnt,
                std::atomic<uint64_t>& winSum) {
    const uint64_t last = g_fpsLastFrameNs.exchange(now);
    if (last == 0) return;                       // 首帧只记基线
    const uint64_t dt = now - last;
    if (dt == 0 || dt > 500u * 1000 * 1000) return;   // 丢异常值
    winCnt.fetch_add(1, std::memory_order_relaxed);
    winSum.fetch_add(dt, std::memory_order_relaxed);
    uint64_t m = g_fpsWinMinNs.load(std::memory_order_relaxed);
    while (dt < m && !g_fpsWinMinNs.compare_exchange_weak(m, dt)) {}
    m = g_fpsWinMaxNs.load(std::memory_order_relaxed);
    while (dt > m && !g_fpsWinMaxNs.compare_exchange_weak(m, dt)) {}
}

id hk_fps_nextDrawable(id self, SEL _cmd) {
    if (g_fpsActive.load(std::memory_order_relaxed)) {
        fps_record(fps_now_ns(), g_fpsWinFrames, g_fpsWinSumNs);
        if (g_fpsProbeLayer.load() == 0) g_fpsProbeLayer.store((uintptr_t)self);
    }
    return g_fpsOrigNextDrawable ? g_fpsOrigNextDrawable(self, _cmd) : nil;
}

void hk_fps_display_loop(CFRunLoopObserverRef obs, CFRunLoopActivity act, void* info) {
    if (!g_fpsActive.load(std::memory_order_relaxed)) return;
    const uint64_t now = fps_now_ns();
    const uint64_t last = g_fpsLastDispNs.exchange(now);
    if (last == 0) return;
    const uint64_t dt = now - last;
    if (dt == 0 || dt > 500u * 1000 * 1000) return;
    g_fpsWinDisp.fetch_add(1, std::memory_order_relaxed);
    g_fpsWinDispSumNs.fetch_add(dt, std::memory_order_relaxed);
}

} // namespace

extern "C" void lzt_ios_install_fps_probe() {
    if (g_fpsActive.load()) return;

    Class cls = objc_getClass("CAMetalLayer");
    if (!cls) {
        lzt_core::log_write("FPSPROBE: no CAMetalLayer class");
        return;
    }
    Method m = class_getInstanceMethod(cls, @selector(nextDrawable));
    if (!m) {
        lzt_core::log_write("FPSPROBE: no nextDrawable method");
        return;
    }
    g_fpsOrigNextDrawable = (id (*)(id, SEL))method_getImplementation(m);
    method_setImplementation(m, (IMP)hk_fps_nextDrawable);
    lzt_core::log_write("FPSPROBE: nextDrawable hooked, orig=%p",
                        reinterpret_cast<void*>(g_fpsOrigNextDrawable));

    CFRunLoopObserverContext ctx{};
    CFRunLoopObserverRef obs = CFRunLoopObserverCreate(
        kCFAllocatorDefault, kCFRunLoopBeforeWaiting, true, 0, hk_fps_display_loop, &ctx);
    if (obs) {
        CFRunLoopAddObserver(CFRunLoopGetMain(), obs, kCFRunLoopCommonModes);
        CFRelease(obs);
        lzt_core::log_write("FPSPROBE: main runloop observer installed");
    }

    g_fpsActive.store(true);
    lzt_core::log_write("FPSPROBE: active");
}

// 每 2 秒由主队列定时器调用一次
extern "C" void lzt_ios_fps_report_tick() {
    if (!g_fpsActive.load(std::memory_order_relaxed)) return;

    const uint64_t pc   = g_fpsWinFrames.exchange(0);
    const uint64_t ps   = g_fpsWinSumNs.exchange(0);
    const uint64_t pmin = g_fpsWinMinNs.exchange(UINT64_MAX);
    const uint64_t pmax = g_fpsWinMaxNs.exchange(0);
    const uint64_t dc   = g_fpsWinDisp.exchange(0);
    const uint64_t ds   = g_fpsWinDispSumNs.exchange(0);

    auto fps = [](uint64_t cnt, uint64_t sumNs) {
        return cnt ? 1e9 / (static_cast<double>(sumNs) / cnt) : 0.0;
    };
    auto ms = [](uint64_t v) { return static_cast<double>(v) / 1e6; };

    lzt_core::log_write(
        "FPSPROBE: game=%llu f (%.2f fps, avg %.2f ms, min %.2f, max %.2f) | loop=%llu (%.2f fps)",
        (unsigned long long)pc, fps(pc, ps), pc ? ms(ps / pc) : 0.0,
        pmin == UINT64_MAX ? 0.0 : ms(pmin), ms(pmax),
        (unsigned long long)dc, fps(dc, ds));

    // 首次拿到 layer 后 dump 3 次配置
    const uintptr_t layer = g_fpsProbeLayer.load();
    if (layer != 0 && g_fpsCfgDumps.fetch_add(1) < 3) {
        CAMetalLayer* ml = (__bridge CAMetalLayer*)(void*)layer;
        lzt_core::log_write(
            "FPSPROBE layer: presentsWithTransaction=%d allowsTimeout=%d timeoutSec=%.3f size=%.0fx%.0f maxDrawable=%llu",
            ml.presentsWithTransaction ? 1 : 0,
            ([ml respondsToSelector:@selector(allowsNextDrawableTimeout)] ? [[ml valueForKey:@"allowsNextDrawableTimeout"] boolValue] : 0) ? 1 : 0,
            [ml respondsToSelector:@selector(drawableTimeoutSeconds)]
                ? [[ml valueForKey:@"drawableTimeoutSeconds"] doubleValue] : -1.0,
            ml.drawableSize.width, ml.drawableSize.height,
            ([ml respondsToSelector:@selector(maximumDrawableCount)] ? [[ml valueForKey:@"maximumDrawableCount"] unsignedLongLongValue] : 0ULL));
    }
}
// ============================================================
// 渲染路径运行时探测：列出进程内与渲染相关的 ObjC 类实例
//
// 思路：静态分析（binary strings / dylib list）只能知道"链接了什么"，
// 不能知道"实际用了什么"。直接在运行时枚举：
//   ① 所有 CA*Layer 子类实例（含 MTKView 内部的 CAMetalLayer）
//   ② EAGLContext / MTLDevice / MTLCommandQueue 实例
//   ③ UIWindow 的 layer 树（找游戏真正的渲染载体）
// ============================================================
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <OpenGLES/ES3/gl.h>
#import <UIKit/UIKit.h>
#include <objc/runtime.h>
#include <mach/mach_time.h>
#include <cstdint>
#include <atomic>

namespace {

// 只统计，不打每帧日志；达到采样次数后一次性 dump
std::atomic<int> g_rtMtlLayer{0};
std::atomic<int> g_rtCaMetal{0};
std::atomic<int> g_rtEagl{0};
std::atomic<int> g_rtMtkView{0};
std::atomic<int> g_rtGllayer{0};
std::atomic<int> g_rtSamples{0};

void rt_count_layer(id layer) {
    if (layer == nil) return;
    Class c = object_getClass(layer);
    if (c == nil) return;
    const char* cn = class_getName(c);
    if (cn == nullptr) return;

    if (strcmp(cn, "CAMetalLayer") == 0) g_rtCaMetal.fetch_add(1);
    else if (strcmp(cn, "CAEAGLLayer") == 0) g_rtGllayer.fetch_add(1);
    // MTKView 是 UIView 子类，其 layer 是 CAMetalLayer（由 MTKView 设置）
}

} // namespace

// 在 layer 创建之后周期性调用（由主队列定时器驱动，采样 5 次）
extern "C" void lzt_ios_rtprobe_tick() {
    if (g_rtSamples.fetch_add(1) > 5) return;

    // ① 遍历所有 UIWindow，dump layer 树（限深度 3）
    int metalLayers = 0, eagLayers = 0, mtkViews = 0;

    for (UIWindow* w in [UIApplication sharedApplication].windows) {
        if (w == nil) continue;
        // window.layer
        rt_count_layer(w.layer);
        // 递归子 view（限 2 层，避免太深）
        NSArray<UIView*>* subs = w.subviews;
        for (UIView* v in subs) {
            rt_count_layer(v.layer);
            if ([v isKindOfClass:NSClassFromString(@"MTKView")]) ++mtkViews;
            for (UIView* vv in v.subviews) {
                rt_count_layer(vv.layer);
                if ([vv isKindOfClass:NSClassFromString(@"MTKView")]) ++mtkViews;
            }
        }
    }

    // ② 全局类实例统计（通过 objc runtime 拿已注册类的实例数不可行，
    //    改为查关键类的类方法是否存在 + 默认设备是否可用）
    id mtlDevice = [MTLCreateSystemDefaultDevice() retain];
    id eaglCtx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];

    lzt_core::log_write(
        "RTPROBE[%d]: windows=%lu mtkView=%d mtlDevice=%p eaglCtx=%p",
        g_rtSamples.load(),
        (unsigned long)[UIApplication sharedApplication].windows.count,
        mtkViews, (void*)mtlDevice, (void*)eaglCtx);

    [mtlDevice release];
    [eaglCtx release];
}

// ============================================================
// 帧率探针 v2：改 hook CAMetalDrawable 的 -present
//
// 依据 metalprobe + eaglprobe 实测：
//   · CAMetalLayer.nextDrawable 被 hook 但 24 秒零调用 → 游戏不走公开路径
//   · CAEAGLLayer / GLKView 类在进程内不存在 → 不用 OpenGL ES layer
//   · CAMetalDrawable 有 present / presentWithOptions: 方法
//     （MTLCommandBuffer presentDrawable: 内部最终调它们）
// ============================================================
namespace {

std::atomic<uint64_t> g_p2LastNs{0};
std::atomic<uint64_t> g_p2Win{0};
std::atomic<uint64_t> g_p2Sum{0};
std::atomic<bool>      g_p2Active{false};
std::atomic<bool>      g_p2Installed{false};

void (*g_p2OrigPresent)(id, SEL) = nullptr;
void (*g_p2OrigPresentOpts)(id, SEL, NSUInteger) = nullptr;

uint64_t p2_now_ns() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    return static_cast<uint64_t>(mach_absolute_time()) * tb.numer / tb.denom;
}

void p2_record() {
    const uint64_t now = p2_now_ns();
    const uint64_t last = g_p2LastNs.exchange(now);
    if (last == 0) return;
    const uint64_t dt = now - last;
    if (dt == 0 || dt > 500u * 1000 * 1000) return;
    g_p2Win.fetch_add(1, std::memory_order_relaxed);
    g_p2Sum.fetch_add(dt, std::memory_order_relaxed);
}

void hk_p2_present(id self, SEL _cmd) {
    if (g_p2Active.load(std::memory_order_relaxed)) p2_record();
    if (g_p2OrigPresent) g_p2OrigPresent(self, _cmd);
}

void hk_p2_presentOpts(id self, SEL _cmd, NSUInteger opts) {
    if (g_p2Active.load(std::memory_order_relaxed)) p2_record();
    if (g_p2OrigPresentOpts) g_p2OrigPresentOpts(self, _cmd, opts);
}

} // namespace

extern "C" void lzt_ios_install_fps_probe2() {
    if (g_p2Installed.load()) return;
    Class cls = objc_getClass("CAMetalDrawable");
    if (!cls) {
        lzt_core::log_write("FPSPROBE2: no CAMetalDrawable");
        return;
    }
    Method m1 = class_getInstanceMethod(cls, @selector(present));
    if (m1) {
        g_p2OrigPresent = (void (*)(id, SEL))method_getImplementation(m1);
        method_setImplementation(m1, (IMP)hk_p2_present);
        lzt_core::log_write("FPSPROBE2: present hooked orig=%p",
                            reinterpret_cast<void*>(g_p2OrigPresent));
    }
    Method m2 = class_getInstanceMethod(cls, @selector(presentWithOptions:));
    if (m2) {
        g_p2OrigPresentOpts = (void (*)(id, SEL, NSUInteger))method_getImplementation(m2);
        method_setImplementation(m2, (IMP)hk_p2_presentOpts);
        lzt_core::log_write("FPSPROBE2: presentWithOptions: hooked orig=%p",
                            reinterpret_cast<void*>(g_p2OrigPresentOpts));
    }
    g_p2Installed.store(true);
    g_p2Active.store(true);
}

extern "C" void lzt_ios_fps_report_tick2() {
    if (!g_p2Active.load(std::memory_order_relaxed)) return;
    const uint64_t pc = g_p2Win.exchange(0);
    const uint64_t ps = g_p2Sum.exchange(0);
    if (pc == 0) {
        lzt_core::log_write("FPSPROBE2: present=0 f (no drawable presented)");
        return;
    }
    lzt_core::log_write("FPSPROBE2: present=%llu f (%.2f fps, avg %.2f ms)",
                        (unsigned long long)pc,
                        1e9 / (static_cast<double>(ps) / pc),
                        static_cast<double>(ps) / pc / 1e6);
}

// ============================================================
// 帧率探针 v3：捕获 CADisplayLink 实例（依据用户提供的线索）
//
// 线索：游戏用 CADisplayLink 驱动主循环，帧间隔由 setFrameInterval: 设定，
// 值来自配置键 "drivers.ios.displaylink_interval"，但代码里直接硬编码了默认值。
//
// 前两轮探测失败的教训：hook CAMetalLayer.nextDrawable / CAMetalDrawable.present
// 都是零调用 —— 因为 Sexy 引擎不是直接用 Metal 呈现 API。
// CADisplayLink 则不同：它是主循环唯一入口，必有 +displayLinkWithTarget:selector:。
//
// 本探针只测量：
//   · 捕获创建出的 CADisplayLink，打印 frameInterval / preferredFramesPerSecond /
//     duration / 屏幕 maximumFramesPerSecond / tick 方法名
//   · 每次报告时用 link.timestamp 差值算真实 tick 频率（不改任何东西）
extern "C" int lzt_ios_fps60_requested();
extern "C" void lzt_ios_dl5_install_for(id, const char*);
extern "C" void lzt_ios_dl4_note_link(CADisplayLink*, const char*);
// ============================================================
namespace {

CADisplayLink* g_dlLink = nil;
double g_dlLastTs = 0.0;
double g_dlLastWall = 0.0;
std::atomic<int> g_dlCount{0};
CADisplayLink* (*g_dlOrigWithTarget)(id, SEL, id, SEL) = nullptr;

CADisplayLink* hk_dl_withTarget(id self, SEL _cmd, id target, SEL sel) {
    CADisplayLink* link = g_dlOrigWithTarget ? g_dlOrigWithTarget(self, _cmd, target, sel) : nil;
    if (link != nil) {
        lzt_ios_dl4_note_link(link, sel ? sel_getName(sel) : nullptr);
        if (sel && strcmp(sel_getName(sel), "mainLoop") == 0) {
            lzt_ios_dl5_install_for(target, "mainLoop");
        }
        const int n = g_dlCount.fetch_add(1);
        if (n < 6) {
            lzt_core::log_write(
                "DLLINK[%d]: target=%p tick=%s frameInterval=%.3f preferredFPS=%.3f "
                "duration=%.5f maxFPS=%.0f",
                n, (void*)target, sel ? sel_getName(sel) : "?",
                (double)link.frameInterval,
                (double)link.preferredFramesPerSecond,
                (double)link.duration,
                (double)[UIScreen mainScreen].maximumFramesPerSecond);
        }
        if (g_dlLink == nil) {
            g_dlLink = [link retain];
            g_dlLastTs = link.timestamp;
            g_dlLastWall = CACurrentMediaTime();
        }
    }
    return link;
}

} // namespace

extern "C" void lzt_ios_install_displaylink_probe() {
    if (g_dlOrigWithTarget != nullptr) return;
    Class cls = objc_getClass("CADisplayLink");
    if (cls == nullptr) {
        lzt_core::log_write("DLLINK: no CADisplayLink class");
        return;
    }
    Method m = class_getClassMethod(cls, @selector(displayLinkWithTarget:selector:));
    if (m == nullptr) {
        lzt_core::log_write("DLLINK: no +displayLinkWithTarget:selector:");
        return;
    }
    g_dlOrigWithTarget = (CADisplayLink* (*)(id, SEL, id, SEL))method_getImplementation(m);
    method_setImplementation(m, (IMP)hk_dl_withTarget);
    lzt_core::log_write("DLLINK: +displayLinkWithTarget:selector: hooked, orig=%p",
                        reinterpret_cast<void*>(g_dlOrigWithTarget));
}

extern "C" void lzt_ios_displaylink_report() {
    if (g_dlLink == nil) {
        if (g_dlCount.load() == 0) {
            lzt_core::log_write("DLLINK: not captured yet (0 created)");
        }
        return;
    }
    const double ts = g_dlLink.timestamp;
    const double now = CACurrentMediaTime();
    const double dts = ts - g_dlLastTs;
    const double dnow = now - g_dlLastWall;
    if (dnow > 0.1 && dts > 0.0) {
        lzt_core::log_write(
            "DLLINK tick=%.2f fps (dts=%.4f over %.3fs) frameInterval=%.3f preferredFPS=%.3f",
            dts / dnow, dts, dnow,
            (double)g_dlLink.frameInterval,
            (double)g_dlLink.preferredFramesPerSecond);
    }
    g_dlLastTs = ts;
    g_dlLastWall = now;
}

// ============================================================
// 帧率探针 v4：抓 setFrameInterval: 的调用者与值 + 精确跟踪 mainLoop 频率
//
// 依据 v3 实测：游戏主循环是 tick=mainLoop 的 CADisplayLink，
// 创建时 frameInterval=1.000、屏幕 maxFPS=60。但用户报告实际 30fps，
// 说明限帧发生在"创建之后"的 setFrameInterval: 调用里。
//
// 本探针：
//   ① hook -[CADisplayLink setFrameInterval:] 打印值与调用栈偏移
//      （偏移 = 返回地址 - 主程序 base，可直接对照反汇编定位）
//   ② hook -[CADisplayLink setPreferredFramesPerSecond:] 同理（iOS10+ 新 API）
//   ③ 只跟踪 tick=mainLoop 的那个 link，用 timestamp 差值算真实频率
// ============================================================
namespace {

void (*g_dl4OrigSetFI)(id, SEL, NSInteger) = nullptr;
void (*g_dl4OrigSetPFS)(id, SEL, NSInteger) = nullptr;
CADisplayLink* g_dl4Main = nil;
double g_dl4LastTs = 0.0;
double g_dl4LastWall = 0.0;
std::atomic<int> g_dl4FiCalls{0};

void hk_dl4_setFrameInterval(id self, SEL _cmd, NSInteger v) {
    NSInteger applied = v;
    if (v == 2 && lzt_ios_fps60_requested()) {
        applied = 1;
        // 只在首次生效时留一行，便于确认功能已启用；不做回溯、不刷屏。
        static std::atomic<int> once{0};
        if (once.fetch_add(1) == 0) {
            lzt_core::log_write("fps60: frameInterval %ld -> %ld", (long)v, (long)applied);
        }
    }
    if (g_dl4OrigSetFI) g_dl4OrigSetFI(self, _cmd, applied);
}

void hk_dl4_setPreferredFPS(id self, SEL _cmd, NSInteger v) {
    if (g_dl4OrigSetPFS) g_dl4OrigSetPFS(self, _cmd, v);
}

} // namespace

extern "C" void lzt_ios_install_dl4() {
#if defined(LZT_STATIC_ONLY)
    /* 静态专用模式：禁止一切运行时修改。
     *
     * 本函数用 method_setImplementation 改写 CADisplayLink 的实现指针，
     * 属于运行时改内存；且 CADisplayLink 是系统类（dyld 共享缓存），
     * 修改影响范围超出本进程，在无越狱环境下不被允许。
     * 关闭后 60fps 不生效——这是排查期取舍，确认稳定后再迁移到静态方案。 */
    lzt_core::log_write("[STATIC-ONLY] dl4 hook skipped; 60fps via static frame-interval patch");
    return;
#endif
    Class cls = objc_getClass("CADisplayLink");
    if (!cls) return;
    Method m1 = class_getInstanceMethod(cls, @selector(setFrameInterval:));
    if (m1) {
        g_dl4OrigSetFI = (void (*)(id, SEL, NSInteger))method_getImplementation(m1);
        method_setImplementation(m1, (IMP)hk_dl4_setFrameInterval);
    }
    Method m2 = class_getInstanceMethod(cls, @selector(setPreferredFramesPerSecond:));
    if (m2) {
        g_dl4OrigSetPFS = (void (*)(id, SEL, NSInteger))method_getImplementation(m2);
        method_setImplementation(m2, (IMP)hk_dl4_setPreferredFPS);
    }
}

// v3 的捕获点也记录 mainLoop link（selector 名为 mainLoop 的那个）
extern "C" void lzt_ios_dl4_note_link(CADisplayLink* link, const char* sel) {
    if (link && sel && strcmp(sel, "mainLoop") == 0 && g_dl4Main == nil) {
        g_dl4Main = [link retain];
        g_dl4LastTs = link.timestamp;
        g_dl4LastWall = CACurrentMediaTime();
    }
}

extern "C" void lzt_ios_dl4_report() {
    if (g_dl4Main == nil) {
        lzt_core::log_write("DL4: mainLoop link not seen yet");
        return;
    }
    const double ts = g_dl4Main.timestamp;
    const double now = CACurrentMediaTime();
    const double dts = ts - g_dl4LastTs;
    const double dnow = now - g_dl4LastWall;
    if (dnow > 0.1 && dts > 0.0) {
        lzt_core::log_write(
            "DL4 mainLoop: %.2f fps (dts=%.4f/%.3fs) frameInterval=%.2f preferredFPS=%.2f "
            "fiCalls=%d",
            dts / dnow, dts, dnow,
            (double)g_dl4Main.frameInterval,
            (double)g_dl4Main.preferredFramesPerSecond,
            g_dl4FiCalls.load());
    }
    g_dl4LastTs = ts;
    g_dl4LastWall = now;
}

// ============================================================
// 帧率探针 v5：定位 mainLoop 实现 + 精确计数
//
// v4 已确认限帧点：0x101c34678 处 [displayLink setFrameInterval:2]，
// 默认值来源为立即数 2（0x101c34654: mov w21, #2）。
//
// 本探针：
//   ① 用 ObjC runtime 取 target 的 mainLoop 实现地址，打印 base 相对偏移，
//      便于直接反汇编查看它内部是否按真实时间步进（决定 60fps 会不会让动画加速）
//   ② swizzle 该实现计数，精确测出回调频率（比 timestamp 可靠）
// ============================================================
namespace {

void (*g_dl5OrigTick)(id, SEL) = nullptr;
std::atomic<uint64_t> g_dl5Ticks{0};
double g_dl5LastWall = 0.0;
std::atomic<bool> g_dl5Installed{false};

void hk_dl5_tick(id self, SEL _cmd) {
    g_dl5Ticks.fetch_add(1, std::memory_order_relaxed);
    if (g_dl5OrigTick) g_dl5OrigTick(self, _cmd);
}

} // namespace

extern "C" void lzt_ios_dl5_install_for(id target, const char* selName) {
    if (g_dl5Installed.load() || target == nil || selName == nullptr) return;
    SEL sel = sel_registerName(selName);
    Class cls = object_getClass(target);          // 实例 → 类
    if (cls == nullptr) return;
    Method m = class_getInstanceMethod(cls, sel);
    if (m == nullptr) {
        lzt_core::log_write("DL5: method %s not found on %s",
                            selName, class_getName(cls));
        return;
    }
    const uintptr_t base = lzt_core::base();
    const uintptr_t imp = (uintptr_t)method_getImplementation(m);
    lzt_core::log_write("DL5: mainLoop class=%s imp=+0x%lx",
                        class_getName(cls),
                        (unsigned long)(base ? imp - base : imp));
    g_dl5OrigTick = (void (*)(id, SEL))imp;
    method_setImplementation(m, (IMP)hk_dl5_tick);
    g_dl5LastWall = CACurrentMediaTime();
    g_dl5Installed.store(true);
    lzt_core::log_write("DL5: mainLoop swizzled for counting");
}

extern "C" void lzt_ios_dl5_report() {
    if (!g_dl5Installed.load()) return;
    const double now = CACurrentMediaTime();
    const double dt = now - g_dl5LastWall;
    if (dt < 0.5) return;
    const uint64_t n = g_dl5Ticks.exchange(0, std::memory_order_relaxed);
    lzt_core::log_write("DL5 mainLoop ticks: %.2f Hz (%llu ticks in %.3fs)",
                        (double)n / dt, (unsigned long long)n, dt);
    g_dl5LastWall = now;
}

// ============================================================
// 60fps：默认启用（把 setFrameInterval: 的 2 改成 1，60Hz 回调）
//   默认     → 开启
//   仅当存在 Documents/lzt_fps60_off → 回到 30Hz，供排障/对照使用
// ============================================================
extern "C" int lzt_ios_fps60_requested() {
    @autoreleasepool {
        NSString* p = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/lzt_fps60_off"];
        return [[NSFileManager defaultManager] fileExistsAtPath:p] ? 0 : 1;
    }
}

// 字形缓存刷新自检开关：存在 Documents/lzt_glyph_selftest 时，
// 启动后主动刷新一次，用于验证「销毁 + 重建」路径（无需 UI 操作）。
extern "C" int lzt_ios_glyph_selftest_requested() {
    @autoreleasepool {
        NSString* p = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/lzt_glyph_selftest"];
        return [[NSFileManager defaultManager] fileExistsAtPath:p] ? 1 : 0;
    }
}

// 图鉴切换刷新开关：默认开启；存在 Documents/lzt_glyph_flush_off 时关闭。
extern "C" int lzt_ios_glyph_flush_enabled() {
    @autoreleasepool {
        NSString* p = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/lzt_glyph_flush_off"];
        return [[NSFileManager defaultManager] fileExistsAtPath:p] ? 0 : 1;
    }
}
