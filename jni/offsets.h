#ifndef OFFSETS_H
#define OFFSETS_H

// ============================================================
//  偏移量配置 - 用户必填
//
//  使用说明：
//    1. 用 IDA Pro 打开对应架构的 libPVZ2.so
//    2. 按下方注释定位函数和字段偏移
//    3. 填入对应数值后即可编译
//
//  详细定位方法见 docs/offsets-guide.md（偏移量填写指南）。
//  本文件当前偏移值基于国际版 PvZ2 9.8.1；其他版本需自行配置并核对。
//  实现代码中禁止出现地址字面量，所有地址必须在本文件定义后引用。
//
//  === ARM64 与 ARM32 偏移差异 ===
//  ARM32 的 Board 结构体和 DisplayInfo 结构体与 ARM64 布局不同
//  （指针宽度 4 vs 8 字节导致字段位置整体前移），所有偏移必须
//  在对应架构的 IDB 中独立验证，不能直接套用。
// ============================================================

// ============================================================
//  双架构共用 Settings 常量（与架构无关的版本知识）
//
//  来源：《Android设置界面-Tab注册与分发总表》逆向结论：
//  两架构原版 Tab/控件 id 均只占用 3~29 区间，30+ 空闲。
// ============================================================
constexpr uint32_t SETTINGS_BUILDVERSION_ID    = 6;  // Build Version Tab 的原版 id（默认注入锚点）
constexpr uint32_t SETTINGS_ANCHOR_NEXT_TAB_ID = 7;  // 锚点后首个原版 Tab（Settings Help，注入插入点）
constexpr uint32_t SETTINGS_FIRST_DYNAMIC_ID   = 30; // 框架动态 id 分配起点

// ============================================================
//  ARM64 (arm64-v8a) 偏移
//
//  IDA 验证方法：
//    - BoardLayout_ApplyZoom @ 0xADEB80：函数内访问 Board+0x460(float)/0x46C~0x478(int)
//    - BoardZoom2 @ 0xADEDE0：函数内访问 Board+0x460(float)，长屏检测读 g_DisplayInfo+0x88/0x8C
//    - g_DisplayInfo @ 0x26BFC10：指针全局，解引用得到 DisplayInfo 对象
// ============================================================
#ifdef __aarch64__

// --- 函数地址（在 libPVZ2.so 中的文件偏移）---
constexpr uintptr_t OFF_BoardZoom  = 0xADEB80;  // BoardLayout_ApplyZoom（计算 board[283~286]/270/284）
constexpr uintptr_t OFF_BoardZoom2 = 0xADEDE0;  // BoardZoom2（计算 board[280~282]）
constexpr uintptr_t OFF_ShakeBoard = 0xAF8650;  // ShakeBoard(Board*, xAmt, yAmt, duration) — 创建震动action
constexpr uintptr_t OFF_SettingsCreate = 0xA4D79C;
constexpr uintptr_t OFF_SettingsAttach = 0xA4DA68;
constexpr uintptr_t OFF_SettingsDispatch = 0xA501E0;
constexpr uintptr_t OFF_SettingsDataSharing = 0xA4EA34; // sub_A4EA34：DataSharing 页面创建（dirty flag 重建入口）
constexpr uintptr_t OFF_SettingsLayout = 0xA5084C;
constexpr uint32_t SETTINGS_VIEW_ANGLE_ID = 30;   // 本项目注入的"视角"Tab id（原版未占用）
constexpr uintptr_t OFF_SettingsBuildVersionString = 0x1CBD3F0;
constexpr uintptr_t OFF_SettingsBuildVersionIconNormal = 0x26A2DF0;
constexpr uintptr_t OFF_SettingsBuildVersionIconSelected = 0x26A2E18;
constexpr uintptr_t OFF_SettingsStringCreate = 0x5F1540;
constexpr uintptr_t OFF_SettingsTitleStringCreate = 0x5EC760;
constexpr uintptr_t OFF_ReleaseTitle = 0x5ECB48;    // sub_5ECB48 释放字符串标题字段(out, flag)
constexpr uintptr_t OFF_SettingsIconLoad = 0x61FD94;
constexpr uintptr_t OFF_SettingsContentCreate = 0xA53C18;
constexpr uintptr_t OFF_SettingsContentWidth = 0xA4E5F0;
constexpr uintptr_t OFF_SettingsUIScaleContext = 0x26D0B58;
constexpr uintptr_t OFF_SettingsUIScale = 0x7095C8;
constexpr uintptr_t OFF_SettingsScaleFloat = 0x7095DC;
constexpr uintptr_t OFF_CheckboxCreate = 0xA4DEAC;
constexpr uintptr_t OFF_ScrollAddRow = 0xA52458;
constexpr uintptr_t OFF_SettingsAddWidget = 0xA4DA68;
constexpr uintptr_t OFF_LocalizeKey = 0x14F3354;
constexpr uintptr_t OFF_PERSISTENT_BANNER_REBUILD = 0x1301B08; // sub_1301B08：PersistentMessage 横幅文本重建
                                                            // （按当前语言 localize OFFLINE/_CONTENT；
                                                            //   仅 screen_bindings.cpp 使用，v7.37 迁出框架）
constexpr uintptr_t OFF_PersistSave = 0x153E084;     // sub_153E084(manager,key*,value)：保存 bool 配置项
constexpr uintptr_t OFF_PersistReadBool = 0x153E560; // sub_153E560(config,key*,out*)：读取 bool 配置项，返回是否命中
constexpr uintptr_t OFF_PersistManager = 0x26C5ED8;  // qword_26C5ED8：持久化管理器指针全局
constexpr uint32_t CHECKBOX_VIEW_HIGH_ID = 31;
constexpr uint32_t CHECKBOX_VIEW_LOW_ID = 32;
// Settings 页面结构偏移（hook 共享代码引用，按架构取值）
constexpr uintptr_t SETTINGS_PAGE_CONTAINER = 240;  // 0xF0 page+240 → tab 容器
constexpr uintptr_t SETTINGS_PAGE_DIRTY = 292;      // page+292 dirty flag（sub_A4E7B0 检查→调 sub_A4EA34）
constexpr uintptr_t SETTINGS_PAGE_OWNER = 216;      // 0xD8 page+216 → owner
constexpr uintptr_t SETTINGS_OWNER_CONTROLLER = 8;  // owner+8 → controller
constexpr uintptr_t OFF_SETTINGS_TITLE_CTRL_VTABLE = 0x24E3EC0; // 0xD8 标题controller vtable(sub_A538F0)
// 页内 controller 向量=std::vector{begin@216,end@224,cap@232}
// (sub_A4C490:push 比较 *(+224)==*(+232) 判扩容、*(+224)+=8 推进=end 游标;
//  v7.13 误用 224/232 导致解引用 end 游标→vtable 校验失败→静默返回)
constexpr uintptr_t SETTINGS_PAGE_CTRL_VEC         = 216;   // 向量 begin(根标题=element[0])
constexpr uintptr_t SETTINGS_PAGE_CTRL_VEC_END     = 224;   // 向量 end 游标
constexpr uintptr_t SETTINGS_CONTROLLER_TITLE = 184;      // controller+184 标题字段（原版页面函数写入）
constexpr uintptr_t SETTINGS_CONTROLLER_TITLE_HEAP = 200; // controller+200 标题字符串堆指针
constexpr uintptr_t SETTINGS_CONTROLLER_CONTENT = 208;    // controller+208 挂载的 content（vtable+44 字段）
constexpr uintptr_t SETTINGS_DISPATCH_GUARD = 184;        // page+184 dispatch 收尾 guard（诊断用，双架构同值）
constexpr uintptr_t SETTINGS_VT_LAYOUT = 416;             // content vtable+416 五参 layout 槽（ARM32=208）
constexpr uintptr_t SETTINGS_VT_SETPOS = 424;             // prompt 容器 vtable+424 setPos 槽（ARM32=212）
constexpr uintptr_t SETTINGS_CONTENT_ALLOC = 0xF0;        // content operator new 大小（ARM32=0xA8）
constexpr uintptr_t SETTINGS_CONTAINER_ALLOC = 0xD0;      // prompt 文本容器分配大小（ARM32=0x94）

