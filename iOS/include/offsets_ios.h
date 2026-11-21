#ifndef OFFSETS_IOS_H
#define OFFSETS_IOS_H

// ============================================================
// offsets_ios.h — iOS（PvZ2 国际版 9.8.1 / arm64）地址与偏移总表
//
// 与 Android 侧 jni/offsets.h 同构、同命名，便于框架层与业务模块共用源码。
// 全部地址为【相对主程序 __TEXT 基址】的偏移；运行时地址 = 主程序 slide +
// 0x100000000 + 本表偏移（iOS 主程序为 PvZ2.app/PvZ2，非 dylib）。
//
// 来源：
//   · 视角相关：静态定位（交叉引用 + 字段访问模式比对），见 iOS/FINDINGS.md
//   · 设置页相关：从原版 Tab 注册链倒推，见 iOS/FINDINGS.md
//   · 所有地址均已核对为合法函数序言
//
//  详细的定位方法与填写规范见 docs/offsets-guide.md。
//
// 注意：iOS 的字符串资源是 UTF-32LE 编码（每个字符占 4 字节），
//       与 Android 的 UTF-16/UTF-8 不同，直接 grep ASCII 是找不到键的。
// ============================================================

#include <cstdint>

// 平台标识：共享源码据此区分平台分支（Android 侧未定义该宏，
// 因此 `#if !LZT_PLATFORM_IOS` 在 Android 上恒为真，行为不变）。
#define LZT_PLATFORM_IOS 1

// ============================================================
// 尚未在 iOS 上定位的项（先留 0 并在使用处平台分支屏蔽，避免误用）
// ============================================================
constexpr uintptr_t OFF_LocalizeKey             = 0;  // TODO: iOS 统一本地化读取口
constexpr uintptr_t OFF_SETTINGS_ROW_INIT       = 0;  // TODO: iOS 设置行初始化器
constexpr uintptr_t OFF_SETTINGS_LABEL_CREATE   = 0;  // TODO: iOS 0x2D8 标签构造器
constexpr uintptr_t OFF_SETTINGS_TAB_VTABLE     = 0;  // TODO: iOS 设置行 vtable
constexpr uintptr_t OFF_SETTINGS_TAB_LABEL_VTABLE = 0; // TODO: iOS 标签 vtable

// ============================================================
// 一、设置页（Settings）—— Tab ID 表
// 与 Android 的 id 体系一致：6 = Download Preferences（安卓为 Build Version）
// ============================================================
constexpr uint32_t SETTINGS_BUILDVERSION_ID   = 6;    // 本机锚点：Download Preferences
constexpr uint32_t SETTINGS_ANCHOR_NEXT_TAB_ID = 7;   // 锚点后首个原版 Tab（Settings Help）
constexpr uint32_t SETTINGS_FIRST_DYNAMIC_ID  = 30;   // 框架动态 id 分配起点

// 设置页原版 Tab ID → 标题键（供锚点选择与冲突校验）
constexpr uint32_t SETTINGS_TAB_NEWS              = 0x3;
constexpr uint32_t SETTINGS_TAB_DOWNLOADPREFS     = 0x6;
constexpr uint32_t SETTINGS_TAB_HELP              = 0x7;
constexpr uint32_t SETTINGS_TAB_EULA              = 0x8;
constexpr uint32_t SETTINGS_TAB_PRIVACY           = 0x9;
constexpr uint32_t SETTINGS_TAB_TERMS             = 0xA;
constexpr uint32_t SETTINGS_TAB_DATASHARING       = 0xC;
constexpr uint32_t SETTINGS_TAB_PLAYERTARGETING   = 0xF;
constexpr uint32_t SETTINGS_TAB_PLAYERTARGETING_EA= 0x12;
constexpr uint32_t SETTINGS_TAB_ACCOUNT_DELETION  = 0x15;
constexpr uint32_t SETTINGS_TAB_GAMECREDITS       = 0x18;
constexpr uint32_t SETTINGS_TAB_RESTORE_PURCHASES = 0x19;
constexpr uint32_t SETTINGS_TAB_RESET_AGE         = 0x1A;

