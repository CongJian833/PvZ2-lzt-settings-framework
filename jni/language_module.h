#ifndef LANGUAGE_MODULE_H
#define LANGUAGE_MODULE_H

#include <stddef.h>   // size_t

// ============================================================
// language_module — "语言"设置模块
//
// Settings UI 框架的第二个接入模块：位于 Build Version 之后第一
// 位，提供 English / 简体中文 两个互斥选项。
//
// 语言切换（v3.3）：选择持久化到标志文件（files/LanguageFlag），
// 重启生效——模块自带 constructor 抢跑线程在游戏消费语言字段前
// 将其直写为目标 locale（机制详见 language_module.cpp 文件头）。
// ============================================================

// 注册本模块到 Settings UI 框架（须在 engine_install 之前调用）。
bool language_module_init();

// ---- 文本查询（供 iOS 物化层实现「切换语言后 UI 文本立即更新」）----
//
// iOS 上游戏按语言标识加载 LawnStrings 后常驻内存，改标识不会触发重载，
// 因此本模块自行解析 CDN 里对应语言的 LawnStrings 表，并在控件物化时把
// 「文本键」直接替换成「该语言的显示文本」。
//
// 用法：控件创建前用它查一次；命中则把文本传给控件，未命中继续用原键。
//   key   — 游戏文本键（形如 L"[LANGUAGE_ENGLISH]"）
//   out   — 输出缓冲；命中时写入显示文本（NUL 结尾）
//   cap   — 缓冲容量（wchar_t 个数）
// 返回 true 表示命中。
bool language_lookup_text(const wchar_t* key, wchar_t* out, size_t cap);

// 按当前语言（或指定 locale）加载 LawnStrings 表；成功返回条目数，0 表示不可用。
// 切换语言时由模块内部调用；也可由外部在需要时主动刷新。
size_t language_load_lawnstrings(const char* locale);

// ---- 语言表预热（供平台层在依赖就绪后主动调用）----
//
// 语言表的数据包兜底来源依赖三个迟到输入（libPVZ2 base、
// 游戏 std::string 构造器、RSB 挂载）。模块 constructor 执行时它们
// 尚未就绪，首次加载必然失败并置入 3 秒节流；而 Tab 可见性判定
// （hook_create_tab）只触发一次，若那一刻仍未就绪，Tab 会被误判为
// 「无来源」而永不显示。
//
// 平台层在自身依赖就绪后（iOS：wait_and_start 末尾）调用本函数做一次
// 预热：成功则 Tab 判定直接成立；失败也不留副作用（TableLoader 自行
// 置节流，后续页面重建时会再评估）。
//
// force=true 绕过节流强制重测——用于「通道刚就绪」的时机，因为模块
// constructor 那次失败置入的 3 秒节流通常仍未到期，走 ensure() 会被
// 节流直接挡掉而白跑一次。force=false 为普通惰性加载。
bool language_table_prewarm(bool force);

#endif // LANGUAGE_MODULE_H
