/*
 * lzt_main_ios.cpp — iOS 主模块（对应 Android 的 lzt_settings_framework.cpp）
 *
 * 分层与 Android 严格对应：本文件只承担「机制」——
 *   1. lzt_hooks::install 双平台门面在 iOS 侧的实现（ARM64 → A64HookFunction）
 *   2. 视角 hook 的挂载/卸载（高视角 = 强制 board[280] = 1.0）
 *   3. 初始化入口：等主程序就绪 → 写入基址 → 注册模块 → 安装引擎
 *
 * 与 Android 的差异集中在三处，其余逻辑（阈值判定、对齐公式）完全同源：
 *   · hook 后端：Android 用 And64InlineHook 的 A64HookFunction + 自实现 ARM32；
 *     iOS 只需 ARM64 一支，同样走 And64InlineHook（已适配 iOS 的 W^X 与页大小）
 *   · 基址：iOS 主程序是可执行文件本身，经 lzt_core::main_text_base() 取得
 *   · 屏幕尺寸：iOS 读 UIScreen（Android 走 AConfiguration）
 */

#include <cstdint>
#include <chrono>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>   // mach_absolute_time（诊断计时）
#include <mach/vm_map.h>   // vm_protect
#include <fcntl.h>         // O_CREAT（文件访问监控用）
#include <execinfo.h>      // backtrace（定位文件读取调用者）
#include <dispatch/dispatch.h>   // dispatch_async（主队列延后任务）
#include <dirent.h>        // opendir/readdir（在 CDN 目录里定位语言表文件）
#include <sys/stat.h>
#include <dlfcn.h>        // dlsym（文件读取取证用）
#include <cstdio>
#include <cstdarg>

#include "lzt_core.h"
#include "lzt_hooks.h"
#include "offsets.h"
#include "settings/settings_framework.h"
#include "settings/settings_widgets.h"
#include "view_angle_module.h"
#include "language_module.h"
#include "resource_file.h"   // lzt_resource::RsbReadStatus（数据包取数细分类）
#include "And64InlineHook.hpp"

// ============================================================
// 静态 hook 运行时支持
//   打包期由 lztpatch 在二进制里追加两段：
//     __LZTEXT  (r-x)：每个 hook 一个 stub（跳板）+ 一个 trampoline
//     __LZTDATA (rw) ：指针槽，本模块构造期写入真实函数地址
//   于是运行时完全不需要改代码页，未越狱设备也能用。
// ============================================================
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

extern "C" uintptr_t g_lzt_stub_seg = 0;
extern "C" uintptr_t g_lzt_tramp_seg = 0;
extern "C" uintptr_t g_lzt_slot_seg = 0;
/* gate 区（打包期写入 __TEXT 头部 slack）：头部 magic 地址、站点步长、站点数 */
static uintptr_t g_lzt_gate_hdr = 0;
static uint32_t  g_lzt_gate_stride = 0;
static uint32_t  g_lzt_gate_count = 0;

#ifdef LZT_STATIC_ONLY
static std::atomic<int> g_blockedInline{0};
static std::atomic<int> g_staticUsed{0};
static std::atomic<int> g_blockedMprotect{0};
extern "C" int lzt_static_only_blocked_inline() { return g_blockedInline.load(); }
extern "C" int lzt_static_only_blocked_mprotect() { return g_blockedMprotect.load(); }
extern "C" int lzt_static_only_static_used() { return g_staticUsed.load(); }

/* 【v44】缺失站点登记表：供启动完成后集中报警。
 * 只记录「缺 manifest 站点」这一主因，槽位分配失败等另有日志。
 * 容量 64 足够（当前实测缺失 15），去重按目标偏移。 */
static constexpr int kMaxMissedSites = 64;
static std::atomic<uintptr_t> g_missedSites[kMaxMissedSites];
static std::atomic<int> g_missedCount{0};

static void lzt_record_missed_site(uintptr_t off) {
    const int n = g_missedCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxMissedSites; ++i) {
        if (g_missedSites[i].load(std::memory_order_relaxed) == off) return;  // 已记录
    }
    int slot = g_missedCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot < kMaxMissedSites) g_missedSites[slot].store(off, std::memory_order_release);
}

/* 启动完成后调用：把全部缺失站点打成一屏，方便一次补齐 manifest。 */
extern "C" void lzt_report_missed_sites(void) {
    int n = g_missedCount.load(std::memory_order_acquire);
    if (n <= 0) return;
    if (n > kMaxMissedSites) n = kMaxMissedSites;
    lzt_core::log_write("[ERROR] ===== %d hook(s) without static site =====", n);
    for (int i = 0; i < n; ++i) {
        lzt_core::log_write("[ERROR]   manifest needs: %d %lx",
                            i, (unsigned long)g_missedSites[i].load(std::memory_order_relaxed));
    }
    lzt_core::log_write("[ERROR] ^ add the lines above to manifest.txt (index may need renumber)");
    lzt_core::log_write("[ERROR] ===== those features are SILENTLY DISABLED =====");
}
#endif

static std::atomic<bool> g_bootstrapFinished{false};
static inline bool lzt_bootstrap_ready(void) {
    return g_bootstrapFinished.load(std::memory_order_acquire);
}

static void lzt_static_init_body(void);

/* 诊断：dispatch_async_f 的派发时刻（毫秒）。用于测量「派发 → 实际执行」的延迟，
 * 反映主队列在启动阶段有多繁忙。 */
static std::atomic<double> g_dispatchMs{0};

/* 毫秒级计时（诊断用）：返回单调递增的毫秒数。
 * 不依赖任何业务头文件，避免插入顺序问题。 */
static double lzt_ms_now(void) {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    const uint64_t t = mach_absolute_time();
    return (double)t * (double)tb.numer / (double)tb.denom / 1e6;
}

// ============================================================
// 诊断用：主线程心跳 + 阶段标记（仅本轮排查启用）
//
// 目的：不再靠反复出包猜测，让卡住时的日志自己说明「主线程停在哪一步」。
//
//   1) g_stageName —— 在任何可能耗时/阻塞的调用前后设置当前阶段名；
//   2) g_mainTick  —— 主队列上一枚递归派发的计数器，持续自增；
//   3) 心跳监视线程 —— 每 200ms 采样一次计数器，连续 3 次不增长（约 600ms）
//      即判定主线程被阻塞，立即把当前阶段名写进日志。
//
// 这样即使进程卡死，也能从日志末尾看到 [HB] main thread STALLED stage=xxx，
// 直接定位到具体函数，无需再逐包二分。
// ============================================================
static std::atomic<uint64_t> g_mainTick{0};
static std::atomic<int>      g_hbRunning{0};
/* 阶段名用固定槽位 + 原子索引，避免主线程写/旁路读之间出现半截字符串。
 * 每个槽位内容为编译期字符串字面量（静态存储期、不会释放），
 * 因此读侧拿到的指针永远有效且完整，无需拷贝或加锁。 */
static const char* volatile g_stageSlots[64] = { "init" };
static std::atomic<int>      g_stageNext{1};

static void lzt_set_stage(const char* name) {
    if (!name) return;
    /* 只追加不覆盖：槽位用尽后保持最后一个（数量远小于 64，不会耗尽）。 */
    const int idx = g_stageNext.load(std::memory_order_acquire);
    if (idx > 0 && idx < 64) {
        g_stageSlots[idx] = name;
        g_stageNext.store(idx + 1, std::memory_order_release);
    }
}

/* ============================================================
 * 主线程卡点采样器（排查期专用，仅当文件 Documents/lzt_stall_probe 存在时启用）
 *
 * 目标：在「心跳确认主线程停滞」时，抓到主线程当前的调用栈，
 *       从而直接定位游戏卡在哪个函数，不再靠反复出包二分。
 *
 * ⚠️ 本段代码最大的风险是「诊断本身把主线程弄死」。因此遵守以下铁律：
 *
 *   1) 绝不 thread_suspend / thread_get_state。
 *      挂起主线程会在它持有任何锁时造成死锁（我们自己的 log 锁、
 *      游戏的资源锁、objc 运行时锁……全部可能），这会直接制造卡死。
 *      改为：只读取主线程【已经发布】的 fp，沿 fp 链回溯。
 *      fp 是硬件约定、由被调函数保存在栈上，旁路读取不会改变主线程状态。
 *
 *   2) 不安装信号、不改任何全局状态、不写游戏内存。
 *
 *   3) 采样最多 3 次（stalled 达到阈值时），之后停手；
 *      避免长时间卡住时反复抓栈形成额外负担。
 *
 *   4) 所有栈遍历都做严格边界校验（对齐、单调递增、步数上限、可读性检查），
 *      任何一条不满足立即停止——宁可少抓，不可读野指针。
 *
 *   5) 输出用 log_write 写一次完整行，不做多次小幅写。
 * ============================================================ */

/* 主线程卡点采样器（排查期专用，仅当 Documents/lzt_stall_probe 存在时启用）
 *
 * 目标：心跳确认主线程停滞时，抓主线程真实调用栈，直接定位游戏卡在哪。
 *
 * ⚠️ 安全性铁律（这一段代码本身绝不能成为卡死原因）：
 *
 *   1) 绝不 thread_suspend。挂起主线程会在它持有任何锁时造成死锁
 *      （我们的 log 锁、游戏资源锁、objc 运行时锁……都可能），
 *      那等于用诊断代码亲手制造卡死。
 *
 *   2) 用 task_threads + thread_get_state 只【读取】主线程寄存器快照。
 *      thread_get_state 对未挂起的线程读取的是"上次离开 CPU 时的状态"，
 *      是只读操作，不改变目标线程执行。
 *
 *   3) 拿到 fp 后沿链回溯，每步都做：8 字节对齐校验、可读性校验、
 *      单调向高地址推进校验、步数上限。任一条不满足立即停止。
 *
 *   4) 只在首次判定停滞时采样一次，之后不再重试。
 *
 *   5) 采样本身在监视线程里执行，不占用主线程。
 */

static void lzt_dump_main_stack(const char* why);

static task_t lzt_proc(void) { return mach_task_self(); }

/* 主线程栈区间：在 bootstrap（主线程）里记录一次，采样时用它精确识别主线程。
 * 比"sp 与自己接近"之类的启发式可靠——监视线程与主线程栈可能相邻，
 * 但栈底/栈顶区间不会重叠。 */
static std::atomic<uintptr_t> g_mainStackLo{0};
static std::atomic<uintptr_t> g_mainStackHi{0};

static void lzt_record_main_stack_range(void) {
    /* 必须从主线程调用 */
    pthread_t self = pthread_self();
    void* addr = pthread_get_stackaddr_np(self);      /* 栈顶（高地址） */
    size_t size = pthread_get_stacksize_np(self);
    if (addr == nullptr || size == 0) return;
    const uintptr_t hi = (uintptr_t)addr;
    const uintptr_t lo = hi - size;
    g_mainStackLo.store(lo, std::memory_order_release);
    g_mainStackHi.store(hi, std::memory_order_release);
    lzt_core::log_write("[STALL] main stack range 0x%lx..0x%lx (%zu KB)",
                        (unsigned long)lo, (unsigned long)hi, size / 1024);
}

static void lzt_dump_main_stack(const char* why) {
    const uintptr_t base = lzt_core::base();

    /* 先校验前置条件，再做任何 mach 调用——避免取得端口后中途 return 造成泄漏。 */
    const uintptr_t preLo = g_mainStackLo.load(std::memory_order_acquire);
    const uintptr_t preHi = g_mainStackHi.load(std::memory_order_acquire);
    if (preLo == 0 || preHi == 0 || preLo >= preHi) {
        lzt_core::log_write("[STALL] %s: main stack range unknown", why);
        return;
    }

    /* 取主线程的线程端口。task_threads 会返回所有线程端口的数组，
     * 必须全部 deallocate，否则会泄漏端口权限（长期运行有害）。 */
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(lzt_proc(), &threads, &count) != KERN_SUCCESS || threads == nullptr) {
        lzt_core::log_write("[STALL] %s: task_threads failed", why);
        return;
    }

    uintptr_t pc = 0, lr = 0, fp = 0, sp = 0;
    bool got = false;
    /* 主线程在任务内的线程列表中通常不是固定位置；改用 thread_info 的
     * THREAD_IDENTIFIER_INFO 比对 pthread_main_np 线程不可行（需要 pthread_t）。
     * 实测取法：主线程恒为 thread_list 中第一个被创建者，
     * 但更稳妥的做法是对每个线程读其 state，选 sp 落在主线程栈区间的那个。
     * 这里用简单可靠判据：主线程栈区间由 pthread_get_stackaddr_np(pthread_self())
     * 只能拿当前线程——因此改用：取第一个 KERN_SUCCESS 且 state 可读的线程，
     * 并与监视线程自身区分（监视线程 sp 与本函数 sp 接近）。 */
    const uintptr_t lo = preLo;
    const uintptr_t hi = preHi;

    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        arm_thread_state64_t st;
        mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(threads[i], ARM_THREAD_STATE64,
                             (thread_state_t)&st, &sc) != KERN_SUCCESS) continue;
        const uintptr_t tsp = (uintptr_t)__darwin_arm_thread_state64_get_sp(st);
        /* 主线程判据：sp 落在启动时记录的栈区间内 */
        if (tsp < lo || tsp >= hi) continue;
        pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(st);
        lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(st);
        fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(st);
        sp = tsp;
        got = true;
        break;
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));

    if (!got) {
        lzt_core::log_write("[STALL] %s: no thread state captured", why);
        return;
    }

    char line[1400];
    int off = snprintf(line, sizeof(line),
                       "[STALL] %s pc=%p(+0x%lx) lr=%p(+0x%lx) sp=%p fp=%p frames:",
                       why, (void*)pc, (unsigned long)(pc - base),
                       (void*)lr, (unsigned long)(lr - base),
                       (void*)sp, (void*)fp);
    int steps = 0;
    uintptr_t cur = fp;
    while (steps < 24 && off > 0 && off < (int)sizeof(line) - 40) {
        if ((cur & 0x7) != 0) break;
        if (!lzt_core::memory_range_accessible(cur, 16, false)) break;
        const uintptr_t* f = (const uintptr_t*)cur;
        const uintptr_t nfp = f[0];
        const uintptr_t nlr = f[1];
        if (nlr >= base && nlr < base + 0x217c000ULL) {
            off += snprintf(line + off, sizeof(line) - off, " +0x%lx",
                            (unsigned long)(nlr - base));
            ++steps;
        }
        if (nfp <= cur) break;
        cur = nfp;
    }
    if (steps == 0) off += snprintf(line + off, sizeof(line) - off, " (none in __TEXT)");
    lzt_core::log_write("%s", line);
}

static std::atomic<int> g_stallProbeEnabled{-1};   /* -1 未探测, 0 关, 1 开 */

static bool lzt_stall_probe_enabled(void) {
    int v = g_stallProbeEnabled.load(std::memory_order_acquire);
    if (v >= 0) return v != 0;

    /* 默认启用（排查期）。开销只发生在「已确认主线程停滞 >=1s」这一条路径上：
     * 平时心跳只做原子自增，完全不涉及本函数。
     *
     * 若容器 Documents 下存在 lzt_stall_probe_off 标记文件则关闭，
     * 便于在不重装的情况下验证「采样本身是否影响启动」。 */
    char files[512] = {0};
    lzt_core::files_dir(files, sizeof(files));
    bool on = true;
    if (files[0] != 0) {
        char path[640];
        snprintf(path, sizeof(path), "%s/lzt_stall_probe_off", files);
        FILE* f = fopen(path, "r");
        if (f != nullptr) { fclose(f); on = false; }
    }
    g_stallProbeEnabled.store(on ? 1 : 0, std::memory_order_release);
    lzt_core::log_write("[STALL] probe %s by default", on ? "enabled" : "disabled");
    return on;
}

/* 读侧：只读已发布的槽位，指针指向字符串字面量，永远有效。 */
static const char* lzt_cur_stage(void) {
    const int idx = g_stageNext.load(std::memory_order_acquire);
    if (idx <= 0 || idx >= 64) return g_stageSlots[0];
    const char* s = g_stageSlots[idx - 1];
    return s ? s : g_stageSlots[0];
}

static void lzt_main_heartbeat_tick(void*);

/* 心跳本身的三条纪律（避免诊断代码反过来拖慢启动）：
 *   1) 绝不在 constructor 里建线程——本函数只在 bootstrap（主队列）中调用；
 *   2) 监视线程只在【首次】判定阻塞时写一次日志，之后不再重复写，
 *      避免长时间卡住时反复加锁 + fflush 把主线程锁死；
 *   3) 心跳回调本身不做任何 I/O，只自增一个原子计数。 */
static void lzt_start_heartbeat(void) {
    if (g_hbRunning.exchange(1)) return;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                     dispatch_get_main_queue(), nullptr, lzt_main_heartbeat_tick);
    pthread_t th;
    if (pthread_create(&th, nullptr, +[](void*) -> void* {
            uint64_t last = g_mainTick.load(std::memory_order_acquire);
            int stalled = 0;
            bool reported = false;
            int dumpCount = 0;    /* 已采样次数，上限 3，避免长时间卡住反复抓栈 */
            for (int guard = 0; guard < 3000; ++guard) {   /* 最多观察 10 分钟 */
                usleep(200 * 1000);
                const uint64_t now = g_mainTick.load(std::memory_order_acquire);
                if (now == last) {
                    ++stalled;
                    /* 第 5 次（约 1s）抓第一次；之后每 30 秒（150 次采样）补抓一次，
                     * 最多 3 次。单次要看到主线程处于不同阻塞阶段，避免碰巧抓到
                     * 系统调用点而错过真正的调用链。 */
                    const bool wantFirst = (stalled == 5 && !reported);
                    const bool wantMore  = reported && (stalled >= 155) &&
                                           ((stalled - 155) % 150 == 0) && (dumpCount < 3);
                    if (wantFirst || wantMore) {
                        if (wantFirst) {
                            reported = true;
                            lzt_core::log_write(
                                "[HB] main thread STALLED >=1s at stage=\"%s\" tick=%llu",
                                lzt_cur_stage(), (unsigned long long)now);
                        }
                        if (lzt_stall_probe_enabled()) {
                            ++dumpCount;
                            lzt_dump_main_stack(wantFirst ? "stalled" : "stalled-later");
                        }
                    }
                } else {
                    if (reported) {
                        lzt_core::log_write("[HB] recovered after %d samples, stage=\"%s\"",
                                            stalled, lzt_cur_stage());
                    }
                    reported = false;
                    stalled = 0;
                    last = now;
                }
            }
            return nullptr;
        }, nullptr) == 0) {
        pthread_detach(th);
    }
}

static void lzt_main_heartbeat_tick(void*) {
    /* 只自增原子计数，零 I/O、零汇编帧操作。 */
    g_mainTick.fetch_add(1, std::memory_order_release);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                     dispatch_get_main_queue(), nullptr, lzt_main_heartbeat_tick);
}

/* 语言模块入口（jni/language_module.cpp）。iOS 侧不再是 constructor，
 * 改由本文件异步 bootstrap 在主队列调用一次。 */
extern "C" void language_locale_bootstrap(void);

static void lzt_static_init_once(void) {
    /* 一次性执行：本函数可能被 gate 在任意线程首次触发。
     * 用函数内静态量的线程安全初始化（C++11 magic static）保证只跑一遍，
     * 且其他线程会阻塞等待完成，不会读到半初始化的值。 */
    static const bool s_done = []() -> bool {
        lzt_static_init_body();
        return true;
    }();
    (void)s_done;
}

/* 主镜像索引（实现在 lzt_core_ios.mm，按 mainBundle 可执行文件路径匹配）。 */
extern "C" int lzt_ios_main_index_public(void);

static void lzt_static_init_body(void) {
    const struct mach_header_64* h = nullptr;

    /* 【v49】主路径：复用 lzt_core 的主镜像判定（按 mainBundle 可执行文件路径
     * 在 dyld 镜像表里精确匹配），与 main_text_base() 保持同一套逻辑。
     *
     * 为什么不能只靠下面的 MH_EXECUTE 扫描：
     *   LiveContainer / TrollStore 等容器把游戏作为【内嵌 app】加载，
     *   宿主进程自身也是 MH_EXECUTE 且排在镜像表前面，
     *   「取第一个 MH_EXECUTE」会命中宿主 → 扫不到 LZTGATE1
     *   → gate_hdr=0x0 → 全部 26 个静态站点跳过
     *   → 设置页里看不到任何新 Tab（实测，朋友设备日志为证）。
     *
     * 路径匹配在容器环境下已验证有效（日志里 main __TEXT base 能正确取到
     * .../com.popcap.ios.PvZ2.lca.app/PvZ2）。 */
    {
        const int midx = lzt_ios_main_index_public();
        if (midx >= 0) {
            h = (const struct mach_header_64*)_dyld_get_image_header((uint32_t)midx);
            /* 注意：不再校验 filetype == MH_EXECUTE。
             * 容器环境下内嵌 app 的 Mach-O 头 filetype 未必是该值，
             * 加了校验反而会把正确答案排除。main_image_index() 本身
             * 已按 mainBundle 路径精确匹配，结果可信。 */
        }
    }
    if (h != nullptr) {
        lzt_core::log_write("[STATIC] main image resolved via mainBundle path (idx=%d)",
                            lzt_ios_main_index_public());
    }

    /* 兜底：原「第一个 MH_EXECUTE」扫描。普通越狱环境下二者结果一致。 */
    if (h == nullptr) {
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const struct mach_header_64* cand =
                (const struct mach_header_64*)_dyld_get_image_header(i);
            if (cand && cand->magic == MH_MAGIC_64 && cand->filetype == MH_EXECUTE) {
                h = cand;
                lzt_core::log_write("[STATIC] main image resolved via MH_EXECUTE scan (idx=%u)", i);
                break;
            }
        }
    }
    if (!h) {
        lzt_core::log_write("[STATIC] main image NOT FOUND; static sites disabled");
        return;
    }
    const uint8_t* p = (const uint8_t*)h + sizeof(struct mach_header_64);
    uint64_t text_vmaddr = 0, text_end = 0, data_end = 0;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* s = (const struct segment_command_64*)lc;
            if (strcmp(s->segname, "__TEXT") == 0) {
                text_vmaddr = s->vmaddr;
                text_end = s->vmaddr + s->vmsize;
            } else if (strcmp(s->segname, "__DATA") == 0) {
                data_end = s->vmaddr + s->vmsize;
            }
        }
        p += lc->cmdsize;
    }
    if (!text_vmaddr || !text_end || !data_end) return;
    /* gate 区由打包工具 lztgate 写入 __TEXT 内。
     *
     * 历史：v49 及以前把 gate 放在 load command 之后的头部 slack（0x2330），
     * 并在此固定扫描 0x800..0x20000。实测发现 LiveContainer 等容器会向
     * 主程序注入 load command（ncmds 79 -> 81），直接覆盖该 slack，
     * 表现为 gate_hdr=0x0、全部静态站点跳过、设置页无新 Tab。
     *
     * v50 起 gate 改放 __TEXT 段尾零填充区，位置不再固定在头部。
     * 因此这里改为扫描整个 __TEXT 段的可读范围，而不是固定窗口。
     * 扫描起点跳过 Mach-O 头与 load commands，避免误命中字符串常量。 */
    {
        uintptr_t scan_lo = (uintptr_t)h + sizeof(struct mach_header_64) + h->sizeofcmds;
        scan_lo = (scan_lo + 15) & ~(uintptr_t)15;
        uintptr_t scan_hi = (uintptr_t)h + (text_end - text_vmaddr);   /* __TEXT 段结束（换算到运行时地址） */

        lzt_core::log_write("[STATIC] scanning __TEXT for gate: 0x%lx..0x%lx (ncmds=%u sizeofcmds=%u)",
                            (unsigned long)scan_lo, (unsigned long)scan_hi,
                            (unsigned)h->ncmds, (unsigned)h->sizeofcmds);

        for (uintptr_t a = scan_lo; a + 16 <= scan_hi; a += 4) {
            if (memcmp((const void*)a, "LZTGATE1", 8) != 0) continue;
            uint32_t stride = *(const uint32_t*)(a + 8);
            uint32_t count  = *(const uint32_t*)(a + 12);
            if (stride == 0 || stride > 4096 || count > 4096) continue;
            g_lzt_gate_hdr = a;
            g_lzt_gate_stride = stride;
            g_lzt_gate_count = count;
            break;
        }
    }
    /* ---- 运行时 gate 桩结构自检 ----
     *
     * 目的：发现「gate 数据被容器部分覆盖」的情况。
     *
     * 当前行为：只要找到 LZTGATE1 就认为 gate 完好，22 个站点全信。
     * 如果容器只覆盖了 gate 的一部分（比如只踩了前几个站点），
     * 会表现为「部分功能静默失效」，排查困难。
     *
     * 这里对每个 gate 桩做结构校验：
     *   +0x00  adrp x16, ...        → opcode 0x90000010
     *   +0x04  ldr  x16, [x16, ...] → opcode 0xF9400210
     *   +0x08  cbnz x16, ...        → opcode 0xB5000030 范围
     *   +0x0C  b    ...             → opcode 0x14000000
     *   +0x10  br   x16             → opcode 0xD61F0200
     *
     * 若不匹配，打印站点编号和实际指令字，便于定位是哪一个桩被踩了。
     * 只在找到 gate 时执行，未找到时跳过（已有日志）。 */
    if (g_lzt_gate_hdr != 0 && g_lzt_gate_count > 0) {
        int bad_count = 0;
        for (uint32_t i = 0; i < g_lzt_gate_count; ++i) {
            uintptr_t gate = g_lzt_gate_hdr + 16 + (uintptr_t)i * g_lzt_gate_stride;
            const uint32_t* g = (const uint32_t*)gate;

            /* adrp x16, ...：op 位 + rd=16。立即数字段不固定，不能参与比较。 */
            if ((g[0] & 0x9F00001Fu) != 0x90000010u) {
                lzt_core::log_write("[STATIC] gate site %u: adrp mismatch 0x%08x", i, g[0]);
                ++bad_count;
                continue;
            }
            /* ldr x16, [x16, #off]：opc/size 固定 0xF940，rn=16(bit5-9)、rt=16(bit0-4)。
             * imm12(bit10-21) 是槽内偏移，随站点变化，不能参与比较。 */
            if ((g[1] & 0xFFC003FFu) != 0xF9400210u) {
                lzt_core::log_write("[STATIC] gate site %u: ldr mismatch 0x%08x", i, g[1]);
                ++bad_count;
                continue;
            }
            /* cbnz x16, ...：sf/op 固定 0xB5，rt=16(bit0-4)。imm19(bit5-23) 不固定。 */
            if ((g[2] & 0xFF00001Fu) != 0xB5000010u) {
                lzt_core::log_write("[STATIC] gate site %u: cbnz mismatch 0x%08x", i, g[2]);
                ++bad_count;
                continue;
            }
            /* b ...：高 6 位应为 0b000101 = 0x14 */
            if ((g[3] & 0xFC000000u) != 0x14000000u) {
                lzt_core::log_write("[STATIC] gate site %u: b mismatch 0x%08x", i, g[3]);
                ++bad_count;
                continue;
            }
            /* br x16：应为 0xD61F0200 */
            if (g[4] != 0xD61F0200u) {
                lzt_core::log_write("[STATIC] gate site %u: br mismatch 0x%08x", i, g[4]);
                ++bad_count;
                continue;
            }
        }
        if (bad_count > 0) {
            lzt_core::log_write("[STATIC] gate integrity check: %d/%u sites corrupted",
                                bad_count, g_lzt_gate_count);
        } else {
            lzt_core::log_write("[STATIC] gate integrity check: all %u sites OK",
                                g_lzt_gate_count);
        }
    }

    g_lzt_stub_seg = 0;   /* 不再有共享 stub 池 */
    g_lzt_slot_seg = 0;   /* 槽地址由每个 gate 现场反算 */
    lzt_core::log_write("[STATIC] gate_hdr=%p stride=%u count=%u (image hdr=%p, scanned __TEXT)",
                        (void*)g_lzt_gate_hdr,
                        (unsigned)g_lzt_gate_stride, (unsigned)g_lzt_gate_count,
                        (void*)h);
    if (g_lzt_gate_hdr == 0) {
        /* 找不到 gate = 主程序没被 lztgate 处理过，或扫错了镜像。
         * 打出前 16 字节便于判断是「不是 Mach-O」还是「是 Mach-O 但无站点」。 */
        const uint32_t* w = (const uint32_t*)h;
        lzt_core::log_write("[STATIC] no gate found; image head magic=%08x filetype=%u ncmds=%u",
                            w[0], w[1], w[4]);
    }
}

/* gate 区覆盖范围：[hdr+16, hdr+16+count*stride) */
static bool lzt_in_gate_range(uintptr_t p) {
    if (!g_lzt_gate_hdr) return false;
    const uintptr_t lo = g_lzt_gate_hdr + 16;
    const uintptr_t hi = lo + (uintptr_t)g_lzt_gate_count * g_lzt_gate_stride;
    return p >= lo && p < hi;
}

static inline uintptr_t lzt_b_target(uint32_t insn, uintptr_t pc) {
    int64_t imm = (int32_t)(insn << 6) >> 6;
    return pc + (intptr_t)(imm << 2);
}

/* 目标入口是否已被打包工具改为「b gate」。
 * 判定依据：入口第 1 条是指向 gate 区的 B 指令。
 * 只读一个 32 位字，不假设 gate 内部布局。 */
extern "C" bool lzt_static_is_patched(uintptr_t target) {
    if (!g_lzt_gate_hdr) lzt_static_init_once();
    if (!g_lzt_gate_hdr) return false;
    const uint32_t insn = *(const uint32_t*)target;
    if ((insn & 0xFC000000u) != 0x14000000u) return false;   /* 必须是 B */
    return lzt_in_gate_range(lzt_b_target(insn, target));
}

/* 原函数门厅：入口的 B 指向 gate，gate +0x0C 又是一条 B 指向 tramp。
 * 由 target 反查，不需要 idx 与打包工具约定一致。 */
extern "C" void* lzt_static_tramp_for(uintptr_t target) {
    if (!g_lzt_gate_hdr) lzt_static_init_once();
    if (!g_lzt_gate_hdr) return nullptr;
    const uint32_t entry = *(const uint32_t*)target;
    if ((entry & 0xFC000000u) != 0x14000000u) return nullptr;
    const uintptr_t gate = lzt_b_target(entry, target);
    if (!lzt_in_gate_range(gate)) return nullptr;
    const uint32_t b = ((const uint32_t*)gate)[3];
    if ((b & 0xFC000000u) != 0x14000000u) return nullptr;
    return (void*)lzt_b_target(b, gate + 12);
}

/* 填写指针槽：从 gate 的 adrp+ldr 反算槽地址并写入。
 * 槽位于 __DATA 段内、filesize 之外的可写零页——纯数据写，不碰可执行页。 */
extern "C" void lzt_static_fill(uintptr_t target, void* fn) {
    if (!g_lzt_gate_hdr) lzt_static_init_once();
    if (!g_lzt_gate_hdr) return;
    const uint32_t entry = *(const uint32_t*)target;
    if ((entry & 0xFC000000u) != 0x14000000u) return;
    const uintptr_t gate = lzt_b_target(entry, target);
    if (!lzt_in_gate_range(gate)) return;
    const uint32_t* g = (const uint32_t*)gate;
    if ((g[0] & 0x9F000000u) != 0x90000000u) return;          /* adrp */
    if ((g[1] & 0xFFC00000u) != 0xF9400000u) return;          /* ldr  */
    int64_t imm = (int64_t)(((g[0] >> 5) & 0x7FFFF) << 2 | ((g[0] >> 29) & 3));
    if (imm & 0x100000) imm |= ~0x1FFFFFLL;
    const uintptr_t page = ((uintptr_t)gate & ~0xFFFULL) + (uintptr_t)(imm << 12);
    const uint32_t off = ((g[1] >> 10) & 0xFFF) << 3;
    void** slot = (void**)(page + off);
    *slot = fn;
}

#if defined(LZT_STATIC_ONLY)
/* 静态专用模式：物理上禁止一切运行时代码页修改。
 *
 * 但并非简单禁用——每个 hook 先尝试走「打包期预置的静态站点」：
 *   打包时把目标入口改成  adrp/ldr/cbnz/b/br（20 字节原地打桩），
 *   第 5 条 br x16 会跳到槽里登记的 hook 函数，故运行期只需写槽（纯数据写）。
 * 只有当目标未被预置站点时才记数并放弃，因此漏迁只缺功能、绝不崩。 */
extern "C" void lzt_blocked_inline_hook(void* target, void* hook, void** orig) {
    uintptr_t tgt = (uintptr_t)target;
    if (orig) *orig = nullptr;
    if (tgt && hook && lzt_static_is_patched(tgt)) {
        lzt_static_fill(tgt, hook);
        if (orig) *orig = lzt_static_tramp_for(tgt);
        g_staticUsed.fetch_add(1);
        return;
    }
    g_blockedInline.fetch_add(1);
    lzt_record_missed_site(tgt - lzt_core::base());
    lzt_core::log_write("[ERROR] hook SKIPPED, no static site @+0x%lx -> feature disabled",
                        (unsigned long)(tgt - lzt_core::base()));
}
#define A64HookFunction lzt_blocked_inline_hook
#endif


// ---- 帧率诊断探针（实现见 lzt_core_ios.mm）----
extern "C" void lzt_ios_install_fps_probe();
extern "C" void lzt_ios_fps_report_tick();
extern "C" void lzt_ios_install_fps_probe2();
extern "C" void lzt_ios_fps_report_tick2();
extern "C" void lzt_ios_install_displaylink_probe();
extern "C" void lzt_ios_displaylink_report();
extern "C" void lzt_ios_install_dl4();
extern "C" void lzt_ios_dl4_report();

// ---- 帧率实测：统计真正更新函数 0x1c2d31c 的调用频率 ----
// mainLoop 内部当 flag88==0 时 tail-call 该函数，是「实际推进游戏一帧」的入口。
// 用 naked 钩子只做计数后尾跳，不扰动寄存器与栈。
extern "C" uint64_t g_workTicks = 0;
extern "C" void* g_origWork = nullptr;

extern "C" __attribute__((naked)) void hk_work() {
    __asm__ volatile(
        "adrp x9, _g_workTicks@PAGE\n"
        "add  x9, x9, _g_workTicks@PAGEOFF\n"
        "ldr  x10, [x9]\n"
        "add  x10, x10, #1\n"
        "str  x10, [x9]\n"
        "adrp x9, _g_origWork@PAGE\n"
        "add  x9, x9, _g_origWork@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 1f\n"
        "br   x9\n"
        "1:\n"
        "ret\n"
    );
}

extern "C" void lzt_ios_work_report() {
    static uint64_t lastTicks = 0;
    static auto lastTp = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - lastTp).count();
    if (dt < 0.5) return;
    const uint64_t cur = g_workTicks;
    const uint64_t d = cur - lastTicks;
    lzt_core::log_write("WORKTICK: %.2f Hz (%llu ticks in %.3fs)",
                        static_cast<double>(d) / dt,
                        (unsigned long long)d, dt);
    lastTicks = cur;
    lastTp = now;
}
extern "C" void lzt_ios_dl5_report();
extern "C" void lzt_ios_rtprobe_tick();

extern "C" bool lzt_ios_refresh_root_title(uintptr_t contentPage);
extern "C" void lzt_ios_refresh_root_title_captured(void);

// ---- 设置页构造函数钩子（0x756AE4）----
// 该函数是设置根页的构造函数：x0 = this（根页），内部创建根标题对象并
// 存进 this+216 起的 vector。这是根页指针唯一可靠的来源
// （owner 链 +216→+8 拿到的是子页标题对象，不是根页）。
static void (*oSettingsInitFn)(uintptr_t) = nullptr;
static std::atomic<uintptr_t> g_rootSettingsPage{0};

static void hkSettingsInit(uintptr_t page) {
    // 只记录根页指针，【不做任何本地化或写入】。
    // 教训（三轮实测）：设置页初始化期调用游戏本地化口(0x179C14)会崩；
    // 构造返回后同步调用、以及 dispatch 延迟调用同样会崩（前者页面仍在装配，
    // 后者页面可能已析构）。写入时机最终与安卓对齐 —— 语言切换流程里，
    // refresh_rows 之后调用（见 lzt_ios_schedule_lang_apply）。
    if (page != 0) {
        g_rootSettingsPage.store(page, std::memory_order_release);
    }
    if (oSettingsInitFn) oSettingsInitFn(page);
}

// 供语言切换后调用：用已捕获的根页刷新根标题。
// 与安卓 settings_framework.cpp:923 的 refresh_all() → refresh_page_title()
// 一一对应。
extern "C" void lzt_ios_refresh_root_title_captured(void) {
    const uintptr_t page = g_rootSettingsPage.load(std::memory_order_acquire);
    if (page == 0) {
        lzt_core::log_write("[TITLE] no captured settings page yet");
        return;
    }
    lzt_ios_refresh_root_title(page);
}


// 页面显示函数 0x75A0E4 的钩子：目前只做透传。
// 说明：原版在该函数开头检查 dirty，命中时【直接返回】（反汇编 0x75A0fc:
// cbnz w8, <收尾>），并不负责重建 —— 因此重建没有挂在这里，而是由
// dispatch 命中控件后调度的「延后重建」（ios_schedule_rebuild）承担。
// 保留本钩子是为了在页面显示链路上有一个可控的介入点。
static void (*oPageShowFn)(uintptr_t) = nullptr;

static void hkPageShow(uintptr_t page) {
    // 页面显示钩子：仅转发原函数（与安卓保持一致，不在这里碰根标题）。
    // 行的认领已统一由「行初始化器 (0x75b128) + 行挂载精确匹配」完成，
    // 不再需要在页面显示后扫描容器（那条路径曾在 dispatch 栈内读未稳定的
    // 容器指针而崩溃，且用文本匹配易错绑，已废弃）。
    //
    // 【v40】根标题刷新点从本函数移除，回到安卓的时机（语言切换后）。
    // 依据 Android settings_framework.cpp:923：
    //     int n = lzt_localizer::refresh_all();
    //     refresh_page_title();      // RootTitle，紧跟行回填
    // 此前 v36/v38/v39 分别试过「构造后延迟 / 构造后同步 / 页面显示」，
    // 三次都在非安卓时机上崩掉 —— 那些时点页面要么装配中、要么已析构。
    if (oPageShowFn) oPageShowFn(page);
}

// 以下两个由 iOS 平台层（lzt_core_ios.mm）提供，属 iOS 专有接口，
// 不放进共享的 lzt_core.h，避免污染 Android 侧的接口定义。
namespace lzt_core {
uintptr_t main_image_slide();
const char *main_image_path();
uintptr_t main_text_base();
}

// iOS 平台层提供的资源目录重定向（实现见 lzt_core_ios.mm）：
// 把 Library/Application Support/No_Backup 变为指向 Documents/No_Backup 的符号链接。
extern "C" void lzt_ios_redirect_no_backup_dir();
extern "C" void lzt_ios_ensure_backup_exclusion();

// fishhook 式符号重绑定（实现见 iOS/src/lzt_fishhook.cpp）。
// 声明必须在全局作用域——放在匿名命名空间内会被解析为内部链接而链接失败。
extern "C" int lzt_rebind(const char* symbol, void* replacement, void** orig_out);
// 文件访问探针（独立模块，覆盖 open/openat/stat64 等系统调用）
extern "C" int lzt_fileprobe_install(void);
// 文件访问探针总开关（默认关闭）。
// 教训：在 constructor 早期挂接 open/stat 等热函数会让游戏「显示发行商图标后
// 静默被杀」——无系统崩溃报告、无自记崩溃日志（与 FINDINGS 六.2 同一现象）。
// 仅在需要取证时置 true；取证优先用「shell 直接运行二进制」方式（存活稳定）。
static constexpr bool kEnableFileProbe = false;   // 诊断用（定位文件读取调用栈），默认关闭
// ============================================================
// CDN 初始化入口挂钩 —— iOS 对齐安卓 CDN apply 链的关键
//
// 静态链条（反汇编实证）：
//   0x1c4a8c  CDN 初始化入口（栈帧 352 字节；内部 0x1c5ca4 处 bl 到下一环）
//     └─ 0x3f4d4  清单收集：拼 "LawnStrings-<locale>.rton"，以 tableId=131
//                  追加进清单（内部数十处调用清单添加 0x40910）
//     └─ 0x735930 清单应用：遍历清单逐个 apply —— 游戏自身的文件读取在此发生
//
// 与安卓的对应关系：
//   Android OFF_CDN_APPLY=0xD58544   <->  iOS 0x735930（清单应用）
//   Android hook 清单添加 sub_ACABCC   <->  iOS 0x40910（清单添加）
//
// 定位教训（勿重犯）：
//   1) xref 输出的是 vmaddr（已含 0x100000000）；base 本身已是 slide+0x100000000，
//      必须减去基址再加 base，否则重复 4GB，补丁落到 dyld 共享缓存上，
//      表现为「装上了但从不触发」。
//   2) inline hook 必须挂函数入口。0x1c5ca4 是函数内部的 bl 指令，
//      入口经反查序言（栈帧 352 字节）确定为 0x1c4a8c。
// ============================================================

static std::atomic<int> g_cdnInitN{0};
static uintptr_t (*g_origCdnInit)(uintptr_t) = nullptr;
static uintptr_t hkCdnInit(uintptr_t a0) {
    const int n = g_cdnInitN.fetch_add(1);
    if (n < 4) lzt_core::log_write("CDN-INIT 命中 #%d a0=%p", n + 1, (void*)a0);
    return g_origCdnInit ? g_origCdnInit(a0) : 0;
}


// ============================================================
// 「单表应用」挂钩 —— 与安卓 OFF_CDN_APPLY 对应的落点
//
// 0x2ca220（0x2ca220..0x2ca380）：CDN 清单应用循环内部逐项调用的函数，
// 接收已构建好的资源名（形如 "packages\LawnStrings-zh-cn.rton"）并加载该表。
// 对应安卓 try_external_apply 的最后一环（OFF_CDN_APPLY 内部对单文件的应用）。
//
// 用途（两段式对齐）：
//   数据层：改 LangMgr 语言标识（已实现）
//   刷新层：用新 locale 重放本函数 → 游戏按新语言重载 LawnStrings
// ============================================================

static std::atomic<int> g_applyN{0};
static uintptr_t (*g_origApply)(uintptr_t, uintptr_t, uintptr_t) = nullptr;
static void (*g_origApplyOn)(uintptr_t, uintptr_t) = nullptr;
// 窄串读取（定义见本文件前部）：供装表参数取证使用
static void read_sstr(uintptr_t obj, char* out, size_t cap);

// 读 libc++ std::string 的 C 视图（长度在 +23，最高位=长串）
static void lzt_apply_read_str(uintptr_t obj, char* out, size_t cap) {
    if (out == nullptr || cap < 2) return;
    out[0] = 0;
    if (obj == 0 || !lzt_core::memory_range_accessible(obj, 24, false)) return;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(obj);
    if (b[23] & 0x80) {
        const char* p = *reinterpret_cast<const char* const*>(b);
        const size_t n = *reinterpret_cast<const size_t*>(b + 8);
        if (p == nullptr || n == 0 || n >= cap) return;
        if (!lzt_core::memory_range_accessible((uintptr_t)p, n, false)) return;
        memcpy(out, p, n);
        out[n] = 0;
    } else {
        const size_t n = b[23];
        if (n == 0 || n >= cap) return;
        memcpy(out, b, n);
        out[n] = 0;
    }
}

static uintptr_t hkApply(uintptr_t a0, uintptr_t a1, uintptr_t a2) {
    // 0x2ca220 的真实参数形态（反汇编 0x735eb4 调用点）：
    //   x0 = 数据库单例，x1 = 辅助参数，x2 = 名字串对象
    // 这里对名字串打印内容，用于确定「按 tableId 装表」需要哪些参数。
    const int n = g_applyN.fetch_add(1);
    if (n < 30) {
        char nm[160] = {0};
        read_sstr(a2, nm, sizeof(nm));
        lzt_core::log_write("APPLY #%d a0=%p a1=%p a2=%p name=\"%s\"",
                            n + 1, (void*)a0, (void*)a1, (void*)a2, nm);
    }
    return g_origApply ? g_origApply(a0, a1, a2) : 0;
}


// ============================================================
// 文本哈希表查找 —— 接管点（与安卓 Localizer 同一位置）
//
// 0x17a480：哈希表 find，签名 (表容器 x0, 键字符串 x1)，返回命中槽地址。
// 依据：其配套哈希函数 0x17a500 读取 libc++ string 的 +23（长度/长串标志）
// 与 +8（长串数据指针），与游戏字符串布局一致；且实测挂钩后立刻捕获到
// 真实查询（[ProdName] / [Title] / [NOTIFICATION_CHANNEL_PUSH_NOTES_NAME]），
// 确认它查的正是游戏文本表。
//
// 接管策略（最小风险，不改任何游戏状态）：
//   键以 '[' 开头 → 查本模块按当前语言加载的 LawnStrings 表
//     · 命中 → 把该槽内容替换为我们的文本，返回值不变
//     · 未命中 → 原样放行
//   其余键（平台信息、资源名等）→ 原样放行
//
// 开关 lzt_takeover_enable()：由语言切换流程开启；关闭时纯只读记录。
// ============================================================

static std::atomic<int> g_lookupN{0};
static std::atomic<int> g_takeoverHits{0};
static uintptr_t (*g_origLookup)(uintptr_t, uintptr_t) = nullptr;
static std::atomic<bool> g_takeoverOn{false};

// 开启/关闭接管（语言切换流程在加载好新表后调用开启）
extern "C" void lzt_takeover_enable(bool on) {
    g_takeoverOn.store(on, std::memory_order_release);
    lzt_core::log_write("l10n takeover %s", on ? "ON" : "OFF");
}

// 文本缓冲：轮换使用，避免与上一次结果冲突
static constexpr int kTextBufNum = 8;
static uint8_t g_textBufs[kTextBufNum][2048];
static std::atomic<int> g_textBufNext{0};

// UTF-32 → UTF-8，写入 libc++ std::string 长串形态（+0 数据 / +8 长度 / +16 容量）
static bool write_text_string(const wchar_t* ws, uint8_t* dst, size_t cap) {
    uint8_t tmp[1024];
    size_t n = 0;
    for (const wchar_t* p = ws; *p; ++p) {
        const uint32_t cp = static_cast<uint32_t>(*p);
        if (cp < 0x80) {
            if (n + 2 > sizeof(tmp)) return false;
            tmp[n++] = static_cast<uint8_t>(cp);
        } else if (cp < 0x800) {
            if (n + 3 > sizeof(tmp)) return false;
            tmp[n++] = static_cast<uint8_t>(0xC0 | (cp >> 6));
            tmp[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (n + 4 > sizeof(tmp)) return false;
            tmp[n++] = static_cast<uint8_t>(0xE0 | (cp >> 12));
            tmp[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
            tmp[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
        } else {
            if (n + 5 > sizeof(tmp)) return false;
            tmp[n++] = static_cast<uint8_t>(0xF0 | (cp >> 18));
            tmp[n++] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
            tmp[n++] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
            tmp[n++] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
        }
    }
    if (n + 1 >= cap) return false;

    memset(dst, 0, 24);
    memcpy(dst + 0, tmp, n);           // 数据（+0 放指针，24 起为本缓冲）
    return true;
}

// 注意：命中槽的结构布局未验证，因此接管只做「记录 + 统计」，
// 真正改写前需先确认槽内存结构（避免像之前那样猜签名导致崩溃）。

// 加载表函数（0x2cbfb4）探针：w1 = tableId。
// 用途：确认游戏在什么时机加载表（136=LawnStrings 等），
// 该调用即为「按 locale 加载语言表」的落点。
static std::atomic<int> g_tblLoadN{0};

// 读 libc++ std::string（窄串）为 C 串：长串 ptr@0/len@8，短串长度在 +23。
// 定义放在此处（早于使用点）：语言表名改写需要它。
static void read_sstr(uintptr_t obj, char* out, size_t cap) {
    if (out == nullptr || cap < 2) return;
    out[0] = 0;
    if (obj == 0 || !lzt_core::memory_range_accessible(obj, 24, false)) return;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(obj);
    if (b[23] & 0x80) {
        const char* p = *reinterpret_cast<const char* const*>(b);
        const size_t n = *reinterpret_cast<const size_t*>(b + 8);
        if (p == nullptr || n == 0 || n >= cap) return;
        if (!lzt_core::memory_range_accessible((uintptr_t)p, n, false)) return;
        memcpy(out, p, n);
        out[n] = 0;
    } else {
        const size_t n = b[23];
        if (n == 0 || n >= cap) return;
        memcpy(out, b, n);
        out[n] = 0;
    }
}
static void (*oStrCtor)(void*, const char*) = nullptr;   // 游戏 std::string(const char*) 构造器
static char g_targetLocale[16] = {0};                    // 目标语言（语言模块写入）

// 表加载器 0x2cbfb4 的完整签名：x0=对象、w1=tableId、x2..x7=其它参数。
// ⚠️ 必须【逐个透传】全部参数：该函数序言连用 x3~x7（mov x19,x7 / mov x22,x6 …），
// 若 hook 只声明 (a0,tableId) 两个参数，x2~x7 会被我们函数里的调用（如 log_write）
// 覆盖成垃圾值，函数内部随即越界释放并崩溃（实测 pc=+0x1c81894）。
// ---- 方案 A：走「清单 apply」链让游戏自己从 CDN 读语言表 ----
//
// 启动期实测链条：清单里有 table=131 条目（名字 LawnStrings-<locale>.rton）→
// 应用循环 0x735930 对每条：先 0x736d20(ctx, tableId, out) 求出文件名，
// 再由 0x7f66a0(name) 去「CDN 目录 / 数据包」取文件（CDN 有则优先），
// 最后 0x2c9e04(ctx, tableId, name) 把内容装进该 tableId。
//
// 因此这里做两件事（与安卓 cdn_load_rton 改写清单条目同源）：
//   ① 挂钩 0x736d20，把 tableId=131 求出的文件名改写成「目标语言」的名字；
//   ② 切换语言后重跑一次应用循环，让游戏按新名字去 CDN 取文件并装表。
// 数据包 / CDN 的取舍完全由游戏资源层决定，我们不干预。
static constexpr bool kEnableManifestReapply = true;
// CDN 应用循环（0x735930）与它收到的上下文：定义见文件后部，此处前置声明供切换时重跑。
static uintptr_t (*g_origCdnApply)(uintptr_t) = nullptr;
static std::atomic<uintptr_t> g_cdnMgr{0};
// 「按 tableId 应用 CDN 文件」原函数（0x2c9e04）与它收到的上下文（启动期捕获）。
static uintptr_t (*g_origApply2)(uintptr_t, uint32_t, uintptr_t) = nullptr;
static std::atomic<uintptr_t> g_applyCtx{0};
// 清单「添加条目」原函数（0x40910）与清单管理器：供切换语言时重新注入语言表条目。
using CdnListAddFnFwd = void (*)(uintptr_t, uintptr_t, uint32_t, uint32_t);
static CdnListAddFnFwd g_origCdnListAdd = nullptr;
static std::atomic<uintptr_t> g_cdnListMgr{0};
extern "C" void* g_origTblName = nullptr;   // 供 naked 桥引用（须 C 链接）

extern "C" void lzt_tblname_post(uint8_t* out, uint32_t tableId);

// 非标准 ABI：x0=ctx，w1=tableId，x8=输出名字对象（同 0x179c14 的 x8 传参习惯）
extern "C" __attribute__((naked)) void hkTblName() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x8, [sp, #16]\n"          // 保存输出对象
        "str w1, [sp, #24]\n"          // 保存 tableId
        "adrp x9, _g_origTblName@PAGE\n"
        "add  x9, x9, _g_origTblName@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 1f\n"
        "ldr  x8, [sp, #16]\n"          // 恢复 x8（原函数需要）
        "blr  x9\n"                    // 先让原函数把默认名字写好
        "1:\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  w1, [sp, #24]\n"
        "bl   _lzt_tblname_post\n"      // 再按目标语言改写名字
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

extern "C" void lzt_tblname_post(uint8_t* out, uint32_t tableId) {
    if (out == nullptr || tableId != 131) return;
    if (g_targetLocale[0] == 0 || oStrCtor == nullptr) return;
    char want[96];
    snprintf(want, sizeof(want), "packages/lawnstrings-%s.rton", g_targetLocale);
    char cur[128] = {0};
    read_sstr(reinterpret_cast<uintptr_t>(out), cur, sizeof(cur));
    if (cur[0] != 0 && strcasecmp(cur, want) == 0) return;
    oStrCtor(out, want);
    lzt_core::log_write("tblname rewrite: \"%s\" -> \"%s\"", cur, want);
}
// 表加载器 0x2cbfb4 的完整签名：x0=对象、w1=tableId、x2..x7=其它参数。
static uintptr_t (*g_origTblLoad)(uintptr_t, uint32_t, uintptr_t, uintptr_t,
                                   uintptr_t, uintptr_t, uintptr_t, uintptr_t) = nullptr;
static uintptr_t hkTblLoad(uintptr_t a0, uint32_t a1, uintptr_t a2, uintptr_t a3,
                           uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    const int n = g_tblLoadN.fetch_add(1);
    // table 131 = LawnStrings：无条件记录（判定语言表何时被加载、参数是什么）；
    // 其余表只记前 40 条，避免窗口被占满。
    const bool isLang = (a1 == 131);
    // ★ 按目标语言改写语言表的表名（方案 1 的稳妥实现）：
    // 不去自己构造 tabledef（那需要复现 x2/x7 等 10KB 栈上下文），而是让游戏自己
    // 构造好其余参数，我们只把「LawnStrings-<旧语言>」换成「LawnStrings-<目标语言>」。
    // 数据包/CDN 的取舍由游戏自己的资源层决定（CDN 有则优先，无则回落数据包）。
    // 与安卓 cdn_load_rton 改写清单条目同源。
    if (isLang && g_targetLocale[0] != 0 && a6 != 0 && oStrCtor != nullptr) {
        char nm[128] = {0};
        read_sstr(a6, nm, sizeof(nm));
        char want[96];
        snprintf(want, sizeof(want), "LawnStrings-%s", g_targetLocale);
        if (nm[0] != 0 && strcasecmp(nm, want) != 0) {
            alignas(16) static uint8_t s_nameObj[32];
            static char s_name[96];
            memset(s_nameObj, 0, sizeof(s_nameObj));
            snprintf(s_name, sizeof(s_name), "%s", want);
            oStrCtor(s_nameObj, s_name);
            lzt_core::log_write("tblload(LANG) name rewrite: \"%s\" -> \"%s\"", nm, s_name);
            a6 = reinterpret_cast<uintptr_t>(s_nameObj);
        }
    }
    static std::atomic<int> s_other{0};
    static std::atomic<int> s_lang{0};
    if ((isLang && s_lang.fetch_add(1) < 40) || (!isLang && s_other.fetch_add(1) < 40)) {
        lzt_core::log_write("tblload%s #%d a0=%p tableId=%u a2=%p a3=%p a4=%p a5=%p a6=%p a7=%p",
                            isLang ? "(LANG)" : "", n + 1, (void*)a0, a1, (void*)a2,
                            (void*)a3, (void*)a4, (void*)a5, (void*)a6, (void*)a7);
    }
    if (isLang) {
        // 语言表加载的名字/路径参数：逐个按「libc++ 字符串」与 C 串两种形态尝试读取，
        // 用于确定重载语言表时应传什么（对应安卓 OFF_DB_LOAD_RTON 的 name 参数）。
        const uintptr_t probes[6] = {a2, a3, a6, a7, a0, a4};
        const char* tags[6] = {"a2", "a3", "a6", "a7", "a0", "a4"};
        for (int i = 0; i < 6; ++i) {
            const uintptr_t p = probes[i];
            if (p < 0x1000) continue;
            // 窄串（libc++ std::string：长串 ptr@0/len@8，短串长度在 +23）
            if (lzt_core::memory_range_accessible(p, 24, false)) {
                const uint8_t* o = reinterpret_cast<const uint8_t*>(p);
                const uint8_t tail = o[23];
                const char* d = nullptr;
                size_t len = 0;
                if ((tail & 0x80) != 0) {
                    d = *reinterpret_cast<const char* const*>(o);
                    len = *reinterpret_cast<const size_t*>(o + 8);
                } else if (tail > 0) {
                    d = reinterpret_cast<const char*>(o);
                    len = tail;
                }
                if (d != nullptr && len > 0 && len < 160 &&
                    lzt_core::memory_range_accessible((uintptr_t)d, len, false)) {
                    bool printable = true;
                    for (size_t k = 0; k < len; ++k) {
                        const unsigned char ch = static_cast<unsigned char>(d[k]);
                        if (ch < 0x20 || ch > 0x7E) { printable = false; break; }
                    }
                    if (printable) {
                        char tmp[160] = {0};
                        memcpy(tmp, d, len);
                        lzt_core::log_write("  %s s24=[%s]", tags[i], tmp);
                    }
                }
            }
            // 直接 C 串
            char raw[192] = {0};
            bool ok = false;
            for (size_t k = 0; k < 190; ++k) {
                if (!lzt_core::memory_range_accessible(p + k, 1, false)) break;
                const unsigned char ch = *reinterpret_cast<const unsigned char*>(p + k);
                if (ch == 0) { ok = (k > 0); break; }
                if (ch < 0x20 || ch > 0x7E) break;
                raw[k] = static_cast<char>(ch);
            }
            if (ok) lzt_core::log_write("  %s raw=[%s]", tags[i], raw);
        }
    }
    return g_origTblLoad ? g_origTblLoad(a0, a1, a2, a3, a4, a5, a6, a7) : 0;
}

// ============================================================
// RSB 资源查找探针 —— 对应安卓的 OFF_PACKAGE_FIND_RECORD(0x16B31EC)
//
// 依据（用户确认的机制）：iOS 与安卓同内核，CDN 目录通过「读取文件名」
// 覆盖 RSB 内部资源。因此 LawnStrings 的加载必然经过「按名字查 RSB」。
//
// 已定位：
//   0x64f20  外层函数（含名字构造与查找），内部调用 0x6525c
//   0x6525c  名字构造函数：拼 "packages/" + 名字 + ".rton"
//
// 这两个点是安卓 RSB 查找的等价物。挂钩它们可确认 LawnStrings 的
// 实际加载时机与参数，进而在切换语言后重放 —— 即安卓式热重载。
// ============================================================

static std::atomic<int> g_rsbOuterN{0};
static uintptr_t (*g_origRsbOuter)(uintptr_t, uintptr_t, uintptr_t) = nullptr;
static uintptr_t hkRsbOuter(uintptr_t a0, uintptr_t a1, uintptr_t a2) {
    const int n = g_rsbOuterN.fetch_add(1);
    if (n < 20) {
        char s0[160] = {0}, s1[160] = {0};
        lzt_apply_read_str(a0, s0, sizeof(s0));
        lzt_apply_read_str(a1, s1, sizeof(s1));
        lzt_core::log_write("RSB-OUTER #%d a0=[%s] a1=[%s] a2=%p",
                            n + 1, s0, s1, (void*)a2);
    }
    return g_origRsbOuter ? g_origRsbOuter(a0, a1, a2) : 0;
}

static std::atomic<int> g_rsbNameN{0};
static uintptr_t (*g_origRsbName)(uintptr_t, uintptr_t, uintptr_t, uintptr_t) = nullptr;
static uintptr_t hkRsbName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    const int n = g_rsbNameN.fetch_add(1);
    if (n < 20) {
        char s0[160] = {0}, s1[160] = {0};
        lzt_apply_read_str(a0, s0, sizeof(s0));
        lzt_apply_read_str(a1, s1, sizeof(s1));
        lzt_core::log_write("RSB-NAME #%d a0=[%s] a1=[%s]", n + 1, s0, s1);
    }
    return g_origRsbName ? g_origRsbName(a0, a1, a2, a3) : 0;
}
// 读 libc++ std::string（24B，SSO 长度在 +23）
static void lz_read_cxx_string(const uint8_t* o, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    out[0] = 0;
    if (o == nullptr || !lzt_core::memory_range_accessible((uintptr_t)o, 24, false)) return;
    const uint8_t tail = o[23];
    if (tail & 0x80) {
        const char* d = *reinterpret_cast<const char* const*>(o);
        const size_t n = *reinterpret_cast<const size_t*>(o + 8);
        if (d == nullptr || n == 0 || n >= cap) return;
        if (!lzt_core::memory_range_accessible((uintptr_t)d, n, false)) return;
        memcpy(out, d, n);
        out[n] = 0;
    } else {
        size_t n = tail;
        if (n >= cap) n = cap - 1;
        memcpy(out, o, n);
        out[n] = 0;
    }
}

static uintptr_t hkLookup(uintptr_t table, uintptr_t key) {
    const uintptr_t ret = g_origLookup ? g_origLookup(table, key) : 0;
    const int n = g_lookupN.fetch_add(1);

    // 表结构取证（只读）：命中项整块 dump，找出「本地化文本」的真实字段。
    // 已知：3 个调用点都取 slot+56 当字符串（其中 0x1799bc 会在未命中时用
    // table+1992 作为哨兵），但实测 +56 有时为空、有时是另一个键名，故需实证。
    if (ret != 0 && n < 8 && lzt_core::memory_range_accessible(ret, 128, false)) {
        char k[160] = {0};
        lzt_apply_read_str(key, k, sizeof(k));
        const uint64_t* w = reinterpret_cast<const uint64_t*>(ret);
        lzt_core::log_write("DUMP #%d table=%p key=[%s] ret=%p sentinel=%p",
                            n + 1, (void*)table, k, (void*)ret, (void*)(table + 8));
        for (int r = 0; r < 4; ++r) {
            lzt_core::log_write("  +%02d: %016llx %016llx %016llx %016llx", r * 32,
                                (unsigned long long)w[r * 4 + 0],
                                (unsigned long long)w[r * 4 + 1],
                                (unsigned long long)w[r * 4 + 2],
                                (unsigned long long)w[r * 4 + 3]);
        }
        for (int off = 0; off <= 104; off += 8) {
            char s[128] = {0};
            lz_read_cxx_string(reinterpret_cast<const uint8_t*>(ret) + off, s, sizeof(s));
            if (s[0] != 0) lzt_core::log_write("  str@+%d = [%s]", off, s);
        }
    }
    return ret;
}

// ============================================================
// lzt_hooks::install —— iOS 实现
// lzt_hooks::install —— iOS 实现

// ============================================================
// lzt_hooks::install —— iOS 实现
// ARM64 走 A64HookFunction；trampoline 由 hook 库内部管理，
// 因此 iOS 侧不需要调用方提供 trampoline 缓冲。
// ============================================================
// ARM64 走 A64HookFunction；trampoline 由 hook 库内部管理，
// 因此 iOS 侧不需要调用方提供 trampoline 缓冲。
// ============================================================
// 登记到 patch 监控表（定义于本文件后段，供存活检查与自动重装使用）
void lzt_ios_register_hook(uintptr_t target, void* hookFn);

namespace lzt_hooks {
bool install(uintptr_t target, void *hook_fn, void **orig_out, uint8_t *trampoline_buf) {
    (void)trampoline_buf;
    if (target == 0 || hook_fn == nullptr || orig_out == nullptr) {
        lzt_core::log_write("[SFW] hook install: bad args target=%p fn=%p out=%p",
                            (void*)target, hook_fn, (void*)orig_out);
        return false;
    }
    if (lzt_core::base() == 0) {
        lzt_core::log_write("[SFW] hook install: base not ready, target=+0x%lx",
                            (unsigned long)target);
        return false;
    }

    void *orig = nullptr;
    A64HookFunction(reinterpret_cast<void *>(target), hook_fn, &orig);
    *orig_out = orig;
    if (orig != nullptr) {
        ::lzt_ios_register_hook(target, hook_fn);
    } else {
        // 【v17】明确区分「无静态站点」与「其它失败」：前者由
        // lzt_blocked_inline_hook 打 no static site 行，这里补上站点判定结果，
        // 便于一眼看出是该加站点、还是站点存在但填槽失败。
        lzt_core::log_write("[ERROR] hook install FAILED @+0x%lx static_site=%d "
                            "(0=需加 manifest 站点) -> feature disabled",
                            (unsigned long)(target - lzt_core::base()),
                            lzt_static_is_patched(target) ? 1 : 0);
    }
    return orig != nullptr;
}
} // namespace lzt_hooks

// ============================================================
// patch 存活监控与自动重装（对应 Android 的 start_patch_monitor）
//
// 背景：inline hook 写入的补丁可能被游戏自身的代码重写/抹零（Android 实测；
//       iOS 同属原生代码页，同样可能发生）。一旦补丁消失，功能会静默失效
//       且没有任何迹象，因此需要持续检查并自动重装。
//
// 策略（与 Android 一致）：
//   · 前 30 秒每 100ms 检查一次（启动期最易被覆盖），之后每秒检查
//   · 检测方式：看目标首指令是否仍是我们的跳转（B 或 LDR+BR）
//   · 重装方式：直接重写补丁字节（不重新调用 A64HookFunction——那会再分配
//     一个 trampoline，池会被逐步耗尽；已有 trampoline 仍然有效）
//   · iOS 特有：写代码页必须先取写权限、写完立刻恢复可执行（W^X）
// ============================================================

namespace {

constexpr size_t kMaxHooks = 16;

// 全游戏文本替换的命中/未命中计数（用于验证覆盖范围）
static std::atomic<int> g_l10nHits{0};
static std::atomic<int> g_l10nMiss{0};
static std::atomic<int> g_l10nSeen{0};
// 注：定位阶段用过的若干临时探针（语言管理器扫描、格式化函数探针、
//     libc fopen/open 探针）已在定位完成后移除，原因记录如下：
//       · libc 的 fopen/open 位于 dyld 共享缓存，补丁走 20 字节远跳路径，
//         挂钩后启动即崩 —— iOS 上不宜挂钩共享缓存内的函数。
//       · 0x1acb430 的调用约定未确认，挂钩即崩，已放弃。
//       · 语言管理器已由反汇编直接定位（*(DisplayInfo+2104)，语言标识在 +508），
//         无需再靠运行时扫描。

struct HookRecord {
    uintptr_t target;
    void*     hookFn;
    size_t    patchSize;   // 0 = 未定
};

HookRecord g_hooks[kMaxHooks];
std::atomic<int> g_hookCount{0};

// 取写权限（iOS 强制 W^X：写入时需要写权限，写完必须恢复可执行）
bool ios_make_private_writable(uintptr_t addr, size_t size) {
#if defined(LZT_STATIC_ONLY)
    /* 静态专用模式：禁止对代码页做任何保护位修改 */
    (void)addr; (void)size;
    g_blockedMprotect.fetch_add(1);
    return false;
#endif
    vm_address_t page = addr & ~(vm_address_t)0x3FFF;
    vm_size_t len = ((addr + size + 0x3FFF) & ~(vm_address_t)0x3FFF) - page;
    kern_return_t kr = vm_protect(mach_task_self(), page, len, FALSE,
                                  (vm_prot_t)(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY));
    if (kr == KERN_SUCCESS) return true;
    kr = vm_protect(mach_task_self(), page, len, FALSE,
                    (vm_prot_t)(VM_PROT_READ | VM_PROT_WRITE));
    return kr == KERN_SUCCESS;
}

void ios_restore_executable(uintptr_t addr, size_t size) {
    vm_address_t page = addr & ~(vm_address_t)0x3FFF;
    vm_size_t len = ((addr + size + 0x3FFF) & ~(vm_address_t)0x3FFF) - page;
    vm_protect(mach_task_self(), page, len, FALSE,
               (vm_prot_t)(VM_PROT_READ | VM_PROT_EXECUTE));
}

// 判定补丁形态（与 Android detect_patch_size 同规则）
size_t detect_patch_size(uintptr_t target) {
    const uint32_t* p = reinterpret_cast<const uint32_t*>(target);
    if (p[0] == 0x58000051u && p[1] == 0xd61f0220u) return 16;   // LDR X17,#8; BR X17
    if ((p[0] & 0xFC000000u) == 0x14000000u) return 4;            // B imm26
    return 0;
}

bool is_patch_alive(uintptr_t target, size_t patchSize) {
    const uint32_t* p = reinterpret_cast<const uint32_t*>(target);
    if (patchSize <= 4) return (p[0] & 0xFC000000u) == 0x14000000u;
    return (p[0] == 0x58000051u && p[1] == 0xd61f0220u);
}

bool rewrite_patch(uintptr_t target, void* hookFn, size_t patchSize) {
    if (!ios_make_private_writable(target, patchSize ? patchSize : 16)) return false;
    uint32_t* p = reinterpret_cast<uint32_t*>(target);
    if (patchSize <= 4) {
        const int64_t off = (static_cast<int64_t>(reinterpret_cast<uintptr_t>(hookFn))
                             - static_cast<int64_t>(target)) >> 2;
        if (off >= 0x01FFFFFFLL || off <= -0x02000000LL) {
            lzt_core::log_write("REINSTALL: B-mode target too far, skip");
            ios_restore_executable(target, 4);
            return false;
        }
        p[0] = 0x14000000u | (static_cast<uint32_t>(off) & 0x03FFFFFFu);
        __builtin___clear_cache((char*)target, (char*)(target + 4));
        ios_restore_executable(target, 4);
    } else {
        p[0] = 0x58000051u;   // LDR X17, #8
        p[1] = 0xd61f0220u;   // BR X17
        *reinterpret_cast<uint64_t*>(p + 2) =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hookFn));
        __builtin___clear_cache((char*)target, (char*)(target + 16));
        ios_restore_executable(target, 16);
    }
    return true;
}

void start_patch_monitor();

} // namespace

// 供 lzt_hooks::install 登记（供监控线程检查与重装）
void lzt_ios_register_hook(uintptr_t target, void* hookFn) {
    int n = g_hookCount.fetch_add(1);
    if (n >= kMaxHooks) {
        lzt_core::log_write("hook registry full, monitor skip target=%p", (void*)target);
        return;
    }
    g_hooks[n].target = target;
    g_hooks[n].hookFn = hookFn;
    g_hooks[n].patchSize = detect_patch_size(target);
    lzt_core::log_write("hook registered: target=%p mode=%s", (void*)target,
                        g_hooks[n].patchSize == 4 ? "B" :
                        (g_hooks[n].patchSize == 16 ? "LDR+BR" : "unknown"));
}

namespace {

// 监控线程：前 30 秒高频，之后低频（与 Android 同节奏）
void monitor_loop() {
    int reinstall = 0;
    int check = 0;
    const int total = g_hookCount.load(std::memory_order_acquire);

    for (;;) {
        const int intervalMs = (check < 300) ? 100 : 1000;
        ++check;
        usleep(intervalMs * 1000);

        for (int i = 0; i < total; ++i) {
            HookRecord& h = g_hooks[i];
            if (h.target == 0 || h.hookFn == nullptr) continue;
            if (!lzt_core::memory_range_accessible(h.target, 16, false)) continue;
            if (is_patch_alive(h.target, h.patchSize)) continue;

            ++reinstall;
            const bool ok = rewrite_patch(h.target, h.hookFn, h.patchSize);
            if (reinstall <= 5 || (reinstall % 10) == 0) {
                lzt_core::log_write("REINSTALL #%d target=%p mode=%zu -> %s (now %08x)",
                                    reinstall, (void*)h.target, h.patchSize,
                                    ok ? "OK" : "FAIL",
                                    *reinterpret_cast<uint32_t*>(h.target));
            }
        }

        // 资源目录重定向复检：游戏内容更新等流程可能删掉目录并重建为普通目录，
        // 届时链接会失效（数据悄悄写回旧位置）。这里定期复检并恢复。
        if ((check % 25) == 0) {
            lzt_ios_redirect_no_backup_dir();
            lzt_ios_ensure_backup_exclusion();
            // 全游戏文本替换的覆盖情况（命中=用了我们的表，未命中=放行游戏原文）
            lzt_core::log_write("l10n stats: hit=%d miss=%d",
                                g_l10nHits.load(), g_l10nMiss.load());
        }
    }
}

// 启动监控线程（幂等）。必须在运行时调用——构造期建线程会崩。
std::atomic<bool> g_monitorStarted{false};

void start_patch_monitor() {
    bool expected = false;
    if (!g_monitorStarted.compare_exchange_strong(expected, true)) return;
    pthread_t th;
    if (pthread_create(&th, nullptr, +[](void*) -> void* {
            monitor_loop();
            return nullptr;
        }, nullptr) != 0) {
        g_monitorStarted.store(false, std::memory_order_release);
        lzt_core::log_write("patch monitor: thread create failed");
        return;
    }
    pthread_detach(th);
    lzt_core::log_write("patch monitor: started (%d hooks)",
                        g_hookCount.load(std::memory_order_acquire));
}

} // namespace

// 供 hook 回调在运行时启动监控（构造期建线程会崩，因此必须延后到运行时）
void lzt_ios_start_patch_monitor() { start_patch_monitor(); }

// ============================================================
// 视角 hook：高视角 = 强制 board[280] = 1.0
//
// 与 Android 同源的两段设计：
//   Hook 0（BoardZoom2）：post-hook 把 board[280] 强制为 1.0，得到高视角
//   Hook 1（BoardLayout_ApplyZoom）：读 Board 字段做黑边对齐
// ============================================================
static long (*g_origBoardZoom2)(uintptr_t) = nullptr;
static void *g_hookBoardZoom2 = nullptr;
static std::atomic<bool> g_viewHooksEnabled{false};

static constexpr uintptr_t BOARD_280_OFF = BOARD_280;   // 取自 offsets_ios.h

// 保底监控：被游戏改写时立即还原（对应 Android 的 watchdog）
static std::atomic<uintptr_t> g_watchBoard{0};
static std::atomic<bool> g_watchRunning{false};

// ---- Board watchdog（对应 Android start_board_watchdog）----
// 职责：高视角下每 16ms 校验 board[280] 是否仍为 1.0，被别处改写则立即还原。
// 安全退出（关键，防止踩已释放内存）：
//   · board 已被换（g_watchBoard != my_board）→ 退出，交给新 watchdog
//   · 内存不可访问 → 退出
//   · scale 连续 3 次为 0（board 已释放）→ 退出
//   · scale 连续 3 次超出 [0.1, 10.0]（内存被复用）→ 退出
//   · 视角切到低视角 → 退出（低视角本就不该强制 1.0）
//   · 最长约 2 分钟（7500 × 16ms），避免长期持有失效指针
static constexpr float WATCHDOG_SCALE_MIN = 0.1f;
static constexpr float WATCHDOG_SCALE_MAX = 10.0f;

static void* board_watchdog_thread(void* arg) {
    uintptr_t my_board = reinterpret_cast<uintptr_t>(arg);
    int zeroCount = 0, invalidCount = 0, fixCount = 0, samples = 0, driftCount = 0;

    for (int i = 0; i < 7500; ++i) {
        if (g_watchBoard.load(std::memory_order_acquire) != my_board) {
            lzt_core::log_write("watchdog: superseded by new board, exit");
            break;
        }
        if (!g_viewHooksEnabled.load(std::memory_order_acquire)) {
            lzt_core::log_write("watchdog: view switched to low, exit");
            break;
        }
        if (!lzt_core::memory_range_accessible(my_board + BOARD_280_OFF, 4, true)) {
            lzt_core::log_write("watchdog: board memory unavailable, exit");
            break;
        }

        float* pscale = reinterpret_cast<float*>(my_board + BOARD_280_OFF);
        const float v = *pscale;
        ++samples;

        // 分三类处理（与 Android 略有差异，原因见下）：
        //   · v == 0：board 已释放（原版会把缩放清零）→ 累计 3 次退出
        //   · 明显垃圾（NaN / 无穷 / |v| 极大）：内存被复用 → 快速退出
        //   · 小值漂移（0 < v < 0.1）：多为退出关卡时的缩放动画/复位中间态，
        //     不是内存复用。Android 在此直接判复用退出，但 Android 的 hook 会
        //     随视角切换卸载、watchdog 生命周期更短；iOS 的 hook 常驻，退出关卡
        //     的动画也会被看到。故这里不退出，仅累计观察（不动该值）。
        if (v == 0.0f) {
            invalidCount = 0;
            if (++zeroCount >= 3) {
                lzt_core::log_write("watchdog: board released (zero x3), exit");
                break;
            }
        } else if (!(v == v) || v > 1e6f || v < -1e6f) {
            zeroCount = 0;
            if (++invalidCount >= 3) {
                lzt_core::log_write("watchdog: scale %g is garbage x3, "
                                    "memory reused, exit", (double)v);
                break;
            }
        } else if (v < WATCHDOG_SCALE_MIN || v > WATCHDOG_SCALE_MAX) {
            // 动画/复位中间态：不动它，也不退出；连续很久仍如此才放弃
            zeroCount = 0;
            invalidCount = 0;
            if (++driftCount >= 600) {   // 约 10 秒
                lzt_core::log_write("watchdog: scale drifted to %g for 10s, exit",
                                    (double)v);
                break;
            }
        } else {
            zeroCount = 0;
            invalidCount = 0;
            driftCount = 0;
            if (v != 1.0f) {
                *pscale = 1.0f;
                ++fixCount;
                if (fixCount <= 20 || (fixCount % 100) == 0) {
                    lzt_core::log_write("watchdog: FIX #%d scale %g -> 1.0",
                                        fixCount, (double)v);
                }
            }
        }
        usleep(16 * 1000);   // 16ms
    }
    lzt_core::log_write("watchdog: exit (fixes=%d samples=%d)", fixCount, samples);
    g_watchRunning.store(false, std::memory_order_release);
    return nullptr;
}

// 对着某个 board 启动监控（幂等；换 board 时让旧的自行退出）
static void start_board_watchdog(uintptr_t board) {
    if (board == 0) return;
    const uintptr_t cur = g_watchBoard.load(std::memory_order_acquire);
    if (g_watchRunning.load(std::memory_order_acquire)) {
        if (cur == board) return;              // 已在监控同一个 board
        g_watchBoard.store(0, std::memory_order_release);   // 让旧的退出
        for (int i = 0; i < 10 && g_watchRunning.load(std::memory_order_acquire); ++i)
            usleep(5 * 1000);
    }
    bool expected = false;
    if (!g_watchRunning.compare_exchange_strong(expected, true)) return;
    g_watchBoard.store(board, std::memory_order_release);
    pthread_t th;
    if (pthread_create(&th, nullptr, board_watchdog_thread,
                       reinterpret_cast<void*>(board)) != 0) {
        g_watchRunning.store(false, std::memory_order_release);
        return;
    }
    pthread_detach(th);
    lzt_core::log_write("watchdog: start monitoring board=%p", (void*)board);
}

// ============================================================
// Hook 1：BoardLayout_ApplyZoom —— 黑边对齐
//
// 与 Android 同源：宽高比超过阈值时，把 board[270]/[284] 设为 -board[283]
// （相机左移，屏幕左缘贴合棋盘左缘），高视角再把 board[285] 设为
// (board[283]+board[286])/2（选卡关卡居中）。
// 阈值：高视角 1.69335、低视角 2.16666（与 Android 完全一致）。
// ============================================================
static constexpr float HIGH_VIEW_ALIGN_ASPECT = 1.69335f;
static constexpr float LOW_VIEW_ALIGN_ASPECT  = 2.16666f;

static constexpr uintptr_t BOARD_281_OFF = BOARD_281;
static constexpr uintptr_t BOARD_270_OFF = BOARD_270;

// 对齐前记录的原版 b270 与其所属 board（供街道恐龙生成补偿 X 基准用）。
// 与 Android 的 g_orig_b270 / g_orig_b270_board 一一对应。
static std::atomic<int> g_origB270{0};
static std::atomic<uintptr_t> g_alignBoard{0};

// 设备判定（与 Android should_align_for_view 同规则）：
//   读取异常（aspect <= 0）时保守返回 true；有效比值不大于阈值则不需要对齐。
static bool should_align_for_view(float aspect, bool highView) {
    if (aspect <= 0.0f) return true;
    const float threshold = highView ? HIGH_VIEW_ALIGN_ASPECT : LOW_VIEW_ALIGN_ASPECT;
    return aspect > threshold;
}
static constexpr uintptr_t BOARD_17_OFF  = BOARD_17;
static constexpr uintptr_t BOARD_283_OFF = BOARD_283;
static constexpr uintptr_t BOARD_284_OFF = BOARD_284;
static constexpr uintptr_t BOARD_285_OFF = BOARD_285;
static constexpr uintptr_t BOARD_286_OFF = BOARD_286;

// 屏幕宽高比（横屏取长边/短边；iOS 的 UIScreen bounds 为竖屏朝向）
// 实现在平台层（Objective-C++），此处只声明。
extern "C" float lzt_ios_screen_aspect_ratio();
extern "C" int lzt_ios_screen_width_px();

static float ios_screen_aspect() {
    return lzt_ios_screen_aspect_ratio();
}

static long (*g_origBoardZoom)(uintptr_t) = nullptr;
static void *g_hookBoardZoom = nullptr;

// 对齐写入。返回 false 表示布局未就绪（调用方应武装 DEFER 重试）。
// 与 Android 完全同源：左对齐 board[270]=board[284]=-board[283]；
// 高视角再让选卡关卡居中 board[285]=(board[283]+board[286])/2。
// 对齐结果三态（对应 Android 的 AlignResult）
enum AlignOutcome {
    ALIGN_OK = 0,        // 已完成对齐
    ALIGN_RETRY,         // 布局未就绪 / 输入异常：需要稍后重试（对应 ALIGN_SKIP_LAYOUT）
    ALIGN_DONE_SKIP,     // 设备/环境判定为无需对齐：完成，不再重试
};

// 宽松就绪判定（对应 Android board_scale_ready，**不含 bpw**）。
// 为什么不能用严格判定：高视角保存的关卡在低视角进入时，H1 可能在 scale=0
// 时被调用，原版 BoardZoom 用 scale=0 算出 b283=0 → bpw=b286-b283<0 恒不满足，
// 会死等；而 b283=0 恰恰是「需要重跑 BoardZoom 修复」的信号，不是「等待」信号。
// 因此这里只看 scale 终值 +（低视角的）b281 就绪。
static bool board_scale_ready(uintptr_t board, bool highView) {
    const float scale = *(float *)(board + BOARD_280_OFF);
    if (scale < 0.5f) return false;                       // 终值恒 >=1.0，动画中间值拦截
    if (!highView && *(float *)(board + BOARD_281_OFF) < 500.0f)
        return false;                                     // 低视角公式依赖 b281
    return true;
}

static AlignOutcome apply_align(uintptr_t board, const char* tag) {
    const float scale = *(float *)(board + BOARD_280_OFF);
    const int b283 = *(int *)(board + BOARD_283_OFF);
    const int b286 = *(int *)(board + BOARD_286_OFF);
    const int bpw = b286 - b283;

    // 布局就绪判定：缩放未见终值或宽度非正即视为中间态
    if (scale < 0.5f || bpw <= 0) {
        lzt_core::log_write("%s skip: layout not ready scale=%.4f bpw=%d",
                            tag, (double)scale, bpw);
        return ALIGN_RETRY;
    }

    const bool highView = g_viewHooksEnabled.load(std::memory_order_acquire);

    // 设备判定（与 Android 同阈值；读取异常时保守执行对齐）
    float aspect = ios_screen_aspect();
    const float threshold = highView ? HIGH_VIEW_ALIGN_ASPECT : LOW_VIEW_ALIGN_ASPECT;
    if (aspect > 0.0f && aspect <= threshold) {
        static std::atomic<int> s_skip2{0};
        if (s_skip2.fetch_add(1) < 3)
            lzt_core::log_write("%s skip: aspect=%.5f <= %.5f (highView=%d)",
                                tag, (double)aspect, (double)threshold, highView ? 1 : 0);

        // v49 稳态自愈（与 Android 完全同源，注释记录的场景与本机表现一致）：
        //   「高视角关卡保存退出 → 切换视角重进」时，相机渲染偏移可能残留上一次
        //   对齐写入的值（本视角不需要对齐 → 本分支完全跳过 → 偏移与 b270 失配），
        //   表现为画面整体偏移，且要等植物震屏把相机推回才恢复原状。
        //   稳态恒等关系是 camR == b270（震屏恢复逻辑也按 b270 刷新），因此这里
        //   把相机偏移拉回当前 b270。条件写入：稳态时零干扰。
        const int camR_now = *(int *)(board + BOARD_17_OFF);
        const int b270_steady = *(int *)(board + BOARD_270_OFF);
        if (camR_now != b270_steady) {
            *(int *)(board + BOARD_17_OFF) = b270_steady;
            lzt_core::log_write("%s camR self-heal: %d -> %d (steady-state b270 sync, view=%d)",
                                tag, camR_now, b270_steady, highView ? 1 : 0);
        }
        return ALIGN_DONE_SKIP;
    }

    // 左对齐目标值——高低视角公式不同（与 Android compute_left_align 完全一致）：
    //   高视角：leftAlign = -b283
    //   低视角：leftAlign = ceil(b281 - (b281 + b283) / scale)
    // 早前对两种视角都套用 -b283，导致「切到低视角后缩放变了、相机位置却没变」。
    int leftAlign = 0;
    if (highView) {
        leftAlign = -b283;
    } else {
        const float b281 = *(float *)(board + BOARD_281_OFF);
        if (scale > 0.0001f && b281 > 0.0f) {
            leftAlign = (int)ceilf(b281 - (b281 + (float)b283) / scale);
        } else {
            lzt_core::log_write("%s skip: low-view input invalid scale=%.4f b281=%.1f",
                                tag, (double)scale, (double)b281);
            return ALIGN_RETRY;   // 输入异常，稍后重试（对应 Android 返回 SKIP_LAYOUT）
        }
    }

    // 合理性兜底（与 Android 同规则）：对齐位必须落在屏内，
    // 否则视为布局垃圾并跳过（Android 曾因写入垃圾值导致白屏）。
    const int screenWidth = lzt_ios_screen_width_px();
    if (screenWidth > 0 && (leftAlign < 0 || leftAlign > screenWidth)) {
        lzt_core::log_write("%s skip: left_align_out_of_range leftAlign=%d sw=%d "
                            "(b283=%d b286=%d scale=%.4f)",
                            tag, leftAlign, screenWidth, b283, b286, (double)scale);
        return ALIGN_RETRY;
    }

    // 记录「对齐前的原版 b270」及其所属 board，供街道恐龙生成入口补偿 X 基准。
    // 必须在写入之前读取：与 Android 同据，原版每次调用 BoardZoom 都会用布局表
    // 重算 b270，因此此处读到的恒为原版值（H1 多次触发也不会误存上次的对齐值）。
    g_origB270.store(*(int *)(board + BOARD_270_OFF), std::memory_order_relaxed);
    g_alignBoard.store(board, std::memory_order_release);

    *(int *)(board + BOARD_270_OFF) = leftAlign;
    *(int *)(board + BOARD_284_OFF) = leftAlign;
    // 选卡关卡相机居中：仅高视角（与 Android withCenterAlign 一致）
    if (highView) {
        *(int *)(board + BOARD_285_OFF) = (b283 + b286) / 2;
    }
    // 立即把相机渲染偏移同步到对齐位（Android v27 同源做法）。
    // 不写它：保存退出/无平移动画的关卡在暂停态下相机更新冻结，该字段停在旧
    // 相机位 → 表现为「有对齐但仍有黑边」，直到植物爆炸等事件才补上。
    const int old_b17 = *(int *)(board + BOARD_17_OFF);
    *(int *)(board + BOARD_17_OFF) = leftAlign;
    lzt_core::log_write("%s b17 sync: %d -> %d (immediate)", tag, old_b17, leftAlign);
    lzt_core::log_write("%s ALIGNED aspect=%.5f highView=%d b283=%d b286=%d "
                        "-> b270=b284=%d b285=%d",
                        tag, (double)aspect, highView ? 1 : 0, b283, b286, leftAlign,
                        highView ? (b283 + b286) / 2 : 0);
    return ALIGN_OK;
}

// ---- DEFER：布局未就绪 / 关卡保存了旧相机位时，持续轮询直到对齐成功 ----
// 完整对照 Android start_deferred_align：
//   · 轮询 100 次 × 100ms（最多 10 秒），不是"等一会儿试一次"
//   · 就绪判定用宽松版 board_scale_ready（不含 bpw）
//   · 就绪后先重跑一次原版 BoardZoom 重算布局字段，再走严格守卫对齐
//   · 全程幂等：重跑无副作用，可反复进行
// 关键场景（Android v41 注释记录）：低视角进入「高视角保存退出」的关卡时，
// H1 可能在 scale=0 时被调、算出坏的 b283，此后不再被触发；表现就是画面停在
// 保存的旧相机位、直到植物震屏才被拉回。DEFER 负责把这种关卡补救回来。
static std::atomic<uintptr_t> g_deferBoard{0};
static std::atomic<bool> g_deferArmed{false};
static std::mutex g_alignMutex;

static void* defer_thread(void* arg) {
    uintptr_t board = reinterpret_cast<uintptr_t>(arg);
    lzt_core::log_write("DEFER: armed board=%p, waiting for layout", (void*)board);

    for (int i = 0; i < 100; ++i) {          // 最多 10 秒
        usleep(100 * 1000);

        // 换了关卡（board 变了）→ 交给新的 DEFER
        if (g_deferBoard.load(std::memory_order_acquire) != board) {
            lzt_core::log_write("DEFER: board changed, exit");
            break;
        }
        if (g_origBoardZoom == nullptr) break;

        // 写 board 前确认内存有效（板对象可能已被释放）
        if (!lzt_core::memory_range_accessible(board + BOARD_286_OFF, 4, true)) {
            lzt_core::log_write("DEFER: board memory unavailable, exit");
            break;
        }

        const bool highView = g_viewHooksEnabled.load(std::memory_order_acquire);
        if (!board_scale_ready(board, highView)) continue;   // 未就绪 → 继续轮询

        lzt_core::log_write("DEFER: layout ready (scale=%.4f), rerun BoardZoom + align",
                            (double)*(float *)(board + BOARD_280_OFF));

        AlignOutcome outcome = ALIGN_RETRY;
        {
            std::lock_guard<std::mutex> lk(g_alignMutex);
            if (!lzt_core::memory_range_accessible(board + BOARD_286_OFF, 4, true)) break;
            if (highView) *(float *)(board + BOARD_280_OFF) = 1.0f;  // 模拟 H1 的 pre
            g_origBoardZoom(board);        // 用已就绪的 scale 重算 b283~286
            outcome = apply_align(board, "DEFER");
        }

        if (outcome == ALIGN_OK) {
            g_deferArmed.store(false, std::memory_order_release);
            break;
        }
        if (outcome == ALIGN_DONE_SKIP) {   // 设备/环境无需对齐
            g_deferArmed.store(false, std::memory_order_release);
            break;
        }
        // ALIGN_RETRY：重跑后仍未就绪（scale 动画仍在推进）→ 继续轮询
        lzt_core::log_write("DEFER: layout still not ready after rerun, keep waiting");
    }

    g_deferArmed.store(false, std::memory_order_release);
    return nullptr;
}

static void arm_defer(uintptr_t board) {
    if (board == 0) return;
    g_deferBoard.store(board, std::memory_order_release);
    bool expected = false;
    if (!g_deferArmed.compare_exchange_strong(expected, true)) return;  // 已有线程在轮询
    pthread_t th;
    if (pthread_create(&th, nullptr, defer_thread, reinterpret_cast<void*>(board)) != 0) {
        g_deferArmed.store(false, std::memory_order_release);
        return;
    }
    pthread_detach(th);
}

static long hkBoardZoom(uintptr_t board) {
    // 首次进关卡（运行时）时启动 patch 存活监控：构造期不能建线程。
    static std::atomic<bool> s_monitorKicked{false};
    if (!s_monitorKicked.exchange(true)) {
        extern void lzt_ios_start_patch_monitor();
        lzt_ios_start_patch_monitor();
    }

    if (g_origBoardZoom == nullptr) return 0;
    if (!lzt_core::memory_range_accessible(board + BOARD_286_OFF, 8, true)) {
        return g_origBoardZoom(board);
    }

    const bool highView = g_viewHooksEnabled.load(std::memory_order_acquire);
    // pre：高视角把缩放钉在 1.0，保证原函数按高视角算出 board[283]
    if (highView) *(float *)(board + BOARD_280_OFF) = 1.0f;

    long ret = g_origBoardZoom(board);

    bool needDefer = false;
    {
        std::lock_guard<std::mutex> lk(g_alignMutex);
        if (apply_align(board, "H1") == ALIGN_RETRY) {
            needDefer = true;   // 布局未就绪：锁外再武装 DEFER（避免重入）
        }
    }
    if (needDefer) arm_defer(board);
    return ret;
}

static long hkBoardZoom2(uintptr_t board) {
    if (g_origBoardZoom2 == nullptr) return 0;

    // 低视角：原样放行，让游戏算出它自己的缩放（1.27 等）。
    // 这一条是「checkbox 状态驱动相机」的关键——早期版本无条件强制 1.0，
    // 导致切到低视角后画面没有任何变化。
    if (!g_viewHooksEnabled.load(std::memory_order_acquire)) {
        return g_origBoardZoom2(board);
    }

    // 与 Android 一致：pre 阶段把缩放钉在 1.0，再让原函数据此重算布局
    if (board && lzt_core::memory_range_accessible(board + BOARD_280_OFF, 4, true)) {
        float *scale = reinterpret_cast<float *>(board + BOARD_280_OFF);
        const float before = *scale;
        *scale = 1.0f;
        long ret = g_origBoardZoom2(board);
        float after = *scale;
        if (after != 1.0f) {
            *scale = 1.0f;   // 原函数可能重新计算，这里再钉一次
            after = 1.0f;
        }
        static std::atomic<int> s_log{0};
        if (s_log.fetch_add(1) < 6) {
            lzt_core::log_write("H0 BoardZoom2: scale %.4f -> %.4f -> %.4f",
                                (double)before, 1.0, (double)after);
        }
        g_watchBoard.store(board, std::memory_order_release);
        start_board_watchdog(board);   // 保底：被别处改写时 16ms 内还原
        return ret;
    }
    return g_origBoardZoom2(board);
}

// ============================================================
// 街道恐龙生成补偿（对应 Android hkStreetDinos）
//
// 恐龙生成的 X 基准由 board[270] 派生：SpawnStreetDinos 用 b285 + b270、
// PlaceStreetDinos 用 b286 + b270（0x1005edff0 / 0x1005edd7c 反汇编实证）。
// 我们为左对齐改写了 board[270]，这些基准会整体偏移，表现为恐龙位置错位。
// 补偿：把 xBase 减去「对齐造成的偏移」（= 对齐后 b270 − 对齐前 b270）。
// ============================================================
static uintptr_t (*g_origStreetDinos)(uintptr_t, unsigned int, unsigned int) = nullptr;
static void* g_hookStreetDinos = nullptr;

static uintptr_t hkStreetDinos(uintptr_t ctx, unsigned int xBase, unsigned int spawnMode) {
    if (g_origStreetDinos == nullptr) return 0;

    // 仅在「同一关卡 + 本视角确实执行过对齐」时补偿（与 Android 同条件）：
    // 用游戏自己的 getter 取当前 board，与对齐时记录的 board 比对，避免串关卡。
    using GetBoardFn = uintptr_t (*)();
    auto getBoard = reinterpret_cast<GetBoardFn>(lzt_core::base() + OFF_GetBoard);
    const uintptr_t curBoard = getBoard ? getBoard() : 0;
    const uintptr_t snapBoard = g_alignBoard.load(std::memory_order_acquire);

    if (curBoard != 0 && curBoard == snapBoard &&
        lzt_core::memory_range_accessible(curBoard + BOARD_270_OFF, 4, false)) {
        const bool highView = g_viewHooksEnabled.load(std::memory_order_acquire);
        const float aspect = ios_screen_aspect();
        if (should_align_for_view(aspect, highView)) {
            const int origB270 = g_origB270.load(std::memory_order_relaxed);
            const int curB270 = *(int *)(curBoard + BOARD_270_OFF);
            if (curB270 != origB270) {
                const unsigned int fixed =
                    (unsigned int)((int)xBase + (origB270 - curB270));
                lzt_core::log_write("DINO xBase fix: %u -> %u (orig_b270=%d cur_b270=%d "
                                    "delta=%d spawn=%u)",
                                    xBase, fixed, origB270, curB270,
                                    origB270 - curB270, spawnMode);
                xBase = fixed;
            }
        }
    }
    return g_origStreetDinos(ctx, xBase, spawnMode);
}

static void install_view_hooks_impl() {
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    if (g_hookBoardZoom2 != nullptr) return;   // 幂等

    void *orig = nullptr;
    A64HookFunction(reinterpret_cast<void *>(base + OFF_BoardZoom2),
                    reinterpret_cast<void *>(hkBoardZoom2), &orig);
    if (orig != nullptr) {
        g_origBoardZoom2 = reinterpret_cast<long (*)(uintptr_t)>(orig);
        g_hookBoardZoom2 = reinterpret_cast<void *>(orig);
        ::lzt_ios_register_hook(base + OFF_BoardZoom2, (void*)hkBoardZoom2);
        lzt_core::log_write("view hooks: BoardZoom2 hooked (force high view)");
    } else {
        lzt_core::log_write("view hooks: BoardZoom2 hook FAILED");
    }

    // Hook 1：对齐修正（高低视角均挂载，低视角仅左对齐不居中）
    if (g_hookBoardZoom == nullptr) {
        void *orig2 = nullptr;
        A64HookFunction(reinterpret_cast<void *>(base + OFF_BoardZoom),
                        reinterpret_cast<void *>(hkBoardZoom), &orig2);
        if (orig2 != nullptr) {
            g_origBoardZoom = reinterpret_cast<long (*)(uintptr_t)>(orig2);
            g_hookBoardZoom = orig2;
            ::lzt_ios_register_hook(base + OFF_BoardZoom, (void*)hkBoardZoom);
            lzt_core::log_write("view hooks: BoardLayout_ApplyZoom hooked (align)");
        } else {
            lzt_core::log_write("view hooks: BoardLayout_ApplyZoom hook FAILED");
        }
    }

    // 街道恐龙补偿：总是挂载（内部按「同关卡 + 已对齐」自判是否补偿）
    if (g_hookStreetDinos == nullptr) {
        void* orig3 = nullptr;
        A64HookFunction(reinterpret_cast<void*>(base + OFF_StreetDinos),
                        reinterpret_cast<void*>(hkStreetDinos), &orig3);
        if (orig3 != nullptr) {
            g_origStreetDinos = reinterpret_cast<uintptr_t (*)(uintptr_t, unsigned int,
                                                              unsigned int)>(orig3);
            g_hookStreetDinos = orig3;
            ::lzt_ios_register_hook(base + OFF_StreetDinos, (void*)hkStreetDinos);
            lzt_core::log_write("view hooks: StreetDinos hooked (xBase compensate)");
        } else {
            lzt_core::log_write("view hooks: StreetDinos hook FAILED");
        }
    }
}

static void uninstall_view_hooks_impl() {
    // iOS 下不卸载补丁（inline hook 还原涉及签名页，收益低风险高）；
    // 低视角通过关闭「强制 1.0」分支达成——用开关控制即可。
    lzt_core::log_write("view hooks: keep patched, gate by state");
}

// 供业务模块调用的门面（对应 Android 的 lzt_view_hooks_set_enabled）
bool lzt_view_hooks_set_enabled(bool enable) {
    g_viewHooksEnabled.store(enable, std::memory_order_release);
    if (enable) install_view_hooks_impl();
    else uninstall_view_hooks_impl();
    lzt_core::log_write("view hooks enabled=%d", enable ? 1 : 0);
    // 注意：这里【不要】对当前 board 立即重跑缩放/对齐。
    // 原因（实测崩溃）：g_watchBoard 保存的是上一次关卡里的 board 指针，
    // 退出关卡后该对象可能已释放；在设置页切视角时对它调用原函数会踩到
    // 野指针（SIGSEGV）。Android 的做法也是只挂/卸 hook、不立即重算——
    // 真正生效发生在下一次 BoardZoom 调用（进关卡/布局重算）时，
    // 届时 run_board_align 会按最新视角状态重算全部字段。
    return true;
}

// ============================================================
// iOS 物化：复用游戏原版页面构建流程 + 标签/ID 重映射
//
// 思路（与 Android 的「自己物化整页」不同，但更贴合原版行为）：
//   1. 调用原版 DataSharing 页面函数（0x758A94 的 trampoline）让它按原版
//      顺序把页面搭出来（内容容器、布局、滚动等全部由游戏自己负责）；
//   2. 在构建过程中用钩子替换三样东西：
//        · 页面标题键（0x757B78 wstring 构造时替换）
//        · checkbox 的 id / 文本键 / 选中态（0x758740 创建时替换）
//        · prompt 文本键（构造 prompt 时替换）
//   3. 于是得到的就是「与原生完全同构、但内容属于我们模块」的页面。
//
// 这样做的收益：不需要重新逆向内容容器、字体、文本测量、滚动等一大票
// 原语（那一层在 iOS 上尚未定位），页面观感也与原版天然一致。
// ============================================================

extern "C" bool lzt_ios_materialize_page(uintptr_t page, const wchar_t* titleKey,
                                          const lzt_settings::widgets::WidgetPlanItem* items,
                                          size_t count);

namespace {

// 前置声明：读 iOS/libc++ wstring（定义见本文件后部，供自校准校验使用）
static bool read_ios_wstr(const uint8_t* o, wchar_t* out, size_t cap);

// 定位「行标题文本字段」的自校准。
//
// 为什么需要同时认两种形态：libc++ wstring 对 <=3 个宽字符的文本用【内联短串】
// （长度在字节 +23，数据就在字段起始处），长文本才用堆指针。早期只认长串，
// 于是「内容共享 / 恢复购买 / 视角设置」这类短标题的行定位失败（offset=0），
// 表现为这些行切语言后不跟切（实测）。
static uintptr_t find_row_text_offset(uintptr_t host, bool* wideOut) {
    if (wideOut) *wideOut = true;
    // 严格校验：候选字段读出的宽串必须是【可打印文本】。
    // 教训：某些对象在 +184 处存的是「指向另一个对象的指针」，其形态与长串
    // （指针+长度）相同，仅凭布局无法区分；若把它当标题回填，会覆盖那个对象的
    // 头部（含 vtable），游戏随后调用即崩（实测 addr=0x730000005d，正好是被写入
    // 的文本字节被当成地址）。
    auto printable = [](uintptr_t fieldAddr) -> bool {
        wchar_t tmp[64] = {0};
        if (!read_ios_wstr(reinterpret_cast<const uint8_t*>(fieldAddr), tmp, 64)) return false;
        size_t cnt = 0;
        for (const wchar_t* p = tmp; *p != 0 && cnt < 64; ++p, ++cnt) {
            const uint32_t cp = static_cast<uint32_t>(*p);
            if (cp < 0x20 || cp == 0x7F) return false;   // 控制字符 → 不是文本
            if (cp > 0x10FFFF) return false;
        }
        return cnt > 0;
    };
    // 范围收紧到已知区间：实测标题字段恒为 host+184（Tab 行/滑条行/checkbox 行同构）。
    // 早期放宽到 +320 会误匹配到其它成员，回填时写坏对象（实测触发虚表空指针崩溃）。
    // ⚠️ 只认 host+184：实测所有设置行（Tab/滑条/checkbox）的标题字段恒为 +184。
    // 早期扫描 176..200 会命中 +192（正是标题 wstring 的 size 字段本身），
    // 于是把「wstring 内部字段」当标题写入 → 覆盖对象数据 → 游戏调用其 vtable 时崩溃
    // （实测 crash pc 读 [x19+200]，addr=被写入的文本字节）。
    for (uintptr_t off = 184; off <= 184; off += 8) {
        if (!lzt_core::memory_range_accessible(host + off, 24, false)) continue;
        const uint8_t tail = *reinterpret_cast<const uint8_t*>(host + off + 23);
        if ((tail & 0x80) != 0) {
            // 长串：ptr@+0、len@+8
            const uintptr_t dp = *reinterpret_cast<const uint64_t*>(host + off);
            const uint64_t len = *reinterpret_cast<const uint64_t*>(host + off + 8);
            if (len == 0 || len > 256 || dp < 0x1000 || (dp & 3) != 0) continue;
            if (!lzt_core::memory_range_accessible(dp, (size_t)len * 4, false)) continue;
            if (!printable(host + off)) continue;   // 必须真是文本，否则可能是别的对象指针
            if (wideOut) *wideOut = true;
            return off;
        }
        if (tail > 0 && tail <= 6) {
            // 内联短串：字节 +23 = 字符数，字符数据自字段起始处（4 字节/字符）。
            // 注意：这也是【宽串】字段（只是走了 SSO），必须标 wide，否则回填时
            // 会被误当成窄串用 UTF-8 写入（实测会把短标题写坏）。
            if (!printable(host + off)) continue;
            if (wideOut) *wideOut = true;
            return off;
        }
    }
    return 0;
}

// 前置声明：行挂载钩子认领标题键所需（定义见本文件后部）
static void note_pending_title(const wchar_t* key);

// ---- 设置行文本捕获（含模块 Tab 行）：位置自校准 + 语言切换后回填 ----
//
// 与安卓同理念（捕获为主，不硬编码控件内位置）：
//   · capture：行创建时登记 (宿主对象, vtable, 本地化键)，并用「游戏自己给出的
//     显示文本」在该对象内比对，定位标题文本字段（自校准，不猜布局/偏移）；
//   · refresh：切换语言后按同一字段回填新的本地化文本。
//
// iOS 的键来自「构造标题临时串 → createTab」这一固定序列，反汇编实证：
//   adrp/add 取 UTF-32 键 → bl 0x757b78(wstring 构造) → bl 0x757c18(createTab)
//   例：0x100756ee4(构造 out=sp+128) → 0x100756f18(createTab title=sp+128)
// 因此挂钩 wstring 构造记录 (out, key)，再由 createTab 的 title 参数回查键。
struct RowTextRec {
    uintptr_t host;
    uintptr_t vtable;
    uintptr_t offset;
    bool wide;          // 该字段是宽串(wstring) 还是窄串(string)
    bool keyField;      // 字段里存的是「键」(绘制期本地化) 还是「已本地化文本」
    wchar_t key[64];    // 裸键（不带方括号），供游戏本地化口取值
};
static constexpr int kMaxRowTexts = 64;
static RowTextRec g_rowTexts[kMaxRowTexts];
static std::atomic<int> g_rowTextCount{0};

// 条目存活判定：宿主可读、vtable 未变（对象未被复用）、文本字段可读。
static bool row_alive(const RowTextRec& r) {
    if (r.host == 0 || r.offset == 0) return false;
    if (!lzt_core::memory_range_accessible(r.host, 8, false)) return false;
    if (r.vtable != 0 && *reinterpret_cast<const uintptr_t*>(r.host) != r.vtable) return false;
    return lzt_core::memory_range_accessible(r.host + r.offset, 24, false);
}

// 压缩死条目。设置页每次重建都会产生新的行对象、旧的被销毁；若只增不减，
// 表很快被死条目占满 → 新行再也登记不进来（实测症状：切几次后标题不再更新）。
static void compact_rows() {
    const int n = g_rowTextCount.load(std::memory_order_acquire);
    int w = 0;
    for (int i = 0; i < n; ++i) {
        if (row_alive(g_rowTexts[i])) g_rowTexts[w++] = g_rowTexts[i];
    }
    g_rowTextCount.store(w, std::memory_order_release);
}

// 构造器记录环：键 → 临时串输出缓冲；供 createTab 回查键
struct CtorRec { uintptr_t out; wchar_t key[64]; };
static CtorRec g_ctorRecs[8];
static std::atomic<int> g_ctorRecNext{0};

// 近期构造过的「文本键」集合（去重、环形）：
// 用途 —— 页面显示后据此匹配「哪些行属于可本地化的设置行」，
// 从而在不 hook 各行创建函数、也不做内存形态推断的前提下安全认领行。
constexpr int kMaxRecentKeys = 64;
static wchar_t g_recentKeys[kMaxRecentKeys][64];
static std::atomic<int> g_recentKeyCount{0};

static void note_recent_key(const wchar_t* key) {
    if (key == nullptr || key[0] != L'[') return;
    const int n = g_recentKeyCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxRecentKeys; ++i) {
        if (wcscmp(g_recentKeys[i], key) == 0) return;   // 已有，去重
    }
    if (n >= kMaxRecentKeys) return;
    wcsncpy(g_recentKeys[n], key, 63);
    g_recentKeys[n][63] = 0;
    g_recentKeyCount.store(n + 1, std::memory_order_release);
}

// 前置声明（实现见本文件后部的页面物化/布局部分）
static void (*oWstringCtor)(void*, const wchar_t*) = nullptr;
extern "C" void lzt_ios_page_layout(uintptr_t page);

// 读 iOS/libc++ std::wstring（0x757b78 反汇编实证布局）：
//   长串：数据指针@+0、长度@+8、容量|长串标志@+16（长串标志 = 字节 23 的最高位）
//   短串：数据内联在 +0，长度在字节 +23
static bool read_ios_wstr(const uint8_t* o, wchar_t* out, size_t cap) {
    if (o == nullptr || out == nullptr || cap == 0) return false;
    if (!lzt_core::memory_range_accessible((uintptr_t)o, 24, false)) return false;
    size_t klen = 0;
    const wchar_t* data = nullptr;
    const uint8_t tail = o[23];
    if ((tail & 0x80) != 0) {
        klen = *reinterpret_cast<const uint64_t*>(o + 8);
        const uintptr_t dp = *reinterpret_cast<const uintptr_t*>(o);
        if (klen == 0 || klen >= cap || dp < 0x1000) return false;
        data = reinterpret_cast<const wchar_t*>(dp);
    } else {
        klen = tail;
        if (klen == 0 || klen >= cap) return false;
        data = reinterpret_cast<const wchar_t*>(o);
    }
    if (!lzt_core::memory_range_accessible((uintptr_t)data, klen * 4, false)) return false;
    for (size_t i = 0; i < klen; ++i) {
        const uint32_t ch = static_cast<uint32_t>(data[i]);
        if (ch == 0 || ch > 0x10FFFF) return false;
    }
    memcpy(out, data, klen * 4);
    out[klen] = 0;
    return true;
}

// 读 iOS/libc++ std::string（窄串，同 24 字节布局）：长度在字节 +23，
// 长串时 +0 为数据指针、+8 为长度。
static bool read_ios_str(const uint8_t* o, char* out, size_t cap) {
    if (o == nullptr || out == nullptr || cap == 0) return false;
    if (!lzt_core::memory_range_accessible((uintptr_t)o, 24, false)) return false;
    size_t klen = 0;
    const char* data = nullptr;
    const uint8_t tail = o[23];
    if ((tail & 0x80) != 0) {
        klen = *reinterpret_cast<const uint64_t*>(o + 8);
        const uintptr_t dp = *reinterpret_cast<const uintptr_t*>(o);
        if (klen == 0 || klen >= cap || dp < 0x1000) return false;
        data = reinterpret_cast<const char*>(dp);
    } else {
        klen = tail;
        if (klen == 0 || klen >= cap) return false;
        data = reinterpret_cast<const char*>(o);
    }
    if (!lzt_core::memory_range_accessible((uintptr_t)data, klen, false)) return false;
    for (size_t i = 0; i < klen; ++i) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);
        if (ch < 0x20 || ch == 0x7F) return false;   // 键为可见 ASCII
    }
    memcpy(out, data, klen);
    out[klen] = 0;
    return true;
}

// ---- 游戏自己的本地化取值口（对应安卓 localizer 的 OFF_LocalizeKey）----
//
// 用的是游戏公认入口 0x179c14（全二进制 336 个调用点，游戏画任何界面文本都走它）：
//   x0 = 键（形如 "[SETTINGS_HELP_TITLE]"），x8 = 输出对象（libc++ wstring, 24B）
//   —— 非标准 ABI（输出走 x8），C++ 无法表达，故用无参汇编桥透传。
// 实现在 0x179c14 内：剥掉方括号 → 查游戏当前语言表（0x17a480）→ 命中取文本、
// 未命中回退为裸键。因此调用它取到的文本与游戏自身渲染【完全同源】。
//
// 早期误用 0x1799bc（它是 0x179c14 内部调用的下层函数，需要额外的中间对象），
// 直接按 sret 声明调用会一律返回空 —— 探针实测全空即此因。
struct GameWStr { uint64_t flag; uint64_t size; uint64_t heap; };

// 调用游戏本地化口（x8 = 输出对象，x0 = 键）。
// 为什么用「普通函数 + 内联汇编」而不是 naked shim：
//   naked shim 经函数指针调用时，编译器可能在调用前插入其它调用（如 base()），
//   破坏 x0/x1/x2 参数寄存器 → 实测 br 到 0 崩溃（pc=0）。
//   这里用普通函数接收参数，再在汇编里用固定寄存器搬位，寄存器分配完全可控。
extern "C" __attribute__((noinline)) void lzt_localize_x8_call(uint8_t* out,
                                                               const wchar_t* key,
                                                               void* fn) {
    register uint8_t* a asm("x9") = out;
    register const wchar_t* b asm("x10") = key;
    register void* c asm("x11") = fn;
    __asm__ volatile(
        "mov x8, %0\n"
        "mov x0, %1\n"
        "br  %2\n"
        :: "r"(a), "r"(b), "r"(c));
}

extern "C" void* g_promptOrigFn;   // 0x179c14 的 trampoline（定义见本文件后部）

// 取键在当前游戏语言表里的文本（write 到 out24，libc++ wstring 原始 24 字节）
static void game_localize_into(uint8_t* out24, const wchar_t* bracketedKey) {
    if (out24 == nullptr || bracketedKey == nullptr) return;
    memset(out24, 0, 24);
    if (lzt_core::base() == 0) return;
    // ⚠️ 必须用 trampoline：0x179c14 已被本模块 inline hook（install_l10n_hook）。
    // 若 g_promptOrigFn 尚未就绪就调用原始地址，会再次进入我们的 naked shim，
    // 而 shim 末尾是 `br x9`（x9 = g_promptOrigFn），此时为 0 → 跳到 0 崩溃
    // （实测：设置页初始化期 SIGSEGV pc=0）。因此未就绪时直接返回、不调用。
    void* fn = g_promptOrigFn;
    if (fn == nullptr) return;
    using ShimFn = void (*)(uint8_t*, const wchar_t*, void*);
    ShimFn shim = &lzt_localize_x8_call;
    shim(out24, bracketedKey, fn);
}

// 读 libc++ wstring（24B：flag@0 / size@8 / heap@16，flag&1=长串）到宽串缓冲
static bool gw_read(const uint8_t* obj, wchar_t* out, size_t cap) {
    if (obj == nullptr || out == nullptr || cap == 0) return false;
    if (!lzt_core::memory_range_accessible((uintptr_t)obj, 24, false)) return false;
    size_t len = 0;
    const wchar_t* data = nullptr;
    const uint8_t tail = obj[23];
    if ((tail & 0x80) != 0) {
        len = *reinterpret_cast<const uint64_t*>(obj + 8);
        const uintptr_t dp = *reinterpret_cast<const uint64_t*>(obj);
        if (len == 0 || len >= cap || dp < 0x1000) return false;
        data = reinterpret_cast<const wchar_t*>(dp);
    } else {
        len = tail;
        if (len == 0 || len >= cap) return false;
        data = reinterpret_cast<const wchar_t*>(obj);
    }
    if (!lzt_core::memory_range_accessible((uintptr_t)data, len * 4, false)) return false;
    memcpy(out, data, len * 4);
    out[len] = 0;
    return true;
}

// 取证：打印若干键在当前游戏表里的取值（验证表重载是否真的换了内容）
// 注意：0x179c14 内部带按对象缓存，用它做探针可能读到缓存值；
// 因此这里同时打印「键 + 值」，并与文件访问日志（fileprobe）对照 —— 
// 文件被读取 = 表确实被重新加载（真机实证：重载后新页面文本跟随新语言）。
// 宽串 → UTF-8（不依赖进程 locale）。日志若直接用 %ls，含中文时 libc 转换失败会
// 中止格式化并把整行截断（实测：日志行在 key 之后就没了），因此统一改走本函数 + %s。
static void wstr_to_utf8(const wchar_t* ws, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    size_t n = 0;
    if (ws != nullptr) {
        for (const wchar_t* p = ws; *p != 0; ++p) {
            const uint32_t cp = static_cast<uint32_t>(*p);
            if (cp < 0x80) {
                if (n + 1 >= cap) break;
                out[n++] = static_cast<char>(cp);
            } else if (cp < 0x800) {
                if (n + 2 >= cap) break;
                out[n++] = static_cast<char>(0xC0 | (cp >> 6));
                out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                if (n + 3 >= cap) break;
                out[n++] = static_cast<char>(0xE0 | (cp >> 12));
                out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                if (n + 4 >= cap) break;
                out[n++] = static_cast<char>(0xF0 | (cp >> 18));
                out[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    }
    out[n] = 0;
}

static void log_wstr(const char* tag, const char* key, const wchar_t* ws) {
    char u8[512] = {0};
    wstr_to_utf8(ws, u8, sizeof(u8));
    lzt_core::log_write("%s %s = [%s]", tag, key, u8);
}

// 诊断探针（默认关闭）：打印若干键在当前游戏表里的取值。
// 用途：验证「装载 + apply 之后，运行中的容器是否真的换成了新语言」。
// 开启方式：kEnableLocalizeProbe = true（仅调试构建用，正式版保持关闭）。
static constexpr bool kEnableLocalizeProbe = false;

static void probe_localize(const char* tag) {
    if (!kEnableLocalizeProbe) return;   // 默认静默，避免污染日志
    static const char* kKeys[] = {
        "SETTINGS_HELP_TITLE", "SETTINGS_NEWS_TITLE", "SETTINGS_DATASHARING_PROMPT",
        "VIEW_ANGLE_TITLE", "LANGUAGE_ANGLE_TITLE", "SETTINGS_TITLE",
        // 冷门键：用于区分「表已换但旧键走了缓存」与「表根本没换」。
        // 这些键在启动期 UI 里不会出现，因此若切语言后返回新语言文本，
        // 说明表确实换了（A 生效），只是已查询过的键被缓存住了。
        "ADVICE_USE_MAGNIFYING_GRASS", "SETTINGS_RESET_AGE_TITLE",
        "CRAZY_DAVE_HINT_TITLE",
    };
    if (lzt_core::base() == 0) return;
    for (const char* k : kKeys) {
        wchar_t wk[80] = {0};
        wk[0] = L'[';
        size_t i = 0;
        for (; k[i] != 0 && i < 70; ++i) wk[i + 1] = static_cast<wchar_t>(k[i]);
        wk[i + 1] = L']';
        wk[i + 2] = 0;
        uint8_t obj[24] = {0};
        game_localize_into(obj, wk);
        wchar_t text[128] = {0};
        const bool ok = gw_read(obj, text, 128);
        char u8[512] = {0};
        wstr_to_utf8(ok ? text : L"(empty)", u8, sizeof(u8));
        lzt_core::log_write("locprobe[%s] %s -> [%s]", tag, k, u8);
    }
}

// 取某行的当前语言文本：走游戏自己的本地化口（与游戏渲染同源，
// 因此切语言后的结果与重启后【一致】）。
static bool row_current_text(const wchar_t* bareKey, wchar_t* out, size_t cap) {
    if (bareKey == nullptr || out == nullptr || cap == 0) return false;
    wchar_t bracketed[96] = {0};
    const size_t n = wcslen(bareKey);
    if (n + 3 > 96) return false;
    bracketed[0] = L'[';
    memcpy(bracketed + 1, bareKey, n * sizeof(wchar_t));
    bracketed[n + 1] = L']';
    bracketed[n + 2] = 0;
    uint8_t obj[24] = {0};
    game_localize_into(obj, bracketed);
    return gw_read(obj, out, cap);
}

// 把键归一为裸键（去掉方括号）
static void row_bare_key(const wchar_t* key, wchar_t* out, size_t cap) {
    if (key == nullptr || cap == 0) return;
    size_t n = wcslen(key);
    if (n >= 2 && key[0] == L'[' && key[n - 1] == L']') {
        ++key;
        n -= 2;
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, key, n * sizeof(wchar_t));
    out[n] = 0;
}
static void note_ctor(uintptr_t out, const wchar_t* key) {
    if (out == 0 || key == nullptr || key[0] != L'[') return;
    const int n = g_ctorRecNext.fetch_add(1, std::memory_order_acq_rel);
    CtorRec& r = g_ctorRecs[n & 7];
    r.out = out;
    wcsncpy(r.key, key, 63);
    r.key[63] = 0;
}

static const wchar_t* lookup_ctor_key(uintptr_t out) {
    if (out == 0) return nullptr;
    const int n = g_ctorRecNext.load(std::memory_order_acquire);
    for (int i = 0; i < 8 && i < n; ++i) {
        const CtorRec& r = g_ctorRecs[(n - 1 - i) & 7];
        if (r.out == out) return r.key;
    }
    return nullptr;
}

// 按「已知键」登记行（供行挂载钩子调用：行已建好，键由构造器记录提供）。
// 与 capture_row 的差别只在于键的来源；文本字段仍用形态自校准定位。
extern "C" void lzt_ios_capture_row_by_key(uintptr_t host, const wchar_t* key) {
    if (host == 0 || key == nullptr || key[0] != L'[') return;
    // 严格校验（关键）：只登记「该行当前文本 == 该键的当前语言文本」的行。
    // 教训：早期路径①无条件接受固定偏移 +184，未校验文本是否匹配，
    // 结果把同一页的多个不同行都绑成同一个键（实测：删除账户/消息与更新/
    // 音效/音准 全被绑成 SETTINGS_TITLE，切语言时全被刷成 "Settings"）——
    // 这既是错绑（显示错乱），也可能把文本写到非标题字段（引发崩溃）。
    wchar_t bare[64] = {0};
    row_bare_key(key, bare, 64);
    wchar_t want[128] = {0};
    const bool haveWant = row_current_text(bare, want, 128) && wcslen(want) >= 2;
    uintptr_t found = 0;
    bool wideOk = false;
    if (haveWant) {
        // 在对象头部范围内寻找「文本恰好等于 want」的字段（确定性，无形态猜测）
        for (uintptr_t off = 160; off <= 384; off += 8) {
            if (!lzt_core::memory_range_accessible(host + off, 24, false)) continue;
            wchar_t cur[128] = {0};
            if (!read_ios_wstr(reinterpret_cast<const uint8_t*>(host + off), cur, 128)) continue;
            if (wcscmp(cur, want) == 0) { found = off; break; }
        }
    } else {
        // 取不到当前语言文本时（罕见）退回固定偏移，并做宽串校验
        found = find_row_text_offset(host, &wideOk);
    }
    if (found == 0) return;
    int n = g_rowTextCount.load(std::memory_order_acquire);
    int slot = -1;
    for (int i = 0; i < n; ++i) {
        if (g_rowTexts[i].host == host) { slot = i; break; }
    }
    if (slot < 0) {
        if (n >= kMaxRowTexts) { compact_rows(); n = g_rowTextCount.load(std::memory_order_acquire); }
        if (n >= kMaxRowTexts) return;
        slot = n;
        g_rowTextCount.store(n + 1, std::memory_order_release);
    }
    RowTextRec& r = g_rowTexts[slot];
    r.host = host;
    r.vtable = lzt_core::memory_range_accessible(host, 8, false)
                   ? *reinterpret_cast<const uintptr_t*>(host) : 0;
    r.offset = found;
    r.wide = true;
    r.keyField = false;
    row_bare_key(key, r.key, 64);
    char k8[128] = {0};
    wstr_to_utf8(r.key, k8, sizeof(k8));
    lzt_core::log_write("row capture(bykey): host=%p off=%lu key=%s",
                        (void*)host, (unsigned long)found, k8);
}

// 行创建后调用（createTab 返回处 / insert_custom_tab）。
// 与安卓的 capture 同语义：只登记 (宿主对象, vtable, 本地化键)，位置靠自校准；
// 另加两点 iOS 特有处理：
//   · 键直接取自 createTab 收到的标题串（libc++ wstring，内容即 "[KEY]"），
//     不再用「构造记录环按栈地址回查」——栈槽会被复用，实测会张冠李戴
//     （例如 key=[SETTINGS_ACCOUNT_DELETION_TITLE] 的行文本却是 EA User Agreement）。
//   · 立即按当前语言回填一次：设置页每次打开都重新创建 Tab 行，若不即时回填，
//     重新进入时又显示游戏解析的旧语言文本（实测）。
// ============================================================
// 【对比实验开关】模块文本下发总开关
//
// false = 原汁原味：只写游戏语言标识（+ 原版重载），本模块【不下发任何文本】。
//         用于判定「游戏自己是否会按标识去读 CDN 的 LawnStrings 并刷新页面」。
// true  = 本模块自解析 CDN 表并在文本入口处替换（此前实现，用于兜底比对）。
//
// 注意：本开关不控制「模块自身页面」的标题/文案重映射（那是本模块 UI 的组成部分）。
// ============================================================
static constexpr bool kEnableModuleTextOverride = false;

extern "C" void lzt_ios_capture_row(uintptr_t host, uintptr_t titleObj) {
    if (host == 0 || titleObj == 0) return;
    // 键 = createTab 收到的标题串内容（形如 "[SETTINGS_NEWS_TITLE]"）
    wchar_t keyW[128] = {0};
    if (!read_ios_wstr(reinterpret_cast<const uint8_t*>(titleObj), keyW, 128)) return;
    if (keyW[0] != L'[') return;
    const wchar_t* key = keyW;
    uintptr_t found = 0;
    bool wide = false;
    found = find_row_text_offset(host, &wide);
    lzt_core::log_write("row capture: host=%p key=%ls offset=%lu wide=%d",
                        (void*)host, key, (unsigned long)found, wide ? 1 : 0);
    if (found == 0) return;
    // 登记：按 host 去重（行对象销毁后地址会被复用，旧记录必须被覆盖，
    // 否则表里会残留「同一 host 却属于别的行」的条目，回填写错行）
    int n = g_rowTextCount.load(std::memory_order_acquire);
    int slot = -1;
    for (int i = 0; i < n; ++i) {
        if (g_rowTexts[i].host == host) { slot = i; break; }
    }
    if (slot < 0) {
        if (n >= kMaxRowTexts) { compact_rows(); n = g_rowTextCount.load(std::memory_order_acquire); }
        if (n >= kMaxRowTexts) return;
        slot = n;
        g_rowTextCount.store(n + 1, std::memory_order_release);
    }
    RowTextRec& r = g_rowTexts[slot];
    r.host = host;
    r.vtable = lzt_core::memory_range_accessible(host, 8, false)
                   ? *reinterpret_cast<const uintptr_t*>(host) : 0;
    r.offset = found;
    r.wide = wide;
    r.keyField = false;
    row_bare_key(key, r.key, 64);   // 存裸键（游戏本地化口需要裸键）
    // 注：这里【不】立即回填。与安卓一致 —— 表已由「改标识 + 原版重载」换成新语言，
    // 游戏自己重建页面时就会取到新语言文本；本表只在切换语言时用于刷新
    // 已经存在的行（refresh_rows）。创建期改写标题会破坏正在构建的页面（实测崩溃）。
}

// 语言切换后回填已捕获的设置行文本（已显示界面跟切的一部分）。
// 内存实证：行对象 host+184 是 libc++ wstring(长串)，内容为该行【显示文本】
// （News / Download Preferences 等）。切语言后按登记的键取当前语言文本写回，
// 写入用游戏自己的串构造器，不手写布局 —— 与安卓 refresh_tabrow 同语义。
extern "C" void lzt_ios_refresh_rows(uintptr_t page) {
    // 说明：本函数即安卓 localizer 的 refresh_tabrow 移植 —— 用游戏自己的
    // 本地化口（0x179c14）取【当前语言表】的文本，写回已捕获行的标题字段。
    // 因此它不受「模块表文本覆盖」开关影响（那条开关只控制本模块自解析表）。
    const int total = g_rowTextCount.load(std::memory_order_acquire);
    if (total == 0) return;
    if (oWstringCtor == nullptr) return;
    int done = 0, gone = 0, miss = 0;
    wchar_t firstOld[128] = {0};
    wchar_t firstNew[128] = {0};
    bool haveFirst = false;
    for (int i = 0; i < total; ++i) {
        RowTextRec& r = g_rowTexts[i];
        if (!row_alive(r)) { ++gone; continue; }
        wchar_t text[128] = {0};
        if (!row_current_text(r.key, text, 128)) { ++miss; continue; }
        if (!lzt_core::memory_range_accessible(r.host + r.offset, 24, true)) continue;
        if (r.wide) {
            if (oWstringCtor == nullptr) continue;
            if (done < 6) {
                wchar_t old[128] = {0};
                read_ios_wstr(reinterpret_cast<const uint8_t*>(r.host + r.offset), old, 128);
                char oldU8[256] = {0}, newU8[256] = {0}, keyU8[256] = {0};
                wstr_to_utf8(old, oldU8, sizeof(oldU8));
                wstr_to_utf8(text, newU8, sizeof(newU8));
                wstr_to_utf8(r.key, keyU8, sizeof(keyU8));
                lzt_core::log_write("row refresh(w) key=%s old=[%s] new=[%s]",
                                    keyU8, oldU8, newU8);
            }
            if (!haveFirst) {
                haveFirst = true;
                read_ios_wstr(reinterpret_cast<const uint8_t*>(r.host + r.offset), firstOld, 128);
                wcsncpy(firstNew, text, 127);
            }
            oWstringCtor(reinterpret_cast<void*>(r.host + r.offset), text);
        } else {
            if (oStrCtor == nullptr) continue;
            // 窄串字段：把当前语言文本按 UTF-8 写入（与游戏全局文本编码一致）
            char utf8[512] = {0};
            size_t un = 0;
            bool ok = true;
            for (const wchar_t* p = text; *p != 0; ++p) {
                const uint32_t cp = static_cast<uint32_t>(*p);
                if (cp < 0x80) {
                    if (un + 1 >= sizeof(utf8)) { ok = false; break; }
                    utf8[un++] = static_cast<char>(cp);
                } else if (cp < 0x800) {
                    if (un + 2 >= sizeof(utf8)) { ok = false; break; }
                    utf8[un++] = static_cast<char>(0xC0 | (cp >> 6));
                    utf8[un++] = static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    if (un + 3 >= sizeof(utf8)) { ok = false; break; }
                    utf8[un++] = static_cast<char>(0xE0 | (cp >> 12));
                    utf8[un++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8[un++] = static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    if (un + 4 >= sizeof(utf8)) { ok = false; break; }
                    utf8[un++] = static_cast<char>(0xF0 | (cp >> 18));
                    utf8[un++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    utf8[un++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8[un++] = static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
            if (!ok) continue;
            utf8[un] = 0;
            if (done < 3) {
                char oldA[256] = {0};
                read_ios_str(reinterpret_cast<const uint8_t*>(r.host + r.offset), oldA, 256);
                lzt_core::log_write("row refresh(n): host=%p off=%lu key=%ls old=%s new=%s",
                                    (void*)r.host, (unsigned long)r.offset, r.key, oldA, utf8);
            }
            oStrCtor(reinterpret_cast<void*>(r.host + r.offset), utf8);
        }
        ++done;
    }
    lzt_core::log_write("row refresh: written=%d miss=%d gone=%d total=%d",
                        done, miss, gone, total);
    // 清掉死条目，给后续重建登记腾出位置
    if (gone > 0) compact_rows();

    // ---- 回填后处理与第二处缓存排查 ----
    // 现象：字段确实写入了新文本，但 Tab 仍显示旧文本 → 说明要么需要重排重绘，
    // 要么游戏另有一份「已解析文本」缓存。这里两者都查：
    //   ① 扫描页面对象，找是否还有字段等于刚被替换掉的旧文本；
    //   ② 请求一次页面重排（原版 dispatch 各 case 尾部同款收尾）。
    if (haveFirst && page != 0) {
        int hits = 0;
        for (uintptr_t off = 0; off <= 0x800; off += 8) {
            wchar_t cur[128] = {0};
            if (!read_ios_wstr(reinterpret_cast<const uint8_t*>(page + off), cur, 128)) continue;
            if (wcsncmp(cur, firstOld, 127) == 0) {
                lzt_core::log_write("  stale text @page+%lu = [%ls]",
                                    (unsigned long)off, cur);
                if (++hits >= 8) break;
            }
        }
        lzt_core::log_write("stale scan: page=%p old=[%ls] new=[%ls] hits=%d",
                            (void*)page, firstOld, firstNew, hits);
    }
    if (page != 0 && done > 0) lzt_ios_page_layout(page);
}
//
// iOS 上游戏的语言表常驻内存、改标识不重载（实测其表加载路径不参与正常启动），
// 因此由本模块自行解析 CDN 里对应语言的 LawnStrings 表
// （见语言模块的 language_load_lawnstrings）。
// 物化控件前查一次：命中则传显示文本，未命中沿用原键（游戏行为不变）。
// 这是「切换语言后 UI 文本立即更新」的关键：重建页面时即可取到新语言文本。
// 文本查询接口声明见 language_module.h（本文件已 include）。
static std::atomic<int> g_remapHits{0};
static std::atomic<int> g_remapMiss{0};

static bool remap_text(const wchar_t* key, wchar_t* buf, size_t cap) {
    if (key == nullptr || buf == nullptr || cap == 0) return false;
    if (language_lookup_text(key, buf, cap)) {
        g_remapHits.fetch_add(1);
        return true;
    }
    g_remapMiss.fetch_add(1);
    return false;
}

// 释放 wstring 缓冲（libc++ 布局：+23 最高位为长串标志，此时 +0 是堆指针）。
// 原版创建控件后同样会释放（反汇编 0x758d88-0x758d94），不释放会逐次泄漏。
static void free_wstr_buf(uint8_t* buf) {
    if (buf == nullptr) return;
    if (buf[23] & 0x80) {
        void* p = *reinterpret_cast<void**>(buf);
        if (p != nullptr) operator delete(p);
    }
    memset(buf, 0, 24);
}

// ---- 行挂载钩子：捕获内容容器，并在「原版最后一行挂载完成」时追加剩余控件 ----
//
// 为什么必须在这个时机追加（多轮实测得出的关键结论）：
//   原版页面构建函数在【末尾】会调用 0x758268(controller, content) 设置内容，
//   之后 controller 才据此计算布局与滚动范围。
//   若在构建函数返回【之后】才追加（早期做法），controller 已按原版 2 行设置完毕，
//   追加的行虽然确实进了 content 的向量（实测 rows=9），却不参与滚动范围计算 ——
//   表现正是「8 项只显示 7 项、无法滑动、也没有滑条」。
//   因此追加必须发生在「原版最后一个 checkbox 挂载完成、构建函数尚未收尾」时。
static void (*oRowAttachFn)(uintptr_t, uintptr_t, uint32_t, float) = nullptr;
static std::atomic<uintptr_t> g_lastContent{0};
static std::atomic<uint32_t> g_lastAttachArg{0};
static std::atomic<float> g_lastAttachWidth{0.0f};
static std::atomic<uintptr_t> g_lastPrevArg{0};
static std::atomic<int> g_attachCount{0};
static std::atomic<bool> g_appending{false};

// 原版 DataSharing 页固定创建 2 个 checkbox（反汇编实证：id=13 ENABLE、id=14 DISABLE）。
static constexpr int kOriginalCheckboxes = 2;

// 前向声明（实现见下）
void append_extra_checkboxes();

// 构建期上下文（仅 UI 线程使用，无需加锁）
struct RemapCtx {
    bool active = false;
    const wchar_t* titleKey = nullptr;
    const lzt_settings::widgets::WidgetPlanItem* items = nullptr;
    size_t count = 0;
    size_t checkboxIndex = 0;       // 第几个 checkbox（按构建顺序）
    size_t promptIndex = 0;         // 第几个 prompt
    uintptr_t page = 0;             // 本次构建的页面（追加控件时创建 checkbox 需要）
};
RemapCtx g_remap;

// 按顺序取下一个指定类型的控件计划
const lzt_settings::widgets::WidgetPlanItem* next_plan(uint8_t type, size_t* counter) {
    for (size_t i = *counter; i < g_remap.count; ++i) {
        if (g_remap.items[i].type == type) {
            *counter = i + 1;
            return &g_remap.items[i];
        }
    }
    return nullptr;
}

// ---- 钩子 1：wstring 构造（out, key）——替换页面标题键 / 记录行标题键 ----
// 说明：本函数即游戏构造「设置行标题临时串」的唯一入口（反汇编实证），
// 因此在这里记录 (out, key)，供 createTab 返回后把标题串还原成键（捕获用）。
static std::atomic<int> g_ctorN{0};
static void hkWstringCtor(void* out, const wchar_t* key) {
    note_ctor(reinterpret_cast<uintptr_t>(out), key);
    note_recent_key(key);      // 留存近期文本键，供页面显示后安全认领设置行
    note_pending_title(key);   // 供行挂载时认领该行的标题键（覆盖 Tab/滑条/checkbox 行）
    if (g_remap.active && g_remap.titleKey && key) {
        uintptr_t base = lzt_core::base();
        if (reinterpret_cast<uintptr_t>(key) == base + STR_SETTINGS_DATASHARING_TITLE) {
            g_ctorN.fetch_add(1);
            // 标题同样做文本替换（切语言后重建页面即显示新语言标题）
            wchar_t textBuf[128];
            const wchar_t* useKey = g_remap.titleKey;
            if (remap_text(g_remap.titleKey, textBuf, 128)) useKey = textBuf;
            if (oWstringCtor) oWstringCtor(out, useKey);
            return;
        }
    }
    if (oWstringCtor) oWstringCtor(out, key);
}

// 安装 wstring 构造器 hook（幂等）。必须在设置页初始化之前调用，
// 否则 Tab 行创建时拿不到构造器指针，无法按当前语言回填标题。
// 说明：滑块行（Music / SoundFX）曾经尝试单独 hook 其创建函数 0x757ff8，
// 但实测该函数首指令含 PC 相对寻址，inline hook 后其函数体内部会崩
// （crash pc 落在该函数体内 +0x7582dc）。改为【复用已挂的行挂载钩子】：
// 行挂载 (0x757e58) 对 Tab 行 / 滑条行 / checkbox 行统一触发，
// 而每行的标题键由构造器 hook 先行记录（ctor → createRow → rowAttach 固定序列），
// 因此挂载时取「最近的构造器记录」即为该行标题键。
static std::atomic<bool> g_pendingTitleValid{false};
static wchar_t g_pendingTitle[64] = {0};
static std::atomic<int> g_rowCapturePending{0};

static void note_pending_title(const wchar_t* key) {
    if (key == nullptr || key[0] != L'[') return;
    wcsncpy(g_pendingTitle, key, 63);
    g_pendingTitle[63] = 0;
    g_pendingTitleValid.store(true, std::memory_order_release);
}

// 行捕获：按「宿主 + 键」登记（实现见本文件后部）
extern "C" void lzt_ios_capture_row_by_key(uintptr_t host, const wchar_t* key);

// 行挂载时的安全认领：
//   行挂载 (0x757e58) 是 Tab 行 / 滑条行 / checkbox 行【共同】的落点，
//   而滑条行（Music / Sound FX）不经过行初始化器（实测 rowinit 中无 OPTIONS 键）。
//   做法：用「近期构造过的文本键」的当前语言文本，在该行对象内【递归精确匹配】
//   文本字段 —— 必须文本完全相同才认，无任何形态猜测，因此不会误伤。
static void try_claim_row_by_text(uintptr_t row) {
    // 说明：严格匹配已收敛进 lzt_ios_capture_row_by_key（它会校验文本一致），
    // 这里只需把它需要的「候选键」逐个试一遍即可。
    if (row == 0) return;
    const int nk = g_recentKeyCount.load(std::memory_order_acquire);
    if (nk == 0) return;
    for (int k = 0; k < nk; ++k) {
        const int before = g_rowTextCount.load(std::memory_order_acquire);
        lzt_ios_capture_row_by_key(row, g_recentKeys[k]);
        if (g_rowTextCount.load(std::memory_order_acquire) > before) return;
    }
}

static void hkInstallRowAttachHook();   // 定义见上（须在设置页初始化前调用）
static void hkRowAttach(uintptr_t content, uintptr_t row, uint32_t a3, float width);

static void hkInstallWstringCtorHook() {
    if (lzt_core::base() == 0) return;
    // 注：滑条行（Music / SoundFX）不单独挂钩其创建函数 0x757ff8 ——
    // 实测该函数首指令含 PC 相对寻址，inline hook 后其函数体会崩
    // （crash pc 落于该函数体内 +0x7582dc）。改由「行挂载钩子 + 标题键待认领」覆盖，
    // 见 hkRowAttach 与 note_pending_title。
    // 窄串构造器（std::string(const char*)）：无需 hook，只需地址 —— 语言切换时
    // 用它构造「表名/模式串」对象（必须在设置页打开之前就绪）。
    if (oStrCtor == nullptr) {
        oStrCtor = reinterpret_cast<void (*)(void*, const char*)>(
            lzt_core::base() + OFF_SettingsStdStringCtor);
    }
    if (oWstringCtor != nullptr) return;
    void* t = nullptr;
    A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + OFF_SettingsWstringCtor),
                    reinterpret_cast<void*>(hkWstringCtor), &t);
    if (t != nullptr) {
        oWstringCtor = reinterpret_cast<void (*)(void*, const wchar_t*)>(t);
        lzt_core::log_write("wstring ctor hook(0x757b78): ok");
    } else {
        lzt_core::log_write("wstring ctor hook(0x757b78): FAIL");
    }
}

// 行挂载钩子（0x757e58）也必须在设置页初始化【之前】装好：
// 设置页的行（含 Tab 行、Music/SoundFX 滑条行）在 0x756AE4 里一次性创建，
// 若此钩子等到「打开我们模块页」才装，滑条行就永远捕获不到（实测：
// Music / Sound FX 切语言不跟切，而 Tab 行正常）。
static void hkInstallRowAttachHook() {
    if (oRowAttachFn != nullptr || lzt_core::base() == 0) return;
    void* t4 = nullptr;
    A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + OFF_SettingsRowAttach),
                    reinterpret_cast<void*>(hkRowAttach), &t4);
    if (t4 != nullptr) {
        oRowAttachFn = reinterpret_cast<void (*)(uintptr_t, uintptr_t, uint32_t, float)>(t4);
        lzt_core::log_write("row attach hook(0x757e58): ok");
    } else {
        lzt_core::log_write("row attach hook(0x757e58): FAIL");
    }
}

// ---- 钩子 2：checkbox 创建（page, id, key, checked, prevRow）----
static uintptr_t (*oIosCheckboxCreate)(uintptr_t, uint32_t, uintptr_t, uint32_t, uintptr_t) = nullptr;
static std::atomic<int> g_cbN{0};
// 本轮构建中「原版调用创建 checkbox」的次数。
// 追加必须等原版把它的控件都创建完，否则会把本该由原版第二个 checkbox 使用的
// 计划项提前用掉，导致出现「多余控件 + 重复标签」（实测：语言页出现 3 个 checkbox）。
static std::atomic<int> g_cbCreates{0};
static uintptr_t hkIosCheckboxCreate(uintptr_t page, uint32_t id, uintptr_t key,
                                     uint32_t checked, uintptr_t prevRow) {
    g_lastPrevArg.store(prevRow, std::memory_order_relaxed);
    if (g_remap.active) g_cbCreates.fetch_add(1, std::memory_order_relaxed);
    if (g_remap.active) {
        const auto* plan = next_plan(lzt_settings::widgets::WidgetPlanItem::CHECKBOX,
                                     &g_remap.checkboxIndex);
        if (plan) {
            // 文本替换：命中当前语言的 LawnStrings 则直接传显示文本
            wchar_t textBuf[128];
            const wchar_t* useKey = plan->key;
            if (remap_text(plan->key, textBuf, 128)) useKey = textBuf;
            alignas(16) uint8_t wbuf[32] = {0};
            if (oWstringCtor) oWstringCtor(wbuf, useKey);
            g_cbN.fetch_add(1);
            const uintptr_t r = oIosCheckboxCreate ? oIosCheckboxCreate(
                       page, plan->id, reinterpret_cast<uintptr_t>(wbuf),
                       plan->initialState ? 1u : 0u, prevRow) : 0;
            free_wstr_buf(wbuf);   // 长串需释放堆（与原版一致）
            return r;
        }
    }
    const uintptr_t row = oIosCheckboxCreate ?
        oIosCheckboxCreate(page, id, key, checked, prevRow) : 0;
    // 原版 checkbox 行：同样登记标题键（安卓的 TabRow 类型覆盖 Tab 行/滑条行/
    // checkbox 行，标题都在 host+184），切换语言时由同一刷新器回填。
    if (!g_remap.active && row != 0 && key != 0) {
        lzt_ios_capture_row(row, key);
    }
    return row;
}

static void hkRowAttach(uintptr_t content, uintptr_t row, uint32_t a3, float width) {
    const bool active = g_remap.active;
    if (active) {
        if (content != 0) g_lastContent.store(content, std::memory_order_release);
        g_lastAttachArg.store(a3, std::memory_order_relaxed);
        g_lastAttachWidth.store(width, std::memory_order_relaxed);
    }

    if (oRowAttachFn) oRowAttachFn(content, row, a3, width);   // 先按原版挂载

    // 行挂载（0x757e58）对 Tab 行 / 滑条行 / checkbox 行统一触发；
    // 标题键由构造器 hook 先行记录，这里取「最近的记录」作为该行标题键 ——
    // 与安卓 capture 同语义（只登记 对象 + 键，位置自校准）。
    // ⚠️ 注意执行顺序：下面的「原版设置行认领」必须在任何提前 return 之前执行，
    // 因为原版设置页并不处于本模块的重映射状态（active=false），
    // 早期把认领放在 `if (!active) return;` 之后，导致它永远跑不到（实测）。
    if (!g_remap.active && !g_appending.load(std::memory_order_acquire)) {
        try_claim_row_by_text(row);
    }
    g_pendingTitleValid.store(false, std::memory_order_release);

    if (!active || g_appending.load(std::memory_order_acquire)) return;

    const int n = g_attachCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    // 触发条件：挂载数与原版 checkbox 创建数都已达到「原版固定数量」。
    // 若只看挂载数（旧实现），会在原版第二个 checkbox 创建【之前】就追加，
    // 把它的计划项抢走 → 语言页多出一个原版控件、标签重复。
    if (n >= kOriginalCheckboxes &&
        g_cbCreates.load(std::memory_order_acquire) >= kOriginalCheckboxes) {
        // 原版最后一行已挂载：此刻仍在构建函数的调用栈内，
        // 因此构建函数末尾的「设置内容」会把追加行一并纳入布局与滚动范围。
        g_appending.store(true, std::memory_order_release);
        append_extra_checkboxes();
        g_appending.store(false, std::memory_order_release);
    }
}

static size_t count_checkboxes() {
    size_t n = 0;
    for (size_t i = 0; i < g_remap.count; ++i) {
        if (g_remap.items[i].type == lzt_settings::widgets::WidgetPlanItem::CHECKBOX) ++n;
    }
    return n;
}

// 追加剩余 checkbox：原版页面只创建固定数量的控件（实测 DataSharing 为 2 个），
// 因此模块声明超过该数量时（如语言页 8 种语言），多出的部分由本模块自己创建并
// 挂到同一内容容器——复用的仍是游戏自己的控件创建/挂载原语，观感与原版一致。
static void append_extra_checkboxes() {
    if (g_remap.items == nullptr || g_remap.page == 0) return;
    const uintptr_t page = g_remap.page;
    const uintptr_t content = g_lastContent.load(std::memory_order_acquire);
    if (content == 0 || oIosCheckboxCreate == nullptr || oRowAttachFn == nullptr) {
        lzt_core::log_write("ios-materialize: append skipped (content=%p)", (void*)content);
        return;
    }
    const uint32_t arg3 = g_lastAttachArg.load(std::memory_order_relaxed);
    const float width = g_lastAttachWidth.load(std::memory_order_relaxed);
    // create 的第 5 参数用「原版首次记录的固定值」：反汇编显示原版两次 create 传的是
    // 同一个值（0x758d7c / 0x758dd8 均用 x25，而 x25 在第二次调用之后才更新）。
    // 早期改成「上一个创建的行」会让行间关联错乱（实测控件显示异常）。
    const uintptr_t fixedPrev = g_lastPrevArg.load(std::memory_order_relaxed);
    // 原版已创建的 checkbox 数量 = 已消耗的计划数（create 钩子按顺序替换）
    const size_t alreadyCreated = g_remap.checkboxIndex;

    // 取证：记录内容容器的行数（container+200 为行指针数组 begin，+208 为 end）
    auto row_count = [](uintptr_t c) -> long {
        if (!lzt_core::memory_range_accessible(c + 200, 16, false)) return -1;
        const uintptr_t b = *reinterpret_cast<uintptr_t*>(c + 200);
        const uintptr_t e = *reinterpret_cast<uintptr_t*>(c + 208);
        if (e < b || (e - b) % 8 != 0) return -1;
        return static_cast<long>((e - b) / 8);
    };
    const long rowsBefore = row_count(content);

    size_t cbIndex = 0;
    int appended = 0;
    for (size_t i = 0; i < g_remap.count; ++i) {
        if (g_remap.items[i].type != lzt_settings::widgets::WidgetPlanItem::CHECKBOX) continue;
        ++cbIndex;
        if (cbIndex <= alreadyCreated) continue;
        const auto& plan = g_remap.items[i];
        if (plan.key == nullptr) continue;
        alignas(16) uint8_t wbuf[32] = {0};
        if (oWstringCtor) oWstringCtor(wbuf, plan.key);
        const uintptr_t row = oIosCheckboxCreate(
            page, plan.id, reinterpret_cast<uintptr_t>(wbuf),
            plan.initialState ? 1u : 0u, fixedPrev);
        if (row != 0) {
            oRowAttachFn(content, row, arg3, width);
            ++appended;
        }
    }
    lzt_core::log_write("ios-materialize: appended %d extra checkbox(es) "
                        "(plan=%zu original=%zu) content=%p rows=%ld->%ld",
                        appended, count_checkboxes(), alreadyCreated, (void*)content,
                        rowsBefore, row_count(content));

    // 追加完成 → 通知 controller「内容已更新」（纯重排通知）。
    //
    // ⚠️ 只调 vtable[88] 这一个方法，绝不能调 0x758268（内容替换函数）：
    // 后者会先释放旧内容再存入新内容（反汇编 0x75827c-0x7582ac：
    // ldr x1,[x0,#208] → vtable[96] 释放 → str x19,[x20,#208]）。
    // 我们传的正是当前正在使用的 content，走替换流程等于先销毁再使用
    // → 立即崩溃（实测）。
    if (page != 0 && lzt_core::memory_range_accessible(page + SETTINGS_PAGE_OWNER, 8, false)) {
        const uintptr_t owner = *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_OWNER);
        if (owner != 0 && lzt_core::memory_range_accessible(owner + SETTINGS_OWNER_CONTROLLER, 8, false)) {
            const uintptr_t controller =
                *reinterpret_cast<uintptr_t*>(owner + SETTINGS_OWNER_CONTROLLER);
            if (controller != 0 && lzt_core::memory_range_accessible(controller, 8, false)) {
                const uintptr_t vt = *reinterpret_cast<uintptr_t*>(controller);
                if (lzt_core::memory_range_accessible(vt + 88, 8, false)) {
                    using RelayoutFn = void (*)(uintptr_t, uintptr_t);
                    auto relayout = reinterpret_cast<RelayoutFn>(
                        *reinterpret_cast<uintptr_t*>(vt + 88));
                    if (relayout != nullptr) relayout(controller, content);
                }
            }
        }
    }
}
// ============================================================
// 【v23】接管语言标识解析（按用户设计的优先级）
//
// 优先级：Flag > 用户配置（lzt.lang.*）> 游戏自身配置 > 系统语言
//
// 三条规则（用户指定）：
//   ① 游戏传入 FourCC 时，与 Flag 比对：
//        相同 → 放行原版参数
//        不同 → 用 Flag 的值传入
//   ② Flag 不存在 → 放行原版数据；等用户配置可读后，再设置 FourCC
//      并主动触发一次语言切换（延迟补一次，不阻塞启动）
//   ③ 用户配置也不存在 → 放行
//
// 关键设计：hook 内部【绝不访问语言表】。它只读一个已经算好的
// FourCC（g_flagFourcc）并与入参比对，因此无时机要求、无锁、无分配。
// 语言表的加载则完全推迟到主界面之后（闸门解除时），互不干扰。
// ============================================================
static std::atomic<uint32_t> g_flagFourcc{0};    // Flag 的 FourCC；0 = 无 Flag
static std::atomic<uint32_t> g_userFourcc{0};    // 用户配置的 FourCC；0 = 无配置
static std::atomic<bool> g_langReady{false};     // 允许接管（Flag 已读）
extern "C" void (*g_origFourccToString)(void*, uint32_t) = nullptr;

static constexpr uintptr_t OFF_FourccToString = 0x1AC6D50;

extern "C" uint32_t lzt_ios_lang_pick_fourcc(uint32_t incoming) {
    if (!g_langReady.load(std::memory_order_acquire)) return incoming;

    // 规则①：Flag 与传入值比对。相同放行，不同用 Flag。
    const uint32_t flag = g_flagFourcc.load(std::memory_order_acquire);
    if (flag != 0) {
        // 健全性校验：四个字节必须都是 A-Z。异常则视为无 Flag（规则②③）。
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = (flag >> (24 - i * 8)) & 0xFF;
            if (c < 'A' || c > 'Z') { ok = false; break; }
        }
        if (!ok) {
            lzt_core::log_write("LangOverride: bad flag fourcc 0x%08X, ignore", flag);
        } else if (flag == incoming) {
            static std::atomic<int> s_n{0};
            if (s_n.fetch_add(1) < 3) {
                lzt_core::log_write("LangOverride: flag==incoming 0x%08X, pass through",
                                    incoming);
            }
            return incoming;                 // 规则①：相同 → 放行
        } else {
            static std::atomic<int> s_n{0};
            const int n = s_n.fetch_add(1);
            if (n < 6) {
                lzt_core::log_write("LangOverride: 0x%08X -> 0x%08X (flag wins, #%d)",
                                    incoming, flag, n + 1);
            }
            return flag;                     // 规则①：不同 → 用 Flag
        }
    }

    // 规则②：Flag 不存在 → 放行原版。
    // 用户配置若已可读（g_userFourcc 非 0），由 lzt_ios_mark_user_config_ready()
    // 走「设置 + 主动切换」路径，不在这里改参数（此时改会造成半途切换）。
    //
    // 规则③：用户配置也不存在 → 同样放行（即本分支）。
    return incoming;
}

// naked shim：x8 = 输出缓冲（sret），w1 = FourCC。C++ 签名无法表达 x8，
// 故用汇编保存/恢复 x8，只替换 w1。
extern "C" __attribute__((naked)) void hkFourccToString() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-32]!\n"
        "str x8, [sp, #16]\n"              // 保存输出缓冲（x8 非标准参数）
        "mov w0, w1\n"                     // 入参：w0 = fourcc
        "bl  _lzt_ios_lang_pick_fourcc\n"  // 返回：w0 = 最终 fourcc
        "mov w1, w0\n"
        "ldr x8, [sp, #16]\n"              // 恢复输出缓冲
        "ldp x29, x30, [sp], #32\n"
        "adrp x9, _g_origFourccToString@PAGE\n"
        "add  x9, x9, _g_origFourccToString@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "br   x9\n"                        // 尾调用原函数
    );
}

// 安装（幂等）。必须在任何语言表加载之前调用。
static void install_fourcc_override_hook(uintptr_t base) {
    if (base == 0 || g_origFourccToString != nullptr) return;
    void* t = nullptr;
    A64HookFunction(reinterpret_cast<void*>(base + OFF_FourccToString),
                    reinterpret_cast<void*>(hkFourccToString), &t);
    g_origFourccToString = reinterpret_cast<void (*)(void*, uint32_t)>(t);
    lzt_core::log_write("fourcc override hook(0x%lx): %s",
                        (unsigned long)OFF_FourccToString, t ? "ok" : "FAIL");
}

// Flag 已读到 → 设置比对基准并允许接管（规则①②的入口）。
// fourcc==0 表示无 Flag：此时规则②生效，hook 一律放行原版。
extern "C" void lzt_ios_set_flag_fourcc(uint32_t fourcc) {
    g_flagFourcc.store(fourcc, std::memory_order_release);
    lzt_core::log_write("LangOverride: flag fourcc=0x%08X (hook=%d)",
                        fourcc, g_origFourccToString != nullptr ? 1 : 0);
}

extern "C" void lzt_ios_mark_lang_ready(void) {
    if (!g_langReady.exchange(true, std::memory_order_acq_rel)) {
        lzt_core::log_write("LangOverride: lang ready, flag=0x%08X",
                            g_flagFourcc.load());
    }
}

// 读取游戏当前语言标识（LangMgr 次语言字段）。仅用于比对，不写。
static uint32_t lzt_ios_current_langmgr_fourcc(void) {
    const uintptr_t base = lzt_core::base();
    if (base == 0) return 0;
    const uintptr_t gDisp = *reinterpret_cast<volatile uintptr_t*>(base + OFF_G_DisplayInfo);
    if (gDisp == 0) return 0;
    const uintptr_t mgr = *reinterpret_cast<volatile uintptr_t*>(gDisp + DISPLAYINFO_LANGMGR);
    if (mgr == 0) return 0;
    if (!lzt_core::memory_range_accessible(mgr + LANGMGR_LOCALE_ID, 4, false)) return 0;
    return *reinterpret_cast<volatile uint32_t*>(mgr + LANGMGR_LOCALE_ID);
}

// 请求语言模块执行一次【强制运行时切换】（定义见 language_module.cpp）。
// 用 FourCC 定位对应 locale 字符串。
// 声明须在文件作用域：块内 extern "C" 会得到内部链接而链接失败。
//
// 为何不用 lzt_lang_switch_by_locale（→ write_state）：
// 那条路开头会比对「配置里的当前语言」，配置已是目标值时直接返回
// （日志实证：`Language set_state: no change (zh-cn)` 后整段跳过）。
// 而重启补切场景恰恰是「配置对、游戏内存不对」，必须绕过该比对。
extern "C" void lzt_lang_force_runtime_switch(const char *locale);

extern "C" void lzt_ios_request_language_switch(uint32_t fourcc) {
    if (fourcc == 0) return;
    // FourCC → locale 字符串：字节依次为 c0 c1 c2 c3（连字符被省略），
    // 按游戏的 "%c%c-%c%c" 约定还原成 "zh-cn" 形式。
    char locale[16] = {0};
    const char c0 = static_cast<char>((fourcc >> 24) & 0xFF);
    const char c1 = static_cast<char>((fourcc >> 16) & 0xFF);
    const char c2 = static_cast<char>((fourcc >> 8) & 0xFF);
    const char c3 = static_cast<char>(fourcc & 0xFF);
    snprintf(locale, sizeof(locale), "%c%c-%c%c",
             static_cast<char>(c0 - 'A' + 'a'), static_cast<char>(c1 - 'A' + 'a'),
             static_cast<char>(c2 - 'A' + 'a'), static_cast<char>(c3 - 'A' + 'a'));
    lzt_core::log_write("LangOverride: request switch fourcc=0x%08X -> locale=%s",
                        fourcc, locale);
    // 走模块侧【强制】入口：跳过配置比对，直接同步游戏内存与已显示文本
    lzt_lang_force_runtime_switch(locale);
}

// 【规则②】Flag 缺失时的延迟补救：用户配置已可读后，设置目标并主动切换一次。
// 由语言模块在主界面之后调用（那时语言表与配置都可读）。
// desiredFourcc==0 表示用户配置也不存在 → 什么都不做（规则③）。
//
// 实现要点：这不是「改写启动期参数」，而是【运行期主动切换】——
// 复用 lzt_ios_schedule_lang_apply 已验证的换表链（重建表定义 → cdn apply
// → 回填已显示控件 → 置脏重建）。因此它天然是安全的：那一刻游戏已完全起跑。
extern "C" void lzt_ios_apply_user_config_later(uint32_t desiredFourcc) {
    if (desiredFourcc == 0) {
        lzt_core::log_write("LangOverride: no flag and no user config, "
                            "leave game default untouched (rule 3)");
        return;
    }
    // 健全性校验（同 hook 内规则）
    for (int i = 0; i < 4; ++i) {
        const uint32_t c = (desiredFourcc >> (24 - i * 8)) & 0xFF;
        if (c < 'A' || c > 'Z') {
            lzt_core::log_write("LangOverride: bad user fourcc 0x%08X, ignore",
                                desiredFourcc);
            return;
        }
    }
    // 当前游戏标识已是目标值 → 无需切换
    const uint32_t now = lzt_ios_current_langmgr_fourcc();
    if (now == desiredFourcc) {
        lzt_core::log_write("LangOverride: user config 0x%08X already active",
                            desiredFourcc);
        return;
    }
    lzt_core::log_write("LangOverride: no flag, apply user config 0x%08X "
                        "(current 0x%08X) by reusing reload chain",
                        desiredFourcc, now);
    // 交给语言模块走标准切换流程（写 FourCC → 重载表 → 回填 → 重建）。
    // 这里只做「请求」，实际切换在语言模块内完成，避免两处逻辑分叉。
    lzt_ios_request_language_switch(desiredFourcc);
}


// ============================================================
// iOS 根标题刷新
//
// 定位依据（全部由反汇编 + 运行时日志双重证实）：
//   · 根页由 0x756AE4（设置页构造函数）创建，x0 即 this；
//     钩子 hkSettingsInit 在入口记录到 g_rootSettingsPage。
//   · 根标题对象由 0x75BCB4 构造（216 字节），挂在根页 +216 起的
//     vector 上：begin=+216 / end=+224 / cap=+232
//     （0x756B58 `str xzr,[x25,#216]!` 前变址、0x756DE0 `stp x25,x20,[x19,#216]`）。
//   · 标题文本在对象 +184（24 字节 libc++ wstring；
//     析构 0x75BD24 `ldrsb w8,[x19,#207]` / `ldr x0,[x19,#184]` 佐证）。
//   · 对象 vtable = 0x10224A330（0x75BCD0 `adrp x8,0x10224a000; add #816`）。
//
// 写入方式：与行标题刷新（lzt_ios_refresh_rows）完全一致 ——
//   用 row_current_text 走游戏本地化口取【当前语言文本】，
//   再用 wstring 构造器 oWstringCtor 写入 +184。
//   绝不对已存在对象调用 0x75B128（那是行对象的构造函数，会就地重建）。
//
// 时机：与安卓 settings_framework.cpp:923 一致 ——
//   语言切换流程里，refresh_rows 之后紧接着调用（见 lzt_ios_schedule_lang_apply）。
// ============================================================
extern "C" bool lzt_ios_refresh_root_title(uintptr_t contentPage) {
    // 根页来源：hkSettingsInit 捕获的 g_rootSettingsPage（设置页构造函数 x0）。
    // 【不用 contentPage 反推】—— 传入的是「当前 Tab 的内容页」，它的 +216 是
    // owner 指针，与根页 +216（vector begin）语义冲突，判断会误命中
    // （v31 就是这样追到子页标题对象上的）。
    (void)contentPage;
    const uintptr_t rootPage = g_rootSettingsPage.load(std::memory_order_acquire);
    if (rootPage == 0) {
        lzt_core::log_write("[TITLE] skip: no captured root page yet");
        return false;
    }

    // vector 布局（反汇编 0x756B58 / 0x756DE0 实证）：
    //   0x756B58: str xzr,[x25,#216]!   ← 前变址，x25 此后 = page+216
    //   0x756B5C: stp xzr,xzr,[x0,#224] ← 清零 +224/+232
    //   0x756DE0: stp x25,x20,[x19,#216] ← 写入 begin/end
    // 因此三指针为：begin=+216, end=+224, cap=+232
    // 【v37 修正】此前把 +224 当 begin（实为 end），读到的 obj 全是垃圾
    // （日志实证 0x100000206443f09 / 0x0 / 0x46e0feed2870）。
    // 存活校验：根页若已被销毁，+216..+232 会读到释放后的内存
    // （v37 崩溃就是这么来的）。先确认这三个指针落在可读页内。
    if (!lzt_core::memory_range_accessible(rootPage + 216, 24, false)) {
        lzt_core::log_write("[TITLE] skip: root page %p not readable (destroyed?)",
                            (void*)rootPage);
        return false;
    }
    const uintptr_t vBegin = *reinterpret_cast<volatile uintptr_t*>(rootPage + 216);
    const uintptr_t vEnd   = *reinterpret_cast<volatile uintptr_t*>(rootPage + 224);
    if (vBegin == 0 || vEnd <= vBegin || (vEnd - vBegin) > 8 * sizeof(uintptr_t)) {
        lzt_core::log_write("[TITLE] bail: bad vector %p..%p", (void*)vBegin, (void*)vEnd);
        return false;
    }
    if (!lzt_core::memory_range_accessible(vBegin, 8, false)) {
        lzt_core::log_write("[TITLE] bail: vector storage %p unreadable", (void*)vBegin);
        return false;
    }
    const uintptr_t obj    = *reinterpret_cast<volatile uintptr_t*>(vBegin);
    if (obj == 0 || !lzt_core::memory_range_accessible(obj + 184, 24, true)) {
        lzt_core::log_write("[TITLE] bail: obj unreadable %p", (void*)obj);
        return false;
    }
    // 类型校验：标题对象的 vtable 由 0x75BCB4 安装：
    //   adrp x8, 0x10224a000 ; add x8,x8,#816 ; str x8,[x0]
    // 注意 0x10224A330 是【vm 地址】，运行时必须加 slide，不能直接比常量
    // （v37 就错在这里：拿 vm 值比运行时值，必然 mismatch，功能被自己挡住）。
    {
        const uintptr_t vt = *reinterpret_cast<volatile uintptr_t*>(obj);
        const uintptr_t expect = lzt_core::base() + (0x10224a330ULL - 0x100000000ULL);
        if (vt != expect) {
            lzt_core::log_write("[TITLE] bail: vtbl mismatch obj=%p vt=%p expect=%p",
                                (void*)obj, (void*)vt, (void*)expect);
            return false;
        }
    }

    // 2) 取当前语言的显示文本（与行刷新完全同一条路径：
    //    row_current_text → game_localize_into(0x179C14) → 当前表）
    wchar_t text[160] = {0};
    if (!row_current_text(L"SETTINGS_TITLE", text, 160) || text[0] == 0) {
        lzt_core::log_write("[TITLE] skip: key missing (obj=%p)", (void*)obj);
        return false;
    }

    wchar_t before[96];
    before[0] = 0;
    read_ios_wstr(reinterpret_cast<const uint8_t*>(obj + 184), before, 96);

    // 3) 用 wstring 构造器写【文本】—— 绝不能把键交给 0x75B128：
    //    那是行对象的构造函数（含 vtable 安装后的完整初始化），对已存在对象
    //    调用等于就地重建，会把正在构建/正在显示的页面弄坏（v35 实测 SIGSEGV
    //    addr=0x71，且字段留下被截断的脏串 "NGS_TITLE]"）。
    //    行标题刷新（lzt_ios_refresh_rows）一直用 oWstringCtor(字段, 文本)，
    //    稳定运行至今，此处与它对齐。
    if (oWstringCtor == nullptr) return false;
    oWstringCtor(reinterpret_cast<void*>(obj + 184), text);

    wchar_t after[96];
    after[0] = 0;
    read_ios_wstr(reinterpret_cast<const uint8_t*>(obj + 184), after, 96);
    // 日志用宽串转 UTF-8 后再打印：本机 vsnprintf 对 %ls 支持不完整
    // （实测只输出到第一个词就被截断），改用现成的 wstr_to_utf8。
    char beforeU8[256] = {0}, afterU8[256] = {0};
    wstr_to_utf8(before, beforeU8, sizeof(beforeU8));
    wstr_to_utf8(after, afterU8, sizeof(afterU8));
    lzt_core::log_write("[TITLE] root title: obj=%p \"%s\" -> \"%s\"",
                        (void*)obj, beforeU8, afterU8);
    return true;
}


// ---- 钩子 3：prompt 文本创建（0x179C14）——替换说明文案的键 ----
// 注意：该函数使用非标准调用约定 —— 文案键在 x0，输出对象在【x8】（见其序言
// `mov x19, x8` 与调用点 `add x8, sp, #104; bl ...`）。C++ 函数签名无法表达
// x8 参数，故用 naked 汇编 shim 透传 x8、只替换 x0（早期版本按 x1 声明，
// 导致输出对象拿到垃圾指针并立即崩溃）。
extern "C" void* g_promptOrigFn = nullptr;
extern "C" const wchar_t* lzt_prompt_key_filter(const wchar_t* key);
static std::atomic<int> g_promptN{0};

extern "C" __attribute__((naked)) void hkPromptMake() {
    // 尾调用原函数（本模块不再在下游拦截文本，只做键的模块页重映射）。
    // 已显示文本的刷新走「行捕获 + 用游戏表重新解析写回」(见 refresh_rows)，
    // 与安卓 localizer 一致；此前在下游登记 x8 输出对象的做法经实证无效
    // （x8 是调用方的栈上临时对象，非控件持久字段），已移除。
    __asm__ volatile(
        "stp x29, x30, [sp, #-32]!\n"
        "str x8, [sp, #16]\n"          // 保存输出对象（非标准 ABI，走 x8）
        "bl  _lzt_prompt_key_filter\n" // x0 = key → 返回替换后的键
        "ldr x8, [sp, #16]\n"          // 恢复输出对象
        "ldp x29, x30, [sp], #32\n"
        "adrp x9, _g_promptOrigFn@PAGE\n"
        "add  x9, x9, _g_promptOrigFn@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "br   x9\n"                    // 尾调用原函数（其 ret 直接回到调用方）
    );
}

extern "C" const wchar_t* lzt_prompt_key_filter(const wchar_t* key) {
    // ---- ① 模块页的 prompt 重映射（仅构建模块页面时生效）----
    if (g_remap.active && key) {
        const uintptr_t base = lzt_core::base();
        const uintptr_t k = reinterpret_cast<uintptr_t>(key);
        if (k == base + STR_SETTINGS_DATASHARING_PROMPT ||
            k == base + STR_SETTINGS_DATASHARING_PROMPT_DISABLED) {
            const auto* plan = next_plan(lzt_settings::widgets::WidgetPlanItem::PROMPT,
                                         &g_remap.promptIndex);
            if (plan && plan->key) {
                g_promptN.fetch_add(1);
                // 返回指针需在本函数返回后仍有效，故用静态缓冲；
                // UI 线程串行，同一时刻只有一次构建在途。
                static wchar_t s_promptBuf[256];
                if (remap_text(plan->key, s_promptBuf, 256)) return s_promptBuf;
                return plan->key;
            }
            // 本模块没有声明 prompt（例如语言页只有一列选项）：
            // 必须返回【空串】把原版文案顶掉，否则会显示原版 DataSharing 的说明文字。
            static const wchar_t kEmpty[] = L"";
            g_promptN.fetch_add(1);
            return kEmpty;
        }
    }

    // ---- ② 全游戏文本替换（本函数即游戏的通用本地化查表入口）----
    // 依据：该函数（0x179C14）在全二进制中有 336 个调用点，是本地化文本的统一下发点。
    // 语义：x0 = 文本键（形如 "[KEY]"），x8 = 输出对象。
    //
    // 做法：凡是以 '[' 开头的键，先查本模块按当前语言加载的 LawnStrings 表；
    // 命中就用我们的文本，未命中放行（游戏用自己的表，行为不变）。
    //
    // 由此实现「整个游戏 UI 跟随语言切换」：语言切换后我们换掉自己的表，
    // 之后所有新建界面（菜单、HUD、对话框……）都会取到新语言的文本。
    // 注意：已经建好的界面不会自动重绘——那需要各自的刷新机制，属后续工作。
    if (kEnableModuleTextOverride && key != nullptr && key[0] == L'[') {
        static wchar_t s_commonBuf[512];
        const int seq = g_l10nSeen.fetch_add(1);
        if (remap_text(key, s_commonBuf, 512)) {
            const int n = g_l10nHits.fetch_add(1);
            if (n < 6) {
                lzt_core::log_write("l10n hit #%d: %ls -> %.24ls", n, key, s_commonBuf);
            }
            return s_commonBuf;
        }
        g_l10nMiss.fetch_add(1);
        if (seq < 8) {
            lzt_core::log_write("l10n miss #%d: %.40ls", seq, key);
        }
    }
    return key;
}

// ============================================================
// 全游戏本地化入口 hook 的安装（启动时调用，必须早于游戏首次取文本）
//
// 为何要早装：0x179C14 是游戏通用的本地化查表入口（336 个调用点），
// 游戏在启动阶段就会用它解析界面文本。若等到打开设置页才装，
// 之前解析过的文本就覆盖不到，全游戏替换会「看起来没生效」。
// ============================================================
void install_l10n_hook() {
    if (g_promptOrigFn != nullptr) return;   // 幂等
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    void* t = nullptr;
    A64HookFunction(reinterpret_cast<void*>(base + OFF_SettingsPromptMake),
                    reinterpret_cast<void*>(hkPromptMake), &t);
    g_promptOrigFn = t;   // naked shim 经全局指针尾调用原函数
    lzt_core::log_write("l10n hook installed @+0x%lx orig=%p",
                        (unsigned long)OFF_SettingsPromptMake, t);
}

} // namespace
extern "C" bool lzt_ios_materialize_page(uintptr_t page, const wchar_t* titleKey,
                                          const lzt_settings::widgets::WidgetPlanItem* items,
                                          size_t count) {
    if (page == 0) return false;

    // 一次性安装重映射钩子（幂等）
    // ⚠️ 这里的每个钩子都必须【真幂等】：启动阶段已经装过行挂载钩子，
    // 若此处对同一函数再打一次补丁，A64HookFunction 会二次改写已改写过的入口，
    // 造成跳转错乱/自递归 —— 实测表现为「一打开本模块页（语言/视角 Tab）就崩」，
    // 而原版 Tab 不走物化因此正常。
    static bool hooksReady = false;
    if (!hooksReady) {
        uintptr_t base = lzt_core::base();
        void* t1 = nullptr; void* t2 = nullptr;
        // wstring 构造器 hook 已在启动阶段安装（Tab 行创建时就要用）；
        // 这里只兜底：若当时未装成功，此刻补装一次。
        if (oWstringCtor == nullptr) {
            A64HookFunction(reinterpret_cast<void*>(base + OFF_SettingsWstringCtor),
                            reinterpret_cast<void*>(hkWstringCtor), &t1);
            oWstringCtor = reinterpret_cast<void (*)(void*, const wchar_t*)>(t1);
        } else {
            t1 = reinterpret_cast<void*>(oWstringCtor);
        }
        // 窄串构造器（std::string(const char*)）：行对象里的键字段是窄串，
        // 回填显示文本（UTF-8）时用它，避免手写串布局。
        oStrCtor = reinterpret_cast<void (*)(void*, const char*)>(
            base + OFF_SettingsStdStringCtor);
        A64HookFunction(reinterpret_cast<void*>(base + OFF_SettingsCheckboxCreate),
                        reinterpret_cast<void*>(hkIosCheckboxCreate), &t2);
        oIosCheckboxCreate =
            reinterpret_cast<uintptr_t (*)(uintptr_t, uint32_t, uintptr_t, uint32_t, uintptr_t)>(t2);
        // 行挂载钩子：启动阶段已由 hkInstallRowAttachHook() 装好；此处仅在未装成功时补装，
        // 绝不重复打补丁（重复打补丁会导致模块页崩溃，详见上方注释）。
        if (oRowAttachFn == nullptr) {
            void* t4 = nullptr;
            A64HookFunction(reinterpret_cast<void*>(base + OFF_SettingsRowAttach),
                            reinterpret_cast<void*>(hkRowAttach), &t4);
            oRowAttachFn = reinterpret_cast<void (*)(uintptr_t, uintptr_t, uint32_t, float)>(t4);
        }
        // 本地化入口（0x179C14）在启动时已由 install_l10n_hook 装好。
        hooksReady = (t1 != nullptr && t2 != nullptr && oRowAttachFn != nullptr);
        lzt_core::log_write("ios-materialize: hooks ctor=%p cb=%p attach=%p l10n=%p",
                            t1, t2, (void*)oRowAttachFn, (void*)g_promptOrigFn);
    }

    g_remap.active = true;
    g_remap.titleKey = titleKey;
    g_remap.items = items;
    g_remap.count = count;
    g_remap.checkboxIndex = 0;
    g_remap.promptIndex = 0;
    g_remap.page = page;
    g_attachCount.store(0, std::memory_order_release);   // 本轮挂载计数复位
    g_cbCreates.store(0, std::memory_order_release);     // 本轮原版 checkbox 创建计数复位

    lzt_core::log_write("ios-materialize: begin page=%p title=%ls items=%zu",
                        (void*)page, titleKey ? titleKey : L"(null)", count);

    // 交给游戏原版页面构建函数（被 hook 的那个），由它把整页搭出来。
    // 追加控件由「行挂载钩子」在原版最后一行挂载完成时自动完成 ——
    // 必须在那时（而不是这里返回之后）追加，才能进入原版的布局与滚动范围计算。
    const uintptr_t r = lzt_settings::call_original_page(page);

    // ⚠️ 必须复位：本标志若保持 true，会让「之后所有原版页面构建」也被本模块的
    // 替换逻辑命中（标题/控件/prompt 被改写），表现为原版 DataSharing 页 prompt
    // 消失、控件文本错乱、多余行等（实测）。
    g_remap.active = false;

    lzt_core::log_write("ios-materialize: end page=%p ret=0x%lx ctor=%d cb=%d "
                        "textHit=%d textMiss=%d",
                        (void*)page, (unsigned long)r, g_ctorN.load(), g_cbN.load(),
                        g_remapHits.load(), g_remapMiss.load());
    return true;
}

// ============================================================
// iOS 页面级 layout（对应 Android 的 run_page_layout）
//
// 原版 dispatch 的每个 case 在页面构建之后都会做这一步：遍历内容容器里的
// 控件，逐个调用 (widget, 1) 的显示/布局方法。
// 反汇编依据（case 12 尾部 0x75A0E4）：
//   container = *(page+240)
//   for (it = container+200; it != container+208; it += 8)
//       w = *it;  vt = *w;
//       ((void(*)(w,int))vt[136])(w, 1);
//       ((void(*)(w,int))vt[400])(w, 1);
// 缺这一步的表现正是「页面 mount 了但没有任何刷新/不显示」。
// ============================================================
extern "C" void lzt_ios_page_layout(uintptr_t page) {
    if (page == 0) return;
    // 原版 dispatch 的 case 尾部不是「遍历控件」而是 tail-call 一个独立函数
    // （0x75A0E4）。它内部才做真正的显示收尾：
    //   if (*(page+288) != 0) → 走 dirty 重建分支
    //   else → 遍历内容容器（page+240 → +200..+208）逐个控件调用显示方法
    // 因此这里直接调用它，而不是自行遍历（自行遍历缺少前置状态处理，
    // 会导致页面不切换、且页面状态错乱使其它 Tab 也失灵）。
    using PageShowFn = void (*)(uintptr_t);
    auto show = reinterpret_cast<PageShowFn>(lzt_core::base() + OFF_SettingsPageShow);
    if (show == nullptr) return;
    show(page);
    lzt_core::log_write("ios-page-layout: page=%p -> show() called", (void*)page);
}

// ============================================================
// 入口
// ============================================================
// 与 Android 一致：起独立线程等主程序就绪，再装 hook。
// iOS 侧主程序必然已加载（本 dylib 由它加载），因此只需等 __TEXT 可解析。
namespace {

// 注：曾用「表加载函数 0x2CBFB4」的探针来判断能否单独重载 LawnStrings 表，
//     实测该函数在正常启动流程中【从未被调用】（143 个调用点所属的大初始化
//     函数不参与本次启动），因此放弃「重载游戏表」路线，改为本模块自行解析
//     CDN 里的 LawnStrings 表并直接替换控件文本（见 remap_text）。


// ============================================================
// CDN 应用链：取证与实际重触发（用于让整个游戏 UI 跟随语言切换）
//
// 目标：切换语言后让游戏重新加载 LawnStrings 表，从而使【所有】界面文本跟切
//       （而不只是我们模块自己构建的设置页）。
//
// 依据：0x100735930 是游戏的 CDN 文件应用循环（内含 "Applying file %s" 字符串，
//       全二进制仅 1 个调用点 0x100040BC8，参数为 CDN 管理器对象）。
//       这与 Android 热重载走的 CDN apply 链同源。
//
// 做法分两步（避免盲目调用游戏状态机）：
//   ① 启动阶段只读记录该管理器对象（游戏自身会调用它完成 CDN 处理）；
//   ② 语言切换时，若已记录到对象，则受控调用一次并打印结果。
// 安全性：调用点有重入门保护（同一时刻只允许一次），失败仅记日志、不影响游戏。
// 如遇异常，把 kEnableCdnReapply 改为 false 即可关闭（不影响其它功能）。
// ============================================================
static constexpr bool kEnableCdnReapply = true;

// 探针钩子总开关（默认关闭）。
// 教训：探针里存在两类致命写法，实测导致真机点图标启动「显示发行商图标后 SIGBUS 崩溃」：
//   · 0x2c1298 处在大表加载函数 0x2c0bf0 的【中段】（违反「hook 必须挂函数入口」）；
//   · 0x6525c 被 rsb name 与 path-D 各挂一次（重复 hook 同一函数会破坏 trampoline）。
// 崩溃现场回传：pc=+0x1c81894，返回地址 +0x2c0cf0（正是大表加载函数调用 0x2cbfb4 之后）。
// 需要取证时再置 true，且一次只开一个。
static constexpr bool kEnableProbeHooks = false;

// ---- 语言表重载（按原版方法，对应安卓 hot_reload 的 LoadPackage / CDN apply）----
//
// 0x2c0bf0：游戏自己的「读 locale → 拼 LawnStrings-<locale> → 加载该表」函数
//   · 栈帧 ~10432B（反汇编实证：sub sp,#8192 与 #2240 两段）
//   · 唯一调用者 0x1c5c80（位于 CDN 初始化链内），启动时触发一次
//   · 参数 x0 = 加载上下文对象（序言即 mov x20,x0; ldr x23,[x20,#48]!）
// 安卓的等价动作是：改语言标识 → LoadPackage/CDN apply 重载该表 → apply 生效。
// 因此 iOS 端不另写解析，改标识后用游戏这个入口再跑一遍。
static constexpr bool kEnableLangTableReload = true;

// ---- 0x2ca220：CDN「单文件 Apply」探针（只读）----
//
// 安卓对应 sub_D58544(ctx, path, name, tableId) —— 它是「把某个 CDN 文件按
// tableId 应用进表」的唯一入口（LawnStrings 的 tableId=131），内部再调
// sub_120AEFC 做 RTON 反序列化。iOS 侧签名尚未确认，因此这里在启动期 CDN
// 应用时记录它的真实参数（含指针处字符串），据此实现「按语言重载语言表」。
// 只读、不改行为；失败仅记日志。
static constexpr bool kEnableApplyProbe = false;   // 诊断用（记录 0x2ca220 参数），默认关闭
static uintptr_t (*g_origApplyProbe)(uintptr_t, uintptr_t, uintptr_t) = nullptr;

static void log_cstr_at(const char* tag, uintptr_t p) {
    if (p < 0x1000 || !lzt_core::memory_range_accessible(p, 1, false)) {
        lzt_core::log_write("    %s=%p (unreadable)", tag, (void*)p);
        return;
    }
    char buf[192] = {0};
    size_t n = 0;
    for (; n < 191; ++n) {
        if (!lzt_core::memory_range_accessible(p + n, 1, false)) break;
        const unsigned char ch = *reinterpret_cast<const unsigned char*>(p + n);
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7E) { n = 0; break; }
        buf[n] = static_cast<char>(ch);
    }
    if (n > 0) {
        lzt_core::log_write("    %s=%p [%s]", tag, (void*)p, buf);
    } else {
        lzt_core::log_write("    %s=%p (non-ascii)", tag, (void*)p);
    }
}

static uintptr_t hkApplyProbe(uintptr_t a0, uintptr_t a1, uintptr_t a2) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) < 16) {
        lzt_core::log_write("apply-probe: a0=%p a1=%p a2=%p",
                            (void*)a0, (void*)a1, (void*)a2);
        log_cstr_at("a0", a0);
        log_cstr_at("a1", a1);
        log_cstr_at("a2", a2);
    }
    return g_origApplyProbe ? g_origApplyProbe(a0, a1, a2) : 0;
}
static uintptr_t (*g_origBigTableLoad)(uintptr_t) = nullptr;
static std::atomic<uintptr_t> g_bigTableArg{0};
static std::atomic<int> g_bigTableCalls{0};
static std::atomic<bool> g_langTableReloading{false};

static uintptr_t hkBigTableLoad(uintptr_t a0) {
    g_bigTableArg.store(a0, std::memory_order_release);
    const int n = g_bigTableCalls.fetch_add(1);
    if (n < 6) lzt_core::log_write("langtable: load call #%d a0=%p", n + 1, (void*)a0);
    return g_origBigTableLoad ? g_origBigTableLoad(a0) : 0;
}

// 前置声明：CDN 应用循环重放（实现见本文件后部）
bool lzt_ios_reapply_cdn();

// 供语言模块在「改标识之后」调用：按原版方式让游戏重新加载语言表。
//
// 做法（对齐安卓 try_external_apply → OFF_CDN_APPLY）：调用游戏自己的 CDN
// 应用循环 0x735930（启动期游戏用它逐条应用 CDN 文件，含 LawnStrings 表）。
// 为什么不用 0x2c0bf0（大表加载函数）：实测重放它会重入 CDN 初始化链并崩溃。
extern "C" bool lzt_ios_reload_lang_table(void) {
    if (!kEnableLangTableReload) return false;
    if (g_origBigTableLoad == nullptr) {
        lzt_core::log_write("langtable reload: hook 未就绪");
        return false;
    }
    const uintptr_t a0 = g_bigTableArg.load(std::memory_order_acquire);
    if (a0 == 0) {
        lzt_core::log_write("langtable reload: 未捕获启动期上下文");
        return false;
    }
    bool expected = false;
    if (!g_langTableReloading.compare_exchange_strong(expected, true)) {
        lzt_core::log_write("langtable reload: 已有一次在进行，跳过");
        return false;
    }
    lzt_core::log_write("langtable reload: scheduled a0=%p", (void*)a0);
    dispatch_async(dispatch_get_main_queue(), ^{
        probe_localize("before-reload");
        const uintptr_t r = g_origBigTableLoad(a0);
        lzt_core::log_write("langtable reload: end ret=0x%lx", (unsigned long)r);
        probe_localize("after-reload");
        g_langTableReloading.store(false, std::memory_order_release);
    });
    return true;
}

// ============================================================
// 切换语言的收尾链（顺序在此统一保证，与安卓 hot_reload 同序）：
//   ① 换表：用游戏自己的加载链把语言表换成目标语言
//   ② 回填：用游戏自己的本地化口重刷【已加载】控件的文本
//   ③ 置脏：让页面在安全点重建（新建控件自然取到新语言）
//
// 为什么必须有顺序：回填若发生在换表之前，取到的仍是旧语言文本，
// 表现为「已显示的文本不跟切、只有新解析的文本是新语言」（实测）。
// 为什么下沉到主队列：本链由语言切换的点击回调触发，此时仍在 dispatch 调用栈内
// （控件正在被使用），在栈内换表/改写控件会释放正在使用的对象（历史实测崩溃）。
// ============================================================
// 取证：把已捕获行的「当前字段文本」打印出来（UTF-8，避免 %ls 截断）。
// 用途：回填后延迟复查，判断我们写入的文本是否保留、或被游戏自身重新解析覆盖。
extern "C" void lzt_ios_log_row_texts(const char* tag) {
    const int n = g_rowTextCount.load(std::memory_order_acquire);
    int shown = 0;
    for (int i = 0; i < n && shown < 6; ++i) {
        const RowTextRec& r = g_rowTexts[i];
        if (!row_alive(r)) continue;
        wchar_t cur[128] = {0};
        if (!read_ios_wstr(reinterpret_cast<const uint8_t*>(r.host + r.offset), cur, 128)) continue;
        char keyU8[256] = {0};
        wstr_to_utf8(r.key, keyU8, sizeof(keyU8));
        log_wstr(tag, keyU8, cur);
        ++shown;
    }
}

// 调用「CDN 按模式收集 + 应用」入口 0x1c4a8c：x0 = 模式串对象，x8 = 输出缓冲。
// 实测其启动期调用点传入的模式串是 "{LOCALE}"（游戏用它挑出与语言相关的 CDN 文件，
// 例如 LawnStrings-<locale>.rton），这正是「按语言标识读 CDN 语言表」的源头。
// 用普通函数 + 内联汇编搬寄存器（避免 naked shim 被编译器插入调用破坏参数）。
extern "C" __attribute__((noinline)) void lzt_call_cdnload(void* fn, uintptr_t a0,
                                                          void* out8) {
    register uintptr_t p0 asm("x9") = a0;
    register void* p8 asm("x10") = out8;
    register void* pf asm("x11") = fn;
    __asm__ volatile(
        "mov x0, %0\n"
        "mov x8, %1\n"
        "br  %2\n"
        :: "r"(p0), "r"(p8), "r"(pf));
}

// 在 CDN 目录里找到目标语言表文件的【绝对路径】。
// 依据：启动期游戏自己就是拿绝对 CDN 路径去 apply 的（实测日志：
//   cdn-apply(LANG) table=131 name="/.../Documents/No_Backup/CDN.2.0/LawnStrings-zh-cn.rton"），
// 因此运行期重放必须同样给绝对路径，游戏才会去读外置文件。
static bool lzt_find_lang_file(const char* locale, char* out, size_t cap) {
    if (locale == nullptr || out == nullptr || cap == 0) return false;
    char base[512] = {0};
    if (!lzt_core::cdn_dir(base, sizeof(base))) return false;
    DIR* nb = opendir(base);
    if (nb == nullptr) return false;
    char fname[80];
    snprintf(fname, sizeof(fname), "LawnStrings-%s.rton", locale);
    bool found = false;
    struct dirent* e;
    while ((e = readdir(nb)) != nullptr) {
        if (strncmp(e->d_name, "CDN.", 4) != 0) continue;
        char p[768];
        snprintf(p, sizeof(p), "%s/%s/%s", base, e->d_name, fname);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            snprintf(out, cap, "%s", p);
            found = true;
            break;
        }
    }
    closedir(nb);
    return found;
}

// ---- iOS 版「行初始化器」= 0x75bcb4 → 内部 0x75b128（安卓 sub_A54230 的等价物）----
//
// 反汇编实证（0x75bcb4）：
//   · 入参 x0=行对象、x1=标题键（std::string）
//   · 先置该行的 vtable，再把 +184/+200（标题 wstring 两处）清零
//   · 再调 0x75b128(row, key)：内部 bl 0x179b70 取本地化文本，
//     然后【释放旧串 + 把新串写入 row+184】（q0/x8 三字段 = libc++ wstring 布局）
// 因此 0x75b128 是「行 + 标题键 + 写入点」三者齐全的锚点：
// 挂钩它即可拿到全部设置行（Tab 行、滑条行、checkbox 行）的 (row, 键)，
// 无需任何内存形态推断 —— 与安卓一致，也就不会再出现「错绑键」的破坏性后果。
extern "C" void* g_origRowInit = nullptr;   // 须 C 链接（供 naked 桥引用）
extern "C" void lzt_row_init_capture(uintptr_t row, uintptr_t keyObj);

extern "C" __attribute__((naked)) void hkRowInit() {
    // x0 = row，x1 = 键对象；先转发原函数，再登记。
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "adrp x9, _g_origRowInit@PAGE\n"
        "add  x9, x9, _g_origRowInit@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 1f\n"
        "blr  x9\n"                    // 原函数完成「键 → 标题」写入
        "1:\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "bl   _lzt_row_init_capture\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

// 登记行的标题（键来自行初始化器，安全且确定）
// ⚠️ 0x75b128 的 x1 是【窄串 std::string】（其内部 0x179b70 按 +23 取长度、
// 首字符判 '['），不是宽串 —— 早期按宽串读导致键读不到（实测）。
// 读行初始化器传入的「标题键」：**UTF-16LE 宽串**（libc++ long string 布局）。
// 实证（内存 dump）：ptr@+0、len@+8（=字符数）、cap@+16（最高位=1 长串）；
// 数据形如 5B 00 53 00 45 00 … 即 "[SETTINGS_...]"。
// 早期按窄串读取会在首个 0x00 处截断（只得到 "["），导致键认不出（实测）。
static bool read_u16_key(uintptr_t obj, wchar_t* out, size_t cap) {
    if (out == nullptr || cap == 0) return false;
    if (!lzt_core::memory_range_accessible(obj, 24, false)) return false;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(obj);
    const uintptr_t dp = *reinterpret_cast<const uint64_t*>(b);
    const uint64_t len = *reinterpret_cast<const uint64_t*>(b + 8);
    if (len == 0 || len >= cap) return false;
    const wchar_t* src = nullptr;
    uint16_t ssoBuf[12] = {0};
    const uint8_t tail = b[23];
    if ((tail & 0x80) != 0) {
        if (dp < 0x1000) return false;
        if (!lzt_core::memory_range_accessible(dp, (size_t)len * 2, false)) return false;
        src = reinterpret_cast<const wchar_t*>(dp);
    } else {
        // 内联短串：字符数据自 +0（每字符 2 字节）
        for (size_t i = 0; i < len && i < 12; ++i) {
            ssoBuf[i] = static_cast<uint16_t>(b[i * 2] | (b[i * 2 + 1] << 8));
        }
        src = reinterpret_cast<const wchar_t*>(ssoBuf);
    }
    for (size_t i = 0; i < len; ++i) {
        out[i] = static_cast<wchar_t>(static_cast<uint16_t>(src[i]));
    }
    out[len] = 0;
    return true;
}

extern "C" void lzt_row_init_capture(uintptr_t row, uintptr_t keyObj) {
    if (row == 0 || keyObj == 0) return;
    wchar_t wkey[128] = {0};
    if (!read_u16_key(keyObj, wkey, 128)) return;
    if (wkey[0] != L'[') return;
    lzt_ios_capture_row_by_key(row, wkey);
}

// ---- iOS 版 ApplyLocalizedStrings（安卓 sub_13D0F30 的同构函数）----
//
//   · 无参数：序言直接 bl 0x21fd8 自取 PVZDB 单例（与安卓一致）
//   · 取表 131：mov w1,#131 → bl 0x760bb4（sret 走 x8，同安卓 0x13D12E0 形态）
//   · 空表判据：ldp [表+8]/[表+0x10] 求差，按 entry 步长 48 求条数，cmp #1; b.lt 返回
//     （iOS 的 entry 是 48 字节，安卓是 24；判据形态一致）
//   · 容器 owner：adrp 0x1027f4000 + add #3792 → *(0x27f4ed0)，与查询口同一处 ★
//   · 写容器：bl 0x1abbb60(owner, &key, &value)（安卓为 sub_15407DC）
//   · 清表：结尾 bl 0x21fd8 → mov w1,#131 → bl 0x2ca3e0（安卓为 sub_120C170）
//
// ★ 这一步是「装载后让容器生效」的缺失环节：只做 LoadPackage 时数据停在 table 131，
//   容器（查询口读的 manager+1984）不会更新，表现就是「装载成功但查询仍旧语言」。
// ---- 滑条行标题写入点 0x757a90（Music / Sound FX 等）----
//
// 反汇编实证：0x757ff8（滑条行创建）内 `bl 0x757a90`，参数 (x0 = 行的标题字段地址,
// x1 = 标题键)；其函数体即「libc++ 字符串构造」（先取长度，再按 SSO/长串布局写字段）。
// 因此挂钩它即可拿到「标题字段地址」—— 该地址即行对象的 +184，
// 行对象 = 字段地址 - 184，于是得到确定的 (行, 键)，无需任何推断。
// 安全性：0x757a90 专用于「按 C 字符串构造 std::string」，调用者都是构造文本的场景，
// 与 vtable 写入路径（0x75be74 会把 vtable 写在 +184）完全不同，因此不会误伤对象。
extern "C" void* g_origStrCtorAt = nullptr;
extern "C" void lzt_strctor_capture(uintptr_t fieldAddr, uintptr_t key);

extern "C" __attribute__((naked)) void hkStrCtorAt() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "adrp x9, _g_origStrCtorAt@PAGE\n"
        "add  x9, x9, _g_origStrCtorAt@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 1f\n"
        "blr  x9\n"
        "1:\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "bl   _lzt_strctor_capture\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

extern "C" void lzt_strctor_capture(uintptr_t fieldAddr, uintptr_t key) {
    if (fieldAddr < 0x1000 || key == 0) return;
    char k8[128] = {0};
    if (!lzt_core::memory_range_accessible(key, 1, false)) return;
    for (size_t i = 0; i < 127; ++i) {
        if (!lzt_core::memory_range_accessible(key + i, 1, false)) break;
        const unsigned char c = *reinterpret_cast<const unsigned char*>(key + i);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return;   // 非 ASCII → 不是文本键
        k8[i] = static_cast<char>(c);
    }
    if (k8[0] != '[') return;
    wchar_t wkey[128] = {0};
    size_t n = 0;
    for (; k8[n] != 0 && n < 127; ++n) wkey[n] = static_cast<wchar_t>(k8[n]);
    wkey[n] = 0;
    lzt_ios_capture_row_by_key(fieldAddr - 184, wkey);
}

static constexpr uintptr_t OFF_IOS_APPLY_LOCALIZED_STRINGS = 0x760a20;

static bool lzt_ios_apply_localized_strings(void) {
    if (lzt_core::base() == 0) return false;
    using ApplyLocalizedFn = uintptr_t (*)(void);
    lzt_core::log_write("apply-localized: begin");
    const uintptr_t r =
        reinterpret_cast<ApplyLocalizedFn>(lzt_core::base() + OFF_IOS_APPLY_LOCALIZED_STRINGS)();
    lzt_core::log_write("apply-localized: end ret=0x%lx", (unsigned long)r);
    return true;
}

// 重放「按 tableId 应用 CDN 文件」——用启动期同样的调用与参数。
static bool lzt_ios_reload_cdn_by_locale(void) {
    if (lzt_core::base() == 0) return false;
    const uintptr_t ctx = g_applyCtx.load(std::memory_order_acquire);
    if (ctx == 0 || g_origApply2 == nullptr || oStrCtor == nullptr) {
        lzt_core::log_write("lang file apply: 上下文/构造器未就绪（ctx=%p orig=%p ctor=%p）",
                            (void*)ctx, (void*)g_origApply2, (void*)oStrCtor);
        return false;
    }
    alignas(16) static uint8_t s_pathObj[32];
    static char s_path[768];
    // 两条通道（与安卓同序）：
    //   ① 外置优先：CDN 目录存在 LawnStrings-<locale>.rton → 用【绝对路径】装载
    //      （实测必须绝对路径，游戏才会去读外置文件）
    //   ② 数据包兜底：CDN 无该文件（如 en-us，英文表内置在数据包）→ 用游戏真实名字
    //      "packages\\LawnStrings-<locale>.rton" 走 RSB（安卓答复 §6：前缀必须有）
    bool useCdn = lzt_find_lang_file(g_targetLocale, s_path, sizeof(s_path));
    if (useCdn) {
        lzt_core::log_write("lang file apply: 外置表命中 path=%s", s_path);
    } else {
        snprintf(s_path, sizeof(s_path), "packages\\LawnStrings-%s.rton", g_targetLocale);
        lzt_core::log_write("lang file apply: CDN 无 LawnStrings-%s.rton，走数据包 name=%s",
                            g_targetLocale, s_path);
    }
    memset(s_pathObj, 0, sizeof(s_pathObj));
    oStrCtor(s_pathObj, s_path);
    const uintptr_t r = g_origApply2(ctx, 131, reinterpret_cast<uintptr_t>(s_pathObj));
    lzt_core::log_write("lang file apply: end ret=0x%lx", (unsigned long)r);
    // 按安卓流程补上「应用本地化」：把 table 131 的键值对搬进本地化容器。
    if (r != 0) lzt_ios_apply_localized_strings();
    else lzt_core::log_write("lang file apply: 装载失败，保留旧表（不进入 apply）");
    return r != 0;
}

// ---- 业务屏幕刷新的「挂起 / 执行」（方案 A）----
//
// 问题：在设置页打开期间重建主界面会新建顶层节点，盖在设置页之上（实测）。
// 做法：设置页存在时只置「待刷新」标志；等离开设置页时统一刷新一次。
// 这样主界面文本最终跟切，且不会出现「主界面浮在设置页之上」。
static std::atomic<bool> g_screenRefreshPending{false};

static void run_screen_refreshers_now() {
    lzt_settings::run_screen_refreshers();
}

// 供收尾链调用：设置页存在 → 挂起；否则立即刷新
static void lzt_ios_request_screen_refresh(void) {
    const uintptr_t sp = lzt_settings::last_settings_page();
    if (sp != 0 && lzt_core::memory_range_accessible(sp, 8, false)) {
        g_screenRefreshPending.store(true, std::memory_order_release);
        lzt_core::log_write("screen refresh: suspended (settings page open)");
        return;
    }
    run_screen_refreshers_now();
}

// 供布局层在「离开设置页」时调用：把挂起的刷新补上
static void lzt_ios_flush_pending_screen_refresh_impl(void) {
    if (!g_screenRefreshPending.exchange(false, std::memory_order_acq_rel)) return;
    lzt_core::log_write("screen refresh: flush on leaving settings");
    run_screen_refreshers_now();
}

extern "C" void lzt_ios_schedule_lang_apply(uintptr_t page) {
    if (!kEnableLangTableReload) return;
    // 【v34】根标题刷新【不能】放在这里 —— 本函数此时语言表还是旧的
    // （v33 日志实证：TITLE 写入 "Settings" 发生在 "lang apply: table reload"
    // 之前），写进去的是旧语言文本。正确时机在表加载完成、行回填之后，
    // 见本函数尾部 lzt_ios_refresh_root_title_captured()。
    const uintptr_t a0 = g_bigTableArg.load(std::memory_order_acquire);
    if (g_origBigTableLoad == nullptr || a0 == 0) {
        lzt_core::log_write("lang apply: 缺加载上下文，仅回填+重建");
        dispatch_async(dispatch_get_main_queue(), ^{
            lzt_ios_refresh_rows(page);
            if (page != 0 && lzt_core::memory_range_accessible(page + 288, 4, true)) {
                *reinterpret_cast<volatile uint32_t*>(page + 288) = 1;
            }
        });
        return;
    }
    bool expected = false;
    if (!g_langTableReloading.compare_exchange_strong(expected, true)) {
        lzt_core::log_write("lang apply: 已有一次在进行，跳过");
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        // ①′ 方案 A：让游戏按（已改写成目标语言的）清单条目去 CDN 取语言表并装表。
        //     这是安卓 cdn_load_rton / try_external_apply 的等价物：文件由游戏资源层
        //     自行决定「CDN 优先、数据包兜底」，我们不干预。
        // ① 先按新语言重建表定义（0x2c0bf0 会按当前语言标识重新拼表名），
        //    这样紧接着按 tableId 应用时才会去找 LawnStrings-<新语言>.rton。
        probe_localize("before-reload");
        const uintptr_t r = g_origBigTableLoad(a0);
        lzt_core::log_write("lang apply: table reload ret=0x%lx", (unsigned long)r);
        probe_localize("after-reload");
        // ② 按 tableId 让游戏自己去 CDN 取语言表并装表（顺序必须在 ① 之后，
        //    否则名字仍是旧语言 —— 实测 apply 用的是 packages/lawnstrings-en-us.rton）。
        if (g_targetLocale[0] != 0) {
            lzt_ios_reload_cdn_by_locale();
            probe_localize("after-table-by-id");
        }
        // ③ 兜底：重跑一次 CDN 清单应用循环
        if (g_origCdnApply != nullptr && g_cdnMgr.load() != 0) {
            const uintptr_t mgr = g_cdnMgr.load();
            lzt_core::log_write("lang apply: re-run cdn apply loop mgr=%p", (void*)mgr);
            const uintptr_t rr = g_origCdnApply(mgr);
            lzt_core::log_write("lang apply: cdn apply loop ret=0x%lx", (unsigned long)rr);
        }
        // ② 回填已加载控件（此时表已是新语言 → 取到新语言文本）
        lzt_ios_refresh_rows(page);
        lzt_ios_log_row_texts("rowtext-after-refresh");
        // ②′ 根标题：与安卓 settings_framework.cpp:923 完全同位 ——
        //     refresh_all() 之后紧跟 refresh_page_title()。
        //     必须晚于换表，否则取到的是旧语言文本。
        lzt_ios_refresh_root_title_captured();
        // 延迟复查：确认写入是否被游戏自身重新解析覆盖
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1200 * NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), ^{
            lzt_ios_log_row_texts("rowtext-later");
        });
        // ③ 置脏：由游戏在安全点重建页面
        if (page != 0 && lzt_core::memory_range_accessible(page + 288, 4, true)) {
            *reinterpret_cast<volatile uint32_t*>(page + 288) = 1;
            lzt_core::log_write("lang apply: page=%p marked dirty", (void*)page);
        }
        // ④ 业务屏幕刷新（主界面 / 横幅等）：必须在【换表之后】执行。
        lzt_ios_request_screen_refresh();
        g_langTableReloading.store(false, std::memory_order_release);
    });
}

static std::atomic<int> g_cdnApplyCalls{0};
static std::atomic<bool> g_cdnReapplying{false};

static uintptr_t hkCdnApply(uintptr_t mgr) {
    if (mgr != 0) {
        g_cdnMgr.store(mgr, std::memory_order_release);
        const int n = g_cdnApplyCalls.fetch_add(1);
        if (n < 4) {
            lzt_core::log_write("cdn apply: mgr=%p (#%d)", (void*)mgr, n + 1);
        }
    }
    return g_origCdnApply ? g_origCdnApply(mgr) : 0;
}

// 语言切换后调用：请求游戏重新应用 CDN 文件（即重载 LawnStrings 等表）。
// 返回 true 表示已发起调用（不代表游戏一定重载成功，以日志为准）。
bool lzt_ios_reapply_cdn() {
    if (!kEnableCdnReapply) return false;
    const uintptr_t mgr = g_cdnMgr.load(std::memory_order_acquire);
    if (mgr == 0 || g_origCdnApply == nullptr) {
        lzt_core::log_write("cdn reapply: 跳过（mgr=%p orig=%p）", (void*)mgr,
                            (void*)g_origCdnApply);
        return false;
    }
    bool expected = false;
    if (!g_cdnReapplying.compare_exchange_strong(expected, true)) {
        lzt_core::log_write("cdn reapply: 已有一次在进行，跳过");
        return false;
    }
    lzt_core::log_write("cdn reapply: begin mgr=%p", (void*)mgr);
    const uintptr_t r = g_origCdnApply(mgr);
    lzt_core::log_write("cdn reapply: end ret=0x%lx", (unsigned long)r);
    g_cdnReapplying.store(false, std::memory_order_release);
    return true;
}

// 是否启用文本表内存扫描（仅研究用，默认关闭：它会遍历可写内存，耗时且无功能收益）。
// 需要继续研究「运行时替换游戏文本表」时置 true 即可。
static constexpr bool kEnableTextTableScan = false;

// ---- 文本表内存定位探针（只读）----
// 目的：找到游戏把 LawnStrings 存在哪里，为「运行时替换表内容」做准备。
// 思路：取一条已知译文（"账户删除" / "Account Deletion"），在可写内存里扫描其
//       UTF-32 编码；命中处即表内条目，随后 dump 周边内存推断表结构。
// 只读扫描，不修改任何内存，因此安全。
static void lzt_ios_scan_text_table() {
    // 两个候选译文（UTF-32LE）
    static const wchar_t kZh[] = L"账户删除";
    static const wchar_t kEn[] = L"Account Deletion";
    struct { const wchar_t* s; size_t n; const char* name; } cands[] = {
        {kZh, sizeof(kZh) / sizeof(wchar_t) - 1, "zh"},
        {kEn, sizeof(kEn) / sizeof(wchar_t) - 1, "en"},
    };

    int totalHits = 0;
    // ---- 先扫 ASCII 键名：若键与译文在同一记录内，即可据此推断表结构 ----
    {
        static const char kKey[] = "ACCOUNT_DELETION";
        const size_t klen = sizeof(kKey) - 1;
        int hits = 0;
        vm_address_t addr = 0;
        while (hits < 6) {
            vm_size_t size = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            kern_return_t kr = vm_region_64(mach_task_self(), &addr, &size,
                                            VM_REGION_BASIC_INFO_64,
                                            (vm_region_info_t)&info, &cnt, &obj);
            if (kr != KERN_SUCCESS) break;
            if ((info.protection & VM_PROT_WRITE) != 0 &&
                size <= (vm_size_t)(64u * 1024u * 1024u)) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
                for (vm_size_t off = 0; off + klen <= size; ++off) {
                    if (memcmp(p + off, kKey, klen) != 0) continue;
                    lzt_core::log_write("TBLSCAN key hit #%d @ %p (region %p)",
                                        hits + 1, (void*)(addr + off), (void*)addr);
                    // 打印命中点后 96 字节（键之后通常紧跟译文或指针）
                    char line[3 * 33 + 1];
                    size_t k = 0;
                    for (int j = 0; j < 32 && off + (size_t)j < size; ++j) {
                        k += static_cast<size_t>(snprintf(
                            line + k, sizeof(line) - k, "%02x ", p[off + j]));
                    }
                    line[k] = 0;
                    lzt_core::log_write("TBLSCAN   next32: %s", line);
                    ++hits;
                    ++totalHits;
                    off += klen - 1;
                    if (hits >= 6) break;
                }
            }
            addr += size;
            if (addr == 0) break;
        }
    }
    for (auto& cand : cands) {
        const size_t bytes = cand.n * sizeof(wchar_t);
        int hits = 0;
        vm_address_t addr = 0;
        while (hits < 4) {
            vm_size_t size = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            kern_return_t kr = vm_region_64(mach_task_self(), &addr, &size,
                                            VM_REGION_BASIC_INFO_64,
                                            (vm_region_info_t)&info, &cnt, &obj);
            if (kr != KERN_SUCCESS) break;
            const bool writable = (info.protection & VM_PROT_WRITE) != 0;
            // 只扫可写区（数据表在堆里），且限制单区大小避免长时间卡顿
            if (writable && size <= (vm_size_t)(64u * 1024u * 1024u)) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
                for (vm_size_t off = 0; off + bytes <= size; off += sizeof(wchar_t)) {
                    if (memcmp(p + off, cand.s, bytes) != 0) continue;
                    lzt_core::log_write("TBLSCAN %s hit #%d @ %p (region %p+0x%llx)",
                                        cand.name, hits + 1, (void*)(addr + off),
                                        (void*)addr, (unsigned long long)size);
                    // dump 命中点前后 64 字节的原始内容（含前后相邻条目线索）
                    if (off >= 32) {
                        char line[3 * 33 + 1];
                        size_t k = 0;
                        for (int j = -32; j < 0; j += 2) {
                            k += static_cast<size_t>(snprintf(
                                line + k, sizeof(line) - k, "%02x%02x ",
                                p[off + j + 1], p[off + j]));   // 小端显示
                        }
                        line[k] = 0;
                        lzt_core::log_write("TBLSCAN   prev32: %s", line);
                    }
                    ++hits;
                    ++totalHits;
                    if (hits >= 4) break;
                }
            }
            addr += size;
            if (addr == 0) break;
        }
    }
    if (totalHits == 0) {
        lzt_core::log_write("TBLSCAN: 未命中（当前语言可能既非 zh-cn 也非 en-us，"
                            "或文本以其它编码存放）");
    }
}

// ============================================================
// CDN 清单注入（与 Android cdn_load_rton.cpp 同机制）
//
// Android 的机制：hook CDN 清单收集过程中「添加条目」的函数（sub_ACABCC），
// 在首次触发时往清单里注入自备条目，随后游戏自己的 apply 链就会加载这些文件。
//
// iOS 对应点（反汇编实证）：
//   · 清单收集：0x1000403E0 附近读取 LangMgr 的语言标识(+508)，
//     格式化出 "LawnStrings-<locale>.rton"，再以 tableId=131 追加进清单；
//   · 追加函数：0x100040910(manager, nameString, tableId, flag)。
//     内部是 32 字节/项的 vector push_back（与 Android 的条目布局同类）。
//
// 用途：语言切换时把「新语言的 LawnStrings 文件」注入清单并触发应用，
// 让游戏按新语言重载文本表 —— 这就是安卓式重载。
// ============================================================
// ---- CDN 单文件应用 0x2c9e04 探针（只读）----
// 调用来源：CDN 应用循环 0x737640 处 `bl 0x2c9e04(ctx, tableId, nameString)`
// 签名（反汇编 0x2c9e04 序言实证）：
//   x0 = 上下文 / w1 = tableId / x2 = 名字串对象（libc++ string，24B）
// 作用：记录游戏实际「把哪个 CDN 文件应用到哪个 tableId」——
//       这是判定「游戏是否读了 CDN 语言表、以及它落到哪个表」的唯一直接证据。
static constexpr bool kEnableApplyProbe2 = false;   // 诊断用（记录 CDN 单文件应用参数），默认关闭
// 注：语言表装载走 g_origApply2（0x2c9e04）—— 该 hook 本身是功能链路的一部分
// （热重载需要它提供的上下文），因此即便探针关了，钩子仍需安装。
static constexpr bool kEnableApplyHook = true;

// 单独启用「单表 RTON 加载器 0x2cbfb4」探针（Android OFF_DB_LOAD_RTON 的等价物）。
// 用途：抓它在启动期加载各表时的真实参数，据此实现「按 tableId + CDN 文件加载语言表」。
static constexpr bool kEnableTblLoadProbe = false;   // 诊断用（记录表加载参数），默认关闭
static constexpr bool kEnableCdnManifestProbe = false;  // 诊断用（记录 CDN 清单条目），默认关闭
static std::atomic<int> g_applyCtxCalls{0};

// 供语言模块调用：用游戏自己的 CDN 单文件应用，把指定文件名应用到 table 131。
// 这正是安卓 try_external_apply 调 OFF_CDN_APPLY(ctx, path, name, tableId) 的等价物。
extern "C" bool lzt_ios_apply_lang_file(const char* fileName) {
    if (fileName == nullptr || g_origApply2 == nullptr) return false;
    const uintptr_t ctx = g_applyCtx.load(std::memory_order_acquire);
    if (ctx == 0) {
        lzt_core::log_write("lang file apply: 未捕获 CDN 应用上下文");
        return false;
    }
    if (oStrCtor == nullptr) {
        lzt_core::log_write("lang file apply: 字符串构造器未就绪");
        return false;
    }
    alignas(16) uint8_t nameObj[32] = {0};
    oStrCtor(nameObj, fileName);
    lzt_core::log_write("lang file apply: begin name=%s ctx=%p", fileName, (void*)ctx);
    const uintptr_t r = g_origApply2(ctx, 131 /*LawnStrings*/,
                                     reinterpret_cast<uintptr_t>(nameObj));
    lzt_core::log_write("lang file apply: end ret=0x%lx", (unsigned long)r);
    return r != 0;
}

static uintptr_t hkApply2(uintptr_t ctx, uint32_t tableId, uintptr_t nameStr) {
    // 默认静默（kEnableApplyProbe2）；仅保留上下文捕获（功能链路需要它）。
    const bool isLang = (tableId == 131);
    if (isLang) g_applyCtx.store(ctx, std::memory_order_release);
    else if (g_applyCtxCalls.fetch_add(1) == 0) g_applyCtx.store(ctx, std::memory_order_release);
    if (kEnableApplyProbe2) {
        static std::atomic<int> s_n{0};
        static std::atomic<int> s_lang{0};
        if ((isLang && s_lang.fetch_add(1) < 40) ||
            (!isLang && s_n.fetch_add(1) < 30)) {
            char name[160] = {0};
            read_sstr(nameStr, name, sizeof(name));
            lzt_core::log_write("cdn-apply%s: table=%u name=\"%s\"",
                                isLang ? "(LANG)" : "", tableId, name);
        }
    }
    return g_origApply2 ? g_origApply2(ctx, tableId, nameStr) : 0;
}

static constexpr bool kEnableCdnManifestInject = kEnableCdnManifestProbe;

// ---- 目标语言（供清单注入改写文件名；由语言模块在切换时写入）----
// 对应安卓 cdn_load_rton：把 CDN 清单里的 LawnStrings 条目固定注入成静态名
// （如 LawnStrings-zh-cn.rton，tableId=131），使中文表与游戏语言解耦。
// 这里改为「按当前目标语言动态改写」——因为我们的表本身就是按语言分文件的。
extern "C" void lzt_ios_set_target_locale(const char* locale) {
    if (locale == nullptr) return;
    snprintf(g_targetLocale, sizeof(g_targetLocale), "%s", locale);
    lzt_core::log_write("cdn manifest target locale = %s", g_targetLocale);
}

// ---- 候选函数探针：确认哪条路径在启动时真正被调用（只读，仅记日志）----
// 候选 A：0x1002C1298 —— 内部会读 LangMgr 语言标识并拼 "LawnStrings-<locale>"，
//         但静态交叉引用未找到调用者（疑似经函数指针调用）。
// 候选 B：清单收集函数（含 0x100040448）—— 同样拼该名字并追加进清单。
static std::atomic<int> g_probeA{0};
static uintptr_t (*g_origProbeA)(uintptr_t, uintptr_t) = nullptr;
static uintptr_t hkProbeA(uintptr_t a0, uintptr_t a1) {
    const int n = g_probeA.fetch_add(1);
    if (n < 4) lzt_core::log_write("PATH-A 命中 #%d a0=%p a1=%p", n + 1, (void*)a0, (void*)a1);
    return g_origProbeA ? g_origProbeA(a0, a1) : 0;
}

// 候选 C：0x10003F4D4 —— 清单收集函数本体（栈帧 2256 字节，内部会拼
// "LawnStrings-<locale>.rton" 并以 tableId=131 追加进清单）。
// 挂钩它可确认：该收集流程在当前 iOS 启动过程中是否真的执行。
static std::atomic<int> g_probeC{0};
static uintptr_t (*g_origProbeC)(uintptr_t) = nullptr;
static uintptr_t hkProbeC(uintptr_t a0) {
    const int n = g_probeC.fetch_add(1);
    if (n < 4) lzt_core::log_write("PATH-C 命中 #%d a0=%p", n + 1, (void*)a0);
    return g_origProbeC ? g_origProbeC(a0) : 0;
}

// 候选 D：0x10006525C —— 内部用 "packages/" 前缀 + 管理器(0x1027F4ED0) 拼资源名，
// 疑似 RSB/资源名构建路径（游戏读外置资源的入口之一）。
static std::atomic<int> g_probeD{0};
static uintptr_t (*g_origProbeD)(uintptr_t, uintptr_t, uintptr_t, uintptr_t) = nullptr;
static uintptr_t hkProbeD(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    const int n = g_probeD.fetch_add(1);
    if (n < 4) lzt_core::log_write("PATH-D 命中 #%d a0=%p a1=%p", n + 1, (void*)a0, (void*)a1);
    return g_origProbeD ? g_origProbeD(a0, a1, a2, a3) : 0;
}

// 读取 libc++ std::string 对象的 C 视图（长度在 +23，最高位=长串）。
// 游戏的清单条目名以 std::string 传递，这里只读、不修改。
using CdnListAddFn = void (*)(uintptr_t, uintptr_t, uint32_t, uint32_t);
static std::atomic<int> g_cdnAddCalls{0};

static void hkCdnListAdd(uintptr_t mgr, uintptr_t nameStr, uint32_t tableId,
                         uint32_t flag) {
    if (mgr != 0) g_cdnListMgr.store(mgr, std::memory_order_release);
    // 安卓式注入（对应 cdn_load_rton 的清单改写）：
    // LawnStrings 条目（tableId=131）按【当前目标语言】改写文件名，
    // 于是游戏会用该语言的文件去填充语言表 —— 与游戏语言标识解耦。
    if (tableId == 131 && g_targetLocale[0] != 0 && nameStr != 0) {
        char nm[128] = {0};
        read_sstr(nameStr, nm, sizeof(nm));
        if (strncasecmp(nm, "LawnStrings-", 12) == 0) {
            char want[96];
            snprintf(want, sizeof(want), "LawnStrings-%s.rton", g_targetLocale);
            if (strcasecmp(nm, want) != 0) {
                static char s_injectName[96];   // 静态缓冲：确保调用期间有效
                snprintf(s_injectName, sizeof(s_injectName), "%s", want);
                alignas(16) static uint8_t s_injectObj[32];
                memset(s_injectObj, 0, sizeof(s_injectObj));
                if (oStrCtor != nullptr) {
                    oStrCtor(s_injectObj, s_injectName);
                    lzt_core::log_write("cdn manifest inject(131): \"%s\" -> \"%s\"",
                                        nm, s_injectName);
                    if (g_origCdnListAdd) {
                        g_origCdnListAdd(mgr, reinterpret_cast<uintptr_t>(s_injectObj),
                                         tableId, flag);
                    }
                    return;
                }
            }
        }
    }
    const int n = g_cdnAddCalls.fetch_add(1);
    if (n < 60) {
        char name[96];
        read_sstr(nameStr, name, sizeof(name));
        lzt_core::log_write("cdn manifest add #%d: mgr=%p table=%u flag=%u name=\"%s\"",
                            n + 1, (void*)mgr, tableId, flag, name);
    }
    if (g_origCdnListAdd) g_origCdnListAdd(mgr, nameStr, tableId, flag);
}

// ============================================================
// 文件访问监控（fishhook 式符号重绑定）
//
// 目的：定位「游戏读取 CDN 语言包的代码」。此前用 inline hook 挂 libc 会崩
//       （dyld 共享缓存 + 远跳补丁），而本方式只替换数据段的符号指针，
//       不改任何指令，因此安全。
//
// 找到读取路径后，即可在语言切换时重新执行它 —— 这就是对齐安卓热重载的关键。
// ============================================================
// 打印调用栈（相对主程序基址的偏移），用于定位「谁读了目标文件」。
// 这是定位加载路径最直接的手段：hook 文件访问 + 回栈 = 精确调用点。
static void lzt_dump_backtrace(const char* tag) {
    void* frames[16];
    const int n = backtrace(frames, 16);
    char line[600];
    int off = snprintf(line, sizeof(line), "%s bt:", tag);
    for (int i = 0; i < n && off < 540; ++i) {
        off += snprintf(line + off, sizeof(line) - off, " %lx",
                        (unsigned long)((uintptr_t)frames[i] - lzt_core::base()));
    }
    lzt_core::log_write("%s", line);
}

static bool lzt_path_interesting(const char* p) {
    if (p == nullptr) return false;
    return strstr(p, "LawnStrings") != nullptr ||
           strstr(p, "No_Backup") != nullptr ||
           strstr(p, "CDN.") != nullptr;
}

static std::atomic<int> g_fileProbeN{0};

static int (*g_origOpenFn)(const char*, int, ...) = nullptr;
static int lzt_open_hook(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    // 先记录所有调用（确认 hook 是否真的生效），再单独标记关心路径
    {
        static std::atomic<int> s_all{0};
        if (s_all.fetch_add(1) < 10) {
            const uintptr_t lr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            lzt_core::log_write("ALL open: %s (caller +0x%lx)",
                                path ? path : "(null)",
                                (unsigned long)(lr - lzt_core::base()));
        }
    }
    if (lzt_path_interesting(path)) {
        const int n = g_fileProbeN.fetch_add(1);
        if (n < 40) {
            const uintptr_t lr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            lzt_core::log_write("FILE open: %s (caller +0x%lx)", path,
                                (unsigned long)(lr - lzt_core::base()));
            lzt_dump_backtrace("FILE open");
        }
    }
    return g_origOpenFn ? g_origOpenFn(path, flags, mode) : -1;
}

static FILE* (*g_origFopenFn)(const char*, const char*) = nullptr;
static FILE* lzt_fopen_hook(const char* path, const char* mode) {
    if (lzt_path_interesting(path)) {
        const int n = g_fileProbeN.fetch_add(1);
        if (n < 60) {
            const uintptr_t lr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            lzt_core::log_write("FILE fopen: %s (caller +0x%lx)", path,
                                (unsigned long)(lr - lzt_core::base()));
        }
    }
    return g_origFopenFn ? g_origFopenFn(path, mode) : nullptr;
}

// openat(2)：现代 iOS 上的文件打开几乎都走它（fs_usage 显示的 "open" 实为 openat）。
static int (*g_origOpenatFn)(int, const char*, int, ...) = nullptr;
static int lzt_openat_hook(int fd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    if (lzt_path_interesting(path)) {
        const int n = g_fileProbeN.fetch_add(1);
        if (n < 80) {
            const uintptr_t lr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            lzt_core::log_write("FILE openat: %s (caller +0x%lx)", path,
                                (unsigned long)(lr - lzt_core::base()));
            lzt_dump_backtrace("FILE openat");
        }
    }
    return g_origOpenatFn ? g_origOpenatFn(fd, path, flags, mode) : -1;
}

static void install_file_probe() {
    if (g_origOpenFn != nullptr) return;   // 幂等
    int n1 = lzt_rebind("open", reinterpret_cast<void*>(lzt_open_hook),
                        reinterpret_cast<void**>(&g_origOpenFn));
    int n2 = lzt_rebind("fopen", reinterpret_cast<void*>(lzt_fopen_hook),
                        reinterpret_cast<void**>(&g_origFopenFn));
    int n3 = lzt_rebind("openat", reinterpret_cast<void*>(lzt_openat_hook),
                        reinterpret_cast<void**>(&g_origOpenatFn));
    lzt_core::log_write("file probe: open=%d fopen=%d openat=%d", n1, n2, n3);
}

// ---- 资源目录重定向：路径改写 ----
//
// 游戏的资源路径对象（0x1C32040 构造）在 +88 处保存 Application Support 路径，
// No_Backup 即拼在它之后。这里把该前缀原地改写为 Documents，使游戏内部记录的
// 路径直接指向 Documents/No_Backup。
//   · 函数 A（0x1C310E8）返回 base + "No_Backup/"，但实测启动流程未调用它；
//     仍一并处理，以防某些流程使用。
//   · 函数 B（0x1C32040）构造路径根对象——成员 +16 是 Documents、+88 是
//     Application Support（运行时 dump 实证）。
// 原地改写是安全的：替换后字符串变短（27 → 9 字符），前移即可，无需重新分配。

// libc++ std::string（24 字节，与游戏内布局一致）
struct LzS24 { uint64_t w[3]; };

static bool read_lz_str(const uint8_t* o, std::string& out) {
    if (!lzt_core::memory_range_accessible((uintptr_t)o, 24, false)) return false;
    const uint8_t tail = o[23];
    if (tail & 0x80) {
        const char* p = *reinterpret_cast<const char* const*>(o);
        const size_t n = *reinterpret_cast<const size_t*>(o + 8);
        if (p == nullptr || n == 0 || n > 8192) return false;
        if (!lzt_core::memory_range_accessible((uintptr_t)p, n, false)) return false;
        out.assign(p, n);
    } else {
        if (tail == 0) return false;
        out.assign(reinterpret_cast<const char*>(o), tail);
    }
    return true;
}

static LzS24 (*g_origNbPathA)(const void*) = nullptr;
static std::atomic<int> g_nbPathLog{0};

// 把字符串中的路径前缀 "Library/Application Support" 原地替换为 "Documents"。
// 为什么安全：替换后字符串变短（19 字符 → 9 字符），原地前移即可，
// 既不需要重新分配，也不改变字符串对象的存放位置。
// 这正是把游戏资源目录从 Library/Application Support 重定向到 Documents 的
// 关键一步——拼接时用的是运行期取得的系统路径，所以只能在「拼好的结果」上换前缀。
static const char kAppSupportSeg[] = "Library/Application Support";
static constexpr size_t kAppSupportLen = 27;
static constexpr size_t kDocumentsLen = 9;

static char* nb_find(char* hay, size_t hlen, const char* needle, size_t nlen) {
    if (hlen < nlen) return nullptr;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return nullptr;
}

// 取得字符串对象的数据指针与长度（iOS libc++ 布局：长度在 +23，最高位=长串）
static char* nb_data(uint8_t* s, size_t* len, bool* isLong) {
    const uint8_t tail = s[23];
    if (tail & 0x80) {
        *isLong = true;
        *len = *reinterpret_cast<size_t*>(s + 8);
        return *reinterpret_cast<char**>(s);
    }
    *isLong = false;
    *len = tail;
    return reinterpret_cast<char*>(s);
}

static const char* nb_read_cstr(const uint8_t* s) {
    size_t len = 0; bool isLong = false;
    char* p = nb_data(const_cast<uint8_t*>(s), &len, &isLong);
    return p ? p : "";
}

static bool nb_replace_inplace(uint8_t* s) {
    size_t len = 0; bool isLong = false;
    char* data = nb_data(s, &len, &isLong);
    if (data == nullptr || len < kAppSupportLen) return false;
    char* pos = nb_find(data, len, kAppSupportSeg, kAppSupportLen);
    if (pos == nullptr) return false;

    const size_t tailBytes = (data + len) - (pos + kAppSupportLen);   // 需前移的尾段
    memmove(pos + kDocumentsLen, pos + kAppSupportLen, tailBytes + 1); // +1 含结尾 NUL
    memcpy(pos, "Documents", kDocumentsLen);
    const size_t newLen = len - (kAppSupportLen - kDocumentsLen);
    if (isLong) *reinterpret_cast<size_t*>(s + 8) = newLen;
    else s[23] = static_cast<uint8_t>(newLen);
    return true;
}

static LzS24 hkNbPathA(const void* obj) {
    LzS24 r = g_origNbPathA(obj);
    // 函数 A 返回 base + "No_Backup/"（base 取自入参 +88），对结果做前缀替换。
    (void)nb_replace_inplace(reinterpret_cast<uint8_t*>(&r));
    return r;
}
// ============================================================
// 方案 A：把 FS 单例的 AppSupport 路径源改为 Documents
// （对齐文档《No_Backup 迁移至 Documents》§2.1 的静态补丁）
//
// 文档的静态补丁只改一条 BL：
//   0x101C30FE0: BL sub_101C36564 (NSApplicationSupportDirectory)
//              → BL sub_101C3639C (NSDocumentDirectory)
// 效果：FS 单例 +0x58 从 getter 起就返回 Documents，因此
//   · case 3/4 天然指向 Documents/No_Backup、Documents/Backup
//   · 目录初始化（0x1c32040）创建子目录时路径已经正确
//     （不再像旧方案那样「先用旧路径创建、事后才改写」）
//   · iCloud 排除属性会设到正确位置
//
// iOS 侧无需改二进制：hook AppSupport getter(0x1c36564) 并【尾跳转】到
// Documents getter(0x1c3639c)。理由：
//   · 两者签名一致（x8 = 输出 std::string，sret 约定；x0 未被使用）
//   · 0x1c36564 全二进制【只有 1 个调用点】= 0x1c30fe0，正是文档补丁位置
//     （0x1c30f24 成员初始化：bl 0x1c36564 → 写 FS 单例 +88）
// 因此本 hook 与文档那条静态补丁等价，且不触碰二进制。
//
// 保留 hkNbPathB（事后前缀改写）与符号链接作为兜底：
// 若本 hook 因时机太晚而未覆盖到 FS 单例初始化，旧路径仍能被纠正。
extern "C" void* g_origDocGetter = nullptr;

extern "C" __attribute__((naked)) void hkAppSupGetter() {
    __asm__ volatile(
        "adrp x9, _g_origDocGetter@PAGE\n"
        "add  x9, x9, _g_origDocGetter@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 1f\n"
        "br   x9\n"          // 尾跳转到 Documents getter
        "1:\n"
        "ret\n"
    );
}
// 函数 B（0x1c32040）：路径根对象构造，+16/+40/+64/+88 分别为
// Documents / Bundle / Caches / Application Support。构建完成后把后者改写为
// Documents，使游戏内部记录的 No_Backup 路径直接指向新位置。
static uintptr_t (*g_origNbPathB)(uintptr_t) = nullptr;
static std::atomic<int> g_nbPathBLog{0};

static uintptr_t hkNbPathB(uintptr_t obj) {
    // 记录调用者：定位「谁在启动时构建资源路径对象」，进而找到读取 CDN 文件的代码。
    {
        static std::atomic<int> s_n{0};
        if (s_n.fetch_add(1) < 6) {
            const uintptr_t lr = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            lzt_core::log_write("no_backup redirect: called from +0x%lx",
                                (unsigned long)(lr - lzt_core::base()));
        }
    }
    // 先调用原函数（它负责把四个基础目录填进对象成员）。
    // 注意必须在原函数【之后】替换：对象在入口时还是空的，
    // 而拼子目录（No_Backup/ 等）都发生在它内部。
    const uintptr_t ret = g_origNbPathB(obj);

    if (obj != 0) {
        static const size_t kOffs[] = {16, 40, 64, 88};
        int fixed = 0;
        for (size_t off : kOffs) {
            if (nb_replace_inplace(reinterpret_cast<uint8_t*>(obj) + off)) ++fixed;
        }
        // 只首次记录：证明重定向已生效（含结果路径，便于核对）
        if (fixed > 0 && g_nbPathBLog.fetch_add(1) == 0) {
            lzt_core::log_write("no_backup redirect(path): %s",
                                nb_read_cstr(reinterpret_cast<const uint8_t*>(obj) + 88));
        }
    }
    return ret;
}

} // namespace

// 对外入口：供语言模块在切换语言后调用（请求游戏重新应用 CDN 文件，
// 目的是让整个游戏 UI 跟随语言切换，而不只是本模块构建的设置页）。
bool lzt_ios_reapply_cdn_export() { return lzt_ios_reapply_cdn(); }

// ============================================================
// 自动化/调试：启动后自动触发一次语言切换
//
// 读取 <Documents>/lzt_autolang.txt（内容为目标 locale），在启动后延迟若干秒调用
// 语言模块的切换入口 —— 等价于点一次语言 checkbox，便于无人值守验证整体链路。
// 文件不存在时完全无副作用；正式使用请删除该文件。
// ============================================================
// 语言模块的程序化切换入口（实现见 jni/language_module.cpp）
// 调试总开关：false 时「自动切换语言」入口完全禁用（对外发布版保持 false）。
static constexpr bool kEnableAutolangDebug = false;   // 调试入口：默认关闭（代码保留备查）
extern "C" void lzt_lang_switch_now(const char* locale);

static void lzt_ios_maybe_autolang(void) {
    if (!kEnableAutolangDebug) return;   // 对外版：调试入口关闭（代码保留备查）
    // 默认关闭：仅当容器 Documents 下存在 lzt_autolang.txt 时才启用（无人值守验证用）。
    // 正式使用请确保删除该文件，否则游戏会周期性自动切换语言。
    char path[512] = {0};
    char files[384] = {0};
    lzt_core::files_dir(files, sizeof(files));
    if (files[0] == 0) return;
    snprintf(path, sizeof(path), "%s/../Documents/lzt_autolang.txt", files);
    FILE* f = fopen(path, "r");
    if (f == nullptr) return;
    char locale[32] = {0};
    if (fgets(locale, sizeof(locale), f) == nullptr) { fclose(f); return; }
    fclose(f);
    for (char* p = locale; *p != 0; ++p) {
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { *p = 0; break; }
    }
    if (locale[0] == 0) return;
    lzt_core::log_write("autolang: 周期性切换语言（目标 %s，每 20 秒一轮）", locale);
    char loc[32] = {0};
    snprintf(loc, sizeof(loc), "%s", locale);
    static char s_autolang[32];
    snprintf(s_autolang, sizeof(s_autolang), "%s", loc);
    static int s_round = 0;
    static void (^tick)(void);
    tick = ^{
        ++s_round;
        const char* want = (s_round % 2 == 1) ? "en-us" : s_autolang;
        lzt_core::log_write("autolang: tick #%d -> %s", s_round, want);
        lzt_lang_switch_now(want);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(20 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), tick);
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(15 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), tick);
}

// ============================================================
// 主界面横幅（PersistentMessage）—— 对应安卓 screen_bindings 的横幅刷新
//
// 定位：横幅文本键 [PERSISTENT_MESSAGING_OFFLINE]（标题）/ [..._CONTENT]，
// 由 0x6978f8(a1 = 横幅对象) 构建：内部用游戏本地化口取值后建立控件。
// 做法（与安卓一致）：hook 该构建函数记录最新横幅对象（带存活校验），
// 切换语言时对它重新调用一次 —— 它会用【换过的表】重新取值并刷新文本。
// 机制级：不硬编码横幅内容，也不直接改文本。
static constexpr uintptr_t OFF_PERSISTENT_BANNER_REBUILD = 0x6978f8;
static void (*g_origBannerRebuild)(uintptr_t) = nullptr;
static std::atomic<uintptr_t> g_bannerObj{0};
static std::atomic<uintptr_t> g_bannerVt{0};

// 供收尾链调用：设置页存在 → 挂起；否则立即刷新（定义见本文件后部）
static void lzt_ios_request_screen_refresh(void);

static void hkBannerRebuild(uintptr_t a1) {
    if (g_origBannerRebuild) g_origBannerRebuild(a1);
    if (a1 != 0 && lzt_core::memory_range_accessible(a1, 8, false)) {
        g_bannerObj.store(a1, std::memory_order_release);
        g_bannerVt.store(*reinterpret_cast<uintptr_t*>(a1), std::memory_order_release);
        static std::atomic<int> s_n{0};
        if (s_n.fetch_add(1) < 4) lzt_core::log_write("banner captured a1=%p", (void*)a1);
    }
}

// 切语言时重建横幅（vtable 校验防悬垂，与安卓 refresh_persistent_banner 同款）
static void refresh_persistent_banner() {
    const uintptr_t a1 = g_bannerObj.load(std::memory_order_acquire);
    if (a1 == 0 || g_origBannerRebuild == nullptr) return;
    if (!lzt_core::memory_range_accessible(a1, 8, false)) return;
    if (*reinterpret_cast<uintptr_t*>(a1) != g_bannerVt.load(std::memory_order_acquire)) return;
    g_origBannerRebuild(a1);
    lzt_core::log_write("banner refreshed a1=%p", (void*)a1);
}

// 安装横幅捕获 hook（引擎就绪后调用，自幂等）
static void install_banner_hook(uintptr_t base) {
    if (g_origBannerRebuild != nullptr || base == 0) return;
    void* t = nullptr;
    A64HookFunction(reinterpret_cast<void*>(base + OFF_PERSISTENT_BANNER_REBUILD),
                    reinterpret_cast<void*>(hkBannerRebuild), &t);
    g_origBannerRebuild = reinterpret_cast<void (*)(uintptr_t)>(t);
    lzt_core::log_write("banner hook(0x6978f8): %s", t ? "ok" : "FAIL");
}

// ---- 主界面（MainMenu）----
//
// 0x6f349c(a1 = 主界面对象)：主界面的构建/重建函数。内部可见：
//   · 用 [MAINMENU_PLAY] 等键经本地化口取值后建立按钮（0x6f3504 一带）
//   · 文本写到对象 +184 附近（0x6f3500: add x21, x19, #184）
// 做法与横幅一致：hook 它记录对象，切语言时对它重调 → 文本按新表重新取值。
static constexpr uintptr_t OFF_MAINMENU_REBUILD = 0x6f349c;
static void (*g_origMainMenuRebuild)(uintptr_t) = nullptr;
static std::atomic<uintptr_t> g_mainMenuObj{0};
static std::atomic<uintptr_t> g_mainMenuVt{0};

static void hkMainMenuRebuild(uintptr_t a1) {
    if (g_origMainMenuRebuild) g_origMainMenuRebuild(a1);
    // 【v15】主界面重建 = 已进入主界面。
    //
    // 以本函数的【调用事实】作为「启动完成、资源系统就绪」信号，解除语言表的
    // 启动期闸门（language_module.cpp 的 g_bootPhase）。此前 v13 的解除点在
    // lang_should_create()，那是在 hook_create_tab 的游戏 UI 构建调用栈内同步
    // 重入游戏资源系统，与已修复的「启动期重入」属同类风险。
    //
    // 为何用主界面重建而非横幅（PersistentMessage）重建：
    //   · 横幅只在存在横幅消息时才重建，触发不稳定；
    //   · 本函数每次进入主界面都会执行（代码注释已实证：它产生的顶层节点
    //     会压在设置页之上），玩家必然经过，时机也早于任何设置页构建。
    //
    // 无条件通知（不看 a1）：本函数被调用本身即「主界面正在构建」的证据，
    // a1 是横幅对象句柄，其有效性不影响该事实。通知侧幂等，重复调用无副作用。
    lzt_settings::notify_main_screen_entered();
    if (a1 != 0 && lzt_core::memory_range_accessible(a1, 8, false)) {
        g_mainMenuObj.store(a1, std::memory_order_release);
        g_mainMenuVt.store(*reinterpret_cast<uintptr_t*>(a1), std::memory_order_release);
        static std::atomic<int> s_n{0};
        if (s_n.fetch_add(1) < 4) lzt_core::log_write("mainmenu captured a1=%p", (void*)a1);
    }
}

static void refresh_main_menu() {
    const uintptr_t a1 = g_mainMenuObj.load(std::memory_order_acquire);
    if (a1 == 0 || g_origMainMenuRebuild == nullptr) return;
    if (!lzt_core::memory_range_accessible(a1, 8, false)) return;
    if (*reinterpret_cast<uintptr_t*>(a1) != g_mainMenuVt.load(std::memory_order_acquire)) return;
    g_origMainMenuRebuild(a1);
    lzt_core::log_write("mainmenu refreshed a1=%p", (void*)a1);
    // ⚠️ 主界面重建会产生【新的顶层节点】，它会压在设置页之上（实测）。
    // 因此若设置页当前打开着，重建后必须把它重新提到最前，否则设置面板被盖住。
    const uintptr_t sp = lzt_settings::last_settings_page();
    if (sp != 0 && lzt_core::memory_range_accessible(sp, 8, false)) {
        lzt_ios_page_layout(sp);
        lzt_core::log_write("mainmenu refresh: settings page re-shown %p", (void*)sp);
    }
}

static void install_mainmenu_hook(uintptr_t base) {
    if (g_origMainMenuRebuild != nullptr || base == 0) return;
    void* t = nullptr;
    A64HookFunction(reinterpret_cast<void*>(base + OFF_MAINMENU_REBUILD),
                    reinterpret_cast<void*>(hkMainMenuRebuild), &t);
    g_origMainMenuRebuild = reinterpret_cast<void (*)(uintptr_t)>(t);
    lzt_core::log_write("mainmenu hook(0x6f349c): %s", t ? "ok" : "FAIL");
}

// 供语言切换收尾链调用：按注册顺序执行「业务屏幕刷新器」（主界面 / 横幅等）。
// 由框架的 register_refresher 注册，这里只负责遍历（框架不感知具体屏幕）。
// ---- 业务屏幕刷新的「挂起 / 执行」（方案 A）----
//
// 问题：在设置页打开期间重建主界面会新建顶层节点，盖在设置页之上（实测）。
// 做法：设置页存在时只置「待刷新」标志；等离开设置页时统一刷新一次。
// 这样主界面文本最终跟切，且不会出现「主界面浮在设置页之上」。
static std::atomic<bool> g_screenRefreshPending{false};


// 供框架在「离开设置页」时调用：把挂起的刷新补上（实现在上方命名空间内）
extern "C" void lzt_ios_flush_pending_screen_refresh(void) {
    lzt_ios_flush_pending_screen_refresh_impl();
}

static void probe_pakmgr();   // 定义见本文件后部（诊断）
static void lzt_ios_prewarm_language_table_async();   // 定义见本文件后部

static void register_banner_binding() {
    using RegisterRefresherFn = bool (*)(void (*)());
    using RegisterReadyFn = bool (*)(void (*)(uintptr_t));
    RegisterRefresherFn rr = &lzt_settings::register_refresher;
    RegisterReadyFn rr2 = &lzt_settings::register_engine_ready;
    rr(&refresh_persistent_banner);
    rr(&refresh_main_menu);
    rr2(&install_banner_hook);
    rr2(&install_mainmenu_hook);
    lzt_core::log_write("[SFW] banner + mainmenu binding registered");
    // ⚠️ engine_install 可能已经执行过（本函数在模块就绪后调用），
    // 那样就绪回调不会被再次触发 —— 因此这里立即自行安装一次（幂等）。
    install_banner_hook(lzt_core::base());
    install_mainmenu_hook(lzt_core::base());
    // 【诊断】此处已晚于启动早期，pakMgr 应已构造：探测其结构
    probe_pakmgr();
}

// ============================================================
// 【诊断】语言短码映射函数 0x1c8380（安卓 sub_98A694 的等价物）
//
// 结构（反汇编实证，与安卓答复完全一致）：
//   · 序言读 *(x0+2104) 的 +508 → 当前语言标识（FourCC）
//   · 依次构造 2 字符短码（"en"/"fr"/"pt"…，取自相邻常量串表）
//   · 调 0x101ac6e5c（短码 → locale id 转换），再 `cmp w22,w20`（寄存器对寄存器）
//   · 命中即返回对应 locale id 字符串
// 用途：确认它在 News 取键链中的调用点（静态找不到调用者，说明经函数指针调用）。
static constexpr bool kEnableLocaleMapProbe = false;   // 仅控制「打印映射调用日志」
// News 兜底所需：映射函数（0x1c8380）必须挂钩 —— 与日志开关解耦（曾因共用开关导致功能失效）。
static constexpr bool kEnableNewsLocaleHook = true;
// ⚠️ 该函数使用非标准 ABI：x0 = 输入对象，x8 = 输出对象（sret）——
// 序言第一行即 `mov x19, x8`（保存输出对象）。
// 早期按单参数挂钩会丢失 x8 → 函数内部写入垃圾地址而崩溃（实测）。
extern "C" __attribute__((naked)) void hkLocaleMap() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x8, [sp, #24]\n"          // 保存 sret 输出对象
        "str x30, [sp, #32]\n"          // 保存调用者 LR（用于打印调用来源）
        "adrp x9, _g_origLocaleMap@PAGE\n"
        "add  x9, x9, _g_origLocaleMap@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x8, [sp, #24]\n"          // 恢复 x8
        "blr  x9\n"
        "9:\n"
        "ldr  x0, [sp, #24]\n"          // x0 = 输出对象（sret）
        "ldr  x1, [sp, #32]\n"          // x1 = 调用者 LR
        "bl   _lzt_localemap_log\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

// News 兜底（定义见本文件后部）：映射函数出口检查并填替身短码
extern "C" void lzt_news_shortcode_fallback(uint8_t* out24);

extern "C" void lzt_localemap_log(uintptr_t out24, uintptr_t callerLr) {
    const uintptr_t lr = callerLr;
    char before[64] = {0};
    char after[64] = {0};
    read_sstr(out24, before, sizeof(before));
    // News 兜底：映射得到的短码为空时，用当前 locale 首段填入。
    lzt_news_shortcode_fallback(reinterpret_cast<uint8_t*>(out24));
    read_sstr(out24, after, sizeof(after));
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) < 60) {
        lzt_core::log_write("localeMap: caller=+0x%lx before=\"%s\" after=\"%s\"",
                            (unsigned long)(lr - lzt_core::base()), before, after);
    }
}

extern "C" void* g_origLocaleMap = nullptr;   // 须 C 链接（供 naked 桥引用）
static std::atomic<int> g_localeMapN{0};

// 若需打印调用来源，可在 hkLocaleMap 的裸汇编里额外保存 x30（当前只做透传）。

// 【诊断】短码 → locale id 转换函数（0x101ac6e5c）：已完成使命，默认关闭。
// 参数：x0 = 输出对象（sret），x1 = 输入的 2 字符短码对象（libc++ string）。
static constexpr bool kEnableShortcodeProbe = false;
extern "C" void* g_origShortcodeFn = nullptr;

extern "C" __attribute__((naked)) void hkShortcodeFn() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "adrp x9, _g_origShortcodeFn@PAGE\n"
        "add  x9, x9, _g_origShortcodeFn@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "blr  x9\n"
        "9:\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}


// ============================================================
// News 内容本地化补丁（对应安卓 screen_bindings 的方案 D）
//
// 缺陷链（iOS 实测，与安卓同构）：
//   News 取键 0x1c6db0 → bl 0x1c8380（当前 locale → 两字母短码；白名单 en/fr/… 不含 zh）
//                     → 短码为空串 → 与 "UI_MainMenuUI_…" 等前缀拼接成资源名 → 查不到 → 内容空白
//
// 做法（与安卓 screen_bindings 的方案 D 对齐）：
//   · 触发条件：取到的【键/短码为空串】时（长度 0）才处理
//   · 替身值：按语言标识做【前缀匹配】——取首段两字母小写（zh-cn → "zh"，jp-jp → "jp"）
//   · 不改写调用方内存：替身串仅在本次查询期间有效（与安卓同款：短串 SSO 就地构造）
// 落点选择：映射函数 0x1c8380 的出口（News 取键 0x1c6db0 紧接其后拼接使用），
// 与安卓 hook sub_145BE84「取键处兜底」等价，且不涉及该函数其他少量调用者。
static constexpr bool kEnableNewsLocaleFallback = true;

// 取当前 locale 的首段短码（小写），如 zh-cn → "zh"。
// 来源：*(*(base+0x27eb870)+2104) 的 +508（语言标识 FourCC，如 0x5A48434E = "ZH-CN"）。
static bool current_locale_short(char* out, size_t cap) {
    if (out == nullptr || cap < 3) return false;
    out[0] = 0;
    const uintptr_t base = lzt_core::base();
    if (base == 0) return false;
    if (!lzt_core::memory_range_accessible(base + OFF_G_Config, 8, false)) return false;
    const uintptr_t disp = *reinterpret_cast<const uintptr_t*>(base + OFF_G_Config);
    if (disp == 0) return false;
    if (!lzt_core::memory_range_accessible(disp + 2104, 8, false)) return false;
    const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(disp + 2104);
    if (mgr == 0 || !lzt_core::memory_range_accessible(mgr + 508, 4, false)) return false;
    const uint32_t fourcc = *reinterpret_cast<const uint32_t*>(mgr + 508);
    const char c0 = static_cast<char>((fourcc >> 24) & 0xFF);
    const char c1 = static_cast<char>((fourcc >> 16) & 0xFF);
    if (c0 < 'A' || c0 > 'z') return false;
    if (c1 < 'A' || c1 > 'z') return false;
    out[0] = static_cast<char>((c0 >= 'A' && c0 <= 'Z') ? c0 + 32 : c0);
    out[1] = static_cast<char>((c1 >= 'A' && c1 <= 'Z') ? c1 + 32 : c1);
    out[2] = 0;
    return true;
}

// 供映射函数出口调用：若输出短码为空，用 locale 首段填入（libc++ 短串布局）。
extern "C" void lzt_news_shortcode_fallback(uint8_t* out24) {
    if (!kEnableNewsLocaleFallback || out24 == nullptr) return;
    if (!lzt_core::memory_range_accessible(reinterpret_cast<uintptr_t>(out24), 24, false)) return;
    //   · 触发条件：映射结果为【空串】时才替换（非空一律原样放行）
    // ⚠️ libc++ 短串：byte23 = len*2。空串即 0；早期用 `tail != 0` 判断导致空串被跳过（实测）。
    const uint8_t tail = out24[23];
    if ((tail & 0x80) != 0) {
        // 长串形态：长度为 0 才需要兜底
        const uint64_t len = *reinterpret_cast<const uint64_t*>(out24 + 8);
        if (len != 0) return;
    } else if (tail != 0) {
        return;                                    // 非空短串 → 原样保留
    }
    // 走到这里：确实是空串（tail == 0 或长串长度 0）
    // 兜底
    (void)0;
    char s[4] = {0};
    if (!current_locale_short(s, sizeof(s))) {
        static std::atomic<int> s_f{0};
        if (s_f.fetch_add(1) < 6) {
            lzt_core::log_write("news fallback: current_locale_short 失败（检查语言字段路径）");
        }
        return;
    }
    memset(out24, 0, 24);
    out24[0] = static_cast<uint8_t>(s[0]);
    out24[1] = static_cast<uint8_t>(s[1]);
    out24[23] = 2;                                 // ⚠️ libc++ SSO：长度在【最后字节 +23】，不是 +0
    lzt_core::log_write("news locale fallback: empty shortcode -> \"%s\"", s);
}

// 【诊断】本地化数据查询 0x7c18fc（News 内容取值）：记录查询名。
// 参数：x0 = 查询名（如 "zh_UI_..."），x8 = 输出对象。
static constexpr bool kEnableLdQueryProbe = false;
extern "C" void* g_origLdQuery = nullptr;

extern "C" __attribute__((naked)) void hkLdQuery() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x8, [sp, #24]\n"
        "adrp x9, _g_origLdQuery@PAGE\n"
        "add  x9, x9, _g_origLdQuery@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x8, [sp, #24]\n"
        "blr  x9\n"
        "9:\n"
        "ldr  x0, [sp, #16]\n"
        "bl   _lzt_ldquery_log\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

extern "C" void lzt_ldquery_log(uintptr_t nameObj) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) >= 40) return;
    char s[192] = {0};
    read_sstr(nameObj, s, sizeof(s));
    if (s[0] == 0) return;
    lzt_core::log_write("ldquery: \"%s\"", s);
}

// 【诊断】拼接函数 0x1bd388c（前缀 + 语言短码 → 最终查询名）
// 参数：x0 = 目标串对象，x1 = 追加的串对象。用于确认最终查询名的真实形态。
static constexpr bool kEnableConcatProbe = false;
extern "C" void* g_origConcat = nullptr;

extern "C" __attribute__((naked)) void hkConcat() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "adrp x9, _g_origConcat@PAGE\n"
        "add  x9, x9, _g_origConcat@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "blr  x9\n"
        "9:\n"
        "ldr  x0, [sp, #16]\n"
        "bl   _lzt_concat_log\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

extern "C" void lzt_concat_log(uintptr_t dstObj) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) >= 30) return;
    char s[192] = {0};
    read_sstr(dstObj, s, sizeof(s));
    if (s[0] == 0) return;
    lzt_core::log_write("concat: \"%s\"", s);
}

// ============================================================
// 【诊断】RSB 包管理器（pakMgr）结构探测
//
// 目的：为「从数据包读取 LANGUAGETYPES.rton」定位 rsm 指针偏移。
// 安卓：pakMgr + 0x858 → ResStreamsManager；iOS 偏移待确认（本探针 dump 头部）。
static constexpr bool kEnablePakMgrProbe = false;

static void probe_pakmgr(void) {
    if (!kEnablePakMgrProbe) return;
    const uintptr_t base = lzt_core::base();
    if (base == 0) return;
    const uintptr_t slot = base + OFF_PAK_MANAGER;
    if (!lzt_core::memory_range_accessible(slot, 8, false)) return;
    const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(slot);
    lzt_core::log_write("pakmgr: slot=%p mgr=%p", (void*)slot, (void*)mgr);
    if (mgr == 0) return;
    // dump 头部 0x100 字节 + 安卓已知偏移 0x858 附近
    for (uintptr_t off = 0; off < 0x100; off += 32) {
        if (!lzt_core::memory_range_accessible(mgr + off, 32, false)) break;
        const uint64_t* w = reinterpret_cast<const uint64_t*>(mgr + off);
        lzt_core::log_write("  mgr+%03lx: %016llx %016llx %016llx %016llx", (unsigned long)off,
                            (unsigned long long)w[0], (unsigned long long)w[1],
                            (unsigned long long)w[2], (unsigned long long)w[3]);
    }
    if (lzt_core::memory_range_accessible(mgr + 0x850, 40, false)) {
        const uint64_t* w = reinterpret_cast<const uint64_t*>(mgr + 0x850);
        lzt_core::log_write("  mgr+850: %016llx %016llx %016llx %016llx %016llx",
                            (unsigned long long)w[0], (unsigned long long)w[1],
                            (unsigned long long)w[2], (unsigned long long)w[3],
                            (unsigned long long)w[4]);
    }
}

// ============================================================
// 【诊断】RSB 资源读取入口 0x64f20（含 packages/ 名字构造）
//
// 它读 `*(base+0x27eb870)+2104`（语言管理器）后按名字取包内资源；
// 疑似「按语言从数据包取表」的入口 —— 正是 LANGUAGETYPES 数据包来源所需。
// 参数：x0 = 名字（或名字对象）；此处打印名字与调用者。
static constexpr bool kEnableRsbReadProbe = false;
extern "C" void* g_origRsbRead = nullptr;

extern "C" __attribute__((naked)) void hkRsbRead() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "str x30, [sp, #32]\n"
        "adrp x9, _g_origRsbRead@PAGE\n"
        "add  x9, x9, _g_origRsbRead@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "blr  x9\n"
        "9:\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #32]\n"
        "bl   _lzt_rsbread_log\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

extern "C" void lzt_rsbread_log(uintptr_t nameObj, uintptr_t callerLr) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) >= 40) return;
    char s[192] = {0};
    read_sstr(nameObj, s, sizeof(s));
    lzt_core::log_write("rsbread: name=\"%s\" caller=+0x%lx", s,
                        (unsigned long)(callerLr - lzt_core::base()));
}

// 【诊断】名字构造 0x6525c 出口：打印拼好的资源名 + 调用者 + 输出对象地址。
// 该函数读 0x27f4xxx 页（包管理器全局所在页），处于「按名字取包内资源」的上游，
// 与下方的 0x64f20 探针共同构成「上下同时打点」。
static constexpr bool kEnableRsbNameProbe = false;
extern "C" void* g_origRsbNameCtor = nullptr;

extern "C" __attribute__((naked)) void hkRsbNameCtor() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "str x30, [sp, #32]\n"
        "adrp x9, _g_origRsbNameCtor@PAGE\n"
        "add  x9, x9, _g_origRsbNameCtor@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "blr  x9\n"
        "9:\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #32]\n"
        "bl   _lzt_rsbname_log2\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

extern "C" void lzt_rsbname_log2(uintptr_t outObj, uintptr_t callerLr) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) >= 60) return;
    char s[192] = {0};
    read_sstr(outObj, s, sizeof(s));
    lzt_core::log_write("rsbname[%p]: \"%s\" caller=+0x%lx", (void*)outObj, s,
                        (unsigned long)(callerLr - lzt_core::base()));
}

// 【诊断】0x64f20 完整观测：入参对象 + 返回值（判断它是否可直接用于读包内文件）
extern "C" void* g_origRsbReadX = nullptr;
extern "C" void lzt_rsbreadx_log(uintptr_t a0, uintptr_t ret);

extern "C" __attribute__((naked)) void hkRsbReadX() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-48]!\n"
        "str x0, [sp, #16]\n"
        "adrp x9, _g_origRsbReadX@PAGE\n"
        "add  x9, x9, _g_origRsbReadX@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "blr  x9\n"
        "str x0, [sp, #24]\n"
        "b   8f\n"
        "9:\n"
        "str xzr, [sp, #24]\n"
        "8:\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "bl   _lzt_rsbreadx_log\n"
        "ldr  x0, [sp, #24]\n"
        "ldp x29, x30, [sp], #48\n"
        "ret\n"
    );
}

extern "C" void lzt_rsbreadx_log(uintptr_t a0, uintptr_t ret) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1) >= 25) return;
    char s0[192] = {0};
    read_sstr(a0, s0, sizeof(s0));
    lzt_core::log_write("rsbreadX: a0=%p [%s] ret=0x%lx", (void*)a0, s0,
                        (unsigned long)ret);
}

// 【诊断】主动调用 0x64f20 读 packages\\LANGUAGETYPES.rton，dump 输出对象布局。
// 目的：确定「数据指针 / 长度」在输出对象中的偏移，据此实现 iOS 版 read_bytes_from_package。
extern "C" void lzt_try_read_pkg(void) {
    lzt_core::log_write("pkgread: enter base=%p ctor=%p", (void*)lzt_core::base(), (void*)oStrCtor);
    if (lzt_core::base() == 0 || oStrCtor == nullptr) return;
    static const char kName[] = "packages\\LANGUAGETYPES.rton";
    alignas(16) static uint8_t nameObj[32];
    alignas(16) static uint8_t outObj[64];
    memset(nameObj, 0, sizeof(nameObj));
    memset(outObj, 0, sizeof(outObj));
    oStrCtor(nameObj, kName);
    using ReadFn = uintptr_t (*)(uintptr_t, uintptr_t);
    const uintptr_t r = reinterpret_cast<ReadFn>(lzt_core::base() + 0x64f20)(
        reinterpret_cast<uintptr_t>(outObj), reinterpret_cast<uintptr_t>(nameObj));
    lzt_core::log_write("pkgread: name=%s ret=0x%lx", kName, (unsigned long)r);
    for (int i = 0; i < 4; ++i) {
        const uint64_t* w = reinterpret_cast<const uint64_t*>(outObj + i * 16);
        lzt_core::log_write("  out+%02d: %016llx %016llx", i * 16,
                            (unsigned long long)w[0], (unsigned long long)w[1]);
    }
    // 若 out+0 是数据指针、out+8 是长度，则尝试读前 8 字节验证
    const uintptr_t dp = *reinterpret_cast<const uint64_t*>(outObj);
    const uint64_t len = *reinterpret_cast<const uint64_t*>(outObj + 8);
    if (dp > 0x1000 && len > 0 && len < 100000 &&
        lzt_core::memory_range_accessible(dp, 8, false)) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(dp);
        lzt_core::log_write("  data: len=%llu head=%02x%02x%02x%02x", (unsigned long long)len,
                            b[0], b[1], b[2], b[3]);
    }
}

// 【诊断】按名字取资源 0x65428：记录 (资源对象, 名字, 输出) —— 目的是拿到可复用的资源对象。
// 调用约定（反汇编 0x65094 一带实证）：x0 = 资源对象（来自挂载组的 x19），
// x1 = 指向名字的指针，x2 = 输出对象。
static constexpr bool kEnableRsbFetchProbe = false;
extern "C" void* g_origRsbFetch = nullptr;
static std::atomic<uintptr_t> g_rsbResObj{0};

extern "C" __attribute__((naked)) void hkRsbFetch() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-64]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "str x2, [sp, #32]\n"
        "adrp x9, _g_origRsbFetch@PAGE\n"
        "add  x9, x9, _g_origRsbFetch@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "ldr  x2, [sp, #32]\n"
        "blr  x9\n"
        "str x0, [sp, #40]\n"
        "b   8f\n"
        "9:\n"
        "str xzr, [sp, #40]\n"
        "8:\n"
        "ldr  x0, [sp, #16]\n"
        "ldr  x1, [sp, #24]\n"
        "ldr  x2, [sp, #32]\n"
        "ldr  x3, [sp, #40]\n"
        "bl   _lzt_rsbfetch_log\n"
        "ldr  x0, [sp, #40]\n"
        "ldp x29, x30, [sp], #64\n"
        "ret\n"
    );
}

// ============================================================
// iOS 版「按名字取 RSB 记录原始字节」—— 对齐安卓 sub_16B31EC
//
// 【重大进展】经安卓侧答复（ANSWER_ANDROID_RSB_LOOKUP.md）纠正后定位成功。
// 关键认知：iOS 的 0x7eb1c0 是「名字 → 路径」解析壳，**不是**取数原语；
//   真正的取数原语是从 LoadPackage(0x2c9e04) 内部反推得到的 0x101c44df4。
//
// 调用链（反汇编实证）：
//   PVZDB::LoadPackageForTableFromRTONFile = 0x2c9e04
//     └─ 0x101aba81c(pakMgr, name, outObj, 1)      ← 取数包装（含 rsm 获取与开关检查）
//          ├─ rsm = *(pakMgr + 2248)                ← 安卓是 pakMgr+0x858(2136)，差 112
//          ├─ if (*(rsm+24) == 0) fail              ← RSB 通道开关（与安卓 +24 同偏移）
//          ├─ 0x101c44bb0(rsm, name, 1, &groupIdx)     ← 遍历 type3 组找命中组
//          └─ 0x101c44df4(rsm, groupIdx, name, &dataPtr, &dataSize)  ← ★ 取数原语
//
// ★ 必须两步串起来：不能给 0x101c44df4 传 groupIdx = -1 让它自行遍历——
//   实测必然 miss（其内部 0x101c44b20 与 0x101c44bb0 不等价）。详见实现处。
//
// 取数原语 = 0x101c44df4(rsm, groupIdx, name, &dataPtr, &dataSize) → w0=命中
//   与安卓 sub_16B31EC 逐条同构：
//     · groupIdx == -1 → 调 0x101c44b20 遍历（rsm+112 数量、rsm+120 数组、
//       组+24 == 3 过滤、偏移 += **192**）      ← 安卓步长 200，iOS 实测 192
//     · node[0] 必须 == 0 才算命中             ← 与安卓同判据
//     · dataPtr = [[组+0x38]+0x40][组+0x80]     ← 组 buffer 基址（索引 ×16）
//               + *(*(组+0x48)+0x94)            ← 组内文件偏移
//               + *(u32)(node+4)                ← node.off
//     · size    = *(u32)(node+8)                ← node.size
//
// 名字格式：radix 查找内部会把 '/' 规范化为 '\'（0x101c44d8c），
//   因此传 "packages/LANGUAGETYPES.rton" 或 "packages\LANGUAGETYPES.rton" 均可。
// ============================================================

// 取数原语（0x101c44df4）：(rsm, groupIdx, nameStr*, &dataPtr, &dataSize) → 命中标志
//
// 返回值分类（与安卓 read_bytes_from_package 的非 iOS 分支等价）：
//   Ok                        —— 取到字节
//   LibraryUnavailable        —— base / std::string 构造器未就绪
//   ResourceSystemUnavailable —— pakMgr / rsm 为空，或 RSB 挂载通道未开
//   ResourceIdNotFound        —— 资源系统已就绪，但包内无此记录（确定的「无来源」）
//   ResourceObjectUnavailable —— 记录存在但指针/大小异常
//
// 定义在 namespace lzt_resource 内，与 jni/resource_file.h 的声明匹配。
namespace lzt_resource {
RsbReadStatus lzt_ios_read_rsb_bytes(const char* rsbName, std::string& out) {
    out.clear();
    if (rsbName == nullptr || rsbName[0] == 0) return RsbReadStatus::ResourceIdNotFound;
    const uintptr_t base = lzt_core::base();
    if (base == 0 || oStrCtor == nullptr) {
        lzt_core::log_write("LRPKG FAIL P01 library/ctor unavailable base=%p ctor=%p",
                            (void*)base, reinterpret_cast<void*>(oStrCtor));
        return RsbReadStatus::LibraryUnavailable;
    }

    // pakMgr → rsm（iOS: pakMgr+2248；安卓 pakMgr+0x858）
    const uintptr_t slot = base + OFF_PersistManager;
    if (!lzt_core::memory_range_accessible(slot, 8, false)) {
        lzt_core::log_write("LRPKG FAIL P01 pakMgr slot inaccessible");
        return RsbReadStatus::ResourceSystemUnavailable;
    }
    const uintptr_t pakMgr = *reinterpret_cast<const uintptr_t*>(slot);
    if (pakMgr == 0 ||
        !lzt_core::memory_range_accessible(pakMgr + 2248, 8, false)) {
        lzt_core::log_write("LRPKG FAIL P01 pakMgr null=%d", pakMgr == 0);
        return RsbReadStatus::ResourceSystemUnavailable;
    }
    const uintptr_t rsm = *reinterpret_cast<const uintptr_t*>(pakMgr + 2248);
    if (rsm == 0) {
        lzt_core::log_write("LRPKG FAIL P02 rsm null");
        return RsbReadStatus::ResourceSystemUnavailable;
    }
    // RSB 通道开关（与安卓 sub_16AD8D0 判据一致：指针非 0 才走 RSB）
    const uintptr_t sw = *reinterpret_cast<const uintptr_t*>(rsm + 24);
    if (sw == 0) {
        lzt_core::log_write("LRPKG FAIL P02 RSB channel off");
        return RsbReadStatus::ResourceSystemUnavailable;
    }

    // 构造名字 std::string（用游戏自己的构造器，保证布局与析构规则一致）
    alignas(16) uint8_t nameObj[40];
    memset(nameObj, 0, sizeof(nameObj));
    oStrCtor(nameObj, rsbName);

    using FindRecFn = uint32_t (*)(uintptr_t, int, void*, uint64_t*, uint32_t*);
    auto findRec = reinterpret_cast<FindRecFn>(base + 0x1c44df4);

    // 两步链（对齐 0x101aba81c 的原始调用顺序）：
    //   ① 0x101c44bb0(rsm, name, 1, &grp) 先定位命中组索引
    //   ② 0x101c44df4(rsm, grp, name, &dataPtr, &dataSize) 再取数
    //
    // 【踩坑记录】曾直接传 groupIdx = -1 让 0x1c44df4 内部自行遍历，实测
    // 必然 miss：那条内部路径（0x1c44b20）与 ① 的 0x1c44bb0 并不等价，
    // 即使 rsm+112 挂载数=1648、rsm+32 mmap 基址非空也查不到记录。
    // 必须显式两步，grp 才能对上（实测 grp=280）。
    using FindGroupFn = int32_t (*)(uintptr_t, void*, int, int32_t*);
    auto findGroup = reinterpret_cast<FindGroupFn>(base + 0x1c44bb0);
    int32_t grp = -1;
    if (lzt_core::memory_range_accessible(base + 0x1c44bb0, 16, false)) {
        const int32_t gr = findGroup(rsm, reinterpret_cast<void*>(nameObj), 1, &grp);
        if (gr >= 0) grp = gr;
    }

    uint64_t dataPtr = 0;
    uint32_t dataSize = 0;
    const uint32_t hit = findRec(rsm, static_cast<int>(grp),
                                 reinterpret_cast<void*>(nameObj),
                                 &dataPtr, &dataSize);
    if (hit == 0 || dataPtr == 0 || dataSize == 0) {
        lzt_core::log_write("LRPKG P03 miss name=%s (rsm=%p)", rsbName, (void*)rsm);
        return RsbReadStatus::ResourceIdNotFound;
    }
    if (dataSize > 64u * 1024 * 1024) {
        lzt_core::log_write("LRPKG FAIL P04 size too large %u", dataSize);
        return RsbReadStatus::ResourceObjectUnavailable;
    }
    out.assign(reinterpret_cast<const char*>(dataPtr), static_cast<size_t>(dataSize));
    char prefix[25]{};
    const size_t pn = dataSize < 8 ? static_cast<size_t>(dataSize) : 8;
    for (size_t i = 0; i < pn; ++i) {
        snprintf(prefix + i * 3, sizeof(prefix) - i * 3, "%02x ",
                 static_cast<unsigned char>(out[i]));
    }
    lzt_core::log_write("LRPKG P04 READ name=%s bytes=%u prefix=%s",
                        rsbName, dataSize, prefix);
    return RsbReadStatus::Ok;
}
} // namespace lzt_resource

// ============================================================
// 数据包来源通道就绪判定（供 language_module 的 Tab 可见性判定使用）
//
// 回答「通道是否已就绪」，不回答「是否有来源」。四项全齐才返回 true：
//   ① base 非 0
//   ② oStrCtor 非空（wait_and_start 内统一初始化）
//   ③ pakMgr / rsm 非空且 RSB 挂载开关已开
//   ④ 挂载组数量 > 0（rsm+112）
// 任一未齐 → 取数必失败，但原因是「还不知道」而非「无来源」。
//
// 为什么必须有④：实测 ③ 满足时仍会 miss——rsm+24 开关已开、rsm 非空，
// 但 radix 树尚未装入记录（`LRPKG P03 miss ... rsm=0xaa69cb960`，
// 发生在启动后约 1s；同一次运行约 40s 后同一调用成功）。仅凭 ③ 判定
// 会把「包还在挂载」误判为「无来源」。
// ============================================================
// 注意：不用 extern "C"——language_module.cpp 里以 C++ 链接声明它，
// 两边必须一致，否则链接期找不到符号。
bool lzt_ios_pkg_channel_ready() {    const uintptr_t base = lzt_core::base();
    if (base == 0 || oStrCtor == nullptr) return false;
    const uintptr_t slot = base + OFF_PersistManager;
    if (!lzt_core::memory_range_accessible(slot, 8, false)) return false;
    const uintptr_t pakMgr = *reinterpret_cast<const uintptr_t*>(slot);
    if (pakMgr == 0 || !lzt_core::memory_range_accessible(pakMgr + 2248, 8, false)) {
        return false;
    }
    const uintptr_t rsm = *reinterpret_cast<const uintptr_t*>(pakMgr + 2248);
    if (rsm == 0) return false;
    if (*reinterpret_cast<const uintptr_t*>(rsm + 24) == 0) return false;
    // 挂载组数量：0 表示包尚未挂载完，此时取数必 miss
    return *reinterpret_cast<const uint32_t*>(rsm + 112) > 0;
}

// ============================================================
// 语言表异步预热：等 pakMgr/RSB 就绪后再加载一次
//
// 为什么需要轮询：wait_and_start 执行时主程序 __TEXT 已就绪，但游戏资源
// 系统尚未初始化（日志实证：`pakmgr: slot=... mgr=0x0`），此刻直接加载
// 必然失败并置入 TableLoader 的 3 秒节流。而 hook_create_tab 只在锚点
// Tab 创建时触发一次，用户若在节流窗口内进设置页，shouldCreate 会拿到
// 「通道未就绪」→ 不建 Tab → 本次运行再无第二次评估机会。
//
// ★ 必须跑在主队列：0x101c44bb0 / 0x101c44df4 内部会读写 radix 树与
//   挂载组缓存，非线程安全。曾用 dispatch_get_global_queue 在后台线程
//   轮询，实测与主线程的游戏资源加载并发时必崩（sig=11，
//   bt: +0x1c43ac0 +0x1c44ca4 +0x2c96224，即 0x101c44bb0 内部）。
//   改主队列后与游戏自身调用串行，不再有并发。
//
// 轮询而非固定延时：RSB 挂载完成时机随设备性能波动，固定延时不可靠。
// 上限 120 × 250ms = 30s，覆盖正常启动流程；超时即放弃，由 createTab
// 的 retry() 与页面重建继续评估（shouldCreate 已能区分「还不知道」与
// 「无来源」，不会把未就绪误判成无来源）。
// ============================================================
static void lzt_ios_prewarm_language_table_async(void) {
    // 【v13】启动期预热已停用。
    //
    // 停用依据（iPad 日志实证，卡死那次的顺序）：
    //   [HB] main thread STALLED ... tick=1
    //   LRPKG P03 miss name=packages\LANGUAGETYPES.rton
    //   [SFW] prewarm: channel ready at poll=1 ok=0
    // 即预热发生在主线程已卡死之后，且它去触碰了游戏资源系统。
    //
    // 与 lzt_main_ios.cpp 中 kEnableRsbFetchProbe 的停用记录同源：
    // 在【主队列】上从外部重入游戏的资源加载批次，会让游戏「随机停在 EA
    // 界面、须杀进程重启」。预热走 package-rton 时正是这一类调用。
    //
    // 而它在功能上已无必要：language_module 的启动期闸门（g_bootPhase）
    // 使启动阶段只走文件系统来源，prewarm 能做的加载文件系统通道早在
    // language_locale_bootstrap 里已做过一遍；真正的数据包来源推迟到
    // 用户进入设置页（lang_should_create / build_page 时 language_end_boot_phase
    // 解除闸门）再评估，那时主循环已稳定。
    //
    // 保留函数与开关以便需要时复验：置 true 即恢复原轮询预热行为。
    static constexpr bool kEnableBootPrewarm = false;
    if (!kEnableBootPrewarm) {
        lzt_core::log_write("[SFW] prewarm: skipped (boot prewarm disabled, "
                            "package-rton deferred to settings page)");
        return;
    }

    static constexpr int kMaxPolls = 120;
    static constexpr uint64_t kIntervalMs = 250;

    // 必须跑在主队列：0x101c44bb0 / 0x101c44df4 内部会读写 radix 树与挂载组
    // 缓存，非线程安全。曾用 dispatch_get_global_queue 在后台线程轮询，实测
    // 与主线程的游戏资源加载并发时必崩（sig=11，bt 落在 0x101c44bb0 内部）。
    // 改主队列后与游戏自身调用串行，不再有并发。
    // （language_module 侧另有 g_pkgReadMutex 串行化所有取数路径，双保险。）
    //
    // 主队列上不能 usleep 阻塞（会卡 UI），故用 dispatch_after 递归轮询。
    dispatch_async(dispatch_get_main_queue(), ^{
        __block int polls = 0;
        __block void (^tick)(void) = nullptr;
        tick = ^{
            if (lzt_ios_pkg_channel_ready()) {
                // force=true：绕过节流。constructor 那次失败置入的 3 秒
                // 节流通常仍未到期，走 ensure() 会被节流挡掉而白跑一次。
                const bool ok = language_table_prewarm(true);
                lzt_core::log_write("[SFW] prewarm: channel ready at poll=%d ok=%d",
                                    polls, ok ? 1 : 0);
                return;
            }
            ++polls;
            if (polls >= kMaxPolls) {
                lzt_core::log_write(
                    "[SFW] prewarm: channel not ready after %d polls, give up",
                    kMaxPolls);
                return;
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                         (int64_t)(kIntervalMs * NSEC_PER_MSEC)),
                           dispatch_get_main_queue(), tick);
        };
        tick();
    });
}
// 【诊断】RSM 结构 dump：按「同一游戏」原则对照安卓公式
//
// 安卓公式（jni/offsets.h:264-280）：
//   pakMgr = *(base + OFF_PAK_MANAGER)
//   rsm    = *(pakMgr + 0x858)                 ← 安卓 2136
//   rsm+112 = 挂载包数量
//   rsm+120 = 挂载包数组指针
//   命中: dataPtr = 组buffer基址 + 组内文件偏移 + node.off ; size = node.size
//
// iOS 已知：
//   0x7eb30c 读 *(base+0x27f4ed0)+2248 → 2248 - 2136 = 112
//   ← 正是本项目反复印证的「iOS 结构体偏移比安卓多 112」规律
//
// 本探针 dump iOS rsm 头部 256 字节，逐项与安卓语义比对。
// 【诊断】挂载组内文件表 dump
//
// 上轮已确证（MMAPDUMP 实测）：
//   rsbMain = *(rsm+32) = main.rsb mmap 基址（magic "1bsr" 已验）
//   mountCnt(rsm+112) = 1648，mountArr(rsm+120) = 组表指针
//   组步长 128 字节；组+0 = 名字（"AlwaysLoaded_1536"），组+24 = type(==3)
//   组+32 / 组+56 = 两个子结构指针；组+72 = rsbMain + 0x1c0a9c
//
// 本探针 dump：
//   1) 组 0/1/2 的完整 128 字节（确认步长与字段稳定性）
//   2) 组+32 与 组+56 指向的子结构（找「文件名 → (偏移,大小)」表）
//   3) 组 1516（索引表里 LANGUAGETYPES 的 v1 值）
static void rsbdump_group(int idx, uintptr_t arr) {
    const uintptr_t g = arr + (uintptr_t)idx * 128;
    lzt_core::log_write("GRPDUMP --- group[%d] @%p ---", idx, (void*)g);
    for (int r = 0; r < 128; r += 8) {
        uint64_t v; memcpy(&v, reinterpret_cast<const uint8_t*>(g) + r, 8);
        // 组名前 16 字节按 ASCII 渲染
        if (r == 0) {
            const uint8_t* s = reinterpret_cast<const uint8_t*>(g);
            char nm[24]; int li = 0;
            for (int k = 0; k < 24; ++k) {
                uint8_t c = s[k];
                if (c == 0) break;
                nm[li++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            nm[li] = 0;
            lzt_core::log_write("  grp+%03d: %016llx  name=\"%s\"", r,
                                (unsigned long long)v, nm);
        } else {
            lzt_core::log_write("  grp+%03d: %016llx", r, (unsigned long long)v);
        }
    }
}

static void rsbdump_subtable(const char* tag, uintptr_t p) {
    if (p == 0) { lzt_core::log_write("GRPDUMP %s = null", tag); return; }
    lzt_core::log_write("GRPDUMP %s @%p:", tag, (void*)p);
    for (int r = 0; r < 128; r += 8) {
        uint64_t v; memcpy(&v, reinterpret_cast<const uint8_t*>(p) + r, 8);
        lzt_core::log_write("    +%03d: %016llx", r, (unsigned long long)v);
    }
}

static void rsbdump_groups(void) {
    const uintptr_t base = lzt_core::base();
    if (base == 0) return;
    const uintptr_t pakMgr = *reinterpret_cast<const uintptr_t*>(base + 0x27f4ed0);
    if (pakMgr == 0) return;
    const uintptr_t rsm = *reinterpret_cast<const uintptr_t*>(pakMgr + 2248);
    if (rsm == 0) return;
    const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(rsm + 120);
    const uint32_t cnt = *reinterpret_cast<const uint32_t*>(rsm + 112);
    if (arr == 0 || cnt == 0) return;

    lzt_core::log_write("GRPDUMP rsm=%p arr=%p cnt=%u", (void*)rsm, (void*)arr, cnt);
    rsbdump_group(0, arr);
    rsbdump_group(1, arr);
    if (cnt > 1516) rsbdump_group(1516, arr);
    else lzt_core::log_write("GRPDUMP cnt=%u < 1516, skip", cnt);

    // 组 0 的两个子结构
    const uintptr_t g0 = arr;
    const uintptr_t p32 = *reinterpret_cast<const uintptr_t*>(g0 + 32);
    const uintptr_t p56 = *reinterpret_cast<const uintptr_t*>(g0 + 56);
    rsbdump_subtable("grp0+32", p32);
    rsbdump_subtable("grp0+56", p56);
}
// 【诊断】验证 main.rsb 的 mmap 基址 + 找 slot/挂载表
//
// 上轮 dump 已证实（RSMDUMP 实测）：
//   iOS rsm = *(pakMgr + 2248)   （安卓 0x858=2136，差 112 → 规律成立）
//   rsm+032 = 0x8bce00000        ← RSB_MAIN，疑似 main.rsb 映射基址
//   rsm+040 = 0x253000           ← main.rsb seg[0].off
//   rsm+048 = base + 0x70        ← seg[1].off
//   rsm+080 = base + 0x6d994     ← seg[3].off
//   rsm+096 = base + 0x1af610    ← seg[7].off
//   rsm+112 = 0x670 (= seg[3].val)
//
// 本探针：
//   1) 读 base 处 16 字节 → 若为 "1bsr" 则 mmap 假设成立
//   2) 读 base+0x253000 → 若为 "pgsr" 则段表映射成立
//   3) dump rsm+040..+200 结构
//   4) dump rsm+120 指向的表（安卓 ch 挂载包数组）
static void rsbdump_mmap(void) {
    const uintptr_t base = lzt_core::base();
    if (base == 0) return;
    const uintptr_t pakMgr = *reinterpret_cast<const uintptr_t*>(base + 0x27f4ed0);
    if (pakMgr == 0) return;
    const uintptr_t rsm = *reinterpret_cast<const uintptr_t*>(pakMgr + 2248);
    if (rsm == 0) return;

    const uintptr_t rsbMain = *reinterpret_cast<const uintptr_t*>(rsm + 32);
    lzt_core::log_write("MMAPDUMP rsm=%p rsbMain=%p", (void*)rsm, (void*)rsbMain);

    // 1) 验证 magic
    if (rsbMain != 0) {
        uint8_t hdr[16];
        memcpy(hdr, reinterpret_cast<const void*>(rsbMain), sizeof(hdr));
        lzt_core::log_write("MMAPDUMP rsbMain[0..15]: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x  (%c%c%c%c)",
                            hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7],
                            hdr[8], hdr[9], hdr[10], hdr[11], hdr[12], hdr[13], hdr[14], hdr[15],
                            hdr[0] >= 0x20 ? hdr[0] : '.', hdr[1] >= 0x20 ? hdr[1] : '.',
                            hdr[2] >= 0x20 ? hdr[2] : '.', hdr[3] >= 0x20 ? hdr[3] : '.');
        // 2) 段表位置
        uint8_t seg0[16];
        memcpy(seg0, reinterpret_cast<const void*>(rsbMain + 0x253000), sizeof(seg0));
        lzt_core::log_write("MMAPDUMP rsbMain+0x253000: %02x%02x%02x%02x  (%c%c%c%c)",
                            seg0[0], seg0[1], seg0[2], seg0[3],
                            seg0[0] >= 0x20 ? seg0[0] : '.', seg0[1] >= 0x20 ? seg0[1] : '.',
                            seg0[2] >= 0x20 ? seg0[2] : '.', seg0[3] >= 0x20 ? seg0[3] : '.');
    }

    // 3) rsm+040..+200
    lzt_core::log_write("MMAPDUMP --- rsm+040..+200 ---");
    for (int r = 40; r < 200; r += 8) {
        uint64_t v; memcpy(&v, reinterpret_cast<const uint8_t*>(rsm) + r, 8);
        lzt_core::log_write("  rsm+%03d: %016llx", r, (unsigned long long)v);
    }

    // 4) rsm+120 指向的表（安卓：挂载包数组）
    const uintptr_t mountArr = *reinterpret_cast<const uintptr_t*>(rsm + 120);
    const uint32_t mountCnt = *reinterpret_cast<const uint32_t*>(rsm + 112);
    lzt_core::log_write("MMAPDUMP mountCnt(rsm+112)=%u mountArr(rsm+120)=%p",
                        mountCnt, (void*)mountArr);
    if (mountArr != 0) {
        // dump 前 128 字节
        for (int r = 0; r < 128; r += 8) {
            uint64_t v; memcpy(&v, reinterpret_cast<const uint8_t*>(mountArr) + r, 8);
            lzt_core::log_write("  arr+%03d: %016llx", r, (unsigned long long)v);
        }
    }

    // 5) 安卓公式验证：组 buffer 基址 + 组内文件偏移 + node.off
    //    iOS 若组步长 200，组+24 = type==3 → 打印前几组
    const uintptr_t arr2 = *reinterpret_cast<const uintptr_t*>(rsm + 136);
    lzt_core::log_write("MMAPDUMP rsm+136 ptr=%p", (void*)arr2);
}
static void rsbdump_rsm(void) {
    const uintptr_t base = lzt_core::base();
    if (base == 0) return;
    const uintptr_t slot = base + 0x27f4ed0;
    if (!lzt_core::memory_range_accessible(slot, 8, false)) {
        lzt_core::log_write("RSMDUMP slot inaccessible");
        return;
    }
    const uintptr_t pakMgr = *reinterpret_cast<const uintptr_t*>(slot);
    lzt_core::log_write("RSMDUMP pakMgr=%p", (void*)pakMgr);
    if (pakMgr == 0) return;

    // 安卓 0x858 / iOS 2248 两处都试
    for (int variant = 0; variant < 2; ++variant) {
        const uintptr_t off = (variant == 0) ? 2248 : 0x858;
        if (!lzt_core::memory_range_accessible(pakMgr + off, 8, false)) {
            lzt_core::log_write("RSMDUMP variant off=%lu inaccessible", (unsigned long)off);
            continue;
        }
        const uintptr_t rsm = *reinterpret_cast<const uintptr_t*>(pakMgr + off);
        lzt_core::log_write("RSMDUMP variant off=%lu -> rsm=%p", (unsigned long)off, (void*)rsm);
        if (rsm == 0) continue;
        if (!lzt_core::memory_range_accessible(rsm, 256, false)) {
            lzt_core::log_write("RSMDUMP rsm head inaccessible");
            continue;
        }
        // dump 头部 256 字节
        for (int r = 0; r < 256; r += 8) {
            uint64_t v;
            memcpy(&v, reinterpret_cast<const uint8_t*>(rsm) + r, 8);
            const char* tag = "";
            // 安卓语义标注（原偏移；iOS 若整体 +112 则另标）
            if (r == 24) tag = " <-RSB_SWITCH(安卓+24)";
            else if (r == 48) tag = " <-GLOBAL_HASH_TREE(安卓+48)";
            else if (r == 112) tag = " <-MOUNT_COUNT(安卓+112)";
            else if (r == 120) tag = " <-MOUNT_ARRAY(安卓+120)";
            else if (r == 32) tag = " <-RSB_MAIN(安卓+32)";
            lzt_core::log_write("  rsm+%03d(0x%03x): %016llx%s", r, r,
                                (unsigned long long)v, tag);
        }
        // 若 rsm+112 像包数量（1~64），按安卓公式读包数组
        const uint32_t cnt = *reinterpret_cast<const uint32_t*>(rsm + 112);
        lzt_core::log_write("  rsm+112 as u32 = %u", cnt);
        if (cnt > 0 && cnt < 64) {
            const uintptr_t arr = *reinterpret_cast<const uintptr_t*>(rsm + 120);
            lzt_core::log_write("  rsm+120 as ptr = %p (count=%u)", (void*)arr, cnt);
            if (arr != 0 && lzt_core::memory_range_accessible(arr, 8, false)) {
                // 安卓：每组 200 字节，组+24 = type（只认 type==3）
                for (uint32_t g = 0; g < cnt && g < 4; ++g) {
                    const uint8_t* grp = reinterpret_cast<const uint8_t*>(arr) + (size_t)g * 200;
                    if (!lzt_core::memory_range_accessible(reinterpret_cast<uintptr_t>(grp), 64, false)) continue;
                    uint64_t g24; memcpy(&g24, grp + 24, 8);
                    lzt_core::log_write("    group[%u] addr=%p  +24=0x%llx (安卓 type)", g,
                                        (void*)grp, (unsigned long long)g24);
                    // 组前 64 字节
                    for (int r = 0; r < 64; r += 8) {
                        uint64_t v; memcpy(&v, grp + r, 8);
                        lzt_core::log_write("      grp+%03d: %016llx", r, (unsigned long long)v);
                    }
                }
            }
        }
    }
}

// 由 wait_and_start 末尾延迟调用（等 pakMgr 就绪）
//
// 【默认关闭】RSB 结构 dump —— 诊断残留，已定位完成，产品版不启用。
// 关闭原因（2026-10-20，iPad 无越狱实机）：
//   该线程启动后固定等 10 秒，然后直接解引用 pakMgr→rsm→挂载表。但游戏
//   初始化耗时随设备状态波动，10 秒是拍出来的定值，不保证结构已就绪：
//     · 实测同一偏移读到 MOUNT_COUNT=88383296 这种明显垃圾值（半初始化态）
//     · 每次 log_write 都 fprintf+fflush 同一 FILE*，与主线程并发写；
//       一旦读野指针崩溃在持锁状态，主线程会卡死在该锁上——
//       现象正是「随机卡在 EA 标志、必须杀进程重启」。
//   本函数只被这里调用，无其它依赖，关闭不影响任何功能。
static constexpr bool kEnableRsmDump = false;

extern "C" void lzt_ios_dump_rsm_deferred(void) {
    if (!kEnableRsmDump) return;
    pthread_t th;
    if (pthread_create(&th, nullptr, +[](void*) -> void* {
        for (int i = 0; i < 100; ++i) usleep(100 * 1000);   // 10s
        rsbdump_rsm();
        rsbdump_mmap();
        rsbdump_groups();
        return nullptr;
    }, nullptr) == 0) {
        pthread_detach(th);
    }
}
// ============================================================
// 【诊断】0x7eb1c0 查找入口探针（阶段：RSB 数据包来源）
//
// 目的：dump (x1=名字, x2=输出对象) 与返回值，确定「按名字查 RSB」的
//       输出结构 —— 这是 iOS 版 read_bytes_from_package 的最后一块。
//
// 与安卓的对应关系（FINDINGS.md §16.1 / 安卓 jni/offsets.h:264 注释）：
//   安卓  sub_16B31EC(rsm, -1, nameStr, &dataPtr, &dataSize)
//           rsm = *(pakMgr+0x858)，pakMgr = *(base+OFF_PAK_MANAGER)
//   iOS   0x7eb1c0(x0=管理器, x1=名字, x2=输出)
//           内部 0x7eb280 查主包 *(base+0x27f4ed0)
//                0x7eb30c 查挂载组 *(base+0x27f4ed0)+2248
//   两者同构：全局槽 → 资源管理器 → 按名字查 → 出 (指针, 长度)。
//
// 参考调用序列（0x8106e8 实证）：
//   0x7f6634 取管理器 → x2=&栈上输出 → bl 0x7eb1c0 → cbz w0 跳过
//   → 命中后同一输出对象传给 0x2c9e04(LoadPackageForTableFromRTONFile)
//
// 安全：只读 dump，不改行为；限次；只 dump 返回非 0 的调用。
//       解引用收紧在已实证安全区间（吸取 §17 假指针崩溃教训）。
// ============================================================
static constexpr bool kEnableResLookupProbe = false;
extern "C" void* g_origResLookup = nullptr;

extern "C" __attribute__((naked)) void hkResLookup() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-64]!\n"
        "str x0, [sp, #16]\n"
        "str x1, [sp, #24]\n"
        "str x2, [sp, #32]\n"
        "adrp x9, _g_origResLookup@PAGE\n"
        "add  x9, x9, _g_origResLookup@PAGEOFF\n"
        "ldr  x9, [x9]\n"
        "cbz  x9, 9f\n"
        "ldr x0, [sp, #16]\n"
        "ldr x1, [sp, #24]\n"
        "ldr x2, [sp, #32]\n"
        "blr x9\n"
        "str x0, [sp, #40]\n"
        "b   8f\n"
        "9:\n"
        "str xzr, [sp, #40]\n"
        "8:\n"
        "ldr x0, [sp, #16]\n"
        "ldr x1, [sp, #24]\n"
        "ldr x2, [sp, #32]\n"
        "ldr x3, [sp, #40]\n"
        "bl   _lzt_reslookup_log\n"
        "ldr x0, [sp, #40]\n"
        "ldp x29, x30, [sp], #64\n"
        "ret\n"
    );
}

// 只 dump 落在安全区间的指针内容（0x30xx_xxxx_xxx = ObjC 堆，0x1xx_xxxx_xxx = 主程序 __TEXT）
static void reslookup_deref(const char* what, uint64_t v) {
    const bool inObjcHeap = (v >= 0x1000000000ULL && v < 0x4000000000ULL);
    const bool inMainText = (v >= 0x100000000ULL && v < 0x120000000ULL);
    if (!(inObjcHeap || inMainText)) return;
    if (!lzt_core::memory_range_accessible(v, 16, false)) return;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(v);
    lzt_core::log_write("    %s -> 0x%llx: %02x%02x%02x%02x %02x%02x%02x%02x",
                        what, (unsigned long long)v,
                        d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}

extern "C" void lzt_reslookup_log(uintptr_t mgr, uintptr_t nameObj, uintptr_t outObj,
                                   uintptr_t ret) {
    static std::atomic<int> s_n{0};
    const int n = s_n.fetch_add(1);
    if (n >= 30) return;

    char name[192] = {0};
    read_sstr(nameObj, name, sizeof(name));
    lzt_core::log_write("reslookup #%d: mgr=%p name=\"%s\" out=%p ret=0x%lx",
                        n + 1, (void*)mgr, name, (void*)outObj, (unsigned long)ret);

    // dump 输出对象前 64 字节（按 8 字节一行）
    if (outObj != 0 && lzt_core::memory_range_accessible(outObj, 64, false)) {
        const uint8_t* o = reinterpret_cast<const uint8_t*>(outObj);
        for (int r = 0; r < 64; r += 8) {
            uint64_t v;
            memcpy(&v, o + r, 8);
            lzt_core::log_write("    out+%02d: %016llx", r, (unsigned long long)v);
        }
        // 对前 4 个 qword 若是安全区间指针则尝试解引用看内容
        for (int r = 0; r < 32; r += 8) {
            uint64_t v;
            memcpy(&v, o + r, 8);
            char tag[32];
            snprintf(tag, sizeof(tag), "out+%d", r);
            reslookup_deref(tag, v);
        }
    }

    // 返回值也 dump（可能是记录对象，如 0x65428 的 +56 形态）
    if (ret != 0 && lzt_core::memory_range_accessible(ret, 32, false)) {
        const uint8_t* o = reinterpret_cast<const uint8_t*>(ret);
        uint64_t a, b, c, d;
        memcpy(&a, o, 8); memcpy(&b, o + 8, 8);
        memcpy(&c, o + 16, 8); memcpy(&d, o + 24, 8);
        lzt_core::log_write("    ret+00: %016llx %016llx %016llx %016llx",
                            (unsigned long long)a, (unsigned long long)b,
                            (unsigned long long)c, (unsigned long long)d);
    }
}
// 【诊断】0x65428 出口 dump：记录对象结构（阶段：RSB 数据包来源）
//
// 目的：确定 1336 字节「记录对象」中哪两个偏移是 (数据指针, 长度)，
//       据此实现 iOS 版 read_bytes_from_package。
//
// 背景（反汇编实证，见 FINDINGS.md §16.1）：
//   0x65428 命中后经 0x67f70 → operator new(1336) 构造记录对象，
//   末尾 `add x0, x0, #56` 后返回，即返回值 = 记录对象 + 56。
//   因此 dump 时须先减 56 回到对象起点，再看 +56 附近的数据指针。
//
// 安全：只读 dump，不改行为；限次（前 6 次命中）；只对返回非 0 的调用 dump。
// ============================================================
static void rsbdump_record(const char* name, uintptr_t ret) {
    if (ret < 0x1000) return;
    // 0x65428 返回「记录对象+56」，回到对象起点
    const uintptr_t obj = ret - 56;
    if (!lzt_core::memory_range_accessible(obj, 1336, false)) {
        lzt_core::log_write("rsbdump: name=\"%s\" ret=0x%lx obj=0x%lx inaccessible",
                            name, (unsigned long)ret, (unsigned long)obj);
        return;
    }
    lzt_core::log_write("rsbdump: name=\"%s\" ret=0x%lx obj=0x%lx", name,
                        (unsigned long)ret, (unsigned long)obj);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(obj);
    // 对象前 160 字节，按 8 字节一行，同时标注「像指针/像长度」的候选
    for (int r = 0; r < 1336; r += 8) {
        uint64_t v;
        memcpy(&v, b + r, 8);
        const char* hint = "";
        if (v > 0x1000 && v < 0x100000000000ULL) hint = " <-ptr?";
        else if (v > 0 && v < 0x10000000ULL) hint = " <-len?";
        lzt_core::log_write("  obj+%03d(0x%03x): %016llx%s", r, r,
                            (unsigned long long)v, hint);
    }
    // 重点：+56 附近（原函数 add x0,x0,#56 指向处）前后各 32 字节
    lzt_core::log_write("  --- around +56 ---");
    for (int r = 40; r < 96; r += 8) {
        uint64_t v;
        memcpy(&v, b + r, 8);
        lzt_core::log_write("  obj+%03d: %016llx", r, (unsigned long long)v);
    }
    // 若某偏移是堆指针，尝试读其前 16 字节（可能是 RTON 数据头）。
    // ⚠️ 安全（本轮实测教训）：绝不能用「v < 0x100000000000」这类宽泛上界 + vm_region_64
    //    判断可读性 —— 记录对象里存在形如 0x2ea194200 的「假指针」（高位看似合法），
    //    vm_region_64 对它返回成功而实际读取 SIGSEGV（本轮崩溃 addr=0x2ba194200，
    //    stage=0，栈落在 CDN 区，正是 dump 内解引用所致）。
    //    因此只接受本项目已反复实证的安全区间：Objective-C 堆 0x3000_0000_0000 起
    //    （本轮所有真实指针均为 0x30xx_xxxx_xxx 形态）。
    for (int r = 0; r < 1336; r += 8) {
        uint64_t v;
        memcpy(&v, b + r, 8);
        const bool inObjcHeap = (v >= 0x1000000000ULL && v < 0x4000000000ULL);
        const bool inMainText = (v >= 0x100000000ULL && v < 0x120000000ULL);
        if ((inObjcHeap || inMainText) &&
            lzt_core::memory_range_accessible(v, 16, false)) {
            const uint8_t* d = reinterpret_cast<const uint8_t*>(v);
            lzt_core::log_write("  deref obj+%d -> 0x%llx: %02x%02x%02x%02x %02x%02x%02x%02x",
                                r, (unsigned long long)v,
                                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        }
    }
}

extern "C" void lzt_rsbfetch_log(uintptr_t resObj, uintptr_t namePtr, uintptr_t outObj,
                                   uintptr_t ret) {
    // 保存首个资源对象（供后续复用）
    if (resObj != 0) {
        uintptr_t expected = 0;
        g_rsbResObj.compare_exchange_strong(expected, resObj, std::memory_order_release);
    }
    static std::atomic<int> s_n{0};
    const int n = s_n.fetch_add(1);
    if (n >= 40) return;
    char s[160] = {0};
    const uintptr_t sp = *reinterpret_cast<const uintptr_t*>(namePtr);
    read_sstr(sp, s, sizeof(s));
    lzt_core::log_write("rsbfetch: res=%p name=\"%s\" out=%p ret=0x%lx",
                        (void*)resObj, s, (void*)outObj, (unsigned long)ret);
    // 只 dump 前 6 次命中（3699 次调用中前几次即覆盖首批资源）
    if (ret != 0 && n < 6) rsbdump_record(s, ret);
}


// 【诊断】用抓到的资源对象主动读 RSB 资源。注意：游戏按【资源 id】读取，不是文件名；
//   LANGUAGETYPES 的资源 id 为 RESFILE_PACKAGES_LANGUAGETYPES（main.rsb 索引表条目名）。
//   此前传文件名 packages\LANGUAGETYPES.rton 返回 0（实测）。
extern "C" void lzt_try_fetch_pkg(void) {
    const uintptr_t res = g_rsbResObj.load(std::memory_order_acquire);
    if (res == 0 || oStrCtor == nullptr || lzt_core::base() == 0) {
        lzt_core::log_write("tryfetch: 前置不满足 res=%p ctor=%p", (void*)res, (void*)oStrCtor);
        return;
    }
    static const char kName[] = "RESFILE_PACKAGES_LANGUAGETYPES";
    alignas(16) static uint8_t nameObj[32];
    alignas(16) static uint8_t namePtrSlot[16];
    alignas(16) static uint8_t outObj[64];
    memset(nameObj, 0, sizeof(nameObj));
    memset(outObj, 0, sizeof(outObj));
    oStrCtor(nameObj, kName);
    *reinterpret_cast<uintptr_t*>(namePtrSlot) = reinterpret_cast<uintptr_t>(nameObj);
    using FetchFn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
    lzt_core::log_write("tryfetch: res=%p name=%s", (void*)res, kName);
    const uintptr_t r = reinterpret_cast<FetchFn>(lzt_core::base() + 0x65428)(
        res, reinterpret_cast<uintptr_t>(namePtrSlot), reinterpret_cast<uintptr_t>(outObj));
    lzt_core::log_write("tryfetch: ret=0x%lx", (unsigned long)r);
    for (int i = 0; i < 4; ++i) {
        const uint64_t* w = reinterpret_cast<const uint64_t*>(outObj + i * 16);
        lzt_core::log_write("  out+%02d: %016llx %016llx", i * 16,
                            (unsigned long long)w[0], (unsigned long long)w[1]);
    }
    if (r > 0x1000 && lzt_core::memory_range_accessible(r, 16, false)) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(r);
        lzt_core::log_write("  ret head: %02x%02x%02x%02x %02x%02x%02x%02x",
                            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
}

// ============================================================
// 字形渲染缓存：强制 2048（移植自 Android components/glyph_cache_size）
//
// iOS 侧构建点在 0x1cb7d38：读屏幕宽高（+1972/+1976）取最小边，然后
//   0x1cb7d98  cmp  w8, #1024
//   0x1cb7da0  mov  w10, #2048
//   0x1cb7da8  cmp  w8, #641
//   0x1cb7db0  csel w23, w8, w9, lt      ← 打这里
//   0x1cb7e0c  str  w23, [x21, #164]     ← 缓存对象 +0xA4
// 把 0x1cb7db0 改成 `mov w23, #2048`，所有设备统一 2048，不动选择逻辑。
// 注：该指令之后 w23 只用于写 +0xA4，无其它读者，替换安全。
// ============================================================
static constexpr uintptr_t OFF_GlyphSizeSelect = 0x1cb7db0; // csel 处
static constexpr uintptr_t OFF_EATextGlobal    = 0x27fdab0; // 全局 EAText*
static constexpr uint32_t  kGlyphInsnCsel = 0x1a89b117u;    // csel w23, w8, w9, lt
static constexpr uint32_t  kGlyphInsnMov  = 0x52810017u;    // mov  w23, #2048
static int g_glyph2048_state = 0;   // 0=未装 1=已生效 2=字节不符

// ---- 图集页上限：1 → 4 ----
// 构建点 0x1cb7e1c 起：
//   0x1cb7e14  mov w8, #1
//   0x1cb7e18  str w8, [x21, #312]
//   0x1cb7e1c  mov w1, #1          ← 页数上限（缓存 +0xA0）
//   0x1cb7e20  mov w2, #1          ← 初始分配页数
//   0x1cb7e24  bl  0x101c3f384     ; SetPageLimit(limit=w1, initial=w2)
// 上限提到 4 后，引擎在页满时会自己再开一页，而不是走到
// 「no place to put this glyph」→ 字形渲染为空白。
static constexpr uintptr_t OFF_GlyphPageLimit = 0x1cb7e1c;
static constexpr uint32_t  kInsnLimit1 = 0x52800021u;   // mov w1, #1
static constexpr uint32_t  kInsnLimit4 = 0x52800081u;   // mov w1, #4
static int g_pagelimit_state = 0;

static void lzt_ios_glyph_pagelimit_apply(const char* why) {
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    uintptr_t site = base + OFF_GlyphPageLimit;
    uint32_t cur = *reinterpret_cast<const uint32_t*>(site);
    if (cur == kInsnLimit4) {
        if (g_pagelimit_state != 1) {
            g_pagelimit_state = 1;
            lzt_core::log_write("[CMP] glyph page limit -> 4 (%s)", why);
        }
        return;
    }
    if (cur != kInsnLimit1) {
        if (g_pagelimit_state != 2) {
            g_pagelimit_state = 2;
            lzt_core::log_write("[CMP] page limit patch: unexpected insn 0x%08x", cur);
        }
        return;
    }
    if (!ios_make_private_writable(site, 4)) return;
    *reinterpret_cast<uint32_t*>(site) = kInsnLimit4;
    __builtin___clear_cache(reinterpret_cast<char*>(site),
                            reinterpret_cast<char*>(site) + 4);
    ios_restore_executable(site, 4);
    g_pagelimit_state = 1;
    lzt_core::log_write("[CMP] glyph page limit -> 4 (%s)", why);
}

static void lzt_ios_glyph_cache_2048_apply(const char* why) {
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    uintptr_t site = base + OFF_GlyphSizeSelect;
    uint32_t cur = *reinterpret_cast<const uint32_t*>(site);
    if (cur == kGlyphInsnMov) {
        if (g_glyph2048_state != 1) {
            g_glyph2048_state = 1;
            lzt_core::log_write("[CMP] glyph cache forced to 2048 (%s)", why);
        }
        return;
    }
    if (cur != kGlyphInsnCsel) {
        if (g_glyph2048_state != 2) {
            g_glyph2048_state = 2;
            lzt_core::log_write("[CMP] glyph cache patch: unexpected insn 0x%08x @+0x%lx",
                                cur, (unsigned long)OFF_GlyphSizeSelect);
        }
        return;
    }
    if (!ios_make_private_writable(site, 4)) {
        lzt_core::log_write("[CMP] glyph cache patch: mprotect FAILED");
        return;
    }
    *reinterpret_cast<uint32_t*>(site) = kGlyphInsnMov;
    __builtin___clear_cache(reinterpret_cast<char*>(site),
                            reinterpret_cast<char*>(site) + 4);
    ios_restore_executable(site, 4);
    g_glyph2048_state = 1;
    lzt_core::log_write("[CMP] glyph cache forced to 2048 (%s)", why);
}

static inline uintptr_t lzt_strip_data_ptr(uintptr_t p) {
    return p & 0x0000FFFFFFFFFFFFULL;   // arm64e：去掉 ptrauth 顶部标签
}

// 运行期校验：直接读回缓存对象 +0xA4 / +0xA0 / +0x1A0，确认实际生效值。
static void lzt_ios_glyph_cache_report() {
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    uintptr_t g = lzt_strip_data_ptr(*reinterpret_cast<uintptr_t*>(base + OFF_EATextGlobal));
    if (g < 0x100000000ULL || g > 0x100000000000ULL) {
        lzt_core::log_write("[CMP] glyph report: EAText global not ready (g=%p)",
                            reinterpret_cast<void*>(g));
        return;
    }
    uintptr_t cache = lzt_strip_data_ptr(*reinterpret_cast<uintptr_t*>(g + 8));
    if (cache < 0x100000000ULL || cache > 0x100000000000ULL) {
        lzt_core::log_write("[CMP] glyph report: cache not ready (cache=%p)",
                            reinterpret_cast<void*>(cache));
        return;
    }
    int size  = *reinterpret_cast<int*>(cache + 0xA4);
    int limit = *reinterpret_cast<int*>(cache + 0xA0);
    int pages = *reinterpret_cast<int*>(cache + 0x1A0);
    lzt_core::log_write("[CMP] glyph cache: size=%d pageLimit=%d pages=%d cache=%p",
                        size, limit, pages, reinterpret_cast<void*>(cache));
}

// ============================================================
// 组件补丁二：图鉴切换时主动刷新字形缓存
// （移植自 Android components/glyph_cache_flush）
//
// iOS 入口：AlmanacWidget::ButtonDepress @0x3c4c04
//   - 对 w1 做 [-1..5] 跳转表分发：-1=关闭，0/1/2=植物/僵尸/升级，3..5=其它按钮
//   - 与 Android 的 sub_86EC40(a1, a2) 语义一致
//
// 刷新 = 销毁旧缓存 + 用真实 app 对象重建：
//   销毁  0x1cba198(cache)          deleting dtor，释放两个容器并 free 对象
//   重建  0x1cb7d38(eatext, app)    首次调用时截获真实 app 指针
//   EAText 全局 @0x27fdab0，+8 = 缓存实例
// ============================================================
static constexpr uintptr_t OFF_AlmanacDepress  = 0x3c4c04;
static constexpr uintptr_t OFF_ClearPrimeGlyphCache = 0x1cb8420;   // PrimeText::ClearPrimeGlyphCache
// AlmanacWidget 的 Update/Draw（vtable 第 34 项）。只在图鉴存在时被调用，
// 因此可以安全地用它作为「图鉴打开中」的信号。
static constexpr uintptr_t OFF_AlmanacUpdate = 0x3c44a8;
// 图鉴翻页渲染的两个阶段（文档实证：case 0 / case 1）。这两个函数才是
// 「点卡槽 / 按左右翻页」时真正重建文本的地方，入口只用到 x0。
static constexpr uintptr_t OFF_AlmanacPageCase0 = 0x3c5330;
static constexpr uintptr_t OFF_AlmanacPageCase1 = 0x3c55a4;
static constexpr uintptr_t OFF_AlmanacVtable   = 0x21cdb10;

extern "C" int lzt_ios_glyph_flush_enabled();   // 定义在 lzt_core_ios.mm（默认关）
static constexpr uintptr_t OFF_GlyphCacheInit  = 0x1cb7d38;
static constexpr uintptr_t OFF_GlyphCacheDtor  = 0x1cba198;

extern "C" int lzt_ios_glyph_selftest_requested();   // 定义在 lzt_core_ios.mm

typedef void (*GlyphInitFn)(uintptr_t, uintptr_t);
typedef void (*DepressFn)(uintptr_t, int);

static GlyphInitFn g_origGlyphInit = nullptr;
static DepressFn   g_origDepress   = nullptr;
static uintptr_t   g_appObj        = 0;   // 真实 app/renderer 对象（重建缓存必需）
static int         g_flushCount    = 0;

static void hk_glyph_init(uintptr_t eatext, uintptr_t app) {
    if (app) g_appObj = app;
    if (g_origGlyphInit) g_origGlyphInit(eatext, app);
}

static void lzt_ios_glyph_flush(int id) {
    (void)id;
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    uintptr_t g = lzt_strip_data_ptr(*reinterpret_cast<uintptr_t*>(base + OFF_EATextGlobal));
    if (g < 0x100000000ULL || g > 0x100000000000ULL) return;

    // 直接调用引擎自己的 Sexy::PrimeText::ClearPrimeGlyphCache(g)。
    // 依据（与 B 站专栏一致）：
    //   0x100606fbc  bl GetInstance()      → x0 = PrimeText*
    //   0x100606fc0  bl 0x101cb8420        → ClearPrimeGlyphCache(x0)
    // 内部行为：重置页分配(+0x1A0=0)、摘除字形条目、推进字体代次。
    // 这是引擎关闭图鉴时自己走的路径，安全且语义正确。
    auto clear = reinterpret_cast<void (*)(uintptr_t)>(base + OFF_ClearPrimeGlyphCache);
    clear(g);
    ++g_flushCount;
}

// 翻页入口 hook 状态（供关闭时统计用，定义提前）
static int       g_pageClearCount = 0;
static int       g_pageHooksOk    = 0;
static uintptr_t g_origPageCase0  = 0;
static uintptr_t g_origPageCase1  = 0;

static void hk_almanac_depress(uintptr_t self, int id) {
    if (!lzt_bootstrap_ready()) {
        if (g_origDepress) g_origDepress(self, id);
        return;
    }
    uintptr_t base = lzt_core::base();
    if (base) {
        uintptr_t vt = lzt_strip_data_ptr(*reinterpret_cast<uintptr_t*>(self));
        // Tab 切换时也清一次（翻卡片不经过这里，由 Update 钩子兜底）
        if (vt == base + OFF_AlmanacVtable && lzt_ios_glyph_flush_enabled())
            lzt_ios_glyph_flush(id);
        // 关闭图鉴时输出一次统计（每会话一行，便于确认翻页 hook 是否在工作）
        if (vt == base + OFF_AlmanacVtable && id == -1 && g_pageClearCount > 0) {
            lzt_core::log_write("[CMP] almanac closed: page clears=%d", g_pageClearCount);
            g_pageClearCount = 0;
        }
    }
    if (g_origDepress) g_origDepress(self, id);
}

static uintptr_t g_origAlmanacUpdate = 0;
static double    g_lastAlmanacClear  = 0.0;

// ---- 翻页入口 pre-hook ----

static void hk_page_case0(uintptr_t self) {
    if (lzt_bootstrap_ready() && lzt_ios_glyph_flush_enabled()) {
        lzt_ios_glyph_flush(-2);
        ++g_pageClearCount;
    }
    if (g_origPageCase0)
        reinterpret_cast<void (*)(uintptr_t)>(g_origPageCase0)(self);
}

static void hk_page_case1(uintptr_t self) {
    if (lzt_bootstrap_ready() && lzt_ios_glyph_flush_enabled()) {
        lzt_ios_glyph_flush(-3);
        ++g_pageClearCount;
    }
    if (g_origPageCase1)
        reinterpret_cast<void (*)(uintptr_t)>(g_origPageCase1)(self);
}

static double lzt_mono_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void hk_almanac_update(uintptr_t self) {
    // 回退机制：翻页入口 hook 失效时，靠它兜底。
    // 主路径装好后间隔放宽到 5s，仅作保险；否则保持 1.5s。
    const double interval = g_pageHooksOk ? 5.0 : 1.5;
    const double now = lzt_mono_seconds();
    if (g_origAlmanacUpdate && now - g_lastAlmanacClear >= interval) {
        g_lastAlmanacClear = now;
        lzt_ios_glyph_flush(-1);
    }
    if (g_origAlmanacUpdate) {
        reinterpret_cast<void (*)(uintptr_t)>(g_origAlmanacUpdate)(self);
    }
}

static void lzt_ios_install_glyph_flush() {
    uintptr_t base = lzt_core::base();
    if (base == 0 || g_origDepress != nullptr) return;
#if defined(LZT_AB_NO_ALMANAC)
    lzt_core::log_write("[AB] almanac slots remain empty; glyph hooks not installed");
    return;
#endif

    void* orig = nullptr;
    // 不再需要 app 对象：刷新改为原地重置，不重建实例。
    // （保留 hk_glyph_init 以便日后需要时直接启用）
    orig = nullptr;
    if (lzt_hooks::install(base + OFF_AlmanacDepress,
                           reinterpret_cast<void*>(&hk_almanac_depress), &orig, nullptr)) {
        g_origDepress = reinterpret_cast<DepressFn>(orig);
        lzt_core::log_write("[CMP] almanac depress hook installed @+0x3c4c04");
    } else {
        lzt_core::log_write("[CMP] almanac depress hook FAILED");
    }

    // 图鉴 Update/Draw：图鉴存在期间按秒清理字形缓存（主要修复手段）
    orig = nullptr;
    if (lzt_hooks::install(base + OFF_AlmanacUpdate,
                           reinterpret_cast<void*>(&hk_almanac_update), &orig, nullptr)) {
        g_origAlmanacUpdate = reinterpret_cast<uintptr_t>(orig);
        lzt_core::log_write("[CMP] almanac update hook installed @+0x3c44a8");
    } else {
        lzt_core::log_write("[CMP] almanac update hook FAILED");
    }

    // 翻页入口：真正按卡片切换清理缓存
    orig = nullptr;
    int ok = 0;
    if (lzt_hooks::install(base + OFF_AlmanacPageCase0,
                           reinterpret_cast<void*>(&hk_page_case0), &orig, nullptr)) {
        g_origPageCase0 = reinterpret_cast<uintptr_t>(orig);
        ++ok;
    }
    orig = nullptr;
    if (lzt_hooks::install(base + OFF_AlmanacPageCase1,
                           reinterpret_cast<void*>(&hk_page_case1), &orig, nullptr)) {
        g_origPageCase1 = reinterpret_cast<uintptr_t>(orig);
        ++ok;
    }
    g_pageHooksOk = (ok == 2);
    lzt_core::log_write("[CMP] almanac page hooks %s (%d/2) @+0x3c5330,+0x3c55a4",
                        g_pageHooksOk ? "installed" : "PARTIAL", ok);
}

// ============================================================
// 组件补丁：戴夫待机动画 / 传送带递进计数
//
// 依据 cave1_戴夫+摄像机+传送带补丁.md（同一 iOS 二进制的地址）。
// 文档给的是「静态改字节 + 固定代码洞穴」；这里在运行时用同样的方式写入，
// 但机器码全部按当前基址重新编码，避免照抄文档里可能过期的字节。
//
//   戴夫：0x1a7440 处把「NPC+0x100 的字符串指针」换成字面量 "anim_%s_idle"
//   传送带：0x31585c 跳到洞穴，补上「场上植物计数」，并 NOP 0x31582c 的跳过分支
// ============================================================
static uint32_t enc_b(uintptr_t from, uintptr_t to) {
    int64_t off = static_cast<int64_t>(to - from) >> 2;
    return 0x14000000u | (static_cast<uint32_t>(off) & 0x03FFFFFFu);
}
static uint32_t enc_bl(uintptr_t from, uintptr_t to) {
    int64_t off = static_cast<int64_t>(to - from) >> 2;
    return 0x94000000u | (static_cast<uint32_t>(off) & 0x03FFFFFFu);
}
static uint32_t enc_adrp(int rd, uintptr_t pc, uintptr_t target) {
    int64_t imm = (static_cast<int64_t>(target & ~0xFFFULL)
                   - static_cast<int64_t>(pc & ~0xFFFULL)) >> 12;
    return 0x90000000u
         | ((static_cast<uint32_t>(imm) & 3u) << 29)
         | (((static_cast<uint32_t>(imm >> 2)) & 0x7FFFFu) << 5)
         | static_cast<uint32_t>(rd);
}
static uint32_t enc_add_imm(int rd, int rn, uint32_t imm) {
    return 0x91000000u | ((imm & 0xFFFu) << 10)
         | (static_cast<uint32_t>(rn) << 5) | static_cast<uint32_t>(rd);
}
static uint32_t enc_ldr_x(int rt, int rn, uint32_t off) {
    return 0xF9400000u | (((off / 8) & 0xFFFu) << 10)
         | (static_cast<uint32_t>(rn) << 5) | static_cast<uint32_t>(rt);
}

static bool patch_words(uintptr_t addr, const uint32_t* w, int n) {
    const size_t bytes = static_cast<size_t>(n) * 4;
    if (!ios_make_private_writable(addr, bytes)) return false;
    memcpy(reinterpret_cast<void*>(addr), w, bytes);
    __builtin___clear_cache(reinterpret_cast<char*>(addr),
                            reinterpret_cast<char*>(addr) + bytes);
    ios_restore_executable(addr, bytes);
    return true;
}

static int g_dave_state = 0;   // 0 未装 / 1 已装 / 2 字节不符
static int g_conv_state = 0;

static void lzt_ios_apply_dave_patch() {
    uintptr_t base = lzt_core::base();
    if (base == 0 || g_dave_state != 0) return;
    uintptr_t site = base + 0x1a7440;
    const uint32_t orig[4] = {0x910402a0u, 0x39c45ea8u, 0x36f80048u, 0xf9400000u};
    /* 【v46】已改为打包期静态补丁：若入口已是我们的 adrp/add/b/nop 形态，
     * 说明补丁在包里，运行期无需再写（STATIC_ONLY 也写不了）。 */
    const uint32_t* cur4 = reinterpret_cast<const uint32_t*>(site);
    if ((cur4[0] & 0x9F000000u) == 0x90000000u && (cur4[2] & 0xFC000000u) == 0x14000000u) {
        g_dave_state = 1;
        lzt_core::log_write("[CMP] dave idle-anim patch: already static @+0x1a7440 "
                            "(adrp=%08x add=%08x b=%08x nop=%08x)",
                            cur4[0], cur4[1], cur4[2], cur4[3]);
        return;
    }
    if (memcmp(reinterpret_cast<const void*>(site), orig, sizeof(orig)) != 0) {
        g_dave_state = 2;
        lzt_core::log_write("[CMP] dave patch: unexpected bytes, skipped");
        return;
    }
    uintptr_t str = base + 0x18bea44;          // 洞穴区空闲字符串槽
    char idle[16] = "anim_%s_idle";            // 13 字符 + NUL，余位补零
    lzt_core::log_write("[CMP] dave patch: runtime path (str slot @+0x18bea44)");
    if (!patch_words(str, reinterpret_cast<const uint32_t*>(idle), 4)) return;

    uint32_t p[4] = {
        enc_adrp(0, site, str),
        enc_add_imm(0, 0, static_cast<uint32_t>(str & 0xFFFu)),
        enc_b(site + 8, site + 0x10),          // 跳过原条件判断
        0xd503201fu,                           // NOP（原 LDR X0,[X0]）
    };
    if (!patch_words(site, p, 4)) return;
    g_dave_state = 1;
    lzt_core::log_write("[CMP] dave idle-anim patch applied @+0x1a7440");
}

static void lzt_ios_apply_conveyor_patch() {
    uintptr_t base = lzt_core::base();
    if (base == 0 || g_conv_state != 0) return;
    uintptr_t e1 = base + 0x31582cu;   // cbz x23, 0x315864
    uintptr_t e2 = base + 0x31585cu;   // mov x0, sp
    uintptr_t cave = base + 0x18bea1cu;

    /* 【v46】静态补丁检测：e2 已变成 B 跳洞穴、e1 已变成 NOP 即视为已打。 */
    {
        const uint32_t* q1 = reinterpret_cast<const uint32_t*>(e1);
        const uint32_t* q2 = reinterpret_cast<const uint32_t*>(e2);
        if (*q1 == 0xd503201fu && (*q2 & 0xFC000000u) == 0x14000000u) {
            g_conv_state = 1;
            lzt_core::log_write("[CMP] conveyor count patch: already static "
                                "@+0x31585c (b=%08x) +0x31582c (nop) cave[0]=%08x",
                                *q2, *reinterpret_cast<const uint32_t*>(cave));
            return;
        }
    }

    if (*reinterpret_cast<const uint32_t*>(e1) != 0xb40001d7u
        || *reinterpret_cast<const uint32_t*>(e2) != 0x910003e0u) {
        g_conv_state = 2;
        lzt_core::log_write("[CMP] conveyor patch: unexpected bytes, skipped");
        return;
    }

    uint32_t c[10];
    c[0] = 0x910003e0u;                                   // mov x0, sp
    c[1] = enc_bl(cave + 4,  base + 0x1c81b20);           // 析构 v24
    c[2] = enc_adrp(8, cave + 8, base + 0x27eb870);       // adrp x8, 全局页
    c[3] = enc_ldr_x(8, 8, 0x870);                        // ldr x8,[x8,#0x870]
    c[4] = enc_ldr_x(0, 8, 0xa18);                        // ldr x0,[x8,#0xa18] board
    c[5] = enc_ldr_x(8, 19, 0x2c8);                       // ldr x8,[x19,#0x2c8] pool
    c[6] = 0x9B000000u | (24u << 16) | (8u << 10) | (21u << 5) | 1u;  // madd x1,x21,x24,x8
    c[7] = enc_bl(cave + 28, base + 0x19427c);            // 场上植物计数
    c[8] = 0x0B000000u | (22u << 5) | 22u;                // add w22,w22,w0
    c[9] = enc_b(cave + 36, base + 0x315864);             // 回原流程
    if (!patch_words(cave, c, 10)) return;

    uint32_t br = enc_b(e2, cave);
    if (!patch_words(e2, &br, 1)) return;
    uint32_t nop = 0xd503201fu;
    patch_words(e1, &nop, 1);
    g_conv_state = 1;
    lzt_core::log_write("[CMP] conveyor count patch applied @+0x31585c");
}

static void lzt_schedule_bootstrap_async(void);

static void wait_and_start() {
#define LZT_STAGE(name) do { lzt_set_stage(name); lzt_core::log_write("[STAGE] %s", name); } while (0)

/* 计时打点：记录每段耗时，定位主队列被占用最久的地方。 */
    const double t_enter = lzt_ms_now();
    double t_prev = t_enter;
#define LZT_TICK(name) do { \
        const double _t = lzt_ms_now(); \
        lzt_core::log_write("[COST] %-38s %7.1f ms (cum %7.1f)", name, _t - t_prev, _t - t_enter); \
        t_prev = _t; \
    } while (0)

    LZT_STAGE("begin wait main image");
    {
        /* 主队列上不能长阻塞：主镜像通常在 constructor 返回时已就绪，
         * 这里只做短促重试（最多 2s），避免旧实现 300×100ms=30s 的卡顿。 */
        uintptr_t base = lzt_core::main_text_base();
        for (int i = 0; i < 100 && base == 0; ++i) {
            usleep(20 * 1000);
            base = lzt_core::main_text_base();
        }
        if (base != 0) {
            oStrCtor = reinterpret_cast<void (*)(void*, const char*)>(
                base + OFF_SettingsStdStringCtor);
            lzt_core::log_write("std::string ctor @ base+0x%lx = %p",
                                (unsigned long)OFF_SettingsStdStringCtor,
                                reinterpret_cast<void*>(oStrCtor));
            lzt_core::set_base(base);
            lzt_core::log_write("main __TEXT base = 0x%lx (%s)", (unsigned long)base,
                                lzt_core::main_image_path());
        }
    }

    if (lzt_core::base() == 0) {
        lzt_core::log_write("wait main image timeout, abort");
        return;
    }

    lzt_core::log_write("=== lzt iOS main module start ===");
    LZT_STAGE("base ready; apply early patches");

    // 【v21】接管语言标识解析：必须尽可能早装 —— 游戏在进入主界面之前就会
    // 调用 0x1AC6D50 拼 "LawnStrings-<locale>"。装晚了这一轮就错过，
    // 只能等下次（表现为「重启后仍显示旧语言」）。
    install_fourcc_override_hook(lzt_core::base());

    // 字形缓存 2048：在构造函数阶段立即打补丁（早于游戏 UI 初始化）。
    // 之后 1s / 3s 各复查一次——若 __TEXT 解密回填覆盖了补丁，checksum 会变回
    // 原始 csel，apply() 会自动重新写入并重新打日志。
    lzt_ios_glyph_cache_2048_apply("boot");
    lzt_ios_glyph_pagelimit_apply("boot");
    // 戴夫待机动画 / 传送带递进计数
    lzt_ios_apply_dave_patch();
    lzt_ios_apply_conveyor_patch();
    // 图鉴切换刷新：必须先于游戏初始化装好，才能截获 app 对象。
    lzt_ios_install_glyph_flush();
    LZT_TICK("early patches (glyph/dave/conveyor/almanac)");
    LZT_STAGE("early patches done; register deferred glyph tasks");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        lzt_ios_glyph_cache_2048_apply("recheck-1s");
        lzt_ios_glyph_pagelimit_apply("recheck-1s");
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        lzt_ios_glyph_cache_2048_apply("recheck-3s");
        lzt_ios_glyph_pagelimit_apply("recheck-3s");
    });
    // 8s：此时游戏 UI 已初始化，缓存实例必然存在，读回实际生效值。
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(8 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        lzt_ios_glyph_cache_2048_apply("recheck-8s");
        lzt_ios_glyph_pagelimit_apply("recheck-8s");
        lzt_ios_glyph_cache_report();
    });
    // 自检（仅当 Documents/lzt_glyph_selftest 存在）：15s 主动刷新一次，
    // 在不开图鉴的情况下验证「销毁 + 重建」路径。
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(15 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        if (lzt_ios_glyph_selftest_requested()) {
            lzt_core::log_write("[CMP] glyph selftest: begin (6x)");
            for (int i = 0; i < 6; ++i) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                             (int64_t)(i * 2) * NSEC_PER_SEC),
                               dispatch_get_main_queue(), ^{
                    lzt_ios_glyph_flush(-99);
                });
            }
        }
    });

    // 【诊断】探测 pakMgr 结构（延迟到 pakMgr 构造后；见 register_banner_binding 里的调用）

    // 0) 文件访问探针（默认关闭；开启后只对关心路径抓调用栈，限次 + 重入保护）
    if (kEnableFileProbe) lzt_fileprobe_install();

    // 资源目录重定向：把游戏路径对象里的 Application Support 前缀改写为 Documents。
    // 两个相关函数都处理（A 返回 base+"No_Backup/"，B 构造路径根对象）。
    LZT_TICK("deferred glyph task registration");
    LZT_STAGE("deferred glyph tasks registered; install path redirects");
    {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x1c310e8),
                        reinterpret_cast<void*>(hkNbPathA), &t);
        g_origNbPathA = reinterpret_cast<LzS24 (*)(const void*)>(t);
        lzt_core::log_write("no_backup redirect: hook A=%s", t ? "ok" : "FAIL");
    }
    // 注：以下两类探针均已放弃，原因记录在案：
    //   · 0x1acb430（游戏内部格式化函数）：调用约定未确认，挂钩即崩
    //   · libc 的 fopen/open：位于 dyld 共享缓存，补丁走 20 字节远跳路径，
    //     挂钩后启动即崩（实测）。iOS 上不宜挂钩共享缓存内的函数。
    // 【v46】0x75a0e4 无需 hook：lzt_ios_page_layout 直接 base+OFF_SettingsPageShow
    // 调用原函数即可（见该函数实现）。此前这里多装一次 hook 且丢弃返回值，
    // 导致它出现在「缺失站点」报错清单里，干扰排查（实测无任何功能依赖该 hook）。
    // 【v33】设置页构造函数钩子从早期批次移走 —— 见 engine_install 之后。
    // v32 在此处安装，实测 FAIL：本批次（与 no_backup redirect A/B 同批）
    // 装的所有钩子全部失败，而 engine_install 里的 core hook 全部成功。
    // 原因是此时 __TEXT 页尚未转为可写，A64HookFunction 的 mprotect 未生效。
    {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x1c32040),
                        reinterpret_cast<void*>(hkNbPathB), &t);
        g_origNbPathB = reinterpret_cast<uintptr_t (*)(uintptr_t)>(t);
        lzt_core::log_write("no_backup redirect: hook B=%s", t ? "ok" : "FAIL");
    }

    // 【方案 A】AppSupport getter(0x1c36564) 尾跳转到 Documents getter(0x1c3639c)。
    // 与文档《No_Backup 迁移至 Documents》§2.1 的静态补丁等价（那条 BL 就在 0x101C30FE0），
    // 但通过运行时 hook 实现，不改动二进制。
    // 目的：让 FS 单例 +0x58 从初始化起就是 Documents，使
    //   · case 3/4 天然指向 Documents/No_Backup、Documents/Backup
    //   · 目录初始化创建子目录时路径已正确（消除「先创建后改写」的时序缺陷）
    //   · iCloud 排除属性设到正确位置
    {
        g_origDocGetter = reinterpret_cast<void*>(lzt_core::base() + 0x1c3639c);
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x1c36564),
                        reinterpret_cast<void*>(hkAppSupGetter), &t);
        lzt_core::log_write("no_backup migrate(A): AppSupGetter(0x1c36564)->DocGetter: %s",
                            t ? "ok" : "FAIL");
    }

    LZT_TICK("path redirect hook install");
    // 1) 注册业务模块（纯入表，须在 engine_install 之前）
    LZT_STAGE("path redirects done; register modules");
#if defined(LZT_AB_NO_LANGUAGE) || defined(LZT_AB_ALMANAC_ONLY)
    LZT_STAGE("language module skipped for isolation");
#else
    //    注册顺序决定动态 id 分配顺序：语言的 slotAfterAnchor=1 在前，
    //    视角 slotAfterAnchor=2 在后（与 Android 一致）。
    LZT_STAGE("calling language_module_init");
    language_module_init();
    LZT_STAGE("calling view_angle_module_init");
    view_angle_module_init();

    // 2) 安装设置框架引擎（Tab 注入三件套）
    LZT_STAGE("calling engine_install");
    lzt_settings::engine_install();
    LZT_STAGE("engine_install returned");

    // 2.5) 设置页构造函数钩子 0x756AE4（入口 x0 = 设置根页 this）
    // 必须放在 engine_install 之后：同一套 A64HookFunction，早期批次全部 FAIL
    // （__TEXT 未转可写），而这里安装的核心钩子全部成功，时机已验证。
    // 根页指针是根标题刷新的唯一确定来源 —— owner 链（+216 → +8）拿到的是
    // 子页标题对象，不是根页（v31/v32 实证）。
    {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + OFF_SettingsInit),
                        reinterpret_cast<void*>(hkSettingsInit), &t);
        oSettingsInitFn = reinterpret_cast<void (*)(uintptr_t)>(t);
        lzt_core::log_write("settings init hook(0x756ae4): %s tramp=%p",
                            t ? "ok" : "FAIL", t);
    }

    // 3) 视角 hook 按状态挂载
    LZT_STAGE("calling view_angle_sync_hooks");
    view_angle_sync_hooks();
    LZT_STAGE("view_angle_sync_hooks returned");
    LZT_TICK("language_module_init+view_angle+engine+sync_hooks");

    // 4) 全游戏本地化入口 hook：必须尽早装，否则启动阶段已解析的文本覆盖不到
    // (计时点见下方，此处不再插入)
    LZT_STAGE("calling install_l10n_hook");
    install_l10n_hook();
    LZT_TICK("install_l10n_hook");
    LZT_STAGE("install_l10n_hook returned");

    // 5) CDN 应用链取证（只读记录管理器对象，供语言切换后重触发）
    LZT_STAGE("settings/view/l10n done; install CDN and language hooks");
    {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x735930),
                        reinterpret_cast<void*>(hkCdnApply), &t);
        g_origCdnApply = reinterpret_cast<uintptr_t (*)(uintptr_t)>(t);
        lzt_core::log_write("cdn apply hook: %s", t ? "ok" : "FAIL");
    }

    // 5a-2) 单表应用 0x2ca220 —— 语言切换后用它重放加载（对齐安卓 OFF_CDN_APPLY）

    // 5a-3) 语言表加载入口 0x2c0bf0（按原版方法重载；见 lzt_ios_reload_lang_table）
    if (kEnableLangTableReload) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x2c0bf0),
                        reinterpret_cast<void*>(hkBigTableLoad), &t);
        g_origBigTableLoad = reinterpret_cast<uintptr_t (*)(uintptr_t)>(t);
        lzt_core::log_write("langtable hook(0x2c0bf0): %s", t ? "ok" : "FAIL");
    }

    // 5a-4) wstring 构造器（0x757b78）—— 必须在设置页初始化【之前】就绪：
    // 设置行（Tab）在 0x756AE4 里创建，而创建后我们要立即按当前语言回填标题，
    // 那时若构造器指针还没赋值就只能跳过（表现为「退出重进设置页文本不变」）。
    hkInstallWstringCtorHook();
    hkInstallRowAttachHook();
#endif
    // 【诊断·已停用】主动读包内 LANGUAGETYPES.rton。
    //
    // 停用原因（无越狱 iPad 实测）：本段会在启动 1.5s 后开始轮询，一旦拿到
    // 资源对象就在【主队列】上调用游戏内部函数 0x65428 去取资源。
    // 该函数的时序状态不受我们控制——游戏此刻可能正处在其自身的资源加载
    // 批次中，被我们从外部重入后表现为「随机停在 EA 界面、必须杀进程重启」。
    // 这与「主动调用 0x64f20 会崩溃」是同一类问题，只是换了个入口。
    // 该探针的取证目的早已完成，产品版不应保留任何主动调用游戏函数的代码。
    if (kEnableRsbFetchProbe) {
        static std::atomic<int> s_tries{0};
        static void (^tick)(void);
        tick = ^{
            const uintptr_t res = g_rsbResObj.load(std::memory_order_acquire);
            if (res != 0 || s_tries.fetch_add(1) >= 40) {
                lzt_try_fetch_pkg();
                return;
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(500 * NSEC_PER_MSEC)),
                           dispatch_get_main_queue(), tick);
        };
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1500 * NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), tick);
    }
    // 【诊断】按名字取资源（0x65428）：记录资源对象与名字
    if (kEnableRsbFetchProbe) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x65428),
                        reinterpret_cast<void*>(hkRsbFetch), &t);
        g_origRsbFetch = t;
        lzt_core::log_write("rsbfetch hook(0x65428): %s", t ? "ok" : "FAIL");
    }
    // 【诊断】查找入口 0x7eb1c0（安卓 sub_16B31EC 的 iOS 等价物）
    // 与安卓不同：安卓直接输出 (dataPtr, size)，iOS 的 x2 是输出对象，
    // 本探针用于确定该对象内哪两个偏移承载 (指针, 长度)。
    if (kEnableResLookupProbe) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x7eb1c0),
                        reinterpret_cast<void*>(hkResLookup), &t);
        g_origResLookup = t;
        lzt_core::log_write("reslookup hook(0x7eb1c0): %s", t ? "ok" : "FAIL");
    }
    // 【诊断】名字构造（0x6525c）：打印按名字取包内资源时的真实资源名
    if (kEnableRsbNameProbe) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x6525c),
                        reinterpret_cast<void*>(hkRsbNameCtor), &t);
        g_origRsbNameCtor = t;
        lzt_core::log_write("rsbname hook(0x6525c): %s", t ? "ok" : "FAIL");
    }
    // 【诊断】0x64f20 完整观测
    if (kEnableRsbReadProbe) {
        void* t = nullptr;
        uintptr_t tgt = lzt_core::base() + 0x64f20;
        bool st = lzt_static_is_patched(tgt);
        if (st) {
            lzt_static_fill(tgt, reinterpret_cast<void*>(hkRsbReadX));
            t = lzt_static_tramp_for(tgt);
        } else {
            A64HookFunction(reinterpret_cast<void*>(tgt),
                            reinterpret_cast<void*>(hkRsbReadX), &t);
        }
        g_origRsbReadX = t;
        lzt_core::log_write("rsbreadX hook(0x64f20): %s (%s)", t ? "ok" : "FAIL",
                            st ? "static" : "inline");
    }
    // 【诊断】主动调用 0x64f20 会崩溃（该函数是「枚举挂载包」而非通用读取），已停用。
    // 改为纯只读观测：见下方 kEnableRsbNameProbe / kEnableRsbReadProbe。
    // 【诊断】拼接函数（0x1bd388c）：确认 News 查询名的最终形态
    if (kEnableConcatProbe) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x1bd388c),
                        reinterpret_cast<void*>(hkConcat), &t);
        g_origConcat = t;
        lzt_core::log_write("concat hook(0x1bd388c): %s", t ? "ok" : "FAIL");
    }
    // 【诊断】本地化数据查询（0x7c18fc）：观察 News 内容实际查询的名字
    if (kEnableLdQueryProbe) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x7c18fc),
                        reinterpret_cast<void*>(hkLdQuery), &t);
        g_origLdQuery = t;
        lzt_core::log_write("ldquery hook(0x7c18fc): %s", t ? "ok" : "FAIL");
    }
    // 【诊断】短码 → locale id 转换函数（0x101ac6e5c）：观察 News 取键用的短码
    if (kEnableShortcodeProbe) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x101ac6e5c),
                        reinterpret_cast<void*>(hkShortcodeFn), &t);
        g_origShortcodeFn = t;
        lzt_core::log_write("shortcode hook(0x101ac6e5c): %s", t ? "ok" : "FAIL");
    }
    // 【诊断】语言短码映射函数（0x1c8380）：确认 News 取键链的调用来源
    if (kEnableNewsLocaleHook) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x1c8380),
                        reinterpret_cast<void*>(hkLocaleMap), &t);
        g_origLocaleMap = t;
        lzt_core::log_write("localeMap hook(0x1c8380): %s", t ? "ok" : "FAIL");
    }
    // 行初始化器（0x75bcb4 → 0x75b128）：直接给出 (行, 标题键)，
    // 覆盖 Tab 行与带标题的设置行，这是安卓 sub_A54230 的等价锚点。
    // 注：实测滑条行（Music / Sound FX）【不经过】它，需另用容器认领（见 hkRowAttach）。
    {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x75b128),
                        reinterpret_cast<void*>(hkRowInit), &t);
        g_origRowInit = t;
        lzt_core::log_write("row init hook(0x75b128): %s", t ? "ok" : "FAIL");
    }
    // 滑条行（Music / Sound FX）走另一条路径：其创建函数 0x757ff8 内用
    // 0x757a90(字段地址, 键) 写标题 —— 挂钩该写入点即可拿到确定的行对象。
    {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x757a90),
                        reinterpret_cast<void*>(hkStrCtorAt), &t);
        g_origStrCtorAt = t;
        lzt_core::log_write("row strctor hook(0x757a90): %s", t ? "ok" : "FAIL");
    }

    // 5a-5) 单文件 Apply 探针（0x2ca220）：记录启动期 CDN 应用的真实参数
    if (kEnableApplyProbe) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x2ca220),
                        reinterpret_cast<void*>(hkApplyProbe), &t);
        g_origApplyProbe = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t)>(t);
        lzt_core::log_write("apply-probe hook(0x2ca220): %s", t ? "ok" : "FAIL");
    }

    // 5c-1) 文本哈希表查找 0x17a480 —— 与安卓 Localizer 同一位置
    {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x17a480),
                        reinterpret_cast<void*>(hkLookup), &t);
        g_origLookup = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(t);
        lzt_core::log_write("lookup hook(0x17a480): %s", t ? "ok" : "FAIL");
    }
    {
        void* t = nullptr;

    // 加载表函数 0x2cbfb4 探针（确认语言表加载时机）
    {
        void* t = nullptr;
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x2cbfb4),
                        reinterpret_cast<void*>(hkTblLoad), &t);
        g_origTblLoad = reinterpret_cast<decltype(g_origTblLoad)>(t);
        lzt_core::log_write("tblload hook(0x2cbfb4): %s", t ? "ok" : "FAIL");
    }
    // 用例：单独启用「单表 RTON 加载器」探针（Android OFF_DB_LOAD_RTON 的等价物）
    if (kEnableTblLoadProbe) {
        void* t2 = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x2cbfb4),
                        reinterpret_cast<void*>(hkTblLoad), &t2);
        g_origTblLoad = reinterpret_cast<decltype(g_origTblLoad)>(t2);
        lzt_core::log_write("tblload probe installed: %s", t2 ? "ok" : "FAIL");
    }
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x2ca220),
                        reinterpret_cast<void*>(hkApply), &t);

    // RSB 资源查找探针（外层 + 名字构造）
    {
        void* t = nullptr;
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x64f20),
                        reinterpret_cast<void*>(hkRsbOuter), &t);
        g_origRsbOuter = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t)>(t);
        lzt_core::log_write("rsb outer hook: %s", t ? "ok" : "FAIL");
    }
    {
        void* t = nullptr;
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x6525c),
                        reinterpret_cast<void*>(hkRsbName), &t);
        g_origRsbName = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(t);
        lzt_core::log_write("rsb name hook: %s", t ? "ok" : "FAIL");
    }
        g_origApply = reinterpret_cast<decltype(g_origApply)>(t);
        lzt_core::log_write("apply hook(0x2ca220): %s", t ? "ok" : "FAIL");
    }

    if (kEnableApplyHook) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x2c9e04),
                        reinterpret_cast<void*>(hkApply2), &t);
        g_origApply2 = reinterpret_cast<uintptr_t (*)(uintptr_t, uint32_t, uintptr_t)>(t);
        lzt_core::log_write("cdn-apply hook(0x2c9e04): %s", t ? "ok" : "FAIL");
    }
    // 方案 A：挂钩「清单条目 → 文件名」的求名函数（0x736d20），
    // 使 tableId=131 的文件名可被改写成目标语言。
    if (kEnableManifestReapply) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x736d20),
                        reinterpret_cast<void*>(hkTblName), &t);
        g_origTblName = t;
        lzt_core::log_write("tblname hook(0x736d20): %s", t ? "ok" : "FAIL");
    }

    // 5b) CDN 清单「添加条目」hook —— 安卓式重载的落点
    if constexpr (kEnableCdnManifestInject) {
        void* t = nullptr;
        A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x40910),
                        reinterpret_cast<void*>(hkCdnListAdd), &t);
        g_origCdnListAdd = reinterpret_cast<CdnListAddFn>(t);
        lzt_core::log_write("cdn manifest hook: %s", t ? "ok" : "FAIL");
    }
// 5c) CDN 初始化入口探针 —— CDN 重载链的源头。
    static uintptr_t (*g_origCdnInit)(uintptr_t) = nullptr;
// 5c) CDN 初始化入口探针 —— CDN 重载链的源头。
    //     链：0x1c4a8c(入口) → 内部 bl 0x3f4d4(清单收集) → 0x735930(清单应用)
    //     ⚠️ 0x1c5ca4 是函数内部的 bl 指令，不是入口；inline hook 必须挂入口。
    //        入口经反查函数序言（栈帧 352 字节）确定为 0x1c4a8c。
    {
        void* t = nullptr;
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x1c4a8c),
                        reinterpret_cast<void*>(hkCdnInit), &t);
        g_origCdnInit = reinterpret_cast<uintptr_t (*)(uintptr_t)>(t);
        lzt_core::log_write("CDN-INIT hook: %s", t ? "ok" : "FAIL");
    }
    // 文件访问监控（默认关闭：iOS 经 Foundation 内部发起 syscall，拿不到调用者）
    /* 探针暂时关闭：backtrace 在高频路径可能不稳，先保证游戏可玩 */

    // 5d) 探针：哪条路径在启动时真正拼 "LawnStrings-<locale>"
    {
        void* t = nullptr;
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x2c1298),
                        reinterpret_cast<void*>(hkProbeA), &t);
        g_origProbeA = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(t);
        lzt_core::log_write("path-A hook: %s", t ? "ok" : "FAIL");
    }
    {
        void* t = nullptr;
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x3f4d4),
                        reinterpret_cast<void*>(hkProbeC), &t);
        g_origProbeC = reinterpret_cast<uintptr_t (*)(uintptr_t)>(t);
        lzt_core::log_write("path-C hook: %s", t ? "ok" : "FAIL");
    }
    {
        void* t = nullptr;
        if (kEnableProbeHooks) A64HookFunction(reinterpret_cast<void*>(lzt_core::base() + 0x6525c),
                        reinterpret_cast<void*>(hkProbeD), &t);
        g_origProbeD = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(t);
        lzt_core::log_write("path-D hook: %s", t ? "ok" : "FAIL");
    }

    // 6) 文本表内存定位（只读；默认关闭，见 kEnableTextTableScan）
    if constexpr (kEnableTextTableScan) {
        pthread_t th;
        if (pthread_create(&th, nullptr, +[](void*) -> void* {
                for (int i = 0; i < 20; ++i) usleep(1000 * 1000);
                lzt_ios_scan_text_table();
                return nullptr;
            }, nullptr) == 0) {
            pthread_detach(th);
        }
    }

    lzt_core::log_write("=== lzt iOS main module ready ===");
    LZT_TICK("remaining probe hooks");
#if defined(LZT_STATIC_ONLY)
    lzt_core::log_write("[STATIC-ONLY] static sites used=%d, no-site skipped=%d, blocked code-page writes=%d",
                        lzt_static_only_static_used(),
                        lzt_static_only_blocked_inline(),
                        lzt_static_only_blocked_mprotect());
    lzt_report_missed_sites();   /* 【v44】缺站点的功能会静默失效，集中报错 */
#endif
    // 业务屏幕绑定（主界面横幅）：经框架注册点接入，切语言时按序回调。
    LZT_STAGE("main hook installation returned; register tail tasks");
    register_banner_binding();
    // 自动化/调试：若容器 Documents 下存在 lzt_autolang.txt（内容为 locale），
    // 在启动若干秒后自动触发一次语言切换，用于无人值守验证切换链路。
    lzt_ios_maybe_autolang();
    // 【诊断】RSM 结构 dump（对照安卓公式）
    lzt_ios_dump_rsm_deferred();

    // 【帧率】安装 setFrameInterval: 钩子 —— 60fps 开关的唯一功能依赖。
    // 其余诊断探针（nextDrawable / CAMetalDrawable present / CADisplayLink 捕获 /
    // WORKTICK 计数 / 2 秒报告定时器）已移除，避免日志噪声。
    // 需要重新测量时：恢复这些安装调用与定时器，或参考 docs/60FPS_UNLOCK.md。
    lzt_ios_install_dl4();

    // 语言表预热：此刻 base 与 oStrCtor 已就绪，但 pakMgr/RSB 通常尚未
    // 构造（日志实证：此处 `pakmgr: slot=... mgr=0x0`），故直接加载必然失败。
    //
    // 因此不做一次性预热，而是起一个轻量轮询：等 pakMgr/RSB 就绪后再加载。
    // 轮询而非 dispatch_after 固定延时，是因为 RSB 就绪时机随设备性能波动，
    // 固定延时不可靠。轮询上限 40 × 250ms = 10s，覆盖正常启动流程；
    // 超时则放弃——后续 createTab 的 retry() 与页面重建仍会评估，
    // 且 shouldCreate() 已能区分「还不知道」与「无来源」，不会误判。
    LZT_TICK("banner binding + prewarm scheduled");
#if !defined(LZT_AB_NO_LANGUAGE) && !defined(LZT_AB_ALMANAC_ONLY)
    LZT_STAGE("calling prewarm_language_table_async");
    lzt_ios_prewarm_language_table_async();
    LZT_STAGE("prewarm_language_table_async returned");
#endif
    LZT_STAGE("all tail tasks registered; wait_and_start returns");
    LZT_TICK("tail (prewarm dispatch)");
    lzt_core::log_write("[COST] ==== wait_and_start TOTAL %.1f ms ====", lzt_ms_now() - t_enter);
#undef LZT_TICK
#undef LZT_STAGE
}

static std::atomic<bool> g_bootstrapScheduled{false};

static void lzt_bootstrap_on_main(void*) {
    const double t_boot = lzt_ms_now();
    const double disp = g_dispatchMs.load(std::memory_order_acquire);
    if (disp > 0) {
        lzt_core::log_write("[COST] dispatch -> bootstrap start: %.1f ms", t_boot - disp);
    }
    lzt_set_stage("deferred bootstrap begins"); lzt_core::log_write("[STAGE] %s", "deferred bootstrap begins");
    /* 心跳在这里启动：已在主队列、游戏已起跑，建线程安全；
     * 且能覆盖紧随其后的全部业务初始化。 */
    lzt_record_main_stack_range();
    lzt_start_heartbeat();
    /* A/B/C 单变量对照：
     * LZT_AB_NO_ALMANAC   — 不注册 4 个图鉴回调，其余初始化不变；
     * LZT_AB_NO_LANGUAGE  — 不跑语言入口/表预热，其余初始化不变；
     * LZT_AB_ALMANAC_ONLY — 仅注册图鉴回调，跳过语言和视角业务初始化。
     * 这些开关只用于稳定性分组，不应进入最终功能版。 */
#if !defined(LZT_AB_NO_LANGUAGE) && !defined(LZT_AB_ALMANAC_ONLY)
    lzt_set_stage("calling language_locale_bootstrap"); lzt_core::log_write("[STAGE] %s", "calling language_locale_bootstrap");
    language_locale_bootstrap();
    lzt_set_stage("language_locale_bootstrap returned"); lzt_core::log_write("[STAGE] %s", "language_locale_bootstrap returned");
#endif
    wait_and_start();
    g_bootstrapFinished.store(true, std::memory_order_release);
    lzt_set_stage("deferred bootstrap finished"); lzt_core::log_write("[STAGE] %s", "deferred bootstrap finished");
    lzt_core::log_write("[COST] bootstrap TOTAL %.1f ms (from dispatch to finished)",
                        lzt_ms_now() - t_boot);
}

static void lzt_schedule_bootstrap_async(void) {
    bool expected = false;
    if (!g_bootstrapScheduled.compare_exchange_strong(expected, true)) return;
    g_dispatchMs.store(lzt_ms_now(), std::memory_order_release);
    /* 用 dispatch_async_f（C 接口）把业务初始化派发到主队列：
     *   · 不阻塞 dyld 加载阶段（constructor 立即返回，游戏正常起跑）
     *   · 仍在主线程执行，与游戏自身 UI/资源调用天然串行，
     *     避免此前实测过的“在后台线程装 hook 必崩”
     * 本文件按 C++ 编译，不能用 ^block 语法，故用函数指针形式。 */
    dispatch_async_f(dispatch_get_main_queue(), nullptr, lzt_bootstrap_on_main);
}

__attribute__((constructor)) static void lzt_ios_main_entry() {
#if defined(LZT_BASELINE_CTOR_ONLY)
    /* 稳定性分层实验：保留原 dylib 的全局静态初始化、日志和崩溃诊断，
     * 但跳过目录操作与全部业务安装。与零行为库相比只增加这一层。 */
    lzt_core::log_init();
    lzt_core::install_crash_diagnostics();
    lzt_core::log_write("[BASELINE] ctor-only: no filesystem or business install");
    return;
#endif
    // 资源目录重定向（两层保障，效果一致、互不冲突）：
    //   ① 文件系统层：把 Library/Application Support/No_Backup 变成指向
    //      Documents/No_Backup 的符号链接（覆盖一切未走路径对象 B 的访问，
    //      例如游戏自身的「确保目录存在」逻辑——实测它会重建旧目录）。
    //   ② 代码层：把路径对象里的 Application Support 前缀改写成 Documents
    //      （见 wait_and_start 的资源目录重定向）
    // 目录重定向：保留在 constructor 中执行（游戏读取数据包要求此路径就绪）。
    // 该函数已改为「快路径只读」：配置正确时仅一次 lstat + 一次 readlink 即返回，
    // 只有真正的迁移/修复场景才会写文件系统。
    lzt_set_stage("constructor: redirect_no_backup_dir");
    lzt_ios_redirect_no_backup_dir();
    lzt_set_stage("constructor: ensure_backup_exclusion");
    lzt_ios_ensure_backup_exclusion();

    lzt_core::log_init();
    lzt_core::install_crash_diagnostics();
    lzt_core::log_write("constructor: lzt iOS main module loading");
#if defined(LZT_BASELINE_FS_ONLY)
    /* 第二层：只增加构造期的目录/文件属性操作，随后立即返回。 */
    lzt_core::log_write("[BASELINE] fs-only: no business install");
    return;
#endif


    // constructor 只登记异步启动，不在这里等待主镜像、不安装 hook、
    // 不扫描资源、不做同步业务初始化。否则 dyld 加载锁会拖住 EA 启动。
    // ⚠️ 心跳也不能在这里启动：它要建线程，而「构造期建线程」在本项目
    // 已被实测证明会崩。心跳改在 bootstrap（主队列）里启动。
    lzt_schedule_bootstrap_async();
}
// ============================================================
// CDN 初始化入口（iOS 对齐安卓 CDN apply 链的关键）
//
// 静态链条（反汇编实证）：
//   0x1001c5ca4  CDN 初始化（本函数，全二进制唯一调用点→0x3f4d4）
//     └─ 0x10003f4d4  清单收集：拼 "LawnStrings-<locale>.rton" 并以 tableId=131
//                 追加进清单（内部调用清单添加 0x40910，数十处）
//     └─ 0x100735930  清单应用：遍历清单逐个 apply（游戏自身的读取发生在此）
//
// 与安卓的对应关系：Android 的 OFF_CDN_APPLY=0xD58544 对应 iOS 的 0x735930；
// 清单注入（安卓 cdn_load_rton hook sub_ACABCC）对应 iOS 的 0x40910。
// ============================================================
