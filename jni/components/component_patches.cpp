// ============================================================
// component_patches.cpp — 组件补丁（框架与业务模块之外的通用组件修复）
//
// 见 components/README.md。当前两条：
//   一、glyph_cache_size   强制所有设备字形渲染缓存为 2048 + 图集页数上限 4
//   二、glyph_cache_flush  图鉴（Almanac）翻页时主动触发字形缓存刷新（修复翻页脏缓存）
//
// 接入：经 lzt_settings::register_engine_ready 在引擎就绪（基址可用）后打补丁；
// 仅 ARM64 编译实体，ARM32/正式版为空实现。
// ============================================================

#include "component_patches.h"

#include <chrono>
#include <thread>

#include <sys/mman.h>

#include <cstdio>
#include <unistd.h>

#include "lzt_settings_framework_config.h"
#include "settings/settings_framework.h"
#include "lzt_core.h"
#include "lzt_hooks.h"
#include "offsets.h"

#ifdef __aarch64__

// 四cc→字符串 原函数指针。必须定义在文件作用域且用 C 链接：
// naked shim 里以 ADRP/ADD 直接寻址（匿名 namespace 内的符号无法这样引用）。
extern "C" { void (*oFourccToString)(void*, uint32_t) = nullptr; }

namespace {

// ============================================================
// 组件补丁一：字形渲染缓存 2048 + 图集页数上限 4
//
// 参照 iOS 的做法：iOS 是「尺寸 + 页上限」
// 两条指令补丁成套生效
//   尺寸   0x1cb7db0  csel w23,w8,w9,lt → mov w23,#2048
//   页上限 0x1cb7e1c  mov  w1,#1        → mov  w1,#4
// Android 侧此前只做了「尺寸」，且是通过 hook 构造函数改 a4（= cache+0xA4）。
// 但 Android 与 iOS 结构不同：iOS 只改页上限即可，Android 的页增长还受
// cache+0x134 这个独立开关控制（iOS 无此开关，见下方 C 节）。
// 结果：页上限 1 + 增长开关 0 → 图集永远只有 1 页 → 页满即
// 「no place to put this glyph into the glyph cache」，表现为缺字/闪退。
//
// Android 对应点（偏移与字节均按 .so 本体 PT_LOAD 映射核对，非 IDA 显示值）：
//
//   A. 尺寸决策  sub_1713384（EAText 缓存初始化）
//      0x17133F0  CMP  W23,#0x401      ; min>=1025 ?
//      0x17133F4  MOV  W8,#0x800       ;   2048
//      0x17133F8  MOV  W9,#0x400       ;   1024
//      0x17133FC  CSEL W8,W9,W8,LT
//      0x1713400  CMP  W23,#0x281      ; min>=641 ?
//      0x1713404  MOV  W9,#0x200       ;   512
//      0x1713408  CSEL W3,W9,W8,LT   ← 打这里（w3 只喂给下方的 ctor）
//      0x1713414  BL   sub_17F3560     ; PrimeGlyphCache ctor(cache,_,_,size=w3)
//      改为 MOV W3,#2048。该处 w3 在函数内唯一消费方就是 ctor 的 a4，
//      与 iOS「w23 只用于写 +0xA4」同构，替换安全。
//
//   B. 页数上限  sub_1713384（ctor 之后紧接的虚调用）
//      0x1713424  STR  W9,[X21,#0x138] ; SetParameter(4, 1)
//      0x1713428  MOV  W1,#1           ; ← 页数上限（= cache+0xA0）
//      0x1713430  MOV  W2,#1           ;   初始分配页数
//      0x1713438  BLR  X8              ; SetPageLimit(limit=w1, initial=w2)
//      注意：构造期 sub_1818C08 已把 +0xA0 置 4（0x1818C54），但这里
//      立刻用 SetPageLimit(1,1) 覆盖回 1。改为 MOV W1,#4 恢复 4。
//
//   C. 页增长开关  cache+0x134（SetParameter id 2）
//      sub_1818C08 构造期清零（0x1818C9C STURH WZR,[X20,#0x134]），
//      全库唯一写点就是 sub_181928C 的 SetParameter(2,x)；字节级扫描
//      （STRB [Xn,#0x134] 全库 25 处，仅 0x18192EC 属本类）证明**无人开启**。
//      sub_181A368（EnsurePage）逻辑：
//        0x181A400 LDRB W8,[X20,#0x134] → CBZ 直接返回 0（不再开页）
//        0x181A40C 页数 >= +0xA0(=4)      → 返回 0
//        0x181A420 +0x13C 重入保护
//      故 A+B 之外必须同时打开该开关，否则上限 4 永远摸不到。
//      实现方式：hook ctor，调用原函数后写 cache+0x134 = 1。
//      只在 EAText 初始化时发生（ctor 全库唯一调用点 0x1713414），非热路径。
//      **不改 ctor 入参**——改 a4 才是此前「尺寸字段/存储分配不一致→堆破坏」的成因。
//
// 三项都做原始字节校验：任一不符只记一次日志并按原样跳过，绝不盲写；
// 并配有 20s 复检，覆盖「补丁安装前 EAText 已初始化」的时序竞态。
// ============================================================

// 页增长 / 页上限总开关。false = 只强制尺寸 2048（保守，真机已验证路径）。
// 真机回归：开启后两个变体均闪退（尺寸补丁当时因偏移 bug 未生效，缓存仍 1024）。
constexpr bool kGlyphAllowPageGrowth = false;

// 指令字（小端存放，数值为 IDA 反汇编语义）
constexpr uint32_t kInsnCselW3    = 0x1A88B123u; // CSEL W3,W9,W8,LT   原始
constexpr uint32_t kInsnMovW3_2048= 0x52810003u; // MOV  W3,#2048      补丁
constexpr uint32_t kInsnMovW1_1   = 0x320003E1u; // MOV  W1,#1         原始
constexpr uint32_t kInsnMovW1_4   = 0x52800081u; // MOV  W1,#4         补丁

// 0 = 未处理 1 = 已生效 2 = 字节不符
int g_glyph_size_state = 0;
int g_glyph_limit_state [[maybe_unused]] = 0;
int g_glyph_grow_state = 0;

inline uintptr_t page_mask() {
    long ps = ::sysconf(_SC_PAGESIZE);
    return (ps > 0 ? static_cast<uintptr_t>(ps) : 4096u) - 1;
}

// 把 target 起 len 字节所在页改为 RWX，写完由调用方 clear_cache。
static bool make_writable(uintptr_t target, size_t len) {
    uintptr_t mask = page_mask();
    uintptr_t page = target & ~mask;
    uintptr_t end = (target + len + mask) & ~mask;
    return ::mprotect(reinterpret_cast<void*>(page), end - page,
                      PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

// 单条 32 位指令替换：校验原始字 → 改 RWX → 写 → clear_cache。
// 返回 true 表示本次写入生效（含「已是目标值」）。
static bool patch_insn(const char* what, uintptr_t site,
                       uint32_t expect, uint32_t want, int* state) {
    uint32_t cur = *reinterpret_cast<const uint32_t*>(site);
    if (cur == want) {
        if (*state != 1) {
            *state = 1;
            lzt_core::log_write("[CMP] %s: already patched @+0x%lx", what,
                                static_cast<unsigned long>(site - lzt_core::base()));
        }
        return true;
    }
    if (cur != expect) {
        if (*state != 2) {
            *state = 2;
            lzt_core::log_write("[CMP] %s: unexpected insn 0x%08x @+0x%lx (skip)",
                                what, cur,
                                static_cast<unsigned long>(site - lzt_core::base()));
        }
        return false;
    }
    if (!make_writable(site, 4)) {
        lzt_core::log_write("[CMP] %s: mprotect FAILED @+0x%lx", what,
                            static_cast<unsigned long>(site - lzt_core::base()));
        return false;
    }
    *reinterpret_cast<uint32_t*>(site) = want;
    __builtin___clear_cache(reinterpret_cast<char*>(site),
                            reinterpret_cast<char*>(site) + 4);
    *state = 1;
    lzt_core::log_write("[CMP] %s: patched @+0x%lx  %08x -> %08x", what,
                        static_cast<unsigned long>(site - lzt_core::base()),
                        expect, want);
    return true;
}

// 补丁 C：cache+0x134 = 1（允许图集开新页）。
// 由 hook ctor 返回后调用，a1 即新建的 PrimeGlyphCache 实例（ctor 的 x0）。
// 注意字段语义（勿凭名字猜）：
//   +0xA0 页数上限      —— sub_1818C08 构造置 4，随后 SetPageLimit 覆盖；由补丁 B 固定为 4
//   +0xA4 缓冲区尺寸    —— ctor 的 a4 写入；由补丁 A 固定为 2048
//   +0xA8 SetParameter(id=1) —— 构造置 3，被 sub_1819B38 用作 glyph 对象 +132 默认值，
//                          与尺寸无关，**不得改写**
//   +0x134 页增长开关   —— SetParameter(id=2)，构造期清零，全库无人开启
constexpr uintptr_t GLYPH_OFF_PAGE_LIMIT = 0xA0;
constexpr uintptr_t GLYPH_OFF_SIZE       = 0xA4;
constexpr uintptr_t GLYPH_OFF_ALLOW_GROW = 0x134;

// EAText 全局 getter（sub_1713288：仅读 qword_26D2230）与字形缓存实例。
// 补丁二的「清+重建」与补丁 C 的时序兜底都要用，故在此提前声明。
typedef uintptr_t (*EaTextGlobal_t)();
EaTextGlobal_t g_eaTextGlobal = nullptr;

typedef void (*GlyphCacheCtor_t)(uintptr_t, uintptr_t, uintptr_t, int);
GlyphCacheCtor_t oGlyphCacheCtor = nullptr;
alignas(32) uint8_t g_tr_glyph_ctor[lzt_hooks::TRAMPOLINE_SIZE];

// 只在头几次构造时打印，避免刷屏
int g_glyph_fixup_logged = 0;
static int g_glyph_recheck_logged = 0;

static void fixup_glyph_cache_obj(uintptr_t cache) {
    if (!cache) return;
    if constexpr (!kGlyphAllowPageGrowth) return;   // 页增长停用：仅保留尺寸补丁
    *reinterpret_cast<uint8_t*>(cache + GLYPH_OFF_ALLOW_GROW) = 1;
    if (g_glyph_fixup_logged < 3) {
        ++g_glyph_fixup_logged;
        lzt_core::log_write(
            "[CMP] glyph cache %p: size=%d pageLimit=%d growth=1",
            reinterpret_cast<void*>(cache),
            *reinterpret_cast<int*>(cache + GLYPH_OFF_SIZE),
            *reinterpret_cast<int*>(cache + GLYPH_OFF_PAGE_LIMIT));
    }
    if (g_glyph_grow_state == 0) {
        g_glyph_grow_state = 1;
        lzt_core::log_write("[CMP] glyph cache growth enabled (+0x134 = 1)");
    }
}

__attribute__((noinline))
void hk_glyph_cache_ctor(uintptr_t a1, uintptr_t a2, uintptr_t a3, int size) {
    oGlyphCacheCtor(a1, a2, a3, size);
    if constexpr (lzt_config::kEnableGlyphCacheSize2048) {
        fixup_glyph_cache_obj(a1);
    }
}

// 补丁 C 的补时序：指令补丁 A/B 只对「之后新建」的缓存生效。
// 若 EAText 在补丁安装前就已初始化，需对现有对象补一次字段修正
// （iOS 用 boot/1s/3s/8s 复检解决同一问题）。
//
// 安全性：sub_1713288() 只是读全局 qword_26D2230（无分配、无锁），
// 可安全轮询；读到对象后再用 memory_range_accessible 双重确认。
constexpr int kGlyphRecheckAttempts = 40;    // 40 × 500ms = 20s
constexpr int kGlyphRecheckIntervalMs = 500;

static void glyph_cache_recheck_once(uintptr_t base) {
    if (!g_eaTextGlobal) return;
    uintptr_t g = g_eaTextGlobal();
    if (g == 0) return;
    if (!lzt_core::memory_range_accessible(g, 16, false)) return;
    uintptr_t cache = *reinterpret_cast<uintptr_t*>(g + 8);
    if (cache == 0) return;
    if (!lzt_core::memory_range_accessible(cache, 0x1B8, true)) return;
    // 已有对象：页上限/尺寸是 ctor 写死的，改不了指令就改字段；
    // +0x134 是构造期清零的开关，任何时候写都安全。
    int* pLimit = reinterpret_cast<int*>(cache + GLYPH_OFF_PAGE_LIMIT);
    int* pSize  = reinterpret_cast<int*>(cache + GLYPH_OFF_SIZE);
    if constexpr (!kGlyphAllowPageGrowth) {
        // 页增长停用：不再改写 limit/growth，只报告实际值
        if (g_glyph_recheck_logged == 0) {
            g_glyph_recheck_logged = 1;
            lzt_core::log_write("[CMP] glyph recheck: cache=%p size=%d limit=%d growth=%d",
                                reinterpret_cast<void*>(cache), *pSize, *pLimit,
                                *reinterpret_cast<uint8_t*>(cache + GLYPH_OFF_ALLOW_GROW));
        }
        return;
    }
    if (*pLimit != 4) *pLimit = 4;
    // 尺寸兜底仅在指令补丁未生效时使用（补丁晚于首次构造）。
    // 指令补丁已生效时，新建对象本就是 2048，无需也无法安全改写既有分配。
    if (g_glyph_size_state != 1 && *pSize != 2048) {
        lzt_core::log_write("[CMP] glyph recheck: size patch missed first ctor; "
                            "existing cache size=%d (left unchanged, "
                            "next init will be 2048)", *pSize);
    }
    *reinterpret_cast<uint8_t*>(cache + GLYPH_OFF_ALLOW_GROW) = 1;
    if (g_glyph_recheck_logged == 0) {
        g_glyph_recheck_logged = 1;
        lzt_core::log_write("[CMP] glyph recheck: existing cache %p fixed "
                            "(size=%d limit=%d growth=1)",
                            reinterpret_cast<void*>(cache),
                            *pSize, *pLimit);
    }
}



static void start_glyph_recheck(uintptr_t base) {
    std::thread([base]() {
        for (int i = 0; i < kGlyphRecheckAttempts; ++i) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kGlyphRecheckIntervalMs));
            glyph_cache_recheck_once(base);
            if (g_glyph_recheck_logged) return;   // 命中一次即可收工
        }
        lzt_core::log_write("[CMP] glyph recheck: gave up (EAText not ready)");
    }).detach();
}