// --- B 方案：Sexy::ScrollWidget 可滚动内容容器（v7.26 起，仅 ARM64）---
// 逆向来源（IDA，同 9.8.1 基址）：
//   vtable off_260BFF0(vtab+416 layout=sub_16EF39C, vtab+88 AddChild=sub_1699E20)
//   单参构造 sub_16EE994；双参构造 sub_16EE71C；对象 operator new 大小 0x1E8(=488)。
//   layout(sub_16EF39C) 以内容尺寸(+312/+316) 对比视口(a1宽高-内边距-滚动条宽)
//   决定是否需要滚动：结果存入 +468 标志；+308 为滚动轴 flags；+300/+304 为
//   垂直/水平 mode(0=内容起点,2=居中,3=贴边)。画滚动条判定依此。
constexpr uintptr_t OFF_ScrollWidgetCtor   = 0x16EE994;   // ScrollWidget 单参构造(obj)
constexpr uintptr_t OFF_ScrollWidgetCtor2  = 0x16EE71C;   // 双参构造(obj, rect)（预留）
constexpr uint32_t  SCROLL_WIDGET_ALLOC    = 0x1E8;       // ScrollWidget operator new 大小
// Widget 通用位置/尺寸字段（float，双架构同值，ARM64 验证于 sub_16EF39C）
constexpr uintptr_t WIDGET_OFF_X = 68;                    // +68  x（sub_A4DA68 setPos / layout 读）
constexpr uintptr_t WIDGET_OFF_Y = 72;                    // +72  y
constexpr uintptr_t WIDGET_OFF_W = 76;                    // +76  width
constexpr uintptr_t WIDGET_OFF_H = 80;                    // +80  height
// ScrollWidget 滚动相关字段（相对对象头）
constexpr uintptr_t SCROLL_MODE_Y = 300;                  // +300 垂直滚动 mode
constexpr uintptr_t SCROLL_MODE_X = 304;                  // +304 水平滚动 mode
constexpr uintptr_t SCROLL_FLAGS  = 308;                  // +308 滚动轴 flags（bit0=X,bit1=Y）
constexpr uintptr_t SCROLL_CONTENT_W = 312;               // +312 内容宽
constexpr uintptr_t SCROLL_CONTENT_H = 316;               // +316 内容高
constexpr uintptr_t SCROLL_STATE    = 468;                // +468 需要滚动标志（bit0=X,bit1=Y）
constexpr uintptr_t SCROLL_INSETS   = 320;                // +320 滚动内边距（int32x2 left,top）
constexpr uintptr_t SCROLL_OFF_W    = 460;                // +460 水平滚动偏移（float）
constexpr uintptr_t SCROLL_OFF_H    = 464;                // +464 垂直滚动偏移（float）
constexpr uintptr_t SCROLL_VP_W     = 452;                // +452 可视区宽（float）
constexpr uintptr_t SCROLL_VP_H     = 456;                // +456 可视区高（float）

// UseHighViewAngle 配置字段：DisplayInfo 对象（即 g_DisplayInfo 解引用后的对象）+ 0xA16(2582)
// 该字节位于 HasDisabledUsageSharing(+2579)/DownloadPermissionOnWWAN(+2580)/
// HasAskedPermissionOnWWAN(+2581) 与 PatchVersionProgressDismissed(+2584) 之间的空闲 padding。
constexpr uintptr_t CONFIG_USE_HIGH_VIEW_ANGLE = 2582; // 0xA16

// --- Prompt 文本创建链路（复刻 DataSharing sub_A4EA34）---
constexpr uintptr_t OFF_FontLoad = 0x7C65BC;           // sub_7C65BC(fontConfig)：加载文本/字体上下文
constexpr uintptr_t OFF_FontContext = 0x26A1D50;       // qword_26A1D50：字体配置对象指针
constexpr uintptr_t OFF_PromptStyle = 0x26A1C18;       // unk_26A1C18：prompt 文本样式（16 字节）
constexpr uintptr_t OFF_TextMeasure = 0x171122C;       // sub_171122C(ctx,str,outW,outH,width)：测量文本尺寸
constexpr uintptr_t OFF_TextContainerCreate = 0x16AC6DC; // sub_16AC6DC(container)：文本容器构造
constexpr uintptr_t OFF_TextLabelCreate = 0x1714EE0;   // sub_1714EE0(...)：创建文本标签
constexpr uintptr_t OFF_TextContainerAdd = 0x16AC798;  // sub_16AC798(container,label)：标签加入容器

// --- Board 对象字段偏移 ---
// 验证：反编译 OFF_BoardZoom，观察其读写的 Board 成员偏移
constexpr uintptr_t BOARD_17  = 68;     // 0x044  X坐标偏移 (int, 坐标转换用: screenX = b281 + b280*(PPU*x - b281) + b17)
constexpr uintptr_t BOARD_CAM_RENDER_X = BOARD_17; // v39：共享诊断/直写代码用别名（ARM32 为 board+0x24，见 ARM32 段）
constexpr uintptr_t BOARD_270 = 1080;   // 0x438  种植偏移 (int, 进入 board[281] 参与坐标转换)
constexpr uintptr_t BOARD_280 = 1120;   // 0x460  PPU缩放因子 (float, Hook强制1.0)
constexpr uintptr_t BOARD_281 = 1124;   // 0x464  棋盘宽度像素 (float, = UIScale(board[274]+board[272]) + board[270])
constexpr uintptr_t BOARD_283 = 1132;   // 0x46C  左对齐偏移 (int, 负值, 像素)
constexpr uintptr_t BOARD_284 = 1136;   // 0x470  种植摄像机位置 (int, =board[270])
constexpr uintptr_t BOARD_285 = 1140;   // 0x474  选卡摄像机位置 (int, 像素)
constexpr uintptr_t BOARD_286 = 1144;   // 0x478  展示僵尸位置 (int, 正值, 像素)
constexpr uintptr_t BOARD_PAUSED = 180;  // 0xB4   暂停标志 (byte, 0=运行 1=暂停)
// 验证：sub_AF3330 读取 board+0xB4 决定是否跳过 board 更新主逻辑；
//       sub_1526CE8(board, 0/1) 写入该字节切换暂停/恢复。

