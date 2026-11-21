# -*- coding: utf-8 -*-
"""Git 同步工作流（Windows 侧）

用途：一条命令完成拉取/推送/状态检查，避免手工敲 git。
设计前提：本机 git 在沙箱内无法调用外部 credential helper，
        但在完全访问权限下可直接使用 Windows 凭据管理器，无需内嵌 token。

子命令：
  status    查看本地与远程状态（默认）
  pull      拉取远程改动（fast-forward，遇冲突停下报告）
  push      推送本地提交
  sync      pull + push
  check     提交前自检（测试 + 工作区状态）

用法：
  python Tools/git_sync.py status
  python Tools/git_sync.py sync -m "feat: xxx"
  python Tools/git_sync.py check
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# 脚本位于 <仓库>/tools/，仓库根即 parents[1]
REPO = Path(__file__).resolve().parents[1]
BRANCH = "sync/android-baseline"

# 提交前要跑的静态测试（与归档脚本保持一致）
TESTS = [
    "tests/test_ios_platform_guards.py",
    "tests/test_fourcc_shim.py",
    "tests/test_glyph_patch_sites.py",
    "tests/test_build_modes.py",
    "tests/test_lzt_guards.py",
    "tests/test_lzt_branding.py",
]


def git(*args: str, check: bool = True, capture: bool = True) -> subprocess.CompletedProcess:
    """执行 git 命令，自动设置非交互环境。"""
    env = {"GIT_TERMINAL_PROMPT": "0"}  # 禁止交互提示，失败即报错
    cmd = ["git", *args]
    r = subprocess.run(cmd, cwd=REPO, capture_output=capture,
                       text=True, errors="replace", env={**__import__("os").environ, **env})
    if check and r.returncode != 0:
        out = (r.stdout or "") + (r.stderr or "")
        raise SystemExit(f"git {' '.join(args)} 失败:\n{out.strip()}")
    return r


def say(msg: str) -> None:
    print(msg)


# ---------------- status ----------------

def cmd_status() -> int:
    say("=== 仓库 ===")
    say(f"路径: {REPO}")
    say(f"分支: {git('branch', '--show-current').stdout.strip()}")

    say("\n=== 提交 ===")
    for line in git("log", "--oneline", "-3").stdout.strip().splitlines():
        say(f"  {line}")

    say("\n=== 工作区 ===")
    dirty = git("status", "--porcelain").stdout.strip()
    if not dirty:
        say("  干净，无未提交改动")
    else:
        lines = dirty.splitlines()
        say(f"  {len(lines)} 个文件有改动:")
        for l in lines[:20]:
            say(f"    {l}")
        if len(lines) > 20:
            say(f"    ... 另有 {len(lines) - 20} 个")

    say("\n=== 与远程对比 ===")
    local = git("rev-parse", "HEAD").stdout.strip()
    try:
        remote = git("rev-parse", f"origin/{BRANCH}").stdout.strip()
    except SystemExit:
        say("  远程引用不存在，先跑 pull")
        return 0

    if local == remote:
        say("  与远程一致")
    else:
        ahead = git("rev-list", "--count", f"origin/{BRANCH}..HEAD").stdout.strip()
        behind = git("rev-list", "--count", f"HEAD..origin/{BRANCH}").stdout.strip()
        if ahead != "0":
            say(f"  本地领先 {ahead} 个提交（待推送）")
        if behind != "0":
            say(f"  本地落后 {behind} 个提交（需拉取）")
    return 0


# ---------------- pull ----------------

def cmd_pull() -> int:
    say("=== 拉取 ===")
    # 先确认工作区干净——有未提交改动时拒绝 pull，避免陷入合并状态
    dirty = git("status", "--porcelain").stdout.strip()
    if dirty:
        say("  工作区有未提交改动，先提交或 stash 再拉取：")
        for l in dirty.splitlines()[:10]:
            say(f"    {l}")
        return 1

    git("fetch", "--all", "--prune")

    local = git("rev-parse", "HEAD").stdout.strip()
    remote = git("rev-parse", f"origin/{BRANCH}").stdout.strip()
    if local == remote:
        say("  已是最新")
        return 0

    behind = int(git("rev-list", "--count", f"HEAD..origin/{BRANCH}").stdout.strip())
    ahead = int(git("rev-list", "--count", f"origin/{BRANCH}..HEAD").stdout.strip())
    say(f"  远程有 {behind} 个新提交，本地有 {ahead} 个待推")

    # 只做 fast-forward；分叉就停下让用户决定
    r = git("merge", "--ff-only", f"origin/{BRANCH}", check=False)
    if r.returncode != 0:
        say("\n  无法 fast-forward（本地与远程已分叉）。")
        say("  需要人工决定：rebase 还是 merge")
        say(f"  {(r.stdout or '') + (r.stderr or '')}".rstrip())
        return 1

    say("  已更新到最新")
    for line in git("log", "--oneline", f"{local}..HEAD").stdout.strip().splitlines():
        say(f"    + {line}")
    return 0


# ---------------- check ----------------

def cmd_check() -> int:
    say("=== 提交前自检 ===")
    failed = 0
    for t in TESTS:
        p = REPO / t
        if not p.exists():
            say(f"  MISSING  {t}")
            failed += 1
            continue
        r = subprocess.run([sys.executable, str(p)], cwd=REPO,
                           capture_output=True, text=True, errors="replace")
        mark = "PASS" if r.returncode == 0 else "FAIL"
        tail = (r.stdout or r.stderr or "").strip().splitlines()
        say(f"  {mark}  {t}  {tail[-1] if tail else ''}")
        if r.returncode != 0:
            failed += 1

    if failed:
        say(f"\n{failed} 项失败，不建议提交")
        return 1
    say("\n全部通过")
    return 0


# ---------------- push ----------------

def cmd_push(message: str | None, skip_check: bool) -> int:
    dirty = git("status", "--porcelain").stdout.strip()

    if dirty and not message:
        say("工作区有改动但没给 -m 提交信息。")
        say("要么先提交，要么加 -m 让本脚本一并提交：")
        say('  python Tools/git_sync.py sync -m "feat: xxx"')
        return 1

    if dirty and message:
        if not skip_check:
            rc = cmd_check()
            if rc != 0:
                say("\n自检未通过，已中止提交（确认无误可加 --skip-check）")
                return rc
        say("\n=== 提交 ===")
        git("add", "-A")
        git("commit", "-m", message)
        say(f"  已提交: {message}")

    say("\n=== 推送 ===")
    local = git("rev-parse", "HEAD").stdout.strip()
    try:
        remote = git("rev-parse", f"origin/{BRANCH}").stdout.strip()
    except SystemExit:
        remote = ""

    if local == remote:
        say("  无需推送（与远程一致）")
        return 0

    r = git("push", "origin", BRANCH, check=False)
    if r.returncode != 0:
        out = (r.stdout or "") + (r.stderr or "")
        say(f"  推送失败:\n{out.strip()}")
        if "rejected" in out or "non-fast-forward" in out:
            say("\n  远程有新提交，先 pull 再推：")
            say("    python Tools/git_sync.py pull")
        return 1

    say(f"  已推送 {remote[:7]}..{local[:7]}")
    return 0


# ---------------- main ----------------

def main() -> int:
    ap = argparse.ArgumentParser(description="Git 同步工作流")
    ap.add_argument("action", nargs="?", default="status",
                    choices=["status", "pull", "push", "sync", "check"])
    ap.add_argument("-m", "--message", help="提交信息（有未提交改动时必填）")
    ap.add_argument("--skip-check", action="store_true", help="跳过提交前测试")
    args = ap.parse_args()

    if not REPO.is_dir():
        print(f"仓库不存在: {REPO}", file=sys.stderr)
        return 1

    if args.action == "status":
        return cmd_status()
    if args.action == "pull":
        return cmd_pull()
    if args.action == "check":
        return cmd_check()
    if args.action == "push":
        return cmd_push(args.message, args.skip_check)
    if args.action == "sync":
        # 顺序很重要：有未提交改动时先提交，否则 pull 会因为工作区不干净而拒绝。
        dirty = git("status", "--porcelain").stdout.strip()
        if dirty and args.message:
            rc = cmd_push(args.message, args.skip_check)
            if rc != 0:
                return rc
            # 提交后再拉取，把远程新提交合进来
            rc = cmd_pull()
            if rc != 0:
                return rc
            # 拉取后如果又出现新提交（远端领先），需要再推一次
            if git("rev-list", "--count", f"origin/{BRANCH}..HEAD").stdout.strip() != "0":
                return cmd_push(None, True)
            return 0
        if dirty and not args.message:
            print("工作区有改动但没给 -m。加 -m 让脚本一并提交：")
            print('  python Tools/git_sync.py sync -m "feat: xxx"')
            return 1
        rc = cmd_pull()
        if rc != 0:
            return rc
        return cmd_push(None, args.skip_check)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
