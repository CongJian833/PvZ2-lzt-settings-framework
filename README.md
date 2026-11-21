# lzt-settings-framework

> **Plants vs. Zombies 2** 设置界面扩展框架 —— 向游戏设置页注入自定义模块，
> 内置**视角**与**语言**两个模块。
>
> 仓库：<https://github.com/CongJian833/PvZ2-lzt-settings-framework>
>
> 平台文档：**Android（本文件）** ｜ [iOS](iOS/README.md)

两端共享同一套框架源码，差异只在平台层与接入方式：

| | Android | iOS |
|---|---|---|
| 注入物 | `libsettingsframework.so`（置于 APK 的 `lib/<abi>/`） | `liblztsettingsframework.dylib`（置于 `.app/Frameworks/`） |
| 接入时机 | 运行时改写代码入口 | 打包期写入跳转桩，运行时不改代码页 |
| 交付形态 | APK | IPA |
| 架构 | arm64-v8a + armeabi-v7a | arm64 |
| 最低系统 | Android 7.0（API 24） | iOS 11.0 |

本文件只讲 Android 侧；框架机制（Tab 注册、页面构建、本地化刷新）两端一致，
iOS 侧的实现差异见其文档。

---

## 一、这是什么

框架运行在游戏进程内，向内给原版设置页加一个新的 Tab，页面内容由业务模块
自己声明；向外按需修正相机与布局字段。

```text
设置界面
├── Music / SoundFX
├── Notifications
├── Build Version          ← 锚点
│   ├── 语言               ← 本框架注入
│   └── 视角               ← 本框架注入
└── Settings Help
```

特点：

- **声明式接入**：模块只声明「页面长什么样」和「点了做什么」，Tab 注入、控件
  id 分配、页面重建、持久化读写由框架承担。
- **不依赖外部框架**：不需要 Root、Xposed 或额外 Java 组件；集成后就是一个
  普通的、通过 `System.loadLibrary` 加载原生库的 APK。
- **不改游戏内容**：不修改关卡数据、不提供游戏本体、不解锁官方付费内容。

---

## 二、包含的模块

| 模块 | 能力 |
|---|---|
| 视角 | 在设置页注入「视角」Tab，切换高/低视角；高视角下恢复原生缩放比，宽屏设备按屏占比对齐棋盘左缘与选卡居中 |
| 语言 | 运行时切换游戏语言，支持自定义语言表外置加载，已加载文本随切换即时刷新 |
| 通用 | 组件补丁层：字形渲染缓存尺寸统一、图鉴翻页缓存刷新 |

---

## 三、开始使用

### 1. 配置地址表

所有与游戏版本相关的地址与字段偏移集中在一个头文件里，双架构分段维护：

```cpp
// jni/offsets.h — ARM64 段
constexpr uintptr_t OFF_BoardZoom   = 0xADEB80;  // BoardLayout_ApplyZoom
constexpr uintptr_t OFF_BoardZoom2  = 0xADEDE0;  // BoardZoom2
constexpr uintptr_t OFF_SettingsCreate = 0xA4D79C;
// ... 其余常量见文件内注释
```

当前配置对应国际版 9.8.1。换版本时只需重新核对这一个文件；实现代码中不允许
出现裸地址字面量。

偏移表的分类说明与「怎么定位一个新地址」见
[docs/offsets-guide.md](docs/offsets-guide.md)。

### 2. 编译

需要 Android NDK（r26b 或更新）。

```bash
ndk-build NDK_PROJECT_PATH=. \
  APP_BUILD_SCRIPT=jni/Android.mk \
  NDK_APPLICATION_MK=jni/Application.mk
```

产物：

```text
libs/arm64-v8a/libsettingsframework.so
libs/armeabi-v7a/libsettingsframework.so
```

仓库自带 `build_variants.py`，一次产出正式版与 Debug 版两套库。两种模式用同一
份代码，差别只是编译期开关 `kDebugMode`：Debug 版额外输出相机轨迹、字段快照
等诊断日志。日常使用与对外交付用正式版，收集问题日志时再切 Debug 版。

```bash
python build_variants.py --ndk <NDK目录>
```

### 3. 集成与打包

