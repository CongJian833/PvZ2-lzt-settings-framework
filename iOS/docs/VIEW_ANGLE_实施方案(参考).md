# VIEW_ANGLE 功能实施方案

> 生成时间：2026-07-04
> 目标二进制：PvZ2 iOS ARM64 FAT Mach-O
> ARM64 切片偏移：0x2244000
> 基址 VA：0x100000000
> 详细代码分析见：[VIEW_ANGLE_代码分析.md](./VIEW_ANGLE_代码分析.md)

---

## 一、需求概述

1. 新建 TITLE "VIEW_ANGLE"，位置紧接在 DOWNLOADPREFS 之后
2. 页面内两个互斥开关 "VIEW_HIGH" 与 "VIEW_LOW"，能正确保存状态
3. 核心约束：通过在原 DATASHARING 页面创建与回调函数中增加条件分支来实现，避免完整复制函数

---

## 二、关键分析摘要（实施基础）

### 2.1 核心配置与 ID
- **配置字段**：`UseHighViewAngle` @ cfg+0xA86（VIEW_ANGLE 状态，1=高视角）
- **原 DataSharing 字段**：`HasDisabledUsageSharing` @ cfg+0xA83
- **用户配置对象指针**：`qword_1027EB870`

**ID 分配（关键设计 - 避免修改 W3 计算逻辑）**：
| 项目 | ID | 路径 |
|------|-----|------|
| VIEW_ANGLE Tab | 0x1E (30) | 避开 13/14/24-29 |
| VIEW_LOW Checkbox | 0x20 (32) | 走原始 ID=13 路径（W3=W26^1，LOW选中=!高视角） |
| VIEW_HIGH Checkbox | 0x1F (31) | 走原始 ID=14 路径（W3=W26，HIGH选中=高视角） |

### 2.2 全局标志位
| 标志位 | 地址 | 用途 |
|--------|------|------|
| g_isViewAnglePage | 0x1018bebf0 | 1=VIEW_ANGLE页面，0=DATASHARING页面 |

### 2.3 关键修改目标函数
| 函数 | 地址 | 修改目标 |
|------|------|----------|
| sub_100756AE4 | 0x100756ae4 | 新增 VIEW_ANGLE Tab 创建 |
| sub_100759DD4 | 0x100759dd4 | 拦截 case 30（VIEW_ANGLE Tab 点击） |
| sub_100758A94 | 0x100758a94 | 条件分支：标题/PROMPT/字符串/ID 切换 |
| sub_10075ADB4 | 0x10075adb4 | 拦截 case 31/32（Checkbox 保存） |
| sub_1001C8958 | 0x1001c8958 | 加载默认值到 cfg+0xA86 |

