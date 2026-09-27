// ============================================================
// settings_widgets.cpp — 控件物化原语层实现
//
// 迁移来源（lzt_settings_framework.cpp 历史版本，v48 行号基准）：
//   - GameString / 字符串工具          ← 主文件 Settings 区
//   - insert_custom_tab               ← hkSettingsCreateTab 插入序列 (L1137~1171)
//   - ARM64 页面物化                  ← open_view_angle_page (L1265~1525)
//   - ARM32 页面物化                  ← open_view_angle_page32 (L1595~1745)
//   - ARM64/ARM32 prompt 物化         ← add_view_angle_prompt(_32)
//   - validate_settings_tab_container32 ← ARM32 容器校验 (L1038~1052)
//   - call_s0_float                   ← ARM32 softfp S0 取值 (L1482~1493)
//
// 行为保真原则：除"控件清单循环化"外不改变任何调用序列、参数
// 与时序；历史教训注释（v34/v36/v37/v38 等）随代码一并保留。
// ============================================================

#include "settings_widgets.h"

#include "../lzt_core.h"
#include "../lzt_settings_framework_config.h"
#include "../offsets.h"
#include "localizer.h"   // v7.30:语言页 checkbox 名称显式补捕获为 TabRow

#include <cstring>
#include <wchar.h>

