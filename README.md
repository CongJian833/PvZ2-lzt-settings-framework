# lzt-settings-framework

> **PvZ 2** Android 设置界面扩展框架 + 视角模块 + 语言模块
>
> 通过 inline hook `游戏库文件`，提供通用的设置界面扩展框架：注册式多模块
> Tab 注入、声明式页面构建（PageBuilder）、自动 ID 分配与冲突校验、
> dirty 异步重建等机制全部由框架引擎承担，业务模块只声明"页面长什么样"
> 与"点击后做什么"。框架之上另有一层类型化本地化服务（Localizer），
> 让已加载的设置项与主界面文本在同一接口下随语言切换实时刷新。
>
> 内置两个接入范例：**视角**模块（切换高低视角缩放并修复宽屏设备黑边）
> 与**语言**模块（运行时切换游戏语言，支持外置语言表热重载）。
>
> 支持 **ARM64**（arm64-v8a）与 **ARM32**（armeabi-v7a，目标函数为 ARM 模式）
> 双架构；相机/视角与框架注入功能对等，语言热重载与本地化刷新为 ARM64 实现
> （ARM32 降级为"保存选择 + 重启生效"），详见[架构差异](#架构差异)。
>
> 仓库：<https://github.com/CongJian833/PvZ2-lzt-settings-framework>

本项目原名 **LawnZoomTab**，现名 **lzt-settings-framework**（仓库
`PvZ2-lzt-settings-framework`），由
[PvZ2-MaxZoomHook](https://github.com/CongJian833/PvZ2-MaxZoomHook)
扩展而来。CongJian833 为本项目独立创作的源代码、原创修改、文档及其他受著作权
保护的材料采用 [PolyForm Noncommercial License 1.0.0](LICENSE)，仅允许非商业
用途；这些材料的源代码、编译形式和修改形式均在许可范围内。上游代码与
And64InlineHook 继续适用各自的 MIT License，游戏本体、`游戏库文件`、游戏资源与
商标不属于本项目授权材料。完整适用范围、来源和授权边界见 [LICENSE](LICENSE) 与
[NOTICE](NOTICE)。由于附加了非商业限制，本项目属于源码可用项目，不使用 OSI
定义下的“开源许可证”表述。

## 项目概览

lzt-settings-framework 直接运行在游戏进程中。它做两件事：向内注入
新的设置 Tab（及其交互与持久化），向外改写 Board 布局与相机字段以修正视角
表现。项目不修改关卡数据，不提供游戏本体，也不依赖 Xposed、Magisk 模块或额外
Java 框架；集成后的 APK 通过 `System.loadLibrary("settingsframework")` 加载原生库。

为减少版本适配成本，所有游戏函数地址和结构字段偏移集中在 `jni/offsets.h`，
ARM64 与 ARM32 分别维护。

### 阅读导航

- 首次使用：从[快速开始](#快速开始)和[兼容性](#兼容性)开始

- 只想加一个设置项：阅读[设置框架引擎](#设置框架引擎)与[如何写一个模块](#如何写一个模块)

- 适配其他游戏版本：阅读[偏移量指南](docs/offsets-guide.md)

- 了解某个模块：见[模块：视角切换](#模块视角切换)与[模块：语言切换](#模块语言切换)

- 编写语言表：阅读[编写语言表（LANGUAGETYPES）](#编写语言表languagetypes)，模板见 [docs/LANGUAGETYPES.json](docs/LANGUAGETYPES.json)

- 了解组件补丁：见[组件补丁](#组件补丁)与 [jni/components/README.md](jni/components/README.md)

- 排查问题：查看[日志与故障排查](#日志与故障排查)

- 使用和分发前：阅读[架构差异](#架构差异)、[已知限制](#已知限制)与[许可证](#许可证)

## 功能

### 框架能力

| 能力          | 说明                                                                     |
| ----------- | ---------------------------------------------------------------------- |
| 注册式 Tab 注入  | 模块用 `ModuleDesc` 声明标题键、图标、锚点位置与构建回调，引擎统一注入                             |
| 声明式页面构建     | `PageBuilder::add_prompt / add_checkbox / add_raw_widget` 按调用顺序即布局顺序   |
| 自动 ID 分配    | Tab id 与控件 id 共用游戏分发命名空间，由引擎自动分配并做冲突校验                                 |
| dirty 异步重建  | 点击回调返回后引擎统一置脏标记并在安全点重建，回调内禁止同步重建                                       |
| 控件物化        | `settings_widgets` 负责把构建清单变成游戏真实控件（双架构底层）                              |
| 条件建 Tab     | `ModuleDesc.shouldCreate` 返回 false 时跳过该模块 Tab，框架不硬编码任何模块               |
| 状态持久化工具     | bool 配置（带/不带缓存字节）与单选组读写，封装游戏偏好后端                                       |
| 本地化统一刷新     | Localizer 类型化注册表：控件创建时 capture，切语言时按类型回填                               |
| 通用注册点       | `register_refresher`（参与统一刷新遍历）与 `register_engine_ready`（引擎就绪后装自身 hook） |
| 数据表来源调度     | TableLoader 按优先级尝试多个来源，命中即用，失败节流 3 秒并支持强制重测                            |
| 双架构 Hook 门面 | `lzt_hooks::install` 统一 ARM64（And64InlineHook）与 ARM32（自实现）             |

### 内置模块

| 模块 | 功能        | 说明                                                               |
| -- | --------- | ---------------------------------------------------------------- |
| 视角 | 视角设置 Tab  | 在设置界面注入“视角”Tab，可切换高视角/低视角并持久化                                    |
| 视角 | 高视角缩放     | 强制 `board[280]=1.0`，恢复 1.0 缩放比                                   |
| 视角 | 左对齐修复     | 左平移至草坪左侧时，屏幕左边缘与棋盘左边缘对齐                                          |
| 视角 | 选卡居中      | 选卡关卡左平移时，相机居中对齐棋盘中央                                              |
| 视角 | 目标展示起点修正  | 低视角目标展示首帧位置修正（ARM32 v44 / ARM64 v42）                             |
| 视角 | 设备自适应     | 高视角宽高比 > 1.69335 时对齐；低视角仅在宽高比 > 2.16666 的超宽屏设备上左对齐               |
| 视角 | 无平移动作关卡补偿 | 通过 ShakeBoard action 触发相机重新读取对齐字段，覆盖砸罐子等关卡                       |
| 视角 | Hook 稳定性  | patch 存活监控 + 自动重装，Board 指针 watchdog 保底覆盖                         |
| 语言 | 动态语言表     | 按优先级读取 LANGUAGETYPES（CDN rton → CDN json → 数据包 rton），生成语言 Tab 选项 |
| 语言 | Tab 可见性控制 | 无可用来源或仅一种语言启用时不创建语言 Tab（可配置回退为空白页）                               |
| 语言 | 选择持久化     | 每语言一个 bool 配置（`lzt.lang.<locale>`）实现单选语义，并双写标志文件                 |
| 语言 | 运行时热重载    | 外置语言表优先 apply，未命中回退数据包直载，成功后回填全部已加载文本                            |
| 日志 | 持久化       | 同时输出到 logcat 和本地文件，便于离线排查                                        |

## 快速开始

编译本模块只需要三样东西：

1. **IDA Pro**（用于按自己的游戏版本了解实现原理、核对偏移）
2. **Android NDK**（推荐 r26b 或更新）
3. **目标 APK**（内含 `游戏库文件`）

### 1. 填写偏移量

所有与游戏版本强相关的地址都集中在 [`jni/offsets.h`](jni/offsets.h)。
项目默认偏移基于国际版 9.8.1；换版本时只需修改该文件：

```cpp
// jni/offsets.h — ARM64 段
constexpr uintptr_t OFF_BoardZoom  = 0xADEB80;  // BoardLayout_ApplyZoom
constexpr uintptr_t OFF_BoardZoom2 = 0xADEDE0;  // BoardZoom2
// ...其余常量见文件内注释
```

详细定位方法见 [docs/offsets-guide.md](docs/offsets-guide.md)。实现代码中
不允许出现地址字面量，新增地址必须放入 `offsets.h`。

### 2. 编译

```bash
# 编译双架构
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
```

产物位于：

```text
libs/arm64-v8a/libsettingsframework.so
libs/armeabi-v7a/libsettingsframework.so
```

原生库模块名（`jni/Android.mk` 的 `LOCAL_MODULE`）、产物文件名与 APK 内
`System.loadLibrary()` 参数三者必须一致：仓库名为 `PvZ2-lzt-settings-framework`，
对外项目名为 `lzt-settings-framework`，库名为 `settingsframework`。

#### 构建模式

项目提供正式版与 Debug 版两种模式，差异是 `jni/lzt_settings_framework_config.h`
中的编译期开关 `lzt_config::kDebugMode`：正式版为 `false`，Debug 版为
`true`。Debug 版额外启用详细相机轨迹、字段快照、页面阶段日志与只读诊断探针。
切换模式后重新编译即可；发布源码始终以 `false` 交付。

| 项目           | 正式版       | Debug 版        |
| ------------ | --------- | -------------- |
| `kDebugMode` | `false`   | `true`         |
| 功能 Hook      | 完整启用      | 完整启用           |
| 高频诊断日志       | 裁剪        | 启用             |
| 纯诊断 Hook     | 不安装       | 按架构安装          |
| 只读诊断探针       | 不执行       | 执行并记录          |
| 使用场景         | 日常使用与最终交付 | 偏移定位、相机轨迹、异常复现 |

Debug 版不会改变核心对齐公式或设置行为，它只增加诊断路径。向普通用户交付时应使用
正式版；收集问题日志时再临时使用 Debug 版，并避免长期保留高频日志。

#### 功能开关（feature\_flags.conf）

仓库根目录的 [`feature_flags.conf`](feature_flags.conf) 集中声明各可开关功能（组件
补丁、语言 / 本地化特性等）的启用状态，每个开关一行、可带行尾注释：

```ini
[components]
glyph_cache_size  = true    # 强制字形渲染缓存 2048
glyph_cache_flush = true    # 图鉴翻页时清 + 重建字形缓存

[features]
fast_read_channel = false   # CDN-json 快速读取通道（开发期提速用）
```

该文件是**纯数据**，自身不含任何构建逻辑。构建脚本读取它，把每个开关映射到
`jni/lzt_settings_framework_config.h` 的同名编译期常量后再编译；因此切换功能只需
改这一处，构建结束后源码开关会恢复为配置文件声明的状态。

| 开关                         | 对应编译期常量                        | 默认      |
| -------------------------- | ------------------------------ | ------- |
| `glyph_cache_size`         | `kEnableGlyphCacheSize2048`    | `true`  |
| `glyph_cache_flush`        | `kEnableAlmanacGlyphFlush`     | `true`  |
| `news_localization`        | `kEnableNewsLocalization`      | `true`  |
| `fast_read_channel`        | `kEnableFastReadChannel`       | `false` |
| `empty_lang_page_fallback` | `kEnableEmptyLangPageFallback` | `false` |

其中 `fast_read_channel`（对应 `kEnableFastReadChannel`，CDN-json 快速读取通道，
详见“模块：语言切换”）与构建模式**相互独立**：Debug 版不会自动开启该通道，
开发期需要时可单独置 `true`。
`kDebugMode` 则不属于功能开关，它由构建模式（正式版 / Debug 版）决定，不写入本文件。

### 3. 集成到 APK

```java
// 在 Application 的 attachBaseContext 中注入
System.loadLibrary("settingsframework");
```

把编译出的 `libsettingsframework.so` 放入 APK 的 `lib/<arch>/` 目录，重新打包并签名
（zipalign + apksigner）即可。

## 架构分层

```text
业务层    view_angle_module / language_module / screen_bindings
          （状态含义、文案、持久化策略、屏幕绑定全部在这里）
             │  注册 ModuleDesc / 调用框架 API
框架层    lzt_settings（settings_framework + settings_widgets + table_loader + game_abi）
          （hook 安装、ID 分配、事件路由、页面骨架、控件物化、生命周期防护）
          lzt_localizer（已加载本地化控件的类型化注册表与回填调度）
             │  引用地址常量
基础层    lzt_core（基址解析 / 日志 / 包名与 files 目录）+ lzt_hooks（双架构 hook 门面）
资源层    resource_file（CDN 文件通道 / RSB 包内查找）+ language_types(_rton) + cdn_load_rton
地址层    offsets.h（全部函数地址与字段偏移，双架构分段）
```

分层的实际收益是**框架内不出现任何具体屏幕语义**。举例来说，主界面
PersistentMessage 横幅的刷新实现位于 `screen_bindings.cpp`，它只向框架注册一个
刷新回调；框架遍历调用时并不知道这个回调在刷哪个界面。新增屏幕适配不需要改框架。

## 设置框架引擎

### 如何写一个模块

```cpp
#include "settings/settings_framework.h"

// 1) 声明页面：调用顺序即控件在页面上的排列顺序
static void on_build_page(lzt_settings::PageBuilder& b) {
    b.add_prompt(L"[MY_MODULE_HINT]");
    lzt_settings::CheckboxDesc desc{};
    desc.labelKey = L"[MY_MODULE_ENABLE]";
    desc.initialState = my_state_read();
    desc.onClick = [](bool newState) { my_state_write(newState); };  // 业务逻辑
    b.add_checkbox(desc);
}

// 2) 注册模块（纯入表，不触碰游戏内存；必须在 engine_install 之前）
static bool my_module_init() {
    lzt_settings::ModuleDesc desc{};
    desc.titleKey = L"[MY_MODULE_TITLE]";
    desc.anchorTabId = 6;          // 插入到原版 Build Version 之后
    desc.slotAfterAnchor = 2;      // 锚点后第二位（多位模块按此排序）
    desc.onBuildPage = &on_build_page;
    return lzt_settings::register_module(desc);
}
```

宿主在 `游戏库文件` 就绪后调用一次 `engine_install()`，引擎完成 ID 定案并安装
Settings 三件套 hook（createTab / dispatch / createPage）。没有任何模块注册时，
引擎不安装任何 hook。

### 线程模型与回调约束

- `register_module`：仅在 `engine_install()` 之前调用（宿主初始化线程）。

- `onBuildPage` / 点击回调：均在游戏 UI 线程触发。

- 点击回调**只写业务状态**；页面刷新由引擎的 dirty 异步重建自动完成，重建时会
  重新执行 `onBuildPage`，因此随状态变化的文案会自然更新。回调内同步重建是
  框架封死的错误路径。

### 常用 API

| API                                | 用途                                          |
| ---------------------------------- | ------------------------------------------- |
| `register_module(desc)`            | 注册模块（入表）                                    |
| `engine_install()`                 | 安装 hook 并定案 ID（幂等）                          |
| `refresh_all_localized()`          | 统一本地化刷新入口（Localizer + 页标题 + 业务刷新器）          |
| `refresh_page_title()`             | 设置页顶部标题原位刷新                                 |
| `register_refresher(fn)`           | 注册语言切换刷新器，`refresh_all_localized()` 按注册顺序遍历 |
| `register_engine_ready(fn)`        | 引擎就绪且基址可用后回调一次，供模块安装自身 hook（回调须自幂等）         |
| `config::query_bool / save_bool`   | 零字段占用的 bool 配置读写（直查偏好后端）                    |
| `config::query_radio / save_radio` | 单选组读写，内部先查旧项，只写变动的两项                        |
| `config::read_bool / write_bool`   | 带缓存字节的高频读版本（占用宿主一个空闲字节）                     |

### 本地化刷新层（Localizer）

`lzt_localizer` 把“哪些已加载控件持有本地化文本、用哪个回填器刷新”收敛为一张
类型化注册表：

- `capture(type, host, key, hasSnapshot, snapshot)`：控件**创建时**登记（由框架既有
  创建入口 hook 捕获），只收已解析的宽字符串键与文本快照。

- `refresh_all()`：语言切换后遍历注册表，按 `HostType` 分发回填。

`HostType` 现有三种：`TabRow`（设置行标题，串在 host/+184）、`Label`（0x2D8 标签，
`SetText(vt+752)`）、`RootTitle`（设置根视图顶部大标题）。新增屏幕适配 = 新增一个
枚举值 + 一个回填器 + 对应 offsets 常量，不需要改 `refresh` 的调度逻辑。

该层不 hook 高频统一读取口，只把它当取值函数调用，因此不引入 ABI 或性能新风险；
同时用“宿主当前文本 ≠ 捕获快照”判断游戏是否已接管为动态文本，避免误改。

## 模块：视角切换

“视角”模块是框架的首个接入范例，业务全部在
[`view_angle_module.cpp`](jni/view_angle_module.cpp)：Tab 声明与页面编排走框架，
相机与视角的实际改写逻辑留在主模块。

### 接入方式

模块用 `ModuleDesc` 声明一个 Tab，锚定原版 Build Version 之后第二位（第一位让给
语言模块）：

| 声明项               | 取值                                        |
| ----------------- | ----------------------------------------- |
| `titleKey`        | `[VIEW_ANGLE_TITLE]`                      |
| 图标                | 复用 Build Version 的正常态 / 选中态图标             |
| `anchorTabId`     | `SETTINGS_BUILDVERSION_ID`（Build Version） |
| `slotAfterAnchor` | 2                                         |
| `requestedTabId`  | 0（由引擎自动分配，实际为 33）                         |
| `onBuildPage`     | `build_page`——1 个 prompt + 2 个互斥 checkbox |

状态持久化用框架的 `config::read_bool / write_bool`，键为 `UseHighViewAngle`
（占用 `DisplayInfo` 内一个空闲字节作缓存）。页面每次构建都重新读取状态，所以
prompt 文案随视角切换自动更新，模块不需要自己刷新。

点击回调用的是旧版**无序号回调** `add_checkbox(label, initialState, onClick)`：
视角只有两个固定选项，不需要语言模块那样的按序号分发。

### 状态语义

| 状态             | 含义                                             |
| -------------- | ---------------------------------------------- |
| 高视角（`true`，默认） | 挂载 BoardZoom2 强制缩放 Hook，`board[280]` 恒被强制为 1.0 |
| 低视角（`false`）   | 卸载该 Hook，保留游戏原生低视角缩放；对齐修正 Hook 始终生效            |

初始化沿用历史版本的三重保险：配置对象未就绪期间按默认高视角工作且**不锁定缓存**，
就绪后由重试线程（60 次 × 500ms）重新读取真实配置，并同步 Hook 挂载状态——避免
启动期过早读取把用户选择覆盖为默认值。

### 相机改造原理

相机与视角部分采用“修改数据源”而非“拦截读取者”的策略：Board 字段是所有相机平移
路径的共同数据源，直接修改字段即可从源头影响所有读取者。旧方案试图拦截坐标查询
函数，但部分路径直接读取 Board 字段绕过查询函数，因此失败。

#### 主调用链

```
游戏上层函数
  (ARM64: sub_AD93E4 / ARM32: sub_75C9F8)
   │
   ├─ 1. 调用 BoardLayout_ApplyZoom  ← Hook 1
   │        pre-hook:  board[280] = 1.0
   │        原函数    → 计算 board[283~286]/270/284
   │        post-hook: 宽高比判定 → 修改 board[270/284/285]
   │                    低视角布局未就绪 → 武装 DEFER 延迟对齐
   │
   └─ 2. 调用 BoardZoom2            ← Hook 0
           原函数    → 计算 board[280~282]
           post-hook: board[280] = 1.0（强制高视角，仅高视角模式）
                      启动 watchdog 保底监控
```

设置界面另有独立 Hook 组：注入模块 Tab、处理 Tab 分发与页面构建。视角切换会动态
挂载 / 卸载 Hook 0：高视角强制 `scale=1.0`；低视角保留对齐修正但不强制 scale。

#### 设备判定

宽高比 `aspect = screenWidth / screenHeight` 按当前视角选择阈值：

- **高视角**：`aspect > 1.69335` → 执行左对齐与选卡居中；有效比值小于等于阈值时跳过

- **低视角**：`aspect > 2.16666` → 仅执行相机左对齐；有效比值小于等于阈值时跳过

- **异常回退**：`aspect <= 0`（尚未初始化或读取失败）→ 高低视角均保守执行对应对齐

低视角的主对齐、ARM32 方向表起点修正、ARM64 MoveBoard 动画起点修正和恐龙
位置补偿统一使用 `2.16666`，避免主画面与附属动画路径采用不同设备判定。

初始化三重保险：applyHooks 首次尝试 + 重试线程（60 次 × 500ms）+ hkBoardZoom
lazy init。读取失败时进入上述保守回退。

#### 低视角目标展示修正

- ARM32：方向表输出起点改写，保持目标展示首帧停留在可见对齐位

- ARM64：MoveBoard action 工厂起点改写，同一问题的对称实现

#### 稳定性设计

- **patch 监控**：前 30 秒 100ms 高频检查，之后 1s 低频，被清零自动重装

- **watchdog**：16ms 级监控 board\[280]，被篡改立即还原；检测到内存复用安全退出

- **DEFER**：低视角布局未就绪时后台等待，就绪后重算对齐

- **并发安全**：跨线程状态全部原子化，对齐组合操作由互斥锁串行化

详细原理见 `jni/lzt_settings_framework.cpp` 文件头注释。

## 模块：语言切换

### 语言表来源与优先级

| 优先级 | 来源                                 | 读取方式                                         |
| --- | ---------------------------------- | -------------------------------------------- |
| 1   | CDN 目录 `LANGUAGETYPES.rton`        | AsciiORM 二进制解码                               |
| 2   | CDN 目录 `LANGUAGETYPES.json`        | 严格最小 JSON 解析器（受 `kEnableFastReadChannel` 控制） |
| 3   | 数据包内 `packages\LANGUAGETYPES.rton` | 游戏 RSB 名查找（仅 ARM64）                          |

外置优先、内置兜底。CDN 文件名为**全名精确匹配（大小写不敏感）**，扩展名变体一律不匹配、不读取。

其中第 2 项（CDN-json 快速读取通道）由 `jni/lzt_settings_framework_config.h` 的
`kEnableFastReadChannel` 控制：**默认关闭**，此时该来源不参与尝试，正式版与 Debug 版
均不读取 CDN `LANGUAGETYPES.json`；开启后才纳入来源表（开发期以明文 JSON 快速调试）。
它与 `kDebugMode` 相互独立——Debug 版不会自动开启本通道。

解析结果统一产出：`LocalizedName` → checkbox 文本键、`LawnStringsType` → locale、
`Enabled` → 是否展示；最多 16 项；表内缺 `en-us` 时自动补首位。各来源的
尝试顺序、失败节流（3 秒）与强制重测由框架 `TableLoader` 统一调度，模块只提供
`fetch` / `parse` 回调。

### 编写语言表（LANGUAGETYPES）

语言 Tab 里出现哪些语言，完全由一份语言表决定。可直接复制模板
[`docs/LANGUAGETYPES.json`](docs/LANGUAGETYPES.json) 改成自己的语言清单：

```json
{
  "#comment": "Make new languages here!",
  "version": 1,
  "objects": [
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

顶层必须是一个含 `objects` 数组的对象；解析器只挑出 `objclass == "LanguageType"`
的条目（`#comment` / `version` 等其余键忽略），并从其 `objdata` 读取三个字段：

| 字段                | 必填 | 说明                                                                               |
| ----------------- | -- | -------------------------------------------------------------------------------- |
| `objclass`        | 是  | 固定 `"LanguageType"`；其他取值（或缺失）的对象被忽略                                              |
| `LocalizedName`   | 是  | 该语言在 Tab 中显示的**游戏文本键**（如 `[LANGUAGE_SIMPLIFIED_CHINESE]`），需在 LawnStrings 中存在对应条目 |
| `LawnStringsType` | 是  | 该语言的 locale（如 `zh-cn`），同时也是语言包 `LawnStrings-<locale>.rton` 的后缀名                  |
| `Enabled`         | 否  | `false` 时该语言不在 Tab 中展示；缺省视为 `true`                                               |
| `TypeName`        | 否  | 解析器跳过，仅作可读性标注                                                                    |

约定与限制：

- **locale 归一化**：解析时统一转小写、把 `_` 换成 `-`（`EN_US` → `en-us`）。归一化后必须是
  2\~15 个字符、仅含 `[a-z0-9-]`，否则该条目被丢弃（这一约束同时天然排除了 `../` 类路径注入）。

- **数量上限**：最多 **16** 条启用项，超出部分截断。

- **`en-us`** **兜底**：表中没有 `en-us` 时，引擎自动在首位补一个默认英语项（固定标签）。

- **重复 locale**：归一化后重复的条目，后出现者覆盖前者，位置保持在首次出现处。

- **显示文案**：Tab 文案取自 `LocalizedName` 指向的文本键，请确保该键在 LawnStrings 中已定义——
  自定义语言可在 `packages\LawnStrings-<locale>.rton` 里补上，否则该行会显示为原样键名。

把语言表放到设备的 CDN 目录即可被优先读取（外置优先、内置兜底）：

```text
/sdcard/Android/data/<包名>/files/No_backup/CDN.x.x/LANGUAGETYPES.rton
/sdcard/Android/data/<包名>/files/No_backup/CDN.x.x/LANGUAGETYPES.json   # 需 kEnableFastReadChannel
```

两种形态的内容语义一致、解析结果同构：`.rton` 是游戏的 AsciiORM 二进制形态（由官方导出
工具生成），`.json` 是明文形态（仅在 `kEnableFastReadChannel` 开启时参与尝试）。CDN 目录
以 `CDN.` 为前缀（如 `CDN.2.0`），文件名必须**全名精确匹配（大小写不敏感）**——
`LANGUAGETYPES.json.bak`、`.json.xx` 等扩展名变体一律不匹配、不读取。

### Tab 可见性

由 `jni/lzt_settings_framework_config.h` 的 `kEnableEmptyLangPageFallback` 控制：

- **关闭（默认）**：来源就绪且启用语言数 > 1 才创建语言 Tab，否则不创建、不进入
  后续流程。

- **开启**：始终创建（保留旧版空白页行为）。

Tab 创建晚于 constructor，因此建 Tab 前会强制重测一次来源——此时 `游戏库文件` 基址
已就绪，数据包兜底可读，避免误判“无来源”。

### 选择持久化

每种语言一个游戏 bool 配置，key 为 `lzt.lang.<locale>`，单选语义：唯一为 true 者为
当前语言，全 false 回落 `en-us`。同时保持 `files/LanguageFlag` 双写（启动层的真相
源）；旧版短标志（`zh` / `en`）在首次读取时迁移。

### 启动层（重启生效）

游戏语言管理器中当前语言的 FourCC 位于 `DisplayInfo + 0x1E4`（大写打包，如
`"ZH-CN"`）。模块的 constructor 启动抢跑线程（5ms 轮询），在游戏首次消费该字段前
写入标志文件中的目标 locale。相邻主字段 `+0x1E0` 恒为 `0x600`，只读不写——历史上
写它曾导致进游戏崩溃。

### 热重载层（ARM64）

语言切换后按顺序执行：

1. 前置写入语言管理器的次语言字段（匹配用）；
2. 外置表命中（LangCache / CDN）→ 走原版 CDN apply 链；
3. 未命中 → 数据包 RSB 直载（主形式 `packages\LawnStrings-<locale>.rton`）→ 官方
   apply → 必要时补一次表变更广播；
4. 成功后统一调用 `refresh_all_localized()` 回填全部已加载文本。

两个历史坑写在源码注释里：RSB 名必须带 `packages\` 前缀（否则运行期永久 miss）；
CDN apply 的日志上下文必须传栈上对象（传空指针即写地址 `0x48` 崩溃）。

### News 页本地化（自定义语言短码兜底）

News「News and Updates」页的条目文本来自游戏表 `NewsData` 的 `LocalizedData` 映射，
引擎按当前语言生成一个**两字母短码**作键去查。该短码由引擎内部函数硬编码白名单
`en / de / fr / it / pt / es` 产出——**自定义语言（如** **`zh-cn`）不在白名单，产出空
串**，于是查空键、未命中，条目显示空白。

修复由 `jni/lzt_settings_framework_config.h` 的 `kEnableNewsLocalization` 控制（见
`jni/screen_bindings.cpp`）：在 News 取键函数入口做兜底，当 key 为空时改用当前
locale 的**第一段后缀**（`ZH-CN` → `zh`）作为替身键，从而命中 `LocalizedData` 里
以 `zh` 为键的条目。改写只作用于本次查询、不回写调用方内存。

> 数据侧约定：News 的 `LocalizedData` 键用**第一段后缀**（`zh`、`fr`…），
> 与引擎对内置语言的行为一致。

## 组件补丁

除了框架引擎与业务模块，仓库还有一层**组件补丁**：它们修复的是游戏内
**通用组件**，被多个模块共用，既不属于
框架引擎、也不属于任何业务模块，故独立成层，避免污染框架与业务代码。

| # | 名称                  | 目标                           | 状态  |
| - | ------------------- | ---------------------------- | --- |
| 1 | glyph\_cache\_size  | 强制所有设备字形渲染缓存为 **2048**       | 已实现 |
| 2 | glyph\_cache\_flush | 图鉴翻页 / 切换条目时主动触发字形缓存「清 + 重建」 | 已实现 |

- 均由 `jni/lzt_settings_framework_config.h` 的 `kEnableGlyphCacheSize2048` /
  `kEnableAlmanacGlyphFlush` 控制（默认开启）。

- 安装时机沿用 `lzt_settings::register_engine_ready()`：引擎就绪、基址可用后再装自身 hook。

- 偏移为 ARM64 专用，仅 ARM64 编译实体，ARM32 为空实现。

> 问题描述、Android 结论（含精确指令地址与根因告警串）、实现方案与真机验证记录，
> 详见 [jni/components/README.md](jni/components/README.md)。

## 兼容性

| 项目          | 当前支持情况                                      |
| ----------- | ------------------------------------------- |
| 游戏版本        | 默认偏移基于 PvZ2 国际版 9.8.1；其他版本需重新核对 `offsets.h` |
| Android ABI | `arm64-v8a` 与 `armeabi-v7a`                 |
| ARM32 指令集   | Hook 目标为 ARM 模式，函数地址必须为偶数                   |
| 最低构建平台      | `android-24`（由 `jni/Application.mk` 配置）     |
| C++ 运行时     | `c++_static`（静态链接 libc++，避免与游戏自带运行时冲突）      |
| 推荐 NDK      | Android NDK r26b 或更新兼容版本                    |
| 设备方向        | 按横屏宽高比计算，`screenWidth / screenHeight`       |
| 设置状态        | 视角与语言选择写入游戏偏好后端并在启动/切换时恢复                   |

“支持其他版本”不等于直接复用 9.8.1 地址。函数布局、全局对象地址和结构字段都可能
随更新变化；未经核对的偏移通常表现为 Hook 未触发、设置页面异常或直接崩溃。

## 架构差异

| 项目               | ARM64                       | ARM32                                 |
| ---------------- | --------------------------- | ------------------------------------- |
| Hook 库           | And64InlineHook             | 自实现 ARM 模式 inline hook                |
| Patch 模式         | B 近跳(4字节) / LDR+BR 远跳(16字节) | 固定 `LDR PC,[PC,#-4]` + 地址 + NOP（12字节） |
| Board 字段偏移       | 0x438/0x460/0x46C\~0x478    | 0x338/0x35C/0x368\~0x374              |
| DisplayInfo 偏移   | +0xF4/+0xF8                 | +0x88/+0x8C                           |
| 相机 / 视角 Hook     | 完整                          | 完整                                    |
| 设置框架注入           | 完整                          | 完整                                    |
| 语言热重载            | 完整（含表重载与广播）                 | 不支持，保存选择后重启生效                         |
| 本地化刷新（Localizer） | 完整                          | 占位返回，不刷新                              |

ARM32 目标函数为 ARM 模式，使用偶数函数地址，由 BLX 完成指令集切换；禁止将
地址 bit0 置为 1。所有地址常量见 `jni/offsets.h`。

## 日志与故障排查

Hook 运行日志同时输出到：

- **logcat**：tag 为 `settingsframework`

- **本地文件**：`/sdcard/Android/data/<包名>/files/settingsframework.log`（每次启动覆盖）

关键日志：

| 日志关键词                                   | 含义                                                  |
| --------------------------------------- | --------------------------------------------------- |
| `applyHooks: base = 0x...`              | 定位游戏库文件稳定基址并安装 hook                                 |
| `H0 BoardZoom2: scale X -> 1.0`         | Hook 0 强制高视角生效                                      |
| `H1 BoardZoom ALIGNED`                  | Hook 1 按当前视角阈值完成对齐                                  |
| `DEFER BoardZoom ALIGNED`               | 延迟线程在布局就绪后完成对齐                                      |
| `A23A8C v44 align-start`                | ARM32 目标展示起点修正生效                                    |
| `C187C MoveBoard v42 align-start`       | ARM64 目标展示起点修正生效                                    |
| `[SFW]` 前缀                              | 设置框架引擎日志（注册、ID 分配、Tab 注入、页面构建）                      |
| `LangTable: source=`                    | 语言表来源命中结果（`cdn-rton` / `cdn-json` / `package-rton`） |
| `table loader: all sources unavailable` | 语言表全部来源不可用（tried N = 参与尝试的来源数）                      |
| `createTab: skip`                       | `shouldCreate` 判定为不建该模块 Tab                         |
| `Language config save:`                 | 语言单选组写入与回读结果                                        |
| `hot_reload begin/end`                  | 语言热重载流程边界                                           |
| `watchdog: FIX #N`                      | board\[280] 被篡改并还原                                  |
| `REINSTALL #N`                          | patch 被清零，监控线程自动重装                                  |

### 常见现象

| 现象                        | 优先检查                                                                         |
| ------------------------- | ---------------------------------------------------------------------------- |
| 完全没有 settingsframework 日志 | APK 是否执行 `System.loadLibrary("settingsframework")`，ABI 目录是否放入正确 `.so`        |
| 日志有 `base` 但 Hook 不触发     | 当前游戏版本偏移是否与 `offsets.h` 一致，目标函数是否被更新或内联                                      |
| 设置页进入即崩溃                  | Settings 函数地址、控件辅助函数和字段偏移是否来自同一游戏版本                                          |
| 设置页没有视角 Tab               | 模块注册是否成功（查 `[SFW]` 注册日志），锚点 Tab 是否存在                                         |
| 设置页没有语言 Tab               | 是否配置了 LANGUAGETYPES 来源、启用语言是否多于一种（见 `createTab: skip`）                       |
| 切换语言后语言 Tab 内文本未刷新        | 语言热重载与本地化刷新均为 ARM64 实现；ARM32 需重启生效                                           |
| 低视角普通设备不左对齐               | 预期行为；有效宽高比必须严格大于 `2.16666`                                                   |
| 高视角普通手机不对齐                | 检查宽高比是否大于 `1.69335`，以及日志中的当前视角和 `layout_not_ready`                           |
| 进入保存关卡暂时偏移                | 查看 `DEFER` 是否武装并在布局就绪后输出 `ALIGNED`                                           |
| 白屏或相机移出画面                 | 检查是否出现 `layout_not_ready` / `left_align_out_of_range`；通常表示字段偏移错误             |
| 目标展示首帧跳变                  | ARM32 查找 `A23A8C v44 align-start`，ARM64 查找 `C187C MoveBoard v42 align-start` |

报告问题时请提供游戏版本、设备宽高比、CPU 架构、正式版或 Debug 版、复现步骤，以及
问题发生前后的日志片段。日志可能包含包名和内存地址，公开前请自行脱敏。

## 已知限制

- 默认偏移只针对国际版 9.8.1；游戏更新后必须重新验证，不提供通用的自动适配。

- 模块依赖游戏内部私有函数和结构布局，无法保证与其他 Hook、修改器或重打包方案兼容。

- 语言热重载与本地化刷新为 ARM64 专属；ARM32 上语言选择需要重启生效。

- 宽高比判定面向横屏显示；分屏、自由窗口或运行时分辨率变化可能需要重新初始化。

- `aspect <= 0` 时会保守执行对齐，这是读取失败时避免漏修的回退，而不是设备分类结果。

- 项目只提供源码和构建说明，不提供游戏 APK、游戏库文件或签名材料。

## 常见问题

### 为什么修改 Board 字段而不是坐标查询函数

部分相机路径直接读取 Board 字段，不经过坐标查询函数。修改共同数据源可以覆盖这些
读取路径，而只 Hook 查询函数会遗漏目标展示、选卡平移等流程。

### 为什么低视角使用更高的宽高比阈值

低视角本身保留更多草坪范围，常见手机比例不需要额外左对齐。低视角仅在
`aspect > 2.16666` 的超宽屏设备执行左对齐，避免普通设备发生不必要偏移；
高视角仍以 `1.69335` 为阈值。

### 框架里为什么不写任何屏幕名称

框架只做机械且通用的部分（hook 安装、ID 分配、事件路由、页面骨架、控件物化、
生命周期防护）。一旦框架里出现具体屏幕名或业务状态名，新增同类适配就得改框架。
因此业务适配一律放业务文件（如 `screen_bindings.cpp`），通过注册点接入。

### 为什么其他版本只改 offsets.h 仍可能失败

地址集中不代表函数语义永远相同。游戏更新可能改变 ABI、参数顺序、字段类型或调用链。
适配时既要更新地址，也要在实现原理与运行日志中确认目标函数仍承担同一职责。

### 能否把本项目或修改版用于收费服务

本项目原创材料采用 PolyForm Noncommercial License 1.0.0，仅允许非商业用途。
上游和第三方 MIT 部分保留各自权利，完整边界以 `LICENSE` 与 `NOTICE` 为准。

## 项目结构

```text
PvZ2-lzt-settings-framework/
├── jni/                                 # 原生源码
│   ├── Android.mk / Application.mk      # NDK 构建描述（模块名、ABI、平台）
│   ├── lzt_settings_framework.cpp       # 主实现：基址解析、补丁流程、实体注入
│   ├── lzt_settings_framework_config.h  # ★ 唯一编译配置（日志门 + 功能开关常量）
│   ├── lzt_core.cpp / lzt_core.h        # 基址解析、日志、包名与 files 目录
│   ├── lzt_hooks.h                      # 双架构 inline hook 门面
│   ├── offsets.h                        # 全部函数地址与字段偏移（ARM64 / ARM32 分段）
│   ├── view_angle_module.cpp / .h       # 视角模块（首个接入范例）
│   ├── language_module.cpp / .h         # 语言模块（语言表、热重载、持久化）
│   ├── language_types.cpp / .h          # LANGUAGETYPES（JSON 解析）
│   ├── language_types_rton.cpp / .h     # LANGUAGETYPES（RTON 解码）
│   ├── resource_file.cpp / .h           # 外置文件通道 / 包内查找
│   ├── resource_file_arm64.S            # ARM64 资源读取汇编
│   ├── cdn_load_rton.cpp                # 外置 RTON 加载
│   ├── screen_bindings.cpp / .h         # 屏幕本地化绑定（经注册点接入框架）
│   ├── And64InlineHook.cpp / .hpp       # 上游内联 hook 库（MIT）
│   ├── settings/                        # 框架层 + 本地化刷新层
│   │   ├── settings_framework.cpp / .h  # Tab 注入、ID 分配、事件路由、页面骨架
│   │   ├── settings_widgets.cpp / .h    # 构建清单 → 游戏真实控件（双架构底层）
│   │   ├── localizer.cpp / .h           # 类型化本地化注册表与回填调度
│   │   ├── table_loader.cpp / .h        # 数据表来源调度（优先级 + 失败节流）
│   │   └── game_abi.cpp / .h            # 游戏 ABI 工具（ARM64）
│   ├── components/                      # 组件补丁层
│   │   ├── component_patches.cpp / .h   # 字形缓存尺寸 / 图鉴翻页刷新
│   │   └── README.md                    # 组件补丁档案
│   └── deprecated/                      # 归档实现（不参与构建）
│       └── resource_id_channel.cpp
├── docs/
│   ├── offsets-guide.md                 # 偏移定位指南
│   └── LANGUAGETYPES.json               # 语言表模板
├── .github/                             # Issue 模板与 CI
├── feature_flags.conf                   # ★ 功能开关配置（唯一声明处）
├── LICENSE
├── NOTICE                               # 上游来源、第三方 MIT 与授权边界
└── README.md
```

## 许可证

`LICENSE` 前部的适用范围声明明确界定 PolyForm Noncommercial License 1.0.0 中
“the software”所指材料，后部完整保留该许可证原文。该许可仅适用于 CongJian833
为 lzt-settings-framework（原 LawnZoomTab）独立创作、且依法享有著作权的材料及其源代码、编译、修改和被包含
形式；禁止将这些材料用于预期商业应用、收费分发、商业服务或其他营利用途。

上游 PvZ2-MaxZoomHook 与 And64InlineHook 的 MIT 权利不因本项目附加许可而被撤销
或缩减；游戏本体及其他第三方材料不由本项目授权。混合文件中的各部分分别继续适用
其各自许可证，具体边界见 `LICENSE` 与 `NOTICE`。

## 致谢与版权

本项目作者：**落筆从生簡**（BiliBili）

**鸣谢：雪竹子池**

在这里也一并感谢各位支持项目部分功能实现、参与项目运行测试，并为本项目提出改进建议的伙伴们\~

分发本项目时，请同时保留 `LICENSE`、`NOTICE` 和其中的 Required Notice。

## 免责声明

本项目仅供学习与技术研究使用。使用者需自行承担使用风险，作者不对任何因使用
本项目造成的后果负责。请遵守当地法律法规，尊重游戏版权。
