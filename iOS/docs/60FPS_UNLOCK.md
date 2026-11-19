# PvZ2 iOS 60FPS 解锁成功记录

> 记录一次从「30fps 限帧」到「60fps 画面刷新、动画原速」的完整攻关。
> 本文只记录**已验证成功**的事实、方法与数据，失败路径单独归档以便后来者避坑。

---

## 一、目标与关键区分

需求是把游戏从 30fps 提到 60fps，但**这里的 60fps 指画面刷新频率**，
与游戏内固定 30fps 的动画材质是两件事：

| 概念 | 含义 | 目标 |
| - | - | - |
| 画面刷新率 | 每秒向屏幕提交多少帧 | **60 Hz** |
| 动画帧率 | 精灵动画序列本身的帧数（美术资源按 30fps 绘制） | **保持 30fps 原速** |

**关键约束**：不能出现「动画快一倍」。这是本任务最容易踩的坑——
直接把限帧常量从 2 改成 1，若引擎不按真实时间补偿，所有动画会变成 2 倍速。

---

## 二、结论摘要

| 项 | 结果 |
| - | - |
| 限帧机制 | `CADisplayLink` + `-[CADisplayLink setFrameInterval:]`，值 **2**（60Hz ÷ 2 = 30fps） |
| 限帧调用点 | 主程序 `+0x1c34678`（`objc_msgSend` 调用 `setFrameInterval:`） |
| 默认值来源 | `+0x1c34654: mov w21, #2`（硬编码立即数 2） |
| 解锁方式 | 运行时 hook `setFrameInterval:`，把 `2` 改写为 `1` |
| 实测画面频率 | 基线 **30.00 Hz** → 解锁后 **50–60 Hz** |
| 动画速度 | **原速不变**（引擎按真实时间补偿，见 §六） |
| 崩溃 | 0 |
| 副作用 | 无（语言表、设置页、存档读写均正常） |

**开关方式**：**默认开启 60fps**；仅当 `Documents/lzt_fps60_off` 文件存在时回到原版 30fps（排障/对照用）。
同一份二进制可来回切换，无需重装。

---

## 三、失败的探测路径（先记下来，避免重走）

最初从「渲染 API」入手，连错两次：

| 尝试 | 结果 |
| - | - |
| hook `-[CAMetalLayer nextDrawable]` | hook 成功，24 秒 **0 次调用** |
| hook `-[CAMetalDrawable present]` / `presentWithOptions:` | hook 成功，**0 次调用** |
| 枚举 `MTKView` 实例 | **0 个** |
| 检查 `CAEAGLLayer` / `GLKView` 类 | 进程内**不存在** |
| 静态搜 `CADisplayLink` 字符串 | 二进制里**有**，但反汇编文本里搜不到（它在 `__objc_classname`，不在 `__text`） |

**根因**：这是 PopCap 的 **Sexy 框架**（类名 `SexyApplicationDelegate` 是标志），
渲染后端不走 `CAMetalLayer` 的公开呈现 API，所以从渲染侧 hook 什么也抓不到。

**教训**：先确定「主循环由谁驱动」，再谈渲染。对老 2D 引擎，`CADisplayLink`
才是唯一的节拍源，渲染 API 是下游。

---

## 四、事实链（反汇编 + 运行时双向实证）

### 4.1 主循环由 CADisplayLink 驱动

运行时 hook `+[CADisplayLink displayLinkWithTarget:selector:]` 捕获到多个实例：

```
DLLINK[1]: target=0x302b906e0 tick=mainLoop frameInterval=1.000 maxFPS=60
```

- tick 方法是 `-[SexyApplicationDelegate mainLoop]`
- 创建时 `frameInterval=1`，**但随后被改成 2**（见 4.2）
- 屏幕 `maximumFramesPerSecond = 60`（iPhone 11，非 ProMotion）

### 4.2 限帧点：setFrameInterval 被设为 2

hook `-[CADisplayLink setFrameInterval:]` 抓到调用者与值：

```
DL4 setFrameInterval: value=2  bt=+0x1c3467c +0x8c9a5a9c ...
DL4 mainLoop: frameInterval=2.00 preferredFPS=30.00
```

栈回溯中只有 `+0x1c3467c` 落在**主程序**内（其余 `+0x9f...` 在 dyld 共享缓存，
属系统组件）。

对应反汇编：

```asm
101c3464c: ldrsw x21, [x0, #56]      ; 从配置对象读值
101c34654: mov   w21, #2             ; ★ 默认硬编码 2
101c34668: ldr   x0, [x22, #128]     ; self->displayLink
101c34670: ldr   x1, [x8, #1032]     ; sel = "setFrameInterval:"
101c34674: mov   x2, x21
101c34678: bl    objc_msgSend        ; [displayLink setFrameInterval:2]
```

配置键 `drivers.ios.displaylink_interval` 确实存在于二进制字符串表
（与 `drivers.ios.use_multitouch` 等并列），但实际未生效——代码走的是
`[x0+56]`，取不到时退回立即数 2。

### 4.3 真正推进一帧的函数

