# v53 — gate 三层加固

## 一、为什么加固而不是改架构

评估过「把 gate 搬进 dylib」的方案，结论是**不可行**，原因有两条硬约束：

1. **入口跳转无法回避。** 无论 trampoline 放哪，主程序入口那 4~20 字节都必须静态写入，这一步省不掉。
2. **打包期不知道 dylib 的运行时基址。** 入口若直接跳 dylib，目标地址无法计算；想靠 dyld 重定向也不行，游戏内部函数无符号，`fishhook`/interpose 拦不到。

现有设计恰好绕开了这两点：gate 桩留在主程序内（地址可算），桩里用 `adrp` 取槽（槽在主程序 `__DATA`，地址同样可算），槽里才是 dylib 函数地址。

**所以 gate 在主程序里不是权宜之计，而是约束推出来的结果。** 真正值得补的是自检能力。

## 二、三层加固

### 第 1 层：`lztgate.c` 落点硬校验

新增 `MIN_LC_SLACK = 4096`，并要求：

- gate 区必须位于 `sizeofcmds` 之后
- 与 `sizeofcmds` 之间至少留 4KB
- 不得越过首个 section 起点

**同时删除了「回退到紧贴 sizeofcmds」的旧路径。**

那条路径正是 v50 故障的根源：余量不足时悄悄退回危险位置，而不是报错。现在直接 `return 1`，把问题暴露在打包期。

实测输出：

```text
gate location: __TEXT slack @0x6bd0 (need=1072 B, LC slack=18600 B, first_sect=0x7000)
```

4KB 的取值依据：LiveContainer 实测注入量约 104 字节，4KB 留约 40 倍余量。

### 第 2 层：`pack.sh` 打包后 gate 完整性自检

新增 `[4d/6] gate 完整性自检`，在打桩后、重签前执行：

```text
magic @0x6bd0（唯一）
stride=48 count=22 ✓
距 sizeofcmds 18600 字节（≥4096）✓
全部入口均为 B 指令 ✓
```

四项任一失败即 `exit 1`，不出包。

这层针对的是 v50 出现过的「打包日志显示成功，包里却没有 gate」的情况。

### 第 3 层：运行时逐站点桩结构自检

`lzt_static_init_body()` 找到 gate 后，对每个站点做指令模式校验：

```text
[STATIC] gate integrity check: all 22 sites OK
```

或被踩时：

```text
[STATIC] gate site 3: adrp mismatch 0x00000000
[STATIC] gate site 3: ldr mismatch 0x00000000
[STATIC] gate integrity check: 5/22 sites corrupted
```

**解决的问题**：此前只验证「找到 LZTGATE1」就认为 22 个站点全部完好。如果容器只覆盖了 gate 的一部分，会表现为「部分功能静默失效」，极难定位。现在能直接指出是哪个站点、哪条指令被改。

## 三、指令掩码的正确写法

这是本次最容易踩错的地方。**立即数字段必须排除在比较之外**，否则不同站点的合法指令会被误判为损坏。

| 指令 | 掩码 | 期望值 | 排除的字段 |
|---|---|---|---|
| `adrp x16, ...` | `0x9F00001F` | `0x90000010` | immlo/immhi |
| `ldr x16,[x16,#off]` | `0xFFC003FF` | `0xF9400210` | imm12（bit10-21） |
| `cbnz x16, ...` | `0xFF00001F` | `0xB5000010` | imm19（bit5-23） |
| `b ...` | `0xFC000000` | `0x14000000` | imm26 |
| `br x16` | 全等 | `0xD61F0200` | 无 |

实际编码对照（站点 0）：

```text
b0014430  adrp x16
f9400210  ldr x16,[x16,#0]
b5000050  cbnz x16, +8
14000005  b +0x14
d61f0200  br x16
```

### 两个具体错误

**`adrp` 掩码写 `0xFF00001F`**：会把立即数高位也锁死，合法站点被误判。正确是 `0x9F00001F`。

