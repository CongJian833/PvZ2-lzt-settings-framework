# 窗口迁移交接文档（iOS 语言模块 · 当前进度总览）

> 用途：把当前窗口的上下文完整交给新窗口。**新窗口请按顺序读：本文件 → `HANDOFF_HOTRELOAD.md` → `FINDINGS.md`。**
> 最后更新：News 内容本地化补丁完成；LANGUAGETYPES 数据包来源进行中（卡在 RSB 取记录原语）。

---

## 一、项目与用户约定（长期，勿偏离）

- 目标：把**安卓**项目 `PvZ2-lzt-settings-framework` 移植到 iOS（PvZ2 国际版 9.8.1 / arm64）。
- **以安卓为地基**，不另起炉灶；平台差异只留在平台层；**逻辑要对齐安卓**。
- 双版本同步维护；包内注入（dylib 打进 .app，等价安卓 so 打进 APK）。
- **每次改完必须编译 + 部署 + 真机验证**；不要连续做多个未验证的改动。
- sudo 密码 `123456`；危险操作先确认；git 提交等用户发话（用户目前无提交计划）。
- 用户偏好：**先取证再改码**；一次只做一步；不要盲搜要「高效打点」。
- **可以自动启停游戏做探测**（用 `uiopen` 启动、`kill` 关闭），但**方向抉择交给用户**。

---

## 二、环境（重要，重装后已变更）

| 项 | 值 |
| --- | --- |
| 设备 | iPhone12,1 / iOS 17.5.1 / rootless（/var/jb）/ ElleKit |
| 游戏包 | `/var/containers/Bundle/Application/D59F5916-2310-4762-AF40-03DDA143EEB1/PvZ2.app` |
| 数据容器 | `/var/mobile/Containers/Data/Application/00FF737F-A94D-48DD-A321-8D37742B9234` |
| 日志 | `<容器>/Documents/lztsettingsframework.log`（每启覆盖） |
| 崩溃日志 | `<容器>/Documents/lztsettingsframework_crash.log`（追加，含 PC/bt 偏移） |
| CDN 目录 | `<容器>/Documents/No_Backup/CDN.2.0/`（当前只有 LANGUAGETYPES.json） |
| 待注入包体 | `/var/mobile/Containers/Shared/AppGroup/B0708F7E-.../File Provider Storage/PvZ2/PvZ 2 701 9.8.1`（Payload/PvZ2.app） |
| 工作目录 | `/var/mobile/Documents/SCodeProjects/PvZ2-lzt-settings-framework`（`iOS/` = iOS 侧，未纳入 git） |

### 常用命令
```sh
# 编译
cd iOS && sh build.sh

# 部署 dylib（主二进制未变时只需这一步，避免整体重签 OOM）
APP=/var/containers/Bundle/Application/D59F5916-2310-4762-AF40-03DDA143EEB1/PvZ2.app
sudo cp -f iOS/build/liblztsettingsframework.dylib "$APP/Frameworks/"
sudo chown _installd:_installd "$APP/Frameworks/liblztsettingsframework.dylib"
sudo chmod 755 "$APP/Frameworks/liblztsettingsframework.dylib"

# 启动/关闭游戏（shell 直跑二进制不进入完整流程，必须用 uiopen）
sudo uiopen -b com.popcap.ios.PvZ2.cln
for p in $(ps -A -o pid,comm | grep "PvZ2.app/PvZ2" | sed 's/^ *//' | cut -d' ' -f1); do sudo kill -9 $p; done

# 定向反汇编（地址 = 文件 vmaddr，已是 0x100000000+偏移）
llvm-objdump-16 --disassemble --start-address=0x100XXXXXX --stop-address=0x100XXXXXX /tmp/pvz.arm64
# 全量反汇编文本已存在：/tmp/pvz.dis（约 390MB，grep 用）
# 宽串查找工具：iOS/tools/wfind.c → /tmp/wfind <macho> <ASCII>
```

### 地址规则（务必记住）
- 文件里 vmaddr 已是 `0x100000000 + 偏移`；运行时地址 = `base + 偏移`，其中 `base = slide + 0x100000000`。
- **绝不要写成 base + vmaddr**（早期失误导致 6 处 hook 打到 dyld 共享缓存）。

---