namespace lzt_settings {
namespace widgets {

// 诊断日志门统一为 lzt_settings_framework_config.h 的 LZT_DBG_LOG（v7.37 起全工程一套）。

// ============================================================
// 游戏字符串对象（libc++ std::string / std::wstring 同构）
//   ARM64: 24 字节 {flag(8B), size(8B), heap(8B)}
//   ARM32: 12 字节 {flag(4B), size(4B), heap(4B)}
//   flag bit0 = 长串标志（小端），长串时 heap 为堆指针
// ============================================================
struct GameString {
#ifdef __aarch64__
    uint64_t flag;
    uint64_t size;
    uint64_t heap;
#else
    uint32_t flag;
    uint32_t size;
    uint32_t heap;
#endif
};
using LocalizedString = GameString;

static void free_game_string(GameString& s) {
    if (s.flag & 1) {
        operator delete(reinterpret_cast<void*>((uintptr_t)s.heap));
    }
}

// ============================================================
// 游戏函数指针类型（与主文件历史定义一致）
// ============================================================
typedef void (*SettingsStringCreate_t)(uintptr_t out, uintptr_t text, uint32_t length);
typedef uintptr_t (*SettingsIconLoad_t)(uintptr_t resource);
typedef long (*SettingsAttachTab_t)(uintptr_t container, uintptr_t tab, uint8_t centered,
                                    float uiScale);
typedef uintptr_t (*SettingsLayout_t)(uintptr_t page);
typedef void (*SettingsContentCreate_t)(uintptr_t content);
typedef float (*SettingsScaleFloat_t)(uintptr_t context, float value);
typedef uintptr_t (*CheckboxCreate_t)(uintptr_t page, uint32_t id, uintptr_t labelString,
                                      char initialState, int width);
typedef void (*SettingsAddWidget_t)(uintptr_t content, uintptr_t widget, uint8_t centered,
                                    float uiScale);
typedef uintptr_t (*FontLoad_t)(uintptr_t fontConfig);
typedef uintptr_t (*TextMeasure_t)(uintptr_t ctx, uintptr_t strObj, uint32_t* outW,
                                   uint32_t* outH, float width);
typedef uintptr_t (*TextContainerCreate_t)(uintptr_t container);
typedef void (*ContainerSetPos_t)(uintptr_t container, uint32_t* pos);
typedef void (*TextContainerAdd_t)(uintptr_t container, uintptr_t label);

#ifdef __aarch64__
// sub_14F3354：本地化键字符串（sret 经 X8 返回 24 字节对象）
typedef LocalizedString (*LocalizeKey64_t)(const wchar_t* key);
typedef uintptr_t (*TextLabelCreate_t)(uintptr_t ctx, uintptr_t strObj, int a3, int a4,
                                       uintptr_t style, float fontsize, float x,
                                       float width, float height);
#else
// sub_10F6754：本地化键（ARM32 为 (out, key) 双参写入式）
typedef void (*LocalizeKey32_t)(uintptr_t out, const wchar_t* key);
// ARM32 文本标签创建（softfp ABI：float 参数经核心寄存器传位模式）
typedef uintptr_t (*TextLabelCreate32_t)(uintptr_t ctx, float fontsize, int a3,
                                         float width, float height, uintptr_t strObj,
                                         int a7, int a8, uintptr_t style);
#endif

// ---- 本层私有函数指针（resolve_helpers 解析；引擎 hook 的 trampoline 不在此管理）----
static SettingsStringCreate_t pStringCreate = nullptr;        // wstring 构造(out,key,len)
static SettingsIconLoad_t    pIconLoad = nullptr;             // 图标资源加载
static SettingsAttachTab_t   pAttachTab = nullptr;            // Tab 挂载到容器
static SettingsLayout_t      pPageLayout = nullptr;           // 页面级 layout（ARM64 收尾用）
static SettingsContentCreate_t pContentCreate = nullptr;      // content 构造
static SettingsScaleFloat_t  pScaleFloat = nullptr;           // scaleFloat(ctx,v)
static CheckboxCreate_t      pCheckboxCreate = nullptr;       // checkbox 创建
static SettingsAddWidget_t   pAddWidget = nullptr;            // widget 挂到 content

bool resolve_helpers() {
    uintptr_t base = lzt_core::base();
    if (!base) {
        lzt_core::log_write("[SFW] resolve_helpers fail: base not ready");
        return false;
    }
    pStringCreate   = reinterpret_cast<SettingsStringCreate_t>(base + OFF_SettingsStringCreate);
    pIconLoad       = reinterpret_cast<SettingsIconLoad_t>(base + OFF_SettingsIconLoad);
    pAttachTab      = reinterpret_cast<SettingsAttachTab_t>(base + OFF_SettingsAttach);
    pPageLayout     = reinterpret_cast<SettingsLayout_t>(base + OFF_SettingsLayout);
    pContentCreate  = reinterpret_cast<SettingsContentCreate_t>(base + OFF_SettingsContentCreate);
    pScaleFloat     = reinterpret_cast<SettingsScaleFloat_t>(base + OFF_SettingsScaleFloat);
    pCheckboxCreate = reinterpret_cast<CheckboxCreate_t>(base + OFF_CheckboxCreate);
    pAddWidget      = reinterpret_cast<SettingsAddWidget_t>(base + OFF_SettingsAddWidget);

    const bool ok = pStringCreate && pIconLoad && pAttachTab && pPageLayout &&
                    pContentCreate && pScaleFloat && pCheckboxCreate && pAddWidget;
    if (!ok) {
        lzt_core::log_write("[SFW] resolve_helpers fail: str=%d icon=%d attach=%d layout=%d "
                            "content=%d scaleF=%d cb=%d addW=%d",
                            pStringCreate != nullptr, pIconLoad != nullptr,
                            pAttachTab != nullptr, pPageLayout != nullptr,
                            pContentCreate != nullptr, pScaleFloat != nullptr,
                            pCheckboxCreate != nullptr, pAddWidget != nullptr);
    }
    return ok;
}

// ============================================================
// 共享工具：读取 UI 缩放上下文（两架构同构）
// ============================================================
static uintptr_t get_ui_context() {
    return *reinterpret_cast<uintptr_t*>(lzt_core::base() + OFF_SettingsUIScaleContext);
}

// scaleInt 换算（scaleInt(ctx,v) = (int)(scale*v)，双架构同语义）
static int scale_int(uintptr_t uiContext, int v) {
    using ScaleInt_t = int (*)(uintptr_t, int);
    auto scaleIntF = reinterpret_cast<ScaleInt_t>(lzt_core::base() + OFF_SettingsUIScale);
    return scaleIntF(uiContext, v);
}

// ---- 布局查询（双架构安全）----
#ifdef __arm__
static float call_s0_float(uintptr_t fn);   // 定义见下方 ARM32 工具区
#endif

int query_content_width_px() {
#ifdef __arm__
    // v34：ARM32 内容宽返回值在 S0，必须走 call_s0_float
    float w = call_s0_float(lzt_core::base() + OFF_SettingsContentWidth);
    return (int)w;
#else
    using ContentWidthFn_t = float (*)();
    auto getContentWidth = reinterpret_cast<ContentWidthFn_t>(
        lzt_core::base() + OFF_SettingsContentWidth);
    return (int)getContentWidth();
#endif
}

float query_ui_scale() {
    uintptr_t uiContext = get_ui_context();
    if (!uiContext) return 0.0f;
    return pScaleFloat(uiContext, 1.0f);
}

bool run_page_layout(uintptr_t page) {
    if (!pPageLayout) return false;
    pPageLayout(page);
    return true;
}

// ---- 游戏 key 字符串构造（迁移自主文件 build_key_string）----
void make_key_string(void* out, const char* key) {
    GameString& s = *reinterpret_cast<GameString*>(out);
    size_t len = strlen(key);
    char* buf = static_cast<char*>(operator new(0x20u));   // 32 字节堆缓冲
    s.flag = 0x20 | 1;                                     // capacity 32 | 堆标志
    s.size = static_cast<decltype(s.size)>(len);
    s.heap = static_cast<decltype(s.heap)>((uintptr_t)buf);
    strcpy(buf, key);
}

void release_key_string(void* out) {
    free_game_string(*reinterpret_cast<GameString*>(out));
}

#ifdef __arm__
// sub_6D3FD0（内容宽度查询）返回值在 S0（VFP），R0 留给内存审计差值。
// 本 so 是 softfp（float 声明从 R0 取返回值），C 函数指针声明会读到审计
// 差值（实测 0）→ 必须用内联汇编从 S0 取位模式。（v34 教训）
static float call_s0_float(uintptr_t fn) {
    uint32_t bits;
    __asm__ volatile(
        "blx %[fn]\n"
        "vmov %[bits], s0\n"
        : [bits] "=r"(bits)
        : [fn] "r"(fn)
        : "r0", "r1", "r2", "r3", "r12", "lr", "s0", "memory", "cc");
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// ARM32 tab 容器校验：只读类型与内存校验，异常容器安全跳过自定义 Tab。
// （不应擅自清空/重建容器——会破坏原版所有 Tab。）
static bool validate_tab_container32(uintptr_t container) {
    if (!container ||
        !lzt_core::memory_range_accessible(container, sizeof(uintptr_t), false)) {
        return false;
    }
    uintptr_t actualVtable = *reinterpret_cast<uintptr_t*>(container);
    uintptr_t expectedVtable = lzt_core::base() + OFF_SettingsTabContainerVtable;
    if (actualVtable != expectedVtable) {
        lzt_core::log_write("[SFW] insertion32 skip: invalid page+%u container=%p "
                            "vtable=%p expected=%p (possible lifecycle corruption)",
                            (unsigned)SETTINGS_PAGE_CONTAINER, (void*)container,
                            (void*)actualVtable, (void*)expectedVtable);
        return false;
    }
    return true;
}
#endif // __arm__

// ============================================================
// Tab 注入原语（迁移自 hkSettingsCreateTab 插入序列，双架构共享；
// 仅 ARM32 多一步容器校验）。直调 createTabFn trampoline，不经
// hook 入口，天然无递归风险。
// ============================================================
bool insert_custom_tab(uintptr_t page, uint32_t tabId, const wchar_t* titleKey,
                       uintptr_t iconNormalOffset, uintptr_t iconSelectedOffset,
                       uintptr_t createTabFn) {
    if (!createTabFn || !pStringCreate || !pIconLoad || !pAttachTab) {
        lzt_core::log_write("[SFW] tab insert skip: helpers unavailable");
        return false;
    }

    uintptr_t container = *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_CONTAINER);
    if (container == 0) {
        lzt_core::log_write("[SFW] tab insert skip: container null before anchor tab");
        return false;
    }
#ifdef __arm__
    if (!validate_tab_container32(container)) {
        return false;
    }
#endif

    lzt_core::log_write("[SFW] tab insert: page=%p container=%p id=%u",
                        (void*)page, (void*)container, tabId);
    GameString titleObject = {};
    pStringCreate(reinterpret_cast<uintptr_t>(&titleObject),
                  reinterpret_cast<uintptr_t>(titleKey), (uint32_t)wcslen(titleKey));
    uintptr_t normalIcon = pIconLoad(lzt_core::base() + iconNormalOffset);
    uintptr_t selectedIcon = pIconLoad(lzt_core::base() + iconSelectedOffset);
    if (normalIcon == 0 || selectedIcon == 0) {
        lzt_core::log_write("[SFW] tab insert skip: icon load failed normal=%p selected=%p",
                            (void*)normalIcon, (void*)selectedIcon);
        free_game_string(titleObject);
        return false;
    }
    using CreateTabFn_t = uintptr_t (*)(uintptr_t, uint32_t, uintptr_t, uintptr_t, uintptr_t);
    uintptr_t tab = reinterpret_cast<CreateTabFn_t>(createTabFn)(
        page, tabId, reinterpret_cast<uintptr_t>(&titleObject), normalIcon, selectedIcon);
    free_game_string(titleObject);   // createTab 内部已拷贝，释放构造串（与原版注册序列一致）
    if (tab == 0) {
        lzt_core::log_write("[SFW] tab insert skip: creation failed id=%u", tabId);
        return false;
    }

    uintptr_t uiContext = get_ui_context();
    if (uiContext == 0) {
        // 与历史行为一致：Tab 已创建但放弃挂载，交由调用方继续原流程
        lzt_core::log_write("[SFW] tab insert skip: UI scale context null");
        return false;
    }
    float uiScale = static_cast<float>(scale_int(uiContext, 0));
    pAttachTab(container, tab, 0, uiScale);
    lzt_core::log_write("[SFW] tab insert: attached tab=%p id=%u", (void*)tab, tabId);
    return true;
}

// ============================================================
// ARM32：prompt 物化（迁移自 add_view_angle_prompt32）
// ============================================================
#ifdef __arm__
static bool materialize_prompt32(uintptr_t content, uintptr_t uiContext,
                                 const WidgetPlanItem& item, int checkboxWidth) {
    // 1. 本地化 prompt 键（sub_10F6754(out12B, key)，缺省键原样拷贝）
    LocalizedString promptStr = {};
    auto localizeKey = reinterpret_cast<LocalizeKey32_t>(lzt_core::base() + OFF_LocalizeKey);
    localizeKey(reinterpret_cast<uintptr_t>(&promptStr), item.key);

    // 2. 加载文本上下文
    uintptr_t fontConfig = *reinterpret_cast<uintptr_t*>(lzt_core::base() + OFF_FontContext);
    auto fontLoad = reinterpret_cast<FontLoad_t>(lzt_core::base() + OFF_FontLoad);
    uintptr_t textCtx = fontLoad(fontConfig);
    LZT_DBG_LOG("[SFW] p32 ctx: fontCfg=%p textCtx=%p", (void*)fontConfig, (void*)textCtx);

    // 3. 文本测量：自动宽度 = checkboxWidth - 8*scale - 20*scale（原版 v48）
    //    自定义宽度 = scaleInt(widthDp)。宽度出参写入独立缓冲后丢弃——
    //    不能传 &rect[0]，否则容器 x 被测量宽度覆写（历史教训）。
    int promptWidth = item.widthDp < 0.0f
        ? checkboxWidth - scale_int(uiContext, 8) - scale_int(uiContext, 20)
        : scale_int(uiContext, (int)item.widthDp);
    int padDp = item.paddingDp < 0.0f ? 10 : (int)item.paddingDp;
    uint32_t measureW = 0;                              // 原版 v43：丢弃的宽度出参
    uint32_t rect[4] = {
        (uint32_t)scale_int(uiContext, 4),   // x（原版 v47[0]）
        (uint32_t)scale_int(uiContext, 2),   // y（原版 v47[1]）
        (uint32_t)promptWidth,               // w（原版 v48）
        0                                    // h（先置 0，测量后回填；原版 v49）
    };
    auto textMeasure = reinterpret_cast<TextMeasure_t>(lzt_core::base() + OFF_TextMeasure);
    textMeasure(textCtx, reinterpret_cast<uintptr_t>(&promptStr),
                &measureW, &rect[3], (float)promptWidth);   // outH 写回 rect[3]
    rect[3] += (uint32_t)scale_int(uiContext, padDp);

    // 4. 创建文本容器 new(CONTAINER_ALLOC) + 构造 + setPos(vt+VT_SETPOS, rect4)
    uintptr_t container = reinterpret_cast<uintptr_t>(
        operator new((size_t)SETTINGS_CONTAINER_ALLOC));
    auto containerCreate = reinterpret_cast<TextContainerCreate_t>(
        lzt_core::base() + OFF_TextContainerCreate);
    containerCreate(container);
    uintptr_t containerVtable = *reinterpret_cast<uintptr_t*>(container);
    uintptr_t setPosFunc = *reinterpret_cast<uintptr_t*>(containerVtable + SETTINGS_VT_SETPOS);
    reinterpret_cast<ContainerSetPos_t>(setPosFunc)(container, rect);

    // 5. 创建文本标签（第二次 fontLoad 与原版一致）
    //    v36 教训：style 源就是槽地址本身（base+OFF_PromptStyle），直接传
    //    槽地址给 styleCopy，【禁止】再解引用（v35 误解引用 .bss 内容 0 → SIGSEGV）。
    uintptr_t textCtx2 = fontLoad(fontConfig);
    float fontSize = (float)scale_int(uiContext, 8);
    alignas(16) uint8_t style[16];
    auto styleCopy = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(
        lzt_core::base() + OFF_StyleCopy);
    styleCopy(reinterpret_cast<uintptr_t>(style), lzt_core::base() + OFF_PromptStyle);
    auto labelCreate = reinterpret_cast<TextLabelCreate32_t>(
        lzt_core::base() + OFF_TextLabelCreate);
    uintptr_t label = labelCreate(textCtx2, fontSize, 0,
                                  (float)promptWidth, (float)rect[3],
                                  reinterpret_cast<uintptr_t>(&promptStr), 0, 0,
                                  reinterpret_cast<uintptr_t>(style));

    // 6. 标签加入容器，容器加入 content（uiScale = scaleInt(ctx,0)=0，与原版一致）
    auto containerAdd = reinterpret_cast<TextContainerAdd_t>(
        lzt_core::base() + OFF_TextContainerAdd);
    containerAdd(container, label);
    float zeroScale = (float)scale_int(uiContext, 0);
    pAddWidget(content, container, 0, zeroScale);

    free_game_string(promptStr);
    return true;
}
#endif // __arm__

// ============================================================
// ARM64：prompt 物化（迁移自 add_view_angle_prompt）
// ============================================================
#ifdef __aarch64__
static bool materialize_prompt64(uintptr_t content, uintptr_t uiContext, float scale,
                                 int checkboxWidth, float uiScale,
                                 const WidgetPlanItem& item) {
    // 1. 本地化 prompt 键（sret 返回 24 字节对象）
    auto localizeKey = reinterpret_cast<LocalizeKey64_t>(lzt_core::base() + OFF_LocalizeKey);
    LocalizedString promptStr = localizeKey(item.key);

    // 2. 加载字体/文本上下文
    uintptr_t fontConfig = *reinterpret_cast<uintptr_t*>(lzt_core::base() + OFF_FontContext);
    auto fontLoad = reinterpret_cast<FontLoad_t>(lzt_core::base() + OFF_FontLoad);
    uintptr_t textCtx = fontLoad(fontConfig);
    LZT_DBG_LOG("[SFW] p64 prompt step2 ctx=%p", (void*)textCtx);

    // 3. 测量文本尺寸（自动宽 = checkboxWidth - 20·scale；自定义宽按 dp 换算）
    int promptWidth = item.widthDp < 0.0f
        ? checkboxWidth - (int)(scale * 20.0f)
        : (int)pScaleFloat(uiContext, item.widthDp);
    float padPx = item.paddingDp < 0.0f ? scale * 10.0f
                                        : pScaleFloat(uiContext, item.paddingDp);
    auto textMeasure = reinterpret_cast<TextMeasure_t>(lzt_core::base() + OFF_TextMeasure);
    uint32_t measureW = 0, measureH = 0;
    textMeasure(textCtx, reinterpret_cast<uintptr_t>(&promptStr),
                &measureW, &measureH, (float)promptWidth);
    int promptHeight = (int)measureH + (int)padPx;
    LZT_DBG_LOG("[SFW] p64 prompt step3 w=%d h=%d", promptWidth, promptHeight);

    // 4. 创建文本容器 new(CONTAINER_ALLOC)
    auto containerCreate = reinterpret_cast<TextContainerCreate_t>(
        lzt_core::base() + OFF_TextContainerCreate);
    uintptr_t container = reinterpret_cast<uintptr_t>(
        operator new((size_t)SETTINGS_CONTAINER_ALLOC));
    containerCreate(container);

    // 5. 设置容器位置和尺寸 vt+VT_SETPOS(container, [x,y,w,h])
    //    注意 sub_1526C9C 读取 a2[0..3] 四个 int，w/h 写入 container+76/+80
    //    供 AddWidget 自动堆叠时读取高度。
    uintptr_t containerVtable = *reinterpret_cast<uintptr_t*>(container);
    uintptr_t setPosFunc = *reinterpret_cast<uintptr_t*>(containerVtable + SETTINGS_VT_SETPOS);
    uint32_t promptRect[4] = {
        (uint32_t)(int)(scale * 4.0f),   // x
        (uint32_t)(int)(scale * 2.0f),   // y
        (uint32_t)promptWidth,           // width
        (uint32_t)promptHeight           // height
    };
    reinterpret_cast<ContainerSetPos_t>(setPosFunc)(container, promptRect);
    LZT_DBG_LOG("[SFW] p64 prompt step5 pos ok");

    // 6. 创建文本标签（第二次 fontLoad 与原版一致）
    uintptr_t textCtx2 = fontLoad(fontConfig);
    int fontSize = (int)(scale * 8.0f);
    alignas(16) uint8_t style[16];
    memcpy(style, reinterpret_cast<void*>(lzt_core::base() + OFF_PromptStyle), 16);
    auto labelCreate = reinterpret_cast<TextLabelCreate_t>(lzt_core::base() + OFF_TextLabelCreate);
    uintptr_t label = labelCreate(textCtx2, reinterpret_cast<uintptr_t>(&promptStr),
                                  0, 0, reinterpret_cast<uintptr_t>(style),
                                  (float)fontSize, 0.0f, (float)promptWidth,
                                  (float)promptHeight);
    LZT_DBG_LOG("[SFW] p64 prompt step6 label=%p", (void*)label);

    // 7. 标签加入容器，容器加入 content
    auto containerAdd = reinterpret_cast<TextContainerAdd_t>(
        lzt_core::base() + OFF_TextContainerAdd);
    containerAdd(container, label);
    pAddWidget(content, container, 0, uiScale);

    // 8. 释放本地化字符串
    free_game_string(promptStr);
    return true;
}
#endif // __aarch64__

// ============================================================
// ARM64：整页物化（迁移自 open_view_angle_page stage10~150）
// 差异要点：手写 release+attach 挂载序列；无二次 layout；尾部
// pSettingsLayout(page) 由 dispatch 层负责（见引擎 hook_dispatch）。
// ============================================================
#ifdef __aarch64__
static bool materialize_page64(uintptr_t page, const wchar_t* titleKey,
                               const WidgetPlanItem* items, size_t count) {
    lzt_core::set_stage(10);
    lzt_core::log_write("[SFW] p64 begin page=%p title=%ls items=%zu",
                        (void*)page, titleKey, count);

    if (!page) {
        lzt_core::log_write("[SFW] p64 skip: page null");
        return false;
    }

    // ---- stage=20/30: owner = *(page+216)；controller = *(owner+8) ----
    // 原版: v29 = *(_QWORD **)(*(_QWORD *)(a1 + 216) + 8LL);
    lzt_core::set_stage(20);
    uintptr_t owner = *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_OWNER);
    if (!owner) {
        lzt_core::log_write("[SFW] p64 stage=20 fail: owner null page=%p", (void*)page);
        return false;
    }
    lzt_core::set_stage(30);
    uintptr_t controller = *reinterpret_cast<uintptr_t*>(owner + SETTINGS_OWNER_CONTROLLER);
    if (!controller) {
        lzt_core::log_write("[SFW] p64 stage=30 fail: controller null owner=%p", (void*)owner);
        return false;
    }
    LZT_DBG_LOG("[SFW] p64 stage=30 controller=%p", (void*)controller);