**入口 B 指令判据写 `==0x17`**：只对「向低地址跳」成立。`B` 编码是 `0x14000000 | imm26`，最高字节随方向和高位在 `0x14`~`0x17` 变化。改成范围判断。

## 四、插入位置的坑

`grep -n 'g_lzt_stub_seg = 0;' | head -1` 取到的是第 55 行**全局变量定义**，而非函数内的赋值语句（511 行）。代码被插到全局作用域，报 `expected unqualified-id`。

改为带注释的完整行匹配：

```sh
grep -n 'g_lzt_stub_seg = 0;   /\* 不再有共享 stub 池 \*/'
```

**经验**：sed/grep 定位插入点时，模式要包含足够的上下文，不能只靠变量名。

## 五、打包期校验（正向 + 负向）

### 正向

```text
gate location: __TEXT slack @0x6bd0 (need=1072 B, LC slack=18600 B, first_sect=0x7000)
magic @0x6bd0（唯一）
stride=48 count=22 ✓
距 sizeofcmds 18600 字节（≥4096）✓
全部入口均为 B 指令 ✓
```

### 负向（落点校验）

把 `MIN_LC_SLACK` 临时改为 `99999999`，对干净切片执行：

```text
LC slack too small: 18688 B (need >= 99999999 B)
  gate=0x6bd0 sizeofcmds_end=0x22d0
  容器注入 load command 会覆盖 gate，必须换更大 slack 或改用 mid-trampoline
```

校验确实生效，而非摆设。

## 六、双环境实测

已在用户设备上完成两种环境的端到端验证，均正常。

| 指标 | LiveContainer | 设备直装（企业签名） |
|---|---|---|
| `main __TEXT base` | `0x10c894000` | `0x102840000` |
| `gate_hdr` | `0x10c89abd0` | `0x102846bd0` |
| 相对基址偏移 | **+0x6BD0** | **+0x6BD0** |
| 桩完整性 | all 22 sites OK | all 22 sites OK |
| `static sites used` | 19，跳过 7 | 19，跳过 7 |
| `blocked code-page writes` | 0 | 0 |
| `inline hook` 残留 | 0 条 | 0 条 |

**关键结论**：两者 `gate_hdr - base` 都精确等于 `0x6BD0`，证明 gate 定位与 ASLR、容器重定位完全无关。这是 v49~v52 一系列修复的最终验证。

第三层自检的实际输出：

```text
[STATIC] gate integrity check: all 22 sites OK
```

证明 22 个桩的五条指令模式全部匹配，无一被容器改动。此前只能确认「找到 magic」，现在能确认「整个 gate 区完好」。

## 七、产物

```text
PvZ2_v53_hardened.ipa
SHA256: 0ba6db3cda22f732e89dd3a0bd4602a515d67983a069de902ab6490f738f823b
```

归档：`iOS/backup/v53_hardened/`

功能与 v52 完全一致（v52 已验证功能全通），v53 额外提供三层自检能力。

## 八、与 v52 的关系

**功能代码零改动。** v52 已在 LiveContainer 和企业签名两种环境下验证通过。v53 只增加自检能力。

装机后相比 v52 应额外看到：

```text
[STATIC] gate integrity check: all 22 sites OK
```

若出现 `sites corrupted`，说明容器改动了 gate 数据，日志会给出具体站点编号，可直接定位。

## 九、仍未覆盖的情况

需要说清楚这三层自检的边界：

- **第 1、2 层**防的是「打包期位置不对」，对运行时容器的后续改动无能为力。
- **第 3 层**能发现 gate 被改，但只在启动时检查一次。容器若在启动后才改，仍检测不到。
- 三者都不改变「gate 必须在主程序内」这个事实。如果未来某容器彻底重写 `__TEXT`，唯一出路是放弃该环境下的静态打桩，退回纯数据驱动功能（语言资源包、No_Backup）。

这些是当前架构下的合理边界，不是遗漏。
