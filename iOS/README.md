# lzt-settings-framework · iOS

> **PvZ 2** iOS 设置界面扩展框架 —— 与 Android 侧共享同一套框架源码，
> 以「静态跳板（static gate）」实现零运行时改写。
>
> 仓库：<https://github.com/CongJian833/PvZ2-lzt-settings-framework>

Android 侧的完整说明见仓库根目录 [`README.md`](../README.md)。本文件只讲 iOS
侧：**它与安卓有什么不同，以及为什么不同**。框架本身的机制（Tab 注册、页面
构建、本地化刷新）与安卓完全一致，本文不重复展开。

---

## 一、iOS 侧做了什么

在 iOS 上，框架的能力与安卓一致：

- 向游戏注入新的设置 Tab（视角、语言等模块）
- 声明式页面构建、自动 ID 分配与冲突校验、dirty 异步重建
- 类型化本地化服务，已加载文本随语言切换实时刷新

差异只有一条，但很关键：

| | Android | iOS |
|---|---|---|
| 注入物 | `libsettingsframework.so`（打进 APK 的 `lib/<abi>/`） | `liblztsettingsframework.dylib`（放进 `.app/Frameworks/`） |
| Hook 方式 | 运行时 inline hook，改写代码页 | **打包期静态跳板**，运行时不改任何代码页 |
| 交付形态 | APK | IPA |
| 架构 | arm64-v8a + armeabi-v7a | 仅 arm64（armv7 在打包时被裁掉） |
| 最低系统 | Android 7.0（API 24） | iOS 11.0 |

---

## 二、静态跳板：为什么 iOS 不能用运行时 hook

Android 可以在运行时 `mprotect` 改写代码页。iOS 不行：即使在越狱环境，
主程序代码页的写权限仍受限制，而且同一份 dylib 在 **LiveContainer** 与
**企业签名** 两种宿主下被加载到完全不同的地址，运行时 hook 的目标地址
每次启动都不一样。

所以 iOS 侧改成：**在打包阶段就把跳转写死**。

### 跳板结构

每个被接管的函数入口只改 **4 字节**——一条 `B` 指令，跳到 gate 区。
gate 区位于 `__TEXT` 头部 slack（load command 之后、首个 section 之前），
不新增段、不移动任何既有数据。

这个位置不是随便选的：容器类宿主（LiveContainer 等）会向主程序追加自己的
load command，把紧贴 load command 的空白区覆盖掉。因此打包器要求 gate 区
**与 load command 末尾至少留出 4 KB 间距**，且不得越过首个 section 起点，
否则直接报错退出。

```text
入口 (0x3c4c04)                 gate 区 (0x6BD0)
┌──────────────┐              ┌────────────────────────────────────┐
│ B gate       │ ───────────▶ │ +0x00 adrp x16, slot_page          │
└──────────────┘              │ +0x04 ldr  x16, [x16, #slot_off]  │
                              │ +0x08 cbnz x16, +8   ; 槽空?      │
                              │ +0x0C b    tramp     ; → 原逻辑   │
                              │ +0x10 br   x16       ; 槽非空     │
                              │ +0x14 nop                          │
                              │ +0x20 tramp: 原第 1 条指令       │
                              │ +0x24 adrp x17, page(entry+4)     │
                              │ +0x28 add  x17, x17, #lo         │
                              │ +0x2C br   x17     ; → 跳回原函数 │
                              └────────────────────────────────────┘
                                     每站点 48 字节
```

运行时只读一个 **槽位**（`__DATA` 段尾部的可写零页）：

- 槽为空 → 走 `tramp`，原函数照常执行（未启用该功能）
- 槽非空 → 跳进 hook 实现

这个设计带来一个好处：**不启用任何功能时，主程序行为与原版完全一致**，
因为跳板本身就还原了原始指令流。

### 安全边界