    // ---- 标题：LocalizeKey 本地化后写入 controller 标题字段 ----
    // （迁移自 set_controller_title：复用原版本地化 sub_14F3354，自动完成
    //   方括号识别、本地化表查找、缺失加 missing 前缀；随后按原版序列
    //   releaseTitle(controllerTitle,0) → 写 heap 指针 → 拷贝 flag+size 16 字节）
    {
        auto localizeKey = reinterpret_cast<LocalizeKey64_t>(lzt_core::base() + OFF_LocalizeKey);
        LocalizedString title = localizeKey(titleKey);
        using TitleRelease_t = void (*)(uintptr_t, uint32_t);
        auto releaseTitle = reinterpret_cast<TitleRelease_t>(lzt_core::base() + OFF_ReleaseTitle);
        auto controllerTitle = reinterpret_cast<uint8_t*>(controller + SETTINGS_CONTROLLER_TITLE);
        releaseTitle(reinterpret_cast<uintptr_t>(controllerTitle), 0);
        *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_TITLE_HEAP) = title.heap;
        memcpy(controllerTitle, &title, 16);   // flag + size（堆指针单列）
        lzt_core::log_write("[SFW] p64 s30 title ok ctrl=%p", (void*)controller);
    }

    lzt_core::set_stage(40);
    uintptr_t content = reinterpret_cast<uintptr_t>(
        operator new((size_t)SETTINGS_CONTENT_ALLOC));
    if (!content) {
        lzt_core::log_write("[SFW] p64 stage=40 fail: content allocation failed");
        return false;
    }

