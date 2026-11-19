#ifndef LZT_COMPONENT_PATCHES_H
#define LZT_COMPONENT_PATCHES_H

// ============================================================
// component_patches — 组件补丁层（框架与业务模块之外的第三方组件修复）
//
// 定位：既不属于 settings 框架引擎，也不属于「视角/语言」业务模块，
//   而是针对游戏内**通用组件**（EAText 字形缓存、图鉴 Almanac 页面）的定点修复。
//   这类补丁跨模块共用，集中在本目录，避免污染框架与模块代码。
//
// 当前组件补丁：
//   1. glyph_cache_size   —— 强制所有设备字形渲染缓存为 2048（修复小屏字符缺失）
//   2. glyph_cache_flush  —— 图鉴（Almanac）翻页时主动刷新字形缓存（修复翻页脏缓存）
//
// 二者均由 lzt_settings_framework_config.h 的编译开关控制，经 register_engine_ready
// 在引擎就绪（基址可用）后安装；ARM32/正式版不编译实体。
// ============================================================

// 注册全部组件补丁到引擎就绪回调。须在 engine install 之前调用一次。
void component_patches_init();

#endif // LZT_COMPONENT_PATCHES_H