// ============================================================
// 二、设置页函数地址
// ============================================================
constexpr uintptr_t OFF_SettingsInit      = 0x756ae4;  // 设置页初始化（全部 Tab 在此创建）
constexpr uintptr_t OFF_SettingsCreate    = 0x757c18;  // Tab 创建（5 参数）—— createTab hook 点
constexpr uintptr_t OFF_SettingsDispatch  = 0x759dd4;  // Tab 点击 / 事件分发 —— dispatch hook 点
constexpr uintptr_t OFF_SettingsDataSharing = 0x758a94; // 子页面创建（页面骨架）—— page hook 点
constexpr uintptr_t OFF_SettingsCheckboxClick = 0x75adb4; // Checkbox 点击处理
constexpr uintptr_t OFF_SettingsDirtyRefresh  = 0x758930; // dirty 标志检查与重建

// 设置页依赖的辅助函数
constexpr uintptr_t OFF_SettingsWstringCtor   = 0x757b78;  // wstring 构造
constexpr uintptr_t OFF_SettingsCheckboxCreate = 0x758740; // 创建一行 checkbox
constexpr uintptr_t OFF_SettingsRowAttach    = 0x757e58;  // 把行挂到内容容器
constexpr uintptr_t OFF_SettingsIconLoad      = 0x46994;   // 图标资源加载
constexpr uintptr_t OFF_SettingsStdStringCtor = 0x1bfb4c;  // std::string 构造
constexpr uintptr_t OFF_SettingsPersistSave   = 0x1ab9f44; // 持久化保存
constexpr uintptr_t OFF_SettingsGetterUsageSharing = 0x1cdaec; // 读取 cfg+0xA83
constexpr uintptr_t OFF_SettingsSetterUsageSharing = 0x1cda44; // 写入 cfg+0xA83

// ============================================================
// 三、设置页结构偏移
// ============================================================
constexpr uintptr_t OFF_SettingsPromptMake = 0x179c14;  // 说明文案(prompt)创建：x0=文案键, x8=输出对象
constexpr uintptr_t OFF_SettingsPageShow = 0x75a0e4;  // 页面显示/刷新（原版 dispatch case 尾部的 tail call 目标）

// 内容挂载/更新通知：原版构建完页面后调用它（0x758268）。
// 实现：*(controller+208) = content，再调 controller 的 vtable+88（重排/重算尺寸）。
// 用途：iOS 的物化是「复用原版构建 + 追加控件」，追加发生在原版布局之后，
//       因此必须再调一次本函数让游戏重算内容尺寸——否则滚动容器仍按原版项数
//       计算，表现为「内容溢出但不滚动、无滑条」。
constexpr uintptr_t OFF_SettingsSetContent = 0x758268;
constexpr uintptr_t SETTINGS_CONTROLLER_CONTENT_OFF = 208;   // controller+208：当前内容对象
constexpr uintptr_t SETTINGS_PAGE_DIRTY = 288;   // page+0x120（0x75A0E4 开头 `ldr w8,[x0,#288]`）
                                                 // 注意：与 Android 的 292 不同，勿照搬。

// 设置页资源（Tab 图标，可复用给自定义 Tab）
constexpr uintptr_t OFF_SettingsDatasharingIconNormal   = 0x2813100;
constexpr uintptr_t OFF_SettingsDatasharingIconSelected = 0x2813128;
constexpr uintptr_t OFF_SettingsBuildVersionIconNormal   = 0x2813100;  // 沿用 DataSharing 图标
constexpr uintptr_t OFF_SettingsBuildVersionIconSelected = 0x2813128;

// ============================================================
// 四、设置页 UTF-32LE 字符串地址（相对 __TEXT）
// 说明：这些是「键」字符串，本地化后才是显示文本。
// ============================================================
constexpr uintptr_t STR_SETTINGS_DATASHARING_TITLE          = 0x1f04868;
constexpr uintptr_t STR_SETTINGS_DATASHARING_PROMPT         = 0x1f04e6c;
constexpr uintptr_t STR_SETTINGS_DATASHARING_PROMPT_DISABLED= 0x1f04dd0;
constexpr uintptr_t STR_SETTINGS_DATASHARING_ENABLE         = 0x1f04ee4;
constexpr uintptr_t STR_SETTINGS_DATASHARING_DISABLE        = 0x1f04f5c;
constexpr uintptr_t STR_SETTINGS_DOWNLOADPREFS_TITLE        = 0x1f0467c;