### 2.4 关键代码位置（详见分析文档第3章）
- Tab ID switch: 0x100759e88 (B.HI def_100759EA0)
- case 12 入口: 0x10075a050 (MOV X0, X19)
- DataSharing 子页面创建关键点: 0x100758ac8/c00/c04/d54/d5c/d78/db8/dd0
- Checkbox switch: 0x10075adf0 (B.HI def_10075AE08)
- dirty flag 设置: 0x10075ae40 (STRB W8, [X19, #0x124])
- 默认值加载点: 0x1001c8b60 (BL sub_100781FBC)

### 2.5 代码洞穴资源
| 洞穴 | 起始 | 结束 | 大小 | 用途 |
|------|------|------|------|------|
| Cave 1 | 0x1018bead4 | 0x1018bf000 | 1324B | 字符串数据 + 全部 Hook |
| Cave 2 | 0x101924c48 | 0x101924d40 | 248B | save_view_angle + get_view_angle |
| 总余量 | - | - | 265B（预留50B后实际余量215B） | ✓ |

---

## 三、字符串布局（Cave 1 起始）

| 地址 | 字符串 | 编码 | 大小 |
|------|--------|------|------|
| 0x1018bead4 | [VIEW_ANGLE_TITLE] | UTF-32LE | 19×4+4 = 80B |
| 0x1018beb24 | [VIEW_ANGLE_PROMPT] | UTF-32LE | 20×4+4 = 84B |
| 0x1018beb78 | [VIEW_LOW] | UTF-32LE | 10×4+4 = 44B |
| 0x1018beba4 | [VIEW_HIGH] | UTF-32LE | 11×4+4 = 48B |
| 0x1018bebd4 | UseHighViewAngle | ASCII | 17B |
| 0x1018bebe5 | (对齐填充) | 0x00 | 11B |
| 0x1018bebf0 | g_isViewAnglePage | BYTE | 1B |
| 小计 | | | 285B |

---

## 四、修改点与 Hook 布局

### 4.1 修改点汇总（13 处）

| # | 修改点 | 原始指令 | 修改为 | 说明 |
|---|--------|----------|--------|------|
| 1 | 0x1007570d0 | LDRSB W8, [SP,#0x130+var_99] | B Hook1 | VIEW_ANGLE Tab 创建 |
| 2 | 0x100759e88 | B.HI def_100759EA0 | B Hook2 | case 30 拦截 |
| 3 | 0x10075a050 | MOV X0, X19 | B Hook6 | case 12 入口清标志 |
| 4 | 0x100758ac8 | ADRL X1, [DATASHARING_TITLE] | B Hook3.1 | 标题切换 |
| 5 | 0x100758c00 | BL sub_1001CDAEC | B Hook3.2a | PROMPT getter 切换 |
| 6 | 0x100758c04 | ADRL X8, [PROMPT] | B Hook3.7 | PROMPT 字符串切换 |
| 7 | 0x100758d54 | BL sub_1001CDAEC | B Hook3.2b | W26 getter 切换 |
| 8 | 0x100758d5c | ADRL X1, [ENABLE] | B Hook3.3 | ENABLE→VIEW_LOW |
| 9 | 0x100758d78 | MOV W1, #13 | B Hook3.4 | ID 13→32 |
| 10 | 0x100758db8 | ADRL X1, [DISABLE] | B Hook3.5 | DISABLE→VIEW_HIGH |
| 11 | 0x100758dd0 | MOV W1, #14 | B Hook3.6 | ID 14→31 |
| 12 | 0x10075adf0 | B.HI def_10075AE08 | B Hook4 | case 31/32 保存拦截 |
| 13 | 0x1001c8b60 | BL sub_100781FBC | B Hook5 | 默认值加载 |

### 4.2 Cave 1 布局
| 地址 | 大小 | 内容 |
|------|------|------|
| 0x1018bead4 | 285B | 字符串数据 + 标志位 |
| 0x1018bebf1 | 100B | Hook1: Tab 创建 |
| 0x1018bec55 | 80B | Hook2: case 30 拦截 |
| 0x1018beca5 | 24B | Hook6: case 12 清标志 |
| 0x1018becbd | 56B | Hook3.1: 标题切换 |
| 0x1018ecf5 | 56B | Hook3.2a: PROMPT getter |
| 0x1018ed2d | 56B | Hook3.7: PROMPT 字符串 |
| 0x1018ed65 | 56B | Hook3.2b: W26 getter |
| 0x1018ed9d | 56B | Hook3.3: ENABLE→VIEW_LOW |
| 0x1018edd5 | 40B | Hook3.4: ID 13→32 |
| 0x1018edfd | 56B | Hook3.5: DISABLE→VIEW_HIGH |
| 0x1018ee35 | 40B | Hook3.6: ID 14→31 |
| 0x1018ee5d | 160B | Hook4: case 31/32 保存 |
| 0x1018eefd | 60B | Hook5: 默认值加载 |

### 4.3 Cave 2 布局
| 地址 | 大小 | 内容 |
|------|------|------|
| 0x101924c48 | 100B | save_view_angle: 持久化保存函数 |
| 0x101924cac | 32B | get_view_angle: 读取 cfg+0xA86 |

---

## 五、各 Hook 详细设计

### Hook1: VIEW_ANGLE Tab 创建（Cave 1: 0x1018bebf1）
修改点：0x1007570d0

```asm
; 执行原始指令
LDRSB W8, [SP, #0x130+var_99]
TBZ W8, #0x1F, loc_create_va
LDR X0, [SP, #0x130+__p]
BL __ZdlPv

loc_create_va:
ADRL X1, 0x1018bead4          ; [VIEW_ANGLE_TITLE]
ADD X0, SP, #0x130+__p
BL sub_100757B78              ; 构造 wstring

ADRL X0, qword_102813100      ; icon1
BL sub_100046994
MOV X27, X0

ADRL X0, qword_102813128      ; icon2
BL sub_100046994
MOV X4, X0

ADD X2, SP, #0x130+__p
MOV X0, X19
MOV W1, #0x1E                 ; Tab ID = 30
MOV X3, X27
BL sub_100757C18

B 0x1007570e0
```

### Hook2: case 30 拦截（Cave 1: 0x1018bec55）
修改点：0x100759e88

```asm
CMP W20, #0x1E
B.NE loc_original_bhi

ADRP X8, 0x1018be000
MOV W9, #1
STRB W9, [X8, #0x1bf0]        ; g_isViewAnglePage = 1

MOV X0, X19
BL sub_100758A94
B 0x10075a058

loc_original_bhi:
B def_100759EA0
```

### Hook6: case 12 清标志（Cave 1: 0x1018beca5）
修改点：0x10075a050

```asm
MOV X0, X19                   ; 原始指令
ADRP X8, 0x1018be000
STRB WZR, [X8, #0x1bf0]       ; g_isViewAnglePage = 0
B 0x10075a054
```

### Hook3.1: 标题切换（Cave 1: 0x1018becbd）
修改点：0x100758ac8

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_title_orig
ADRL X1, 0x1018bead4          ; [VIEW_ANGLE_TITLE]
B 0x100758ad4
loc_title_orig:
ADRL X1, 0x101f04868          ; [DATASHARING_TITLE]
B 0x100758ad4
```

### Hook3.2a: PROMPT getter（Cave 1: 0x1018ecf5）
修改点：0x100758c00

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_getter_orig1
LDRB W0, [X0, #0xA86]         ; VIEW_ANGLE: cfg+0xA86
B 0x100758c04
loc_getter_orig1:
BL sub_1001CDAEC              ; 原始 getter (cfg+0xA83)
B 0x100758c04
```

### Hook3.7: PROMPT 字符串切换（Cave 1: 0x1018ed2d）
修改点：0x100758c04

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_prompt_orig
ADRL X8, 0x1018beb24          ; [VIEW_ANGLE_PROMPT]
B 0x100758c08
loc_prompt_orig:
ADRL X8, 0x101f04e6c          ; [DATASHARING_PROMPT]
B 0x100758c08
```

### Hook3.2b: W26 getter（Cave 1: 0x1018ed65）
修改点：0x100758d54

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_getter_orig2
LDRB W0, [X0, #0xA86]         ; VIEW_ANGLE: cfg+0xA86
B 0x100758d58
loc_getter_orig2:
BL sub_1001CDAEC              ; 原始 getter (cfg+0xA83)
B 0x100758d58
```

### Hook3.3: ENABLE→VIEW_LOW（Cave 1: 0x1018ed9d）
修改点：0x100758d5c

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_enable_orig
ADRL X1, 0x1018beb78          ; [VIEW_LOW]
B 0x100758d68
loc_enable_orig:
ADRL X1, 0x101f04ee4          ; [DATASHARING_ENABLE]
B 0x100758d68
```

### Hook3.4: ID 13→32（Cave 1: 0x1018edd5）
修改点：0x100758d78

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_id13_orig
MOV W1, #0x20                 ; 32 (VIEW_LOW)
B 0x100758d7c
loc_id13_orig:
MOV W1, #0xD                  ; 13
B 0x100758d7c
```

### Hook3.5: DISABLE→VIEW_HIGH（Cave 1: 0x1018edfd）
修改点：0x100758db8

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_disable_orig
ADRL X1, 0x1018beba4          ; [VIEW_HIGH]
B 0x100758dc4
loc_disable_orig:
ADRL X1, 0x101f04f5c          ; [DATASHARING_DISABLE]
B 0x100758dc4
```

### Hook3.6: ID 14→31（Cave 1: 0x1018ee35）
修改点：0x100758dd0

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_id14_orig
MOV W1, #0x1F                 ; 31 (VIEW_HIGH)
B 0x100758dd4
loc_id14_orig:
MOV W1, #0xE                  ; 14
B 0x100758dd4
```

### Hook4: case 31/32 保存拦截（Cave 1: 0x1018ee5d）
修改点：0x10075adf0

```asm
ADRP X8, 0x1018be000
LDRB W9, [X8, #0x1bf0]
CBZ W9, loc_handler_orig      ; DATASHARING 走原始路径

; VIEW_ANGLE 路径
CMP W21, #0x1F                ; case 31 (VIEW_HIGH)
B.NE loc_check_case32
CBZ W20, loc_set_dirty
MOV W1, #1
BL save_view_angle            ; 保存到 0x101924c48
B loc_set_dirty

loc_check_case32:
CMP W21, #0x20                ; case 32 (VIEW_LOW)
B.NE loc_handler_orig
CBZ W20, loc_set_dirty
MOV W1, #0
BL save_view_angle

loc_set_dirty:
MOV W8, #1
STRB W8, [X19, #0x124]        ; dirty flag
B 0x10075ae44                 ; 返回

loc_handler_orig:
B def_10075AE08               ; 原始 default
```

### Hook5: 默认值加载（Cave 1: 0x1018eefd）
修改点：0x1001c8b60

```asm
BL sub_100781FBC              ; 原始指令

ADRP X0, qword_1027EB870@PAGE
LDR X0, [X0, #0x870]
LDRB W8, [X0, #0xA86]
CBNZ W8, loc_hook5_return
MOV W8, #1
STRB W8, [X0, #0xA86]         ; 默认值 1 (高视角)

loc_hook5_return:
B 0x1001c8b64
```

### save_view_angle 函数（Cave 2: 0x101924c48）

```asm
save_view_angle:
STP X29, X30, [SP, #-0x20]!
STP X19, X20, [SP, #0x10]
MOV X29, SP

LDRB W8, [X0, #0xA86]         ; 当前值
CMP W8, W1
B.EQ loc_save_skip

STRB W1, [X0, #0xA86]         ; 写入新值

ADRP X8, qword_1027F4ED0@PAGE
LDR X20, [X8, #0xED0]
ADRL X1, 0x1018bebd4          ; "UseHighViewAngle"
ADD X0, SP, #8
BL sub_1001BFB4C
LDRB W2, [X19, #0xA86]
ADD X1, SP, #8
MOV X0, X20
BL sub_101AB9F44              ; 持久化保存

loc_save_skip:
LDP X19, X20, [SP, #0x10]
LDP X29, X30, [SP], #0x20
RET
```

### get_view_angle 函数（Cave 2: 0x101924cac）

```asm
get_view_angle:
ADRP X0, qword_1027EB870@PAGE
LDR X0, [X0, #0x870]
LDRB W0, [X0, #0xA86]
RET
```

---

## 六、Checkbox 选中状态逻辑验证

### 创建阶段（sub_100758A94）
| 开关 | ID | W3 计算 | 结果 |
|------|-----|---------|------|
| VIEW_LOW | 32 (走ID=13路径) | W3 = W26^1 = !UseHighViewAngle | LOW选中 = !高视角 ✓ |
| VIEW_HIGH | 31 (走ID=14路径) | W3 = W26 = UseHighViewAngle | HIGH选中 = 高视角 ✓ |

### 点击阶段（Hook4）
| 点击的开关 | W21 | W20(选中) | 保存的值 | 结果 |
|-----------|-----|----------|---------|------|
| VIEW_HIGH | 31 | 1 | UseHighViewAngle=1 | 高视角 ✓ |
| VIEW_LOW | 32 | 1 | UseHighViewAngle=0 | 低视角 ✓ |

---

## 七、完整执行流程验证

### 场景1：进入 VIEW_ANGLE → 点击 VIEW_LOW → 刷新
```
1. case 30 → Hook2: g_isViewAnglePage=1 → sub_100758A94
   - Hook3.1: 标题=[VIEW_ANGLE_TITLE] ✓
   - Hook3.2a: W0=cfg+0xA86 ✓
   - Hook3.7: PROMPT=[VIEW_ANGLE_PROMPT] ✓
   - Hook3.2b: W26=cfg+0xA86 ✓
   - Hook3.3-3.6: 创建 VIEW_LOW(ID=32) + VIEW_HIGH(ID=31) ✓

2. 点击 VIEW_LOW (ID=32, W20=1)
   → sub_10075ADB4 → W21=32
   → Hook4: case 32 → save_view_angle(0) → cfg+0xA86=0
   → set page+0x124=1

3. dirty flag 刷新 → sub_100758930 → sub_100758A94
   - g_isViewAnglePage 仍为 1 ✓
   - Hook3.2b: W26=cfg+0xA86=0
   - VIEW_HIGH: W3=W26=0 (未选中) ✓
   - VIEW_LOW: W3=W26^1=1 (选中) ✓
```

### 场景2：VIEW_ANGLE → DATASHARING → 刷新
```
1. case 30 → Hook2: g_isViewAnglePage=1
2. 用户返回
3. case 12 → Hook6: g_isViewAnglePage=0 → sub_100758A94
   - Hook3.1: 标题=[DATASHARING_TITLE] ✓
   - Hook3.2a: W0=cfg+0xA83 ✓
   - Hook3.7: PROMPT=[DATASHARING_PROMPT] ✓
   - Hook3.2b: W26=cfg+0xA83 ✓
   - Hook3.3-3.6: 创建 ENABLE(ID=13) + DISABLE(ID=14) ✓
```

---

## 八、实施顺序建议

1. 阶段1：写入字符串数据（Cave 1 起始，无风险）
2. 阶段2：实现 save_view_angle + get_view_angle（Cave 2，独立验证）
3. 阶段3：Hook5 默认值加载（独立，验证配置加载）
4. 阶段4：Hook1 Tab 创建（验证 Tab 显示）
5. 阶段5：Hook2 + Hook6 case 拦截（验证页面跳转）
6. 阶段6：Hook3.1-3.7 条件分支（验证页面内容）
7. 阶段7：Hook4 保存拦截（验证状态保存）

---

## 九、风险点与验证要点

### 9.1 高风险点
1. ADRL 编码准确性：每个 ADRL 必须手动计算 ADRP+ADD 偏移
2. B 指令偏移计算：26 位有符号偏移，需逐个验证
3. X8 寄存器安全：所有 Hook3.x 使用 X8 作为临时寄存器，已验证可用
4. save_view_angle 中的 X19 使用：必须保存恢复

### 9.2 验证要点
1. 代码洞穴字节验证：所有代码洞穴必须为全零（已验证通过）
2. 修改点原始字节验证：每个修改点必须与预期值匹配（已验证 15/15 通过）
3. 写入后回读验证：每次写入后立即回读确认
4. 反向解码验证：每条指令手动拆分位域交叉验证
