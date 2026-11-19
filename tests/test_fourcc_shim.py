"""校验 fourcc 接管点的 naked 汇编 shim 结构正确。

背景（2026-11-18 真机）：shim 里 GOT 取值少做一次解引用
    adrp x9, :got:oFourccToString
    ldr  x9, [x9, :got_lo12:oFourccToString]   ; x9 = 变量地址
    br   x9                                     ; 跳到数据段 → 立即 SIGSEGV
表现为日志打出 "[LANGX] ... (flag wins, #1)" 后游戏秒退。

另一次事故：null 分支里多写了一条 add sp,sp,#32，而 ldp 已带写出栈，
会破坏调用方栈帧。

本测试直接反汇编产物 .so，检查：
  1) 尾调用前对指针做了两次 ldr（取变量 → 取函数地址）
  2) br 之前有 cbz 保护
  3) null 分支只有 ret，不得再有 add sp
  4) 序言/结语栈调整量为 32 且一致

产物不存在时跳过（不影响纯源码校验）。
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent

SO = ROOT / "jni" / "libs" / "arm64-v8a" / "libsettingsframework.so"
OBJDUMP = (REPO / "Tools" / "android-ndk-r26b" / "toolchains" / "llvm" /
           "prebuilt" / "windows-x86_64" / "bin" / "llvm-objdump.exe")

SRC = ROOT / "jni" / "components" / "component_patches.cpp"


def source_checks() -> None:
    text = SRC.read_text(encoding="utf-8")
    if "hkFourccToString" not in text:
        raise AssertionError("缺少 hkFourccToString shim")
    # GOT 取值必须有两次 ldr
    if ":got_lo12:oFourccToString" not in text:
        raise AssertionError("shim 未使用 GOT 相对寻址")
    if text.count("ldr  x9, [x9]") < 1:
        raise AssertionError("GOT 取值后缺少第二次解引用（会 br 到数据段）")
    if "cbz  x9" not in text:
        raise AssertionError("尾调用前缺少空指针保护")


def disasm_checks() -> None:
    if not SO.exists() or not OBJDUMP.exists():
        print("fourcc shim: skipped (产物或 objdump 不存在)")
        return
    out = subprocess.run(
        [str(OBJDUMP), "-d", str(SO)],
        capture_output=True, text=True, errors="replace",
    ).stdout
    m = re.search(r"<hkFourccToString>:(.*?)(?:\n\s*\n|\Z)", out, re.S)
    if not m:
        raise AssertionError("反汇编中找不到 hkFourccToString")
    body = m.group(1)
    lines = [l.strip() for l in body.strip().splitlines() if l.strip()]

    # 1) 序言与结语各一次，且栈调整一致（32）
    if not any("stp\tx29, x30, [sp, #-0x20]!" in l for l in lines):
        raise AssertionError("序言应为 stp x29,x30,[sp,#-0x20]!")
    ldp = [l for l in lines if "ldp\tx29, x30, [sp], #0x20" in l]
    if len(ldp) != 1:
        raise AssertionError(f"结语 ldp 应恰好一次，实际 {len(ldp)}")

    # 2) br 之前必须连续两次 ldr x9
    br_idx = next((i for i, l in enumerate(lines) if re.search(r"\bbr\tx9", l)), -1)
    if br_idx < 0:
        raise AssertionError("缺少 br x9 尾调用")
    prev = lines[max(0, br_idx - 4):br_idx]
    ldr_count = sum(1 for l in prev if re.search(r"ldr\tx9,", l))
    if ldr_count < 2:
        raise AssertionError(
            f"br 之前应至少两次 ldr x9（取变量+取函数地址），实际 {ldr_count}"
        )
    if not any("cbz" in l for l in prev):
        raise AssertionError("br 之前缺少 cbz 空指针保护")

    # 3) null 分支不得再调整 sp（ldp 已带写）
    after_br = lines[br_idx + 1:]
    for l in after_br:
        if re.search(r"add\tsp, sp, #", l):
            raise AssertionError("null 分支多余 add sp（ldp 已出栈，会破坏调用方栈帧）")
        if re.search(r"\bret\b", l):
            break


source_checks()
disasm_checks()
print("fourcc shim structure verified")