// ============================================================
// 五、全局对象与配置字段
// ============================================================
// 用户配置对象指针（qword_1027EB870）。解引用后即配置对象：
//   cfg = *(uintptr_t*)(base + OFF_G_Config)
// 该对象同时被 BoardZoom2 引用（用于计算屏幕比例相关的缩放下限），
// 因此它既是「用户配置」也是「显示信息」宿主——两处独立定位互相印证。
constexpr uintptr_t OFF_G_Config = 0x27eb870;
constexpr uintptr_t OFF_G_DisplayInfo = 0x27eb870;  // 同上（沿用 Android 的命名）

// 视角开关的存放位置：按参考文档，cfg+0xA86 是 iOS 上的空闲字节
// （Android 侧用的是 DisplayInfo 内 offset 2582 的空闲 padding）。
constexpr uintptr_t CONFIG_USE_HIGH_VIEW_ANGLE = 0xA86;

// 配置字段（cfg 内偏移，BYTE）
constexpr uintptr_t CONFIG_HAS_DISABLED_USAGE_SHARING = 0xA83;  // 原 DataSharing 状态

constexpr uintptr_t OFF_PersistManager  = 0x27f4ed0;  // 持久化管理器指针全局（qword_1027F4ED0）
constexpr uintptr_t OFF_PersistSave     = 0x1ab9f44;  // 保存 bool 配置（与 OFF_SettingsPersistSave 同址）
constexpr uintptr_t OFF_PersistReadBool = 0x1aba2dc;  // 读取 bool 配置（sub_101ABA2DC，参考文档 3.5 节）

// ---- 数据包（RSB）读取通道：iOS 专用偏移 ----
// 说明：OFF_PAK_MANAGER 与 OFF_PersistManager 同址（安卓亦如此，两处指同一全局槽）。
// 结构体内部偏移（PAKMGR_*/RSM_*）与安卓一致——游戏源码同源，字段布局不随平台变，
// 与 Board 字段偏移同例。
constexpr uintptr_t OFF_PAK_MANAGER        = 0x27f4ed0;  // Pak 管理器指针全局（=PersistManager）
constexpr uintptr_t PAKMGR_RES_STREAMS_MGR = 0x858;      // pakMgr+0x858 → ResStreamsManager
constexpr uintptr_t RSM_RSB_SWITCH         = 24;         // rsm+24：RSB 通道开关（QWORD 指针）
constexpr uintptr_t RSM_MOUNT_COUNT        = 112;        // rsm+112：挂载包数量
constexpr uintptr_t RSM_MOUNT_ARRAY        = 120;        // rsm+120：挂载包数组指针
constexpr uintptr_t RSM_GLOBAL_HASH_TREE   = 48;         // rsm+48：全局哈希树根指针
constexpr uintptr_t HASH_TREE_ENTRY_COUNT  = 8;          // 树对象+8：条目计数

// 【待定位】按 RSB 名字取记录的原语（安卓 sub_16B31EC）。
// 未定位前 iOS 不使用该通道（resource_file/language_module 内已按平台屏蔽），
// 语言表读取先走 CDN 外置通道。
constexpr uintptr_t OFF_PACKAGE_FIND_RECORD = 0;