// --- 全局变量偏移 ---
// 验证：BoardZoom 内通过 ADRP+LDR 加载 g_DisplayInfo 指针，解引用后访问 +0xF4/+0xF8
constexpr uintptr_t OFF_G_DisplayInfo        = 0x26BFC10;  // DisplayInfo 指针全局变量
constexpr uintptr_t DISPLAYINFO_SCREEN_WIDTH  = 244;       // 0xF4  screenWidth 字段偏移
constexpr uintptr_t DISPLAYINFO_SCREEN_HEIGHT = 248;       // 0xF8  screenHeight 字段偏移
constexpr uintptr_t DISPLAYINFO_BOARD         = 2472;      // 0x9A8 board 指针在 DisplayInfo 对象内偏移
constexpr uintptr_t DISPLAYINFO_SCREEN_OFFSET = 1820;      // 0x71C screenOffset（BoardLayout_ApplyZoom 用）

// --- 语言管理器（v3.3 IDA 实证：CDN 清单收集 sub_AC8418 与本地加载
//     sub_11F475C 共用链路 [G]+0x7C8 → LangMgr，[LangMgr+0x1E4] 为
//     当前语言 FourCC（大写 locale 打包，如 "EN-US"=0x454E5553），
//     经 sub_16A8A4C "%c%c-%c%c" 转字符串后拼 "LawnStrings-%s" 文件名；
//     运行期无写入者，仅构造期初始化一次，直写后稳定生效）---
constexpr uintptr_t DISPLAYINFO_LANGMGR       = 1992;      // 0x7C8 语言管理器指针（DisplayInfo 内）
constexpr uintptr_t LANGMGR_LOCALE_ID         = 484;       // 0x1E4 次语言 FourCC 字段（LangMgr 内）
constexpr uintptr_t LANGMGR_MAIN_LOCALE_ID    = 480;       // 0x1E0（v6.2：勿写！）。GetGroupForFile
                                                            // （sub_16B2DEC）匹配为 (条目lang1==0 或
                                                            // ==[+0x1E0]) && (条目lang2==0 或 ==[+0x1E4])，
                                                            // 但 MuMu 实测游戏以 zh-cn 正常运行时该字段
                                                            // 恒为 0x600(=1536，非 FourCC)——LawnStrings 条目
                                                            // lang1==0，匹配只需 +0x1E4；且该字段被游戏他处
                                                            // 读取，v6.1 写 FourCC 污染它 = 进游戏崩溃根因。
                                                            // v6.2 起只读诊断，不写入。
constexpr uint32_t  LANG_ID_EN_US             = 0x454E5553u; // "EN-US"
constexpr uint32_t  LANG_ID_ZH_CN             = 0x5A48434Eu; // "ZH-CN"

// --- 字体配置对象字段（v7.0 只读探针；对象指针 = OFF_FontContext 全局）---
// sub_7C65BC(fontLoad) 实证：+32 = 字体名 wstring（构建文本上下文的输入），
// +104 = 已构建上下文缓存（非 0 时 fontLoad 直接返回缓存，不重建）。
constexpr uintptr_t FONTCONFIG_FONT_NAME      = 32;        // fontConfig+32：字体名 wstring
constexpr uintptr_t FONTCONFIG_CACHED_CTX     = 104;       // fontConfig+104：缓存文本上下文

// ---- 【废弃】资源 id 通道偏移 ----
// 随 deprecated/resource_id_channel.cpp 一并归档，仅归档件引用；现役实现
// 不再使用（改用 OFF_PACKAGE_FIND_RECORD 的 RSB 名查找，见 resource_file.cpp）。
constexpr uintptr_t OFF_RESOURCE_DESC_INIT           = 0x10A9714;
constexpr uintptr_t OFF_RESOURCE_HANDLE_INIT         = 0x153397C;
constexpr uintptr_t OFF_RESOURCE_HANDLE_RELEASE      = 0x15339EC;
constexpr uintptr_t OFF_GENERIC_RESFILE_DESC_VTABLE  = 0x24A2298;
constexpr uintptr_t OFF_GENERIC_RESFILE_RESOLVE      = 0x65C42C;
constexpr uintptr_t OFF_RESOURCE_MANAGER_GET         = 0x1545628;
constexpr uintptr_t OFF_RESOURCE_HANDLE_RESOLVE      = 0x154E264;
constexpr uintptr_t OFF_RESOURCE_TABLE_GET           = 0x154DA3C;
constexpr uintptr_t OFF_RESOURCE_OBJECT_GET          = 0x1548698;
constexpr uintptr_t OFF_GENERIC_RESFILE_TYPE          = 0x169BCF0;
constexpr uintptr_t OFF_RESOURCE_APP_SINGLETON       = 0x26C5ED8;
constexpr uintptr_t OFF_RESOURCE_BUFFER_INIT         = 0x1596A34;
constexpr uintptr_t OFF_RESOURCE_BUFFER_DESTROY      = 0x1596A54;
constexpr uintptr_t OFF_RESOURCE_OPEN                = 0x153EDD0;

