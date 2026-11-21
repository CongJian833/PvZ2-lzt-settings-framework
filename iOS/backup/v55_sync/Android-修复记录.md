# Android 工程修复记录

> 本轮工作周期：2026-11-17 ~ 2026-11-19
> 目标：在 iOS 工程已稳定运行的前提下，验证并修复 Android 工程异常，确保不影响 iOS。
> 底包：`[OG]PvZ 2_Clean.Apk`（com.ea.game.pvz2_cln / 715 / 2.0.cln）
> IDA 数据库：`Tools/IDA/lib/arm64-v8a/libPVZ2.so.i64`（imagebase 0x0）

---

## 一、结论速览

本轮共解决 4 个问题，其中 3 个是功能缺陷、1 个是可用性问题。

| # | 问题 | 根因 | 状态 |
|---|------|------|------|
| 1 | 小屏/大文本场景缺字 | 字形缓存尺寸按屏幕最小边取 512/1024，容量不足 | ✅ 已修复，真机验证 |
| 2 | 日志每次需手动删除才能确认是新的 | 无启动时间戳，无法分辨归属 | ✅ 已修复 |
| 3 | 重启后语言不生效，每次变回英文 | 语言标识写入时机比游戏读表晚约 7 秒 | ✅ 已修复，真机验证 |
| 4 | 删 LanguageFlag 后不主动刷新语言 | 规则② 整段被 `#if LZT_PLATFORM_IOS` 排除 | ✅ 已修复，真机验证 |

另有 2 项在调查中被证伪、1 项待跟进，见 §五、§六。

---

## 二、问题 1：字形缓存缺字

### 2.1 现象与定位

大文本页面（图鉴翻页、设置长文本）出现字符缺失。根因是引擎按屏幕最小边动态选择缓存尺寸：

```
sub_1713384（EAText 缓存初始化）
0x17133F0  CMP  W23, #0x401      ; 最小边 >= 1025 ?
0x17133F4  MOV  W8, #0x800       ;   2048
0x17133F8  MOV  W9, #0x400       ;   1024
0x17133FC  CSEL W8, W9, W8, LT
0x1713400  CMP  W23, #0x281      ; 最小边 >= 641 ?
0x1713404  MOV  W9, #0x200       ;   512
0x1713408  CSEL W3, W9, W8, LT   ; ← 最终尺寸
0x1713414  BL   sub_17F3560      ; PrimeGlyphCache ctor(cache,_,_,size=W3)
```

社区教程（B 站 cv16512322）已用 A/B 实验证明该字段性质：设为 1 几乎无字，增大则字变多，**2048 是安全上限**（再大文字全白，等同设 0）。

### 2.2 关键约束：不能只改尺寸

初始实现是 hook 构造函数改入参 `a4 = 2048`。同时按 iOS 思路补了「页上限 → 4」和「页增长开关」，真机 **两个变体均闪退**。

逐层定位后确认 Android 与 iOS 结构不同：

| | iOS | Android |
|---|---|---|
| 尺寸 | `0x1cb7db0` csel→mov #2048 | `0x1713408` csel→mov #2048 |
| 页上限 | `0x1cb7e1c` mov w1,#1→#4 | `0x1713428` mov w1,#1→#4 |
| 页增长开关 | **无此机制** | `cache+0x134`，**独立开关** |

Android 多出第三项，且证据链完整：

1. `sub_1818C08`（基类构造）把 `+0xA0` 置 4、`+0x134` 清零（`0x1818C9C = STURH WZR,[X20,#0x134]`）
2. 紧接着 `sub_1713428` 的 `SetPageLimit(1,1)` 把页上限**覆盖回 1**
3. 字节扫描 `STRB [Xn,#0x134]` 全库 25 处，属本类的写点仅 `0x18192EC`（`SetParameter(id=2)`），而该调用**全库无人发起**
4. `sub_181A368`（EnsurePage）：`+0x134 == 0` → 直接返回，**永不开新页**

即：单字占 4 倍面积、页数仍锁 1 → 更快耗尽 → 「no place to put this glyph」。

### 2.3 最终方案

**只保留尺寸补丁**，页上限与页增长由 `kGlyphAllowPageGrowth = false` 门控关闭。

| 项 | 位置 | 原始 | 补丁 |
|---|---|---|---|
| 尺寸 | `0x1713408` | `0x1A88B123` CSEL W3,W9,W8,LT | `0x52810003` MOV W3,#2048 |
| 页增长 hook | `0x17F3560` | — | 调用原函数后写 `cache+0x134`（当前关闭） |

真机验证日志：

```
[CMP] glyph size -> 2048: patched @+0x1713408  1a88b123 -> 52810003
[CMP] glyph recheck: cache=0x... size=2048 limit=1 growth=0
```

结果：不崩、缺字改善、无字形告警。

### 2.4 踩过的坑（两次）

