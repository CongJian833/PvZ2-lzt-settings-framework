# v43_banner 基线存档

## 包

```
PvZ2_9.8.1_v43_banner.ipa
110,279,381 字节
SHA256: c8a0cc61671da6604526b7efa9d4fcad873d50344b00b665e47c112c74b05230
```

manifest：`manifest.txt`（18 站点，比 v42 多 `17 6978f8`）

## 本版新增

**主界面横幅（PERSISTENT_MESSAGING）随语言切换刷新。**

### 根因

`banner hook(0x6978f8): FAIL` 从很早就存在。原因不是挂钩逻辑错，
而是 `LZT_STATIC_ONLY` 模式下 `A64HookFunction` 只认打包期预置站点，
而 `0x6978f8` 从未进过 manifest。

失败后 `g_origBannerRebuild == nullptr`，于是：

```cpp
static void refresh_persistent_banner() {
    if (a1 == 0 || g_origBannerRebuild == nullptr) return;  // ← 永远从这里退出
```

主界面按钮正常，是因为它的重建入口 `0x6f349c` 是站点 4，一直在 manifest 里。

### 横幅构建函数实证（0x6978F8）

```asm
100697a18:  adrp x0, 0x101efd000
100697a1c:  add  x0, x0, #3120      ; 0x101efdc30
100697a24:  bl   0x100179c14        ; 本地化口
...
100697b4:   adrp x0, 0x101efd000
100697b8:   add  x0, x0, #3244      ; 0x101efdcac
100697ac0:  bl   0x100179c14
```

二进制解码：

| 地址 | 内容 |
|---|---|
| `0x1efdc30` | `[PERSISTENT_MESSAGING_OFFLINE]` |
| `0x1efdcac` | `[PERSISTENT_MESSAGING_OFFLINE_CONTENT]` |

即横幅标题与正文，文本来自当前语言表。

### 刷新链

```
语言切换 → 换表 → refresh_rows → refresh_root_title
                                  → request_screen_refresh
                                        ├─ 设置页开着 → 挂起
                                        └─ 否则 → run_screen_refreshers
                                              ├─ refresh_persistent_banner  (0x6978f8)
                                              └─ refresh_main_menu          (0x6f349c)
离开设置页 → flush_pending_screen_refresh → 补跑
```

## 验证日志

```
banner hook(0x6978f8): ok
banner captured a1=0x127ae7160
banner refreshed a1=0x127ae7160
[TITLE] root title: obj=0x30255aa00 "设置" -> "Settings"
[TITLE] root title: obj=0x3025e16c0 "Settings" -> "Paramètres"
```

## manifest 全表（18 站点）

```
0  3c4c04   图鉴 ButtonDepress
1  3c44a8   图鉴 Update
2  3c5330   图鉴 PageCase0
3  3c55a4   图鉴 PageCase1
4  6f349c   主界面重建
5  757c18   createTab
6  759dd4   dispatch
7  758a94   page
8  179c14   本地化入口
9  757b78   wstring ctor
10 758740   checkbox ctor
11 757e58   行挂载
12 2c0bf0   表加载入口
13 735930   CDN apply 循环
14 2c9e04   单表 apply
15 1ac6d50   FourCC → 字符串
16 756ae4   设置页构造函数（根标题）
17 6978f8   横幅构建（PERSISTENT_MESSAGING）
```
