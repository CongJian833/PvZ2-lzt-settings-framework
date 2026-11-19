/*
 * iOS 平台垫片：android/log.h 兼容层
 *
 * 目的：让 Android 侧的原生源码（如 And64InlineHook.cpp）在 iOS 上
 * 零改动编译——只需在编译时把本目录加入头文件搜索路径，Android 的
 * #include <android/log.h> 即命中本文件。
 *
 * 实现：转发到 lzt_ios_log_write（定义于 iOS/src/lzt_core_ios.mm），
 * 该函数同时写沙盒文件与 os_log。
 */
#ifndef LZT_IOS_ANDROID_LOG_SHIM_H
#define LZT_IOS_ANDROID_LOG_SHIM_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 兼容 Android 的日志优先级常量 */
enum {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT = 1,
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG = 3,
    ANDROID_LOG_INFO = 4,
    ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6,
    ANDROID_LOG_FATAL = 7,
    ANDROID_LOG_SILENT = 8
};

/* 由 iOS 平台层实现 */
int lzt_ios_log_write(int prio, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* 与 Android 同名同形，源码无需改动 */
static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    __builtin_va_start(ap, fmt);
    /* 转调平台层（内部再格式化） */
    extern int lzt_ios_log_vwrite(int prio, const char *tag, const char *fmt, va_list ap);
    int r = lzt_ios_log_vwrite(prio, tag, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

static inline int __android_log_write(int prio, const char *tag, const char *text) {
    return lzt_ios_log_write(prio, tag, "%s", text ? text : "");
}

#ifdef __cplusplus
}
#endif

#endif /* LZT_IOS_ANDROID_LOG_SHIM_H */