重放原指令需要它不在 PC 相对寻址中。若入口第 1 条指令是
`adrp` / `adr` / `b` / `bl` / `cbz` / `tbz` / `ldr literal`，该站点**直接被拒绝**——
宁可少一个功能，也不产生不确定行为。

### gate 区自描述

gate 区头部带自描述信息，便于校验与排查：

```text
+0x00  magic "LZTGATE1"
+0x08  u32 stride   (48)
+0x0C  u32 count    (22)
```

打包流程在重签之前会回读校验：magic 唯一、stride/count 正确、
所有入口确为 `B` 指令、gate 区与 load command 保持足够间距。
运行时还会再做一次结构自检（逐站点比对 5 条桩指令），两处都会打进日志。

---

## 三、代码组织

iOS 侧严格遵守「共享源码零改动」原则，与安卓侧共用同一份框架代码。

```text
iOS/
├── build.sh              编译脚本（产出 dylib）
├── inject.sh             把 dylib 注入已安装的 .app（包内注入）
├── tools/
│   ├── pack.sh           从底包出 IPA（六步）
│   ├── lztgate.c         静态跳板生成器
│   ├── ldcmd_add.c       给主二进制加 LC_LOAD_DYLIB
│   └── daveconv.c        组件补丁生成器
├── src/                  iOS 平台实现
├── include/              iOS 偏移表
├── port/include/         头文件垫片（android/log.h、offsets.h）
└── backup/               版本归档
```

三层划分：

| 层 | 位置 | 说明 |
|---|---|---|
| 共享源码 | `jni/*` | **与安卓逐字节相同**，iOS 编译时直接复用 |
| 平台实现 | `iOS/src/*` | 基址、日志、沙盒路径、屏幕、hook 门面 |
| 头文件垫片 | `iOS/port/include/*` | 让共享源码在 iOS 上零改动编译 |

垫片的思路是**同名替换**而非条件编译：共享源码写的是
`#include "offsets.h"`，iOS 侧提供一个同名头文件，把地址常量指向 iOS 的
偏移表。平台差异因此不侵入共享代码。

平台专属逻辑用 `#if LZT_PLATFORM_IOS` 隔离。这一条有代价——历史上双方各出过一次
编译事故，都是共享文件里引用了对方平台独有的符号。因此仓库里放了一对
对称的静态检查：

```bash
python3 tests/test_ios_platform_guards.py      # lzt_android_* 是否误入 iOS
python3 tests/test_android_platform_guards.py   # lzt_ios_* 是否误入 Android
```

---

## 四、构建与打包

### 编译

```bash
export PATH=/var/jb/usr/bin:/var/jb/usr/lib/llvm-16/bin:$PATH
cd iOS
LZT_STATIC_ONLY=1 sh build.sh
```

产物：`iOS/build/liblztsettingsframework.dylib`（arm64，约 275 KB）。

> **`LZT_STATIC_ONLY=1` 不能漏。**
> 这个变量决定 dylib 是否走打包期静态 gate。漏掉它会静默退化成运行时
> inline hook，与已打桩的主程序叠加，在 LiveContainer 下会死循环卡在启动图标。
> 脚本每次构建都会打印当前模式，避免打到包里才发现。

### 打包

```bash
OUTNAME=PvZ2_vXX.ipa \
MANIFEST=$PWD/backup/v52_scanfixed/mFinal.txt \
sh tools/pack.sh
```

六步流程：

```text
[0/6] 准备 + 静态模式校验
[1/6] 从 fat 底包抽出 arm64 切片
[2/6] 注入 dylib + LC_LOAD_DYLIB
[3/6] 写入 22 个静态跳板站点
[4/6] 主程序静态补丁（字形缓存 / 页上限 / 60fps）
[4b/6] 组件补丁（动画与传送带）
[4c/6] Info.plist 最低系统版本对齐
[4d/6] gate 完整性自检
[5/6] ldid 重签
[6/6] 打包 IPA
```

### 注入到已安装的 App

不打 IPA、直接改已安装的 `.app`：

