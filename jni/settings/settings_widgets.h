#ifndef SETTINGS_WIDGETS_H
#define SETTINGS_WIDGETS_H

// ============================================================
// settings_widgets.h — 控件物化原语层（框架内部头）
//
// 职责：承载与游戏交互的全部细节（函数指针、结构偏移、构建时序），
// 对上层（settings_framework.cpp 引擎）暴露架构无关的编排接口。
//
// === 双架构收敛策略（实施期确认） ===
// ARM64 与 ARM32 的设置页构建不是"同一算法+不同常数"，而是
// "同一意图+不同实现序列"（ARM32 存在二次 layout、MountContent
// 单函数挂载、call_s0_float 取宽、不同 x 偏移公式；ARM64 为手写
// release/attach 序列）。因此页面物化按架构各自内聚实现，
// 字符串工具与 Tab 插入序列真正共享。行为保真是第一优先级。
//
// 数值偏移一律取自 offsets.h 架构段常量；禁止字面量。
// ============================================================

#include <cstdint>
#include <stddef.h>

namespace lzt_settings {
namespace widgets {

// ---- 单条页面控件计划（引擎经 PageBuilder 收集，由本层物化）----
struct WidgetPlanItem {
    enum Type : uint8_t {
        PROMPT = 0,     // 说明文本（key 必填；widthDp<0 表示自动宽度）
        CHECKBOX = 1,   // 复选框（key/id/initialState 必填）
        RAW = 2,        // 自建原生 widget（rawWidget/rawWidthPx 必填）
    };
    uint8_t type;
    const wchar_t* key;         // PROMPT/CHECKBOX：文本本地化键
    bool initialState;          // CHECKBOX：初始选中态
    uint32_t id;                // CHECKBOX：定案后的运行时控件 id
    float widthDp;              // PROMPT：自定义宽度（<0 自动）
    float paddingDp;            // PROMPT：高度余量（<0 默认）
    uintptr_t rawWidget;        // RAW：widget 指针
    int rawWidthPx;             // RAW：像素宽度
};

// 解析本层所需的全部游戏函数指针（engine_install 阶段调用一次）。
// libPVZ2.so 基址未就绪或缺任何一项时返回 false 并记日志 [SFW]。
bool resolve_helpers();

// ---- 布局查询（双架构安全；供 PageBuilder 查询接口使用）----
// ARM32 的内容宽查询返回值在 S0 寄存器，必须经专用路径读取，
// 上层不得自行以 float(*)() 方式调用游戏函数。
int   query_content_width_px();
float query_ui_scale();

// 页面级 layout 收尾（原版 sub_A5084C/sub_6D61EC）。
// dispatch 拦截分支构建页面后必须调用，否则 mount 后无刷新。
bool run_page_layout(uintptr_t page);

// ---- 游戏 key 字符串构造（config 服务专用）----
// 构造游戏内部窄字节 std::string 对象（key 为 ASCII；长度 >15 时
// 内部走堆分配）。out 缓冲 ≥24 字节且 16 字节对齐；用毕须配对
// release_key_string。
void make_key_string(void* out, const char* key);
void release_key_string(void* out);

// ---- 页面物化：整段构建一个模块的设置页 ----
// titleKey 为页面顶部标题本地化键；items 为控件清单。
// 完成：controller 链解析 → 标题写入 → content 分配/布局 → 逐条物化
// items → 旧 content 换新 content 挂载 → 架构专属收尾。
// 任一关键步骤失败即中止并记日志（不抛异常、不崩进程）。
bool materialize_page(uintptr_t page, const wchar_t* titleKey,
                      const WidgetPlanItem* items, size_t count);

// ---- Tab 注入原语：在设置页容器上创建并挂载一个自定义 Tab ----
// 复刻原版 createTab 序列：标题串构造 → 图标加载 ×2 → createTabFn → attach。
// createTabFn 由调用方（引擎 hook）传入实际应调用的创建函数（通常为其
// trampoline 保存的原函数指针），本层不持有 hook 状态。
// 内部带重入门，防递归拦截。失败返回 false（调用方应回退放行原调用）。
bool insert_custom_tab(uintptr_t page, uint32_t tabId, const wchar_t* titleKey,
                       uintptr_t iconNormalOffset, uintptr_t iconSelectedOffset,
                       uintptr_t createTabFn);

} // namespace widgets
} // namespace lzt_settings

#endif // SETTINGS_WIDGETS_H
