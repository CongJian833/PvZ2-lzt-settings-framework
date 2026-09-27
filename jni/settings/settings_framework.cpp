// ============================================================
// settings_framework.cpp — Settings UI 框架引擎
//
// 组件：模块注册表 / ID 定案与冲突校验 / hook 五件套（createTab/
//       dispatch/DataSharing 页/行初始化器/标签构造器）/ 页面构建调度 /
//       PageBuilder / 三套文本原位刷新（行+184、根标题+184、标签
//       SetText）/ config 工具。
//
// 实现约束（设计文档 1.4/9）：
//   - 引擎代码零业务语义（不含任何具体模块字眼）
//   - 架构知识只出现在 offsets.h 常量与 widgets 层调用中
//   - 全部游戏函数指针经 resolve_helpers/lzt_hooks 解析，缺失即失败
//
// 关键安全机制（历史教训封存于此，模块不可见）：
//   - checkbox 点击只置 dirty 异步重建，禁止同步重建（UAF 防护）
//   - dispatch 拦截分支尾部补调页面级 layout（v38 教训）
//   - vtable 解引用取函数指针、releaseNotify 必须携带旧指针（v37 前）
//   - 文本所有权：localize 返回值拷入目标即转移所有权，成功路径禁
//     delete；SetText 为拷贝语义，调用方须释放自己的解析临时堆
//   - 捕获快照+切换期比对的机制，自己每次写入后必须同步快照
//
// ID 分配策略：tab id 于 engine_install 定案（自动游标从
// SETTINGS_FIRST_DYNAMIC_ID 起，跳过原版占用表）；checkbox id 于
// 该模块首次页面构建时分配并缓存到注册表槽位，后续构建复用同一
// id 保证 dispatch 路由稳定。显式指定 id 冲突时拒绝该控件并记日志。
// ============================================================

#include <mutex>
#include "settings_framework.h"
#include "settings_widgets.h"
#include "localizer.h"
#include "../lzt_core.h"
#include "../lzt_hooks.h"
#include "../lzt_settings_framework_config.h"
#include "../offsets.h"

#include <algorithm>
#include <vector>