// --- PVZDB 热重载（v3.4 IDA 实证：CDN Apply 链路终点，运行期可调）---
// sub_120AEFC(db, tableID, path) "PVZDB::LoadPackageForTableFromRTONFile"：
//   路径先查内部归档失败后走 fopen("rb") 文件系统；对已加载表自动经
//   sub_120C170 清理旧数据再装载 = 原生热替换语义。CDN Apply
//   （sub_D58544）与离线持久化加载均走此入口。
constexpr uintptr_t OFF_PVZDB_SINGLETON       = 0x26962F8; // qword_26962F8：PVZDB 单例指针全局
constexpr uintptr_t OFF_DB_LOAD_RTON          = 0x120AEFC; // 加载器函数偏移
// sub_13D0F30()：官方 Localized Strings 应用入口。读取 table 131 的键值对，
// 逐项写入 qword_26C5ED8 的本地化容器，完成后卸载 table 131。
constexpr uintptr_t OFF_APPLY_LOCALIZED_STRINGS = 0x13D0F30;
// --- 设置页 Tab 对象（v7.4 标题刷新；createTab=sub_A4D79C，Tab::init=sub_A54230）---
// Tab 0xF0 字节：+184 标题 wstring（+184 flag/size、+200 heap，构造时经
// sub_14F324C 从容器解析一次后不再更新=标签栏标题滞留根因）；+216/+224
// 图标 widget；+232 文本标签(0x2D8)。改写序列与 controller 标题同构。
constexpr uintptr_t OFF_SETTINGS_TAB_VTABLE    = 0x24E44E8; // Tab 对象 vtable
constexpr uintptr_t SETTINGS_TAB_TITLE         = 184;       // Tab+184：标题 wstring
constexpr uintptr_t SETTINGS_TAB_TITLE_HEAP    = 200;       // Tab+200：标题堆指针
// Tab+232 文本标签（0x2D8，sub_130A068 创建：标题键经 sub_14F324C 解析后
// 经 vtable+752 SetText 写入，SetText 为拷贝语义）。可见标题文字 = 标签
// 自己的副本，刷新标题必须同时更新标签。
constexpr uintptr_t OFF_SETTINGS_TAB_LABEL_VTABLE = 0x25E06B0; // 标签 vtable
constexpr uintptr_t SETTINGS_TAB_LABEL            = 232;       // Tab+232：标签指针
constexpr uintptr_t SETTINGS_TAB_LABEL_SETTEXT_VT = 752;       // 标签 vt+752：SetText
// --- v7.16：0x2D8 标签构造器捕获（主界面/弹窗等数据驱动文案的统一入口）---
// sub_130A068(label,id,iface,textObj,style,font)：textObj 为键(方括号)时
// 构造器内部 sub_14F324C 解析一次并经 vt+752 SetText 写入 label+360 文本
// wstring（SetText 拷贝语义并同步 label+584 度量对象）。菜单 [MAINMENU_PLAY]
// 即此路径（v7.15 盘点实证：调用点 +0x130a180 在构造器内）。
constexpr uintptr_t OFF_SETTINGS_LABEL_CREATE   = 0x130A068; // 标签构造器
constexpr uintptr_t SETTINGS_LABEL_TEXT         = 360;      // label+360 文本 wstring
// --- 设置行初始化器（v7.7：全部设置行原位改写）---
// sub_A54230(row, titleKeyString)：所有带标题的设置行（Tab 行
// createTab/sub_A4D79C、Music/SoundFX 滑条行 sub_A4DBB0、checkbox 行
// sub_A4DEAC）统一经它初始化 0xF0 行对象：标题键经 sub_14F324C 从容器
// 解析一次,存 +184(flag/size)+200(heap)。hook 它即可捕获全部行对象
// 与标题键,语言切换后原位改写 +184(返回显示/状态变化时按 +184 重绘)。

constexpr uintptr_t OFF_SETTINGS_ROW_INIT       = 0xA54230;
// --- v7.11：容器键写入（语言页标题键,官方 apply 逐键写入同款函数）---
// sub_15407DC(owner, key窄string, value宽string)：向容器插入/更新一个键。
// owner = qword_26C5ED8(与 OFF_PersistManager 同址)。语言切换后将


// --- RSB 通道诊断（v6.2；【仅 Debug 探针】使用，Release 版不执行）---
// sub_120AEFC 内部调 sub_153EDD0(qword_26C5ED8, name, buf, 1)：
//   pakMgr = qword_26C5ED8（与 OFF_PersistManager 同一对象，双重身份：
//   Persist 配置 + Pak 文件管理）；rsm = *(pakMgr+0x858)（ResStreamsManager）。
//   前置开关：rsm 非空 且 *(rsm+24)!=0（sub_16AD8D0），否则整个 RSB 查找
//   被跳过直接落磁盘。GetGroupForFile（sub_16B2DEC）两条查找路径：
//   路径1 挂载表扫描（rsm+112 包数量，rsm+120 包数组，type==3 的包内
//   文件名表查找，无语言匹配）；路径2 全局哈希树（rsm+48 根指针非空
//   即 sub_17AFECC 可用，+48+8 条目计数，查找大小写不敏感，a4=1 时
//   做 +0x1E0/+0x1E4 语言匹配）。
constexpr uintptr_t OFF_PAK_MANAGER           = 0x26C5ED8; // qword_26C5ED8：Pak 管理器（=PersistManager 同址）
constexpr uintptr_t PAKMGR_RES_STREAMS_MGR    = 0x858;     // pakMgr+0x858 → ResStreamsManager 指针
constexpr uintptr_t RSM_RSB_SWITCH            = 24;        // rsm+24：RSB 通道开关（QWORD 指针，非 0 才走
                                                            // RSB，sub_16AD8D0 判定；v6.3 修正为按指针读）
constexpr uintptr_t RSM_MOUNT_COUNT           = 112;       // rsm+112：挂载包数量（路径 1）
constexpr uintptr_t RSM_MOUNT_ARRAY           = 120;       // rsm+120：挂载包数组指针
// sub_16B31EC：ResStreamsManager 按 RSB 名字取记录 (dataPtr,size)。
//   uint32_t sub_16B31EC(rsm, groupIdx, name, &dataPtr, &dataSize)
//   groupIdx=-1 时遍历全部 type3 挂载组按名字查（radix），命中后按
//   "组 buffer 基址 + 组内文件偏移 + node.off" 算出 dataPtr，size=node.size。
//   这是游戏读任意 rton 原始字节的原语（sub_153EDD0 内部即调用它）。
constexpr uintptr_t OFF_PACKAGE_FIND_RECORD   = 0x16B31EC;
constexpr uintptr_t RSM_GLOBAL_HASH_TREE      = 48;        // rsm+48：全局哈希树根指针（路径 2）
constexpr uintptr_t HASH_TREE_ENTRY_COUNT     = 8;         // 树对象+8：条目计数
// --- RSB 树实查诊断（v6.3；【仅 Debug 探针】使用，Release 版不执行）---
// GetGroupForFile（sub_16B2DEC）/取数（sub_16B31EC）共用基数树查找
// sub_17AFEDC(treeRoot, name)：树根【内嵌】于宿主结构（挂载组+32 /
// rsm+48，传结构地址本身而非其内容），逐字符 toupper 大小写不敏感，
// '/'(0x2F) 会被调用方换成 '\'(0x5C) 后再查。返回节点 = uint32[3]：
//   node[0]=标志/组号（0x10000000 位=语言变体复合条目，低 28 位=
//           RSB 主对象记录索引；-1=无效）
//   node[1]=数据偏移  node[2]=数据大小
// 挂载组结构（mountArr 数组，每组 200 字节）：
//   +24 type（路径 1 只认 type==3）  +32 内嵌组内名字树
// 语言变体复合条目（全局树命中且 node[0]&0x10000000）：
//   rsb = *(rsm+32)（RSB 主对象）
//   rec = rsb + *(u32)(rsb+56) + (u64)*(u32)(rsb+60) * (node[0]&0xFFFFFFF)
//   子条目数 = *(u32)(rec+1152)；子条目 i（步长 16 字节）：
//     +0=组号  +4=lang1（匹配 LangMgr+0x1E0，0=不限）
//     +8=lang2（匹配 LangMgr+0x1E4，0=不限）
constexpr uintptr_t OFF_RADIX_FIND            = 0x17AFEDC; // sub_17AFEDC 基数树查找（只读，无副作用）
constexpr uintptr_t GROUP_MOUNT_TYPE          = 24;        // 挂载组+24：type
constexpr uintptr_t GROUP_NAME_TREE           = 32;        // 挂载组+32：内嵌组内名字树
constexpr uintptr_t GROUP_ENTRY_STRIDE        = 200;       // 挂载组数组步长（字节）
constexpr uintptr_t RSM_RSB_MAIN              = 32;        // rsm+32：RSB 主对象指针
constexpr uintptr_t RSB_MAIN_TABLE_OFF        = 56;        // rsb+56：记录表基址偏移（u32）
constexpr uintptr_t RSB_MAIN_REC_SIZE         = 60;        // rsb+60：记录步长（u32）
constexpr uintptr_t RSB_VARIANT_COUNT_OFF     = 1152;      // rec+1152：复合条目子条目数（u32）
constexpr uintptr_t RSB_VARIANT_FIRST_OFF     = 128;       // rec+128：首个子条目
constexpr uintptr_t RSB_VARIANT_STRIDE        = 16;        // 子条目步长（字节）
constexpr uint32_t  RSB_VARIANT_FLAG          = 0x10000000u; // node[0] 语言变体复合条目标志位
constexpr uintptr_t OFF_CDN_APPLY             = 0xD58544;  // CDN Apply（含广播+日志，热重载走此入口）
constexpr uintptr_t OFF_DB_SAVE_RTON          = 0x120C2A8; // PVZDB::SavePackageForTableToFile（内存表→RTON 落盘）
constexpr uintptr_t OFF_DELEGATE_MGR_PTR      = 0x263F070; // off_263F070 → Delegate 管理器指针槽
constexpr uintptr_t DELEGATE_MGR_LOCK_DWORD   = 0x50;      // mgr+0x50 重入计数（v6.0 反汇编复核修正，
                                                            // 旧值 0x250 为误记；d58694: LDR W8,[X20,#0x50]，
                                                            // mgr 对象静态位于 0x26A0DD0，+0x50=dword_26A0E20）