```bash
sh inject.sh          # 注入 / 更新（首次自动备份主二进制）
sh inject.sh restore  # 从备份还原
```

---

## 五、站点表

22 个静态站点覆盖设置页与语言链路的全部接入点：

```text
0  3c4c04    11 757e58
1  3c44a8    12 2c0bf0
2  3c5330    13 735930
3  3c55a4    14 2c9e04
4  6f349c    15 1ac6d50
5  757c18    16 756ae4
6  759dd4    17 6978f8
7  758a94    18 183b10
8  179c14    19 183a58
9  757b78    20 5eddb4
10 758740    21 1c8380
```

| 站点 | 接入点 | 站点 | 接入点 |
|---|---|---|---|
| `3c4c04` | 图鉴按钮按下 | `757e58` | 行挂载 |
| `3c44a8` | 图鉴更新 | `2c0bf0` | 大语言表加载 |
| `3c5330` | 图鉴翻页清缓存 | `735930` | CDN apply 循环 |
| `3c55a4` | 图鉴翻页 | `2c9e04` | 单表 apply |
| `6f349c` | 主界面重建 | `1ac6d50` | FourCC 覆盖 |
| `757c18` | 设置 Tab 创建 | `756ae4` | 设置页初始化 |
| `759dd4` | 设置 dispatch | `6978f8` | 横幅构建 |
| `758a94` | 设置页布局 | `183b10` | 视角缩放 |
| `179c14` | 本地化入口 | `183a58` | 相机对齐 |
| `757b78` | wstring 构造 | `5eddb4` | 街道恐龙补偿 |
| `758740` | checkbox 构造 | `1c8380` | News 语言短码映射 |

其中 3 个视角站点（`183b10` / `183a58` / `5eddb4`）在运行时按条件安装，
因此日志里的实际启用数通常是 **19~20**，而非 22。

这份站点表也可以看作 iOS 侧的「函数表」——每个站点是一个接入点。
完整的地址与字段偏移说明见 [`docs/offsets-guide.md`](../docs/offsets-guide.md)。

---

## 六、双环境验证

iOS 侧的难点在于同一份产物要在两种宿主下都正确：

| | LiveContainer | 企业签名 |
|---|---|---|
| 宿主 | 第三方容器 | 正式签名 |
| 主程序 load command 数 | 81 | 79 |
| gate 区相对基址 | `+0x6BD0` | `+0x6BD0` |
| 站点安装 | 19~20 / 跳过 7 | 19~20 / 跳过 7 |
| 运行时改代码页 | 0 | 0 |
| 崩溃 | 0 | 0 |

`gate_hdr - base` 在两种宿主下都精确等于 `0x6BD0`，说明 gate 定位不受
ASLR、容器重定向、load command 数量变化影响。

load command 数量的差异（79 vs 81）来自容器注入自身追加的加载项——
这正是**必须用静态跳板、不能用运行时 hook** 的直接原因。

---

## 七、日志与排查

iOS 侧不写 logcat，改为 **同时** 输出到沙盒文件和系统日志：

| 输出 | 位置 |
|---|---|
| 文件日志 | `<沙盒>/Documents/lztsettingsframework.log` |
| 崩溃日志 | `<沙盒>/Documents/lztsettingsframework_crash.log`（追加写） |
| 系统日志 | `os_log`，子系统 `com.scode.lztsettingsframework` |

### 日志文件策略

- **每次启动清空重建**：当前日志文件名固定为 `lztsettingsframework.log`，
  启动时以 `"w"` 打开，所以**它永远只包含最近一次启动**的内容。
- **上一次自动归档**：启动时把上一份改名成
  `lztsettingsframework-<yyyyMMdd-HHmmss>.log`（文件名字典序即时间序）。
- **只保留最近 5 份**：更早的归档自动删除。
- **崩溃日志独立追加**：主日志每次启动会被截断，崩溃现场写进单独文件，
  两者互不覆盖。

