# Git 同步工作流（Windows 侧）

> 脚本：`tools/git_sync.py`
> 适用：本机 Windows 环境，分支 `sync/android-baseline`
> 建立时间：2026-11-21

---

## 一、为什么需要这个脚本

直接用 `git` 命令有几个反复踩的坑：

| 坑 | 后果 |
|---|---|
| 有未提交改动时 `pull` 被拒 | 得先手动判断该提交还是 stash |
| 推送被拒（远程有新提交） | 得回头 `pull` 再 `push` |
| 忘记跑测试就提交 | 坏代码进了历史 |
| 中文提交信息被 PowerShell 加 BOM | 提交信息首字符异常（已踩过） |

脚本把这些固化成流程，一条命令完成。

---

## 二、环境前提（重要）

### 2.1 沙箱限制

本机 git 依赖的 `sh.exe` 在某些受限环境（如文件沙箱）下**无法创建命名管道**：

```
fatal error - couldn't create signal pipe, Win32 error 5
```

表现为 git 所有网络操作报：

```
schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS
```

**这不是凭据缺失，也不是配置问题。** 在完全访问权限下 `ls-remote` 可直接成功，无需内嵌 token。

判断方法：

```powershell
& "C:\Program Files\Git\usr\bin\sh.exe" -c "echo ok"
```

能输出 `ok` 就说明环境正常。

### 2.2 凭据

依赖 Windows 凭据管理器（`credential.helper=manager`，system 级已配置），无需额外操作。

**不需要**：内嵌 token、SSH key、手动 `credential fill`。

---

## 三、日常用法

### 3.1 查看状态（最常用）

```powershell
python tools/git_sync.py status
```

输出：

```
=== 仓库 ===
路径: ...\PvZ2-lzt-settings-framework
分支: sync/android-baseline

=== 提交 ===
  095793a docs: add dual-platform sync workflow for iOS handoff
  91c053c chore: establish dual-platform sync baseline (...)

=== 工作区 ===
  干净，无未提交改动

=== 与远程对比 ===
  与远程一致
```

### 3.2 一条命令提交并同步

```powershell
python tools/git_sync.py sync -m "feat: 完成某项改动"
```

执行顺序：

1. **自检** —— 跑 6 项静态测试，任一失败即中止
2. **提交** —— `git add -A` + `commit`
3. **推送** —— `push origin sync/android-baseline`
4. **拉取** —— `fetch` + `merge --ff-only`
5. 若拉取后发现远程领先，再补一次推送

### 3.3 只拉取

```powershell
python tools/git_sync.py pull
```

工作区不干净时会拒绝并列出文件——这是刻意的，避免陷入合并状态。

只做 fast-forward。本地与远程分叉时停下报告，不自动 merge/rebase。

### 3.4 只推送

```powershell
python tools/git_sync.py push                    # 推送已有提交
python tools/git_sync.py push -m "fix: xxx"      # 提交并推送
```

### 3.5 只自检

```powershell
python tools/git_sync.py check
```

提交前想先确认没问题时用。

---

## 四、自检内容

脚本跑的 6 项测试与归档脚本一致：

| 测试 | 检查什么 |
|---|---|
| `test_ios_platform_guards.py` | iOS 构建不引用 Android 专属符号 |
| `test_fourcc_shim.py` | naked 汇编 shim 结构正确 |
| `test_glyph_patch_sites.py` | 补丁点与真实 .so 逐字节一致 |
| `test_build_modes.py` | 构建模式约束 |
| `test_lzt_guards.py` | 护栏逻辑 |
| `test_lzt_branding.py` | 品牌与许可约束 |

任一项失败，`sync` 不会提交。

确认无误要跳过时用 `--skip-check`（**不建议**，这些测试都是为了拦住真实事故）。

---

## 五、常见情况处理

### 5.1 推送被拒（远程有新提交）

`sync` 会自动处理：先推、再拉、必要时补推。

如果手动 `push` 被拒：

```powershell
python tools/git_sync.py pull
python tools/git_sync.py push
```

### 5.2 本地与远程分叉

`pull` 会停下并提示：

```
无法 fast-forward（本地与远程已分叉）。
需要人工决定：rebase 还是 merge
```

此时手动选择：

```powershell
git rebase origin/sync/android-baseline    # 把本地提交放到远程之后
# 或
git merge origin/sync/android-baseline     # 生成合并提交
```

### 5.3 想撤销刚才的提交

```powershell
git reset --hard HEAD~1                        # 撤销本地
git push --force-with-lease origin sync/android-baseline   # 同步到远程
```

`--force-with-lease` 比 `--force` 安全：远程若有他人新提交会拒绝。

### 5.4 中文乱码

脚本输出中文时若显示乱码，在命令前加：

```powershell
chcp 65001 | Out-Null
$env:PYTHONIOENCODING="utf-8"
```

这只影响显示，不影响功能。

---

## 六、脚本行为约定

| 行为 | 说明 |
|---|---|
| 拒绝在有未提交改动时 pull | 避免半合并状态 |
| 只做 fast-forward | 分叉时停下，不擅自 merge |
| 提交前强制跑测试 | 防止坏代码入库 |
| `GIT_TERMINAL_PROMPT=0` | 禁止交互提示，失败即报错而非挂起 |
| 不使用 `--force` | 覆写远程需人工确认 |

---

## 七、与归档的关系

两者目的不同，都要用：

| | git 同步 | 归档 |
|---|---|---|
| 频率 | 每次改动 | 每轮真机验证通过后 |
| 命令 | `python tools/git_sync.py sync -m "..."` | `python Tools/archive_snapshot.py --tag ...` |
| 内容 | 源码与产物 | 源码 + 文档 + 环境快照 + 元数据 |
| 用途 | 协作、历史 | 冻结可用状态、可回滚 |

---

## 八、故障排查

### 8.1 `SEC_E_NO_CREDENTIALS`

先跑：

```powershell
& "C:\Program Files\Git\usr\bin\sh.exe" -c "echo ok"
```

- 输出 `ok` → 环境正常，检查网络
- 报 `couldn't create signal pipe` → 处于受限沙箱，需完全访问权限

### 8.2 `git` 命令找不到

```powershell
(Get-Command git).Source
```

应为 `C:\Program Files\Git\cmd\git.exe`。

### 8.3 脚本报「仓库不存在」

脚本按自身位置推导仓库路径（`<仓库>/tools/git_sync.py` → `<仓库>`）。若把脚本复制到别处，需改 `REPO` 常量。
