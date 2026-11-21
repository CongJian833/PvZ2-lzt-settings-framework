# 偏移量填写指南

> 本项目所有与游戏版本强相关的地址与字段偏移都集中在两个头文件里：
> Android 侧 `jni/offsets.h`，iOS 侧 `iOS/include/offsets_ios.h`。
> **换用其他游戏版本时，通常只需要改这两个文件**，业务代码不动。

- Android 侧入口：[README 第三节「开始使用」](../README.md#三开始使用)
- iOS 侧入口：[iOS/README 第四节「构建与打包」](../iOS/README.md#四构建与打包)
- 日志排查：[日志系统](日志系统.md)

---

## 一、这两份表里各有什么

地址不是随便一个数字——每个常量都对应游戏里的**一个具体函数**或
**一个具体字段**。表分三类：

| 类别 | 含义 | 改动频率 |
|---|---|---|
| 函数地址 | 某个函数的入口偏移（可 hook 的点） | 换游戏版本必改 |
| 字段偏移 | 某个结构体里某个成员的字节偏移 | 换游戏版本必改 |
| 全局槽位 | 某个全局指针变量的地址 | 换游戏版本必改 |

其中有几个是**跨平台通用**的——它们描述的是游戏的数据结构，而不是
某一个平台的二进制布局。这类量在两端数值相同，可以直接沿用。

---

## 二、跨平台通用：Board 字段

Board 是棋盘状态结构，以 int 数组形式索引。以下成员在**两端完全一致**：

| 常量 | 含义 | 字节偏移 |
|---|---|---|
| `BOARD_270` | 种植偏移（int） | 1080（0x438） |
| `BOARD_280` | PPU 缩放因子（float） | 1120（0x460） |
| `BOARD_281` | 棋盘宽度（float） | 1124（0x464） |
| `BOARD_283` | 左对齐偏移（int，负值） | 1132（0x46C） |
| `BOARD_284` | 种植摄像机位置（int） | 1136（0x470） |
| `BOARD_285` | 选卡摄像机位置（int） | 1140（0x474） |
| `BOARD_286` | 展示僵尸位置（int） | 1144（0x478） |
| `BOARD_17` | 相机渲染 X 偏移（int） | 68（0x44） |

原因很直接：Board 的布局由**游戏源码**决定，与操作系统无关。
iOS 侧的实测记录也是逐条对上的（见 `iOS/FINDINGS.md`）。

> 换版本时，Board 字段**通常不变**——只有游戏自己改了 Board 结构才会动。

---

## 三、Android 侧（`jni/offsets.h`）

### 3.1 视角链

| 常量 | 含义 | ARM64 | ARM32 |
|---|---|---|---|
| `OFF_BoardZoom` | 布局缩放与对齐（写 board[283~286]/270/284） | `0xADEB80` | `0x75D044` |
| `OFF_BoardZoom2` | 缩放下限（写 board[280~282]） | `0xADEDE0` | `0x75D2D8` |
| `OFF_ShakeBoard` | 震动 action 创建 | `0xAF8650` | `0x774B64` |
| `OFF_StreetDinos` | 街道恐龙生成 | `0x729638` | `0x3CD500` |
| `OFF_GetBoard / DISPLAYINFO_BOARD` | 取当前 Board 指针 | `DISPLAYINFO_BOARD=2472` | `1728` |

### 3.2 设置页链

| 常量 | 含义 | ARM64 | ARM32 |
|---|---|---|---|
| `OFF_SettingsCreate` | Tab 创建（5 参数） | `0xA4D79C` | `0x6D3068` |
| `OFF_SettingsDispatch` | Tab 点击 / 事件分发 | `0xA501E0` | `0x6D5BE8` |
| `OFF_SettingsDataSharing` | 子页面创建（dirty 重建入口） | `0xA4EA34` | `0x6D4420` |
| `OFF_SettingsLayout` | 页面布局初始化 | `0xA5084C` | `0x6D61EC` |
| `OFF_SettingsAttach / OFF_SettingsAddWidget` | 控件挂到容器 | `0xA4DA68` | `0x6D338C` |
| `OFF_CheckboxCreate` | checkbox 行创建 | `0xA4DEAC` | `0x6D3830` |
| `OFF_LocalizeKey` | 本地化键 → 文本 | `0x14F3354` | `0x10F6754` |
| `OFF_SettingsStringCreate` | wstring 构造 | `0x5F1540` | `0x29837C` |
| `OFF_SettingsIconLoad` | 图标资源加载 | `0x61FD94` | `0x2C9348` |

页面结构偏移（ARM64 / ARM32 不同）：

| 常量 | 含义 | ARM64 | ARM32 |
|---|---|---|---|
| `SETTINGS_PAGE_CONTAINER` | Tab / 内容容器 | 240 | 160 |
| `SETTINGS_PAGE_OWNER` | 页面拥有者 | 216 | 148 |
| `SETTINGS_PAGE_DIRTY` | dirty 标志 | 292 | 188 |
| `SETTINGS_OWNER_CONTROLLER` | 拥有者 → controller | 8 | 4 |
| `SETTINGS_CONTROLLER_TITLE` | controller 标题字段 | 184 | 未定义 |
| `SETTINGS_CONTROLLER_CONTENT` | controller 挂载的 content | 208 | 未定义 |
| `SETTINGS_VT_LAYOUT` | content vtable layout 槽 | 416 | 208 |

### 3.3 持久化与配置

| 常量 | 含义 | ARM64 | ARM32 |
|---|---|---|---|
| `OFF_PersistSave` | 保存 bool 配置 | `0x153E084` | `0x11391A4` |
| `OFF_PersistReadBool` | 读取 bool 配置 | `0x153E560` | `0x113973C` |
| `OFF_PersistManager` | 持久化管理器指针全局 | `0x26C5ED8` | `0x1E62040` |
| `OFF_G_DisplayInfo` | 显示信息指针全局 | `0x26BFC10` | `0x1E5DCEC` |
| `CONFIG_USE_HIGH_VIEW_ANGLE` | 视角开关存放字段 | 2582 | 1794 |
| `DISPLAYINFO_SCREEN_WIDTH / HEIGHT` | 屏幕宽高 | 244 / 248 | 136 / 140 |

### 3.4 语言与本地化

| 常量 | 含义 | ARM64 | ARM32 |
|---|---|---|---|
| `DISPLAYINFO_LANGMGR` | 语言管理器在 DisplayInfo 内的偏移 | 1992 | — |
| `LANGMGR_LOCALE_ID` | 当前语言 FourCC 在 LangMgr 内的偏移 | 484 | — |
| `OFF_FOURCC_TO_STRING` | FourCC → 字符串 | `0x16A8A4C` | — |
| `OFF_LANG_NAME_FN_A / _B` | 拼 `LawnStrings-<locale>` 表名 | `0xAC8418` / `0x11F475C` | — |
| `OFF_APPLY_LOCALIZED_STRINGS` | 应用本地化字符串 | `0x13D0F30` | — |
| `OFF_NEWS_SELECT_LOCALE` | News 页取 LocalizedData | `0x145BE84` | — |
| `OFF_PERSISTENT_BANNER_REBUILD` | 主界面横幅文本重建 | `0x1301B08` | — |

**LANGMGR 字段只读不写**：邻近的主字段（`+0x1E0`）写进去会让游戏崩溃，
这是踩过的坑，改动时务必保持只读。

### 3.5 资源与语言表热重载（仅 ARM64）

| 常量 | 含义 |
|---|---|
| `OFF_PVZDB_SINGLETON` | PVZDB 单例指针全局 |
| `OFF_DB_LOAD_RTON` | RTON 加载器 |
| `OFF_DB_SAVE_RTON` | 内存表落盘 |
| `OFF_CDN_APPLY` | CDN Apply 链入口 |
| `OFF_DELEGATE_MGR_PTR` / `OFF_DELEGATE_UNLOCK` | 表变更广播 |
| `OFF_PAK_MANAGER` | Pak 管理器（与 `OFF_PersistManager` 同址） |
| `OFF_PACKAGE_FIND_RECORD` | 按 RSB 名字取记录 |
| `OFF_RADIX_FIND` | 基数树查找（只读） |

RSB 结构的内部偏移两端一致：

| 常量 | 含义 | 值 |
|---|---|---|
| `PAKMGR_RES_STREAMS_MGR` | pakMgr → 流管理器 | 0x858 |
| `RSM_RSB_SWITCH` | RSB 通道开关 | 24 |
| `RSM_MOUNT_COUNT` / `RSM_MOUNT_ARRAY` | 挂载包数量 / 数组 | 112 / 120 |
| `RSM_GLOBAL_HASH_TREE` | 全局哈希树根 | 48 |
| `GROUP_MOUNT_TYPE` / `GROUP_NAME_TREE` | 挂载组 type / 内嵌名字树 | 24 / 32 |
| `GROUP_ENTRY_STRIDE` | 挂载组数组步长 | 200 |

### 3.6 组件补丁（字形缓存 / 图鉴）

| 常量 | 含义 | ARM64 |
|---|---|---|
| `OFF_GLYPH_CACHE_SIZE_INSN` | 字形缓存尺寸选择的指令位置 | `0x1713408` |
| `OFF_GLYPH_CACHE_PAGE_LIMIT` | 图集页数上限 | `0x1713428` |
| `OFF_GLYPH_CACHE_CTOR` | 缓存构造（写 size） | `0x17F3560` |
| `OFF_GLYPH_CACHE_CLEAR` / `_REBUILD` | 清 / 重建字形数据 | `0x1714090` / `0x1714098` |
| `OFF_EATEXT_GLOBAL_GET` | 取 EAText 全局 | `0x1713288` |
| `OFF_ALMANAC_WIDGET_DEPRESS` | 图鉴按钮按下 | `0x86EC40` |
| `OFF_ALMANAC_CHOOSER_SELECT` | 图鉴条目选择（翻页） | `0x869258` |

> `OFF_GLYPH_CACHE_SIZE_SEL`（`0x1713384`）是**旧位置**，保留仅供对照。
> 补丁必须打在 `OFF_GLYPH_CACHE_SIZE_INSN` 上——早前打错位置会导致
> 启动闪退。

---

## 四、iOS 侧（`iOS/include/offsets_ios.h`）

iOS 的地址体系与 Android **没有一处相同**——那是两个不同的二进制。
但表的**结构与命名完全对应**，方便对照。

### 4.1 设置页链

| 常量 | 含义 | iOS 偏移 |
|---|---|---|
| `OFF_SettingsInit` | 设置页初始化（全部 Tab 在此创建） | `0x756ae4` |
| `OFF_SettingsCreate` | Tab 创建 | `0x757c18` |
| `OFF_SettingsDispatch` | Tab 点击 / 事件分发 | `0x759dd4` |
| `OFF_SettingsDataSharing` | 子页面创建 | `0x758a94` |
| `OFF_SettingsPageShow` | 页面显示 / 刷新 | `0x75a0e4` |
| `OFF_SettingsSetContent` | 写入 content 并触发重排 | `0x758268` |
| `OFF_SettingsCheckboxClick` | checkbox 点击处理 | `0x75adb4` |
| `OFF_SettingsDirtyRefresh` | dirty 标志检查与重建 | `0x758930` |
| `OFF_SettingsWstringCtor` | wstring 构造 | `0x757b78` |
| `OFF_SettingsCheckboxCreate` | checkbox 行创建 | `0x758740` |
| `OFF_SettingsRowAttach` | 行挂到内容容器 | `0x757e58` |
| `OFF_SettingsStdStringCtor` | std::string 构造 | `0x1bfb4c` |
| `OFF_SettingsPromptMake` | 说明文案（prompt）创建 | `0x179c14` |

### 4.2 视角链

| 常量 | 含义 | iOS 偏移 |
|---|---|---|
| `OFF_BoardZoom` | 布局缩放与对齐 | `0x183a58` |
| `OFF_BoardZoom2` | 缩放下限 | `0x183b10` |
| `OFF_StreetDinos` | 街道恐龙生成 | `0x5eddb4` |
| `OFF_GetBoard` | 取当前 Board 指针 | `0x7f65ac` |
| `DISPLAYINFO_BOARD` | Board 指针在 DisplayInfo 内的偏移 | 2584 |
| `CONFIG_USE_HIGH_VIEW_ANGLE` | 视角开关存放字段 | 0xA86（2694） |

### 4.3 语言与本地化

| 常量 | 含义 | iOS 偏移 |
|---|---|---|
| `DISPLAYINFO_LANGMGR` | 语言管理器在 DisplayInfo 内的偏移 | 2104 |
| `LANGMGR_LOCALE_ID` | 当前语言 FourCC 在 LangMgr 内的偏移 | 508 |
| `OFF_G_Config / OFF_G_DisplayInfo` | 用户配置 / 显示信息指针全局 | `0x27eb870` |
| `OFF_PersistManager` | 持久化管理器指针全局 | `0x27f4ed0` |
| `OFF_PersistSave` / `OFF_PersistReadBool` | 读写 bool 配置 | `0x1ab9f44` / `0x1aba2dc` |

语言标识的 FourCC 常量（`LANG_ID_EN_US` / `LANG_ID_ZH_CN`）两端同值。

> iOS 侧的字符串资源是 **UTF-32LE**（每字符 4 字节），与 Android 不同。
> 直接按 ASCII 搜不到本地化键——这是 iOS 侧定位时最容易踩的坑。

### 4.4 静态跳板站点表

iOS 不在运行期改代码页，而是在打包期把函数入口改写成跳转桩。
需要打桩的站点列在 manifest 文件里（当前 22 个）：

```text
iOS/backup/v52_scanfixed/mFinal.txt
```

这份 manifest 也是 iOS 侧的「函数表」——每个站点是一个接入点。
站点含义见 [iOS/README 第五节](../iOS/README.md#五站点表)。

### 4.5 尚未定位的项

iOS 表里有一批常量为 `0`，使用处按平台分支屏蔽。它们不是「忘了填」，
而是**iOS 侧走了另一条实现路径**，不需要这些地址。
填错会让功能静默失效，所以保持 0 比填错更好。

---

## 五、怎么定位一个新的地址

> 下面描述的是**方法**，不是推荐工具链。选哪种工具都可以，
> 关键是最后能写进表里的是「可验证的数值」。

通用流程：

1. 确定要找的是哪个函数（从它的**可观察行为**倒推）
2. 在目标二进制里找到它
3. 核对它的**入参**与**访问的字段**是否与我们的假设一致
4. 写入表里，编译，实机验证

### 5.1 从行为倒推

不要从「某个函数」开始找，而是从**看得见的结果**开始往回追：

| 想找的东西 | 从哪往回追 |
|---|---|
| 设置页的 Tab 注册 | 原版 Tab（如 Build Version）的注册调用 |
| 设置页的事件分发 | 点击某个原版 Tab 后触发的分发逻辑 |
| 页面重建 | 改了某个选项后页面被重建的现象 |
| 配置读写 | 读原版 bool 配置项（Music / SoundFX）的调用点 |
| 本地化取值 | 任一带 `[...]` 文本键的控件创建处 |
| 语言表加载 | `LawnStrings` 文件名的构造链 |
| 资源读取 | `LoadPackage` 名的构造链（前缀 → 追加 → 分隔符归一） |

### 5.2 iOS 侧的补充手段

iOS 侧另有一套轻量工具，放在 `iOS/tools/`：

| 工具 | 用途 |
|---|---|
| `xref` / `xref2` | 字符串交叉引用扫描（快速找到谁引用了某个字符串） |
| `ptrfind` | 查「谁存放了这个地址的 8 字节指针」 |

配合 `llvm-objdump` 全量反汇编，可以直接在设备上完成定位，不需要桌面工具。

### 5.3 核对与验证

找到候选地址后，**必须**核对这几点再写进表：

- **入参数量与类型**是否与我们调用处一致（尤其 ARM64 的 `x8` 隐式返回约定）
- **它访问的字段偏移**是否与表里的字段值对得上
- **指令形态**是否允许改写（ARM64：入口第 1 条不能是 PC 相对寻址；
  ARM32：目标必须是 ARM 模式、地址需为偶数）
- 填入后**实机验证**：进关卡、打开设置页、触发目标功能，确认无崩溃

### 5.4 两条硬性约束

**ARM32 与 ARM64 的偏移不可套用。** 指针宽度 4 vs 8 字节导致结构体字段
整体前移，必须各自独立核对。

**实现代码里禁止出现裸地址字面量。** 所有地址必须先在偏移表里定义，
再由代码引用。否则换版本时无法一次性找全。

---

## 六、常见问题

| 现象 | 多半是 |
|---|---|
| 启动就崩，崩在 hook 安装阶段 | 地址填错，或该地址不是函数入口 |
| 设置页里没有新 Tab | 锚点 Tab id 不对，或 createTab 的入参约定理解错了 |
| 页面能开但控件位置全乱 | 结构体字段偏移不对（常见于跨架构套用） |
| 改一个字段后游戏行为异常 | 写到了只读字段或相邻字段 |
| iOS 上打桩后站点全失效 | gate 区位置或站点偏移不对，先看 `gate integrity check` |
| iOS 上 iOS 字符串搜不到 | 忘了 iOS 用 UTF-32LE |

更细的排查步骤见 [日志系统](日志系统.md)。

---

## 七、相关文件

| 文件 | 内容 |
|---|---|
| `jni/offsets.h` | Android 侧地址与偏移（ARM64 + ARM32 双段） |
| `iOS/include/offsets_ios.h` | iOS 侧地址与偏移 |
| `iOS/port/include/offsets.h` | 垫片：让共享源码在 iOS 上找到上表 |
| `iOS/FINDINGS.md` | iOS 侧的定位过程与实测记录 |
| `iOS/HANDOFF.md` | iOS 侧的环境、约定与调试手段 |
| `iOS/backup/v52_scanfixed/mFinal.txt` | iOS 静态跳板站点表 |

每一项常量的依据都写在偏移表对应行的注释里，改动时请同步更新注释。