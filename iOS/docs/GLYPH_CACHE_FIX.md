# 汉化图鉴缺字修复（PrimeGlyphCache）

> iOS 侧完整实现记录。最终方案来源于 B 站专栏
> 《PvZ 2 国际版汉化导致字符丢失的彻底解决方法》的 iOS 地址实证，
> 并修正了两次错误尝试。已实测：植物图鉴 1→末尾、僵尸图鉴全覆盖，无缺字。

---

## 一、问题

国际版汉化后，翻阅图鉴会出现缺字；翻得越多越严重，最终整页只剩横线。

两类原因并存：

1. **缓冲区不足**——缓存尺寸按屏幕最小边选 512/1024/2048，小屏落在低档
2. **缓存污染**——翻页时新旧文本共用同一张图集，旧字形占位不释放

引擎的报错原文（字符串 @`0x1EE5FF7`）：

```
[WARNING] PrimeGlyphMesh_General::BuildPart - There is no place to put this
glyph into the glyph cache.  You should clear the cache before building the parts.
```

---

## 二、关键结构

`Sexy::PrimeText` 全局实例：`base + 0x27FDAB0`（+8 = `PrimeGlyphCache`）

`PrimeGlyphCache` 对象（440 字节）关键字段：

| 偏移 | 含义 |
| - | - |
| `+0x0A0` | 图集页数上限 |
| `+0x0A4` | 缓存尺寸（512/1024/2048） |
| `+0x140` | 互斥量 |
| `+0x170 / +0x178` | 页描述数组 begin / end（每项 20 字节） |
| `+0x188 / +0x190` | 位图数据区 begin / end |
| `+0x1A0` | 当前页数 |
| `+0x1B0` | 渲染器指针 |

页遍历与上传位于 `0x1cb9cbc`；字形栅格化位于 `0x1cb9f14`。

---

## 三、三个补丁

### 1. 强制 2048

缓存构造内联在 `0x1cb7d38`，没有独立构造函数可 hook，所以直接改选择指令：

```
0x1cb7d4c  sub  sp, sp, #512
0x1cb7d88  ldr  w8, [x1, #1972]        ; 屏幕宽
0x1cb7d8c  ldr  w9, [x1, #1976]        ; 屏幕高
0x1cb7d94  csel w8, w8, w9, lt         ; min
0x1cb7db0  csel w23, w8, w9, lt        ← 改成 mov w23, #2048
0x1cb7e0c  str  w23, [x21, #164]       ; = 缓存 +0xA4
```

补丁：

| 地址 | 原始 | 补丁 |
| - | - | - |
| `0x1cb7db0` | `1a89b117` `csel w23, w8, w9, lt` | `52810017` `mov w23, #2048` |

本机（iPhone 11，最小边 828）原本落在 **1024** 档。

### 2. 页上限 1 → 4（余量）

```
0x1cb7e1c  mov w1, #1        ← 页数上限，改成 #4
0x1cb7e20  mov w2, #1        ; 初始页数，保持
0x1cb7e24  bl  0x101c3f384   ; SetPageLimit(limit, initial)
```

| 地址 | 原始 | 补丁 |
| - | - | - |
| `0x1cb7e1c` | `52800021` `mov w1, #1` | `52800081` `mov w1, #4` |

页按需分配，平时只占 1 页；每页 2048×2048，最坏多占约 48MB 纹理。

> 单独测试：**只改这一项不能修复缺字**（缺字时机完全不变），保留它只是余量。

### 3. 翻页时清理缓存（核心）

`Sexy::PrimeText::ClearPrimeGlyphCache` 在 iOS 上是 **`0x1cb8420`**，
参数是 PrimeText 实例（不是缓存对象）。

**调用点实证**（这也是定位它的方法）：

```
0x100606fbc  bl  0x101cb7c64     ; PrimeText::GetInstance()
0x100606fc0  bl  0x101cb8420     ; ClearPrimeGlyphCache(x0)
```

**翻页入口**：图鉴页面渲染调度器 `0x3c4c04` 对 `a2` 做 switch，
`case 0` / `case 1` 才是翻页重建文本的两个阶段：

| 目的 | 地址 | 栈帧 |
| - | - | - |
| case 0 | `0x3c5330` | `SUB SP, #0x120` |
| case 1 | `0x3c55a4` | `SUB SP, #0xA0` |

两函数入口只用 `x0`（均 `mov x19, x0`），故用 `A64HookFunction` 入口 pre-hook：

```cpp
hk_page_case(self):
    ClearPrimeGlyphCache(PrimeText::GetInstance());
    return original(self);
```

**回退机制**：hook 图鉴的每帧更新 `0x3c44a8`，按时间节流清理。
翻页 hook 成功时放宽到 **5 秒**，失败时退回 **1.5 秒**。

---

## 四、走过的弯路（勿重犯）

| 尝试 | 结果 |
| - | - |
| 新建缓存实例替换 `EAText+8` | **整页横线、字体错乱**——渲染器仍持有旧纹理 |
| 原地重置页数组（`+0x1A0=0`、end=begin） | **缺字依旧**——纹理被新一轮栅格化覆盖，hash 仍认为旧字形有效 |
| 调用析构器 `0x1cba198` | **崩溃**——析构释放页数组，渲染器仍持有指针 |
| hook `0x3c4c04`（调度器）当作按钮入口 | 只在切 Tab / 关闭时触发，**翻卡片不经过**（日志只有 id=1、id=-1） |
| 只提高页上限 | 缺字时机不变 |

**关键教训**：`0x1cb8420` 一开始被我误读为「清单个字体」，
实际它就是 `ClearPrimeGlyphCache`——判断依据应该是**调用点上下文**
（`GetInstance()` 紧跟其后），而不是函数体里看到什么。

---

## 五、日志

启动一次 6 行，之后每会话一行：

```
[CMP] glyph cache forced to 2048 (boot)
[CMP] glyph page limit -> 4 (boot)
[CMP] almanac depress hook installed @+0x3c4c04
[CMP] almanac update hook installed @+0x3c44a8
[CMP] almanac page hooks installed (2/2) @+0x3c5330,+0x3c55a4
[CMP] glyph cache: size=2048 pageLimit=4 pages=0 cache=0x...
[CMP] almanac closed: page clears=23        ← 关闭图鉴时统计翻页清理次数
```

## 六、开关

| 文件（Documents/） | 作用 |
| - | - |
| `lzt_glyph_flush_off` | 关闭全部清理（A/B 对照） |
| `lzt_glyph_selftest` | 启动 15s 后连打 6 次清理，用于回归自检 |

---

## 七、地址速查

| 名称 | 偏移 |
| - | - |
| PrimeText 全局指针 | `0x27fdab0` |
| PrimeText::GetInstance | `0x1cb7c64` |
| PrimeText::ClearPrimeGlyphCache | `0x1cb8420` |
| 缓存构造（含 2048 选择） | `0x1cb7d38` |
| 缓存页遍历 / 上传 | `0x1cb9cbc` |
| 缓存析构（deleting） | `0x1cba198` |
| 图鉴渲染调度器 | `0x3c4c04` |
| 图鉴渲染 case 0 | `0x3c5330` |
| 图鉴渲染 case 1 | `0x3c55a4` |
| 图鉴 Update/Draw | `0x3c44a8` |
| AlmanacWidget vtable | `0x21cdb10` |
| "no place to put this glyph" 字符串 | `0x1ee5ff7` |
