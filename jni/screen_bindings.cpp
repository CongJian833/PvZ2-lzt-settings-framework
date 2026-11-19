// ============================================================
// screen_bindings.cpp — 屏幕本地化绑定（见头文件说明）
//
// 迁移来源：v7.33 该机制原先内联在 settings_framework.cpp（框架耦合业务）。
// v7.37 移出到本层——框架只提供两个通用注册点：
//   register_refresher()    切语言时按注册顺序调用（刷新已加载文本）
//   register_engine_ready() 引擎就绪（基址可用）后调用一次（装模块自身 hook）
//
// 日志前缀沿用 [SFW]，保持既有真机日志过滤习惯不变。
// ============================================================

#include "screen_bindings.h"

#include "lzt_settings_framework_config.h"
#include "settings/settings_framework.h"
#include "lzt_core.h"
#include "lzt_hooks.h"
#include "offsets.h"

#ifdef __aarch64__

#include <cctype>

namespace {

// ---- 主界面 PersistentMessage 横幅（v7.33 机制，v7.37 迁入本层）----
// hook sub_1301B08（横幅文本重建）记录最新横幅 a1（带 vtable 存活校验）；
// 切语言时对其重调重建——文本经 localizeKey 用当前语言取。机制级、不硬编码内容。
typedef void (*BannerRebuild_t)(uintptr_t);
BannerRebuild_t oBanner = nullptr;
alignas(32) uint8_t g_tr_banner[lzt_hooks::TRAMPOLINE_SIZE];
uintptr_t g_bannerA1 = 0;
uintptr_t g_bannerVt = 0;

__attribute__((noinline))
void hk_banner(uintptr_t a1) {
    if (oBanner) oBanner(a1);
    if (a1 && lzt_core::memory_range_accessible(a1, 8, false)) {
        g_bannerA1 = a1;
        g_bannerVt = *reinterpret_cast<uintptr_t*>(a1);
        lzt_core::log_write("[SFW] banner captured a1=%p", (void*)a1);
    }
}

// 切语言时重建横幅（vtable 校验防悬垂）
void refresh_persistent_banner() {
    if (!oBanner || !g_bannerA1 ||
        !lzt_core::memory_range_accessible(g_bannerA1, 8, false) ||
        *reinterpret_cast<uintptr_t*>(g_bannerA1) != g_bannerVt) {
        return;
    }
    oBanner(g_bannerA1);
    lzt_core::log_write("[SFW] persistent banner refreshed");
}

// 引擎就绪回调：安装横幅捕获 hook（幂等——引擎重装时直接返回）
void install_banner_hook(uintptr_t base) {
    if (oBanner != nullptr) return;
    void* orig = nullptr;
    if (lzt_hooks::install(base + OFF_PERSISTENT_BANNER_REBUILD, (void*)hk_banner,
                           &orig, g_tr_banner)) {
        oBanner = reinterpret_cast<BannerRebuild_t>(orig);
        lzt_core::log_write("[SFW] banner hook installed @+0x1301B08");
    } else {
        lzt_core::log_write("[SFW] banner hook install FAILED");
    }
}

// ---- News 页本地化：自定义语言短码兜底 ----
// 根因：News 取键链 sub_A50A28 → sub_98A694 把当前语言 FourCC 映射为两字母
// key（硬编码白名单 en/de/fr/it/pt/es）；自定义语言（如 zh-cn）不在白名单，
// 产出空串 → sub_145BE84/sub_145CAC0 用空键查 LocalizedData → 空白。
// 修复：hook sub_145BE84（News 专用取键，全工程仅被 News 构建链调用），
// 当 key 为空时改用当前 locale 的第一段后缀（ZH-CN → zh）作替身键传入，
// 命中自定义语言的 LocalizedData 条目。
typedef void* (*NewsSelectFn)(void*, void*);
NewsSelectFn oNewsSelect = nullptr;
alignas(32) uint8_t g_tr_news[lzt_hooks::TRAMPOLINE_SIZE];

// 取当前语言的四字母短码（小写两字母，如 "zh"/"en"）。
// 来源同 language_module：LangMgr(+0x1E4) 的 FourCC（大端打包，如
// "ZH-CN"=0x5A48434E，'-' 被省略），取前两字节小写即第一段后缀。
bool current_lang_short(char* out, size_t n) {
    if (n < 3) return false;
    const uintptr_t base = lzt_core::base();
    if (base == 0) return false;
    uintptr_t disp = *reinterpret_cast<volatile uintptr_t*>(base + OFF_G_DisplayInfo);
    if (disp == 0) return false;
    uintptr_t mgr = *reinterpret_cast<volatile uintptr_t*>(disp + DISPLAYINFO_LANGMGR);
    if (mgr == 0) return false;
    uint32_t fourcc = *reinterpret_cast<volatile uint32_t*>(mgr + LANGMGR_LOCALE_ID);
    if (fourcc == 0) return false;
    char c0 = static_cast<char>((fourcc >> 24) & 0xFF);
    char c1 = static_cast<char>((fourcc >> 16) & 0xFF);
    if (!std::isalpha(static_cast<unsigned char>(c0)) ||
        !std::isalpha(static_cast<unsigned char>(c1))) {
        return false;
    }
    out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(c0)));
    out[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(c1)));
    out[2] = 0;
    return true;
}