constexpr uintptr_t OFF_DELEGATE_UNLOCK       = 0x9AA030;  // sub_9AA030(mgr)：计数归零时的解锁调用
// 表应用完成广播（照抄 sub_D58544 @0xd58670-0xd586dc）：
//   mgr=[0x263F070槽]；Lock=vtable+0x18(mgr,&ret)→{begin,end}；
//   步长0x30遍历，回调 elem+0x28 槽位，参数 X1=tableID；
//   前后维护 mgr+0x50 重入计数，归零调 sub_9AA030(mgr)。
constexpr uintptr_t OFF_A23A8C                 = 0xA23A8C; // sub_A23A8C 方向表（8 个 case 返回摄像机移动坐标，诊断用）
constexpr uintptr_t OFF_C187C                  = 0x6C187C; // sub_6C187C MoveBoard action 工厂(xStart,xEnd,a3,a4,a5=4,dur) 诊断用
constexpr uintptr_t OFF_AEF69C                 = 0xAEF69C; // sub_AEF69C 渲染坐标转换(Board,{x,y,w,h}) 乘法型 screenX=b281+scale*(worldXpx-b281+b17) 诊断用
constexpr uintptr_t OFF_G_UIScaleContext       = 0x26D0B58;// g_UIScaleContext 指针全局
constexpr uintptr_t UISCALE_VALUE              = 2432;     // 0x980 UIScale(float) 值偏移（g_UIScaleContext 对象内）
// --- News 页本地化（自定义语言短码兜底，见 screen_bindings.cpp）---
//   News 取键链：sub_A50A28(News页) → sub_98A694(FourCC→两字母 key，白名单
//   en/de/fr/it/pt/es) → sub_145B760(表 "NewsData") → sub_145BE84(entry,key)
//   → sub_145CAC0(红黑树严格 memcmp 查 LocalizedData[key]，未命中→空)。
//   自定义语言不在白名单 → 空 key → 空白；故 hook sub_145BE84 做第一段后缀兜底。
constexpr uintptr_t OFF_NEWS_SELECT_LOCALE     = 0x145BE84; // sub_145BE84(entry, key)：取 LocalizedData[key]

// --- 组件补丁：EAText 字形缓存 / 图鉴（见 components/README.md）---
//   一、字形缓存尺寸：sub_1713384 按屏幕最小边选 512/1024/2048（CMP #0x401/#0x281），
//       经 sub_17F3560(cache,a2,a3,a4) 构造，a4 写入 cache+0xA4。强制 a4=2048 即可。
//   二、图鉴翻页：小图鉴条目切换入口 sub_869258（AlmanacObjectChooser::Select，
//       a2=目标条目索引，内部重建选择项并播放 "Play_UI_Menu_Tab_Scroll"/"SelectItem"）。
//       字形缓存「清+重建」组合（对齐 iOS ClearPrimeGlyphCache + rebuild）：
//         EAText 全局 g = sub_1713288()；cache = *(g+8)（PrimeGlyphCache）
//         清  = sub_1714090(g) = sub_17F3DF4(cache)  —— 销毁 image render data
//         重建 = sub_1714098(g) = sub_17F3E70(cache) —— 重新构建渲染数据
//       二者即引擎自身 sub_153DEB4 重置字形缓存时成对调用的序列（153dec4/153def8）。
//       根因证据：sub_1712664（PrimeGlyphMesh::BuildPart）告警
//         "...There is no place to put this glyph into the glyph cache.
//          You should clear the cache before building the parts."
constexpr uintptr_t OFF_GLYPH_CACHE_CTOR       = 0x17F3560; // sub_17F3560(cache, a2, a3, size)：写入 cache+0xA4
constexpr uintptr_t OFF_GLYPH_CACHE_SIZE_SEL   = 0x1713384; // sub_1713384：屏幕最小边→512/1024/2048
constexpr uintptr_t OFF_EATEXT_GLOBAL_GET      = 0x1713288; // sub_1713288()：返回 EAText 全局指针（+8 = 字形缓存实例）
constexpr uintptr_t OFF_GLYPH_CACHE_DESTROY    = 0x17F3DF4; // sub_17F3DF4(cache)：PrimeGlyphCache::Destroying image render data（被 CLEAR 包裹）
constexpr uintptr_t OFF_GLYPH_CACHE_CLEAR      = 0x1714090; // sub_1714090(g)：= sub_17F3DF4(*(g+8)) 清字形渲染数据
constexpr uintptr_t OFF_GLYPH_CACHE_REBUILD    = 0x1714098; // sub_1714098(g)：= sub_17F3E70(*(g+8)) 重建字形渲染数据
constexpr uintptr_t OFF_ALMANAC_WIDGET_DEPRESS = 0x86EC40;  // sub_86EC40：AlmanacWidget::ButtonDepress（a2: 0=植物/1=僵尸/2=升级）
constexpr uintptr_t OFF_ALMANAC_CHOOSER_SELECT = 0x869258;  // sub_869258：AlmanacObjectChooser::Select（a2=目标条目索引）小图鉴翻页/切换

