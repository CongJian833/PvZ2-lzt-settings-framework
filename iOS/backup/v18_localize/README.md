# v18_localize — 语言 Tab 全链路打通（里程碑存档）

日期：2026-11-04
状态：**已验证，语言设置页功能完整可用**

## 产物

```
PvZ2_9.8.1_v18_localize.ipa
110,277,570 字节
SHA256: 341d0bd309f2d1fc49365741bb8d0af4c05cd845f63905818cd0456add2a939a
```

## 静态站点（12 个）

| # | 地址 | 归属 | 作用 |
|---|---|---|---|
| 0 | 0x3c4c04 | 图鉴 | AlmanacWidget::ButtonDepress |
| 1 | 0x3c44a8 | 图鉴 | AlmanacWidget::Update |
| 2 | 0x3c5330 | 图鉴 | 翻页 case0 |
| 3 | 0x3c55a4 | 图鉴 | 翻页 case1 |
| 4 | 0x6f349c | 主界面 | 主界面重建（启动期闸门解除信号） |
| 5 | 0x757c18 | 设置框架 | createTab |
| 6 | 0x759dd4 | 设置框架 | dispatch |
| 7 | 0x758a94 | 设置框架 | page（子页面骨架） |
| 8 | 0x179c14 | 物化 | 本地化入口 |
| 9 | 0x757b78 | 物化 | wstring 构造器 |
| 10 | 0x758740 | 物化 | checkbox 构造 |
| 11 | 0x757e58 | 物化 | 行挂载 |

gate: cap=409, used=12

## 已实现并验证的功能

| 功能 | 状态 | 证据 |
|---|---|---|
| 语言 Tab 显示 | ✅ | 标题「Language Settings」正常本地化 |
| 点击进入语言页 | ✅ | tabId=30，页面标题「语言设置」 |
| 语言列表（数据包来源） | ✅ | `LRPKG P04 READ packages\LANGUAGETYPES.rton bytes=293` |
| checkbox 切换 | ✅ | English / 简体中文，绿勾表示选中 |
| 语言页文本即时跟随 | ✅ | 走自解析 g_lawnTexts |
| 启动期不重入游戏资源 | ✅ | 37 次启动 0 次永久卡死（v13 起） |
| 图鉴翻页清缓存 | ✅ | 用户实测正常 |

## 关键设计：启动期闸门

`language_module.cpp` 的 `g_bootPhase`：

- **iOS 启动期为 true**：`lang_loader()` 只返回文件系统来源（`kLangSourcesBootFs`），
  绝不调用游戏 0x1c44bb0 / 0x1c44df4。这是 v13 修掉 EA 卡死的核心。
- **解除信号**：主界面重建（`0x6f349c`）→ `notify_main_screen_entered()`
  → `lang_on_main_screen_entered()` → 解除闸门 + 加载数据包来源。
- **安卓恒为 false**：行为与改动前完全等价（平台隔离）。

## 未实现：实时切换对游戏本体的影响

用户实测（v18）：

| 现象 | 状态 |
|---|---|
| 语言页内文本跟随切换 | ✅ |
| 设置页其他行（Music/News…）跟随 | ❌ 仍为旧语言 |
| 游戏本体文本跟随 | ❌ 仍为旧语言 |
| 重启后游戏文本 | ❌ 仍为旧语言（但 checkbox 正确记住中文） |
| 图鉴翻页清缓存 | ✅ 不受影响 |

### 根因

`refresh_all_localized()` → `lzt_ios_schedule_lang_apply()`：
换表链钩子全部 FAIL，每次走降级分支「仅回填+重建」。

`lzt_ios_refresh_rows()` 走 `row_current_text()` → 游戏本地化口 `0x179c14`
→ 读的是**游戏当前语言表**（未换，仍为英文）→ 回填出来仍是英文。

### 待补的钩子（换表链）

| 地址 | 用途 | 现状 |
|---|---|---|
| 0x2c0bf0 | 表加载入口（hkBigTableLoad） | FAIL |
| 0x736d20 | 表名构造 | FAIL |
| 0x2c9e04 | CDN apply | FAIL |
| 0x735930 | CDN apply 循环（hkCdnApply） | FAIL |

## 目标架构（用户指定）

### 实时切换流程

1. 玩家在语言页选中新语言
2. 按 LANGUAGETYPES 中该语言的 LawnStringsType
   **替换掉游戏内当前的 LawnStringsType**
3. 触发原版游戏的 LawnStrings 换表逻辑
4. 由活动屏刷新游戏场景/UI 的字符缓存

### 重启流程

游戏重启后，在合适时机检查「游戏当前 LawnStringsType」
是否与 checkbox 对应的语言配置一致；不一致则更新。

## 已知环境坑（非代码问题，用户自行处理）

`Documents/No_Backup/CDN.2.0/LawnStrings-en-us.rton` 内容不完整（缺自定义键）。
游戏优先读它 → Tab 标题显示 `<Missing LANGUAGE_ANGLE_TITLE>`。
移走该文件后游戏改读数据包内完整表，一切正常。
**此项由用户自行提醒协测人员，代码不做处理。**

## 回滚

```bash
cp iOS/backup/v18_localize/language_module.cpp jni/
cp iOS/backup/v18_localize/lzt_main_ios.cpp iOS/src/
cp iOS/backup/v18_localize/settings_framework.{cpp,h} jni/settings/
cp iOS/backup/v18_localize/screen_bindings.cpp jni/
```

## 排查要点（本轮新增）

- `settings_framework.cpp` **不参与 iOS 构建**，其中的 `OFF_PERSISTENT_BANNER_REBUILD`
  等是安卓偏移，不可用于 iOS。iOS 偏移只在 `iOS/include/offsets_ios.h`。
- 打包时务必同步 **所有** 改动文件。曾因漏传 `lzt_main_ios.cpp` 导致
  `notify_main_screen_entered` 没进二进制，白测一轮。
- 加站点前先反汇编确认目标地址首指令非 PC 相对。
  `0x1301B08` 曾在 iOS 二进制中不存在（误用安卓偏移）。