`mainLoop` 的实现在 `+0x1c34410`，内部有 dt 闸门：

```asm
101c3443c: bl   <取时间>
101c3447c: udiv x9, x20, x8          ; now → 微秒
101c34480: ldp  x10, x8, [x19,#104]  ; x10=上次时间, x8=阈值
101c34484: sub  x9, x9, x10          ; dt = now - last
101c344a0: cmp  x9, x8
101c344a4: b.ls <return>             ; dt <= 阈值 → 本帧不推进
```

当闸门放行时，tail-call 到 **`0x101c2d31c`**：

```asm
101c2d330: bl   0x101c35c70          ; 时间/倍率计算
101c2d344: ldr  x0, [x19, #184]
101c2d34c: ldr  x8, [x8, #248]
101c2d350: blr  x8                   ; → Update()
101c2d354: ldr  x0, [x19, #168]
101c2d35c: strb w8, [x0, #1548]      ; 置「绘制中」标志
101c2d364: ldr  x8, [x8, #120]
101c2d368: blr  x8                   ; → Draw()
101c2d370: strb wzr, [x8, #1548]     ; 清标志
```

**要点**：`Update()` 与 `Draw()` 是**两次独立调用**。这为「逻辑 30Hz + 画面 60Hz」
提供了技术可能（本次未使用，因为引擎已自行补偿）。

---

## 五、验证方法：用数据而非肉眼

### 5.1 WORKTICK 计数器

在 `0x101c2d31c`（真正推进一帧的入口）装 naked 钩子，只做计数后尾跳：

```asm
adrp x9, _g_workTicks@PAGE
add  x9, x9, _g_workTicks@PAGEOFF
ldr  x10, [x9]
add  x10, x10, #1
str  x10, [x9]
adrp x9, _g_origWork@PAGE
add  x9, x9, _g_origWork@PAGEOFF
ldr  x9, [x9]
br   x9
```

每 2 秒采样一次，输出 `WORKTICK: <Hz>`。

> 这条计数器比 `link.timestamp` 可靠。此前用 timestamp 差值算出
> 19 万 fps 的荒谬结果（link 未运行时 timestamp 语义不可靠），已弃用。

### 5.2 A/B 对照

通过 `Documents/lzt_fps60_off` 反向关闭，同一二进制两次冷启动对比。

---

## 六、实测数据

### 6.1 基线（无开关文件）

```
DL4 setFrameInterval: requested=2 applied=2 fps60=0
WORKTICK: 30.00 Hz (60 ticks in 2.000s)
WORKTICK: 29.99 Hz (60 ticks in 2.000s)
WORKTICK: 30.01 Hz (60 ticks in 1.999s)
```

精确 30Hz，与用户报告一致。

### 6.2 解锁（开关文件存在）

```
DL4 setFrameInterval: requested=2 applied=1 fps60=1
WORKTICK: 59.99 Hz (120 ticks in 2.000s)
WORKTICK: 56.01 Hz (112 ticks in 2.000s)
WORKTICK: 50.99 Hz (102 ticks in 2.000s)
WORKTICK: 60.02 Hz (120 ticks in 1.999s)
```

负载轻时稳定 60Hz，复杂场景 50–60Hz 波动（GPU/场景开销所限，非限帧导致）。

### 6.3 稳定性

- 多次冷启动，崩溃日志 **0 行**
- 语言表加载、设置页、存档读写均正常
- 用户主观确认：**UI 滑动更跟手，动画保持原速**

### 6.4 收尾：日志清洁度

诊断探针移除后复测：

| 状态 | 诊断噪声 | 功能日志 | 崩溃 |
| - | - | - | - |
| 开关开 | 7 类关键字计数全 **0** | 仅 `fps60: frameInterval 2 -> 1` 一行 | 0 |
| 开关关 | 7 类关键字计数全 **0** | **无** | 0 |

---

## 七、为什么动画没有变快

这是本次最幸运也最关键的一点：**引擎按真实时间补偿**。

依据：

1. `mainLoop` 内部先用真实时间算 `dt`，再与阈值比较（§4.3）——
   说明引擎关注「真实经过时间」而非「调用次数」。
2. `0x101c2d31c` 开头调 `0x101c35c70`，该函数负责时间/倍率计算；
   `Update()` 与 `Draw()` 分离，动画推进由 `Update` 依据时间决定。
3. 实测：更新频率从 30Hz 翻到 ~60Hz，动画视觉速度不变。

**因此不需要做「Update 隔帧空转、Draw 每帧执行」的解耦**——
若将来换了引擎版本或动画明显变快，再启用该方案（§4.3 已确认技术可行）。

---

## 八、实现位置

**功能只依赖两处**：

| 文件 | 内容 |
| - | - |
| `iOS/src/lzt_core_ios.mm` | `hk_dl4_setFrameInterval()`：把 2 改写为 1 |
| `iOS/src/lzt_core_ios.mm` | `lzt_ios_fps60_requested()`：检查开关文件 |

**诊断工具**（源码保留，**已停止安装**，不产生任何日志）：