// --- 低视角相机系统（调查用）---
// 相机定位公式（sub_7EAD24/sub_7F45C4）：
//   camX(+504) = animX(世界X) - viewW/2 + b17(+68)，再 clamp 到 [+512, +512+520]
// 动画字段（sub_7EAB50 启动，sub_7F45C4 每帧插值）：
//   +1064/+1068 起始(世界坐标)，+1072/+1076 目标，+1080 开始时间(FLT_MAX=无动画，
//   布局期复用为 b270 像素偏移)，+1084 结束时间（时长 0.618s）
constexpr uintptr_t BOARD_18           = 72;    // 0x048  Y坐标偏移 (int, 与 b17 成对)
constexpr uintptr_t BOARD_CAM_X        = 504;   // 0x1F8  相机X (float)
constexpr uintptr_t BOARD_CAM_Y        = 508;   // 0x1FC  相机Y (float)
constexpr uintptr_t BOARD_CLAMP_X_MIN  = 512;   // 0x200  相机X clamp 下限 (int)
constexpr uintptr_t BOARD_CLAMP_X_RNG  = 520;   // 0x208  相机X clamp 范围 (int)
constexpr uintptr_t BOARD_ANIM_START_X = 1064;  // 0x428  相机动画起始X (float, 世界坐标)
constexpr uintptr_t BOARD_ANIM_START_Y = 1068;  // 0x42C
constexpr uintptr_t BOARD_ANIM_END_X   = 1072;  // 0x430  相机动画目标X (float, 世界坐标)
constexpr uintptr_t BOARD_ANIM_END_Y   = 1076;  // 0x434
constexpr uintptr_t BOARD_ANIM_T1      = 1084;  // 0x43C  动画结束时间 (float)
constexpr uintptr_t BOARD_TRANSFORM    = 776;   // 0x308  坐标变换对象指针
// 变换对象（sub_10C0E98）：world = base + (px - base) * k
//   +16 = (kX, kY) float×2；+24 = (baseX, baseY) float×2
constexpr uintptr_t XFORM_K    = 16;
constexpr uintptr_t XFORM_BASE = 24;
constexpr uintptr_t DISPLAYINFO_VIEW_W = 1820;  // 0x71C  viewW（相机公式/布局用视口宽, int）
constexpr uintptr_t DISPLAYINFO_VIEW_H = 1824;  // 0x720  viewH（int）
constexpr uintptr_t OFF_CameraAnimStart = 0x7EAB50; // sub_7EAB50 相机动画启动(Board*, {格X,格Y})
constexpr uintptr_t OFF_CameraJump      = 0x7EAD24; // sub_7EAD24 相机瞬移(Board*, {格X,格Y}, char force)
constexpr uintptr_t OFF_CameraUpdate    = 0x7F45C4; // sub_7F45C4 相机每帧更新(Board*) 动画插值+camX写入 诊断用 v20
                                                   // (v19 曾挂 sub_7EC08C @0x7EC08C，实测零调用为死路径)
constexpr uintptr_t OFF_StreetDinos     = 0x729638; // sub_729638 街道恐龙生成入口(a1=ctx, a2=X基准, a3=spawnMode)
                                                   // 两条路径共用：SpawnStreetDinos(sub_729AC0, X基准=b285+b270)
                                                   // 与 PlaceStreetDinos(sub_729600, X基准=b286+b270)

// --- v20 诊断：相机动画时钟（sub_7F45C4 读写）---
constexpr uintptr_t BOARD_ANIM_T0      = 1080;  // 0x438 动画开始时间 (float, FLT_MAX≈3.4e38=无动画静止)

// --- v23 诊断：震屏速度字段（sub_7F45C4 震屏衰减分支读写）---
// 震屏机制：vel(+528) 每帧 *=0.9（|vel|<=0.2 时置 0），camX(+504) += vel 后
// clamp 到 [+512, +512+520]；vel 归零后 camX 停在原地不回位 —— 撞 clamp 边界
// 即产生永久偏移（高视角左对齐位远离原版 clamp 区间的根因候选）
constexpr uintptr_t BOARD_SHAKE_VEL_X  = 528;   // 0x210 震屏速度X (float, 衰减系数0.9)
constexpr uintptr_t BOARD_SHAKE_VEL_Y  = 532;   // 0x214 震屏速度Y (float)
// clamp 字段由 sub_7EB4A0 设置：clampMin=UIScale*a2[23], range=UIScale*a2[25]-viewW
// （a2=布局表，原版几何；调用者 sub_7EB29C=种植相机链每次 pan 时重置）

#endif // __aarch64__


// ============================================================
//  ARM32 (armeabi-v7a / Thumb-2) 偏移
//
//  IDA 验证方法：
//    - BoardZoom @ 0x75D044 (sub_75D044)：函数内访问 Board+0x35C(float)/0x368~0x374(int)
//      调用者 sub_75C9F8 先调 sub_75D044 再调 sub_75D2D8(BoardZoom2)
//    - BoardZoom2 @ 0x75D2D8 (sub_75D2D8)：函数内访问 Board+0x35C(float)，长屏检测读 DisplayInfo+0x88/0x8C
//    - g_DisplayInfo @ 0x1E5DCEC (dword_1E5DCEC)：指针全局，解引用得到 DisplayInfo 对象
//
//  注意：ARM32 偏移与 ARM64 不同！
//    Board 字段偏移差约 260 字节（如 BOARD_280: ARM64=1120, ARM32=860）
//    DisplayInfo 字段偏移差约 108 字节（如 SCREEN_WIDTH: ARM64=0xF4, ARM32=0x88）
//    这是由于 ARM32 指针宽度 4 字节（ARM64 为 8 字节），结构体中指针字段
//    累积导致后续字段位置前移。
// ============================================================
#ifdef __arm__

// --- 函数地址 ---
constexpr uintptr_t OFF_BoardZoom  = 0x75D044;  // BoardLayout_ApplyZoom（计算 board[283~286]/270/284）
constexpr uintptr_t OFF_BoardZoom2 = 0x75D2D8;  // BoardZoom2（计算 board[280~282]）
constexpr uintptr_t OFF_ShakeBoard = 0x774B64;  // ShakeBoard(Board*, xAmt, yAmt, duration) — 创建震动action
constexpr uintptr_t OFF_A23A8C     = 0x6AC92C;  // sub_6AC92C 方向表（8 case，签名 (result, out startX, out endX)，与 ARM64 sub_A23A8C 一致）
                                               // 反编译验证：v5 = *(board**)(di+1728)；case0 startX = conv(-v5[218]/b283)
constexpr uintptr_t OFF_C187C      = 0x367D18;  // sub_367D18 MoveBoard action 工厂（6 参与 ARM64 sub_6C187C 一致）
constexpr uintptr_t OFF_StreetDinos = 0x3CD500; // sub_3CD500 街道恐龙生成入口(a1=ctx, a2=X基准, a3=spawnMode)
// 注意：OFF_AEF69C（渲染坐标转换）与 OFF_CameraAnimStart/Jump/Update（相机诊断链）
// 的 ARM32 等价函数尚未定位（0x7598E8 候选已排除：564 行大函数且不读 b17），
// 相应诊断 hook 在 ARM32 上不安装。