## 三、已完成并真机验证的功能（四项，全部可用）

### 1. 设置框架 + 视角模块（第一阶段）
Tab 注入、页面物化、滚动、互斥、文本刷新；视角：高视角缩放、黑边对齐、DEFER、恐龙补偿、watchdog、patch 监控。

### 2. 语言热重载（第二阶段核心，已打通）
完整链路（对应安卓 hot_reload）：
1. 写语言标识 `*(*(DisplayInfo)+2104)+508`（**必须先于装载**）
2. 装载语言表：**0x2c9e04** = `PVZDB::LoadPackageForTableFromRTONFile`
   - 外置：CDN 文件的**绝对路径**；兜底：`packages\LawnStrings-<locale>.rton`
3. 灌进容器：**0x760a20** = 安卓 `sub_13D0F30` 的等价物（`ApplyLocalizedStrings`，**无参**）
   - 缺这一步 → 装载成功但查询仍是旧语言（这是卡最久的关键）
4. 回填已显示行：游戏本地化口 **0x179c14**（**键必须裸键**，内部会补方括号）

### 3. 设置页全部行文本跟切
| 角色 | 地址 |
| --- | --- |
| 行初始化器（Tab 行等） | **0x75b128**(row, 键)（内部取文本写入 row+184） |
| 行挂载（所有行共同落点） | **0x757e58** |
| 行标题字段 | 恒为 **row+184** |
| 键的编码 | **UTF-16LE 宽串**（按窄串读会截断） |
| 精确认领 | `lzt_ios_capture_row_by_key`：仅登记「字段文本 == 键当前语言文本」 |
| 滑条行（Music / Sound FX） | 经行挂载 + `try_claim_row_by_text` 精确匹配捕获 |

### 4. 主界面 / 横幅 / News
| 项 | 地址 | 做法 |
| --- | --- | --- |
| 横幅重建 | **0x6978f8** | hook 记录对象 → 切语言时重调 |
| 主界面重建 | **0x6f349c** | 同上 |
| 刷新时机 | — | **必须在换表之后**；设置页打开时挂起，关闭设置页（dispatch `tabId==0`）时刷新（否则主界面新建节点盖住设置页） |
| News 短码映射 | **0x1c8380** | 读 `*(obj+2104)+508`，与短码表比对，**sret(x8)** 输出 |
| News 取键 | **0x1c6db0**（调用点 +0x1c6e40）；另一处 +0x75a424 | 兜底后生效 |
| News 兜底 | — | 映射结果为空串时，填入「语言标识首段两字母小写」（zh-cn→"zh"、jp-jp→"jp"）；非空原样放行 |

**News 兜底踩坑（已修）**：libc++ 短串长度字节在 **+23**，不是 +0；写错导致兜底无效。

---

## 四、当前进行中的任务：LANGUAGETYPES 数据包来源

### 目标
安卓来源优先级：`cdn-rton → cdn-json → package-rton`。iOS 前两项已可用；
**第三项（package-rton）未实现** —— `jni/resource_file.cpp` 的 `read_bytes_from_package` 在 iOS 是空实现，
原因：安卓依赖的 `OFF_PACKAGE_FIND_RECORD = 0x16B31EC`（按名字取 RSB 记录）在 iOS **未定位**。

用户已把 `LANGUAGETYPES.rton` 放进数据包 `packages/` 目录，满足条件。

### 已探明的事实（本窗口成果）
1. **名字构造**：`0x6525c` 把裸名拼成 `packages\xxx.rton`（与安卓格式一致）。
   实测抓到 60 条真实名字，全部 caller=+0x65084。
2. **包内资源读取链**：`0x64f20` 是「遍历挂载组」的批量加载函数
   （内部常量 "Packages"，读 `[x8+8]/[x8+0x10]` 数组，用虚表 +32 方法匹配名字）。
   ⚠️ 它 **x0 是输出对象**，不接受外部名字 → **不可作为通用读取接口**（主动调用会崩，实测 pc=+0x64f44/+0x64f9c）。
3. **单次取条目**：`0x65428(res, &名字, out)`，在名字匹配成功后调用。
   实测：res=**0x3064ea7f0**（启动期缓存的资源对象，每次相同）；名字为空时返回 0。
   但**主动用它读 LANGUAGETYPES 返回 0、out 全空** → 它疑似是该批量链内部专用回调，非通用接口。

