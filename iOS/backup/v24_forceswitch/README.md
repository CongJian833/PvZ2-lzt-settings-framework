# v24_forceswitch — 语言优先级链与重启补切（里程碑存档）

日期：2026-11-05
状态：**已验证通过**。规则①②③全部生效，重启后语言正确恢复。

## 产物

```
PvZ2_9.8.1_v24_forceswitch.ipa
110,278,307 字节
SHA256: f4d75a9f48e9839ac35fb26ab4b965dd055cb4b4be8754f99b47535a67da731b

gate: cap=409 used=16
```

## 本版解决的三个问题

### 问题 1：重启后游戏文本不跟随（核心）

**症状**：checkbox 记得中文，游戏实际是英文（或系统语言）。

**根因（日志实证）**：抢跑线程 8 秒静默后才写 LangMgr+508，
而游戏在进入主界面之前就按该字段读完备语言表 —— 写得太晚。
那 8 秒静默是为了避免「旁路线程读游戏构造中的对象」（曾导致 EA 卡死），
不能缩短。这是死结。

**解法（方案 2，用户设计）**：不改游戏内存，改为接管
「FourCC → 表名」的转换函数 `0x1AC6D50` 的入参。

```
调用约定（反汇编实证）：x8 = 输出缓冲（sret），w1 = FourCC
全程序仅 2 处调用，语义一致：
  0x040420：DisplayInfo+2104 → LangMgr → ldr w1,[x0,#508] → 本函数
  0x2C8984：同上（拼 "LawnStrings-<locale>" 前）
```

无时机要求、不写游戏内存、可随时撤销。

### 问题 2：优先级链错误（用户纠正）

原实现漏了「用户配置」这一层。正确优先级：

```
Flag  >  用户配置（lzt.lang.*）  >  游戏自身配置  >  系统语言
```

### 问题 3：write_state 短路导致补切失效

**症状**：补切日志走到 `Language set_state: no change (zh-cn)` 就断了。

**根因**：`write_state()` 开头比对的是**配置**状态，
配置本来就对（用户早选过中文），于是判定「无变化」直接返回。
但真正需要改的是**游戏内存**（当时是法语 0x46524652）。

**解法**：新增 `lzt_lang_force_runtime_switch()`，
跳过配置比对，只做运行时同步（不改配置、不改 Flag）：

```cpp
更 snprintf(g_currentLocale, ..., locale);
language_load_lawnstrings(locale);
hot_reload(locale);
lzt_settings::refresh_all_localized();
```

## 三条规则（用户指定，已实现）

| 规则 | 条件 | 行为 |
|---|---|---|
| ① | 游戏传 FourCC 时与 Flag 比对 | 相同→放行原版；不同→用 Flag 值 |
| ② | Flag 不存在 | 放行原版；主界面后若用户配置存在，补一次切换 |
| ③ | Flag 和用户配置都不存在 | 放行，用系统语言 |

**关键设计**：hook 内部绝**不访问语言表**。只读已算好的
`g_flagFourcc` 与入参比对，因此无时机要求、无锁、无分配。
语言表加载完全推迟到主界面之后，互不干扰。

（v22 的缺陷正在于此：hook 内查语言表校验，但启动早期表必然未就绪，
导致规则①永远不成立。）

## 代码改动清单

| 文件 | 改动 |
|---|---|
| `iOS/src/lzt_main_ios.cpp` | 新增 FourCC 接管 hook（`hkFourccToString` + `lzt_ios_lang_pick_fourcc`） |
| `iOS/src/lzt_main_ios.cpp` | 新增 `lzt_ios_apply_user_config_later`（规则②延迟补切） |
| `iOS/src/lzt_main_ios.cpp` | 新增 `lzt_ios_request_language_switch`（FourCC → locale） |
| `iOS/src/lzt_main_ios.cpp` | `install_fourcc_override_hook` 在 early patches 最开头调用 |
| `jni/language_module.cpp` | `language_locale_bootstrap` 按三条规则改写（不做表校验） |
| `jni/language_module.cpp` | 新增 `lzt_lang_force_runtime_switch`（跳过配置比对） |
| `jni/language_module.cpp` | `lang_on_main_screen_entered` 增加规则②③分支 |
| `jni/language_module.cpp` | 抢跑线程「无 Flag 无配置」时直接 return，不再写死英文 |

## 静态站点（16 个）

```
0  3c4c04  图鉴 ButtonDepress
1  3c44a8  图鉴 Update
2  3c5330  图鉴翻页 case0
3  3c55a4  图鉴翻页 case1
4  6f349c  主界面重建（启动期闸门解除信号）
5  757c18  createTab
6  759dd4  dispatch
7  758a94  page（子页面骨架）
8  179c14  本地化入口
9  757b78  wstring 构造器
10 758740  checkbox 构造
11 757e58  行挂载
12 2c0bf0  表加载入口
13 735930  CDN apply 循环
14 2c9e04  单表 apply
15 1ac6d50 FourCC → 字符串（本版新增）
```

## 已验证功能

| 功能 | 状态 |
|---|---|
| 语言 Tab 显示与本地化 | ✅ |
| 点击进入语言页、checkbox 列表 | ✅ |
| 语言页内文本即时跟随 | ✅ |
| 设置页其他行 / 主界面文本跟随 | ✅ |
| 图鉴翻页清缓存 | ✅ |
| **重启后语言正确恢复（规则①②③）** | ✅ v24 验证通过 |

## 已知遗留问题

### 设置页根标题不随切换刷新

- 现象：关掉设置页重开才更新
- 根因：`refresh_page_title()` 在 iOS 是**空函数**（整个实现在
  `#if defined(__aarch64__) && !LZT_PLATFORM_IOS` 内）；它依赖的 4 个偏移
  在 `offsets_ios.h` 中全为 0（**从未定位**）；
  `lzt_localizer::HostType::RootTitle` 只是枚举值，无回填器
- 另发现：置脏 `*page+288 = 1` 后，`0x75A0E4` 会走 `0x75a29c` 分支
  **直接 ret**，不重建任何东西。代码注释里「由游戏在安全点自行重建」
  的说法是错的
- 性质：**从未实现**，需完整逆向（定位根页构建器 → 标题 controller 位置
  → 文本字段 → vtable 校验 → 真机验证）
- 优先级：低（用户确认）

## 环境注意（非代码问题）

`Documents/No_Backup/CDN.2.0/LawnStrings-en-us.rton` 内容不完整（缺自定义键），
游戏优先读它 → Tab 标题显示 `<Missing LANGUAGE_ANGLE_TITLE>`。
移走后游戏改读数据包内完整表，一切正常。由用户自行处理。

系统语言若为游戏不支持的语言，游戏按「第一个受支持的语言」显示，
这是游戏自身行为，与本项目无关。

## 回滚

```bash
cp iOS/backup/v24_forceswitch/language_module.cpp jni/
cp iOS/backup/v24_forceswitch/lzt_main_ios.cpp iOS/src/
cp iOS/backup/v24_forceswitch/settings_framework.{cpp,h} jni/settings/
cp iOS/backup/v24_forceswitch/screen_bindings.cpp jni/
```