namespace lzt_settings {

// 诊断日志门统一为 lzt_settings_framework_config.h 的 LZT_DBG_LOG（v7.37 起全工程一套）。

namespace detail {
// 引擎构造 PageBuilder 的唯一合法入口（完整定义须先于使用点）
struct EngineAccess {
    static PageBuilder make(PageBuilder::State& s) { return PageBuilder(s); }
};
}

// ---- PageBuilder 内部状态（对外不透明）----
struct PageBuilder::State {
    struct Item {
        widgets::WidgetPlanItem plan{};
        CheckboxClickFn onClick = nullptr;   // 仅 CHECKBOX 有效
    };
    std::vector<Item> items;
    void* slot = nullptr;        // ModuleSlot*（匿名 ns 类型经 void* 隐藏）
    size_t checkboxSeq = 0;      // 本次构建的 checkbox 序号（对齐 id 缓存槽位）
    int contentWidthPx = 0;      // 内容区像素宽（供 add_* 自定义布局计算）
    float scaleF = 0.0f;         // UI 缩放系数（同上）
};

namespace {

// ---- 引擎容量策略 ----
constexpr uint32_t kMaxModules = 8;
constexpr uint32_t kMaxWidgetsPerModule = 16;

struct CbCacheEntry {
    bool valid = false;
    uint32_t id = 0;
    CheckboxClickFn onClick = nullptr;
    CheckboxIndexClickFn onIndexClick = nullptr;   // v7.37：带序号回调（优先）
};

struct ModuleSlot {
    bool used = false;
    bool finalized = false;      // tabId 已定案（engine_install 后）
    ModuleDesc desc{};
    uint32_t tabId = 0;
    CbCacheEntry cbCache[kMaxWidgetsPerModule];  // 按 checkbox 构建顺序缓存 id
    size_t cbCached = 0;
};

ModuleSlot g_slots[kMaxModules];
size_t g_slotCount = 0;
uint32_t g_nextDynamicId = SETTINGS_FIRST_DYNAMIC_ID;

// 注入计划（engine_install 时按 slotAfterAnchor 升序稳定排定）
const ModuleSlot* g_plan[kMaxModules];
size_t g_planCount = 0;

// ---- hook 三件套 trampoline 与状态 ----
using CreateTabFn_t = uintptr_t (*)(uintptr_t, uint32_t, uintptr_t, uintptr_t, uintptr_t);
using DispatchFn_t = int (*)(uintptr_t, uint32_t);
using CreatePageFn_t = int (*)(uintptr_t);

CreateTabFn_t oCreateTab = nullptr;
DispatchFn_t oDispatch = nullptr;
CreatePageFn_t oCreatePage = nullptr;
// ARM32 需要调用方提供 trampoline 缓冲；ARM64 由 hook 库内部管理，
// 但为保持安装代码架构无关，此处无条件定义（ARM64 下仅浪费少量 BSS）。
alignas(32) uint8_t g_tr_create[lzt_hooks::TRAMPOLINE_SIZE];
alignas(32) uint8_t g_tr_dispatch[lzt_hooks::TRAMPOLINE_SIZE];
alignas(32) uint8_t g_tr_page[lzt_hooks::TRAMPOLINE_SIZE];

enum : uint32_t { HK_NONE = 0, HK_CREATE = 1, HK_DISPATCH = 2, HK_PAGE = 4,
                  HK_CORE = 7, HK_ALL = 15 };
uint32_t g_hookedFlags = HK_NONE;
uintptr_t g_engineBase = 0;
bool g_idsFinalized = false;
const ModuleSlot* g_activeModule = nullptr;   // 当前激活模块（UI 线程串行）

// ---- v7.37：通用注册点（框架不感知内容）----
// 语言切换刷新器 / 引擎就绪回调。注册在模块初始化线程（engine_install 之前），
// 调用在 UI 线程，与 g_slots 同一线程模型，无需加锁。
constexpr size_t kMaxRegistrations = 8;
void (*g_refreshers[kMaxRegistrations])() = {};
size_t g_refresherCount = 0;
void (*g_engineReady[kMaxRegistrations])(uintptr_t) = {};
size_t g_engineReadyCount = 0;

// ---- v7.7：设置行标题原位刷新 ----
// 全部设置行(Tab 行/滑条行/checkbox 行,0xF0 对象)的标题在构造时经
// sub_A54230 从容器解析一次后缓存于行内 +184,不再自动更新。本框架 hook
// 行初始化器捕获 (行对象,标题键),语言切换后由 refresh_tab_titles 原位
// 改写各 +184——返回显示/状态变化时按新标题重绘(引擎返回流程已实证)。

static bool g_rowCaptureSuppress = false;   // v7.9.1:语言页构建期间跳过行捕获

// 原版 id 占用表（两架构一致；《Android设置界面-Tab注册与分发总表》逆向结论）
const uint32_t kVanillaIds[] = {3, 6, 7, 8, 9, 10, 12, 15, 18, 24, 26, 29};

bool vanilla_uses(uint32_t id) {
    for (uint32_t v : kVanillaIds) if (v == id) return true;
    return false;
}

bool id_taken_by_framework(uint32_t id) {
    for (size_t i = 0; i < g_slotCount; ++i) {
        const ModuleSlot& s = g_slots[i];
        if (!s.used) continue;
        if (s.finalized && s.tabId == id) return true;
        for (size_t j = 0; j < s.cbCached; ++j)
            if (s.cbCache[j].valid && s.cbCache[j].id == id) return true;
    }
    return false;
}

bool id_available(uint32_t id) {
    return !vanilla_uses(id) && !id_taken_by_framework(id);
}

// ---- 页面构建调度：执行模块回调收集清单，交 widgets 物化 ----
void build_page_for(const ModuleSlot& m, uintptr_t page) {
    PageBuilder::State st;
    st.slot = const_cast<ModuleSlot*>(&m);
    st.contentWidthPx = widgets::query_content_width_px();
    st.scaleF = widgets::query_ui_scale();

    PageBuilder b = detail::EngineAccess::make(st);
    if (m.desc.onBuildPage) m.desc.onBuildPage(b);

    // Debug 诊断：输出本次构建的控件清单摘要（类型/键/id），
    // 用于核对模块声明与实际物化内容的一致性
    LZT_DBG_LOG("[SFW] build plan: module=%u items=%zu", m.tabId, st.items.size());
    for (size_t i = 0; i < st.items.size(); ++i) {
        const PageBuilder::State::Item& it = st.items[i];
        LZT_DBG_LOG("[SFW]   plan[%zu]: type=%u id=%u key=%ls",
                    i, it.plan.type, it.plan.id,
                    it.plan.key ? it.plan.key : L"(null)");
    }

    // ⚠️ 必须抽取平面 plan 数组再交给物化层：State::Item 比
    // WidgetPlanItem 多 8 字节 onClick 尾随字段，直接 reinterpret_cast
    // 数组指针会以错误步长遍历（历史缺陷：第 2+ 个控件全部错位，
    // 读取 onClick 低字节当 type、垃圾位段当 key 指针 → 野指针崩溃）。
    std::vector<widgets::WidgetPlanItem> plans;
    plans.reserve(st.items.size());
    for (const auto& it : st.items) plans.push_back(it.plan);

    g_rowCaptureSuppress = true;   // v7.9.1:本页构建的 checkbox 不入注册表
    bool ok = widgets::materialize_page(page, m.desc.titleKey,
                                        plans.data(), plans.size());
    g_rowCaptureSuppress = false;
    if (!ok) {
        // 物化失败不致命（页面保持原状），但必须留痕供排查
        lzt_core::log_write("[SFW] build: materialize failed module=%u page=%p",
                            m.tabId, (void*)page);
    }
}

// ---- 标题键捕获已迁移到 lzt_localizer（类型化注册表）----
// v7.20：旧 g_tabTitles 行注册表移除，捕获由 hk_row_init 转发
// lzt_localizer::capture(HostType::TabRow)，刷新由 localizer 类型化回填。

// ---- 共享文本工具（v7.18 整理：全文件唯一定义，勿在局部重复）----
// 游戏 wstring 对象（libc++ 布局）：24 字节 {flag, size, heap}；
// 短串字节0=len<<1 数据自+4（SSO 容量 5 wchar）；长串 {cap|1, len, ptr}。
// 宽字符 = 4 字节 wchar_t（UTF-32LE，与 Android clang 一致，L"" 可直传）。
#ifdef __aarch64__
struct GameWStr {
    uint64_t flag;
    uint64_t size;
    uint64_t heap;
};
using SfwLocalizeFn = GameWStr (*)(const wchar_t *);

// 读游戏 wstring 对象到 NUL 结尾的 wchar 缓冲。asciiOnly 仅用于键读取
// （键=ASCII 标识符，过滤垃圾）；文本读取（快照/当前值）必须传 false
// ——首切后标签文本即 CJK/重音字符，ASCII 过滤会拒绝导致条目被丢
// （v7.16.1/v7.16.2 两轮真机教训封存于此）。
static bool sfw_read_wstr(uintptr_t obj, wchar_t *out, size_t cap,
                          size_t *outLen, bool asciiOnly) {
    if (obj == 0 || !lzt_core::memory_range_accessible(obj, 24, false)) return false;
    const auto *s = reinterpret_cast<const uint8_t *>(obj);
    size_t klen = 0;
    const wchar_t *kdata = nullptr;
    if ((s[0] & 1) == 0) {
        klen = static_cast<size_t>(s[0]) >> 1;
        kdata = reinterpret_cast<const wchar_t *>(obj + 4);
    } else {
        memcpy(&klen, s + 8, sizeof(klen));
        memcpy(&kdata, s + 16, sizeof(kdata));
    }
    if (klen >= cap || !lzt_core::memory_range_accessible(
                           reinterpret_cast<uintptr_t>(kdata),
                           klen * sizeof(wchar_t), false)) return false;
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
#endif

// ---- v7.7：行初始化器 hook（捕获全部设置行 + 标题键）----
#ifdef __aarch64__
using RowInitFn = long long (*)(uintptr_t, uintptr_t);
static RowInitFn oRowInit = nullptr;
alignas(32) uint8_t g_tr_rowinit[lzt_hooks::TRAMPOLINE_SIZE];
static long long hk_row_init(uintptr_t row, uintptr_t keyStr) {
    if (oRowInit) oRowInit(row, keyStr);
    // 捕获 (行对象, 标题键)：'[' 开头的 ASCII 键。Tab 行/滑条行/checkbox
    // 行统一覆盖。v7.9.1:语言页构建期间抑制捕获——语言页 checkbox 每次
    // 进入即重建(自更新),注册表若累积它们,刷新会写到已释放对象(堆损坏
    // 风险)。
    if (row == 0 || keyStr == 0 || g_rowCaptureSuppress) return 0;
    wchar_t key[64] = {0};
    if (!sfw_read_wstr(keyStr, key, sizeof(key) / sizeof(key[0]), nullptr, true))
        return 0;
    if (key[0] != L'[') return 0;
    // v7.20：转发到 Localizer 类型化注册表
    lzt_localizer::capture(lzt_localizer::HostType::TabRow, row, key);
    return 0;
}
#endif


// ---- v7.16：0x2D8 标签构造器捕获（主界面等数据驱动文案统一入口）----
// sub_130A068 内部：textObj 为方括号键时 localize 一次并 SetText 到
// label+360（SetText=vt+752 拷贝语义，同步 label+584 度量对象）。
// 此处捕获 (label, 键, 创建时文本快照)。快照身份比对：当前文本≠快照=
// 游戏已改为动态文本（如活动倒计时），放弃该条目交还游戏管理。
// v7.20：旧 g_labelTexts 标签注册表移除，捕获转发 lzt_localizer.
// capture(HostType::Label)，刷新由 localizer 类型化回填。
#ifdef __aarch64__
using LabelCreateFn = void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                               uintptr_t, uintptr_t);
static LabelCreateFn oLabelCreate = nullptr;
alignas(32) uint8_t g_tr_labelcreate[lzt_hooks::TRAMPOLINE_SIZE];

static void hk_label_create(uintptr_t label, uintptr_t a2, uintptr_t a3,
                            uintptr_t textObj, uintptr_t a5, uintptr_t a6) {
    if (oLabelCreate) oLabelCreate(label, a2, a3, textObj, a5, a6);
    if (g_rowCaptureSuppress) return;   // 本框架物化期间的标签不入注册表
    if (label == 0 || textObj == 0) return;
    wchar_t key[48] = {0};
    if (!sfw_read_wstr(textObj, key, 48, nullptr, true)) return;
    if (key[0] != L'[') return;         // 非键文本（玩家名等）不收
    wchar_t init[48] = {0};
    // 快照须读构造完成后的 label+360（orig 已 SetText）；文本可为任意语言
    if (!sfw_read_wstr(label + SETTINGS_LABEL_TEXT, init, 48, nullptr, false)) return;
    // v7.20：转发到 Localizer 类型化注册表（带创建时快照）
    lzt_localizer::capture(lzt_localizer::HostType::Label, label, key, true, init);
}
#endif

// ---- v7.37：持久横幅（主界面 PersistentMessage）已迁出框架 ----
// 该机制属业务屏幕适配，现位于 screen_bindings.cpp，经 register_refresher /
// register_engine_ready 注册；框架不再感知任何具体屏幕（引擎零业务语义）。


// ---- Hook 1：createTab —— 锚点后首 Tab 创建点按计划批量插入 ----
uintptr_t hook_create_tab(uintptr_t page, uint32_t id, uintptr_t title,
                          uintptr_t iconNormal, uintptr_t iconSelected) {
    if (!oCreateTab) return 0;

    // v7.6.1 审计修复：注入只依赖核心三 hook（HK_CORE），不再要求工厂
    // hook 就绪——工厂失败不应连带禁用 Tab 注入。
    if (g_planCount > 0 && (g_hookedFlags & HK_CORE) == HK_CORE && id == SETTINGS_ANCHOR_NEXT_TAB_ID) {
        // 按注入计划（slotAfterAnchor 升序）依次插入；单个失败不影响其余
        for (size_t i = 0; i < g_planCount; ++i) {
            const ModuleSlot* m = g_plan[i];
            // v7.34：模块可选 shouldCreate 返回 false 时不建该 Tab（如语言模块在
            // 无来源/单一语言时隐藏自身）。
            if (m->desc.shouldCreate && !m->desc.shouldCreate()) {
                lzt_core::log_write("[SFW] createTab: skip module title=%ls",
                                    m->desc.titleKey ? m->desc.titleKey : L"");
                continue;
            }
            widgets::insert_custom_tab(page, m->tabId, m->desc.titleKey,
                                       m->desc.iconNormalOffset,
                                       m->desc.iconSelectedOffset,
                                       reinterpret_cast<uintptr_t>(oCreateTab));
        }
    }
    uintptr_t tab = oCreateTab(page, id, title, iconNormal, iconSelected);

    return tab;
}

// ---- Hook 2：dispatch —— Tab 进入 / 控件点击 / 其余放行 ----
static uintptr_t g_lastSettingsPage = 0;   // v7.12：当前设置页（顶部标题刷新用）
int hook_dispatch(uintptr_t page, uint32_t tabId) {
    if (!oDispatch) return 0;
    if (page != 0) g_lastSettingsPage = page;

    // 命中某模块的 Tab：设为激活并构建其页面
    for (size_t i = 0; i < g_slotCount; ++i) {
        const ModuleSlot& m = g_slots[i];
        if (m.used && m.finalized && tabId == m.tabId) {
            g_activeModule = &m;
            lzt_core::log_write("[SFW] dispatch: enter module tab=%u page=%p",
                                tabId, (void*)page);
            build_page_for(m, page);
            // dispatch 层公共收尾（v38 教训：所有原版 case 尾部都调
            // layout；拦截分支若省略则页面 mount 后无任何刷新）
            lzt_core::set_stage(503);   // 页面级 layout 前(诊断 v7.6.x 崩溃)
            widgets::run_page_layout(page);
            lzt_core::set_stage(504);   // 页面级 layout 后
            return 0;
        }
    }

    // 命中动态控件 id：置 dirty 异步重建（框架封死同步重建路径）+
    // 触发该控件绑定的点击回调（newState 恒为"选中"，语义由模块解释）
    for (size_t i = 0; i < g_slotCount; ++i) {
        const ModuleSlot& m = g_slots[i];
        if (!m.used || !m.finalized) continue;
        for (size_t j = 0; j < m.cbCached; ++j) {
            const CbCacheEntry& c = m.cbCache[j];
            if (c.valid && c.id == tabId) {
                lzt_core::log_write("[SFW] dispatch: widget click id=%u page=%p",
                                    tabId, (void*)page);
                *reinterpret_cast<uint8_t*>(page + SETTINGS_PAGE_DIRTY) = 1;
                // v7.37：带序号回调优先（序号 = 本模块复选框构建顺序 = 缓存下标）
                if (c.onIndexClick) c.onIndexClick(static_cast<int>(j), true);
                else if (c.onClick) c.onClick(true);
                return 0;
            }
        }
    }

    // 其余事件：离开自定义页，放行原版
    g_activeModule = nullptr;
    return oDispatch(page, tabId);
}

// ---- Hook 3：DataSharing 页面函数 —— dirty 重建分流 ----
int hook_create_page(uintptr_t page) {
    if (!oCreatePage) return 0;
    if (g_activeModule) {
        // 框架检测 dirty → 重建当前激活模块的页面（多 Tab 天然安全：
        // 谁激活重建谁）；原版 DataSharing 页面函数恒返 0
        build_page_for(*g_activeModule, page);
        return 0;
    }
    return oCreatePage(page);
}

} // namespace

// ============================================================
// 对外 API：模块注册与引擎安装
// ============================================================
bool register_refresher(void (*fn)()) {
    if (fn == nullptr) return false;
    if (g_refresherCount >= kMaxRegistrations) {
        lzt_core::log_write("[SFW] register_refresher rejected: table full (%u)",
                            (unsigned)kMaxRegistrations);
        return false;
    }
    g_refreshers[g_refresherCount++] = fn;
    return true;
}

bool register_engine_ready(void (*fn)(uintptr_t)) {
    if (fn == nullptr) return false;
    if (g_engineReadyCount >= kMaxRegistrations) {
        lzt_core::log_write("[SFW] register_engine_ready rejected: table full (%u)",
                            (unsigned)kMaxRegistrations);
        return false;
    }
    g_engineReady[g_engineReadyCount++] = fn;
    return true;
}

bool register_module(const ModuleDesc& desc) {
    if (desc.titleKey == nullptr || desc.titleKey[0] == L'\0' ||
        desc.onBuildPage == nullptr) {
        lzt_core::log_write("[SFW] register fail: invalid descriptor (titleKey/onBuildPage)");
        return false;
    }
    if (g_idsFinalized) {
        lzt_core::log_write("[SFW] register fail: ids already finalized");
        return false;
    }
    if (g_slotCount >= kMaxModules) {
        lzt_core::log_write("[SFW] register fail: module table full (%u)",
                            (unsigned)kMaxModules);
        return false;
    }
    ModuleSlot& s = g_slots[g_slotCount++];
    s.used = true;
    s.finalized = false;
    s.desc = desc;
    s.tabId = 0;
    s.cbCached = 0;
    lzt_core::log_write("[SFW] module registered: anchor=%u slot=%u reqTab=%u",
                        desc.anchorTabId, desc.slotAfterAnchor, desc.requestedTabId);
    return true;
}

bool engine_install() {
    // 幂等：已全部安装且基址未变
    const bool allInstalled = (g_hookedFlags == HK_ALL);
    if (allInstalled && g_engineBase == lzt_core::base()) return true;
    if (g_slotCount == 0) return true;   // 无模块：不装任何东西（纯相机场景零开销）

    uintptr_t base = lzt_core::base();
    if (base == 0) {
        lzt_core::log_write("[SFW] install defer: base not ready");
        return false;
    }

    // ---- 定案 ID（仅一次；此后 register_module 拒绝新模块）----
    if (!g_idsFinalized) {
        for (size_t i = 0; i < g_slotCount; ++i) {
            ModuleSlot& s = g_slots[i];
            if (!s.used) continue;
            if (s.finalized) continue;   // 前次定案中断后重试：跳过已定案模块，
                                         // 防止游标重复分配新 id
            // v1 限制：仅支持 Build Version 锚点（其他锚点的"后继 Tab"id
            // 未经验证，见设计文档第 9 节）
            if (s.desc.anchorTabId != SETTINGS_BUILDVERSION_ID) {
                lzt_core::log_write("[SFW] install fail: unsupported anchor %u "
                                    "(only BuildVersion supported)", s.desc.anchorTabId);
                return false;
            }
            if (s.desc.requestedTabId != 0) {
                if (!id_available(s.desc.requestedTabId)) {
                    lzt_core::log_write("[SFW] install fail: requested tab id %u conflicts",
                                        s.desc.requestedTabId);
                    return false;
                }
                s.tabId = s.desc.requestedTabId;
            } else {
                while (!id_available(g_nextDynamicId)) ++g_nextDynamicId;
                s.tabId = g_nextDynamicId++;
            }
            s.finalized = true;
            lzt_core::log_write("[SFW] tab id finalized: %u (anchor=%u slot=%u)",
                                s.tabId, s.desc.anchorTabId, s.desc.slotAfterAnchor);
        }

        // 生成注入计划：按 slotAfterAnchor 升序稳定排序
        g_planCount = 0;
        for (size_t i = 0; i < g_slotCount; ++i)
            if (g_slots[i].used) g_plan[g_planCount++] = &g_slots[i];
        std::stable_sort(g_plan, g_plan + g_planCount,
                         [](const ModuleSlot* a, const ModuleSlot* b) {
                             return a->desc.slotAfterAnchor < b->desc.slotAfterAnchor;
                         });
        g_idsFinalized = true;
    }

    if (!widgets::resolve_helpers()) return false;

    // ---- 安装三件套（基址变化时重置重装；部分成功允许下次调用补装）----
    if (g_engineBase != base) {
        if (g_engineBase != 0)
            lzt_core::log_write("[SFW] base changed 0x%lx -> 0x%lx, reinstalling hooks",
                                (unsigned long)g_engineBase, (unsigned long)base);
        oCreateTab = nullptr;
        oDispatch = nullptr;
        oCreatePage = nullptr;
        g_hookedFlags = HK_NONE;
        g_engineBase = base;
    }

    if (!(g_hookedFlags & HK_CREATE)) {
        void* orig = nullptr;
        if (lzt_hooks::install(base + OFF_SettingsCreate, (void*)hook_create_tab,
                               &orig, g_tr_create)) {
            oCreateTab = reinterpret_cast<CreateTabFn_t>(orig);
            g_hookedFlags |= HK_CREATE;
        }
    }
    if (!(g_hookedFlags & HK_DISPATCH)) {
        void* orig = nullptr;
        if (lzt_hooks::install(base + OFF_SettingsDispatch, (void*)hook_dispatch,
                               &orig, g_tr_dispatch)) {
            oDispatch = reinterpret_cast<DispatchFn_t>(orig);
            g_hookedFlags |= HK_DISPATCH;
        }
    }
    if (!(g_hookedFlags & HK_PAGE)) {
        void* orig = nullptr;
        if (lzt_hooks::install(base + OFF_SettingsDataSharing, (void*)hook_create_page,
                               &orig, g_tr_page)) {
            oCreatePage = reinterpret_cast<CreatePageFn_t>(orig);
            g_hookedFlags |= HK_PAGE;
        }
    }

#ifdef __aarch64__
    if (oRowInit == nullptr) {   // 幂等守卫:安装一次后跳过
        // 行初始化器 hook 需在任何设置页构建前就位;engine_install 首次
        // 调用早于设置页打开,此处安装即满足
        void* orig = nullptr;
        if (lzt_hooks::install(base + OFF_SETTINGS_ROW_INIT, (void*)hk_row_init,
                               &orig, g_tr_rowinit)) {
            oRowInit = reinterpret_cast<RowInitFn>(orig);
            lzt_core::log_write("[SFW] row init hook installed @+0xA54230");
        } else {
            lzt_core::log_write("[SFW] row init hook install FAILED (标题刷新不可用)");
        }
    }
#endif

#ifdef __aarch64__
    if (oLabelCreate == nullptr) {   // v7.16：幂等安装
        void* orig = nullptr;
        if (lzt_hooks::install(base + OFF_SETTINGS_LABEL_CREATE,
                               (void*)hk_label_create, &orig, g_tr_labelcreate)) {
            oLabelCreate = reinterpret_cast<LabelCreateFn>(orig);
            lzt_core::log_write("[SFW] label create hook installed @+0x130A068");
        } else {
            lzt_core::log_write("[SFW] label create hook install FAILED (标签刷新不可用)");
        }
    }
#endif

    // v7.9：重注册崩溃诊断(游戏启动流程可能覆盖 so 早装载时注册的处理器;
    // v7.6.5 实测 NATIVE_CRASH 全部丢失)。重复安装同一 handler 无副作用。
    lzt_core::install_crash_diagnostics();
    lzt_core::start_crash_handler_keepalive();   // v7.10.1:保活 10 分钟
    lzt_core::log_write("[SFW] engine install: create=%d dispatch=%d page=%d modules=%u",
                        (g_hookedFlags & HK_CREATE) ? 1 : 0,
                        (g_hookedFlags & HK_DISPATCH) ? 1 : 0,
                        (g_hookedFlags & HK_PAGE) ? 1 : 0,
                        (unsigned)g_slotCount);
    // v7.37：引擎就绪回调——基址可用，通知已注册方安装自身 hook（须自幂等）
    for (size_t i = 0; i < g_engineReadyCount; ++i) {
        if (g_engineReady[i]) g_engineReady[i](base);
    }
    return g_hookedFlags == HK_ALL;
}


// ---- v7.4 标签栏标题刷新：已迁移到 lzt_localizer（v7.20）----
// 设置行（Tab/滑条/checkbox 行）标题的刷新由
// lzt_localizer::refresh_all()（HostType::TabRow 回填器）接管。

// ---- v7.13：设置根视图顶部标题原位刷新 ----
// 根视图顶部大标题(设置/Settings)= 独立的 0xD8 标题 controller,由根
// 视图构建器以 [SETTINGS_TITLE] 键构造(sub_A4C490 @0xa4c714:16 字符键
// → sub_A538F0 构造器 localize 后存 +184),存于页+224 向量的第一个元
// 素。v7.12 误写 *(页+216)+8(= 子页标题 controller,vector[1],其标题
// 由页构建流程负责)。此处改写 vector[0] 的 +184:vtable 校验
// (off_24E3EC0)防类型不符,写入序列与 materialize 同构(releaseTitle
// → 写 heap → 拷贝 flag+size 16 字节)。切换发生在语言页上时根标题不
// 可见,返回根视图即显示新语言。仅 ARM64。
void refresh_page_title() {
#ifdef __aarch64__
    if (g_lastSettingsPage == 0) return;
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    if (!lzt_core::memory_range_accessible(
            g_lastSettingsPage + SETTINGS_PAGE_CTRL_VEC,
            SETTINGS_PAGE_CTRL_VEC_END - SETTINGS_PAGE_CTRL_VEC + 8, false)) {
        lzt_core::log_write("[SFW] root title bail: page unreadable %p",
                            (void *)g_lastSettingsPage);
        return;
    }
    uintptr_t begin = *reinterpret_cast<volatile uintptr_t *>(
        g_lastSettingsPage + SETTINGS_PAGE_CTRL_VEC);
    uintptr_t end = *reinterpret_cast<volatile uintptr_t *>(
        g_lastSettingsPage + SETTINGS_PAGE_CTRL_VEC_END);
    // 健全性上界：真实向量至多数个 controller，超过即视为页数据损坏
    if (begin == 0 || end <= begin ||
        end - begin > 16 * sizeof(uintptr_t)) {
        lzt_core::log_write("[SFW] root title bail: bad vector %p..%p",
                            (void *)begin, (void *)end);
        return;
    }
    uintptr_t controller = *reinterpret_cast<volatile uintptr_t *>(begin);
    if (controller == 0 ||
        !lzt_core::memory_range_accessible(controller, 8, false) ||
        *reinterpret_cast<volatile uintptr_t *>(controller) !=
            base + OFF_SETTINGS_TITLE_CTRL_VTABLE ||
        !lzt_core::memory_range_accessible(
            controller + SETTINGS_TAB_TITLE_HEAP, 8, true)) {
        uintptr_t vtbl = controller ?
            *reinterpret_cast<volatile uintptr_t *>(controller) : 0;
        lzt_core::log_write("[SFW] root title bail: ctrl=%p vtbl=%p expect=%p",
                            (void *)controller, (void *)vtbl,
                            (void *)(base + OFF_SETTINGS_TITLE_CTRL_VTABLE));
        return;
    }
    // 重解析 [SETTINGS_TITLE](根标题键,构造器同款)
    auto localizeKey = reinterpret_cast<SfwLocalizeFn>(base + OFF_LocalizeKey);
    GameWStr t = localizeKey(L"[SETTINGS_TITLE]");
    const bool longStr = (t.flag & 1) != 0;
    const bool empty = !longStr && (t.flag & 0xFF) == 0;
    // 缺失键会解析为 "<Missing ...>" 前缀：不得写上标题，记录后放弃。
    // 宽短串布局（probe 实证）：字节0=len<<1，数据自 +4 起
    const wchar_t *view = longStr ? reinterpret_cast<const wchar_t *>(t.heap)
                                  : reinterpret_cast<const wchar_t *>(
                                        reinterpret_cast<uintptr_t>(&t) + 4);
    if (empty || (!empty && view[0] == L'<')) {
        if (longStr) delete[] reinterpret_cast<wchar_t *>(t.heap);
        lzt_core::log_write("[SFW] root title skip: [SETTINGS_TITLE] missing");
        return;
    }
    using ReleaseTitleFn = void (*)(uintptr_t, uint32_t);
    auto releaseTitle = reinterpret_cast<ReleaseTitleFn>(base + OFF_ReleaseTitle);
    releaseTitle(controller + SETTINGS_TAB_TITLE, 0);
    *reinterpret_cast<uintptr_t *>(controller + SETTINGS_TAB_TITLE_HEAP) = t.heap;
    memcpy(reinterpret_cast<void *>(controller + SETTINGS_TAB_TITLE), &t, 16);
    // 长 string 的堆归 controller 所有（原版构造序列同样转移所有权，
    // 与 refresh_tab_titles 同规）——不得释放，否则 controller+200 悬挂，
    // 下次 releaseTitle 会对已释放块写 0（v7.12~v7.14 的 UAF,本版修复）
    // 短串的 size 字段是 SSO 数据（v7.12 曾误读为长度），按布局取真长度
    const unsigned trueLen = (t.flag & 1)
        ? static_cast<unsigned>(t.size)
        : static_cast<unsigned>((t.flag & 0xFF) >> 1);
    lzt_core::log_write("[SFW] root title refreshed: ctrl=%p len=%u",
                        (void*)controller, trueLen);
#endif
}

// ---- v7.16 0x2D8 标签文本刷新：已迁移到 lzt_localizer（v7.20）----
// 标签 SetText(vt+752) 刷新由 lzt_localizer::refresh_all()（HostType::Label
// 回填器，含快照比对）接管。

// ============================================================
// v7.19/v7.20：统一本地化刷新入口（Localizer 门面）
// 语言切换只调用一次，设置页 / 主界面所有已加载本地化文本按宿主类型
// 回填当前语言。v7.20 起旧 refresh_tab_titles / refresh_label_texts
// 覆写路径已迁移到 lzt_localizer::refresh_all()（类型化注册表接管）：
//   - TabRow    → 设置行/滑条行/checkbox 行 +184（lzt_localizer 回填）
//   - Label     → 0x2D8 标签 SetText(vt+752)   （lzt_localizer 回填）
//   - RootTitle → 设置根大标题 +184            （refresh_page_title 特判）
// 返回刷新控件数合计。
// ============================================================
int refresh_all_localized() {
    int n = lzt_localizer::refresh_all();
    refresh_page_title();                    // RootTitle（无固定捕获点，设置页自身职责）
    // v7.37：屏幕业务刷新器（如主界面横幅）——框架只遍历，不感知内容
    for (size_t i = 0; i < g_refresherCount; ++i) {
        if (g_refreshers[i]) g_refreshers[i]();
    }
    return n;
}



uint32_t primary_module_tab_id() {
    for (size_t i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].used && g_slots[i].finalized) return g_slots[i].tabId;
    }
    return 0;
}

