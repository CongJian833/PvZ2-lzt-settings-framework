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