// ---------------- 组件补丁二：图鉴翻页刷新字形缓存 ----------------
// 覆盖"小图鉴条目切换/翻页"与"图鉴 Tab 切换"两个入口，切换视图前清+重建字形缓存：
//   AlmanacObjectChooser::Select   sub_869258(a1, a2)   a2=目标条目索引（翻页/切换对象）
//   AlmanacWidget::ButtonDepress   sub_86EC40(a1, a2)   a2: 0=植物/1=僵尸/2=升级（Tab 切换）
typedef void (*Depress2_t)(uintptr_t, int);
Depress2_t oAlmanacWidget = nullptr;    // sub_86EC40
Depress2_t oAlmanacChooser = nullptr;   // sub_869258
alignas(32) uint8_t g_tr_almanac_w[lzt_hooks::TRAMPOLINE_SIZE];
alignas(32) uint8_t g_tr_almanac_c[lzt_hooks::TRAMPOLINE_SIZE];

// 字形缓存「清 + 重建」组合（对齐 iOS ClearPrimeGlyphCache + rebuild）：
//   g     = sub_1713288()          EAText 全局指针，+8 = PrimeGlyphCache 实例
//   清    = sub_1714090(g)         = sub_17F3DF4(*(g+8)) 销毁 image render data
//   重建  = sub_1714098(g)         = sub_17F3E70(*(g+8)) 重新构建渲染数据
// 二者即引擎自身 sub_153DEB4 重置字形缓存时成对调用的序列。
typedef void (*GlyphFlush_t)(uintptr_t);
GlyphFlush_t g_glyphClear = nullptr;
GlyphFlush_t g_glyphRebuild = nullptr;