bool dispatch_module_tab(uintptr_t page, uint32_t tabId) {
#ifdef __aarch64__
    uintptr_t base = lzt_core::base();
    if (page == 0 || base == 0) return false;
    // 经被 hook 的 dispatch 入口调用 → hook_dispatch 命中模块 Tab →
    // 重建模块页面（语言页内容在新页上就位）
    reinterpret_cast<DispatchFn_t>(base + OFF_SettingsDispatch)(page, tabId);
    return true;
#else
    (void)page; (void)tabId;
    return false;
#endif
}

void clear_page_dirty(uintptr_t page) {
    if (page == 0 ||
        !lzt_core::memory_range_accessible(page + SETTINGS_PAGE_DIRTY, 1, true)) {
        return;
    }
    *reinterpret_cast<volatile uint8_t *>(page + SETTINGS_PAGE_DIRTY) = 0;
}

// ============================================================
// PageBuilder 实现
// ============================================================
PageBuilder::PageBuilder(State& s) : m_state(s) {}

bool PageBuilder::add_prompt(const wchar_t* promptKey) {
    return add_prompt(promptKey, -1.0f, -1.0f);   // 自动宽度 + 默认余量
}

bool PageBuilder::add_prompt(const wchar_t* promptKey, float widthDp, float paddingDp) {
    if (promptKey == nullptr || promptKey[0] == L'\0') return false;
    State::Item it;
    it.plan.type = widgets::WidgetPlanItem::PROMPT;
    it.plan.key = promptKey;
    it.plan.widthDp = widthDp;
    it.plan.paddingDp = paddingDp;
    m_state.items.push_back(it);
    return true;
}