### 卡点
iOS 与安卓结构不同：安卓是 `PakMgr → rsm → radix 直查`；
iOS 是「遍历挂载组 + 逐项回调」。**「按名字直查记录」的原语尚未出现。**

### 诊断代码现状（在 lzt_main_ios.cpp 内，均可用开关关闭）
| 开关 | 作用 |
| --- | --- |
| `kEnableRsbNameProbe` | 0x6525c 名字构造探针（**当前 true**） |
| `kEnableRsbFetchProbe` | 0x65428 取条目探针（**当前 true**） |
| `kEnablePakMgrProbe` | pakMgr 结构 dump（当前 true，实测该槽为 0 —— 说明 iOS pakMgr 全局≠base+0x27f4ed0） |
| `kEnableRsbReadProbe` | 0x64f20 只读观测（当前 true） |
| `kEnableNewsLocaleHook` | **News 功能开关（必须 true）** |
| `kEnableNewsLocaleFallback` | **News 兜底开关（必须 true）** |
| `kEnableAutolangDebug` | 自动切换语言调试入口（当前 false） |
| 其余 `kEnable*Probe` | 诊断探针，均 false |

⚠️ **教训**：News 功能开关曾与「诊断日志开关」共用，收尾时误关导致功能回归 —— 功能开关要与日志开关解耦。

### 下一步可选方向（请用户定）
- A. 继续找 iOS「按名字直查记录」原语（只读观测游戏自己取表时的参数，预计 2~4 轮）。
- B. 写文档求助安卓 agent（本目录已有 `ASK_ANDROID_APPLY_LOCALIZED.md`、`ASK_ANDROID_NEWS_LOCALE.md` 两份成功的先例）。
- C. 暂缓本项，先做四项功能的完整验收归档。

---

## 五、关键经验（本窗口积累，勿重犯）

1. **不要用内存形态推断来捕获对象**：曾把 vtable 当标题写入 → 崩溃（addr=被写入的文本字节）。
   正确做法：限定在「确认是设置行的对象」或用**文本精确匹配**。
2. **钩子安装必须真幂等**：同一函数二次打补丁 → 跳转错乱 →「只有本模块 Tab 崩、原版 Tab 正常」。
3. **不要在 dispatch 调用栈内读页面容器**（page+240 仍在构建）→ 崩（+0x75a10c）。
4. **认领代码不能放在 `if (!active) return;` 之后**（原版设置页 active 恒 false → 永不执行）。
5. **0x757ff8（滑条行创建）不可 inline hook**（首指令含 PC 相对寻址）。
6. **sret 约定**：多处函数用 x8 传输出对象（0x760bb4 / 0x1c8380 / 0x179c14），naked 桥必须保存并恢复 x8。
7. **libc++ 短串**：长度在 **+23**；长串 `ptr@+0/len@+8`。
8. **objdump 对 16 位立即数不显式标注** → 找常量要用「32 位常量对」（如 ENUS=0x454E5553 = `mov w'+movk`）或十进制形式。

---

## 六、可用备份

```
iOS/backup/src/
  lzt_main_ios.cpp.hotreload-ok   # 热重载主链路打通
  lzt_main_ios.cpp.mainmenu-ok    # 主界面 + 横幅可用
  lzt_main_ios.cpp.news-ok        # News 兜底可用
  lzt_main_ios.cpp.rsbprobe-2026  # 最新（含 RSB 诊断探针，当前开发态）
  ...（更早的 stage1/v2/v3/v4/bt/lookup/rsb 等）
```

## 七、注入包 / ipa 打包（已完成一次）

- 脚本：`iOS/inject_pkg.sh`（注入指定包体并打包 ipa，产物 `iOS/build/PvZ2-9.8.1-lzt.ipa`）
- 已确认：dylib 在 .app/Frameworks/，主二进制含 LC_LOAD_DYLIB，签名标识 `com.popcap.ios.PvZ2`
- 结论（已答复用户）：别人**自签/企业签后可安装**；改包名（CFBundleIdentifier）不影响加载（用 `@executable_path` 相对路径）
- 提醒：中文表在数据容器，不随包走；ad-hoc 签名只在越狱机有效
