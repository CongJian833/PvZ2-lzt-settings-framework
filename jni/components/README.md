# 组件补丁（components）

本目录存放**既不属于 settings 框架引擎、也不属于「视角/语言」业务模块**的补丁——
它们修复的是游戏内**通用组件**，
被多个模块共用，故独立成层，避免污染框架与业务模块代码。

## 组件补丁一览

| # | 名称 | 目标 | 状态 |
|---|------|------|------|
| 1 | glyph_cache_size | 强制所有设备字形渲染缓存为 **2048** | 已实现（日志已验证） |
| 2 | glyph_cache_flush | 图鉴翻页/切换条目时主动触发字形缓存「清 + 重建」 | 已实现（真机日志已验证） |

---

## 组件补丁一：强制字形缓存 2048

### 问题「字符缺失」

小屏设备上，字体/字形渲染缓存按屏幕最小边动态选择 512 / 1024 / 2048，
小屏落入 512/1024 导致缓存不足 → 字符缺失。

### Android 探索结论

尺寸选择逻辑位于 **`sub_1713384`**（EAText 字体/缓存初始化）：
`0x1713384` 起读屏幕尺寸并计算缓存尺寸。

```
0x17133CC  LDR  W8, [X20,#0x744]      ; 取屏幕维度 A（min 用）
0x17133D0  LDR  W9, [X20,#0x748]      ; 取屏幕维度 B
0x17133D8  CMP  W8, W9
0x17133DC  CSEL W23, W8, W9, LT       ; W23 = min(A, B) —— 屏幕最小边
...
0x17133F0  CMP  W23, #0x401           ; min >= 1025 ?
0x17133F4  MOV  W8, #0x800            ;   2048
0x17133F8  MOV  W9, #0x400            ;   1024
0x17133FC  CSEL W8, W9, W8, LT        ; W8 = (min<1025)?1024:2048
0x1713400  CMP  W23, #0x281           ; min >= 641 ?
0x1713404  MOV  W9, #0x200            ;   512
0x1713408  CSEL W3, W9, W8, LT        ; W3 = (min<641)?512:W8  ← 最终缓存尺寸
0x1713410  MOV  X2, X20
0x1713414  BL   sub_17F3560           ; PrimeGlyphCache 构造(cache, _, _, size=W3)
```

缓存类为 **`Sexy::PrimeGlyphCache`**（RTTI `N4Sexy15PrimeGlyphCacheE` @0x1D706A0），
构造函数 **`sub_17F3560(a1, a2, a3, a4)`** 把尺寸写入对象字段：

```
sub_17F3560: ... ; *(_DWORD *)(a1 + 164) = a4;   // +0xA4 = 缓存尺寸
```

### 实现方案（推荐）

**Hook `sub_17F3560`，强制 a4 = 2048。** 单点、对所有设备统一，不动尺寸选择逻辑。

- hook 点偏移：`0x17F3560`（`OFF_GLYPH_CACHE_CTOR`）
- hook 行为：`a4 = 2048` 后调原函数
- 兼容：`sub_17F3560` 还被 `sub_181B8A8`（另一字形缓存变体）调用，两处统一受益；
  若担心影响，也可改 hook `sub_1713384` 覆盖 W3（但其为函数中部、寄存器上下文复杂，
  故首选 hook 构造函数）。

### 验证

- 日志：install 成功 + 每次构造打印 `size=2048`（Debug）。
- 真机：小屏设备（如 720p）进入含大量文本的页面（图鉴详情、设置长文本），字符不再缺失。

---

## 组件补丁二：图鉴翻页主动刷新字形缓存

### 问题「图鉴翻页累积字形缓存」

图鉴翻页渲染新文本时，字形缓存仍保留上一页内容 → 新页复用脏缓存 → 字符缺失/异常。
与组件补丁一的「缓存不足」不同，这里是「缓存污染」。

**Android 根因铁证**：字形网格构建函数 **`sub_1712664`**
（`PrimeGlyphMesh_General::BuildPart`）内部硬编码告警串：

```
[WARNING] PrimeGlyphMesh_General::BuildPart - There is no place to put this glyph
into the glyph cache.  You should clear the cache before building the parts.
```

### Android 探索结论

- 小图鉴**条目切换 / 翻页**入口：**`sub_869258`**（`AlmanacObjectChooser::Select`）
  - 签名 `(a1, a2)`，`a2` = 目标条目索引；内部 `a1[25] + 8*a2` 定位条目、
    重建选择项（`sub_869858`）、切换选中标志（`+384`），并播放
    `"Play_UI_Button_Almanac_Tab_Small"` / `"Play_UI_Menu_Tab_Scroll"` / `"SelectItem"` 音效。
  - 入口为标准函数序言（`SUB SP,#0xD0` + `STR`/`STP`×3），无 PC 相对指令，inline hook 绝对安全。
- 图鉴 **Tab 切换**入口：`sub_86EC40`（`AlmanacWidget::ButtonDepress`，`a2`：0=植物/1=僵尸/2=升级）。
- 字形缓存「清 + 重建」原语（`PrimeGlyphCache` = `EAText 全局 + 8`）：
  - 取全局：`sub_1713288()` → EAText 全局指针
  - **清**：`sub_1714090(g)` = `sub_17F3DF4(*(g+8))` —— 销毁 image render data
  - **重建**：`sub_1714098(g)` = `sub_17F3E70(*(g+8))` —— 重新构建渲染数据
  - 二者即引擎自身 `sub_153DEB4` 重置字形缓存时成对调用的序列（`0x153dec4` / `0x153def8`），
    完全对齐 iOS 的 `ClearPrimeGlyphCache` + rebuild 语义。

### 实现方案

在翻页/切换入口 **pre-hook**，切换视图前调用字形缓存「清 + 重建」：

- hook 点：`OFF_ALMANAC_CHOOSER_SELECT` (0x869258) 与 `OFF_ALMANAC_WIDGET_DEPRESS` (0x86EC40)
- flush 组合：`sub_1714090(g)` + `sub_1714098(g)`，`g = sub_1713288()`
- 开关：`kEnableAlmanacGlyphFlush`

> 已排除的错误钩点：`sub_14788AC`（`AlmanacPage::ButtonDepress`）实为
> **升级按钮 → 世界地图**（`a2==11`），与翻页无关，不再 hook。

### 验证

- 日志（真机实证）：遍历植物图鉴 135 个条目时，每次切换均打印
  `[CMP] almanac chooser select a2=<条目索引>` 与
  `[CMP] almanac-chooser a2=<条目索引>: glyph cache flushed (cache=<EAText+8>)`；
  关闭图鉴打印 `almanac widget depress a2=-1`。全程无崩溃。
- `cache=` 与补丁一打印的 `cache=` 地址一致 → 证明 `EAText 全局 + 8` 取法正确、两者操作同一缓存实例。
- 真机：图鉴连续翻页（植物↔僵尸↔升级），新页文本不再缺字/串页。

---

## 接入方式

- 目录：`jni/components/`
- 编译开关：`lzt_settings_framework_config.h` 内
  `kEnableGlyphCacheSize2048` / `kEnableAlmanacGlyphFlush`

- 安装时机：经 `lzt_settings::register_engine_ready()` 在引擎就绪后安装（沿用 screen_bindings 范式）
- 架构：仅 ARM64 编译实体（偏移为 ARM64 专用）；ARM32 空实现
