# v51 — 静态模式一致性修复

## 一、v50 为什么卡在 EA 图标

**结论：与 gate 位置无关，是构建时漏定义 `LZT_STATIC_ONLY`。**

v50 打包时主程序 22 个入口已写成 `B gate` 静态桩，但 dylib 编译时没带
`LZT_STATIC_ONLY`，于是运行期又装了一遍 inline hook。两套机制叠加。

### 故障链

```text
打包期：入口写入 B gate（静态桩）
   ↓
运行期：dylib 未开静态模式 → 再装 inline hook
   ↓
LiveContainer 把 dylib 加载到 141MB 外 → 超出 B 的 ±32MB
   ↓
And64InlineHook 退化为 20 字节长跳（LDR X17, #8 / BR X17 / imm64）
   ↓
tramp 备份的是「B gate」，执行后跳回 入口+4
   ↓
入口+4 落在长跳序列中间 → BR X17 → replacement → 无限循环
   ↓
卡死在 EA 图标
```

### 日志证据

```text
28 条 "20 bytes overwritten"        ← 长跳路径（企业签名版是 4 bytes）
stalled pc=0x109a5c384(+0x1c8384)   ← 入口 0x1c8380 + 4
stalled pc=0x10a35ad54(+0x1ac6d54)  ← 入口 0x1ac6d50 + 4
```

### 企业签名版为什么能跑

dylib 距主程序仅 23MB，`B` 够用，4 字节直接覆盖静态桩，
**静态 gate 被完全旁路**。所以「正常」是它没走到冲突路径，不是修好了。

## 二、本次改动

### 1. `iOS/tools/pack.sh`

从 `/tmp/func1.sh` 修正版写回项目。此前项目里这份是**旧版**，
缺小端字节序修正和 entitlements 修正 —— 差点被当基准用。

新增硬校验（`[0/6]` 阶段）：

```sh
DYLIB="$D/build/liblztsettingsframework.dylib"
if grep -qa "STATIC-ONLY" "$DYLIB"; then HAVE_STATIC=1; else HAVE_STATIC=0; fi

if [ -n "$LZT_RUNTIME_HOOK" ]; then
    [ "$HAVE_STATIC" = "1" ] && { echo "!! 模式冲突"; exit 1; }
    SKIP_GATE=1
else
    [ "$HAVE_STATIC" != "1" ] && { echo "!! 请 LZT_STATIC_ONLY=1 重编"; exit 1; }
    SKIP_GATE=0
fi
```

打桩步骤改为条件执行：

```sh
if [ "$SKIP_GATE" = "1" ]; then
    echo "== [3/6] 跳过静态 gate 站点（运行时 hook 模式）=="
else
    echo "== [3/6] 打静态 gate 站点 =="
    "$D/build/lztgate" "$WORK" "$MANIFEST" || exit 1
fi
```

### 2. `iOS/build.sh`

构建末尾打印模式自检：

```text
模式: 【静态】LZT_STATIC_ONLY 已启用
     dylib 只写槽位数据，不改任何代码页。

模式: 【运行时】inline hook（未定义 LZT_STATIC_ONLY）
     ⚠️  此模式的 dylib 不能与打了静态 gate 的包混用！
```

### 判定依据

`lzt_main_ios.cpp:6712` 的 `[STATIC-ONLY] static sites used=...`
位于 `#if defined(LZT_STATIC_ONLY)` 内，所以
`grep -a "STATIC-ONLY" dylib` 能可靠区分两种构建产物。

## 三、验证记录

| 场景 | 期望 | 实测 |
|---|---|---|
| 静态 dylib + `LZT_RUNTIME_HOOK=1` | 拒绝 | ✅ `!! dylib 是静态模式，但要求运行时 hook` |
| 静态 dylib + 正常打包 | 通过 | ✅ `模式: 静态 gate`，22 站点打桩 |
| `LZT_STATIC_ONLY=1 sh build.sh` | 提示静态 | ✅ `模式: 【静态】` |
| `sh build.sh` | 提示运行时 + 警告 | ✅ `模式: 【运行时】` + ⚠️ |
| 两脚本语法 | OK | ✅ `sh -n` 通过 |

## 四、产物

```text
PvZ2_v51_static.ipa
109,740,332 B
SHA256: 247aca56de6be4512a435e20177f7155b407a64cc678db2c4814e02648784922
```

包内验证：

```text
主程序 43,504,272 B
LZTGATE1 @ 0x6BD0
+0x1c8380 = 14 fb f8 17  (B gate)
+0x1ac6d50 = 58 00 95 17 (B gate)
dylib 含 STATIC-ONLY 标志
```

## 五、装机后应看到

```text
[STATIC] scanning __TEXT for gate: ...
[STATIC] gate_hdr=0x100006bd0 stride=48 count=22
[STATIC-ONLY] static sites used=22, no-site skipped=0, blocked code-page writes=0
```

**关键判据**：不应再出现 `inline hook ... successfully! 20 bytes overwritten`。
若仍有，说明静态模式又没生效。

## 六、教训

1. **环境变量型构建开关必须自校验。**
   `LZT_STATIC_ONLY` 漏设不报错，只静默退化，且退化成的新故障（死循环）
   比原故障（无 Tab）严重得多。

2. **旧脚本别留在项目里当新版用。**
   `tools/pack.sh` 曾是旧版，修正版一度只存在于 `/tmp` —— 而 `/tmp`
   本周已被系统清理三次。修正版现已写回项目。

3. **`bytes overwritten` 是重要信号。**
   4 = `B` 短跳，20 = 长跳。长跳 + 已有静态桩 = 必死。
