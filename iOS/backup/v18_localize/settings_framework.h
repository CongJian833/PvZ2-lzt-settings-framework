#ifndef SETTINGS_FRAMEWORK_H
#define SETTINGS_FRAMEWORK_H

// ============================================================
// lzt_settings — PvZ2 设置界面扩展框架（对外唯一入口）
//
// 让一个 inline hook 模块以"声明 + 回调"的方式向游戏设置界面
// 注入自己的设置 Tab，无需接触任何游戏内部地址、结构偏移或
// hook 细节。
//
// === 使用流程 ===
//   1. 实现 ModuleDesc.onBuildPage：在 PageBuilder 上按视觉顺序
//      调用 add_* 声明页面内容（顺序即布局）；
//   2. 调用 register_module(desc) 注册模块（纯入表，不触碰游戏）；
//   3. 宿主在 libPVZ2.so 就绪后调用一次 engine_install()，
//      引擎完成 ID 定案并安装全部 hook。
//
// === 职责边界（重要） ===
//   框架只做机械且通用的部分：hook 安装、ID 分配与冲突校验、
//   事件路由、页面骨架构建、控件物化、use-after-free 防护。
//   业务逻辑（状态含义、文案切换、持久化策略、游戏内联动）
//   全部归模块自己的回调所有——回调内可以写任意普通 C++ 代码。
//
// === 线程模型 ===
//   register_module：仅在 engine_install 之前调用（宿主初始化线程）。
//   onBuildPage / onClick 回调：均在游戏 UI 线程触发；点击回调返回后
//   由引擎统一置 dirty 标记异步重建页面，禁止在回调内同步重建
//   （框架已封死该错误路径，模块无需关心）。
// ============================================================

#include <cstddef>
#include <cstdint>

namespace lzt_settings {

namespace detail { struct EngineAccess; }   // 引擎内部访问器（仅前向声明）

// 控件句柄：框架分配的运行时控件 id。
// tab id 与控件 id 共用游戏的分发命名空间，由引擎保证互不冲突。
struct WidgetHandle {
    uint32_t id;
};

// 复选框点击回调。newState = 点击后该复选框的选中态。
// 回调内只应做业务处理（保存状态/联动逻辑）；页面刷新由引擎的
// dirty 异步重建机制自动完成，重建时会重新执行 onBuildPage，
// 因此 prompt 文案等随状态变化的内容会自然更新。
using CheckboxClickFn = void (*)(bool newState);

// v7.37：带序号的复选框回调（index = 本模块本次构建中的复选框顺序，0 起）。
// 面向"动态 N 项列表"——模块无需为每项手写静态 thunk，由框架统一按序号分发。
using CheckboxIndexClickFn = void (*)(int index, bool newState);

// 复选框描述：文本键、初始选中态、独立处理回调一体声明。
struct CheckboxDesc {
    const wchar_t* labelKey;     // 文本本地化键（如 L"[VIEW_HIGH]"），完全自定义
    bool initialState;           // 本次构建时的选中态（通常从配置读取）
    CheckboxClickFn onClick = nullptr;              // 无序号回调（与下行二选一）
    CheckboxIndexClickFn onClickIndexed = nullptr;  // v7.37：带序号回调（优先）
    uint32_t requestedId = 0;    // 0=由引擎自动分配；非0=显式指定（与其他
                                 //    id 冲突时 engine_install 失败并记日志）
};

// ---- 页面构建器：onBuildPage 回调的参数 ----
// 调用 add_* 的顺序即控件在页面上的排列顺序；add_* 只登记构建
// 意图，不直接操作游戏内存——onBuildPage 返回后由引擎按清单
// 统一物化并处理全部资源细节。
class PageBuilder {
public:
    struct State;                        // 构建清单（引擎内部定义，对外不透明）
    // 添加说明文本（位于后续控件上方）。宽度自动取
    // "checkbox 宽度 − 20·uiScale"，与原版 DataSharing 页面观感一致。
    bool add_prompt(const wchar_t* promptKey);

    // 自定义尺寸版本：widthDp 为期望宽度（dp），paddingDp 为高度余量。
    bool add_prompt(const wchar_t* promptKey, float widthDp, float paddingDp);

    // 添加复选框（两种形态等价）。返回控件句柄（id 可用于日志比对）。
    WidgetHandle add_checkbox(const CheckboxDesc& desc);
    WidgetHandle add_checkbox(const wchar_t* labelKey,
                              bool initialState, CheckboxClickFn onClick);

    // 高级逃生口：挂载调用方自建的原生 widget（完全控制布局的场景）。
    // widget 生命周期移交引擎随 content 管理；失败返回 false。
    bool add_raw_widget(uintptr_t widget, int widthPx);