WidgetHandle PageBuilder::add_checkbox(const CheckboxDesc& d) {
    if (d.labelKey == nullptr ||
        (d.onClick == nullptr && d.onClickIndexed == nullptr)) {
        lzt_core::log_write("[SFW] add_checkbox reject: null label/callback");
        return WidgetHandle{0};
    }
    if (!m_state.slot) {
        lzt_core::log_write("[SFW] add_checkbox reject: no module context");
        return WidgetHandle{0};
    }
    if (m_state.checkboxSeq >= kMaxWidgetsPerModule) {
        lzt_core::log_write("[SFW] add_checkbox reject: per-module widget limit (%u)",
                            (unsigned)kMaxWidgetsPerModule);
        return WidgetHandle{0};
    }

    ModuleSlot& slot = *static_cast<ModuleSlot*>(m_state.slot);
    CbCacheEntry& c = slot.cbCache[m_state.checkboxSeq];
    uint32_t id = 0;
    if (c.valid) {
        id = c.id;                       // 复用缓存 id：保证跨重建路由稳定
    } else {
        if (d.requestedId != 0) {
            if (!id_available(d.requestedId)) {
                lzt_core::log_write("[SFW] add_checkbox reject: requested id %u conflicts",
                                    d.requestedId);
                return WidgetHandle{0};
            }
            id = d.requestedId;
        } else {
            while (!id_available(g_nextDynamicId)) ++g_nextDynamicId;
            id = g_nextDynamicId++;
        }
        c.valid = true;
        c.id = id;
        if (slot.cbCached == m_state.checkboxSeq) slot.cbCached = m_state.checkboxSeq + 1;
    }
    // 始终以最新回调为准（两形态二选一，带序号的优先）
    c.onClick = d.onClick;
    c.onIndexClick = d.onClickIndexed;

    State::Item it;
    it.plan.type = widgets::WidgetPlanItem::CHECKBOX;
    it.plan.key = d.labelKey;
    it.plan.initialState = d.initialState;
    it.plan.id = id;
    it.onClick = d.onClick;
    m_state.items.push_back(it);
    ++m_state.checkboxSeq;
    return WidgetHandle{id};
}

