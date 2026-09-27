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

#include "settings/settings_framework.h"
#include "lzt_core.h"
#include "lzt_hooks.h"
#include "offsets.h"

#ifdef __aarch64__

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

} // namespace

#endif // __aarch64__

void screen_bindings_init() {
#ifdef __aarch64__
    lzt_settings::register_refresher(&refresh_persistent_banner);
    lzt_settings::register_engine_ready(&install_banner_hook);
    lzt_core::log_write("[SFW] screen bindings registered: persistent banner");
#else
    lzt_core::log_write("[SFW] screen bindings: ARM32 not supported (restart applies)");
#endif
}