# 致 iOS 侧的同步说明

> 本轮 Android 侧改动了 `jni/` 下的共享源码，其中 **`jni/language_module.cpp` 是 iOS 也会编译的文件**。
> 本文列出需要 iOS 侧确认的事项。

---

## 一、必须做的事

### 1.1 编译验证

本轮改动涉及 `jni/language_module.cpp`，iOS 的 `iOS/build.sh` 会编它（见其 `SHARED_SRC` 列表）。

```bash
cd iOS && sh build.sh
```

Android 侧已做：预处理分支核验（规则② 的 iOS 分支逐行比对保持原样、`#if/#endif` 嵌套配平）。
**未做**：真实编译。`build.sh` 依赖 `/var/jb/` 路径，Android 侧机器无法执行。

### 1.2 功能回归

Android 侧改了语言启动链路，请确认 iOS 上以下场景仍正常：

- 正常启动（Flag 存在）→ 语言正确
- `Documents` 下无 Flag 文件、但配置里有语言 → 进主界面后应自动切到配置语言
- 无 Flag 且无配置 → 保持系统语言，不干预

---

## 二、改了什么

### 2.1 `jni/language_module.cpp`

**改动点：规则② 从 iOS 专属改为两平台共用。**

原代码（`lang_on_main_screen_entered()` 内）：

```cpp
#if LZT_PLATFORM_IOS
    { ...规则②... }
#endif
```

现在外层去掉平台判断，改为：

```cpp
{
    char flag[16] = {0};
    if (!read_flag_locale(flag, sizeof(flag))) {
        char cfgLocale[16] = {0};
        if (read_config_locale(cfgLocale, sizeof(cfgLocale))) {
#if LZT_PLATFORM_IOS
            const uint32_t want = lzt_settings::game::locale_to_fourcc(cfgLocale);
            lzt_core::log_write("LangOverride: rule2 config locale=%s -> 0x%08X",
                                cfgLocale, want);
            lzt_ios_apply_user_config_later(want);
#else
            lzt_core::log_write("LangOverride: rule2 config locale=%s, "
                                "force runtime switch", cfgLocale);
#if defined(__aarch64__)
            lzt_android_set_flag_fourcc(
                lzt_settings::game::locale_to_fourcc(cfgLocale));
            lzt_android_mark_lang_ready();
#endif
            lzt_lang_force_runtime_switch(cfgLocale);
#endif
        } else {
            lzt_core::log_write("LangOverride: rule3 no config, pass through "
                                "(langs=%d ready=%d)", g_langCount,
                                lang_loader().ready() ? 1 : 0);
#if LZT_PLATFORM_IOS
            lzt_ios_apply_user_config_later(0);
#endif
        }
    } else {
        lzt_core::log_write("LangOverride: flag present (%s), rule1 already handled",
                            flag);
    }
}
```

**iOS 分支的行为与原来完全一致**（`lzt_ios_apply_user_config_later(want)` / `lzt_ios_apply_user_config_later(0)` 均保留在原位置）。

新增的只有：
- 规则③ 的日志多带了 `langs=` / `ready=`，用于区分「确实无配置」与「表未就绪」
- 规则② 成功分支的日志（Android 才有）

### 2.2 其他 `jni/` 改动

以下文件 iOS **不编译**（不在 `SHARED_SRC` 里），仅供了解：

| 文件 | 用途 |
|---|---|
| `jni/components/component_patches.cpp` | 组件补丁（Android 专属） |
| `jni/cdn_load_rton.cpp` | CDN 清单注入（Android 专属） |
| `jni/lzt_core.cpp` | Android 日志实现（iOS 用 `lzt_core_ios.mm`） |
| `jni/offsets.h` | iOS 由 `iOS/port/include/offsets.h` 拦截，不读这份 |

---

## 三、Android 侧发现的结论（对 iOS 有参考价值）

### 3.1 「改字形缓存尺寸会闪退」这个说法要限定范围

`jni/components/README.md` 里曾写「v2.1.0 真机实测：本补丁会造成堆破坏」，真机实测后发现该结论需要限定：

- 它发生在**应用数据被清空（卸载 / `pm clear`）后的首启路径**
- 正常启动时改尺寸**不崩、也不少字**

Android 侧最终只保留了尺寸补丁（2048），页上限与页增长关闭——因为二者组合会在真机闪退，且原因已定位（见下）。

### 3.2 Android 与 iOS 的字形缓存结构不同

iOS 是「尺寸 + 页上限」两条指令补丁即可。Android 多一个**独立的页增长开关** `cache+0x134`（`SetParameter(id=2)`），全库无人开启，导致页上限形同虚设。

如果 iOS 侧未来也遇到「页上限改了但没效果」，可参考这个思路：确认是否存在类似的条件开关。

### 3.3 页面加载变慢与语言标识有关（已确认为网络等待）

切换语言后，游戏启动时会去找 `LawnStrings-<locale>.rton`。若 CDN 目录没有该文件，游戏会发起一次网络查询后再回退数据包——这是「Récupération du CDN」变长的原因。

Android 侧的最终状态：清单层按目标语言改写条目名（与 iOS `hkCdnListAdd` 同构），实测加载恢复正常。

---

## 四、没有做的事

| 项 | 原因 |
|---|---|
| 真实 iOS 编译 | `build.sh` 依赖 `/var/jb/`，Android 机器无法执行 |
| iOS 真机回归 | 无 iOS 设备 |
| `iOS/` 目录下任何文件 | 本轮未改动 |

---

## 五、如果 iOS 编译报错

最可能的位置是 `language_module.cpp` 的规则② 块。请把报错行号发回，Android 侧同步修正。

Android 侧已确认编译通过（arm64-v8a + armeabi-v7a，0 error / 0 warning），但两平台的 `#if` 分支不同，不能直接外推。
