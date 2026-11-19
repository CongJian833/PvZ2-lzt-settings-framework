#ifndef VIEW_ANGLE_MODULE_H
#define VIEW_ANGLE_MODULE_H

// ============================================================
// view_angle_module — "视角"设置模块
//
// Settings UI 框架的第一个接入模块：向设置界面注册"视角"Tab，
// 提供高/低视角切换与状态持久化，并按状态挂载/卸载高视角缩放 hook。
// 业务逻辑全部在本模块内，框架对视角语义零感知。
// ============================================================

// 注册本模块到 Settings UI 框架（须在 engine_install 之前调用）。
// 返回 false 表示注册被拒绝（详见日志）。
bool view_angle_module_init();

// ---- 跨域查询接口（相机对齐逻辑消费）----
// 当前是否高视角。配置对象未就绪期间返回默认值 true 且不锁定缓存。
bool view_angle_state_high();

// 按当前状态重新同步高视角缩放 hook 的挂载（高=挂载，低=卸载）。
void view_angle_sync_hooks();

#endif // VIEW_ANGLE_MODULE_H