// --- Board 对象字段偏移 ---
// 验证：反编译 OFF_BoardZoom (sub_75D044)，关键指令：
//   VLDR S0, [R4,#0x35C]  → board[280] scale (float)
//   LDR  R1, [R4,#0x338]  → board[270] (int, 原版=0)
//   VSTR S0, [R4,#0x368]  → board[283] (int, 写入)
//   STR  R1, [R4,#0x36C]  → board[284] = board[270] (int, 写入)
//   STR  R0, [R4,#0x370]  → board[285] (int, 写入)
//   STR  R0, [R4,#0x374]  → board[286] (int, 写入)
constexpr uintptr_t BOARD_17  = 44;     // 0x2C  【v34 更正】草坪总宽 (int, 原版 BoardZoom sub_75D044 尾部写入 ≈屏宽，实测 3392)
                                         //       —— NOT 相机偏移 b17！ARM64 同一写入在 +0x4C(board[19])，其 b17/b18 在 +0x44/+0x48
                                         //       误当 b17 直写会裁剪种植触控区（v31~v33 实测：写 557 → 触控只剩左侧 557px）
                                         //       保留常量仅供诊断读取（原版值=3392≈屏宽 3413 可作观察锚点）
constexpr uintptr_t BOARD_CAM_RENDER_X = 36;  // 0x24 【v39 定位】相机渲染X偏移 (int) —— ARM64 b17(+0x44) 的 ARM32 等价字段。
                                         //       验证：ARM32 CameraUpdate sub_4924B4 尾部调
                                         //       sub_483504(board, animX - viewW/2 + *(int*)(board+36),
                                         //                       animY - viewH/2 + *(int*)(board+40), 0)
                                         //       与 ARM64 sub_7F45C4 公式 camX=animX-viewW/2+b17 完全同构。
                                         //       +40(0x28) 为 Y 等价（b18）。板书触控链不读此字段（v31~v33
                                         //       验证触控用 b270/b283/b284），直写安全。
constexpr uintptr_t BOARD_270 = 824;    // 0x338  种植偏移 (int, 原版=0)
constexpr uintptr_t BOARD_280 = 860;    // 0x35C  PPU缩放因子 (float, Hook强制1.0)
constexpr uintptr_t BOARD_281 = 864;    // 0x360  棋盘宽度像素 (float, ARM64同义字段0x464的连续排布推断)
constexpr uintptr_t BOARD_283 = 872;    // 0x368  左对齐偏移 (int, 负值, 像素)
constexpr uintptr_t BOARD_284 = 876;    // 0x36C  种植摄像机位置 (int, =board[270])
constexpr uintptr_t BOARD_285 = 880;    // 0x370  选卡摄像机位置 (int, 像素)
constexpr uintptr_t BOARD_286 = 884;    // 0x374  展示僵尸位置 (int, 正值, 像素)
// BOARD_PAUSED（ARM64 board+0xB4）的 ARM32 偏移未定位（0x9C/0xB4 候选扫描均无
// 可靠命中），暂停态绕过在 ARM32 上保持旧路径不启用；v39 起 board+36 直写
// （BOARD_CAM_RENDER_X）已保证暂停画面立即对齐，震屏延迟到 resume 执行无视觉副作用。

// --- 全局变量偏移 ---
// 验证：BoardZoom2 (sub_75D2D8) 内通过 LDR 加载 dword_1E5DCEC 指针：
//   LDR  R5, [PC,R0]; dword_1E5DCEC  → R5 = &dword_1E5DCEC
//   LDR  R0, [R5]                     → R0 = *dword_1E5DCEC = DisplayInfo 对象指针
//   VLDR S0, [R0,#0x8C]               → screenHeight (int, 用于长屏检测 > 1000)
//   VLDR S0, [R0,#0x88]               → screenWidth (int, 用于宽高比计算)
constexpr uintptr_t OFF_G_DisplayInfo        = 0x1E5DCEC;  // DisplayInfo 指针全局变量 (dword_1E5DCEC)
constexpr uintptr_t DISPLAYINFO_SCREEN_WIDTH  = 136;       // 0x88  screenWidth 字段偏移
constexpr uintptr_t DISPLAYINFO_SCREEN_HEIGHT = 140;       // 0x8C  screenHeight 字段偏移
constexpr uintptr_t DISPLAYINFO_BOARD         = 1728;      // 0x6C0 board 指针在 DisplayInfo 对象内偏移
                                                          // 验证：sub_6AC92C 反编译 v5 = *(_DWORD**)(dword_1E5DCEC + 1728)，
                                                          // v5[218..221] = b283/b284/b285/b286 与方向表语义一致

// --- UIScale（conv 换算，hkC187C/hkA23A8C 用）---
// 验证：方向表 sub_6AC92C 内 conv 调用 = sub_3AC310(dword_1E67CBC, px)：
//   int sub_3AC310(int uiCtx, int px) { return (int)((float)px / *(float*)(uiCtx + 1688)); }
constexpr uintptr_t OFF_G_UIScaleContext = 0x1E67CBC; // dword_1E67CBC：UIScale 上下文指针全局
constexpr uintptr_t UISCALE_VALUE        = 1688;      // 0x698 uiScale(float) 值偏移（uiCtx 对象内）

// ============================================================
//  Settings UI 视角切换功能链（v32，全部 IDA 反编译验证）
//
//  定位方法：反编译 ARM32 DataSharing 页面函数 sub_6D4420
//  （对应 ARM64 sub_A4EA34）与 tab 注册外层 sub_6D1894
//  （对应 ARM64 sub_A4D79C 的调用者），从调用链逐一映射：
//    - sub_6D4420 开头：controller = *(*(page+148)+4)（ARM64 page+216/owner+8）
//    - 标题经 sub_6D7AD0(controller, wstring) 写入 controller+132（ARM64 184）
//    - tab 注册序言在 sub_6D1894 内逐个调 sub_6D3068（createTab 5 参数）
//    - id=6/7 tab 用 sub_2C9348(unk_1E4E798/1E4E7B0) 加载图标
//    - dirty flag 框架 sub_6D41C4：page+188 置位 → 下一帧调 sub_6D4420
//    - dispatch sub_6D5BE8：switch(tabId)，case 12 直调 sub_6D4420
//    - 持久化链在 sub_620158（写）/sub_61A404（读）：
//      sub_11391A4(manager@1E62040, key, value) / sub_113973C(config, key, out)
//    - DisplayInfo 布局：+1791 HasDisabledUsageSharing/+1792/+1793/+1796
//      已占用，+1794 空闲（与 ARM64 2579~2584 区间 2582 空闲同构）
// ============================================================
constexpr uintptr_t OFF_SettingsCreate = 0x6D3068;       // sub_6D3068 createTab(page,id,title,iconN,iconS)
                                                        // hook 点；前 12B=PUSH+ADD R11+SUB.W 已验证
constexpr uintptr_t OFF_SettingsDispatch = 0x6D5BE8;     // sub_6D5BE8 dispatch(page,tabId)，switch 结构
                                                        // 与 ARM64 sub_A501E0 一致；12B 安全已验证
