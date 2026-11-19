// ============================================================
// localizer.cpp — lzt_localizer 实现（统一本地化管理层）
//
// 本层自包含：不依赖 settings_framework 私有实现，仅依赖全局 offsets
// 常量与 lzt_core（base / log / memory_range_accessible / GameWStr）。
// 语义与 framework 已验证刷新逻辑保持一致（见下方各回填器注释），
// 但收敛为【类型化注册表 + 类型化回填器 + 统一 refresh_all()】。
//
// ARM64：完整实现（本次验证目标）。
// ARM32：占位（capture 返回 false、refresh_all 返回 -1）。
// ============================================================

#include "localizer.h"

#include "../offsets.h"
#include "../lzt_core.h"
#include "../lzt_settings_framework_config.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// 诊断日志门统一为 lzt_settings_framework_config.h 的 LZT_DBG_LOG（v7.37 起全工程一套）。
// 明细日志用于真机定位"哪些宿主被捕获/刷新/跳过"；正式版不产出。

namespace lzt_localizer {

#ifdef __aarch64__

// ---- 游戏 wstring 对象（libc++ 布局，与 framework 同款）----
// 24 字节 {flag, size, heap}；短串 字节0=len<<1、数据自+4（SSO 5 wchar）；
// 长串 {cap|1, len, ptr}。宽字符 4 字节（UTF-32LE）。
struct GameWStr {
    uint64_t flag;
    uint64_t size;
    uint64_t heap;
};

// 统一读取口：sub_14F3354(key) → 当前语言文本（返回值 = 24 字节结构，
// ARM64 PCS 走 X8 sret，声明为返回结构体即 ABI 正确）。
using LzLocalizeFn = GameWStr (*)(const wchar_t*);

// 读游戏 wstring 到 NUL 结尾 wchar 缓冲。asciiOnly 只用于键读取；
// 文本（快照/当前值）必须传 false（CJK/重音会被 ASCII 过滤拒绝）。
static bool read_wstr(uintptr_t obj, wchar_t* out, size_t cap,
                      size_t* outLen, bool asciiOnly) {
    if (lzt_core::base() == 0 ||
        !lzt_core::memory_range_accessible(obj, 24, false)) return false;
    const auto* s = reinterpret_cast<const uint8_t*>(obj);
    size_t klen = 0;
    const wchar_t* kdata = nullptr;
    if ((s[0] & 1) == 0) {
        klen = static_cast<size_t>(s[0]) >> 1;
        kdata = reinterpret_cast<const wchar_t*>(obj + 4);
    } else {
        memcpy(&klen, s + 8, sizeof(klen));
        memcpy(&kdata, s + 16, sizeof(kdata));
    }
    if (klen >= cap ||
        !lzt_core::memory_range_accessible(reinterpret_cast<uintptr_t>(kdata),
                                           klen * sizeof(wchar_t), false))
        return false;
    if (asciiOnly) {
        for (size_t i = 0; i < klen; ++i) {
            const uint32_t ch = static_cast<uint32_t>(kdata[i]);
            if (ch < 0x20 || ch > 0x7E) return false;
        }
    }
    memcpy(out, kdata, klen * sizeof(wchar_t));
    out[klen] = 0;
    if (outLen) *outLen = klen;
    return true;
}

namespace {
constexpr size_t kMaxCaptures = 256;   // 激进防溢出；破坏则压缩后仍满则清空

struct Capture {
    HostType type;
    uintptr_t host;
    wchar_t key[48];         // 本地化键，'[' 开头
    wchar_t snapshot[48];    // 捕获时已解析文本（Label 动态文本身份比对）
    bool hasSnapshot = false;
};

std::vector<Capture> g_caps;
std::mutex g_mutex;

// 死条目压缩（vtable 校验），caller 持锁。返回存活数。
size_t compact_locked() {
    uintptr_t base = lzt_core::base();
    if (base == 0) return g_caps.size();
    const uintptr_t tabVt = base + OFF_SETTINGS_TAB_VTABLE;
    const uintptr_t labelVt = base + OFF_SETTINGS_TAB_LABEL_VTABLE;
    size_t w = 0;
    for (size_t i = 0; i < g_caps.size(); ++i) {
        const Capture& c = g_caps[i];
        const uintptr_t vt = (c.type == HostType::TabRow) ? tabVt : labelVt;
        if (c.host != 0 &&
            lzt_core::memory_range_accessible(c.host, 8, false) &&
            *reinterpret_cast<volatile uintptr_t*>(c.host) == vt) {
            g_caps[w++] = c;
        }
    }
    g_caps.resize(w);
    return w;
}
} // namespace

bool capture(HostType type, uintptr_t host, const wchar_t* key,
             bool hasSnapshot, const wchar_t* snapshot) {
    if (key == nullptr || key[0] != L'[') return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_caps.size() >= kMaxCaptures) {
        const size_t live = compact_locked();
        if (live >= kMaxCaptures) {
            g_caps.clear();
            lzt_core::log_write("[LZ] capture overflow -> cleared");
        } else {
            lzt_core::log_write("[LZ] capture compacted: %zu live", live);
        }
    }
    if (g_caps.size() >= kMaxCaptures) return false;
    Capture c{};
    c.type = type;
    c.host = host;
    size_t n = 0;
    for (; key[n] && n + 1 < 48; ++n) c.key[n] = key[n];
    c.key[n] = 0;
    if (hasSnapshot && snapshot != nullptr) {
        c.hasSnapshot = true;
        n = 0;
        for (; snapshot[n] && n + 1 < 48; ++n) c.snapshot[n] = snapshot[n];
        c.snapshot[n] = 0;
    }
    g_caps.push_back(c);
    LZT_DBG_LOG("[LZ] capture: type=%u host=%p key=%ls snap=%d",
               (unsigned)type, (void*)host, c.key, c.hasSnapshot ? 1 : 0);
    return true;
}

