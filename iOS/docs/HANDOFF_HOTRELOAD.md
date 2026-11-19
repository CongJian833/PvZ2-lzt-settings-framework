# 任务交接文档（iOS 语言热重载 — 阶段交接）

> 用途：上下文窗口迁移时的交接。**新窗口请先读本文件，再读 HANDOFF.md 与 FINDINGS.md。**
> 本文件记录「语言热重载」这一阶段的突破性进展与当前状态，比 HANDOFF.md 更新。
> 最后更新：语言热重载主链路已打通（含 ApplyLocalizedStrings 定位）。

---

## 一、一句话现状

**iOS 语言热重载的完整链路已经打通并真机验证**（含设置页全部行）：
写语言标识 → 用游戏自己的装载 API 装载目标语言表（CDN 优先/数据包兜底）
→ 调用游戏自己的 `ApplyLocalizedStrings` 把表灌进本地化容器 → 回填已加载控件。
切换后**新解析的文本**与**已显示的所有设置行**（Tab 行 / Music / Sound FX / checkbox 行）
都会跟切（中↔英双向），且无闪退。

可用版本备份：
- `iOS/backup/src/lzt_main_ios.cpp.hotreload-ok`（主链路打通）
- `iOS/backup/src/lzt_main_ios.cpp.rowfix-ok`（滑条行也跟切 + 诊断清理完毕，最新稳定）

---

## 二、本阶段最大的三个发现（务必保留）

### 1. iOS 版 ApplyLocalizedStrings = **0x760a20**（此前是 TODO，是卡了很久的最后一环）

这是「把 table 131 的键值对灌进本地化容器」的函数。安卓侧对应 `sub_13D0F30`。
- **无参数**，序言直接 `bl 0x21fd8` 自取 PVZDB 单例
- 取表 131：`mov w1,#131` → `bl 0x760bb4`（sret 走 x8）
- 空表判据：取 `[表+8]`/`[表+0x10]` 求差 → 按 entry 步长 **48** 求条数 → `cmp #1; b.lt` 返回
- 容器 owner：`*(base+0x27f4ed0)`（与查询口 0x179c14 用的是**同一处**，容器同一性成立）
- 写容器：`bl 0x1abbb60(owner, &key, &value)`
- 结尾清表：`bl 0x21fd8` → `w1=131` → `bl 0x2ca3e0`

**为什么它关键**：只做「装载」时数据停在 table 131，容器（查询口读的 `manager+1984`）
不会更新 → 表现为「装载成功但查询仍是旧语言」。补上这一步容器才真正换语言。

### 2. 表装载 API = **0x2c9e04**（= `PVZDB::LoadPackageForTableFromRTONFile`）

签名：`(x0 = 上下文, w1 = tableId(131), x2 = 路径/名字的 std::string 对象)`，返回非 0 即成功。
- **外置优先**：CDN 目录存在 `LawnStrings-<locale>.rton` → 传**绝对路径**
  （实测必须绝对路径，游戏才会读外置文件）
- **数据包兜底**：CDN 无该文件（如 en-us，英文表内置在数据包）→ 传
  `packages\LawnStrings-<locale>.rton`（**前缀必须有**，安卓答复 §6 同款结论）
- 上下文 `ctx` 由启动期捕获（hook 0x2c9e04 时记录，存在 `g_applyCtx`）

### 3. 「按形态推断捕获行」是危险做法（两次崩溃的根因）

早期为覆盖 Tab 行之外的设置行，用「钩行挂载 + 内存形态猜测标题字段」的路径。
结果：某些对象在 +184 放的是**次级 vtable 指针**（形态与长串一样：指针+长度），
被当标题写入后，游戏调用该 vtable 跳到垃圾地址崩溃
（实测 crash addr=0x730000005d，正好是被写入的文本字节）。
另有一次是扫描范围放宽到 176..200，命中了 +192（wstring 的 size 字段自身）。

**结论**：捕获必须限定在「确认是设置行的对象」上（createTab / checkbox 钩子），
或经「容器遍历 + 文本比对」安全认领，**不得做内存形态推断**。
安卓不存在此问题：它有统一的行初始化器 `sub_A54230(row, 标题键)` 作为锚点。
**iOS 没有这个统一锚点**（各行创建函数各自写 +184），这是平台差异。

### 4. 两个曾导致崩溃的坑（已修，勿重犯）

- **重复挂钩子**：启动阶段装了行挂载钩子 (0x757e58)，页面物化里又装一次 →
  A64HookFunction 二次改写已打补丁的入口 → 跳转错乱 →
  **表现为「只有语言/视角 Tab 崩，原版 Tab 正常」**。所有钩子安装必须真幂等。
- **单独 hook 0x757ff8（滑条行创建）**：该函数首指令含 PC 相对寻址，
  inline hook 后其函数体内部会崩（crash pc 落在函数体内+0x7582dc）。已放弃该方案。

---

## 三、当前代码结构（iOS/src/lzt_main_ios.cpp，约 3640 行）

