#ifndef LZT_SETTINGS_FRAMEWORK_CONFIG_H
#define LZT_SETTINGS_FRAMEWORK_CONFIG_H

#include "lzt_core.h"   // 统一诊断日志宏需要 lzt_core::log_write

// 唯一的编译期模式开关。
// false：正式版，仅保留运行与异常所需日志。
// true ：Debug 版，额外安装纯诊断 Hook、启动采样线程并输出详细轨迹。
// 交付源码必须恢复为 false；build_variants.py 只切换这一处。
namespace lzt_config {
inline constexpr bool kDebugMode = false;   // Debug 版诊断 hooks 存在启动期崩溃（AEF69C 等
                                            // 热路径遗留工具），独立问题，待后续专项排查
inline constexpr const char* kBuildMode = kDebugMode ? "DEBUG" : "RELEASE";

// v7.34：语言 Tab 空白页回退开关。默认关闭。
//   false：语言模块无 LANGUAGETYPES 来源、或来源中只有一种语言 enabled 时，
//          不创建"语言"Tab（不进入后续流程）。
//   true ：始终创建语言 Tab；无来源时保留空白页（旧行为）。
inline constexpr bool kEnableEmptyLangPageFallback = false;

// 快速读取通道开关（LANGUAGETYPES 来源中的 "CDN-json" 方式）。默认关闭。
//   false：来源表不含 CDN LANGUAGETYPES.json——Debug/Release 均不读取；
//   true ：纳入 CDN LANGUAGETYPES.json（明文 JSON 直投，开发期快速调试用）。
// 与 kDebugMode 相互独立：Debug 版不会自动开启本通道。
inline constexpr bool kEnableFastReadChannel = false;

// News 本地化短码兜底开关（见 screen_bindings.cpp「News 页本地化」）。
//   引擎把当前语言 FourCC 映射为两字母 key（en/de/fr/it/pt/es）；自定义语言
//   （如 zh-cn）不在其白名单 → 返回空 key → News 的 LocalizedData 查空键 → 空白。
//   本开关开启后，在 News 取键函数入口做兜底：空 key 时改写为 locale 第一段
//   后缀（zh-cn → zh），使 News 能对上自定义语言的 LocalizedData 条目。
inline constexpr bool kEnableNewsLocalization = true;

// --- 组件补丁开关（见 jni/components/README.md）---
// 一、强制所有设备字形渲染缓存为 2048：引擎按屏幕最小边选 512/1024/2048，
//     小屏落入 512/1024 导致字符缺失；开启后统一为 2048。
inline constexpr bool kEnableGlyphCacheSize2048 = true;
// 二、图鉴（Almanac）翻页时主动刷新字形缓存：翻页复用上一页脏缓存导致字符异常；
//     开启后在翻页入口（AlmanacObjectChooser::Select / AlmanacWidget::ButtonDepress）
//     前置调用字形缓存「清 + 重建」（对齐 iOS ClearPrimeGlyphCache + rebuild）。
inline constexpr bool kEnableAlmanacGlyphFlush = true;
}

// ============================================================
// 统一诊断日志门（全工程唯一定义；各文件不得再定义本地同义宏）
//   LZT_DBG_LOG(...)   输出一行诊断日志——Release 编译期消失（零调用）
//   LZT_DBG_ONLY(...)  执行一段诊断代码——Release 不执行（可多语句）
// 二者均以 kDebugMode 为门，与 Debug/Release 构建一一对应。
// ============================================================
#define LZT_DBG_LOG(...) \
    do { if constexpr (lzt_config::kDebugMode) { lzt_core::log_write(__VA_ARGS__); } } while (0)

#define LZT_DBG_ONLY(...) \
    do { if constexpr (lzt_config::kDebugMode) { __VA_ARGS__; } } while (0)

#endif