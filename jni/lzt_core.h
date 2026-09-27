#ifndef LZT_CORE_H
#define LZT_CORE_H

// ============================================================
// settingsframework 公共基础层
//
// 供三类使用者共用：
//   1. lzt_settings_framework.cpp — 相机/对齐 hook 主模块
//   2. settings/settings_*    — Settings UI 框架引擎
//   3. view_angle_module.cpp  — 业务模块
//
// 职责边界（仅此三项，不含任何游戏业务语义）：
//   - libPVZ2.so 稳定基址：进程级单例，applyHooks 阶段写入，
//     所有模块经此换算游戏地址
//   - 崩溃诊断阶段号：各模块在进入关键序列前推进 stage，
//     崩溃时随 NATIVE_CRASH 日志输出用于定位崩溃点
//   - 双通道日志：logcat(tag=settingsframework) +
//     /sdcard/Android/data/<pkg>/files/settingsframework.log（每次启动覆盖）
// ============================================================

#include <cstdint>
#include <cstdarg>
#include <atomic>
#include <csignal>

namespace lzt_core {

// ---- libPVZ2.so 稳定基址 ----
// 未就绪时返回 0；调用方需自行判空。
void set_base(uintptr_t base);
uintptr_t base();

// 引用访问器：仅供旧代码兼容包装绑定，新代码一律用 set_base/base。
std::atomic<uintptr_t>& base_ref();

// ---- 崩溃诊断阶段号 ----
// 约定：stage 编号区间由各使用者自留（如主模块相机链 1~99、
// Settings 构建流水线 10~150），互不重叠以便区分崩溃来源。
void set_stage(int stage);
int stage();

// 引用访问器：供旧代码 `g_crash_stage = N` 写法零改动迁移。
volatile std::sig_atomic_t& stage_ref();

// 安装致命信号处理器（SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGPE），
// 崩溃时输出 NATIVE_CRASH 行（含 stage 与 base）后恢复默认处理。
void install_crash_diagnostics();
void start_crash_handler_keepalive();   // v7.10:周期性重注册,对抗游戏后装的处理器

// 进程内存范围可访问性检查（解析 /proc/self/maps）。
// address..address+size 完全落在单个 r（writable 时要求 rw）映射内返回 true。
bool memory_range_accessible(uintptr_t address, size_t size, bool writable);

// ---- 进程信息（底层共用工具）----
// 当前进程包名：读取 /proc/self/cmdline 首段，进程内缓存（首次调用时读取）。
// 读取失败返回 ""（空串，调用方判空）。替代各模块自行读 cmdline 的重复实现。
const char* package_name();

// 构造 "<外部存储>/Android/data/<pkg>/files" 到 out；成功返回 true。
bool files_dir(char* out, size_t outSize);

// ---- 日志 ----
// 进程内调用一次；读取 /proc/self/cmdline 定位包名并建日志目录。
void log_init();

// 格式化输出到 logcat 与本地文件（立即 fflush 防崩溃丢日志）。
void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_write_v(const char *fmt, va_list ap);

} // namespace lzt_core

#endif // LZT_CORE_H
