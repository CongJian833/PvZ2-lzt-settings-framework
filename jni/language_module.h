#ifndef LANGUAGE_MODULE_H
#define LANGUAGE_MODULE_H

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

#endif // LANGUAGE_MODULE_H
