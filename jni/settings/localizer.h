#ifndef LZT_LOCALIZER_H
#define LZT_LOCALIZER_H

// ============================================================
// lzt_localizer — 已加载本地化控件统一管理层（"同一接口实时切语言"）
//
// 定位：settings_framework 之上的一个类型化本地化服务层。把"哪些已加
// 载控件持有本地化文本、用哪个回填器刷新"收敛为一张类型化注册表 +
// 统一切换入口，设置页 / 主界面共用同一套语义。
//
// === 语义 ===
//   1. capture()   —— 控件【创建时】登记：(类型, 宿主对象, 本地化键,
//                     创建时文本快照)。捕获点由 framework 的既有 hook
//                     （行初始化器 / 标签构造器）提供，本层不收游戏
//                     wstring 布局细节，只收已解析的宽字符串键与快照。
//   2. refresh()   —— 语言切换后：遍历注册表，按类型分发到类型化回填
//                     器，回填器内部调统一读取口 sub_14F3354(key) 取当前
//                     语言文本写回宿主，实现"同接口下实时切换不同语言"。
//
// === 为什么稳（不引入新问题） ===
//   - 不 hook 高频统一读取口（sub_14F3354 只作回填取值函数被调用，
//     不拦截不改写）→ 无 ABI / 性能新风险。
//   - 捕获点复用框架已验证的创建入口 hook，指针有效性靠 vtable 校验
//     + 存活压缩（沿用框架既有权属与生命周期规则）。
//   - 快照比对：宿主当前文本 ≠ 捕获快照 = 游戏已接管为动态文本，交还
//     游戏，不误改。
//
// === 架构 = ARM64（本次验证目标）；ARM32 占位 ===
//   实现位于 settings/settings_framework.cpp（同 translation unit 可复用
//   其静态工具与 offsets）。ARM32 各函数为占位返回（-1 / false）。
// ============================================================

#include <cstdint>

namespace lzt_localizer {

// 文本宿主的类型 —— 决定用哪个回填器 + 哪个回填字段（字段偏移集中在
// offsets.h）。新屏幕适配 = 新增枚举值 + 对应回填器 + offsets 常量，
// 不需要改 refresh 调度逻辑。
enum class HostType : uint8_t {
    TabRow   = 0,   // 设置行（Tab 行 / 滑条行 / checkbox 行），标题串在 host/+184
    Label    = 1,   // 0x2D8 标签（主界面 / 数据驱动文案），SetText(vt+752)
    RootTitle = 2,  // 设置根视图顶部大标题（无固定捕获点，refresh 时由
                    //   framework 提供"最后活动设置页"特判处理）
};

// 登记一个已加载的本地化控件。framework 的捕获 hook 在控件创建时调用。
//   type      —— 宿主类型（决定回填器）
//   host      —— 宿主控件对象指针
//   key       —— 本地化键（'[' 开头，如 "[MAINMENU_PLAY]"；需保持存活直到
//                注册表被消费，内部会拷贝）
//   hasSnapshot/ snapshot —— 捕获时已解析文本（用于 Label 的动态文本身份
//                比对；TabRow 可传 hasSnapshot=false）。snapshot 为拷贝。
// 返回 false：键非法、host 空、注册表超上限等（调用方自行跳过，不致命）。
bool capture(HostType type, uintptr_t host, const wchar_t* key,
             bool hasSnapshot = false, const wchar_t* snapshot = nullptr);

// 统一切换入口：遍历注册表，按类型回填当前语言文本。
// 返回刷新成功数；-1 = 架构不支持（ARM32 占位）。
int refresh_all();

} // namespace lzt_localizer

#endif // LZT_LOCALIZER_H