**坑 1：补丁点用错偏移。** 最初把 `OFF_GLYPH_CACHE_SIZE_SEL = 0x1713384`（函数入口）当作补丁点，运行时报：

```
[CMP] glyph size -> 2048: unexpected insn 0xa9bc5ffc @+0x1713384 (skip)
```

真实地址是 `0x1713408`（`+0x84`）。此后新增 `OFF_GLYPH_CACHE_SIZE_INSN` 存绝对偏移，并加回归测试钉死。

**坑 2：字段语义靠猜。** 曾把 `+0xA8` 当作「尺寸伴随字段」去写，实际它是 `SetParameter(id=1)` 的值（被 `sub_1819B38` 用作 glyph 对象 `+132` 的默认值），改写会造成另一类错乱。已删除该写入。

---

## 三、问题 2：日志不可分辨

### 3.1 先纠正一个误判

初判为「日志没覆写」，实测两份日志均为单次会话（文件头 / BUILD 横幅 / constructor 各出现 1 次，无 NUL 残留），证实 `fopen(path, "w")` **一直在正常截断**。

真实问题是**没有时间戳，无法分辨归属**，导致只能靠手动删除来确认拿到的是本次输出。

### 3.2 方案（对齐 iOS `lzt_core_ios.mm` 的 v14 日志策略）

1. 每次启动把旧日志改名为 `settingsframework-<yyyyMMdd-HHmmss>.log`
2. 只保留最近 5 份，更早的自动删除
3. 新日志首行写 `===== BOOT <时间戳> =====`

实现位置：`jni/lzt_core.cpp` 的 `archive_previous_log()` + `log_init()`。

归档是便利功能，任何一步失败（opendir/rename）都直接跳过，绝不影响日志本身可用。

---

## 四、问题 3 与 4：语言持久化

### 4.1 问题 3：重启变回英文

真机探针（两次启动一致）：

```
[LANGP] B(sub_11F475C) call#1 langmgr=EN-US
[LANGP] A(sub_AC8418)  call#1 langmgr=EN-US
Language switch: quiet window over, ...
Language switch: wrote 0x454E5553 -> 0x5A48434E
```

游戏在 **~1 秒**就用 EN-US 拼好了表名，而旧方案（抢跑线程）要等 **8 秒静默期**过后才写语言标识。差 7 秒，是时序上必然失败的竞态，不是概率问题。

### 4.2 修法：FourCC 接管（对齐 iOS `0x1AC6D50`）

Android 对应点是 **`sub_16A8A4C`**（`0x16A8A4C`）：纯函数，FourCC → `"en-us"` 格式化，全库仅两处调用，均为此用途。

- naked shim 用汇编保存/恢复 `x8`（sret 输出缓冲），只替换 `w1`
- 三条规则完全照搬 iOS：Flag 与入参比对（相同放行 / 不同用 Flag）/ Flag 缺失放行 / 无配置放行

**未采用**直接改 `LangMgr+0x1E4`：该字段全库 **49 处读者**（含 RSB 记录 lang2 匹配 `sub_16B2DEC`），写它会重演 v6.1 进游戏崩溃。

### 4.3 问题 4：删 Flag 后不主动刷新

真机日志：

```
Language startup: source=game-config locale=zh-cn   ← 配置读到了
[LANGX] override stats: count=0 ... flag=0x00000000 ← 但从未覆盖
```

两处缺失，缺一不可：

**缺失 1**：抢跑线程读到 game-config 后只写了 Flag 文件，没调 `lzt_android_set_flag_fourcc()`，导致 `g_flagFourcc` 恒为 0，接管 hook 里 `if (flag != 0)` 永不成立。

**缺失 2**：规则② 整段被 `#if LZT_PLATFORM_IOS` 包住（`lang_on_main_screen_entered()` 内），Android 编译时不存在。

修法：

1. 抢跑线程读到配置后补 `lzt_android_set_flag_fourcc()` + `mark_lang_ready()`
2. 规则②改为两平台共用，Android 分支走 `lzt_lang_force_runtime_switch()`

**为何不用 `write_state()`**：它在 `language_module.cpp:590` 以 `strcmp(read_state(), locale)` 短路。重启补切场景下配置本来就是对的，会被判成 `no change` 整段跳过（该函数的注释已记录这个实测教训）。

验证日志：

```
Language startup: source=game-config locale=fr-fr
[LANGX] flag fourcc=0x46524652 (hook=1)
Language startup: config fourcc=0x46524652 handed to override hook
[LANGX] override stats: count=2 ... flag=0x46524652
Language state (config): fr-fr
```

zh-cn 与 fr-fr 两种语言均验证通过，`waitMs` 稳定在 215~255ms。

### 4.4 关于「完备的失败回退」

需澄清：iOS 的规则② **没有重试机制**。`lzt_ios_apply_user_config_later()` 只做四字节 A-Z 校验，「已是目标值」则跳过，失败直接 return。

