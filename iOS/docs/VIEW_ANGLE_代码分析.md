# VIEW_ANGLE 功能代码分析

> 生成时间：2026-07-04
> 目标二进制：PvZ2 iOS ARM64 FAT Mach-O
> ARM64 切片偏移：0x2244000
> 基址 VA：0x100000000

---

## 一、需求概述

1. 新建 TITLE "VIEW_ANGLE"，位置紧接在 DOWNLOADPREFS 之后
2. 页面内两个互斥开关 "VIEW_HIGH" 与 "VIEW_LOW"，能正确保存状态
3. 核心约束：通过在原 DATASHARING 页面创建与回调函数中增加条件分支来实现，避免完整复制函数

---

## 二、关键地址与数据结构

### 2.1 配置字段
| 字段 | 偏移 | 类型 | 说明 |
|------|------|------|------|
| HasDisabledUsageSharing | cfg+0xA83 | BYTE | 原始 DataSharing 状态（1=禁用） |
| UseHighViewAngle | cfg+0xA86 | BYTE | 新增 VIEW_ANGLE 状态（1=高视角） |

- 用户配置对象指针：`qword_1027EB870`（ADRP X0, page; LDR X0, [X0, #0x870]）

### 2.2 ID 分配
| 项目 | ID | 说明 |
|------|-----|------|
| VIEW_ANGLE Tab | 0x1E (30) | 避开 13/14/24-29 |
| VIEW_LOW Checkbox | 0x20 (32) | 走原始 ID=13 路径（W3=W26^1） |
| VIEW_HIGH Checkbox | 0x1F (31) | 走原始 ID=14 路径（W3=W26） |

### 2.3 全局标志位
| 标志位 | 地址 | 含义 |
|--------|------|------|
| g_isViewAnglePage | 0x1018bebf0 | 1=当前为VIEW_ANGLE页面，0=DATASHARING页面 |

### 2.4 DataSharing UTF-32LE 字符串地址
| 字符串 | 地址 |
|--------|------|
| [SETTINGS_DATASHARING_TITLE] | 0x101f04868 |
| [SETTINGS_DATASHARING_PROMPT] | 0x101f04e6c |
| [SETTINGS_DATASHARING_PROMPT_DISABLED] | 0x101f04dd0 |
| [SETTINGS_DATASHARING_ENABLE] | 0x101f04ee4 |
| [SETTINGS_DATASHARING_DISABLE] | 0x101f04f5c |

### 2.5 关键函数
| 函数 | 地址 | 说明 |
|------|------|------|
| sub_100756AE4 | 0x100756ae4 | 设置页面初始化（Tab 创建） |
| sub_100759DD4 | 0x100759dd4 | Tab 点击处理函数 |
| sub_100758A94 | 0x100758a94 | DataSharing 子页面创建函数 |
| sub_10075ADB4 | 0x10075adb4 | Checkbox 点击事件处理 |
| sub_100758930 | 0x100758930 | dirty flag 刷新回调（虚函数表） |
| sub_1001CDAEC | 0x1001cdaec | getter (硬编码 cfg+0xA83) |
| sub_1001CDA44 | 0x1001cda44 | setter (写入 cfg+0xA83) |
| sub_1001C8958 | 0x1001c8958 | 用户配置初始化函数 |
| sub_100757C18 | 0x100757c18 | Tab 创建函数（5参数） |
| sub_100757B78 | 0x100757b78 | wstring 构造函数 |
| sub_100046994 | 0x100046994 | 图标资源加载函数 |
| sub_1001BFB4C | 0x1001bfb4c | std::string 构造 |
| sub_101AB9F44 | 0x101ab9f44 | 持久化保存函数 |
| sub_100781FBC | 0x100781fbc | 配置初始化辅助函数 |

### 2.6 Tab ID 完整映射（sub_100756AE4）
| Tab ID | 标题字符串 | 地址 |
|--------|-----------|------|
| 0x3 | SETTINGS_NEWS_TITLE | 0x101f04584 |
| 0x6 | SETTINGS_DOWNLOADPREFS_TITLE | 0x101f0467c |
| 0x7 | SETTINGS_HELP_TITLE | 0x101f046f8 |
| 0x8 | SETTINGS_EULA_TITLE | 0x101f04750 |
| 0x9 | SETTINGS_PRIVACY_TITLE | 0x101f047a8 |
| 0xA | SETTINGS_TERMS_TITLE | 0x101f0480c |
| 0xC | SETTINGS_DATASHARING_TITLE | 0x101f04868 |
| 0x19 | SETTINGS_RESTORE_PURCHASES | 0x101f048dc |
| 0x1A | SETTINGS_RESET_AGE | 0x101f04950 |
| 0x18 | SETTINGS_GAMECREDITS_TITLE | 0x101f049a4 |
| 0xF | SETTINGS_PLAYERTARGETING_TITL | 0x101f04a18 |
| 0x12 | SETTINGS_PLAYERTARGETING_EA_T | 0x101f04a9c |
| 0x15 | SETTINGS_ACCOUNT_DELETION_TIT | 0x101f04b2c |

### 2.7 图标资源
| 资源 | 地址 | 用途 |
|------|------|------|
| qword_102813100 | 0x102813100 | DATASHARING 图标1（VIEW_ANGLE 复用） |
| qword_102813128 | 0x102813128 | DATASHARING 图标2（VIEW_ANGLE 复用） |

---

## 三、关键代码逻辑分析

### 3.1 sub_100759DD4 switch 结构

**第一层 switch（W1=event_id）**：
- case 24-29 范围
- case 25,26 → loc_100759E6C (Tab_Release)
- default → 检查 W20==1 (Back) 或 X_Close

**第二层 switch（W20=Tab_ID）** at loc_100759E6C：
```
0x100759e80: SUB W8, W20, #2       ; W8 = Tab_ID - 2
0x100759e84: CMP W8, #0x18 (24)    ; 范围 2-26
0x100759e88: B.HI def_100759EA0    ; case 30 会跳到 default
```

case 12 入口（0x10075a050）：
```
0x10075a050: MOV X0, X19           ; 页面对象
0x10075a054: BL sub_100758A94      ; 创建 DataSharing 子页面
0x10075a058: B loc_10075A0B0       ; 返回
```

### 3.2 sub_100758A94 关键指令地址
| 地址 | 指令 | 说明 |
|------|------|------|
| 0x100758ac8 | ADRL X1, [DATASHARING_TITLE] | 标题字符串 |
| 0x100758ad4 | BL sub_100757B78 | wstring 构造 |
| 0x100758c00 | BL sub_1001CDAEC | 第一次 getter (PROMPT 选择) |
| 0x100758c04 | ADRL X8, [PROMPT] | PROMPT 字符串 |
| 0x100758c0c | ADRL X9, [PROMPT_DISABLED] | PROMPT_DISABLED 字符串 |
| 0x100758c18 | CSEL X0, X9, X8, NE | 根据状态选择 |
| 0x100758d54 | BL sub_1001CDAEC | 第二次 getter (W26 计算) |
| 0x100758d58 | MOV X26, X0 | W26 = 状态值 |
| 0x100758d5c | ADRL X1, [ENABLE] | ENABLE 字符串 |
| 0x100758d6c | EOR W3, W26, #1 | ENABLE 选中 = !W26 |
| 0x100758d78 | MOV W1, #13 | ENABLE ID = 13 |
| 0x100758db8 | ADRL X1, [DISABLE] | DISABLE 字符串 |
| 0x100758dd0 | MOV W1, #14 | DISABLE ID = 14 |
| 0x100758dd4 | MOV X3, X26 | DISABLE 选中 = W26 |

### 3.3 sub_10075ADB4 switch 结构
```
0x10075ade8: SUB W8, W21, #0xD    ; W8 = component_id - 13
0x10075adec: CMP W8, #0x10        ; 范围 13-29
0x10075adf0: B.HI def_10075AE08   ; case 31/32 跳到 default
```

case 13/14 处理：
```
case 13 (ENABLE点击):
  CBZ W20, loc_10075AE3C          ; 未选中跳过
  ADRP X8, qword_1027EB870
  LDR X0, [X8, #0x870]            ; cfg
  MOV W1, #0                      ; 写入 0
  B loc_10075AE38

case 14 (DISABLE点击):
  CBZ W20, loc_10075AE3C          ; 未选中跳过
  ADRP X8, qword_1027EB870
  LDR X0, [X8, #0x870]            ; cfg
  MOV W1, #1                      ; 写入 1
  BL sub_1001CDA44                ; setter

共同尾部:
  0x10075ae3c: MOV W8, #1
  0x10075ae40: STRB W8, [X19, #0x124]  ; dirty flag (★正确偏移)
  0x10075ae44: B def_10075AE08          ; 返回
```

### 3.4 sub_100758930 dirty flag 刷新路径
```
0x1007589f4: LDRB W8, [X19,#0x124]    ; 检查 dirty flag
0x1007589f8: CBZ W8, loc_100758A0C
0x1007589fc: MOV X0, X19
0x100758a00: BL sub_100758A94         ; 重新创建子页面
0x100758a04: STRB WZR, [X19,#0x124]   ; 清除 dirty flag
```

### 3.5 sub_1001C8958 HasDisabledUsageSharing 加载点
```
0x1001c8b24: ADRL X1, "HasDisabledUsageSharing"
0x1001c8b40: BL sub_101ABA2DC         ; 读取持久化值
0x1001c8b54: LDRB W8, [SP,#0x90+__p]
0x1001c8b58: CBZ W8, loc_1001C8B64
0x1001c8b5c: MOV W0, #1
0x1001c8b60: BL sub_100781FBC         ; ★ Hook5 修改点
```

---

## 四、代码洞穴资源

### 4.1 Cave 1
- 起始：0x1018bead4
- 结束：0x1018bf000
- 大小：1324 字节
- 段：__text (CODE, perm=0x5)
- 前一个函数：0x1018bea9c - 0x1018bead4
- 后续：0x1018bf000 为数据（不可扩展）

### 4.2 Cave 2
- 起始：0x101924c48
- 结束：0x101924d40
- 大小：248 字节
- 段：__text (CODE, perm=0x5)
- 前一个函数：0x101924c3c - 0x101924c48
- 后续：0x101924d40 为数据（不可扩展）

### 4.3 空间预算
| 区域 | 需求 | 可用 | 余量 |
|------|------|------|------|
| Cave 1 | 1175B | 1324B | 149B |
| Cave 2 | 132B | 248B | 116B |
| 总计 | 1307B | 1572B | 265B |
| 预留 | - | - | 50B |
| 实际余量 | - | - | 215B ✓ |