// ---- iOS 尚未定位的诊断用项（release 不执行，仅占位以保证编译）----
// 说明：以下均只被 LZT_DBG_ONLY 诊断探针引用（正式版编译期不执行），
// 定位后填入真值即可启用调试版取证能力。
constexpr uintptr_t FONTCONFIG_FONT_NAME  = 0;
constexpr uintptr_t FONTCONFIG_CACHED_CTX = 0;
constexpr uintptr_t OFF_RADIX_FIND            = 0;   // 基数树查找（RSB 树扫描诊断）
constexpr uintptr_t GROUP_MOUNT_TYPE          = 24;  // 挂载组+24：type（结构体偏移，同 Android）
constexpr uintptr_t GROUP_NAME_TREE           = 32;  // 挂载组+32：内嵌组内名字树
constexpr uintptr_t GROUP_ENTRY_STRIDE        = 200; // 挂载组数组步长（字节）
constexpr uintptr_t RSM_RSB_MAIN              = 32;  // rsm+32：RSB 主对象指针
constexpr uintptr_t RSB_MAIN_TABLE_OFF        = 56;  // rsb+56：记录表基址偏移
constexpr uintptr_t RSB_MAIN_REC_SIZE         = 60;  // rsb+60：记录步长
constexpr uint32_t  RSB_VARIANT_FLAG          = 0x10000000u;  // node[0]：语言变体复合条目标志
constexpr uintptr_t RSB_VARIANT_COUNT_OFF     = 1152; // rec+1152：复合条目子条目数
constexpr uintptr_t RSB_VARIANT_FIRST_OFF     = 128;  // rec+128：首个子条目
constexpr uintptr_t RSB_VARIANT_STRIDE        = 16;   // 子条目步长（字节）

// ---- 语言管理器（已定位，实测确认）----
// 位置：*(DisplayInfo + 2104)。安卓为 DisplayInfo+1992，iOS 整体后移 112
// （与 Board 字段 2584 vs 2472 的布局差异一致）。
// 依据：反汇编 0x2c896c-0x2c897c（游戏拼 "LawnStrings-%s" 之前读该对象）；
//       运行时只读 dump 确认 +480/+484 为状态枚举、+508 为语言标识。
constexpr uintptr_t DISPLAYINFO_LANGMGR       = 2104;
// 当前语言标识（FourCC，大写紧凑，如 "EN-US"=0x454E5553、"ZH-CN"=0x5A48434E）。
// 实测 lm+508 = 0x454E5553（游戏当前为 en-us）。安卓该字段在 +484。
// 注意：安卓侧记录过「写相邻主字段（+0x1E0）导致崩溃」；iOS 上同样只写本字段，
//       不要触碰 +480/+484（实测为 0x4d555458 状态枚举）。
constexpr uintptr_t LANGMGR_LOCALE_ID         = 508;
constexpr uintptr_t LANGMGR_MAIN_LOCALE_ID    = 480;   // 保留定义以便对照；只读

// 常用语言 FourCC（与 Android offsets.h 同值）："XX-YY" 去掉连字符按大端打包。
constexpr uint32_t LANG_ID_EN_US = 0x454E5553;   // "EN-US"
constexpr uint32_t LANG_ID_ZH_CN = 0x5A48434E;   // "ZH-CN"

// ---- 热重载所需函数/全局（待定位）----
constexpr uintptr_t OFF_PVZDB_SINGLETON         = 0;  // TODO PVZDB 单例指针全局
constexpr uintptr_t OFF_DB_LOAD_RTON            = 0;  // TODO RTON 文件加载器
constexpr uintptr_t OFF_APPLY_LOCALIZED_STRINGS = 0;  // TODO 本地化文本应用
constexpr uintptr_t OFF_CDN_APPLY               = 0;  // TODO CDN Apply 链入口
// ---- 本地化阶段（与语言模块一同定位）：先留 0，使用处按平台屏蔽 ----
constexpr uintptr_t SETTINGS_LABEL_TEXT = 0;            // 标签文本字段
constexpr uintptr_t SETTINGS_PAGE_CTRL_VEC = 0;         // 根标题向量
constexpr uintptr_t SETTINGS_PAGE_CTRL_VEC_END = 0;     // 根标题向量尾
constexpr uintptr_t OFF_SETTINGS_TITLE_CTRL_VTABLE = 0; // 标题 controller vtable
constexpr uintptr_t SETTINGS_TAB_TITLE = 0;             // 行标题字段
constexpr uintptr_t SETTINGS_TAB_TITLE_HEAP = 0;        // 行标题堆指针
constexpr uintptr_t OFF_ReleaseTitle = 0;               // 标题释放函数

