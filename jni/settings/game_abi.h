#ifndef LZT_GAME_ABI_H
#define LZT_GAME_ABI_H

// ============================================================
// game_abi — 游戏 ABI / 表热重载通用工具（框架层）
//
// 收敛"与游戏 ABI 细节打交道的机械代码"，供任何做"表热重载"的模块复用，
// 避免各模块重抄寄存器约定与结构布局：
//   - libc++ SSO 字符串构造（把 C 字符串变成游戏可见的 std::string 布局）
//   - CDN apply 链所需的日志上下文（FakeLogCtx）与结果文本读取
//   - 表变更广播（逐指令复刻游戏 sub_D58544 广播段）
//   - locale → 游戏 FourCC 打包
//
// 迁移来源：language_module.cpp 私有实现（v7.37 下沉）。
// ============================================================

#include <cstddef>
#include <cstdint>

namespace lzt_settings {
namespace game {

// libc++ SSO 字符串（24 字节布局，与游戏内 std::string 同构）：
//   短串（<=22 字符）字节0 = len<<1、数据自 +1；长串 {cap|1, len, ptr}
struct SsoString {
    uint8_t bytes[24];
    void init(const char* s);   // 由 C 字符串填充本对象（长短串自动分流）
};

// CDN apply 完整链（校验→重载→游戏日志→广播）的调用上下文。
// 布局（IDA 实证 d5855c-d5856c）：+72 处是 24 字节 libc++ string，apply 链会
// 向其追加 "\nApply %s" / "\nSkipped %s"。全零初始化 = 合法空串。
// 历史教训（v5.0 段错误根因）：第一参数是【指针】——传 0 即写地址 0x48 崩溃。
struct FakeLogCtx {
    uint8_t pad[72];
    uint8_t logString[24];
};

// 读取 FakeLogCtx 内日志串的 C 视图（短串数据在 +1，长串指针在 +16）
const char* log_text(const FakeLogCtx& ctx);

// 常用游戏函数指针类型
//   CdnApplyFn: CDN Apply 链入口（含校验/重载/日志/广播）
//   DbLoadFn  : PVZDB::LoadPackageForTableFromRTONFile（名字经两级查找：包内 RSB → 磁盘）
using CdnApplyFn = void (*)(uintptr_t logCtx, void* path, void* displayName, uint32_t tableId);
using DbLoadFn   = int  (*)(uintptr_t db, uint32_t tableId, void* name);

// 表变更广播（逐指令复刻 sub_D58544 @0xd58670-0xd586dc）：
//   mgr=[OFF_DELEGATE_MGR_PTR 槽] → Lock(vtable+24)(mgr,&ret) → 遍历回调
//   elem+40（步长 48）→ 维护 mgr+DELEGATE_MGR_LOCK_DWORD 重入计数 → 归零解锁。
// 用途：apply 链的 Skipped 分支不带广播时，由调用方补广播完成 UI 刷新。
// 日志前缀沿用 "Lang broadcast"（历史格式，便于对比既有真机日志）。
void broadcast_table_changed(uintptr_t base, uint32_t tableId);

// locale → 游戏 FourCC（大写打包，如 "ZH-CN"=0x5A48434E、"EN-US"=0x454E5553）；
// 长度不足 4 的异常输入回退 EN-US。
uint32_t locale_to_fourcc(const char* locale);

} // namespace game
} // namespace lzt_settings

#endif // LZT_GAME_ABI_H