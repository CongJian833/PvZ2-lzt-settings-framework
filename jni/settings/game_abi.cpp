// ============================================================
// game_abi.cpp — 游戏 ABI / 表热重载通用工具实现（语义见头文件）
//
// 迁移来源：language_module.cpp 的匿名命名空间私有实现（v7.37 下沉），
// 逻辑逐行保留（含历史教训注释），仅改归属与命名。
// ============================================================

#include "game_abi.h"

#include "../lzt_core.h"
#include "../offsets.h"

#include <cstring>

namespace lzt_settings {
namespace game {

static_assert(sizeof(FakeLogCtx) == 96, "FakeLogCtx size mismatch");

void SsoString::init(const char* s) {
    const size_t len = strlen(s);
    memset(bytes, 0, sizeof(bytes));
    if (len <= 22) {
        bytes[0] = static_cast<uint8_t>(len << 1);
        memcpy(bytes + 1, s, len);
    } else {
        const size_t cap = ((len + 16) & ~(static_cast<size_t>(15))) | 1;
        memcpy(bytes + 0, &cap, 8);
        memcpy(bytes + 8, &len, 8);
        const char* p = s;
        memcpy(bytes + 16, &p, 8);
    }
}

const char* log_text(const FakeLogCtx& ctx) {
    const uint8_t* s = ctx.logString;
    if (s[0] & 1) {   // 长串：指针在 +16
        const char* p = nullptr;
        memcpy(&p, s + 16, sizeof(p));
        return p;
    }
    return reinterpret_cast<const char*>(s + 1);
}

// ---- 以下两个函数使用 ARM64 专属偏移（offsets.h 的 __aarch64__ 段）----
// ARM32 无语言热重载，不提供实现；误用会在链接期暴露。
#ifdef __aarch64__

// 表变更广播：逐指令复刻 sub_D58544 广播段（见头文件说明）
void broadcast_table_changed(uintptr_t base, uint32_t tableId) {
    uintptr_t mgr = *reinterpret_cast<volatile uintptr_t*>(base + OFF_DELEGATE_MGR_PTR);
    lzt_core::log_write("Lang broadcast: begin table=%u manager=%p",
                        tableId, reinterpret_cast<void*>(mgr));
    if (mgr == 0) {
        lzt_core::log_write("Lang broadcast: end table=%u result=manager-null callbacks=0",
                            tableId);
        return;
    }
    uintptr_t vtbl = *reinterpret_cast<volatile uintptr_t*>(mgr);
    if (vtbl == 0) {
        lzt_core::log_write("Lang broadcast: end table=%u result=vtable-null callbacks=0",
                            tableId);
        return;
    }
    using LockFn = uintptr_t* (*)(uintptr_t, void*);
    auto lock = *reinterpret_cast<LockFn*>(vtbl + 24);
    if (lock == nullptr) {
        lzt_core::log_write("Lang broadcast: end table=%u result=lock-null callbacks=0",
                            tableId);
        return;
    }
    uintptr_t lockRet = 0;   // 原版 d58690 的 X1 是栈上 &ret 出参槽（v7.3.1 审计修复）
    uintptr_t* vec = lock(mgr, &lockRet);
    volatile uint32_t* counter =
        reinterpret_cast<volatile uint32_t*>(mgr + DELEGATE_MGR_LOCK_DWORD);
    *counter = *counter + 1;
    uint32_t callbacks = 0;
    if (vec != nullptr) {
        uintptr_t it = vec[0], end = vec[1];
        while (it != end) {
            auto cb = *reinterpret_cast<void (**)(uintptr_t, uint32_t)>(it + 40);
            if (cb != nullptr) {
                cb(it, tableId);
                ++callbacks;
            }
            it += 48;
        }
    }
    const uint32_t left = *counter - 1;
    *counter = left;
    if (left == 0) {
        using UnlockFn = void (*)(uintptr_t);
        reinterpret_cast<UnlockFn>(base + OFF_DELEGATE_UNLOCK)(mgr);
    }
    lzt_core::log_write("Lang broadcast: end table=%u result=ok callbacks=%u counter=%u",
                        tableId, callbacks, left);
}

uint32_t locale_to_fourcc(const char* locale) {
    char up[8] = {0};
    size_t n = 0;
    for (; locale[n] && n < 7; ++n) {
        char c = locale[n];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        up[n] = c;
    }
    if (n < 4) return LANG_ID_EN_US;   // 异常兜底
    // "%c%c-%c%c" 打包：首字节=最高字符（sub_16A8A4C HIBYTE 先出）
    return (static_cast<uint32_t>(static_cast<unsigned char>(up[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(up[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(up[3])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(up[4]));
}

#endif // __aarch64__

} // namespace game
} // namespace lzt_settings