// News 取键兜底：引擎对本作自定义语言（zh-cn 等）产出空 key。命空时改用
// 当前 locale 第一段后缀（ZH-CN → "zh"）作为替身键传入——不改写调用方内存
// （替身仅为本次查询构建；sub_145BE84 命中/新建节点时按内容拷贝键）。
// libc++ 短串 SSO：byte0=2*len，数据自 +1。
__attribute__((noinline))
void* hk_news_select(void* entry, void* key) {
    if constexpr (lzt_config::kEnableNewsLocalization) {
        if (key != nullptr) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(key);
            const bool is_long = (p[0] & 1) != 0;
            const size_t len = is_long ? *reinterpret_cast<const size_t*>(p + 8)
                                       : static_cast<size_t>(p[0] >> 1);
            if (len == 0) {
                char s[4];
                if (current_lang_short(s, sizeof s)) {
                    unsigned char sub[24] = {0};
                    sub[0] = static_cast<unsigned char>(2 * 2);   // 长度 2
                    sub[1] = static_cast<unsigned char>(s[0]);
                    sub[2] = static_cast<unsigned char>(s[1]);
                    lzt_core::log_write("[SFW] news locale fallback: empty key -> %s", s);
                    return oNewsSelect(entry, sub);
                }
            }
        }
    }
    return oNewsSelect(entry, key);
}

// 引擎就绪回调：安装 News 取键兜底 hook（幂等）
void install_news_locale_hook(uintptr_t base) {
    if (oNewsSelect != nullptr) return;
    void* orig = nullptr;
    if (lzt_hooks::install(base + OFF_NEWS_SELECT_LOCALE, (void*)hk_news_select,
                           &orig, g_tr_news)) {
        oNewsSelect = reinterpret_cast<NewsSelectFn>(orig);
        lzt_core::log_write("[SFW] news locale hook installed @+0x145BE84");
    } else {
        lzt_core::log_write("[SFW] news locale hook install FAILED");
    }
}

} // namespace

#endif // __aarch64__

void screen_bindings_init() {
#ifdef __aarch64__
    lzt_settings::register_refresher(&refresh_persistent_banner);
    lzt_settings::register_engine_ready(&install_banner_hook);
    lzt_settings::register_engine_ready(&install_news_locale_hook);
    lzt_core::log_write("[SFW] screen bindings registered: persistent banner + news locale");
#else
    lzt_core::log_write("[SFW] screen bindings: ARM32 not supported (restart applies)");
#endif
}