// ---- TabRow 回填：改 host/+184 标题串（原 refresh_tab_titles 语义）----
static int refresh_tabrow(uintptr_t base) {
    if (base == 0) return 0;
    auto localizeKey = reinterpret_cast<LzLocalizeFn>(base + OFF_LocalizeKey);
    using ReleaseTitleFn = void (*)(uintptr_t, uint32_t);
    auto releaseTitle = reinterpret_cast<ReleaseTitleFn>(base + OFF_ReleaseTitle);

    int done = 0;
    size_t write = 0;
    for (size_t i = 0; i < g_caps.size(); ++i) {
        const Capture c = g_caps[i];
        if (c.type != HostType::TabRow) { g_caps[write++] = c; continue; }
        const uintptr_t vt = base + OFF_SETTINGS_TAB_VTABLE;
        const bool alive = c.host != 0 &&
            lzt_core::memory_range_accessible(c.host, SETTINGS_TAB_TITLE_HEAP + 8, false) &&
            *reinterpret_cast<volatile uintptr_t*>(c.host) == vt;
        if (!alive) {
            LZT_DBG_LOG("[LZ] tabrow skip(destroyed) host=%p", (void*)c.host);
            continue;
        }
        g_caps[write++] = c;
        GameWStr t = localizeKey(c.key);
        const bool longStr = (t.flag & 1) != 0;
        const bool empty = !longStr && (t.flag & 0xFF) == 0;
        if (empty) {
            LZT_DBG_LOG("[LZ] tabrow skip(empty) key=%ls", c.key);
            continue;
        }
        releaseTitle(c.host + SETTINGS_TAB_TITLE, 0);
        *reinterpret_cast<uintptr_t*>(c.host + SETTINGS_TAB_TITLE_HEAP) = t.heap;
        memcpy(reinterpret_cast<void*>(c.host + SETTINGS_TAB_TITLE), &t, 16);
        ++done;
        LZT_DBG_LOG("[LZ] tabrow write host=%p key=%ls", (void*)c.host, c.key);
    }
    g_caps.resize(write);
    LZT_DBG_LOG("[LZ] tabrow refreshed: %d/%zu", done, g_caps.size());
    return done;
}