把 `.so` 放进 APK 的 `lib/<abi>/`，并在应用启动时加载：

```java
// Application.attachBaseContext 中
System.loadLibrary("settingsframework");
```

之后是常规的 APK 重打包流程：合并资源与库、对齐、签名。

```text
原 APK
  ├─ 解包 → 注入 loadLibrary("settingsframework") → 重编译 → 取出 dex
  ├─ 合回原 APK：原包 + 新 dex + libsettingsframework.so（双架构）
  ├─ 对齐（页面对齐 .so，提升加载与解压效率）
  └─ 签名（v1 + v2 + v3）
```

仓库内的打包脚本 `tools/repack.py` 把上述步骤串成一条命令，工具路径与签名
信息全部由参数传入，不写死在本机路径：

```bash
python tools/repack.py --src-apk 原版.apk \
  --keystore 我的签名.jks --keystore-pass <密码> \
  --key-alias <别名> --key-pass <密码> --out 输出.apk
```

### 4. 功能开关

仓库根目录的 `feature_flags.conf` 是所有可开关功能的**唯一声明处**，纯数据，
不含构建逻辑。构建脚本读它，把每个开关映射成编译期常量后再编译，源码里的
开关状态始终与配置文件一致。

```ini
[components]
glyph_cache_size  = true    # 字形渲染缓存统一为 2048
glyph_cache_flush = true    # 图鉴翻页时清 + 重建字形缓存

[features]
news_localization    = true    # News 页自定义语言短码兜底
fast_read_channel    = true    # 语言表支持 CDN 明文 json 通道
empty_lang_page_fallback = false  # 无可用语言时是否仍创建语言 Tab
```

---

## 四、框架：写一个自己的模块

模块 = 一张 Tab + 一个页面构建回调 + 一组状态读写。

```cpp
#include "settings/settings_framework.h"

// 1) 声明页面：调用顺序即控件在页面上的排列顺序
static void on_build_page(lzt_settings::PageBuilder& b) {
    b.add_prompt(L"[MY_MODULE_HINT]");
    lzt_settings::CheckboxDesc desc{};
    desc.labelKey = L"[MY_MODULE_ENABLE]";
    desc.initialState = my_state_read();
    desc.onClick = [](bool newState) { my_state_write(newState); };
    b.add_checkbox(desc);
}

// 2) 注册模块（纯登记，不触碰游戏内存；必须在 engine_install 之前）
static bool my_module_init() {
    lzt_settings::ModuleDesc desc{};
    desc.titleKey = L"[MY_MODULE_TITLE]";
    desc.anchorTabId = SETTINGS_BUILDVERSION_ID;  // 插到 Build Version 之后
    desc.slotAfterAnchor = 3;                     // 锚点后第三位
    desc.onBuildPage = &on_build_page;
    return lzt_settings::register_module(desc);
}
```

宿主在游戏库就绪后调用一次 `engine_install()`，引擎完成 id 定案并装好设置页
的三个入口；没有任何模块注册时，它一个 hook 都不装。

### 线程与回调约束

- `register_module`：只在 `engine_install()` 之前调用。
- `onBuildPage` / 点击回调：都在游戏 UI 线程触发。
- 点击回调里**只写业务状态**。页面刷新由引擎置脏后在安全点异步重建，重建时
  会重新执行 `onBuildPage`，所以随状态变化的文案会自然跟着更新。

### 常用接口

| 接口 | 用途 |
|---|---|
| `register_module(desc)` | 注册模块 |
| `engine_install()` | 安装 hook 并定案 id（幂等） |
| `refresh_all_localized()` | 统一本地化刷新入口 |
| `register_refresher(fn)` | 注册语言切换刷新器 |
| `register_engine_ready(fn)` | 引擎与基址就绪后的回调 |
| `config::query_bool / save_bool` | 不占字段位的 bool 读写 |
| `config::query_radio / save_radio` | 单选组读写（只写变动项） |

### 本地化刷新层

语言切换后，已加载控件的文本要跟着变。这一层把「哪些控件持有本地化文本、
用哪个回填器」收敛成一张按宿主类型索引的注册表：控件创建时登记快照，切换时
按类型回填；游戏已接管为动态文本的控件（当前文本 ≠ 捕获快照）会被跳过。

