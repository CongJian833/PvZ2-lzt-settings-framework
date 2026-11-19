/*
 * iOS 平台垫片：offsets.h
 *
 * 目的：让 Android/jni 下的共享源码（框架层与业务模块）在 iOS 上零改动编译。
 * 共享源码写的是 #include "offsets.h"（相对其所在目录解析）——为此 iOS 侧
 * 额外提供一个同名头文件，把其中的地址常量指向 iOS 的偏移表。
 *
 * 注意：本文件位于 iOS/port/include，编译时该目录排在源码目录之前，
 *       因此共享源码的 "offsets.h" 会命中这里。
 */
#ifndef LZT_IOS_OFFSETS_SHIM_H
#define LZT_IOS_OFFSETS_SHIM_H

#include "offsets_ios.h"

#endif // LZT_IOS_OFFSETS_SHIM_H