> **文件名与安卓不同**：安卓是 `settingsframework.log`（与 `.so` 同名），
> iOS 是 `lztsettingsframework.log`（与 dylib 同名）。两套命名各自保持
> 与自身产物名一致，**跨端对照日志时不要按名字去取**。

### 常用关键字

| 关键字 | 含义 |
|---|---|
| `===== BOOT ... =====` | 每次启动的开头，带本地时间戳 |
| `[STATIC] gate_hdr=... stride=48 count=22` | 定位到 gate 区，静态机制生效 |
| `[STATIC] gate integrity check: all 22 sites OK` | 22 个桩结构自检通过 |
| `[STATIC-ONLY] static sites used=19, no-site skipped=7` | 站点启用统计 |
| `[ERROR] hook(s) without static site` | 缺站点的功能会静默失效 |
| `[SFW]` | 设置框架日志（Tab 注册、id 分配、页面构建） |
| `[CMP]` | 组件补丁的字节校验结果 |
| `LangTable: source=` | 语言表来源命中结果 |
| `LangOverride: rule2 ...` | 启动期语言标识判定分支 |
| `Lang hot_reload: ...` | 语言热重载流程边界 |
| `[HB] main thread STALLED` | 主线程停顿看门狗 |
| `NATIVE_CRASH` | 崩溃信号与现场（另见崩溃日志文件） |

### 排查思路

| 现象 | 先看 |
|---|---|
| 设置页里没有新 Tab | `gate_hdr` 是否为 0；`no-site skipped` 是否远大于 7 |
| 站点功能整体失效 | `gate integrity check` 是否报 corrupted |
| 语言表为空 | `LangTable: source=` 是否有输出；No_Backup 重定向是否成功 |
| 启动卡在加载图标 | 确认 dylib 是**静态模式**构建（见第四节） |
| 崩溃 | 附上 `lztsettingsframework_crash.log`，里面有寄存器和调用栈 |

### 取日志

- **越狱直装（企业签名）**：日志在游戏的数据容器 `Documents` 目录下，
  直接用文件管理器或 `scp` 取 `lztsettingsframework.log`。
- **LiveContainer**：日志在**内层 app** 的 `Documents` 目录，
  路径比直装多一层容器目录（形如
  `Documents/Data/Application/<内层UUID>/Documents/`），
  顺着目录找同名文件即可。

反馈问题时请附上：游戏版本、宿主环境（直装 / LiveContainer）、设备与系统
版本、复现步骤，以及出问题前后的日志片段。日志里可能含包名与内存地址，
公开前请自行脱敏。

完整的日志说明（两端输出通道、日志种类、崩溃记录、排查对照表）见
[`docs/日志系统.md`](../docs/日志系统.md)；安卓侧的位置、logcat tag 与归档
策略见仓库根目录 [`README.md`](../README.md) 第十节。

---

## 八、已知限制

- **armv7 不再产出**：打包时只抽 arm64 切片。iOS 侧只面向 arm64。
- **3 个站点按条件安装**：视角相关站点在运行时才注册，
  因此日志里的实际启用数通常是 19~20，而非 manifest 的 22。
  若实际启用数掉到个位数，说明 gate 区没被找到或被覆盖。
- **LiveContainer 下语言表可能为空**：容器的沙盒路径多一层嵌套，
  沙盒重定向的兜底逻辑失效时，游戏找不到 CDN 目录，语言表加载失败。
  企业签名环境下无此问题。
- **7 个站点未纳入**：其中 3 个是 No_Backup 相关路径，静态模式下被跳过。
- **站点表随游戏版本变**：偏移基于 PvZ 2 国际版 9.8.1，换版本需重算。

---

## 九、授权

本项目的源代码与文档采用 [PolyForm Noncommercial License 1.0.0](../LICENSE)，
仅允许非商业用途。And64InlineHook 适用 MIT License。游戏本体、游戏资源与
商标不属于本项目授权材料。完整边界见仓库根目录的 [`LICENSE`](../LICENSE)
与 [`NOTICE`](../NOTICE)。
