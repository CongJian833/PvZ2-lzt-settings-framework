// ============================================================
// component_patches.cpp — 组件补丁（框架与业务模块之外的通用组件修复）
//
// 见 components/README.md。当前两条：
//   一、glyph_cache_size   强制所有设备字形渲染缓存为 2048（修复小屏字符缺失）
//   二、glyph_cache_flush  图鉴（Almanac）翻页时主动触发字形缓存刷新（修复翻页脏缓存）
//
// 接入：经 lzt_settings::register_engine_ready 在引擎就绪（基址可用）后安装 hook；
// 仅 ARM64 编译实体，ARM32/正式版为空实现。
// ============================================================

#include "component_patches.h"

#include "lzt_settings_framework_config.h"
#include "settings/settings_framework.h"
#include "lzt_core.h"
#include "lzt_hooks.h"
#include "offsets.h"

#ifdef __aarch64__

namespace {

// ---------------- 组件补丁一：强制字形缓存 2048 ----------------
// PrimeGlyphCache 构造函数：sub_17F3560(cache, a2, a3, size) → 写 cache+0xA4。
// 上游 sub_1713384 按屏幕最小边选 512/1024/2048；此处统一改为 2048。
typedef void (*GlyphCacheCtor_t)(uintptr_t, uintptr_t, uintptr_t, int);
GlyphCacheCtor_t oGlyphCacheCtor = nullptr;
alignas(32) uint8_t g_tr_glyph_ctor[lzt_hooks::TRAMPOLINE_SIZE];

__attribute__((noinline))
void hk_glyph_cache_ctor(uintptr_t a1, uintptr_t a2, uintptr_t a3, int size) {
    if constexpr (lzt_config::kEnableGlyphCacheSize2048) {
        if (size != 2048) {
            LZT_DBG_LOG("[CMP] glyph cache size forced: %d -> 2048 (cache=%p)",
                        size, reinterpret_cast<void*>(a1));
            size = 2048;
        }
    }
    oGlyphCacheCtor(a1, a2, a3, size);
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
typedef uintptr_t (*EaTextGlobal_t)();
typedef void (*GlyphFlush_t)(uintptr_t);
EaTextGlobal_t g_eaTextGlobal = nullptr;
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

void install_glyph_cache_ctor(uintptr_t base) {
    if (oGlyphCacheCtor != nullptr) return;
    void* orig = nullptr;
    if (lzt_hooks::install(base + OFF_GLYPH_CACHE_CTOR,
                           reinterpret_cast<void*>(&hk_glyph_cache_ctor),
                           &orig, g_tr_glyph_ctor)) {
        oGlyphCacheCtor = reinterpret_cast<GlyphCacheCtor_t>(orig);
        lzt_core::log_write("[CMP] glyph cache ctor hook installed @+0x17F3560");
    } else {
        lzt_core::log_write("[CMP] glyph cache ctor hook install FAILED");
    }
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

} // namespace

#endif // __aarch64__

void component_patches_init() {
#ifdef __aarch64__
    lzt_settings::register_engine_ready(&install_glyph_cache_ctor);
    lzt_settings::register_engine_ready(&install_almanac_flush);
    lzt_core::log_write("[CMP] component patches registered: glyph cache size + almanac flush");
#else
    lzt_core::log_write("[CMP] component patches: ARM32 not supported");
#endif
}
