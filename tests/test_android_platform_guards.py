"""校验共享区源码在 Android 构建下不会引用 iOS 专属符号。

与 `test_ios_platform_guards.py` 互为对称：
    · 那个查 lzt_android_* 在 iOS 构建时是否泄漏
    · 这个查 lzt_ios_*   在 Android 构建时是否泄漏

背景：`lzt_ios_*` 系列函数定义在 `iOS/src/*.mm`，不在 Android 的编译
单元里（见 `jni/Android.mk`）。若共享区在 Android 构建路径上引用了未加
守卫的 `lzt_ios_*` **调用**，`ndk-build` 会报：

    error: undefined reference to 'lzt_ios_xxx'

Android 侧做 Android 编译时能发现，但改动共享区的若是 iOS 侧，就很容易
漏掉——这正是本测试要补的缺口。

两类问题的严重度不同，故分开处理：

    调用（引用）未加守卫  → 链接失败，**报错退出 1**
    纯声明未加守卫        → 不产生引用，仅在符号被用到时才链接，
                            属卫生问题，**警告但不阻断**

原理：逐个 `lzt_ios_*` 出现点，回溯其 #if 嵌套栈，判断 Android 构建
（LZT_PLATFORM_IOS=0, __aarch64__=1, __ANDROID__=1）时该行是否参与编译。

用法：
    python3 tests/test_android_platform_guards.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Android 构建下这些宏的取值（见 jni/offsets.h 的 #if defined(__APPLE__) 分支）
MACROS = {
    "LZT_PLATFORM_IOS": False,
    "__aarch64__": True,
    "__ANDROID__": True,
    "__APPLE__": False,
}

# 共享区：两端都编译的文件（jni/Android.mk 与 iOS/build.sh 的交集）
SHARED_GLOBS = [
    "jni/*.cpp",
    "jni/settings/*.cpp",
]

# 这些符号在 Android 侧确实存在同名实现，不视为 iOS 专属
ALLOWED = set()

TOKEN = re.compile(r"\blzt_ios_\w+\s*\(")
# 声明语句：extern 开头，或纯函数原型声明（以 ; 结尾且无赋值/调用上下文）
DECL = re.compile(r'^\s*(?:extern\s+(?:"C"\s+)?)?[A-Za-z_][\w\s:*&<>]*\blzt_ios_\w+\s*\(')


def eval_cond(cond: str) -> bool:
    """对 #if 条件做最小求值，只覆盖本项目实际用法。"""
    c = re.sub(r"/\*.*?\*/", "", cond.strip()).strip()

    def def_repl(m):
        name = m.group(1) or m.group(2)
        return "1" if MACROS.get(name, False) else "0"

    c = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)", def_repl, c)
    for name, val in MACROS.items():
        c = re.sub(r"\b" + re.escape(name) + r"\b", "1" if val else "0", c)
    # 剩余未知标识符按 0
    c = re.sub(r"\b[A-Za-z_]\w*\b", "0", c)
    c = c.replace("&&", " and ").replace("||", " or ")
    c = re.sub(r"!(?!=)", " not ", c)
    try:
        return bool(eval(c, {"__builtins__": {}}, {}))
    except Exception:
        # 求值失败时保守认为「参与编译」，让泄漏更容易被发现
        return True


def active_map(lines):
    """返回每行在 Android 构建下是否参与编译的布尔列表。

    用「分支组」模型：每个 #if 压栈一个 {any_taken, active}。
    #elif / #else 只有在同组前面没有任何分支成立时才可能激活。
    某行参与编译 ⟺ 栈上所有层的 active 均为 True。
    """
    result = [True] * len(lines)
    stack = []
    for i, line in enumerate(lines):
        if re.match(r"\s*#ifdef\s+(\w+)", line):
            name = re.match(r"\s*#ifdef\s+(\w+)", line).group(1)
            a = MACROS.get(name, False)
            stack.append({"any_taken": a, "active": a})
            result[i] = False
        elif re.match(r"\s*#ifndef\s+(\w+)", line):
            name = re.match(r"\s*#ifndef\s+(\w+)", line).group(1)
            a = not MACROS.get(name, False)
            stack.append({"any_taken": a, "active": a})
            result[i] = False
        elif re.match(r"\s*#if\s+(.*)$", line):
            cond = re.match(r"\s*#if\s+(.*)$", line).group(1)
            a = eval_cond(cond)
            stack.append({"any_taken": a, "active": a})
            result[i] = False
        elif re.match(r"\s*#elif\s+(.*)$", line):
            cond = re.match(r"\s*#elif\s+(.*)$", line).group(1)
            if stack:
                top = stack[-1]
                if top["any_taken"]:
                    top["active"] = False
                else:
                    top["active"] = eval_cond(cond)
                    top["any_taken"] = top["active"]
            result[i] = False
        elif re.match(r"\s*#else\b", line):
            if stack:
                top = stack[-1]
                top["active"] = not top["any_taken"]
                top["any_taken"] = True
            result[i] = False
        elif re.match(r"\s*#endif\b", line):
            if stack:
                stack.pop()
            result[i] = False
        else:
            result[i] = all(e["active"] for e in stack)
    return result


def collect_sources():
    seen = []
    for pattern in SHARED_GLOBS:
        for p in sorted(ROOT.glob(pattern)):
            if p not in seen:
                seen.append(p)
    return seen


def main() -> int:
    sources = collect_sources()
    if not sources:
        print("未找到共享区源文件，检查 SHARED_GLOBS 配置", file=sys.stderr)
        return 1

    calls = []
    decls = []
    total = 0
    for path in sources:
        rel = path.relative_to(ROOT)
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        active = active_map(lines)
        for i, line in enumerate(lines):
            stripped = line.strip()
            # 跳过注释
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            m = TOKEN.search(line)
            if not m:
                continue
            total += 1
            name = m.group(0).rstrip("( \t")
            if name in ALLOWED:
                continue
            if not active[i]:
                continue
            # 判断是「声明」还是「调用」
            is_decl = bool(DECL.match(line)) and stripped.endswith(";") \
                and stripped.count("(") == 1
            loc = f"{rel}:{i + 1}"
            if is_decl:
                decls.append((loc, stripped))
            else:
                calls.append((loc, stripped))

    if calls:
        print("Android 构建会引用 iOS 专属符号的调用（链接必然失败）：", file=sys.stderr)
        print("", file=sys.stderr)
        for loc, text in calls:
            print(f"  {loc}: {text}", file=sys.stderr)
        print("", file=sys.stderr)
        print("修法：用 #if LZT_PLATFORM_IOS 包住该调用。", file=sys.stderr)
        return 1

    if decls:
        print("提示：以下 lzt_ios_* 声明未加平台守卫（不影响链接，属卫生问题）：")
        for loc, text in decls:
            print(f"  {loc}: {text}")
        print("")

    print(f"共享区: no iOS-only symbols leak into Android build "
          f"({len(sources)} files, {total} refs checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
