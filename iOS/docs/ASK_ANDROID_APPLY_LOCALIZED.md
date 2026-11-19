# 向安卓侧请教：ApplyLocalizedStrings 的定位方法

（iOS 移植项目发出的提问文档。请安卓侧同学基于本文件回答，谢谢。）

## 一、背景（iOS 在做什么）

把安卓的「设置框架 + 语言模块」移植到 iOS（PvZ2 国际版 9.8.1 / arm64）。
语言热重载目标：**切换语言后，已显示文本立即跟切**（与安卓一致）。

当前做法（对齐安卓 hot_reload）：改语言标识 → 让游戏重新装载语言表 → 刷新已加载控件。

## 二、iOS 侧已确认的事实（均有真机日志/反汇编证据）

1. **语言标识**：`*(*(DisplayInfo)+2104)+508`，写入后游戏自己拼 `LawnStrings-<locale>`。
2. **装载 API 已找到且可用**：`0x2c9e04`，即 `PVZDB::LoadPackageForTableFromRTONFile`
   （二进制内字符串 `PVZDB::LoadPackageForTableFromRTONFile failed to parse` 证实）。
   调用方式：`x0 = 上下文，w1 = tableId(131)，x2 = 路径串对象`。
   传入 CDN 的绝对路径调用，**返回成功**：
   ```
   lang file apply: begin table=131 path=.../CDN.2.0/LawnStrings-zh-cn.rton
   lang file apply: end ret=0x1
   ```
3. **本地化查询口**：`0x179c14`（336 个调用点）。
   它内部读 `manager = *(base+0x27f4ed0)`，容器在 `manager + 1984`。
   （已确认 iOS 的 `base+0x27f4ed0` 对应安卓的 `qword_26C5ED8` 角色。）
4. **卡点**：装载成功，但查询仍返回旧语言。
   已用「冷门键」实验排除缓存解释：切换前后 `ADVICE_USE_MAGNIFYING_GRASS`
   都返回英文原文 → **表/容器确实没有被换成新语言**。

## 三、我们的判断

缺的是安卓 `OFF_APPLY_LOCALIZED_STRINGS = 0x13D0F30` 的对应步骤
（iOS 侧该项仍是 TODO = 0）。按安卓注释，它的作用是：
> 读取 table 131 的键值对，逐项写入 qword_26C5ED8 的本地化容器，完成后卸载 table 131。

即：装载只是把数据放进 table 131，**还需要一步把 table 131 的内容灌进本地化容器**。

## 四、想请教的具体问题

1. `sub_13D0F30` 的**完整签名与调用约定**：参数（几个、各是什么）、返回值、
   是否需要前置条件（如必须先 LoadPackage 到 table 131）？
2. 它内部**如何遍历 table 131 的键值对**？用哪个函数写入容器
   （是不是注释里提到的 `sub_15407DC(owner, key, value)`）？
3. 写入容器时，**容器的具体字段/偏移**是什么？（我们这边是 `manager+1984`，
   想知道安卓那边容器在 `qword_26C5ED8` 的哪个偏移，以便交叉验证 iOS 是否同构。）
4. 完成后**卸载 table 131** 这一步（注释提到）是怎么做的、是否必需？
5. 安卓热重载的**精确调用顺序**与时机：
   `LoadPackage(tableId, path)` → `ApplyLocalizedStrings()` → 广播/刷新？
   各自在哪个线程、哪一步失败会怎样降级？
6. 如果方便，能否给出 `sub_13D0F30` 的**关键反汇编片段或伪代码**？
   我们有完整 disassembly，可以据此在 iOS 上做「逐指令对照」找同构函数。

## 五、iOS 侧可用于比对的锚点（供你判断同构性）

| 角色 | 安卓 | iOS |
| --- | --- | --- |
| 本地化查询口 | sub_14F3354 | 0x179c14（内部 0x179abc） |
| 容器 owner 全局 | qword_26C5ED8 | base+0x27f4ed0 |
| 容器偏移 | ？ | manager+1984 |
| 装载 RTON | sub_120AEFC (OFF_DB_LOAD_RTON) | 0x2c9e04（已可用） |
| 应用本地化 | **sub_13D0F30（本问）** | **未定位（TODO）** |
| 容器插入 | sub_15407DC ? | 未定位 |

## 六、我们期望的产出

只要能说清 `sub_13D0F30`「**做什么、按什么顺序、访问哪些字段**」，
我们就能在 iOS 二进制里按同样的访问模式找到同构函数，补上这最后一步。

（注：我们不需要安卓的地址本身，只需要行为了解 —— 找的是 iOS 的对应实现。）