// ---- Label 回填：SetText(vt+752) 拷贝语义 + 快照比对（原 refresh_label_texts）----
static int refresh_label(uintptr_t base) {
    if (base == 0) return 0;
    auto localizeKey = reinterpret_cast<LzLocalizeFn>(base + OFF_LocalizeKey);
    using LabelSetTextFn = void (*)(uintptr_t, const void*);
    const uintptr_t labelVt = base + OFF_SETTINGS_TAB_LABEL_VTABLE;

    int done = 0;
    size_t write = 0;
    for (size_t i = 0; i < g_caps.size(); ++i) {
        Capture c = g_caps[i];
        if (c.type != HostType::Label) { g_caps[write++] = c; continue; }
        const bool alive = c.host != 0 &&
            lzt_core::memory_range_accessible(c.host, 8, false) &&
            *reinterpret_cast<volatile uintptr_t*>(c.host) == labelVt;
        if (!alive) {
            LZT_DBG_LOG("[LZ] label skip(destroyed) host=%p", (void*)c.host);
            continue;
        }
        wchar_t cur[48];
        size_t curLen = 0;
        if (!read_wstr(c.host + SETTINGS_LABEL_TEXT, cur, 48, &curLen, false)) {
            LZT_DBG_LOG("[LZ] label skip(unreadable) host=%p", (void*)c.host);
            continue;
        }
        if (c.hasSnapshot && wcscmp(cur, c.snapshot) != 0) {
            LZT_DBG_LOG("[LZ] label skip(dynamic) host=%p key=%ls", (void*)c.host, c.key);
            g_caps[write++] = c;
            continue;
        }
        GameWStr r = localizeKey(c.key);
        const bool longStr = (r.flag & 1) != 0;
        const bool empty = !longStr && (r.flag & 0xFF) == 0;
        const wchar_t* view = longStr
            ? reinterpret_cast<const wchar_t*>(r.heap)
            : reinterpret_cast<const wchar_t*>(reinterpret_cast<uintptr_t>(&r) + 4);
        const size_t rLen = longStr ? static_cast<size_t>(r.size)
                                    : static_cast<size_t>((r.flag & 0xFF) >> 1);
        bool same = rLen == curLen;
        if (same) {
            for (size_t k = 0; k < rLen; ++k)
                if (view[k] != cur[k]) { same = false; break; }
        }
        if (empty || (!empty && view[0] == L'<')) {
            if (longStr) delete[] reinterpret_cast<wchar_t*>(r.heap);
            LZT_DBG_LOG("[LZ] label skip(miss) host=%p key=%ls", (void*)c.host, c.key);
            g_caps[write++] = c;
            continue;
        }
        if (same) {
            if (longStr) delete[] reinterpret_cast<wchar_t*>(r.heap);
            g_caps[write++] = c;
            continue;
        }
        const uintptr_t settext =
            *reinterpret_cast<volatile uintptr_t*>(
                *reinterpret_cast<volatile uintptr_t*>(c.host) + SETTINGS_TAB_LABEL_SETTEXT_VT);
        reinterpret_cast<LabelSetTextFn>(settext)(c.host, &r);
        // 快照随写入更新（否则下次当前文本=新值≠旧快照→误判动态丢弃）。
        // 必须在释放 r.heap 之前读 view（longStr 时 view 指向 r.heap）。
        if (rLen < 48) {
            size_t q = 0;
            for (; q < rLen && q + 1 < 48; ++q) c.snapshot[q] = view[q];
            c.snapshot[q] = 0;
            c.hasSnapshot = true;
        }
        if (longStr) delete[] reinterpret_cast<wchar_t*>(r.heap);
        g_caps[write++] = c;
        ++done;
        LZT_DBG_LOG("[LZ] label write host=%p key=%ls", (void*)c.host, c.key);
    }
    g_caps.resize(write);
    LZT_DBG_LOG("[LZ] label refreshed: %d/%zu", done, g_caps.size());
    return done;
}

int refresh_all() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const size_t captured = g_caps.size();
    if (captured == 0) return 0;
    uintptr_t baseAddr = lzt_core::base();
    if (baseAddr == 0) return 0;
    int done = refresh_tabrow(baseAddr) + refresh_label(baseAddr);
    lzt_core::log_write("[LZ] refresh_all: captured=%zu refreshed=%d", captured, done);
    return done;
}

#else // !__aarch64__  (ARM32 占位)

bool capture(HostType, uintptr_t, const wchar_t*, bool, const wchar_t*) {
    return false;
}
int refresh_all() { return -1; }

#endif // __aarch64__

} // namespace lzt_localizer