### 主链路（保留，勿删）
| 功能 | 入口/实现 |
| --- | --- |
| 语言标识写入 | 语言模块 `write_langmgr_locale()`：`*(*(DisplayInfo)+2104)+508` |
| 装载语言表 | `lzt_ios_reload_cdn_by_locale()`：CDN 绝对路径 或 `packages\...` |
| 灌进容器 | `lzt_ios_apply_localized_strings()` → **0x760a20** |
| 收尾顺序 | `lzt_ios_schedule_lang_apply(page)`：主队列内「先换表 → 再回填 → 最后置脏」 |
| 行文本回填 | `lzt_ios_refresh_rows(page)`：遍历捕获表，用**游戏本地化口**取当前语言文本写回 |
| 行捕获（Tab 行） | 行初始化器钩子 `hkRowInit` @**0x75b128**(row, 键) → `lzt_row_init_capture` |
| 行捕获（滑条/checkbox 行） | 行挂载钩子 `hkRowAttach` @0x757e58 → `try_claim_row_by_text(row)`（**严格文本精确匹配**） |
| 键编码 | **UTF-16LE** 宽串（`read_u16_key`）；按窄串读会截断 |
| 认领校验 | `lzt_ios_capture_row_by_key`：只登记「字段文本 == 键当前语言文本」的位置 |
| 取值口 | `row_current_text(bareKey,...)` → 0x179c14（**键必须裸键**，内部会补方括号） |

### 关键开关（当前值 / 说明）
```cpp
kEnableLangTableReload   = true    // 主链路：装载 + 回填
kEnableApplyHook         = true    // 0x2c9e04 钩子（提供装载上下文，功能必需）
kEnableModuleTextOverride = false  // 本模块自解析表下发（已关；A 路线生效后不需要）
kEnableManifestReapply   = true    // 重跑 CDN 应用循环（兜底）
// 以下均为诊断探针，默认 false：
kEnableFileProbe / kEnableLocalizeProbe / kEnableApplyProbe /
kEnableApplyProbe2 / kEnableTblLoadProbe / kEnableCdnManifestProbe /
kEnableProbeHooks / kEnableTextTableScan
```

### 调试入口（默认关闭，注意清理）
- `lzt_ios_maybe_autolang()`：若存在 `<Documents>/lzt_autolang.txt` 则周期性自动切语言。
  **正式使用务必删除该文件**（本次已删）。
- `lzt_lang_switch_now(locale)`（在 jni/language_module.cpp）：程序化触发一次切换，
  等价于点语言 checkbox，供无人值守验证。

---

## 四、环境与操作（已变更，注意）

- 游戏包（重装后**已变**）：`/var/containers/Bundle/Application/D59F5916-2310-4762-AF40-03DDA143EEB1/PvZ2.app`
  （`iOS/inject.sh` 里的 APP_ID 已更新为此值）
- 数据容器（**已变**）：`/var/mobile/Containers/Data/Application/00FF737F-A94D-48DD-A321-8D37742B9234`
  - 日志：`<容器>/Documents/lztsettingsframework.log`（每启覆盖）
  - 崩溃：`<容器>/Documents/lztsettingsframework_crash.log`（追加）
  - CDN 目录：`<容器>/Documents/No_Backup/CDN.2.0/`
    - `LawnStrings-zh-cn.rton` ← **中文表（实际是完整表，含英文文本；游戏内置表提供英文）**
    - `LANGUAGETYPES.rton`
- 启动游戏（**推荐，全自动**）：`sudo uiopen -b com.popcap.ios.PvZ2.cln`
  - 注意：`sudo` 密码 123456；用 shell 直接跑二进制会停在早期阶段（不完整），**必须用 uiopen**
- 关闭游戏：`for p in $(ps -A -o pid,comm | grep "PvZ2.app/PvZ2" | ...) ; do sudo kill -9 $p; done`
- 部署（只换 dylib，无需重签主程序）：
  ```sh
  sudo cp -f iOS/build/liblztsettingsframework.dylib <app>/Frameworks/
  sudo chown _installd:_installd <app>/Frameworks/liblztsettingsframework.dylib
  sudo chmod 755 <app>/Frameworks/liblztsettingsframework.dylib
  ```
  （主二进制未变时不需要跑整个 inject.sh；整体重签会因内存不足 Killed:9）
- 编译：`cd iOS && sh build.sh`

### 工具
- `iOS/tools/xref.c`：字符串交叉引用（按 ASCII）
- `iOS/tools/wfind.c`：**新增**，UTF-32 宽串查找 + 定位 adrp+add 引用
  （用法：`wfind <macho> OPTIONS_MUSIC_LABEL`；注意 `__TEXT` 为 vmaddr 0x100007000）
