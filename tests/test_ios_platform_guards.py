"""校验 language_module.cpp 在 iOS 构建下不会引用 Android 专属符号。

背景（2026-11-19，iOS 侧反馈）：
Android 侧在启动线程里加了 `lzt_android_log_override_stats()`，但该函数
定义在 `jni/components/component_patches.cpp` —— 这个文件不在 iOS 的
SHARED_SRC 列表内（见 iOS/build.sh）。结果 iOS 编译失败：

    error: use of undeclared identifier 'lzt_android_log_override_stats'

Android 侧当轮只做了 Android 编译验证，build.sh 依赖 /var/jb/ 无法执行，
因此未能发现。本测试用「平台守卫静态求值」替代真实 iOS 编译。

原理：逐个 `lzt_android_*` 调用点，回溯其 #if 嵌套栈，
判断 iOS 构建（LZT_PLATFORM_IOS=1, __aarch64__=1）时该行是否参与编译。
参与编译即视为泄漏 —— 该符号在 iOS 上无定义。
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "jni" / "language_module.cpp"

# iOS 构建下这些宏的取值（见 iOS/include/offsets_ios.h 与 build.sh 的 -DLZT_STATIC_ONLY）
MACROS = {"LZT_PLATFORM_IOS": True, "__aarch64__": True}


def eval_cond(cond: str) -> bool:
    """对 #if 条件做最小求值，只覆盖本项目实际用法。"""
    c = cond.strip()
    c = re.sub(r"/\*.*?\*/", "", c).strip()
    # 处理 defined(X) / defined X
    def def_repl(m):
        name = m.group(1) or m.group(2)
        return "1" if name in MACROS else "0"
    c = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)", def_repl, c)
    # 未知标识符按 0 处理
    for name, val in MACROS.items():
        c = re.sub(r"\b" + re.escape(name) + r"\b", "1" if val else "0", c)
    c = re.sub(r"\b[A-Za-z_]\w*\b", "0", c)
    c = c.replace("&&", " and ").replace("||", " or ")
    c = re.sub(r"!(?!=)", " not ", c)
    try:
        return bool(eval(c, {"__builtins__": {}}, {}))
    except Exception:
        # 求值失败时保守认为「参与编译」，让泄漏更容易被发现
        return True


def ios_active(lines, upto: int) -> bool:
    """判断第 upto 行（0-based）在 iOS 构建时是否参与编译。"""
    stack = []  # [(cond, negated)]
    for i in range(upto + 1):
        line = lines[i]
        m = re.match(r"\s*#if\s+(.*)$", line)
        if m:
            stack.append([m.group(1), False, False])
            continue
        m = re.match(r"\s*#elif\s+(.*)$", line)
        if m:
            if stack:
                # #elif 只在前面所有分支都不成立时才求值。
                # 用「已成立」标记记录，避免把 iOS 为真时的 Android 分支误判为激活。
                cond, negated, taken = stack[-1]
                if negated or eval_cond(cond):
                    stack[-1] = [cond, True, True]   # 强制本分支不激活
                else:
                    stack[-1] = [m.group(1), False, False]
            continue
        if re.match(r"\s*#else\b", line):
            if stack:
                cond, negated, _taken = stack[-1]
                stack[-1] = [cond, not negated, True]
            continue
        if re.match(r"\s*#endif\b", line):
            if stack:
                stack.pop()
    for cond, negated, _taken in stack:
        ok = eval_cond(cond)
        if negated:
            ok = not ok
        if not ok:
            return False
    return True


lines = SRC.read_text(encoding="utf-8").splitlines()

leaks = []
for i, line in enumerate(lines):
    # 只看真正的调用；跳过声明、注释
    stripped = line.strip()
    if stripped.startswith("//") or stripped.startswith("*"):
        continue
    if re.match(r"extern\s", stripped):
        continue
    if not re.search(r"\blzt_android_\w+\s*\(", line):
        continue
    if ios_active(lines, i):
        leaks.append((i + 1, stripped))

if leaks:
    msg = ["iOS 构建会引用 Android 专属符号（该符号在 iOS 无定义）："]
    for n, text in leaks:
        msg.append(f"  language_module.cpp:{n}: {text}")
    msg.append("")
    msg.append("修法：用 #if !LZT_PLATFORM_IOS 包住该调用。")
    raise SystemExit("\n".join(msg))

print("language_module.cpp: no Android-only symbols leak into iOS build")