// 执行一次字形缓存刷新（清 + 重建，带 Debug 日志）。
void do_glyph_flush(const char* tag, int a2) {
    if constexpr (!lzt_config::kEnableAlmanacGlyphFlush) return;
    if (!g_eaTextGlobal || !g_glyphClear || !g_glyphRebuild) return;
    uintptr_t g = g_eaTextGlobal();
    if (!g) {
        LZT_DBG_LOG("[CMP] %s a2=%d: EAText global NULL", tag, a2);
        return;
    }
    uintptr_t cache = *reinterpret_cast<uintptr_t*>(g + 8);
    if (!cache) {
        LZT_DBG_LOG("[CMP] %s a2=%d: glyph cache ptr NULL", tag, a2);
        return;
    }
    g_glyphClear(g);
    g_glyphRebuild(g);
    LZT_DBG_LOG("[CMP] %s a2=%d: glyph cache flushed (cache=%p)", tag, a2,
                reinterpret_cast<void*>(cache));
}

__attribute__((noinline))
void hk_almanac_widget(uintptr_t a1, int a2) {
    LZT_DBG_LOG("[CMP] almanac widget depress a2=%d widget=%p", a2,
                reinterpret_cast<void*>(a1));
    do_glyph_flush("almanac-widget", a2);
    oAlmanacWidget(a1, a2);
}