WidgetHandle PageBuilder::add_checkbox(const wchar_t* labelKey,
                                       bool initialState, CheckboxClickFn onClick) {
    CheckboxDesc d;
    d.labelKey = labelKey;
    d.initialState = initialState;
    d.onClick = onClick;
    d.requestedId = 0;   // 便捷形态不指定 id
    return add_checkbox(d);
}

bool PageBuilder::add_raw_widget(uintptr_t widget, int widthPx) {
    if (widget == 0) return false;
    State::Item it;
    it.plan.type = widgets::WidgetPlanItem::RAW;
    it.plan.rawWidget = widget;
    it.plan.rawWidthPx = widthPx;
    m_state.items.push_back(it);
    return true;
}

int PageBuilder::content_width_px() const {
    return m_state.contentWidthPx;
}

float PageBuilder::ui_scale() const {
    return m_state.scaleF;
}

// ============================================================
// detail::EngineAccess 定义已前移至文件头部（使用点之前）。
// ============================================================

// ============================================================
// 可选工具：bool 配置读写（迁移自主文件 Persist 样板，去业务化）
// ============================================================
bool config::read_bool(const char* key, bool default_value,
                       uintptr_t storage_field_offset) {
    uintptr_t base = lzt_core::base();
    if (base == 0) return default_value;
    uintptr_t cfg = *reinterpret_cast<uintptr_t*>(base + OFF_G_DisplayInfo);
    if (cfg == 0) return default_value;   // 配置对象未就绪：返回默认值且不锁定缓存

    alignas(16) uint8_t keyBuf[24];
    widgets::make_key_string(keyBuf, key);
    using PersistReadBool_t = long (*)(uintptr_t, uintptr_t, uintptr_t);
    auto readBool = reinterpret_cast<PersistReadBool_t>(base + OFF_PersistReadBool);
    long found = readBool(cfg, reinterpret_cast<uintptr_t>(keyBuf),
                          cfg + storage_field_offset);
    widgets::release_key_string(keyBuf);
    // 后端命中时值已写入 storage_field_offset 字节；未命中采用默认值
    return found ? (*reinterpret_cast<uint8_t*>(cfg + storage_field_offset) != 0)
                 : default_value;
}