// ---- iOS 控件物化的对应关系（已由运行时验证确认的）----
//   checkbox 行创建：0x758740(page, id, keyWStr, checked, prevRow)
//   行挂到容器  ：0x757e58(content, row, 0, widthPx)
//   wstring 构造：0x757b78(out, key)
// 下面把 Android 的命名映射到这些已验证的原语上。
constexpr uintptr_t OFF_SettingsStringCreate = 0x757b78;  // wstring 构造
constexpr uintptr_t OFF_SettingsAttach       = 0x757e58;  // 行/控件挂到内容容器
constexpr uintptr_t OFF_SettingsAddWidget    = 0x757e58;  // 同上（Android 两处分列）
constexpr uintptr_t OFF_CheckboxCreate       = 0x758740;  // checkbox 行创建

// ---- 以下为 iOS 尚未定位的项（阶段二/三），先留 0 并禁止使用 ----
constexpr uintptr_t OFF_SettingsLayout = 0;
constexpr uintptr_t OFF_SettingsContentCreate = 0;
constexpr uintptr_t OFF_SettingsScaleFloat = 0;
constexpr uintptr_t OFF_SettingsContentWidth = 0;
constexpr uintptr_t OFF_SettingsUIScaleContext = 0;
constexpr uintptr_t OFF_SettingsUIScale = 0;
constexpr uintptr_t OFF_FontLoad = 0;
constexpr uintptr_t OFF_FontContext = 0;
constexpr uintptr_t OFF_PromptStyle = 0;
constexpr uintptr_t OFF_TextMeasure = 0;
constexpr uintptr_t OFF_TextContainerCreate = 0;
constexpr uintptr_t OFF_TextLabelCreate = 0;
constexpr uintptr_t SETTINGS_PAGE_CONTAINER = 240;   // page+0xF0 → Tab/内容的容器（与原版一致）
constexpr uintptr_t SETTINGS_CONTAINER_ALLOC = 0;
constexpr uintptr_t SETTINGS_VT_SETPOS = 0;
constexpr uintptr_t SETTINGS_TAB_LABEL_SETTEXT_VT = 0;   // TODO: iOS 标签 SetText 虚函数槽

// ---- 表变更广播（语言热重载用，阶段二）----
constexpr uintptr_t OFF_DELEGATE_MGR_PTR = 0;       // TODO: iOS 委托管理器指针槽
constexpr uintptr_t DELEGATE_MGR_LOCK_DWORD = 0;    // TODO: iOS 重入计数位置
constexpr uintptr_t OFF_DELEGATE_UNLOCK = 0;        // TODO: iOS 解锁/广播函数

// ---- 语言标识（LANG_ID_* 已在上文「语言管理器」处定义）----

// ---- 阶段三（iOS 控件物化）待定位：Android 的这批偏移在 iOS 上语义与数值
//      都不同，iOS 将用自己的一套构建序列（checkbox 行/行挂载/wstring 构造），
//      这些符号先留 0，仅供编译通过；使用前必须先在 iOS 上定位。----
constexpr uintptr_t OFF_SettingsTitleStringCreate = 0;
constexpr uintptr_t OFF_TextContainerAdd = 0;
constexpr uintptr_t SETTINGS_CONTENT_ALLOC = 0;
constexpr uintptr_t SETTINGS_CONTROLLER_TITLE = 0;
constexpr uintptr_t SETTINGS_CONTROLLER_TITLE_HEAP = 0;
constexpr uintptr_t SETTINGS_CONTROLLER_CONTENT = 208;
// 页面 → 拥有者 → controller 链（与原版同构，反汇编实证）：
//   0x758e30: ldr x8,[x19,#216] ; ldr x0,[x8,#8]
//   → owner = *(page+216)，controller = *(owner+8)
constexpr uintptr_t SETTINGS_PAGE_OWNER = 216;      // page+0xD8
constexpr uintptr_t SETTINGS_OWNER_CONTROLLER = 8;  // owner+8
constexpr uintptr_t SETTINGS_VT_LAYOUT = 0;
constexpr uintptr_t WIDGET_OFF_X = 0;
constexpr uintptr_t WIDGET_OFF_Y = 0;
constexpr uintptr_t WIDGET_OFF_W = 0;
constexpr uintptr_t WIDGET_OFF_H = 0;
constexpr uintptr_t SCROLL_CONTENT_W = 0;
constexpr uintptr_t SCROLL_CONTENT_H = 0;
constexpr uintptr_t SCROLL_OFF_W = 0;
constexpr uintptr_t SCROLL_OFF_H = 0;
constexpr uintptr_t SCROLL_STATE = 0;
constexpr uintptr_t SCROLL_VP_W = 0;
constexpr uintptr_t SCROLL_VP_H = 0;

