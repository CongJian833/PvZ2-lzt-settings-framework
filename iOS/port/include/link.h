/*
 * iOS 平台垫片：link.h
 *
 * 目的：让共享源码（language_module.cpp / cdn_load_rton.cpp）在 iOS 上零改动编译。
 * 安卓侧用 dl_iterate_phdr 遍历已加载的 so 并按名字找「游戏库」的加载基址；
 * iOS 没有 link.h，改由 dyld 接口提供等价能力（实现见 iOS/src/lzt_core_ios.mm）。
 *
 * 名字对照：安卓的游戏库是 libPVZ2.so；iOS 的游戏代码在主可执行文件
 * （PvZ2.app/PvZ2）里。为让共享源码里 strstr(name, "libPVZ2.so") 的写法无需
 * 改动，本垫片的实现会把【主程序镜像】额外以 "libPVZ2.so" 这个名字报告一次
 * （其 dlpi_addr 即主程序加载偏移 slide，语义与安卓的 dlpi_addr 一致）。
 */
#ifndef LZT_IOS_LINK_H_SHIM
#define LZT_IOS_LINK_H_SHIM

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dl_phdr_info {
    uintptr_t   dlpi_addr;    // 加载偏移（load bias）；安卓为 so 的基址
    const char *dlpi_name;    // 镜像路径（主程序额外以 "libPVZ2.so" 报告一次）
};

typedef int (*lzt_dl_iterate_cb)(struct dl_phdr_info *info, size_t size, void *data);

// 遍历已加载镜像；回调返回非 0 时停止并返回该值。
int dl_iterate_phdr(lzt_dl_iterate_cb callback, void *data);

#ifdef __cplusplus
}
#endif

#endif // LZT_IOS_LINK_H_SHIM