真正「完备」的是**规则③**：无 Flag 且无配置时**主动不干预**，让游戏按系统语言运行。iOS 原注释：

> 我们无法确定用户想要什么时，唯一正确的做法是【不干预】—— 不写内存、不猜语言

---

## 五、被证伪的判断（保留以供后续参考）

工作中有若干判断经真机或 IDA 证据推翻，记录在此避免重复走弯路。

### 5.1 「改字形缓存尺寸必然闪退」——证伪

该说法源自 project1 的 README，但其原文限定在「应用数据被清空（卸载 / `pm clear`）的首启路径」。实测正常启动时改尺寸并不崩、也不缺字。

修正后的表述：该补丁在**首启自举路径**上存在风险，而非普遍崩溃。

### 5.2 「页上限 `+0xA0` 是只读常量」——证伪

`+0xA0` 构造期虽置 4，但随后被 `SetPageLimit(1,1)` 覆盖。它是可变字段。

### 5.3 「日志不覆写」——证伪

见 §3.1，截断一直正常。

### 5.4 我自己的两次实现错误

| 错误 | 表现 | 修正 |
|---|---|---|
| shim 里 GOT 取值少一次解引用 | `br` 到数据段 → 秒退 | 补第二个 `ldr` |
| null 分支多写 `add sp,sp,#32` | `ldp` 已出栈，破坏调用方栈帧 | 删除，只留 `ret` |

两次都是**手写汇编未反汇编核对产物**导致。现已由 `tests/test_fourcc_shim.py` 守住。

### 5.5 「清单层改写能修复卡顿」——证伪

曾判断照 iOS 在清单层改写 tableId=131 条目可解决加载变慢。实测该判断的前提不成立：Android 的清单条目名是从 `LangMgr+0x1E4` 实时读取后拼出的，FourCC 接管生效后名字本来就是目标语言，再改写一遍结果相同。

该改写已保留（与 iOS 行为对齐，且日志会区分「确实改写」与「本就一致」），但**不是卡顿的成因**。

---

## 六、待跟进

### 6.1 `cdn_load_rton.cpp` 的注入数与断言不符

- `kInjectList` 实际 **37 项**
- 但代码注释与断言写的是 **53**（`start_diag_monitor()` 内 `53 + kInjectCount`、`final_count != 53`）

日志实测：vanilla 53 项、注入后 90 项（=53+37），说明断言里的字面量 53 是「vanilla 应有多少」，但比较逻辑写成 `final_count != 53` 会用 90 去比 53，**永不通过**，那条告警实际处于失效状态。

不影响功能，但会误导后续维护，建议修正。

### 6.2 iOS 侧未做真实编译验证

本轮改动了 `jni/language_module.cpp`（iOS 共用）。规则② 的 iOS 分支已逐行核对保持原样，预处理器嵌套配平。

但 `iOS/build.sh` 依赖 `/var/jb/` 路径，本机无法执行，**未做真实编译**。建议交 iOS 侧跑一次 `sh build.sh` 确认。

---

## 七、改动文件清单

| 文件 | 改动 |
|---|---|
| `jni/components/component_patches.cpp` | 字形缓存补丁重写；FourCC 接管 hook；规则② 平台层接口 |
| `jni/components/component_patches.h` | 注释更新 |
| `jni/components/README.md` | 补丁证据链与真机回归记录 |
| `jni/offsets.h` | 新增 `OFF_GLYPH_CACHE_SIZE_INSN` / `OFF_FOURCC_TO_STRING` 等；注释纠错 |
| `jni/language_module.cpp` | 规则② 两平台共用；抢跑线程交接 FourCC；清单层 locale 传递 |
| `jni/cdn_load_rton.cpp` | `gamestring_read()`；tableId=131 清单改写；目标语言接口 |
| `jni/lzt_core.cpp` | 日志归档 + BOOT 时间戳 |
| `jni/lzt_settings_framework_config.h` | 开关语义注释 |
| `feature_flags.conf` | 说明更新 |
| `tests/test_glyph_patch_sites.py` | 新增：补丁点与真实 .so 逐字节校验 |
| `tests/test_fourcc_shim.py` | 新增：naked shim 反汇编结构校验 |

---

## 八、回归测试

```
tests/test_fourcc_shim.py      → fourcc shim structure verified
tests/test_glyph_patch_sites.py → 0x1713408 = 0x1A88B123 -> 0x52810003
tests/test_build_modes.py      → build mode and concurrency constraints present
tests/test_lzt_guards.py       → v43 guards present
tests/test_lzt_branding.py     → branding and licensing constraints present
```

全部 `EXIT=0`。两个新增测试均做过反向验证（注入原 bug 后 `EXIT=1`），确认能拦住同类错误。

构建：arm64-v8a + armeabi-v7a，0 error / 0 warning。
打包：zipalign OK、apksigner v1+v2+v3 OK、ELF/ABI 校验通过。
