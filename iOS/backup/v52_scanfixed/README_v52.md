# PvZ2 设置框架 v52 发布说明

## 一、版本信息

```text
文件名：PvZ2_v52_scanfixed.ipa
大小：  109,740,380 B
SHA256：f8ca599aeaa1361467473c5fd1c83781f2fb283223970fb1a68c986da564d0a1
```

基于 v48（iOS 11 最低版本）修复 LiveContainer 兼容性。

## 二、本次解决的问题

v48/v49 在 **LiveContainer** 中运行时，设置页没有新 Tab。越狱直装环境下一直正常。

原因是三处独立缺陷叠加：

| 版本 | 缺陷 | 状态 |
|---|---|---|
| v49 | 扫错镜像（取第一个 `MH_EXECUTE` 会命中容器宿主） | 已修 |
| v50 | gate 数据位于 Mach-O 头部 slack（`0x2330`），被容器注入的 load command 覆盖 | 已修 |
| v51 | 扫描范围把链接期 VM 地址当运行时指针，范围反向导致循环空转 | 已修 |

### gate 数据为什么会被覆盖

原 gate 区在文件偏移 `0x2330`，紧跟 `sizeofcmds`（`0x2308`）之后，中间只有 **40 字节**。

LiveContainer 会向主程序注入 2 条 load command：

```text
越狱直装：ncmds=79, sizeofcmds=8968
LiveContainer：ncmds=81, sizeofcmds=9072   （+104 字节）
```

104 字节远大于 40 字节的余量，`LZTGATE1` 魔数被直接覆盖，扫描自然落空。

### 现在的 gate 位置

搬到 `__TEXT` 首个 section（`__text`@`0x7000`）之前的 slack 末尾，文件偏移 **`0x6BD0`**：

```text
0x2308 ─ sizeofcmds 结束
         │
         │  约 18,632 字节缓冲
         │
0x6BD0 ─ gate 区（16 字节头 + 22 站点 × 48 字节 = 1,072 字节）
0x7000 ─ __text 起点
```

容器要再次踩到 gate，需要注入约 18KB 的 load command，实际不会发生。

## 三、双环境验证结果

### 环境 A：LiveContainer（越狱宿主内）

```text
main __TEXT base = 0x109128000  (.../Applications/com.popcap.ios.PvZ2.cln.app/PvZ2)
[STATIC] scanning __TEXT for gate: 0x109096390..0x10b210000
[STATIC] gate_hdr=0x10909abd0 stride=48 count=22
[STATIC-ONLY] static sites used=19, no-site skipped=7, blocked code-page writes=0
tab id finalized: 30 (anchor=6 slot=1)
tab id finalized: 31 (anchor=6 slot=2)
```

### 环境 B：企业签名直装

```text
main __TEXT base = 0x10443c000  (/private/var/containers/Bundle/Application/.../PvZ2.app/PvZ2)
[STATIC] scanning __TEXT for gate: 0x10443e330..0x1065b8000
[STATIC] gate_hdr=0x104442bd0 stride=48 count=22
[STATIC-ONLY] static sites used=19, no-site skipped=7, blocked code-page writes=0
```

两种环境的站点统计完全一致，说明 gate 定位与 ASLR/容器重定位无关。

**关键指标**：两环境下 `inline hook ... bytes overwritten` 均为 **0 条**，即运行期完全不修改代码页。

## 四、功能清单

以下均在两种环境下验证通过：

### 设置页面

- 语言 Tab（slot 1，tab id 30）
- 视角 Tab（slot 2，tab id 31）
- 根标题随语言切换
- 横幅随语言刷新
- 行文本实时切换

### 语言

- 主语言表加载（`0x2c0bf0`）
- CDN 应用循环（`0x735930`）
- 单表应用（`0x2c9e04`）
- 图鉴翻页清缓存
- News 语言短码映射

### 视角

```text
view hooks: BoardZoom2 hooked (force high view)
view hooks: BoardLayout_ApplyZoom hooked (align)
view hooks: StreetDinos hooked (xBase compensate)
view hooks enabled=1
```

### 静态字节补丁

```text
[CMP] glyph cache forced to 2048 (boot)
[CMP] glyph page limit -> 4 (boot)
[CMP] dave idle-anim patch: already static @+0x1a7440
[CMP] conveyor count patch: already static @+0x31585c
[CMP] almanac depress/update hook installed @+0x3c4c04, +0x3c44a8
[CMP] almanac page hooks installed (2/2) @+0x3c5330, +0x3c55a4
```

