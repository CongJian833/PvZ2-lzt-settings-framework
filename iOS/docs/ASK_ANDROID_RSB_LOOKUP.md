# ASK_ANDROID：iOS「按名字取 RSB 裸字节」原语定位（对应安卓 sub_16B31EC）

> 用途：向安卓侧请教 iOS 已定位的等价链，请求确认字段语义与正确调用形态。
> 版本：**v2**（2026-10-17，新增 §0 核心假设 + §1.5 运行时实证，修正 v1 两处错误）
>
> **给安卓侧的前提说明**：本项目长期约定「PvZ2 安卓与 iOS 本质是同一个游戏」，
> 且已多次印证「iOS 结构体字段偏移 = 安卓偏移 + 112」的规律
> （例：DisplayInfo 语言管理器 2104 vs 1992；Board 2584 vs 2472；
> LangMgr locale 508 vs 484）。因此本次提问**默认按同一套游戏源码推导**，
> 请优先用安卓实现的字段语义直接映射，不必从零设计 iOS 方案。

---

## 〇、本次提问的核心假设（请优先确认/否认）

`0x7eb30c`（iOS 挂载组查找）内部读 `*(base+0x27f4ed0)+2248`。
而安卓 `PAKMGR_RES_STREAMS_MGR = 0x858 = 2136`。

**2248 − 2136 = 112** —— 与本项目反复印证的「iOS 偏移比安卓多 112」规律吻合。

因此假设：

```
iOS: pakMgr = *(base + 0x27f4ed0)                    ← 与安卓 OFF_PAK_MANAGER 同址
     rsm    = *(pakMgr + 2248)                       ← 安卓是 *(pakMgr + 0x858)
```

若成立，则安卓 `sub_16B31EC` 的整套 rsm 字段语义可平移：

| 安卓偏移 | 语义 | iOS 待验证偏移（假设 +112） |
| - | - | - |
| rsm+24 | RSB 通道开关 | rsm+136 |
| rsm+48 | 全局哈希树根 | rsm+160 |
| rsm+112 | 挂载包数量 | rsm+224 |
| rsm+120 | 挂载包数组指针 | rsm+232 |
| rsm+32 | RSB 主对象 | rsm+144 |

**问题 A：这个「+112」假设对吗？** 若不对，iOS 的 rsm 里这些字段实际在哪？

---

## 一、iOS 侧已确证的事实（静态 + 运行时）

### 1.1 全局对象槽

| 槽 | 地址（相对 __TEXT） | 用途 | 证据 |
| --- | --- | --- | - |
| `base+0x27f4ed0` | `OFF_PersistManager` | 持久化管理器 / Pak 管理器 / **本地化容器 owner** | `0x760a20`(ApplyLocalizedStrings) 与 `0x179c14`(取值口) 读同一处；`0x7eb280`/`0x7eb30c` 也读它 |
| `base+0x27eb870` | `OFF_G_Config` | 用户配置 / DisplayInfo | `BoardZoom2` 引用；+2104 = 语言管理器 |

### 1.2 「按名字查找」的函数链（反汇编实证）

```
0x65428(x1=名字指针, x2=输出)            ← 外层壳：名字规范化 + 临时对象构造
  └─ 0x7eb1c0(x0=管理器, x1=裸名, x2=输出)   ← 【查找入口】返回 w0=是否命中
       ├─ 0x7eb234   名字规范化
       ├─ 0x7eb280   查「主包」：*(base+0x27f4ed0) → 0x101abaeac
       └─ 0x7eb30c   查「挂载组」：*(base+0x27f4ed0)+2248 → 0x101c418c4 逐组
```

**v1 的错误结论已纠正**：`0x65428` 返回的 1336B「记录对象」只是外层壳的产物，
真正的查找语义在 `0x7eb1c0`（对应安卓 `sub_16B31EC`）。

### 1.3 `0x7eb1c0` 的输入输出（运行时 dump 实证）