void config::write_bool(const char* key, bool value, uintptr_t storage_field_offset) {
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    uintptr_t cfg = *reinterpret_cast<uintptr_t*>(base + OFF_G_DisplayInfo);
    if (cfg == 0) {
        lzt_core::log_write("[SFW] write_bool: config object null, drop key=%s", key);
        return;
    }
    *reinterpret_cast<uint8_t*>(cfg + storage_field_offset) = value ? 1 : 0;
    uintptr_t manager = *reinterpret_cast<uintptr_t*>(base + OFF_PersistManager);
    if (manager == 0) {
        lzt_core::log_write("[SFW] write_bool: persist manager null, memory only key=%s", key);
        return;
    }
    alignas(16) uint8_t keyBuf[24];
    widgets::make_key_string(keyBuf, key);
    using PersistSaveBool_t = long (*)(uintptr_t, uintptr_t, char);
    auto saveBool = reinterpret_cast<PersistSaveBool_t>(base + OFF_PersistSave);
    saveBool(manager, reinterpret_cast<uintptr_t>(keyBuf), value ? 1 : 0);
    widgets::release_key_string(keyBuf);
}

// ---- 零字段占用变体：结果在栈上接收，不触碰 DisplayInfo ----
bool config::query_bool(const char* key, bool default_value) {
    uintptr_t base = lzt_core::base();
    if (base == 0) return default_value;
    uintptr_t cfg = *reinterpret_cast<uintptr_t*>(base + OFF_G_DisplayInfo);
    if (cfg == 0) return default_value;   // 配置对象未就绪

    alignas(16) uint8_t keyBuf[24];
    widgets::make_key_string(keyBuf, key);
    uint8_t sink = 0;   // 后端命中时把值写到这里（outPtr 为普通输出指针）
    using PersistReadBool_t = long (*)(uintptr_t, uintptr_t, uintptr_t);
    auto readBool = reinterpret_cast<PersistReadBool_t>(base + OFF_PersistReadBool);
    long found = readBool(cfg, reinterpret_cast<uintptr_t>(keyBuf),
                          reinterpret_cast<uintptr_t>(&sink));
    widgets::release_key_string(keyBuf);
    return found ? (sink != 0) : default_value;
}

