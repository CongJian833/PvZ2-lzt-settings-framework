#ifndef SCREEN_BINDINGS_H
#define SCREEN_BINDINGS_H

// ============================================================
// screen_bindings — 屏幕本地化绑定层（业务侧，非框架）
//
// 职责：把"某个具体屏幕上的文本需要随语言切换刷新"这件事注册到
// lzt_settings 框架的通用刷新注册表；框架只负责遍历调用，不感知
// 任何屏幕语义（v7.37 框架去业务化）。
//
// 当前绑定：
//   主界面 PersistentMessage 横幅 —— hook 其文本重建函数 sub_1301B08
//   捕获横幅对象，切语言时对其重调重建（文本按当前语言重新 localize）。
//
// 调用时机：lzt_settings_framework.cpp 中模块注册之后、applyHooks() 之前
// （横幅 hook 的安装由 register_engine_ready 回调在引擎就绪时完成）。
// ============================================================

void screen_bindings_init();

#endif // SCREEN_BINDINGS_H