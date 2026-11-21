// ============================================================
// view_angle_module.cpp — "视角"设置模块
//
// Settings UI 框架的第一个接入模块。本文件拥有视角业务的全部：
//   - Tab 声明（标题键/图标/锚点槽位）
//   - 页面内容编排（prompt 随状态切换 + 两个互斥 checkbox）
//   - 状态持久化（UseHighViewAngle，经框架可选工具 config）
//   - 游戏内联动（高视角挂 BoardZoom2 强制缩放 hook，低视角卸载）
//
// 框架对以上语义零感知——本模块就是"如何写一个设置模块"的范例。
//
/// === 状态语义 ===
///   true  = 高视角：BoardZoom2 hook 挂载，board[280] 恒被强制为 1.0
///   false = 低视角：BoardZoom2 hook 卸载，保留游戏原生低视角缩放；
///           对齐修正 Hook（BoardLayout）始终生效，与本开关无关
///
/// === 初始化时序（与历史版本一致的三重保险） ===
///   init 注册模块 + 启动重试线程；配置对象未就绪期间按默认高视角，
///   不锁定缓存；就绪后重新读取真实配置并同步 hook 挂载状态。
// ============================================================

#include "view_angle_module.h"

#include "settings/settings_framework.h"
#include "lzt_core.h"
#include "offsets.h"

#include <atomic>
#include <chrono>
#include <thread>

// 主文件导出的视角 hook 挂载门面（机制在主模块，业务调用在此）
extern bool lzt_view_hooks_set_enabled(bool enable);

namespace {

// ---- 文案键（与历史版本逐字节一致）----
constexpr wchar_t kTitleKey[]    = L"[VIEW_ANGLE_TITLE]";   // Tab 标题
constexpr wchar_t kPromptHigh[]  = L"[VIEW_HIGH_PROMPT]";   // 高视角说明文本
constexpr wchar_t kPromptLow[]   = L"[VIEW_LOW_PROMPT]";    // 低视角说明文本
constexpr wchar_t kLabelHigh[]   = L"[VIEW_HIGH]";          // 高视角 checkbox 标签
constexpr wchar_t kLabelLow[]    = L"[VIEW_LOW]";           // 低视角 checkbox 标签

constexpr const char* kConfigKey = "UseHighViewAngle";      // 持久化键（勿改，影响老存档）

// ---- 状态缓存（UI 线程写、游戏逻辑线程/重试线程读）----
std::atomic<bool> g_stateCache{true};     // 默认高视角，与 iOS 一致
std::atomic<bool> g_stateLoaded{false};

// 配置对象（g_DisplayInfo 解引用）是否就绪
bool config_object_ready() {
    uintptr_t base = lzt_core::base();
    if (base == 0) return false;
    return *reinterpret_cast<uintptr_t*>(base + OFF_G_DisplayInfo) != 0;
}

// 读取状态：首次从偏好后端加载到缓存；配置对象未就绪期间返回默认
// 高视角且不锁定缓存（避免启动期过早读取把用户选择覆盖为默认值）
bool read_state() {
    if (g_stateLoaded.load(std::memory_order_acquire)) {
        return g_stateCache.load(std::memory_order_acquire);
    }
    if (!config_object_ready()) {
        return g_stateCache.load(std::memory_order_acquire);
    }
    bool value = lzt_settings::config::read_bool(kConfigKey, true,
                                                 CONFIG_USE_HIGH_VIEW_ANGLE);
    g_stateCache.store(value, std::memory_order_release);
    g_stateLoaded.store(true, std::memory_order_release);
    lzt_core::log_write("View Angle get_state: loaded from config = %d", value);
    return value;
}

// 按当前状态挂载/卸载高视角缩放 hook（高=挂载，低=卸载）
void sync_hooks() {
    bool high = read_state();
    lzt_view_hooks_set_enabled(high);
    lzt_core::log_write("sync_view_hooks: highView=%d", high);
}

// 写状态：变化时持久化并联动 hook；无变化直接返回
void write_state(bool high) {
    if (!config_object_ready()) {
        lzt_core::log_write("View Angle set_state: config object null");
        return;
    }
    if (read_state() == high) {
        lzt_core::log_write("View Angle set_state: no change (current=%d)", high);
        return;
    }
    lzt_settings::config::write_bool(kConfigKey, high, CONFIG_USE_HIGH_VIEW_ANGLE);
    g_stateCache.store(high, std::memory_order_release);
    g_stateLoaded.store(true, std::memory_order_release);
    sync_hooks();
    lzt_core::log_write("View Angle set_state: saved to config = %d", high);
}

// ---- checkbox 点击回调（checked=true 表示该选项被选中；互斥由重建刷新）----
void on_click_low(bool checked) {
    lzt_core::log_write("View Angle checkbox LOW clicked checked=%d", checked ? 1 : 0);
    if (checked) write_state(false);   // LOW = 低视角
}

void on_click_high(bool checked) {
    lzt_core::log_write("View Angle checkbox HIGH clicked checked=%d", checked ? 1 : 0);
    if (checked) write_state(true);    // HIGH = 高视角
}

// ---- 页面内容构建（每次创建/dirty 重建都会重新执行）----
// prompt 文案随状态切换：dirty 重建会重跑本函数，prompt 自然读到最新状态。
void build_page(lzt_settings::PageBuilder& b) {
    bool high = read_state();
    b.add_prompt(high ? kPromptHigh : kPromptLow);
    b.add_checkbox(kLabelLow, !high, &on_click_low);
    b.add_checkbox(kLabelHigh, high, &on_click_high);
}

} // namespace

// ============================================================
// 视角 hook 同步重试线程（迁移自主文件 start_view_hook_sync_retry）
// applyHooks 时配置对象可能未就绪，get_state 会返回默认高视角并挂
// 载 hook；若用户此前选过低视角，需在配置就绪后重新同步以卸载。
// ============================================================
static void start_hook_sync_retry() {
    std::thread([]() {
        for (int i = 0; i < 60; ++i) {
            if (i > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (!config_object_ready()) continue;   // 配置对象还没就绪，继续等待
            lzt_core::log_write("View hook sync: config object ready, syncing view hooks");
            sync_hooks();
            return;
        }
        lzt_core::log_write("View hook sync: timeout waiting for config object");
    }).detach();
}

bool view_angle_module_init() {
    lzt_settings::ModuleDesc desc{};
    desc.titleKey = kTitleKey;
    desc.iconNormalOffset = OFF_SettingsBuildVersionIconNormal;
    desc.iconSelectedOffset = OFF_SettingsBuildVersionIconSelected;
    desc.anchorTabId = SETTINGS_BUILDVERSION_ID;   // Build Version 之后
    desc.slotAfterAnchor = 2;                      // 第二位（第一位为 Language 模块）
    desc.requestedTabId = 0;                       // 由引擎自动分配（30 起）
    desc.onBuildPage = &build_page;

    if (!lzt_settings::register_module(desc)) {
        lzt_core::log_write("View Angle module register failed");
        return false;
    }
    // 配置就绪后按真实状态同步 hook 挂载（与历史版本时序一致）
    start_hook_sync_retry();
    lzt_core::log_write("View Angle module registered");
    return true;
}

// ---- 跨域查询接口（供相机对齐逻辑消费；实现转发到内部状态管理）----
bool view_angle_state_high() {
    return read_state();
}

void view_angle_sync_hooks() {
    sync_hooks();
}
