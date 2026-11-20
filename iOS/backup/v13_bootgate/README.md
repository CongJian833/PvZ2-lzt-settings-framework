# v13_bootgate — 启动期游戏资源重入修复（里程碑存档）

日期：2026-11-03
状态：**已验证通过，作为稳定基线**

## 产物

```
PvZ2_9.8.1_v13_bootgate.ipa
110,276,157 字节
SHA256: 21cb4ae7196ec0b8e59616ad3ab1c2fea696d42c7b412a683ce087d2458e319d
```

## 解决的问题

启动时随机永久卡在 EA 界面（须杀进程重启）。
原版基线：77 次启动 / 11 次永久卡死 ≈ 14.3%。

## 根因

**游戏启动期，从主队列重入游戏自身的资源加载函数。**

```
bootstrap → prewarm → language_table_prewarm(true)
  → 来源 package-rton → lzt_ios_read_rsb_bytes
    → 游戏 0x1c44bb0（radix 定位）/ 0x1c44df4（取数）
```

这两个函数读写游戏的 radix 树与挂载组缓存。启动期游戏正处在自己的资源
加载批次中，被外部重入后即停在 EA 界面不动。

### 决定性证据（iPad 日志，卡死那次）

```
[HB] main thread STALLED ... tick=1
LRPKG P03 miss name=packages\LANGUAGETYPES.rton   ← 卡住期间调用游戏函数
[SFW] prewarm: channel ready at poll=1 ok=0
```

`LRPKG` 出现在 STALL **之后**，证明重入发生在主线程已卡死之后。

### 与历史问题的同源性

`lzt_main_ios.cpp` 中 `kEnableRsbFetchProbe` 的停用记录已描述过同一现象：
「在【主队列】上调用游戏内部函数 0x65428 … 表现为随机停在 EA 界面、必须
杀进程重启」。历史入口 0x65428 已停用，但 prewarm 是同一个雷换了入口。

## 修复内容

| 文件 | 改动 |
|---|---|
| `jni/language_module.cpp` | 新增 `kLangSourcesBootFs`：仅文件系统来源，无 `package-rton` |
| `jni/language_module.cpp` | 新增 `g_bootPhase` 闸门，启动期 `lang_loader()` 只返回文件系统加载器 |
| `jni/language_module.cpp` | `language_load_lawnstrings` 第三通道（package-rsb）加闸门 |
| `jni/language_module.cpp` | `lang_should_create()` / `build_page()` 解除闸门（先解除、再 retry） |
| `iOS/src/lzt_main_ios.cpp` | `kEnableBootPrewarm = false`，停用启动期预热（保留开关便于复验） |

数据包来源**未删除**，仅推迟到用户进入设置页后才评估。

## 验证结果（iPad Air M2 / iOS 18.7.8 / 未越狱）

v13 段共 37 次启动（2026-11-03 18:29:34 起）：

| 指标 | v12 | v13 |
|---|---|---|
| BOOT | 7 | 37 |
| `LRPKG`（游戏资源函数调用） | 13 | **0** |
| `prewarm: skipped` | 0 | **37/37** |
| 永久卡死 | 有 | **0** |
| STALLED | — | 3（**全部 recovered**） |

3 次 STALL 的 tick 分别为 1 / 3 / 2，均在 5–7 个采样后 `recovered`，
游戏继续运行。判定为游戏自身启动抖动，非致命。

## 未验证项

**闸门解除逻辑尚未被触发。** 日志中 `boot phase over` 出现 0 次，因为：

```
37/37  engine install: create=0 dispatch=0 page=0
```

`hook_create_tab` 未挂载 → `lang_should_create()` 从未被调用 → 解除路径
未执行。这是适配期的预期状态。**等 Tab 挂载打通后必须优先验证此项**：
一旦解除逻辑有误，iPad（CDN 无 LANGUAGETYPES.rton）上语言 Tab 会消失。

## 回滚方式

```bash
cp iOS/backup/v13_bootgate/language_module.cpp jni/
cp iOS/backup/v13_bootgate/lzt_main_ios.cpp iOS/src/
```

## 排查过程中的三个误判（勿重犯）

1. **「prewarm 68 次全 miss 只卡 11 次，故非原因」** — 按总数统计掩盖了
   顺序。真实情况是卡死那几次的 `LRPKG` 出现在 STALL 之后。
2. **「LANGUAGETYPES.rton 是死路径」** — 错误。iPhone 11 上实测
   `P04 READ bytes=293`、解析出 en-us/zh-cn 两语言。两设备差异在 CDN 目录
   内容：iPhone 有表，iPad 无表 → iPad 必然 fallthrough 到游戏函数。
3. **「延迟 bootstrap 1.5 秒」** — 会破坏数据包读取。目录重定向在
   constructor 中执行，代码注释明确要求「游戏读取数据包要求此路径就绪」。