__attribute__((noinline))
void hk_almanac_chooser(uintptr_t a1, int a2) {
    LZT_DBG_LOG("[CMP] almanac chooser select a2=%d chooser=%p", a2,
                reinterpret_cast<void*>(a1));
    do_glyph_flush("almanac-chooser", a2);
    oAlmanacChooser(a1, a2);
}

// 应用补丁 A（尺寸）+ B（页上限），并安装 ctor 返回点 hook（补丁 C 的载体）。
// 全部幂等：指令已是对应目标值时只记一次日志，不重复写。
void install_glyph_cache(uintptr_t base) {
    // 幂等：三项都已就绪则直接返回。用独立标志而非 oGlyphCacheCtor——
    // hook 失败时 oGlyphCacheCtor 仍为 null，但补丁 A/B 与复检线程已生效。
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    // A. 尺寸决策：CSEL W3,W9,W8,LT -> MOV W3,#2048
    //    w3 在 sub_1713384 内唯一消费方是 ctor 的 a4（= cache+0xA4）。
    patch_insn("glyph size -> 2048", base + OFF_GLYPH_CACHE_SIZE_INSN,
               kInsnCselW3, kInsnMovW3_2048, &g_glyph_size_state);

    // B. 页数上限：MOV W1,#1 -> MOV W1,#4  【已停用，见 README「真机回归」】
    //    原因：真机在「尺寸未生效 + 页上限 4 + 页增长开启」下闪退；
    //    页上限单独放开会把「页满→优雅返回」变成「继续尝试分配」。
    if constexpr (kGlyphAllowPageGrowth) {
        patch_insn("glyph page limit -> 4", base + OFF_GLYPH_CACHE_PAGE_LIMIT,
                   kInsnMovW1_1, kInsnMovW1_4, &g_glyph_limit_state);
    }

    // C. 页增长开关：hook ctor，调用原函数后写 cache+0x134。
    //    只改字段、不改入参——改入参（a4）才是此前堆破坏的成因。
    //    ctor 仅在 EAText 初始化时调用一次（全库唯一调用点 0x1713414）。
    void* orig = nullptr;
    if (lzt_hooks::install(base + OFF_GLYPH_CACHE_CTOR,
                           reinterpret_cast<void*>(&hk_glyph_cache_ctor),
                           &orig, g_tr_glyph_ctor)) {
        oGlyphCacheCtor = reinterpret_cast<GlyphCacheCtor_t>(orig);
        lzt_core::log_write("[CMP] glyph cache ctor hook installed @+0x%lx",
                            static_cast<unsigned long>(OFF_GLYPH_CACHE_CTOR));
    } else {
        lzt_core::log_write("[CMP] glyph cache ctor hook install FAILED "
                            "(页增长不可用，图集仍限 1 页)");
    }

    // 兜底：处理「补丁安装前 EAText 已初始化」的时序竞态。
    g_eaTextGlobal = reinterpret_cast<EaTextGlobal_t>(base + OFF_EATEXT_GLOBAL_GET);
    start_glyph_recheck(base);
}