    lzt_core::set_stage(50);
    pContentCreate(content);

    // ---- stage=70/80: 解析 vtable[VT_LAYOUT] 并设置布局 ----
    lzt_core::set_stage(70);
    uintptr_t contentVtable = *reinterpret_cast<uintptr_t*>(content);
    using ContentLayout_t = void (*)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t);
    uintptr_t layoutFuncPtr = *reinterpret_cast<uintptr_t*>(contentVtable + SETTINGS_VT_LAYOUT);
    auto setContentLayout = reinterpret_cast<ContentLayout_t>(layoutFuncPtr);

    uintptr_t uiContext = get_ui_context();
    if (!uiContext) {
        lzt_core::log_write("[SFW] p64 stage=70 fail: UI scale context null");
        return false;
    }
    using ContentWidthFn_t = float (*)();
    auto getContentWidth = reinterpret_cast<ContentWidthFn_t>(
        lzt_core::base() + OFF_SettingsContentWidth);
    float fx = pScaleFloat(uiContext, 31.0f);
    float fy = pScaleFloat(uiContext, 72.0f);
    float fw = pScaleFloat(uiContext, getContentWidth());
    // v7.25 A 方案（已禁用，v7.26 起走 B 滚动容器）：
    //   content 高度自适应 items 数。保留代码、默认不启用——真机证实
    //   单纯加高不产生滚动/滚动条，已转移至 B（Sexy::ScrollWidget）。
    constexpr bool kAdaptiveContentHeight = false;
    float fh = pScaleFloat(uiContext, 380.0f);
    if (kAdaptiveContentHeight) {
        const unsigned baseItems = 2;
        float rowH = pScaleFloat(uiContext, 112.0f);
        float fhBase = pScaleFloat(uiContext, 380.0f);
        float extra = count > baseItems ? (static_cast<float>(count - baseItems) * rowH) : 0.0f;
        fh = fhBase + extra;
        float fhCap = pScaleFloat(uiContext, 8192.0f);
        if (fh > fhCap) fh = fhCap;
    }
    setContentLayout(content, (uint32_t)fx, (uint32_t)fy, (uint32_t)fw, (uint32_t)fh);
    lzt_core::log_write("[SFW] p64 s70 layout %u,%u %ux%u (items=%zu adaptive=%d)",
                        (unsigned)fx, (unsigned)fy, (unsigned)fw, (unsigned)fh,
                        count, kAdaptiveContentHeight ? 1 : 0);

    // ---- stage=72: B 方案（v7.27）——最小干预：仅扩展内嵌滚动层内容高度，触发原生滚动 ----
    // 原版 content(sub_A53C18) 已自建内嵌滚动层(对象在 content+192)，checkbox 经
    // pAddWidget 挂入其内容 pane 并自动累加 pane 高度。A 不滚根因=滚动层内容尺寸未超
    // 视口；此前各版手工改写滚动层视口/mode/offset 字段反而破坏 content 布局对滚动
    // 层的定位，导致"全部滚到底/顶部留白"(v7.26~v7.27 教训)。此处改为**最小干预**：
    // 只把滚动层内容尺寸 +312/+316 扩为 count 行，显示位置等全部交给游戏原版布局。
    // 真机留白若仍存在，则从"内容尺寸设错"或"content 布局对滚动层的定位"两方面查，
    // 不再手动写占用字段。此处同时打印滚动层位置/视口/内容/偏移以对照运行时变化。
    uintptr_t scrollLayer = *reinterpret_cast<uintptr_t*>(content + 192);
    if (scrollLayer) {
        // 内容尺寸已在 s69（content 布局前）前置设置，此处仅复检对照
        // 诊断日志：滚动层位置 + 视口 + 内容 + 状态 + 偏移
        int slX = *reinterpret_cast<int*>(scrollLayer + WIDGET_OFF_X);
        int slY = *reinterpret_cast<int*>(scrollLayer + WIDGET_OFF_Y);
        int slW = *reinterpret_cast<int*>(scrollLayer + WIDGET_OFF_W);
        int slH = *reinterpret_cast<int*>(scrollLayer + WIDGET_OFF_H);
        int cntW = *reinterpret_cast<int*>(scrollLayer + SCROLL_CONTENT_W);
        int cntH = *reinterpret_cast<int*>(scrollLayer + SCROLL_CONTENT_H);
        int state = *reinterpret_cast<int*>(scrollLayer + SCROLL_STATE);
        float offW = *reinterpret_cast<float*>(scrollLayer + SCROLL_OFF_W);
        float offH = *reinterpret_cast<float*>(scrollLayer + SCROLL_OFF_H);
        float vpW = *reinterpret_cast<float*>(scrollLayer + SCROLL_VP_W);
        float vpH = *reinterpret_cast<float*>(scrollLayer + SCROLL_VP_H);
        lzt_core::log_write("[SFW] p64 s72 scroll=%p pos=%d,%d sz=%ux%u "
                            "content=%ux%u state=0x%x vp=%.0f,%.0f off=%.0f,%.0f",
                            (void*)scrollLayer, slX, slY, slW, slH, cntW, cntH,
                            state, vpW, vpH, offW, offH);
    } else {
        lzt_core::log_write("[SFW] p64 s72 fail: no inner scroll layer (content+192=0)");
    }

    // ---- stage=85: 控件清单物化 ----
    lzt_core::set_stage(85);
    // UI Scale：DataSharing 原版调 scaleInt(ctx,0) 恒为 0（addWidget 参数）
    float uiScale = 0.0f;
    // 精确复刻 DataSharing 的 Checkbox 宽度计算 v13：
    //   v9 = scale*8；checkboxWidth = (int)(fw - v9) - v9
    float scale = pScaleFloat(uiContext, 1.0f);
    int v9 = (int)(scale * 8.0f);
    int checkboxWidth = (int)(fw - (float)v9) - v9;
    lzt_core::log_write("[SFW] p64 s85 items=%zu cbw=%d", count, checkboxWidth);

    for (size_t i = 0; i < count; ++i) {
        const WidgetPlanItem& it = items[i];
        // Debug 哨兵：type 应为 0/1/2；垃圾值即清单内存被异步改写
        LZT_DBG_LOG("[SFW] p64 w%zu type=%u", i, it.type);
        switch (it.type) {
        case WidgetPlanItem::PROMPT:
            LZT_DBG_LOG("[SFW] p64 w%zu PROMPT begin key=%ls", i, it.key);
            materialize_prompt64(content, uiContext, scale, checkboxWidth, uiScale, it);
            LZT_DBG_LOG("[SFW] p64 w%zu PROMPT ok", i);
            break;
        case WidgetPlanItem::CHECKBOX: {
            LZT_DBG_LOG("[SFW] p64 w%zu CB begin id=%u state=%d",
                        i, it.id, it.initialState ? 1 : 0);
            // 创建 Checkbox 标签串（TitleString 构造，与 DataSharing 一致）
            alignas(16) uint8_t labelString[24] = {};
            using TitleStringCreate_t = void (*)(uintptr_t, uintptr_t, uint32_t);
            auto titleStringCreate = reinterpret_cast<TitleStringCreate_t>(
                lzt_core::base() + OFF_SettingsTitleStringCreate);
            titleStringCreate(reinterpret_cast<uintptr_t>(labelString),
                              reinterpret_cast<uintptr_t>(it.key),
                              (uint32_t)wcslen(it.key));
            // 注意：CheckboxCreate 第一参数是 page（内部读回调字段），非 content
            uintptr_t cb = pCheckboxCreate(page, it.id,
                                           reinterpret_cast<uintptr_t>(labelString),
                                            it.initialState ? 1 : 0, checkboxWidth);
            LZT_DBG_LOG("[SFW] p64 w%zu CB created=%p", i, (void*)cb);
            pAddWidget(content, cb, 0, uiScale);
            LZT_DBG_LOG("[SFW] p64 w%zu CB added", i);
            // v7.30：语言页 checkbox 名称随语言切换刷新。构建期 g_rowCaptureSuppress
            // 拦停了 hk_row_init 的自动捕获→checkbox 行未入注册表。此处显式补捕获
            // (行,名称键) 为 TabRow（标题在 +184，经 refresh_tabrow 回填），机制级。
            lzt_localizer::capture(lzt_localizer::HostType::TabRow, cb, it.key, false, nullptr);
            break;
        }
        case WidgetPlanItem::RAW:
            pAddWidget(content, it.rawWidget, 0, uiScale);
            break;
        default:
            break;
        }
        // Debug 哨兵：单控件物化完成。正式版若卡在 "s85" 与 "s90" 之间，
        // 可临时切 kDebugMode=true 获取逐控件定位。
        LZT_DBG_LOG("[SFW] p64 w%zu done", i);
    }

    // ---- B(v7.29) 循环物化后二次 content layout（复刻原版）----
    // DataSharing/PlayerTargeting 等原版页在加完全控件后、挂载前再次调用 content
    // vtable+416 layout。该次布局依据最终的 pane 内容高度驱动内嵌滚动层重算滚动：
    // 内容高 > 可视化区即可滚动。ARM64 p64 此前缺失这"二次布局"→ 滚动层依据内容
    // 仍为初始 0 判定"无需滚动"，成为不滚的根因。这里补齐，且不手写滚动层字段。
    setContentLayout(content, (uint32_t)fx, (uint32_t)fy, (uint32_t)fw, (uint32_t)fh);
    lzt_core::log_write("[SFW] p64 s84 relayout %u,%u %ux%u (items=%zu)",
                        (unsigned)fx, (unsigned)fy, (unsigned)fw, (unsigned)fh, count);

    // ---- stage=90~110: 释放旧 content 并挂载新 content（严格复刻原版）----
    lzt_core::set_stage(90);
    uintptr_t controllerVtable = *reinterpret_cast<uintptr_t*>(controller);
    uintptr_t oldContent = *reinterpret_cast<uintptr_t*>(
        controller + SETTINGS_CONTROLLER_CONTENT);
    lzt_core::log_write("[SFW] p64 s90 old=%p", (void*)oldContent);
    lzt_core::set_stage(100);
    if (oldContent != 0) {
        // a. controller->vtable[96/8](controller, oldContent) — 从链表移除旧内容。
        //    必须传 oldContent（X1）：sub_169A024 靠它定位链表节点，缺失会导致
        //    旧内容残留为悬空指针，后续遍历崩溃。
        using ReleaseNotify_t = void (*)(uintptr_t, uintptr_t);
        auto releaseNotify = reinterpret_cast<ReleaseNotify_t>(
            *reinterpret_cast<uintptr_t*>(controllerVtable + 96));
        releaseNotify(controller, oldContent);
        // b. 重读旧内容（releaseNotify 可能已将其清零）
        uintptr_t old2 = *reinterpret_cast<uintptr_t*>(
            controller + SETTINGS_CONTROLLER_CONTENT);
        if (old2 != 0) {
            // c. oldContent->vtable[24/8](oldContent) — 析构
            uintptr_t oldVt = *reinterpret_cast<uintptr_t*>(old2);
            auto destruct = reinterpret_cast<void (*)(uintptr_t)>(
                *reinterpret_cast<uintptr_t*>(oldVt + 24));
            destruct(old2);
        }
        // d. 清空旧内容字段
        *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_CONTENT) = 0;
    }

    lzt_core::set_stage(110);
    // 重读 controllerVtable（releaseNotify 可能修改 vtable，原版在此重读 *v30）
    controllerVtable = *reinterpret_cast<uintptr_t*>(controller);
    *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_CONTENT) = content;
    using AttachNew_t = void (*)(uintptr_t, uintptr_t);
    auto attachNew = reinterpret_cast<AttachNew_t>(
        *reinterpret_cast<uintptr_t*>(controllerVtable + 88));
    attachNew(controller, content);

    lzt_core::set_stage(150);
    lzt_core::log_write("[SFW] p64 stage=150 success page=%p content=%p controller=%p",
                        (void*)page, (void*)content, (void*)controller);
    return true;
}
#endif // __aarch64__