新增一类需要刷新的界面 = 加一个类型 + 一个回填器 + 一组地址常量，
调度逻辑不动。

---

## 五、语言模块

### 语言表来源

语言 Tab 里出现哪些语言，由一份「语言表」决定。框架按优先级依次尝试三个来源，
命中即用，全部失败则节流 3 秒后重试：

| 优先级 | 来源 | 形态 |
|---|---|---|
| 1 | CDN 目录 `LANGUAGETYPES.rton` | 游戏自身的二进制表格式 |
| 2 | CDN 目录 `LANGUAGETYPES.json` | 明文 JSON（由 `fast_read_channel` 控制） |
| 3 | 数据包内 `packages/LANGUAGETYPES.rton` | 游戏内置表的兜底 |

外置优先、内置兜底。CDN 目录名形如 `CDN.2.0`，文件名要求**全名精确匹配**
（大小写不敏感），`LANGUAGETYPES.json.bak` 之类的变体不会被读取。

设备上的位置：

```text
/sdcard/Android/data/<包名>/files/No_backup/CDN.x.x/LANGUAGETYPES.rton
/sdcard/Android/data/<包名>/files/No_backup/CDN.x.x/LANGUAGETYPES.json
```

### 自定义语言表

自己写一份语言表即可增减语言。最小示例：

```json
{
  "#comment": "Make new languages here!",
  "version": 1,
  "objects": [
    {
      "objclass": "LanguageType",
      "objdata": {
        "TypeName": "language_english",
        "LocalizedName": "[LANGUAGE_ENGLISH]",
        "LawnStringsType": "en-us",
        "Enabled": true
      }
    },
    {
      "objclass": "LanguageType",
      "objdata": {
        "TypeName": "language_simplified_chinese",
        "LocalizedName": "[LANGUAGE_SIMPLIFIED_CHINESE]",
        "LawnStringsType": "zh-cn",
        "Enabled": true
      }
    }
  ]
}
```

字段含义：

| 字段 | 必填 | 说明 |
|---|---|---|
| `objclass` | 是 | 固定 `"LanguageType"`，其他取值会被忽略 |
| `LocalizedName` | 是 | Tab 中显示的**游戏文本键**，需在游戏文本表中有对应条目 |
| `LawnStringsType` | 是 | 语言标识（`zh-cn`、`en-us`…），同时是语言包文件名后缀 |
| `Enabled` | 否 | `false` 时不在 Tab 中展示，缺省视为 `true` |
| `TypeName` | 否 | 仅作可读性标注，解析时跳过 |

约定：

- 语言标识统一转小写、`_` 换 `-`（`EN_US` → `en-us`）；归一化后只允许
  字母、数字与 `-`，不合规的条目直接丢弃。
- 最多 16 条启用项，超出部分截断。
- 表里没有 `en-us` 时，引擎自动在首位补一个默认英语项。
- 归一化后重复的语言标识，后出现者覆盖前者，位置保持不变。
- 自定义语言需要在语言包里补上对应文本条目，否则该行会显示为原始键名。

### 可见性与持久化

- 默认只有在**可用来源存在且启用语言多于一种**时才创建语言 Tab；
  把 `empty_lang_page_fallback` 置 `true` 可改为始终创建。
- 每个语言对应一条独立配置（`lzt.lang.<locale>`），单选语义：唯一为真的那条
  即当前语言，全为假时回落 `en-us`。
- 同时维护一份 `files/LanguageFlag` 标志文件，作为启动期的状态来源。

### 运行期切换（arm64）

切换语言时按顺序执行：写入当前语言标识 → 优先尝试外置语言表（走游戏自身的
加载与生效链路）→ 未命中则从语言包直载 → 成功后统一回填已加载文本。

---

## 六、视角模块

视角部分采用「改数据源」而不是「拦截读取者」的策略：棋盘布局字段是所有相机
平移路径的共同数据源，直接改字段可以覆盖全部读取路径；若只拦截坐标查询
函数，目标展示、选卡平移等流程会绕过它。

调用关系大致如下：