    // 布局查询：当前内容区像素宽度 / UI 缩放系数（供自定义计算）。
    int   content_width_px() const;
    float ui_scale() const;

private:
    friend struct detail::EngineAccess;  // 仅引擎可构造/读取清单
    explicit PageBuilder(State& s);
    PageBuilder(const PageBuilder&) = delete;
    PageBuilder& operator=(const PageBuilder&) = delete;
    State& m_state;
};

// 页面内容构建回调：每次页面创建/dirty 重建都会重新执行，
// 因此在其中读取最新状态即可实现内容随状态变化。
using BuildPageFn = void (*)(PageBuilder& builder);

// ---- 模块描述符：一个设置 Tab 的完整声明 ----
struct ModuleDesc {
    const wchar_t* titleKey;         // Tab 标题本地化键（如 L"[VIEW_ANGLE]"）
    uintptr_t iconNormalOffset;      // Tab 图标资源地址偏移（正常态，
                                     //   引用 offsets.h 常量，如 OFF_SettingsBuildVersionIconNormal）
    uintptr_t iconSelectedOffset;    // Tab 图标资源地址偏移（选中态）
    uint32_t  anchorTabId = 6;       // 锚点原版 Tab id：新 Tab 插入到它之后
                                     //   （6 = Build Version）
    uint32_t  slotAfterAnchor = 1;   // 锚点后第几个位置：1=紧随其后，
                                     //   2=第二位……同锚点多模块按此排序注入
    uint32_t  requestedTabId = 0;    // 0=由引擎从空闲区间自动分配；
                                     //   非0=显式指定（冲突时安装失败并记日志）
    BuildPageFn onBuildPage;         // 页面内容构建回调（必填）
    bool (*shouldCreate)() = nullptr; // v7.34：可选，为空=总是创建；非空且返回 false 时不建 Tab
};

// 注册模块。仅写入进程内注册表，不触碰游戏内存；
// 必须在 engine_install() 之前完成（engine_install 之后注册无效）。
// 返回 false：描述符非法（缺回调/缺标题键）或注册表已满。
bool register_module(const ModuleDesc& desc);

// 安装 Settings hook 三件套（createTab/dispatch/createPage）并定案
// 全部 ID 与注入计划。幂等：重复调用直接返回 true。
// 无任何模块注册时不安装任何 hook（纯相机场景零开销）。
// 返回 false：函数指针解析失败或存在 id 冲突（详见日志 [SFW]）。
bool engine_install();

// v7.4：刷新全部已注册 Tab 的标题（语言切换后调用）。hook_create_tab
// 捕获每个经 createTab 创建的 Tab（原版+注入）的指针与标题键，此处
// 逐个重新本地化并改写 Tab+184 标题串（写入序列与 controller 标题同
// 构，已真机验证）。仅 ARM64 实现；vtable 校验失败的条目被清除。
// 返回刷新成功的 Tab 数（-1 = 架构不支持）。
// v7.4 起 Tab 行 / 0x2D8 标签刷新已收敛为 Localizer：见 refresh_all_localized。
void      refresh_page_title();               // 设置页顶部标题原位刷新(v7.12)
// v7.19/v7.20：统一本地化刷新入口（Localizer 门面）。刷新已由 Localizer
// 类型化层接管（TabRow/Label）＋ refresh_page_title（RootTitle）。设置页 /
// 主界面已加载文本在同一接口下按宿主类型回填当前语言。返回刷新控件数合计。
int       refresh_all_localized();
void      request_base_rebuild();            // 请求延迟整页重建(下一帧安全点执行,v7.10 诊断)

// iOS：页面显示函数被调用时的 dirty 处理入口。
// 若当前有激活模块且该页被置脏（重建请求），则清脏并重建页面，返回 true
// （调用方应跳过原版处理）；否则返回 false（调用方按原流程走）。
// 之所以需要它：iOS 的 dirty 重建由游戏的页面显示函数（0x75A0E4）驱动，
// 而该函数的原版分支只会重建原版 DataSharing 页，不会用我们的映射。
// iOS：安排一次「当前事件处理结束后」的页面重建（由 iOS 侧用主队列实现）。
// 必须在 dispatch 的调用栈之外执行，否则会释放正在使用的控件（UAF）。
void ios_schedule_rebuild(uintptr_t page);

bool ios_try_rebuild_dirty(uintptr_t page);

// iOS：主动触发页面显示函数（其内部检查 dirty 并重建），供拦截了事件处理的
// 代码路径补上原版这一步。非 iOS 平台为空实现。
void ios_run_page_show(uintptr_t page);

// iOS：调用被 hook 的原版页面构建函数（trampoline）。
// 供 iOS 的物化实现复用游戏原版页面构建流程（见 widgets::materialize_page）。
// 非 iOS 平台返回 0。
uintptr_t call_original_page(uintptr_t page);

// ---- v7.37：通用注册点（框架去业务化）----
// 语言切换刷新器：refresh_all_localized() 按注册顺序调用，刷新器自行校验宿主
// 有效性。框架不感知任何具体屏幕语义。返回 false = 参数为空或注册表已满。
bool register_refresher(void (*fn)());
// iOS：当前设置页（若存在）—— 供业务刷新后把设置页重新提到最前（避免被主界面覆盖）。
uintptr_t last_settings_page();
// iOS：离开设置页时执行挂起的业务屏幕刷新（实现见 iOS/src/lzt_main_ios.cpp）
extern "C" void lzt_ios_flush_pending_screen_refresh(void);
// 按注册顺序遍历执行全部屏幕刷新器（供切换语言收尾链在换表后调用）。
void run_screen_refreshers();
// 引擎就绪回调：engine_install() 成功且 libPVZ2 基址可用后调用一次，供模块安装
// 自身 hook（回调须自幂等——引擎重装时会再次调用）。
bool register_engine_ready(void (*fn)(uintptr_t base));

// ---- v14：主界面已进入 ----
// 用于把「游戏启动完毕」这一时机暴露给模块层。
//
// 背景：语言表的数据包来源（package-rton）会调用游戏资源系统内部函数，
// 在启动期（游戏自己的资源加载批次进行中）重入会导致随机停在 EA 界面。
// 因此需要一个可靠的「启动已完成」信号来解除该限制。
//
// 信号源：主界面 PersistentMessage 横幅重建（sub_1301B08）。玩家必须先看到
// 主界面才会点设置按钮，故此刻资源系统必然就绪，且早于任何设置页构建。
// 语义中性：框架不感知具体屏幕，仅转达"此时机已到"。
// 回调须自幂等（可能被多次调用）。
bool register_main_screen_entered(void (*fn)());
// 由平台层（iOS screen_bindings 的横幅 hook）调用：通知所有注册者。
void notify_main_screen_entered();



// ---- v7.6.3：基础视图重建（治本,游戏原生机制）----
// sub_A4C490 是设置基础视图构建器（Music/SoundFX、通知 checkbox、全部
// Tab 行、页标题标签、内容挂载,自带旧内容释放）,游戏在打开设置/从子页
// 返回等流程反复调用。语言切换完成后对当前设置页直接重跑它 = 全部基础
// 文本按新容器重建;随后用 dispatch_module_tab 回语言 Tab,用户无感。
uint32_t  primary_module_tab_id();        // 首个注册模块（语言）的 tabId
bool      dispatch_module_tab(uintptr_t page, uint32_t tabId);  // 经 hook 派发进入模块 Tab
void      clear_page_dirty(uintptr_t page);  // 清页 dirty 标志（防旧页重建竞态）

// ---- 可选工具：bool 配置读写 ----
// 封装游戏偏好后端的 bool 存取样板（构造 key 字符串对象 → 调用
// PersistReadBool/PersistSaveBool）。纯粹的工具函数——模块完全可以
// 不使用它们而自行实现存储。storage_field_offset 是宿主 DisplayInfo
// 对象内一块空闲字节（如 CONFIG_USE_HIGH_VIEW_ANGLE），用于缓存
// 最近一次读到的值，避免每次都查询后端。
namespace config {
    // 读配置：优先命中 storage_field_offset 缓存字节；未初始化时查询
    // 后端一次。配置对象尚未就绪时返回 default_value 且不锁定缓存。
    bool read_bool(const char* key, bool default_value,
                   uintptr_t storage_field_offset);

    // 写配置：更新缓存字节并持久化到偏好后端。
    void write_bool(const char* key, bool value,
                    uintptr_t storage_field_offset);

    // ---- 零字段占用变体（新模块推荐）----
    // 直查/直写偏好后端，不占用 DisplayInfo 的任何字段位——无需为新
    // 模块寻找"空闲字节"。读取结果在栈上接收；频率为页面构建级（低频），
    // 直接查询后端无性能顾虑。需要高频轮询时再考虑带字节的 read_bool。
    bool query_bool(const char* key, bool default_value);
    void save_bool(const char* key, bool value);

    // ---- v7.37：单选组（一组互斥 bool 配置：唯一为 true 者为选中项）----
    // 返回首个为 true 的下标；无命中返回 -1。
    int  query_radio(const char* const* keys, size_t count);
    // 单选写入：原选中项置 false、keys[index] 置 true（index<0 表示只清旧项）。
    // 内部先查旧项，因此只写"变动的两项"——与逐项 save_bool 的既有行为一致。
    void save_radio(const char* const* keys, size_t count, int index);
}

} // namespace lzt_settings

#endif // SETTINGS_FRAMEWORK_H