void install_almanac_flush(uintptr_t base) {
    g_eaTextGlobal = reinterpret_cast<EaTextGlobal_t>(base + OFF_EATEXT_GLOBAL_GET);
    g_glyphClear = reinterpret_cast<GlyphFlush_t>(base + OFF_GLYPH_CACHE_CLEAR);
    g_glyphRebuild = reinterpret_cast<GlyphFlush_t>(base + OFF_GLYPH_CACHE_REBUILD);
    void* orig = nullptr;
    if (oAlmanacWidget == nullptr &&
        lzt_hooks::install(base + OFF_ALMANAC_WIDGET_DEPRESS,
                           reinterpret_cast<void*>(&hk_almanac_widget),
                           &orig, g_tr_almanac_w)) {
        oAlmanacWidget = reinterpret_cast<Depress2_t>(orig);
        lzt_core::log_write("[CMP] almanac widget hook installed @+0x86EC40");
    }
    orig = nullptr;
    if (oAlmanacChooser == nullptr &&
        lzt_hooks::install(base + OFF_ALMANAC_CHOOSER_SELECT,
                           reinterpret_cast<void*>(&hk_almanac_chooser),
                           &orig, g_tr_almanac_c)) {
        oAlmanacChooser = reinterpret_cast<Depress2_t>(orig);
        lzt_core::log_write("[CMP] almanac chooser hook installed @+0x869258");
    }
}

