/* 兼容垫片：backtrace 在 iOS 11.0 才公开。
 *
 * 目的：让 dylib 的 minos 可以低于 11.0 而不至于 dyld 加载失败。
 * 做法：min_required < 110000 时把 backtrace 换成空实现，并让调用方
 *       得到 n = 0（各调用点都已处理 n = 0 的情况）。
 *
 * 影响面：仅影响 fileprobe 诊断与 lzt_dump_backtrace 的调用栈打印，
 * 这两处默认关闭（kEnableFileProbe = false），不影响任何游戏功能。
 *
 * 用法：在包含 <execinfo.h> 之后 include 本头；或直接依赖编译期宏。
 */
#ifndef LZT_BACKTRACE_SHIM_H
#define LZT_BACKTRACE_SHIM_H

#include <TargetConditionals.h>

#if defined(__APPLE__) && defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && \
    __IPHONE_OS_VERSION_MIN_REQUIRED < 110000

/* 低于 iOS 11：不引用 real backtrace 符号，避免 dyld 解析失败 */
#ifdef __cplusplus
extern "C" {
#endif
static inline int backtrace(void **buffer, int size) {
    (void)buffer; (void)size;
    return 0;   /* 无回溯，调用方按 n = 0 处理 */
}
#ifdef __cplusplus
}
#endif

#define LZT_BACKTRACE_UNAVAILABLE 1

#endif /* < iOS 11.0 */

#endif /* LZT_BACKTRACE_SHIM_H */
