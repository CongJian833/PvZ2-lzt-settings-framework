# v50 — LiveContainer 下 gate 被覆盖的修复

## 一、现象

v48 / v49 在 LiveContainer 中运行：

- dylib 正常加载
- 设置模块注册正常（`[SFW] module registered: anchor=6 slot=1/2`）
- tab id 正常 finalize（`tab id finalized: 30` / `31`）
- **但设置页没有新 Tab**

日志关键行：

```text
[STATIC] main image resolved via mainBundle path (idx=101)
[STATIC] gate_hdr=0x0 stride=0 count=0
[STATIC] no gate found; image head magic=feedfacf filetype=16777228 ncmds=81
[STATIC-ONLY] static sites used=0, no-site skipped=26
```

而静态字节补丁（字形缓存、戴夫动画、传送带）全部正常识别：

```text
[CMP] glyph cache forced to 2048
[CMP] dave idle-anim patch: already static
[CMP] conveyor count patch: already static
```

说明加载的确实是配套主程序，问题只出在 gate 扫描。

## 二、根因

原 gate 区位于文件的 `0x2330`，正好紧跟 `sizeofcmds` 之后：

```text
sizeofcmds = 0x2308
gate       = 0x2330      ← 中间只有 0x28 = 40 字节
首个 section __text = 0x7000
```

LiveContainer 会向主程序注入 2 条 load command（`ncmds` 从 79 变为 81）。新增的 LC 从 `sizeofcmds` 处向上生长，**40 字节的间隔完全不够，直接覆盖了 `LZTGATE1` 魔数**。

日志里的 `ncmds=81` 就是直接证据。

v49 修对的是"扫哪个镜像"，但扫对了镜像时 gate 数据本身已经被踩掉，所以仍然失败。

## 三、方案

把 gate 从「`sizeofcmds` 之后」搬到「`__TEXT` 首个 section 之前的 slack 末尾」。

```text
0x2308 ─ sizeofcmds 结束
         │
         │  18,632 字节缓冲（LC 增长空间）
         │
0x6BD0 ─ gate 区起点（LZTGATE1 + 16 字节头 + 22×48 站点）
0x7000 ─ __text 起点
```

gate 区共 1,072 字节（16 字节头 + 22 站点 × 48 字节），贴近 `0x7000`。

这样 LC 注入若要再次踩到 gate，必须增长约 18KB —— 现实中不可能发生。

### 为什么不用 `__TEXT` 段尾

曾尝试放置于 `__eh_frame` 之后的段尾零区 `0x217b940`（可用 1,728 字节）。位置完全稳定，但**失败**：

```text
B out of range: 8390575
```

ARM64 `B` 指令使用有符号 26 位立即数，范围是 ±2^25 条指令 = **±33,554,432 字节（±32MB）**。

站点 `0x179c14` 到 `0x217b940` 的距离是 33,561,900 字节，**超出 7,468 字节**。

改为 `0x6BD0` 后，最远站点 `0x1ac6d50` 距离 28,049,792 字节，22/22 全部合规。

## 四、改动清单

### `iOS/tools/lztgate.c`

gate 落点计算：

```c
// 旧：紧跟 load commands
uint64_t gate_off = (sizeof(struct mach_header_64) + h->sizeofcmds + 15) & ~15ULL;

// 新：首个 section 之前的 slack 末尾
uint64_t tail_off   = (first_sect > need) ? ((first_sect - need) & ~15ULL) : 0;
uint64_t tail_avail = (first_sect > tail_off) ? (first_sect - tail_off) : 0;
```

设备实测输出：

```text
gate location: __TEXT tail zero region @0x6bd0 (avail=1072 B, need=1072 B)
gate area: file 0x6bd0 vm 0x100006bd0 cap=22 used=22 stride=48
ok: 22 hooks, size unchanged 44067264
```

### `iOS/src/lzt_main_ios.cpp`

gate 扫描范围从固定窗口改为整个 `__TEXT`：

