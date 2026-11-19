# 组件补丁（components）

本目录存放**既不属于 settings 框架引擎、也不属于「视角/语言」业务模块**的补丁——
它们修复的是游戏内**通用组件**，
被多个模块共用，故独立成层，避免污染框架与业务模块代码。

## 组件补丁一览

| # | 名称 | 目标 | 状态 |
|---|------|------|------|
| 1 | glyph_cache_size | 字形缓存 **2048** + 图集页上限 **4** + 页增长开启 | 已实现（待真机验证） |
| 2 | glyph_cache_flush | 图鉴翻页/切换条目时主动触发字形缓存「清 + 重建」 | 已实现（真机日志已验证） |

---

## 组件补丁一：字形缓存 2048 + 图集页上限 4

### 问题「字符缺失」

字形渲染缓存按屏幕最小边动态选 512 / 1024 / 2048，小屏落入低档 → 缓存不足 → 字符缺失。
图鉴等大文本页面尤为明显，严重时闪退。

### 引用：社区已验证的结论

B 站教程 [cv16512322](https://www.bilibili.com/read/cv16512322/) 用 A/B 实验证明：

- 该字段就是**缓冲区大小**：设 1 → 几乎无字；增大 → 字变多
- **2048 是安全上限**：设 >2048 文字全白，效果等同设 0
- 强制 2048 对中低端确实有效，但**高端设备仍不够用**

后两点正是本补丁需要额外处理页数的原因。

### Android 结构（偏移与字节均按 .so 本体 PT_LOAD 映射核对）

尺寸决策在 **`sub_1713384`**（EAText 缓存初始化）：

```
0x17133CC  LDR  W8, [X20,#0x744]      ; 取屏幕维度 A
0x17133D0  LDR  W9, [X20,#0x748]      ; 取屏幕维度 B
0x17133DC  CSEL W23, W8, W9, LT       ; W23 = min(A, B)
0x17133F0  CMP  W23, #0x401           ; min >= 1025 ?
0x17133F4  MOV  W8, #0x800            ;   2048
0x17133F8  MOV  W9, #0x400            ;   1024
0x17133FC  CSEL W8, W9, W8, LT
0x1713400  CMP  W23, #0x281           ; min >= 641 ?
0x1713404  MOV  W9, #0x200            ;   512
0x1713408  CSEL W3, W9, W8, LT        ; ← 补丁 A：最终缓存尺寸
0x1713414  BL   sub_17F3560           ; ctor(cache, _, _, size = W3)
0x1713424  STR  W9, [X21,#0x138]      ; SetParameter(4, 1)
0x1713428  MOV  W1, #1                ; ← 补丁 B：页数上限
0x1713430  MOV  W2, #1                ;   初始分配页数
0x1713438  BLR  X8                    ; SetPageLimit(limit=w1, initial=w2)
```

缓存类 **`Sexy::PrimeGlyphCache`**（RTTI `N4Sexy15PrimeGlyphCacheE` @0x1D706A0），
构造 **`sub_17F3560(a1,a2,a3,a4)`** 把尺寸写入 `+0xA4`；基类构造 `sub_1818C08`
把 `+0xA0`（页数上限）置 4、`+0xA4/+0xA8` 置 512/3、`+0x134`（页增长开关）清零。

**关键：`sub_1713428` 的 `SetPageLimit(1,1)` 会把基类刚设的 4 覆盖回 1。**

### 为什么不能只改尺寸

此前实现 hook ctor 改 `a4 = 2048`，真机会在启动路径闪退（堆破坏）。原因链：

1. 页上限被 `SetPageLimit(1,1)` 固定为 **1**
2. 页增长开关 `+0x134` 构造期清零，**全库无人开启**
   （字节扫描 `STRB [Xn,#0x134]` 全库 25 处，唯一属本类的写点是 `0x18192EC`
   即 `SetParameter(2,x)`，而 `SetParameter(2,x)` 无任何调用者）
3. `sub_181A368`（EnsurePage）：`+0x134 == 0` → 直接返回，**永远不开新页**
4. 于是 512 格子在 1 页里刚好够用；改成 2048 后单字占 4 倍面积，
   页更快耗尽 → 无处可放的告警/空指针

> 注：`+0xA0` 不是「只读常量」，它被 `SetPageLimit` 覆盖；`+0xA8` 也不是
> 「尺寸伴随字段」，而是 `SetParameter(id=1)` 的值（被 `sub_1819B38` 用作
> glyph 对象 `+132` 的默认值），**改写它会造成另一类错乱**。

### 实现（三项成套）

| | 位置 | 原始 | 补丁 |
|---|---|---|---|
| A 尺寸 | `0x1713408` | `0x1A88B123` CSEL W3,W9,W8,LT | `0x52810003` MOV W3,#2048 |
| B 页上限 | `0x1713428` | `0x320003E1` MOV W1,#1 | `0x52800081` MOV W1,#4 |
| C 页增长 | ctor 返回后 | `+0x134 = 0` | 写 1 |

- A/B 是**指令补丁**，不是 hook：`w3`/`w1` 在该函数内唯一消费方就是 ctor 入参
  与 SetPageLimit 入参，替换安全且零开销。
- C 需要缓存实例，故 hook ctor；**只在调用原函数之后写字段，不改入参**——
  改 `a4` 才是堆破坏的成因。
- `sub_17F3560` 全库**只有一个调用点** `0x1713414`（IDA xref 核对），
  故不存在遗漏路径。
- 每项都校验原始指令字：不符只记一次日志并跳过，绝不盲写。
- 20s 内每 500ms 复检一次，覆盖「补丁安装前 EAText 已初始化」的时序竞态
  （对齐 iOS 的 boot/1s/3s/8s 复检）。

### 验证

Debug 日志应出现：

```
[CMP] glyph size -> 2048: patched @+0x1713408  1a88b123 -> 52810003
[CMP] glyph page limit -> 4: patched @+0x1713428  320003e1 -> 52800081
[CMP] glyph cache ctor hook installed @+0x17F3560
[CMP] glyph cache 0x...: size=2048 pageLimit=4 growth=1
```

真机：图鉴连续翻页、设置长文本页不再出现缺字；不再有
`There is no place to put this glyph into the glyph cache`。

> ⚠ 页增长打开后会按需分配图集页。iOS 文档记为每页 2048×2048、
> 最坏多占约 48MB 纹理。低内存设备需实测内存占用。

### ⚠ 真机回归（2026-11-18）：页上限/页增长必须保持关闭

初版按「尺寸 + 页上限 + 页增长」三项成套实现，真机 **两个变体均闪退**。
定位到两个独立问题：

| # | 问题 | 现象 |
|---|------|------|
| 1 | 尺寸补丁打错位置 | 用了函数入口 `0x1713384` 而非 csel 本身 `0x1713408`，`unexpected insn 0xa9bc5ffc (skip)`，尺寸实际仍为 1024 |
| 2 | 页上限+页增长 | 尺寸未生效时把 limit 提到 4 并开启增长 → 闪退 |

修正后（只保留尺寸补丁，`kGlyphAllowPageGrowth = false`）实测：

```
[CMP] glyph size -> 2048: patched @+0x1713408  1a88b123 -> 52810003
[CMP] glyph recheck: cache=0x... size=2048 limit=1 growth=0
```

结果：**不崩、缺字改善、无 `no place to put this glyph` 告警**。

结论：`kGlyphAllowPageGrowth` 是**已验证不安全**的开关，不要重新打开。
若要继续提升大文本容量，方向应是「页满时才扩容」而非启动即放开增长。

防复发：`tests/test_glyph_patch_sites.py` 用 PT_LOAD 映射读真实 `.so`，
把「偏移 → 原始指令字」钉死；偏移写错会直接 `EXIT=1`。

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