constexpr uintptr_t OFF_SettingsDataSharing = 0x6D4420;  // sub_6D4420 DataSharing 页面（dirty 重建入口）
constexpr uintptr_t OFF_SettingsStringCreate = 0x29837C; // sub_29837C wstring 构造(out,wchar_t*,len)
constexpr uintptr_t OFF_SettingsTitleStringCreate = 0x29D5DC; // sub_29D5DC wstring assign(out,src,len)，需先清零
constexpr uintptr_t OFF_SettingsIconLoad = 0x2C9348;     // sub_2C9348 iconLoad(resourcePtr)
constexpr uintptr_t OFF_SettingsBuildVersionIconNormal = 0x1E4E798;   // unk_1E4E798（id=3/6 tab 共用）
constexpr uintptr_t OFF_SettingsBuildVersionIconSelected = 0x1E4E7B0; // unk_1E4E7B0
constexpr uintptr_t OFF_SettingsAttach = 0x6D338C;       // sub_6D338C attachTab/addWidget(container,widget,centered,uiScale)
constexpr uintptr_t OFF_SettingsAddWidget = 0x6D338C;    // 同 attach（ARM64 也同址）
constexpr uintptr_t OFF_SettingsContentCreate = 0x6D9608; // sub_6D9608 content 构造（new(0xA8) 后调用）
constexpr uintptr_t OFF_SettingsTabContainerVtable = 0x1D46128; // sub_6D9608 @0x6D9644 写入的容器 vtable
constexpr uintptr_t OFF_SettingsContentWidth = 0x6D3FD0;  // sub_6D3FD0() 无参返回内容宽度
constexpr uintptr_t OFF_SettingsUIScale = 0x3AC3D0;      // sub_3AC3D0 scaleInt(ctx,v)=(int)(scale*v)
constexpr uintptr_t OFF_SettingsScaleFloat = 0x3AC3F0;   // sub_3AC3F0 scaleFloat(ctx,v)=(float)(scale*v)
constexpr uintptr_t OFF_CheckboxCreate = 0x6D3830;       // sub_6D3830 checkbox(page,id,labelStr,initState,width)
constexpr uintptr_t OFF_LocalizeKey = 0x10F6754;         // sub_10F6754 本地化键(out12B,key)：
                                                         // *key=='[' 时去括号查表，否则原样拷贝
constexpr uintptr_t OFF_PersistSave = 0x11391A4;         // sub_11391A4 saveBool(manager,key*,value)
constexpr uintptr_t OFF_PersistReadBool = 0x113973C;     // sub_113973C readBool(config,key*,out)，返回是否命中
constexpr uintptr_t OFF_PersistManager = 0x1E62040;      // dword_1E62040 持久化管理器指针全局
constexpr uintptr_t OFF_FontLoad = 0x466D64;             // sub_466D64 fontLoad(fontConfig)
constexpr uintptr_t OFF_FontContext = 0x1E4DE88;         // dword_1E4DE88 字体配置对象指针全局
constexpr uintptr_t OFF_PromptStyle = 0x1E4DDAC;         // 【v36 复核】prompt 文本样式 16 字节数据（.bss 内联，运行时初始化填充）。
                                             // 原版 sub_6D4420 @0x6D4770-774：LDR R0,[pool]=偏移 → LDR R1,[PC,R0] 源=.got 槽 0x1DEDB14，
                                             // 槽内容经 linker 重定位 = base+0x1E4DDAC（变量地址本身）→ styleCopy(out16B, base+0x1E4DDAC)。
                                             // 直接传槽地址，【禁止】再解引用（v35 误加一层读 .bss 内容 0 → SIGSEGV）
constexpr uintptr_t OFF_TextMeasure = 0x1329B9C;         // sub_1329B9C(ctx,strObj,0,outH*,width)
constexpr uintptr_t OFF_TextContainerCreate = 0x12CC93C; // sub_12CC93C 容器构造（new(0x94) 后调用）
constexpr uintptr_t OFF_TextLabelCreate = 0x132DA4C;     // sub_132DA4C(ctx,fontsizeBits,0,widthBits,heightF,strObj,0,0,style)
constexpr uintptr_t OFF_TextContainerAdd = 0x12CCA00;    // sub_12CCA00(container,label)
constexpr uintptr_t OFF_SetControllerTitle = 0x6D7AD0;   // sub_6D7AD0(controller,titleWStr)：标题写入 controller+132
constexpr uintptr_t OFF_MountContent = 0x6D3AA4;         // sub_6D3AA4(controller,content)：
                                                         // 内部释放旧 content（controller+144 字段，
                                                         // vtable+48/+12）并挂载新 content（vtable+44）
constexpr uintptr_t OFF_SettingsLayout = 0x6D61EC;       // sub_6D61EC(page) tab 布局初始化：
                                                         // dispatch 各 case 尾部 LABEL_33 调用
                                                         // （对应 ARM64 sub_A5084C）
constexpr uintptr_t OFF_SettingsUIScaleContext = 0x1E67CBC; // 同 OFF_G_UIScaleContext（共享代码统一名字）
constexpr uintptr_t OFF_StyleCopy = 0x118E040;           // sub_118E040(out16B,&unk_1E4DDAC)：拷贝 prompt 文本样式
constexpr uint32_t SETTINGS_VIEW_ANGLE_ID = 30;          // tab id：ARM32 原版占用 3~26，29 为 checkbox，30+ 空闲
constexpr uint32_t CHECKBOX_VIEW_HIGH_ID = 31;
constexpr uint32_t CHECKBOX_VIEW_LOW_ID = 32;
constexpr uintptr_t CONFIG_USE_HIGH_VIEW_ANGLE = 1794;   // 0x702 DisplayInfo 空闲 padding
                                                         // （1791/1792/1793/1796 已被原版占用）

// --- Settings 页面结构偏移（ARM32，指针 4 字节导致整体前移）---
constexpr uintptr_t SETTINGS_PAGE_OWNER = 148;     // page+148 → owner（ARM64 216）
constexpr uintptr_t SETTINGS_OWNER_CONTROLLER = 4; // owner+4 → controller（ARM64 8）
constexpr uintptr_t SETTINGS_PAGE_CONTAINER = 160; // page+160 → tab 容器（ARM64 0xF0=240）
constexpr uintptr_t SETTINGS_PAGE_DIRTY = 188;     // page+188 dirty flag（ARM64 292）
constexpr uintptr_t SETTINGS_DISPATCH_GUARD = 184; // page+184 dispatch 收尾 guard（诊断用）
constexpr uintptr_t CHECKBOX_WIDTH = 144;          // checkbox+144 宽度（诊断用）
constexpr uintptr_t CHECKBOX_STATE = 148;          // checkbox+148 状态（诊断用）
constexpr uintptr_t SETTINGS_VT_LAYOUT = 208;      // content vtable+208 layout（ARM64 416）
constexpr uintptr_t SETTINGS_VT_SETPOS = 212;      // container vtable+212 setPos（ARM64 424）
constexpr uintptr_t SETTINGS_CONTENT_ALLOC = 0xA8; // content 分配大小（ARM64 0xF0）
constexpr uintptr_t SETTINGS_CONTAINER_ALLOC = 0x94; // prompt 容器分配大小（ARM64 0xD0）

#endif // __arm__

#endif // OFFSETS_H