- 全量反汇编：`llvm-objdump-16 --disassemble /tmp/pvz.arm64 > /tmp/pvz.dis`（已存在）
- 定点反汇编：`llvm-objdump-16 --disassemble --start-address=0x... --stop-address=0x... /tmp/pvz.arm64`
  ⚠️ 地址规则：文件里 vmaddr 已是 `0x100000000+偏移`；运行时地址 = `base + 偏移`，
  其中 `base = slide + 0x100000000`（**base + 偏移**才是对的，勿重复加 4GB）

---

## 五、未完成 / 待办（按优先级）

1. ~~Sound FX / Music 行未跟切~~ **已解决**。做法见下方专节。
2. ~~清理诊断日志~~ **已完成**。
3. ~~主界面 / 主界面横幅~~ **已解决**（本轮）。做法见下方专节。
4. **News 页本地化补丁**（下一项工作）：安卓侧 `screen_bindings.cpp` 有
   「News 取键兜底」（自定义语言在引擎的语言白名单外，取键产出空串 → 页面空白）。
   iOS 侧尚未定位对应链，需按安卓的线索重新逆向（安卓 hook `sub_145BE84`，
   在 key 为空时改用当前 locale 首段后缀作替身键）。
5. **中文表补模块键**：`LANGUAGE_ANGLE_TITLE`、`LANGUAGE_ENGLISH`、`LANGUAGE_SIMPLIFIED_CHINESE`
   等在 CDN 中文表里不存在 → 显示 `<Missing ...>`。需在表里补上（已知且预期内）。
6. 真机完整验收（设置页 / 子页 / 主界面 / News / 重启前后一致性），然后更新 HANDOFF.md 与 FINDINGS.md。

### 主界面 / 主界面横幅解决方案（已完成，勿回退）

| 项 | 地址/做法 |
| --- | --- |
| 横幅重建 | **0x6978f8**(a1 = 横幅对象)：内部用键 `[PERSISTENT_MESSAGING_OFFLINE]` 等取值建控件 |
| 主界面重建 | **0x6f349c**(a1 = 主界面对象)：内部用键 `[MAINMENU_PLAY]` 等建按钮，文本写到对象 +184 附近 |
| 刷新方式 | hook 上述两个函数记录最新对象（带 vtable 存活校验），切语言时**重新调用**它们 |
| 顺序要求 | 必须在【换表之后】执行，否则用旧表取值 → 文本不变（早期放在同步路径导致横幅/按钮不更新） |
| 遮挡问题 | 主界面重建会新建顶层节点、盖住设置页（实测）。**方案 A**：设置页打开期间只置「待刷新」标志，
| | 待**关闭设置页**（dispatch 的 `tabId==0` 事件）时再统一刷新。注意：返回设置根视图也会走
| | 「离开模块页」分支，因此不能以它作为触发条件（实测会被误触发）。 |

### 滑条行（Music / Sound FX）解决方案（已完成，勿回退）

三个要点：
1. **锚点**：iOS 无统一的「行初始化器」—— 0x75b128(row, 键) 只覆盖 Tab 行与部分设置行，
   **滑条行不经过它**（日志实证：rowinit 中从无 OPTIONS 键）。
2. **键的编码**：行的标题键是 **UTF-16LE 宽串**（libc++ long string 布局：ptr@+0、len@+8 字符数、
   cap@+16 最高位=1）。按窄串读会在首个 0x00 截断（只得到 "["）—— 必须用 `read_u16_key()`。
3. **认领规则（关键，务必严格）**：行挂载 (0x757e58) 是所有行的共同落点，在挂载时用
   「近期键的当前语言文本」在行对象内做 **精确匹配**（`lzt_ios_capture_row_by_key` 只登记
   「字段文本 == 该键当前语言文本」的位置）。**绝不可**无条件接受固定偏移 +184：
   早期这样做导致同一页多行被绑成同一个键（实测：删除账户/消息与更新/音效/音准 全被绑成
   SETTINGS_TITLE，切语言后全变成 "Settings"），既错乱又可能写坏非标题字段而崩溃。

两个必须避开的坑：
- 认领代码不能在 `if (!active) return;` 之后 —— 原版设置页 active 恒为 false，会永不执行（实测）。
- 不要在 dispatch 调用栈内读页面内容容器（page+240）—— 容器尚在构建，指针无效会崩
  （实测 pc=+0x75a10c）；如确需扫描须投递主队列延后。

---

## 六、方法论要点（本阶段验证有效）

- **先拿日志证据再改代码**：本阶段所有突破都来自「探针 → 日志 → 定位」。
- **跨平台求助**：向安卓侧提问并拿到了关键答复（见
  `iOS/docs/ASK_ANDROID_APPLY_LOCALIZED.md`），答复文档由用户转述
  （要点：apply 无参、entry 向量遍历、容器 owner 同一性验证、装载名必须带 `packages\` 前缀）。
- **改完立刻编译 + 部署 + 实机验证**，不要连续做多个未验证的改动。
- 崩溃先看 `lztsettingsframework_crash.log` 的 `NATIVE_CRASH_PC ... bt:` 行（含相对偏移）。