签名：`0x7eb1c0(x0=管理器, x1=裸名, x2=输出对象)` → `w0` = 是否命中。
- `x0` 形如 `0x300c4e6a0`（ObjC 对象）
- `x1` 必须是**裸名**：无 `packages\` 前缀、无 `.rton` 后缀

**输出对象结构**：
```
out+0x00 = libc++ std::string 数据指针   ← 内容是 "packages/xxx.rton"
out+0x08 = 该字符串长度
out+0x10 = libc++ cap 标志（0x8000000000000030）
```

实测全部 `hit=1`：

| 传入裸名 | out+0x08 | deref 内容 |
| - | - | - |
| `liveconfig` | 24 | `packages/liveconfig.rton` |
| **`LANGUAGETYPES`** | 27 | **`packages/languagetypes.rton`** |
| **`LawnStrings-zh-cn`** | 31 | **`packages/lawnstrings-zh-cn.rton`** |
| **`LawnStrings-en-us`** | 31 | **`packages/lawnstrings-en-us.rton`** |

**即：iOS 侧 `0x7eb1c0` 做「裸名 → 完整路径」，不返回裸字节。**
这与安卓 `sub_16B31EC` 直接输出 `(dataPtr, dataSize)` 不同——**这是本次提问的核心**。

### 1.4 已被实测排除的候选（勿重复）

| 地址/方案 | 现象 | 结论 |
| - | - | - |
| `0x64f20` 主动调用 | 崩（pc=+0x64f44/+0x64f9c） | x0 是**输出对象**；内部按 DisplayInfo+2104 **当前语言**批量取组，非通用接口 |
| 传 `packages\LANGUAGETYPES.rton` | hit=0 | 名字形态错，必须是裸名 `LANGUAGETYPES` |
| 传 `RESFILE_PACKAGES_LANGUAGETYPES` | hit=0 | 同上 |
| `0x6525c` | — | 名字规范化/拆分（`packages\levels\X.rton` → `X`） |
| `0x7ebaa8` | — | 也是名字规范化（`/`↔`\`、剥层级、去扩展名） |
| `base+0x27f4ed0` 直接当 pakMgr | pakMgr 探针读到 mgr=0 | 该槽需解一次才得 pakMgr；时机敏感 |

### 1.5 日志实证（真机 `lztsettingsframework.log`）

```
reslookup #1: mgr=0x300c4e6a0 name="liveconfig" out=0x16f1b52f8 ret=0x1
    out+00: 0000000302e76f40
    out+08: 0000000000000018        ← 24
RSBDEREF LANGUAGETYPES ptr=0x303d02190 len=27
    ascii: packages/languagetypes.rton
RSBDEREF LawnStrings-zh-cn ptr=0x303d00840 len=31
    ascii: packages/lawnstrings-zh-cn.rton
```

---

## 二、main.rsb 容器格式（自解析路线 B 的结论）

`main.rsb`（1.53GB，magic `1bsr`）**不是** RTON 文件堆，而是自有容器：

```
0x00000000  头部（1bsr + 版本 4 + 10 组 (offset, val) 段表）
0x00000100  UTF-32LE 字符表（每字符 4B）："M_SECURITYGOURDS_1536_00.PTX"...
0x00253000  子容器 "pgsr"（magic + 版本 4 + 子段），图集相关
0x0006d994  组名表（UTF-32LE 字符串池）
0x001af610  另一组名表
0x5B002200  RSB manifest（**标准 RTON map**）
0x5B0051E5  资源索引表（RTON map 序列，3699 条 RESFILE_）
```

### 2.1 manifest（RTON 格式，可被 `language_types_rton.cpp` 直接解析）

```
RTON 01 00 00 00
  90 07 "version"          24 01
  90 0f "content_version"  24 01
  90 0a "slot_count"       24 a8 c4 02      ← varint = 41512（v1 误记为 163936）
  90 06 "groups"           86 fd 92 12 85   ← 18 个条目
      每项含 type / id / subgroups / parent / resources
```

### 2.2 索引表条目字段语义（两条目对比定出）

```
"RESFILE_PACKAGES_LANGUAGETYPES"
  91 17 86 fd 02 91 b2 38
  90 12 "languagetypes.rton"
  fe ff 85
  91 04 91 ec 0b 91 11 24 bd 1b
  91 06 90 22
