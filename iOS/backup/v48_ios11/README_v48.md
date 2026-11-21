# v48_ios11 —— 最终归档基线

**最低系统要求：iOS 11.0**　**用户实测：一切正常**

## 包

```
PvZ2_v48_ios11.ipa   109,740,194 B
SHA256 754716e12523690e9e1dc2e2f7169451fbf5458aa8f003a3b7f3d10070700fc6
```

## 相对 v47 的唯一变化

| 项 | v47 | v48 |
|---|---|---|
| dylib `minos` | 15.0 | **11.0** |
| `Info.plist` `MinimumOSVersion` | 10.0（声明与实际不符） | **11.0**（已对齐） |
| Metal 探针 API | 裸调用 | 加 `respondsToSelector` + `valueForKey` |
| `backtrace` 兼容 | 无 | 新增 `lzt_backtrace_shim.h` |

功能行为与 v47 **完全一致**（日志已核对）。

## 降版本暴露并修掉的真实 bug

`lzt_core_ios.mm` 里两个 `CAMetalLayer` 属性原本是裸调用：

```
allowsNextDrawableTimeout   → iOS 11.0
maximumDrawableCount        → iOS 11.2
```

`minos=15.0` 时编译器与运行时都不会碰到。降到 11.0 后，在 iOS 11.0/11.1 上会 `unrecognized selector` 崩溃。

修正方式（两处）：

```objc
([ml respondsToSelector:@selector(allowsNextDrawableTimeout)]
    ? [[ml valueForKey:@"allowsNextDrawableTimeout"] boolValue] : 0)
([ml respondsToSelector:@selector(maximumDrawableCount)]
    ? [[ml valueForKey:@"maximumDrawableCount"] unsignedLongLongValue] : 0ULL)
```

用 `valueForKey` 而非直接属性访问，是为了同时绕开编译期 `-Wunguarded-availability-new`。
现在 `minos=10.0` 也能无告警编译。

## 为什么下限是 11.0 而不是更低

dylib 引用 164 个未定义符号，逐项核对后**只有 `_backtrace` 是 iOS 11.0 才有**。

它用在两处诊断路径（`kEnableFileProbe=false`，永不执行），但**符号会进入未定义符号表，dyld 加载时必须能解析**。

`lzt_backtrace_shim.h` 已提供垫片：当 `__IPHONE_OS_VERSION_MIN_REQUIRED < 110000` 时把 `backtrace` 换成返回 0 的空实现。**这次没用到（minos=11.0），但保留以备将来降得更低。**

## 兼容性矩阵

详见 `compat.html`（同目录）。要点：

- iOS 10：dylib 不加载，**全部功能静默消失**
- iOS 11.0+：所有游戏功能改造生效；仅 Metal 探针在 11.0/11.1 受限（默认关闭）
- iOS 12.5 与 15+ 行为完全一致
- 相机对齐受**屏幕比例**影响（低视角阈值 2.16666，本机 2.16425 未触发）

## 验证日志（实机）

```
view hooks: BoardZoom2 hooked (force high view)
view hooks: BoardLayout_ApplyZoom hooked (align)
view hooks: StreetDinos hooked (xBase compensate)
H1 ALIGNED aspect=2.16425 highView=1 b283=-556 b286=69 -> b270=b284=556 b285=-243
[CMP] dave idle-anim patch: already static @+0x1a7440
      (adrp=f000b8a0 add=91291000 b=14000002 nop=d503201f)
[CMP] conveyor count patch: already static @+0x31585c (b=1456a470)
      +0x31582c (nop) cave[0]=910003e0
[STATIC-ONLY] static sites used=19, no-site skipped=7, blocked code-page writes=0
```

`blocked code-page writes=0` 是关键指标：所有代码页修改都已在打包期完成。

## manifest（22 站点）

```
0  3c4c04   图鉴 ButtonDepress         12 2c0bf0   表加载入口
1  3c44a8   图鉴 Update                13 735930   CDN apply 循环
2  3c5330   图鉴 PageCase0             14 2c9e04   单表 apply
3  3c55a4   图鉴 PageCase1             15 1ac6d50   FourCC → 字符串
4  6f349c   主界面重建                 16 756ae4   设置页构造（根标题）
5  757c18   createTab                 17 6978f8   横幅构建
6  759dd4   dispatch                  18 183b10   BoardZoom2
7  758a94   page                      19 183a58   BoardLayout 对齐
8  179c14   本地化入口                 20 5eddb4   StreetDinos 补偿
9  757b78   wstring ctor              21 1c8380   语言短码映射
10 758740   checkbox ctor
11 757e58   行挂载
```

## ⚠️ 打包必守规则（历史踩坑）

### 1. 静态补丁必须小端字节序

```
52810017（指令字）→ 17 00 81 52（文件字节）
```

写反会毁掉整个函数 → constructor 后立刻闪退。曾因此白出 8 个包。

### 2. 必须用项目自带 `entitlements.plist`

（2222 B，含 `jb.pmap_cs.custom_trust=PMAP_CS_APP_STORE`）
**不能**用 `ldid -e` 从底包导出（1758 B，少 4 条权限）。

### 3. `/tmp` 会被系统清理

打包脚本用 `iOS/tools/pack.sh`，别放 /tmp。曾因 `/tmp/func1.sh` 消失后自行重建，引入字节序和 entitlements 两个 bug。

### 4. 三处主程序静态补丁（底包不含，必须写入）

| 偏移 | 原值 | 目标（小端） | 含义 |
|---|---|---|---|
| `0x1cb7db0` | `17b1891a` | `17008152` | 字形缓存 2048 |
| `0x1cb7e1c` | `21008052` | `81008052` | 页上限 4 |
| `0x1c34674` | `e20315aa` | `220080d2` | 60fps |

### 5. 底包与架构

- 底包：`安装包/原版+Clean/PvZ 2_Clean`（fat armv7+arm64，79,997,376 B）
- 成品：thin 成 arm64 单架构
- lztgate 只处理单架构，必须先 `llvm-lipo -thin arm64`

## 打包命令

```sh
OUTNAME=xxx.ipa MANIFEST=/tmp/mFinal.txt TNAME=build_X \
  sh /var/mobile/Documents/SCodeProjects/PvZ2-lzt-settings-framework/iOS/tools/pack.sh
```

## daveconv 重新编译

```sh
export PATH=/var/jb/usr/bin:/var/jb/usr/lib/llvm-16/bin:$PATH
SDK=/var/jb/usr/share/SDKs/iPhoneOS.sdk
clang -isysroot $SDK -arch arm64 -O1 -o daveconv daveconv.c
```

## 剩余 7 个未挂站点（评估后不做）

| 站点 | 原因 |
|---|---|
| `1c310e8` `1c32040` `1c36564` | No_Backup 主路径；符号链接兜底已能跑 |
| `757a90` | 音量滑块行标题；已被其他机制覆盖 |
| `736d20` | 表名改写；语言切换已由 `2c0bf0`+`2c9e04` 覆盖 |
| `17a480` | 哈希表 find；兜底路径，主路径已通 |
| `75b128` | 行初始化探针；诊断用途 |

## 已知行为特性

**`H0 BoardZoom2: scale nan / 7.4e21 / -2.3e38`** —— 构造期读未初始化字段，代码紧接着 `*scale = 1.0f` 覆盖，无害。反而证明钩子在对象构造期就成功介入。

**低视角对齐不触发** —— 阈值 2.16666，本机比例 2.16425。若要启用，把 `LOW_VIEW_ALIGN_ASPECT` 降到 2.16 以下（属功能调整，非 bug 修复）。
