# v47_complete —— 功能完整基线存档

**验证状态**：用户实测「功能全部生效」。

## 包

```
PvZ2_v47_final.ipa   109,739,451 B
SHA256 46e51f3fc4f0cfb83d861f0e679fb2e37eabef137083a7c636ce764a0c8338d3
```

## manifest（22 站点）

```
0  3c4c04   图鉴 ButtonDepress         12 2c0bf0   表加载入口
1  3c44a8   图鉴 Update                13 735930   CDN apply 循环
2  3c5330   图鉴 PageCase0             14 2c9e04   单表 apply
3  3c55a4   图鉴 PageCase1             15 1ac6d50   FourCC → 字符串
4  6f349c   主界面重建                 16 756ae4   设置页构造（根标题）
5  757c18   createTab                 17 6978f8   横幅构建
6  759dd4   dispatch                  18 183b10   BoardZoom2（高视角缩放）
7  758a94   page                      19 183a58   BoardLayout 对齐
8  179c14   本地化入口                 20 5eddb4   StreetDinos 补偿
9  757b78   wstring ctor              21 1c8380   语言短码映射（News）
10 758740   checkbox ctor
11 757e58   行挂载
```

## 本版新增（相对 v43）

| 项 | 方式 | 位置 |
|---|---|---|
| 高视角缩放下限 | gate 站点 | `183b10` |
| 相机对齐修正 | gate 站点 | `183a58` |
| 街道恐龙补偿 | gate 站点 | `5eddb4` |
| News 语言短码 | gate 站点 | `1c8380` |
| 戴夫待机动画 | **打包期静态补丁** | `1a7440` + 洞穴 `18bea44` |
| 传送带递进计数 | **打包期静态补丁** | `31582c`/`31585c` + 洞穴 `18bea1c` |
| 清理 `75a0e4` 误报 | 删无用 hook | — |

### 戴夫补丁字节（`1a7440`，16 字节）

```
f000b8a0   adrp x0, 洞穴页          (目标 0x1018be000)
91291000   add  x0, x0, #0xA44     (→ 0x1018bea44)
14000002   b    +8                 (跳过原条件判断)
d503201f   nop                     (原 ldr x0,[x0])
```
字符串槽 `0x18bea44` = `"anim_%s_idle"`（13 字符 + NUL）

### 传送带补丁

入口 `0x31585c`：`1456a470`（`b` 到洞穴 `0x1018bea1c`）
分支 `0x31582c`：`d503201f`（NOP，原 `cbz x23`）

洞穴 `0x18bea1c` 十条指令：
```
910003e0  mov  x0, sp
940f0c40  bl   0x101c81b20        析构 v24
b0007968  adrp x8, 0x1027eb000
f9443908  ldr  x8, [x8, #0x870]   全局页
f9450d00  ldr  x0, [x8, #0xa18]   board
f9416668  ldr  x8, [x19, #0x2c8]  pool
9b1822a1  madd x1, x21, x24, x8
97a35611  bl   0x10019427c        场上植物计数
0b0002d6  add  w22, w22, w0
17a95b89  b    0x100315864       回原流程
```

## 验证日志（实机）

```
[CMP] dave idle-anim patch: already static @+0x1a7440
      (adrp=f000b8a0 add=91291000 b=14000002 nop=d503201f)
[CMP] conveyor count patch: already static @+0x31585c (b=1456a470)
      +0x31582c (nop) cave[0]=910003e0
localeMap hook(0x1c8380): ok
localeMap: caller=+0x75a424 before="" after="zh"
[STATIC-ONLY] static sites used=19, no-site skipped=7, blocked code-page writes=0
```

**`blocked code-page writes=0`** 是关键指标：所有代码页修改都已在打包期完成。

## ⚠️ 打包必守规则（这几轮踩的坑）

### 1. 静态补丁必须小端字节序

十六进制串是**指令字**，写入文件时必须倒序：

```
52810017 (指令字) → 17 00 81 52 (文件字节)
```

写反会毁掉整个函数，游戏在 constructor 后立刻闪退。曾因此白跑 v44/v45/A/B/C/D/E/E2 共 8 个包。

### 2. 必须用项目自带 entitlements.plist

`iOS/entitlements.plist`（2222 字节，含 `jb.pmap_cs.custom_trust=PMAP_CS_APP_STORE`）。

**不能**用 `ldid -e <底包>` 导出的那份（1758 字节），会少 4 条权限。

### 3. `/tmp` 会被系统清理

打包脚本必须放项目内：`iOS/tools/pack.sh`。曾因 `/tmp/func1.sh` 被清理，重建时引入上面两个 bug。

### 4. 三处静态补丁（底包不含，必须写入）

| 偏移 | 原值 | 目标（小端） | 含义 |
|---|---|---|---|
| `0x1cb7db0` | `17b1891a` | `17008152` | 字形缓存 2048 |
| `0x1cb7e1c` | `21008052` | `81008052` | 页上限 4 |
| `0x1c34674` | `e20315aa` | `220080d2` | 60fps |

### 5. 底包与架构

- 底包：`安装包/原版+Clean/PvZ 2_Clean`（fat armv7+arm64，79,997,376 B）
- 成品：thin 成 arm64 单架构（43,504,272 B 含签名）
- lztgate 只处理单架构，必须 `llvm-lipo -thin arm64` 后打站点

## 打包命令

```sh
OUTNAME=xxx.ipa MANIFEST=/tmp/mFinal.txt TNAME=build_X sh /var/mobile/Documents/SCodeProjects/PvZ2-lzt-settings-framework/iOS/tools/pack.sh
```

## daveconv 重新编译

```sh
export PATH=/var/jb/usr/bin:/var/jb/usr/lib/llvm-16/bin:$PATH
SDK=/var/jb/usr/share/SDKs/iPhoneOS.sdk
clang -isysroot $SDK -arch arm64 -O1 -o daveconv daveconv.c
```

## 剩余 7 个未挂站点（评估后决定不做）

| 站点 | 原因 |
|---|---|
| `1c310e8` `1c32040` `1c36564` | No_Backup 主路径；符号链接兜底已能跑 |
| `757a90` | 音量滑块行标题；已被其他机制覆盖 |
| `736d20` | 表名改写；语言切换已由 `2c0bf0`+`2c9e04` 覆盖 |
| `17a480` | 哈希表 find；兜底路径，主路径已通 |
| `75b128` | 行初始化探针；诊断用途 |

## 已知行为特性

**低视角相机对齐在本机不触发**：
```
aspect = 2.16425
低视角阈值 = 2.16666   （差 0.00241）
高视角阈值 = 1.69335   （触发）
```
设备比例刚好卡在低视角阈值下方。这是设计行为，不是 bug。若要低视角也对齐，把 `LOW_VIEW_ALIGN_ASPECT` 降到 2.16 以下。

**`H0 BoardZoom2: scale nan / 7.4e21`** 是构造期读未初始化字段，代码紧接着 `*scale = 1.0f` 覆盖，无害。