```text
游戏上层调用
  ├─ BoardLayout_ApplyZoom
  │    ├─ 前置：board[280] = 1.0（高视角下恢复原生缩放比）
  │    └─ 后置：按屏占比判定 → 调整 board[270/284/285] 的对齐量
  └─ BoardZoom2
       └─ 后置：board[280] = 1.0（仅高视角）
```

设备判定按当前屏占比（宽 / 高）取值：

- **高视角**：屏占比 > 1.69335 时执行左对齐与选卡居中。
- **低视角**：保留原生缩放，仅在屏占比 > 2.16666 的超宽屏上做左对齐。

比例读取失败时保守执行对齐，避免漏修；这是兜底，不是设备分类结果。

状态写入游戏自身的偏好存储，页面每次构建时重新读取，因此提示文案随视角
自动更新。

---

## 七、组件补丁

组件补丁修的是游戏内的通用组件，被多个模块共用，既不属于框架引擎也不属于
任何业务模块，所以单独成层。

| 名称 | 作用 |
|---|---|
| `glyph_cache_size` | 字形渲染缓存尺寸在所有设备统一为 2048，小屏不再缺字 |
| `glyph_cache_flush` | 图鉴翻页或切换条目时主动清 + 重建字形缓存，避免复用脏缓存 |

两条补丁都做原始字节校验：写入前先确认目标位置的指令与预期一致，不一致就
跳过并记一条日志，绝不盲目改写。

---

## 八、仓库结构

```text
PvZ2-lzt-settings-framework/
├── jni/                                 # 原生源码（Android 与 iOS 共享）
│   ├── Android.mk / Application.mk      # 构建描述（模块名、ABI、平台）
│   ├── lzt_settings_framework.cpp       # 主实现：基址解析、hook 安装
│   ├── lzt_settings_framework_config.h  # 编译期开关（构建模式）
│   ├── lzt_core.cpp / .h                # 基址解析、日志、包名与目录
│   ├── lzt_hooks.h                      # 双架构 hook 门面
│   ├── offsets.h                        # 全部地址与字段偏移（双架构分段）
│   ├── view_angle_module.cpp / .h       # 视角模块
│   ├── language_module.cpp / .h         # 语言模块
│   ├── language_types*.cpp              # 语言表解析（json / rton）
│   ├── resource_file.cpp / .h           # 外置文件通道 / 包内查找
│   ├── cdn_load_rton.cpp                # 外置语言表加载与清单改写
│   ├── screen_bindings.cpp / .h         # 屏幕本地化绑定
│   ├── settings/                        # 框架层
│   │   ├── settings_framework.cpp / .h  # Tab 注入、ID 分配、事件路由
│   │   ├── settings_widgets.cpp / .h    # 构建清单 → 游戏控件
│   │   ├── localizer.cpp / .h           # 类型化本地化注册表
│   │   ├── table_loader.cpp / .h        # 数据表来源调度
│   │   └── game_abi.cpp / .h            # 游戏 ABI 工具
│   ├── components/                      # 组件补丁层
│   └── And64InlineHook.cpp / .hpp       # 上游 inline hook 库（MIT）
├── iOS/                                 # iOS 侧：构建、打包、平台实现
├── docs/                                # 偏移量指南、日志系统说明、语言表模板
├── tests/                               # 静态检查脚本
├── tools/repack.py                      # APK 重打包工具
├── build_variants.py                    # 双版本构建脚本
├── feature_flags.conf                   # 功能开关声明处
├── LICENSE / NOTICE
└── README.md
```

---

## 九、架构差异

| 项目 | arm64-v8a | armeabi-v7a |
|---|---|---|
| 接入方式 | 标准 inline hook | 自实现跳转改写 |
| 视角 / 设置页注入 | 完整 | 完整 |
| 语言热重载 | 支持 | 仅保存选择，重启生效 |
| 本地化即时刷新 | 支持 | 不刷新 |

armeabi-v7a 上的目标函数为 ARM 模式，函数地址需为偶数，由 `BLX` 完成指令集
切换，禁止把地址第 0 位置 1。

---

## 十、日志与排查

日志系统的完整说明（两端输出通道、日志种类、崩溃记录、排查对照表）见
[docs/日志系统.md](docs/日志系统.md)。本节只讲 Android 侧。