```c
uintptr_t scan_lo = (uintptr_t)h + sizeof(struct mach_header_64) + h->sizeofcmds;
scan_lo = (scan_lo + 15) & ~(uintptr_t)15;
uintptr_t scan_hi = text_end;

for (uintptr_t a = scan_lo; a + 16 <= scan_hi; a += 4) {
    if (memcmp((const void*)a, "LZTGATE1", 8) != 0) continue;
    uint32_t stride = *(const uint32_t*)(a + 8);
    uint32_t count  = *(const uint32_t*)(a + 12);
    if (stride == 0 || stride > 4096 || count > 4096) continue;
    g_lzt_gate_hdr = a;
    g_lzt_gate_stride = stride;
    g_lzt_gate_count = count;
    break;
}
```

原实现写死 `0x800..0x20000`，新 gate 在 `0x6BD0` 虽然在范围内，但为将来留出余量，改为扫全段。

新增诊断日志：

```text
[STATIC] scanning __TEXT for gate: 0x1023008..0x10217c000 (ncmds=79 sizeofcmds=8968)
```

## 五、过程中修掉的三个 bug

### bug 1：section 遍历未过滤段名

```c
// 错
if (lc->cmd == LC_SEGMENT_64) { 统计所有段的 section }
```

把 `__DATA` 的 section 也算进来，`last_sect_end` 被拉到 `0x27eb870`，算出 `avail=35,110,912` 的假值。

修正为只看 `__TEXT`：

```c
if (strcmp(s->segname, "__TEXT") != 0) { o2 += lc->cmdsize; continue; }
```

### bug 2：`continue` 前未推进偏移

第一版修正写成：

```c
if (strcmp(s->segname, "__TEXT") != 0) continue;   // ❌ o2 没推进
```

循环卡在第一个 `LC_SEGMENT_64`（`__PAGEZERO`，`nsects=0`）上无限重复，`last_sect_end` 恒为 0。

### bug 3：段尾位置超出 B 指令范围

见上文第三节。最初想用最稳的段尾位置，被 `B out of range` 挡回来。

## 六、产物

```text
PvZ2_v50_lcgate.ipa
大小：109,740,332 B
SHA256：ce50219c577e544e2ab69973491f74c9e922ec44590e7ec9017cf183392fe7ef
```

主程序检查：

```text
大小：43,504,272 B（arm64 only，与 v48/v49 一致）
LZTGATE1 @ 0x6BD0 (27600)
stride = 0x30 (48)
count  = 0x16 (22)
```

入口打桩抽查（均为 `B` 指令）：

```text
0x3c4c04: f7 07 f1 17    // 图鉴按下
0x757c18: 2e bc e2 17    // 设置 Tab 创建
0x756ae4: ff c0 e2 17    // 设置页初始化
```

## 七、验证结果

用户设备 LiveContainer 实测通过：

- 设置页出现两个新 Tab
- 语言（slot=1，tab id 30）
- 视角（slot=2，tab id 31）
- 语言切换、视角缩放、恐龙补偿、戴夫动画、传送带计数全部正常
- No_Backup 符号链接兜底正常

日志确认：

```text
[SFW] module registered: anchor=6 slot=1 reqTab=0
[SFW] module registered: anchor=6 slot=2 reqTab=0
[SFW] tab id finalized: 30 (anchor=6 slot=1)
[SFW] tab id finalized: 31 (anchor=6 slot=2)
banner hook(0x6978f8): ok
```

## 八、经验

1. **Mach-O 头部 slack 不是安全存放区。**
   任何会重写 load command 的工具（LiveContainer、重签名器、加固、脱壳）都可能踩到。
   必须放数据时，尽量贴近首个 section，与 `sizeofcmds` 拉开余量。

2. **ARM64 `B` 的范围是 ±32MB，不是 ±128MB。**
   混淆点在于 26 位立即数 × 4 字节 = 2^28 = 256MB 总量，但**有符号**范围是 ±2^27 = ±128MB？
   实际编码：`imm26` 乘以 4，`imm26` 是有符号 26 位（范围 -33,554,432 ~ +33,554,428 字节）。
   即 **±32MB**。布局 trampoline 时必须先算距离。

3. **过滤条件里的 `continue` 要检查循环变量推进。**
   这个 bug 的表现是"看起来跑完了但结果全 0"，比崩溃更难发现。

4. **段遍历必须显式过滤段名。**
   `last_sect_end` 这类"最后一个 section"语义，在 `__DATA` 参与时会彻底失真。