// 另两个在启动早期出现的全局槽（BoardZoom2 内引用，疑似渲染/显示信息宿主）
constexpr uintptr_t OFF_G_DisplayInfoSlotA = 0x27eb870;  // 与 OFF_G_Config 同址
constexpr uintptr_t OFF_G_DisplayInfoSlotB = 0x27f6a28;

// 该对象内与屏幕尺寸相关的字段（实测 3324:1536 ≈ 2.164，与 UIScreen 比例吻合）
constexpr uintptr_t DISPLAYINFO_SCREEN_A = 252;   // +0xFC
constexpr uintptr_t DISPLAYINFO_SCREEN_B = 256;   // +0x100

// ============================================================
// 六、视角模块（相机 / Board）
// ============================================================
constexpr uintptr_t OFF_BoardZoom  = 0x183a58;  // BoardLayout_ApplyZoom
constexpr uintptr_t OFF_BoardZoom2 = 0x183b10;  // BoardZoom2（缩放下限 1.0）

// 街道恐龙生成入口（对应 Android sub_729638）：(ctx, xBase, spawnMode)。
// 两个调用方各自算出 xBase 后尾调用它（反汇编实证）：
//   SpawnStreetDinos 0x5edff0：xBase = board[285] + board[270]，spawnMode = 1
//   PlaceStreetDinos 0x5edd7c：xBase = board[286] + board[270]，spawnMode = 0
// 因为我们改写了 board[270]（左对齐），xBase 会随之偏移，需要在入口处补偿
// 回原版基准（补偿量 = 对齐前的 b270 − 对齐后的 b270）。
constexpr uintptr_t OFF_StreetDinos = 0x5eddb4;

// 取当前 board 指针的函数（0x7f65ac：DisplayInfo + DISPLAYINFO_BOARD 解引用）
constexpr uintptr_t OFF_GetBoard = 0x7f65ac;
constexpr uintptr_t DISPLAYINFO_BOARD = 2584;   // 0xA18（Android 为 2472，iOS 不同）

// 相机渲染 X 偏移（ARM64 记作 b17，即 board+0x44）。
// 该字段被渲染每帧消费，直接写入即可让对齐立即生效，
// 无需等待震屏恢复逻辑刷新（安卓 v27 同源做法，iOS 沿用）。
constexpr uintptr_t BOARD_17 = 17 * 4;   // 0x44

// Board 字段：与 Android 完全一致（int 索引 × 4 = 字节偏移）
constexpr uintptr_t BOARD_270 = 270 * 4;   // 0x438
constexpr uintptr_t BOARD_280 = 280 * 4;   // 0x460  float 缩放
constexpr uintptr_t BOARD_281 = 281 * 4;   // 0x464
constexpr uintptr_t BOARD_283 = 283 * 4;   // 0x46C
constexpr uintptr_t BOARD_284 = 284 * 4;   // 0x470
constexpr uintptr_t BOARD_285 = 285 * 4;   // 0x474
constexpr uintptr_t BOARD_286 = 286 * 4;   // 0x478

// ============================================================
// 七、主程序基址常量
// ============================================================
constexpr uintptr_t IOS_TEXT_VMADDR = 0x100000000ULL;  // 主程序 __TEXT 的 vmaddr
constexpr uintptr_t IOS_TEXT_VMSIZE = 0x217c000ULL;    // 主程序 __TEXT 虚拟大小

#endif // OFFSETS_IOS_H