int config::query_radio(const char* const* keys, size_t count) {
    if (keys == nullptr) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (keys[i] != nullptr && query_bool(keys[i], false)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void config::save_radio(const char* const* keys, size_t count, int index) {
    if (keys == nullptr) return;
    const int prev = query_radio(keys, count);
    if (prev >= 0 && prev != index) save_bool(keys[prev], false);
    if (index >= 0 && index < static_cast<int>(count)) save_bool(keys[index], true);
}

void config::save_bool(const char* key, bool value) {
    uintptr_t base = lzt_core::base();
    if (base == 0) return;
    uintptr_t manager = *reinterpret_cast<uintptr_t*>(base + OFF_PersistManager);
    if (manager == 0) {
        lzt_core::log_write("[SFW] save_bool: persist manager null, drop key=%s", key);
        return;
    }
    alignas(16) uint8_t keyBuf[24];
    widgets::make_key_string(keyBuf, key);
    using PersistSaveBool_t = long (*)(uintptr_t, uintptr_t, char);
    auto saveBool = reinterpret_cast<PersistSaveBool_t>(base + OFF_PersistSave);
    saveBool(manager, reinterpret_cast<uintptr_t>(keyBuf), value ? 1 : 0);
    widgets::release_key_string(keyBuf);
}

} // namespace lzt_settings