// ============================================================
// ARM32：整页物化（迁移自 open_view_angle_page32 stage10~150）
// 差异要点：MountContent 单函数挂载；控件后二次五参 layout；
// mount 前重取 controller；无页面级 layout 收尾（v37/v38 教训）。
// ============================================================
#ifdef __arm__
static bool materialize_page32(uintptr_t page, const wchar_t* titleKey,
                               const WidgetPlanItem* items, size_t count) {
    lzt_core::set_stage(10);
    lzt_core::log_write("[SFW] p32 begin page=%p title=%ls items=%zu",
                        (void*)page, titleKey, count);

    if (!page) {
        lzt_core::log_write("[SFW] p32 skip: page null");
        return false;
    }

    // ---- stage=20/30: controller = *(*(page+148)+4) ----
    lzt_core::set_stage(20);
    uintptr_t owner = *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_OWNER);
    if (!owner) {
        lzt_core::log_write("[SFW] p32 stage=20 fail: owner null page=%p", (void*)page);
        return false;
    }
    lzt_core::set_stage(30);
    uintptr_t controller = *reinterpret_cast<uintptr_t*>(owner + SETTINGS_OWNER_CONTROLLER);
    if (!controller) {
        lzt_core::log_write("[SFW] p32 stage=30 fail: controller null owner=%p", (void*)owner);
        return false;
    }
    LZT_DBG_LOG("[SFW] p32 stage=30 controller=%p", (void*)controller);

    // ---- 标题：wstring 构造 + 原版 sub_6D7AD0(controller, wstr) ----
    // （迁移自 open_view_angle_page32 标题段；与 ARM64 的 LocalizeKey 路径
    //   不同，保持各架构历史行为不变）
    {
        GameString titleStr = {};
        pStringCreate(reinterpret_cast<uintptr_t>(&titleStr),
                      reinterpret_cast<uintptr_t>(titleKey),
                      (uint32_t)wcslen(titleKey));
        auto setTitle = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(
            lzt_core::base() + OFF_SetControllerTitle);
        setTitle(controller, reinterpret_cast<uintptr_t>(&titleStr));
        free_game_string(titleStr);
        lzt_core::log_write("[SFW] p32 s30 title ok ctrl=%p", (void*)controller);
    }

    lzt_core::set_stage(40);
    uintptr_t uiContext = get_ui_context();
    if (!uiContext) {
        lzt_core::log_write("[SFW] p32 stage=40 fail: UI scale context null");
        return false;
    }
    // v34：内容宽度必须走 call_s0_float 取 S0；float(*)() 声明读到审计差值 0
    float rawContentW = call_s0_float(lzt_core::base() + OFF_SettingsContentWidth);
    // 原版 v4 = scaleFloat(30.0)+scaleInt(4)；v37：y 立即数为 72.0f
    float fx = pScaleFloat(uiContext, 30.0f) + (float)scale_int(uiContext, 4);
    float fy = pScaleFloat(uiContext, 72.0f);
    float fw = pScaleFloat(uiContext, rawContentW);
    float fh = pScaleFloat(uiContext, 380.0f);
    int checkboxWidth = (int)(fw - (float)scale_int(uiContext, 8));
    lzt_core::log_write("[SFW] p32 s40 layout x=%d y=%d w=%d h=%d (contentW=%.1f)",
                        (int)fx, (int)fy, checkboxWidth, (int)fh, rawContentW);

    // ---- stage=50: content = new(CONTENT_ALLOC) + 构造 + 立即五参 layout ----
    // v37：照抄原版 @0x6D4580——构造后立即五参调 vtable+VT_LAYOUT 给后续
    // addWidget 提供正确基准（误用单参调用会让子控件坐标按垃圾 rect 计算）。
    lzt_core::set_stage(50);
    uintptr_t content = reinterpret_cast<uintptr_t>(
        operator new((size_t)SETTINGS_CONTENT_ALLOC));
    if (!content) {
        lzt_core::log_write("[SFW] p32 stage=50 fail: content allocation failed");
        return false;
    }
    pContentCreate(content);
    lzt_core::set_stage(60);
    uintptr_t contentVtable = *reinterpret_cast<uintptr_t*>(content);
    uintptr_t layoutFunc = *reinterpret_cast<uintptr_t*>(contentVtable + SETTINGS_VT_LAYOUT);
    using Layout5_t = void (*)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t);
    reinterpret_cast<Layout5_t>(layoutFunc)(content, (uint32_t)(int)fx, (uint32_t)(int)fy,
                                            (uint32_t)checkboxWidth, (uint32_t)(int)fh);

    // ---- stage=85: 控件清单物化 ----
    lzt_core::set_stage(85);
    int checkboxW = checkboxWidth - scale_int(uiContext, 8);
    float zeroScale = (float)scale_int(uiContext, 0);
    lzt_core::log_write("[SFW] p32 s85 items=%zu cbw=%d", count, checkboxW);

    for (size_t i = 0; i < count; ++i) {
        const WidgetPlanItem& it = items[i];
        switch (it.type) {
        case WidgetPlanItem::PROMPT:
            materialize_prompt32(content, uiContext, it, checkboxWidth);
            break;
        case WidgetPlanItem::CHECKBOX: {
            GameString label = {};
            pStringCreate(reinterpret_cast<uintptr_t>(&label),
                          reinterpret_cast<uintptr_t>(it.key),
                          (uint32_t)wcslen(it.key));
            uintptr_t cb = pCheckboxCreate(page, it.id,
                                           reinterpret_cast<uintptr_t>(&label),
                                           it.initialState ? 1 : 0, checkboxW);
            pAddWidget(content, cb, 0, zeroScale);
            free_game_string(label);
            // ARM32 的 lzt_localizer 为占位实现，此处不做补捕获（仅 ARM64 有效）。
            // v35 诊断（Debug 版）：checkbox 内部字段——sub_6D3830 写
            // +CHECKBOX_WIDTH=宽度、+CHECKBOX_STATE=选中标志；
            // vtable+212 为 setPos 槽。排查 checkbox 不可见时比对。
            LZT_DBG_LOG("[SFW] p32 checkbox detail: id=%u cb=%p w%u=%d f%u=%d",
                        it.id, (void*)cb,
                        (unsigned)CHECKBOX_WIDTH,
                        cb ? *reinterpret_cast<int*>(cb + CHECKBOX_WIDTH) : -1,
                        (unsigned)CHECKBOX_STATE,
                        cb ? *reinterpret_cast<int*>(cb + CHECKBOX_STATE) : -1);
            break;
        }
        case WidgetPlanItem::RAW:
            pAddWidget(content, it.rawWidget, 0, zeroScale);
            break;
        default:
            break;
        }
    }

    // ---- stage=90: 第二次五参 layout（照抄原版 @0x6D4920-40）----
    // 三次 addWidget 完成后再次调用同一五参 layout 刷新子控件布局。
    lzt_core::set_stage(90);
    contentVtable = *reinterpret_cast<uintptr_t*>(content);
    layoutFunc = *reinterpret_cast<uintptr_t*>(contentVtable + SETTINGS_VT_LAYOUT);
    reinterpret_cast<Layout5_t>(layoutFunc)(content, (uint32_t)(int)fx, (uint32_t)(int)fy,
                                            (uint32_t)checkboxWidth, (uint32_t)(int)fh);
    lzt_core::log_write("[SFW] p32 s90 relayout ok");

    // ---- stage=100: MountContent（v37：mount 前从 page 重取 controller）----
    lzt_core::set_stage(100);
    controller = *reinterpret_cast<uintptr_t*>(
        *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_OWNER) +
        SETTINGS_OWNER_CONTROLLER);
    auto mountContent = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(
        lzt_core::base() + OFF_MountContent);
    lzt_core::log_write("[SFW] p32 s100 mount ctrl=%p content=%p",
                        (void*)controller, (void*)content);
    mountContent(controller, content);
    // v37：无页面级 layout 收尾——原版 sub_6D4420 在 mount 后直接返回

    lzt_core::set_stage(150);
    lzt_core::log_write("[SFW] p32 stage=150 success page=%p content=%p controller=%p",
                        (void*)page, (void*)content, (void*)controller);
    return true;
}
#endif // __arm__

bool materialize_page(uintptr_t page, const wchar_t* titleKey,
                      const WidgetPlanItem* items, size_t count) {
#ifdef __aarch64__
    return materialize_page64(page, titleKey, items, count);
#else
    return materialize_page32(page, titleKey, items, count);
#endif
}

} // namespace widgets
} // namespace lzt_settings