```

| 字段 | LANGUAGETYPES | LawnStrings-de-de | 判定 |
| - | - | - | - |
| v1 | 1516 | 1516 | 组 id（两者同组，故相同） |
| v1b | 17 | 17 | 固定常量（格式版本/类型码） |
| v2 | 3517 | 3518 | 资源序号（相邻资源递增） |

佐证：`joust_tournamentrewards.rton` 的 v2 = 41512，恰等于 `slot_count`，是最后一号。

**v2 与 `slot_count` 已对应上，但「slot 号 → 数据区偏移」的映射表仍未定位。**

### 2.3 slot 表搜索已排除的方法（勿重复）

| 方法 | 结果 |
| - | - |
| 段表反推（val / 41512 = 整数步长） | 无匹配（步长分别为 10.81/1.90/0.04/12.71/1.71/58.40） |
| manifest 前推（manifest − 41512×步长） | 各起点均为索引表数据，非 slot 表 |
| manifest 前 4KB 连续 u32 扫描 | 仅 2 处噪声 |
| seg[4](0x1c0a9c) stride=20 | slot 3517 处读到 0x90000，该偏移处全零 |
| manifest~索引表区间 | 477867B / 41512 ≈ 11.5 B/slot，实测全是指引表条目 |

### 2.4 外置文件对照（已知格式）

`Documents/No_Backup/LANGUAGETYPES.rton`（363B）：
```
52 54 4f 4e 01 00 00 00  81 08 "#comment"  81 18 "Make new languages here!" ...
```
即**标准 RTON 文件头**。「数据包来源」要产出的就是这种裸 RTON 字节流。

**注意：`main.rsb` 内检索 `"LanguageType"` / `"language_simplified_chinese"` /
`"Make new languages here"` 全部 0 命中** —— 数据体内是压缩存储（字符表索引）。

---

## 三、请求安卓侧回答的问题（按优先级）

### Q1（最关键）：安卓 `sub_16B31EC` 的完整反汇编或等价 C 伪代码

重点是它从 `rsm` 出发**依次读了哪些偏移**、遍历哪种容器。
iOS 侧已定位 `0x7eb30c` 读 `*(base+0x27f4ed0)+2248`，若这就是 rsm（假设见 §0），
则请给出安卓从 `rsm` 到 `(dataPtr, dataSize)` 的每一步。

### Q2：安卓的 `组 buffer 基址` 是什么？node.off / node.size 从哪来？

安卓注释写：
```
dataPtr = 组 buffer 基址 + 组内文件偏移 + node.off
size    = node.size
```
请说明：
1. 「组 buffer 基址」是挂载组里的哪个字段？（组+0？组+8？）
2. 「组内文件偏移」是什么？是固定值还是从组里读？
3. `node.off` / `node.size` 是 radix 树节点的哪两个字段？

iOS 侧若走 `0x7eb30c → 0x101c418c4` 逐组查找，需要知道遍历到组之后**怎么取字节**。

### Q3：iOS 侧 `0x7eb1c0` 只返回路径，是否还有第二个函数返回数据？

已知 `0x7eb1c0` 给完整路径 `packages/xxx.rton`。安卓侧拿到路径后是否还要再调一次
才能取字节？如果是，那个函数在 iOS 的对应物可能是什么？

参考：iOS `0x8106e8` 序列是
```
0x7f6634(取管理器) → 0x7eb1c0(查路径) → 0x2c9e04(ctx, tableId, 路径)
```
即 iOS 把路径直接交给 `LoadPackageForTableFromRTONFile` 加载。
**安卓侧是不是也一样：sub_16B31EC 之后还要走 LoadPackage？**
若是，则 iOS 的 LANGUAGETYPES 也应该有对应 tableId 走这条路。

### Q4：LANGUAGETYPES 在安卓侧有 tableId 吗？

本项目 `kLawnStringsTableId = 131`（LawnStrings 专用）。
LANGUAGETYPES 在安卓侧是模块自己读文件解析的（不经游戏表机制）——**确认这一点**。
若安卓也如此，则 iOS 上「让游戏自己加载」这条路对 LANGUAGETYPES 不适用，
只能取裸字节自解析（与现状一致）。

### Q5：安卓是怎么读 main.rsb / 对应包文件的？

是内存映射整个包、还是按需 seek？如果安卓侧有「打开 RSB 文件 + 按偏移读」的代码，
iOS 侧想直接读 app bundle 内的 `main.rsb`（mobile 用户可读，已实测 grep/dd 成功），
需要知道**文件内偏移到数据指针的换算是否与内存态一致**。

---

## 四、iOS 侧当前计划

1. **验证 §0 的 +112 假设**：dump iOS rsm 头部 256 字节，比对 `rsm+224`（数量）
   与 `rsm+232`（包数组）是否合理。已实现探针，待真机跑一轮。
2. 若 rsm 定位成立 → 按 Q2 答复实现 iOS 版 `find_record`。
3. 若 Q3/Q4 表明 LANGUAGETYPES 也应走 LoadPackage → 找它的 tableId，改走游戏加载链。
4. 兜底：维持 `read_bytes_from_package` 返回 UnsupportedArchitecture，
   LANGUAGETYPES 继续走 CDN 外置通道（cdn-json 当前已提供 8 种语言）。

---

## 五、附录：iOS 已定位地址速查（本任务相关）

| 名称 | 偏移 | 说明 |
| --- | - | - |
| `0x7eb1c0` | 查找入口 | `(mgr, 裸名, out)` → w0；out+0x00/0x08 = 路径 string |
| `0x7eb234` | 名字规范化 | |
| `0x7eb280` | 主包查找 | 读 `*(base+0x27f4ed0)` |
| `0x7eb30c` | 挂载组查找 | 读 `*(base+0x27f4ed0)+2248` |
| `0x7ebc3c` | 组内查找 | `mov w1,#92` 取管理器字段 |
| `0x7ebaa8` | 名字规范化 2 | `/`↔`\`、剥层级 |
| `0x67f70` | 取记录 | `0x6800a8` new(1336) |
| `0x655f0` | 记录拷贝构造 | 1336B 对象 |
| `0x65428` | 外层壳 | 返回「记录+56」 |
| `0x64f20` | 按语言批量加载 | **不可作通用接口**（已实测崩溃） |
| `0x6525c` | 名字拆分 | `packages\levels\X.rton` → `X` |
| `0x2c9e04` | LoadPackageForTableFromRTONFile | `(ctx, tableId, 路径串)` |
| `0x760a20` | ApplyLocalizedStrings | 无参 |
| `0x8106e8` 调用点 | 参考序列 | `0x21fd8`(PVZDB) + `0x7f6634`(管理器) + `0x7eb1c0`(查路径) + `0x2c9e04`(加载) |