// ============================================================
// 语言标识解析接管（移植自 iOS lzt_main_ios.cpp「v23 接管语言标识解析」）
//
// 优先级：Flag（files/LanguageFlag）> 游戏自身配置 > 系统语言
//
// iOS 的三条规则（原样保留）：
//   ① 游戏传入 FourCC 时与 Flag 比对：相同 → 放行原版；不同 → 用 Flag
//   ② Flag 不存在 → 放行原版；等用户配置可读后再补一次切换
//   ③ 用户配置也不存在 → 放行
//
// 关键设计（同 iOS）：hook 内部【绝不访问语言表】，只读一个已算好的
// FourCC 与入参比对，因此无时机要求、无锁、无分配。
//
// Android 的必要性（真机探针 2026-11-18 两次启动一致）：
//   [LANGP] B(sub_11F475C) call#1 langmgr=EN-US
//   [LANGP] A(sub_AC8418)  call#1 langmgr=EN-US
//   Language switch: quiet window over, ...
//   Language switch: wrote 0x454E5553 -> 0x5A48434E
// 游戏在 ~1s 就用 EN-US 拼好了表名，抢跑线程要等 8s 静默期才写——
// 时机上必然太晚，这就是「重启变英文」的根因。
//
// 劫持点：sub_16A8A4C 是纯函数（FourCC → "en-us"），全库仅两处调用，
// 均为此用途。对应 iOS 的 0x1AC6D50。
//
// 为何不改 LangMgr+0x1E4 字段：全库 49 处读者（含 RSB 记录 lang2 匹配
// sub_16B2DEC），写它会重演 v6.1 进游戏崩溃。
// ============================================================
static std::atomic<uint32_t> g_flagFourcc{0};   // Flag 的 FourCC；0 = 无 Flag
static std::atomic<bool> g_langReady{false};    // 允许接管（Flag 已读）
static std::atomic<unsigned> g_overrideCount{0};   // 覆盖次数（热路径只计数）
static std::atomic<uint32_t> g_lastIncoming{0};    // 最近一次被覆盖的入参

typedef void (*FourccToString_t)(void* out, uint32_t fourcc);
alignas(32) uint8_t g_tr_fourcc[lzt_hooks::TRAMPOLINE_SIZE];

extern "C" uint32_t lzt_android_pick_fourcc(uint32_t incoming) {
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
            lzt_core::log_write("[LANGX] bad flag fourcc 0x%08X, ignore", flag);
        } else if (flag == incoming) {
            static std::atomic<int> s_n{0};
            if (s_n.fetch_add(1) < 3) {
                lzt_core::log_write("[LANGX] flag==incoming 0x%08X, pass through",
                                    incoming);
            }
            return incoming;                 // 规则①：相同 → 放行
        } else {
            // 热路径：只计数，不在游戏资源加载链上做 I/O（fprintf 会取锁/分配）。
            // 明细由语言模块在启动完成后一次性输出（见 log_override_stats）。
            g_overrideCount.fetch_add(1, std::memory_order_relaxed);
            g_lastIncoming.store(incoming, std::memory_order_relaxed);
            return flag;                     // 规则①：不同 → 用 Flag
        }
    }

    // 规则②③：Flag 不存在 → 放行原版（补切由语言模块在主界面后触发）。
    return incoming;
}