运行日志同时输出到 logcat（tag `settingsframework`）和本地文件：

```text
/sdcard/Android/data/<包名>/files/settingsframework.log
```

每次启动会把上一份日志改名归档（保留最近 5 份），便于对比前后两次运行。

常用关键字：

| 关键字 | 含义 |
|---|---|
| `applyHooks: base = 0x...` | 定位到主库基址并开始安装 |
| `[SFW]` | 设置框架日志（注册、id 分配、Tab 注入、页面构建） |
| `LangTable: source=` | 语言表来源命中结果（`cdn-rton` / `cdn-json` / `package-rton`） |
| `Language config save:` | 语言选择的写入与回读 |
| `Lang hot_reload: begin/end` | 语言热重载流程边界 |
| `LangOverride:` | 启动期语言标识的三条判定路径 |
| `H0 BoardZoom2` / `BoardZoom ALIGNED` | 视角相关逻辑生效 |
| `[CMP]` | 组件补丁的字节校验结果 |

排查思路：

| 现象 | 先看 |
|---|---|
| 完全没有日志 | 是否执行了 `System.loadLibrary`，`lib/<abi>/` 里是否放了对应 `.so` |
| 有基址但功能没反应 | 当前游戏版本的地址是否与 `offsets.h` 一致 |
| 设置页一进去就崩 | 设置页相关地址与字段偏移是否来自同一游戏版本 |
| 没有语言 Tab | 是否配置了语言表来源、启用语言是否多于一种 |
| 切换语言后文本没变 | 语言热重载与本地化刷新均为 arm64 实现 |
| 缺少字形 / 缺字 | 组件补丁是否生效，查 `[CMP]` 行 |

反馈问题时请附上：游戏版本、屏占比、CPU 架构、构建模式、复现步骤，以及出问题
前后的日志片段。日志里可能含包名与内存地址，公开前请自行脱敏。

---

## 十一、已知限制

- 地址配置只对国际版 9.8.1 成立，游戏更新后需重新核对。
- 依赖游戏内部的函数与结构布局，无法保证与其他修改方案同时使用。
- 语言热重载与本地化即时刷新仅 arm64 支持。
- 屏占比判定面向横屏；分屏、自由窗口或运行时分辨率变化需要重新初始化。
- 本项目只提供源码与构建说明，不提供游戏安装包、游戏主库或签名材料。

---

## 十二、许可

本项目的原创代码、修改与文档采用
[PolyForm Noncommercial License 1.0.0](LICENSE)，仅允许非商业用途；
受其约束的范围、排除材料与再分发要求见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
上游项目与 And64InlineHook 继续适用各自的 MIT 许可。

游戏本体、游戏资源与商标属于 Electronic Arts / PopCap，不在本项目授权材料内。
本项目与 EA、PopCap 无隶属、认可或赞助关系。

### 与官方 Mod 政策的关系

EA 公布的 [Plants vs. Zombies Mod Policy](https://help.ea.com/en/articles/plants-vs-zombies/mod-policy/)
允许为 PvZ 系列制作并分享 Mod，要求包括：非商业、免费提供、不分发需购买才能
游玩的游戏本体、不发布在 App Store / Google Play / Steam 等官方分发平台、不得
绕过官方付费或访问限制、内容不得侵权。

本项目是一个**非官方的 Mod**，按上述政策发布：

- 只提供框架源码，**不分发任何游戏文件**；使用者需自行准备合法获得的游戏。
- 非商业、免费提供；不接受与功能绑定的付费。
- 不发布于官方分发平台，不使用官方标识做推广。
- 不含解锁未发布内容、绕过付费墙或影响官方服务器的内容。

如官方政策更新，以官方页面为准。

---

## 十三、致谢

作者：**落筆从生簡**（BiliBili）

鸣谢：**雪竹子池**

感谢各位参与功能实现、运行测试与提出改进建议的伙伴。

再分发时请一并保留 `LICENSE`、`NOTICE` 及其中的 Required Notice。

---

## 十四、免责声明

本项目仅供学习与技术研究使用。使用者自行承担使用风险，作者不对任何因使用
本项目造成的后果负责。请遵守当地法律法规与游戏相关条款。
