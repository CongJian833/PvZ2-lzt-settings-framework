#ifndef LZT_HOOKS_H
#define LZT_HOOKS_H

// ============================================================
// lzt_hooks — inline hook 安装门面（双架构）
//
// 实现在 lzt_settings_framework.cpp 各架构段（复用经过真机验证的机制）：
//   ARM64: And64InlineHook A64HookFunction（trampoline 由库管理）
//   ARM32: 自实现 ARM 模式 inline hook（LDR PC,[PC,#-4] 12 字节 patch，
//          需调用方提供 64 字节 32 对齐的 trampoline 缓冲）
//
// 供 Settings UI 框架引擎等次级模块安装自身 hook 使用；
// 存活保障策略与历史版本一致：基址变化时由主模块监控线程触发
// applyHooks 重装路径覆盖。
// ============================================================

#include <cstdint>
#include <cstddef>

namespace lzt_hooks {

constexpr size_t TRAMPOLINE_SIZE = 64;

// 安装 hook。trampoline_buf 仅 ARM32 需要（须 32 字节对齐、TRAMPOLINE_SIZE 大小，
// 生命周期与进程相同——建议静态分配）；ARM64 传 nullptr。
// 成功时 *orig_out 为可调用的原函数 trampoline，返回 true。
bool install(uintptr_t target, void* hook_fn, void** orig_out,
             uint8_t* trampoline_buf);

} // namespace lzt_hooks

#endif // LZT_HOOKS_H