// naked shim：x8 = 输出缓冲（sret），w1 = FourCC。C++ 签名无法表达 x8，
// 故用汇编保存/恢复 x8，只替换 w1（与 iOS hkFourccToString 同构）。
extern "C" __attribute__((naked)) void hkFourccToString() {
    __asm__ volatile(
        "stp x29, x30, [sp, #-32]!\n"
        "str x8, [sp, #16]\n"                     // 保存输出缓冲（x8 非标准参数）
        "mov w0, w1\n"                            // 入参：w0 = fourcc
        "bl  lzt_android_pick_fourcc\n"           // 返回：w0 = 最终 fourcc
        "mov w1, w0\n"
        "ldr x8, [sp, #16]\n"                     // 恢复输出缓冲
        "ldp x29, x30, [sp], #32\n"
        "adrp x9, :got:oFourccToString\n"
        "ldr  x9, [x9, :got_lo12:oFourccToString]\n"  // x9 = &oFourccToString
        "ldr  x9, [x9]\n"                            // x9 = oFourccToString（函数地址）
        "cbz  x9, 9f\n"                              // 防御：未捕获原函数则直接返回
        "br   x9\n"                                   // 尾调用原函数
        "9:\n"
        
        "ret\n"
    );
}


} // namespace

// 安装入口：必须对 component_patches_init 可见，故放在匿名 namespace 之外。
// 安装（幂等）。必须在任何语言表加载之前调用。
void install_fourcc_override(uintptr_t base) {
    if (base == 0 || oFourccToString != nullptr) return;
    void* orig = nullptr;
    if (lzt_hooks::install(base + OFF_FOURCC_TO_STRING,
                           reinterpret_cast<void*>(&hkFourccToString),
                           &orig, g_tr_fourcc)) {
        oFourccToString = reinterpret_cast<FourccToString_t>(orig);
        lzt_core::log_write("[LANGX] fourcc override hook installed @+0x%lx",
                            static_cast<unsigned long>(OFF_FOURCC_TO_STRING));
    } else {
        lzt_core::log_write("[LANGX] fourcc override hook install FAILED");
    }
}

// Flag 已读到 → 设置比对基准并允许接管（规则①②的入口）。
// fourcc==0 表示无 Flag：此时规则②生效，hook 一律放行原版。
extern "C" void lzt_android_set_flag_fourcc(uint32_t fourcc) {
    g_flagFourcc.store(fourcc, std::memory_order_release);
    lzt_core::log_write("[LANGX] flag fourcc=0x%08X (hook=%d)",
                        fourcc, oFourccToString != nullptr ? 1 : 0);
}

// 供语言模块在启动完成后输出一次统计（避免热路径 I/O）。
extern "C" void lzt_android_log_override_stats(void) {
    const unsigned n = g_overrideCount.load(std::memory_order_relaxed);
    const uint32_t last = g_lastIncoming.load(std::memory_order_relaxed);
    const uint32_t flag = g_flagFourcc.load(std::memory_order_relaxed);
    lzt_core::log_write("[LANGX] override stats: count=%u lastIncoming=0x%08X "
                        "flag=0x%08X", n, last, flag);
}

extern "C" void lzt_android_mark_lang_ready(void) {
    if (!g_langReady.exchange(true, std::memory_order_acq_rel)) {
        lzt_core::log_write("[LANGX] lang ready, flag=0x%08X",
                            g_flagFourcc.load());
    }
}

#endif // __aarch64__

void component_patches_init() {
#ifdef __aarch64__
    lzt_settings::register_engine_ready(&install_fourcc_override);
    lzt_settings::register_engine_ready(&install_glyph_cache);
    lzt_settings::register_engine_ready(&install_almanac_flush);
    lzt_core::log_write("[CMP] component patches registered: "
                        "fourcc override + glyph cache size + almanac flush");
#else
    lzt_core::log_write("[CMP] component patches: ARM32 not supported");
#endif
}