| 文件 | 内容 | 状态 |
| - | - | - |
| `iOS/src/lzt_core_ios.mm` | `hk_dl_withTarget` / `lzt_ios_displaylink_report` | 未安装 |
| `iOS/src/lzt_core_ios.mm` | `lzt_ios_install_fps_probe(2)`（Metal 呈现） | 未安装 |
| `iOS/src/lzt_core_ios.mm` | `lzt_ios_rtprobe_tick` | 未安装 |
| `iOS/src/lzt_main_ios.cpp` | `hk_work` / `lzt_ios_work_report`（WORKTICK 计数） | 未安装 |

需要复测时，在 `wait_and_start()` 末尾恢复对应安装调用即可
（原先还有 2 秒 × 20 轮的 `dispatch_source` 报告定时器）。

核心逻辑（约 10 行）：

```objc
void hk_dl4_setFrameInterval(id self, SEL _cmd, NSInteger v) {
    const int want60 = lzt_ios_fps60_requested();
    NSInteger applied = v;
    if (v == 2 && want60) applied = 1;      // ← 唯一改动：2 → 1
    lzt_core::log_write("DL4 setFrameInterval: requested=%ld applied=%ld fps60=%d",
                        (long)v, (long)applied, want60);
    g_dl4OrigSetFI(self, _cmd, applied);
}
```

---

## 九、使用方法

```bash
# 60fps 默认开启，无需任何操作（lzt_fps60_on 已废弃且被忽略）

# 仅排障时临时关闭：创建空文件后重启游戏
touch <容器>/Documents/lzt_fps60_off

# 恢复默认（删除该文件后重启游戏）
rm <容器>/Documents/lzt_fps60_off
```

改完需重启游戏生效（`setFrameInterval` 在启动时调用一次）。

验证是否生效：日志中应**只有一行**

```
fps60: frameInterval 2 -> 1
```

关闭状态下日志完全无相关输出（已实测：`FPSPROBE` / `DLLINK` / `RTPROBE` /
`DL4` / `WORKTICK` / `DL5` / `fps60` 计数均为 0）。

---

## 十、踩坑记录（给后来者）

1. **`dispatch_source_set_timer` 的 interval 不能给 `DISPATCH_TIME_FOREVER`**
   ——那是「只触发一次」。必须给 `2 * NSEC_PER_SEC` 这类周期值。
   本次因此浪费了一轮测试（只拿到一行数据）。
2. **对老引擎不要先猜渲染 API**。`CAMetalLayer` / `CAEAGLLayer` 全都不成立，
   白装了三套探针。先找 `CADisplayLink` 是正路。
3. **`link.timestamp` 不能用来测频率**。link 未运行时其值不可靠，
   会算出 19 万 fps。用「数回调次数」最稳。
4. **`extern "C"` 声明不要写进匿名 namespace**——会得到内部链接，
   链接期报 undefined。必须放文件作用域。
5. **`std::lock_guard<std::mutex> lk(m);` 有 most vexing parse 风险**，
   写成 `lk{m}`。
6. **栈回溯必须减 base 再记录**，否则每次启动地址都不同，无法对照反汇编。
7. **不要用 `sed` 反复拼大段结构**（本次迁移动一个 mutex 定义失败三次），
   要么一次性切块拼接，要么直接用 file 编辑工具并立即 grep 校验。

---

## 十一、地址速查表

> 全部为 `vmaddr - 0x100000000` 形式的偏移，用时加 `lzt_core::base()`。

| 偏移 | 含义 |
| - | - |
| `0x1c34410` | `-[SexyApplicationDelegate mainLoop]` 实现 |
| `0x1c2d31c` | 真正推进一帧（调 Update + Draw） |
| `0x1c34678` | `[displayLink setFrameInterval:]` 调用点 |
| `0x1c34654` | 硬编码默认值 `mov w21, #2` |
| `0x1c35c70` | 时间/倍率计算（被 `0x1c2d31c` 调用） |
| selref `0x1026e2408` | `"setFrameInterval:"` 的选择子槽 |

---

## 十二、未解与后续

- **渲染后端仍未确定**：链接了 Metal / OpenGLES / GLKit，但公开呈现 API 零调用。
  当前不影响功能（限帧点在主循环，不在渲染层）。若将来需要更细粒度的渲染控制，
  可继续追 `0x101c35c70` 与 `[self+168]` 的 vtable 实现。
- **ProMotion 设备**：本机 `maxFPS=60`。若在 120Hz 设备上运行，
  `frameInterval=1` 会得到 120Hz；如需固定 60，应改用
  `preferredFramesPerSecond = 60`（本次未验证，仅作提示）。
- **诊断探针已停止安装**：`WORKTICK` / `DL4` / `FPSPROBE` 等系列源码保留但不再
  安装，不产生日志（实测关闭态噪声计数全 0）。功能只依赖
  `hk_dl4_setFrameInterval`，与诊断代码完全解耦。

---

*文档生成时间：2026-10-18*
*验证设备：iPhone 11 (iPhone12,1)，iOS 18.x，Dopamine rootless*