60fps 由静态 frame-interval 补丁承担（`dl4 hook skipped`）。

### 其他

- 启动期不永久卡死
- No_Backup 符号链接兜底

## 五、系统兼容性

| 系统版本 | 状态 |
|---|---|
| iOS 10.x | dylib 不加载，全部插件失效 |
| iOS 11.0 / 11.1 | 正式功能全部生效，仅默认关闭的 FPS/Metal 探针受限 |
| iOS 11.2 – 12.5 | 全部生效 |
| iOS 15+ | 全部生效 |

`MinimumOSVersion` 为 `11.0`，dylib 使用 `-mios-version-min=11.0` 编译，两者一致。

相机对齐只由屏幕比例阈值决定（`2.16666`），与系统版本无关。

## 六、已知缺失站点

日志中的 7 条 `hook SKIPPED` 记录均属已知冗余，不影响功能：

| 站点 | 说明 |
|---|---|
| `1c310e8` `1c32040` `1c36564` | No_Backup 主路径，符号链接兜底有效 |
| `757a90` | 音量滑块行标题，已被语言切换机制覆盖 |
| `17a480` | 哈希表 find 兜底 |
| `75b128` | 行初始化诊断探针 |
| `dl4` | 60fps DisplayLink 探针，由静态 frame-interval 补丁替代 |

### 关于 `static sites used=19`

manifest 共 22 条，`used=19` 表示本次运行实际走了 19 次静态站点填充。

差额 3 的来源：部分站点按运行时条件安装，未触发的不会被计入。例如视角三站（`183b10`/`183a58`/`5eddb4`）仅在 `lzt_view_hooks_set_enabled(true)` 时才安装——默认关闭时不计入。

这不是缺陷，但如果哪天 `used` 明显偏离 19（例如降到个位数），应视为异常信号：说明有站点在打包期没写上、或 gate 数据部分损坏。可对照 `no-site skipped` 是否同时激增来判断。

## 七、构建注意事项

### 必须使用静态模式编译

```bash
cd iOS && LZT_STATIC_ONLY=1 sh build.sh
```

漏掉该变量会导致 dylib 走运行时 inline hook，与打包期写入的静态桩叠加，在 LiveContainer 下 **死循环卡在 EA 图标**（v50 实测）。

`build.sh` 会在构建末尾打印当前模式；`tools/pack.sh` 会在打包前硬校验，不一致直接退出。

### 打包命令

```bash
OUTNAME=xxx.ipa MANIFEST=iOS/tools/manifest.txt sh iOS/tools/pack.sh
```

## 八、产物验证方法

```bash
# gate 存在性
grep -abo "LZTGATE1" Payload/PvZ2.app/PvZ2
# 期望：27600  (= 0x6BD0)

# gate 头部
dd if=Payload/PvZ2.app/PvZ2 bs=1 skip=$((0x6bd0)) count=16 | od -An -tx1
# 期望：4c 5a 54 47 41 54 45 31  30 00 00 00  16 00 00 00
#       L  Z  T  G  A  T  E  1   stride=48   count=22

# 入口打桩（应为 B 指令，最高字节 0x17）
dd if=Payload/PvZ2.app/PvZ2 bs=1 skip=$((0x757c18)) count=4 | od -An -tx1
# 期望：xx xx xx 17
```

## 九、故障排查

安装后若无新 Tab，按顺序检查日志：

1. **`gate_hdr` 是否为 0**
   - 非 0 → gate 定位正常，转第 2 步
   - 为 0 → 看 `[STATIC] scanning __TEXT for gate: A..B`，若 A > B 则扫描范围计算有误

2. **是否有 `inline hook ... successfully`**
   - 有 → dylib 未以 `LZT_STATIC_ONLY` 编译，重新构建
   - 无 → 转第 3 步

3. **`static sites used` 数值**
   - 接近 19/22 → 正常
   - 为 0 → gate 数据损坏，检查 `LZTGATE1` 是否仍在 `0x6BD0`

4. **tab id 是否 finalize**
   - 有 `tab id finalized: 30/31` 但设置页无 Tab → 检查 `shouldCreate` 逻辑与语言表就绪状态
