# v54 — 静态 gate 双环境验证通过

## 一、版本信息

```text
PvZ2_v54_final.ipa
大小：109,740,789 B
SHA256：08bdaafd6368181f164606b042ed1a7a882432e8c9cc3476b8046f05490b26a2
```

与 v53 功能代码完全相同。差异仅在打包来源：v54 使用 Android 端交付的完整工程（已验证其 `jni/` 改动不影响 iOS），v53 使用设备原有工程。

两者产物体积差 6 字节，来自签名时间戳。

## 二、产物核验

| 项 | 值 |
|---|---|
| 主程序 | 43,504,272 B（arm64 only） |
| `LZTGATE1` | `0x6BD0`（27600），全文件唯一 |
| gate 头 | stride=48，count=22 |
| 入口打桩 | `+0x3c4c04`/`+0x757c18`/`+0x756ae4`/`+0x1c8380` 均为 `B` 指令 |
| dylib | 275,616 B，含 `STATIC-ONLY` 标志 |
| `MinimumOSVersion` | 11.0 |
| 戴夫补丁 | `f000b8a0 / 91291000 / 14000002 / d503201f` |
| 传送带补丁 | `0x31585c`=`1456a470`，`0x31582c`=`d503201f` |

打包自检全过：

```text
magic @0x6bd0（唯一）
stride=48 count=22 ✓
距 sizeofcmds 18600 字节（≥4096）✓
全部入口均为 B 指令 ✓
```

## 三、双环境验证结果

| 指标 | LiveContainer | 企业签名 |
|---|---|---|
| `main __TEXT base` | `0x10d094000` | `0x104e00000` |
| `gate_hdr` | `0x10d09abd0` | `0x104e06bd0` |
| **相对基址偏移** | **+0x6BD0** | **+0x6BD0** |
| `ncmds` | 81 | 79 |
| gate 完整性 | all 22 sites OK | all 22 sites OK |
| `static sites used` | 19 / 跳过 7 | 19 / 跳过 7 |
| `blocked code-page writes` | 0 | 0 |
| `inline hook` 残留 | 0 | 0 |
| `NATIVE_CRASH` | 0 | 0 |
| tab id | 30 / 31 | 30 / 31 |

**核心结论**：`gate_hdr - base` 在两环境下都精确等于 `0x6BD0`，证明 gate 定位不受 ASLR、容器重定位、load command 数量变化影响。v49~v52 的修复目标已达成。

`ncmds` 81 vs 79 的差异（+104 字节）再次印证 v50 根因判断：旧 gate 在 `0x2330`，距 `sizeofcmds` 仅 40 字节会被覆盖；现 gate 在 `0x6BD0`，间隔 18,600 字节，注入量远不足以触及。

## 四、功能验证

全部正常，两环境一致：

- 设置 Tab：语言（slot 1 / id 30）、视角（slot 2 / id 31）
- 语言 hook：`langtable(0x2c0bf0)`、`localeMap(0x1c8380)`、`banner(0x6978f8)`
- 图鉴：depress / update / page（2/2）
- 视角三支：`BoardZoom2`、`BoardLayout_ApplyZoom`、`StreetDinos`
- 高视角对齐：`H1 ALIGNED aspect=2.16425 highView=1`
- 低视角按设计跳过：`aspect=2.16425 <= 2.16666`
- 静态补丁：字形缓存 2048、页上限 4、戴夫动画、传送带计数
- 60fps：由静态 frame-interval 补丁承担（`dl4 hook skipped`）

## 五、已知差异：LiveContainer 下语言表为空

| 项 | LiveContainer | 企业签名 |
|---|---|---|
| `LangTable[N]` 明细 | 0 条 | 16 条（8 语言） |
| 语言表来源 | `unavailable (langs=0)` | `source=cdn-json languages=8` |
| `no_backup redirect` | **hook A=FAIL / hook B=FAIL** | 符号链接成功 |
| `LRCDN cdn dir` | 无 | 2 条，指向 `No_Backup/CDN.2.0` |

**成因**：No_Backup 的三个 hook 站点（`0x1c310e8`、`0x1c32040`、`0x1c36564`）不在 manifest 中，静态模式下被跳过。越狱直装时符号链接兜底仍成功；LiveContainer 的容器路径多一层嵌套（`Documents/Data/Application/<UUID>/Documents/No_Backup`），兜底失败，游戏找不到 `CDN.2.0`。

**影响范围**：不影响 gate 机制、设置 Tab、视角、静态补丁、60fps。仅语言表加载受影响。

**这与 v52 时的判断需要修正**：此前认定这三个 No_Backup 站点"非必要，符号链接兜底已足够"——该结论在越狱直装下成立，在 LiveContainer 下不成立。

## 六、7 个缺失站点（两环境相同）

```text
1c310e8   No_Backup 主路径
1c32040   No_Backup 主路径
1c36564   No_Backup 主路径（AppSupGetter->DocGetter）
757a90    音量滑块行标题（已被语言切换覆盖）
17a480    哈希表 find 兜底
75b128    行初始化诊断探针
dl4       60fps DisplayLink 探测（由静态补丁替代）
```

其中前三个在 LiveContainer 下有实际影响（见第五节）。

## 七、后续可选修复

若需修复 LiveContainer 下的语言表加载：

1. 把 `1c310e8`、`1c32040`、`1c36564` 加入 `mFinal.txt`（22 → 25 站）
2. gate 区从 1,072 字节扩到 1,216 字节
3. `0x6BD0` 起点不变仍有余量（LC slack 18,600 → 18,456）
4. 需确认 `lztgate` 的 `cap` 计算正确（当前 `cap=22 used=22`，扩到 25 后 `cap` 应为 25）

注意：这三站点的首指令是否为 PC 相对需先验证——`lztgate` 对 PC 相对首指令会 REFUSE。

## 八、归档

```text
iOS/backup/v54_final/
  README_v54.md              本文
  v54双环境日志核对.md        详细日志对照
  mFinal.txt                 22 站点清单
  lzt_main_ios.cpp / lztgate.c / pack.sh / build.sh
```
