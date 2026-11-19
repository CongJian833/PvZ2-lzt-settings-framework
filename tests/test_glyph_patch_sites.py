"""校验组件补丁的指令补丁点与真实 libPVZ2.so 逐字节一致。

背景：v2.1.0 曾把 csel 补丁打到函数入口（0x1713384）而非 csel 本身
（0x1713408），运行时因字节不符被静默 skip，尺寸补丁从未生效。
此测试把「偏移 ↔ 原始指令字」的对应关系钉死，防止同类偏移错误复现。

libPVZ2.so 用 PT_LOAD 做 vaddr→file offset 映射后读取（vaddr != file offset）。
底包缺失时跳过（不影响纯源码校验）。
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent

OFFSETS = ROOT / "jni" / "offsets.h"
PATCHES = ROOT / "jni" / "components" / "component_patches.cpp"

CANDIDATE_SO = [
    REPO / "Tools" / "IDA" / "lib" / "arm64-v8a" / "libPVZ2.so",
    REPO / "out" / "work" / "decoded" / "lib" / "arm64-v8a" / "libPVZ2.so",
]


def parse_const(text: str, name: str) -> int:
    import re

    m = re.search(name + r"\s*=\s*(0x[0-9A-Fa-f]+)", text)
    if not m:
        raise AssertionError(f"未找到常量 {name}")
    return int(m.group(1), 16)


def load_segments(path: Path):
    data = path.read_bytes()
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        if struct.unpack_from("<I", data, o)[0] != 1:  # PT_LOAD
            continue
        off = struct.unpack_from("<Q", data, o + 0x08)[0]
        vaddr = struct.unpack_from("<Q", data, o + 0x10)[0]
        filesz = struct.unpack_from("<Q", data, o + 0x20)[0]
        segs.append((vaddr, vaddr + filesz, off))
    return data, segs


def read_word(data, segs, vaddr: int) -> int:
    for lo, hi, off in segs:
        if lo <= vaddr < hi:
            return struct.unpack_from("<I", data, off + (vaddr - lo))[0]
    raise AssertionError(f"0x{vaddr:X} 不在任何 PT_LOAD 段内")


offsets_text = OFFSETS.read_text(encoding="utf-8")
patches_text = PATCHES.read_text(encoding="utf-8")

# 1) 补丁点必须是绝对偏移，且与源码里的期望指令字对应
site = parse_const(offsets_text, "OFF_GLYPH_CACHE_SIZE_INSN")
expect = parse_const(patches_text, "kInsnCselW3")
patched = parse_const(patches_text, "kInsnMovW3_2048")

# MOV W3,#2048 编码自检：sf=0 opc=10 100101 hw=00 imm16=2048 Rd=3
imm16 = (patched >> 5) & 0xFFFF
rd = patched & 0x1F
if not (patched & 0xFFE00000 == 0x52800000 and imm16 == 2048 and rd == 3):
    raise AssertionError(f"kInsnMovW3_2048 编码不是 MOV W3,#2048：0x{patched:08X}")

# 2) 调用点不得再引用函数入口偏移
if "base + OFF_GLYPH_CACHE_SIZE_SEL" in patches_text:
    raise AssertionError("补丁点仍使用函数入口 OFF_GLYPH_CACHE_SIZE_SEL，应改用 OFF_GLYPH_CACHE_SIZE_INSN")

so_path = next((p for p in CANDIDATE_SO if p.exists()), None)
if so_path is None:
    print("glyph cache patch sites: skipped (libPVZ2.so not found)")
else:
    data, segs = load_segments(so_path)
    actual = read_word(data, segs, site)
    if actual != expect:
        raise AssertionError(
            f"补丁点 0x{site:X} 实际指令字 0x{actual:08X} != 期望 0x{expect:08X}"
            "（偏移错误会被运行时静默 skip）"
        )
    # 顺带确认函数入口处确实是另一条指令，避免再次把入口当补丁点
    entry = parse_const(offsets_text, "OFF_GLYPH_CACHE_SIZE_SEL")
    if entry != site:
        entry_word = read_word(data, segs, entry)
        if entry_word == expect:
            raise AssertionError("函数入口与 csel 指令字相同，偏移定义需复核")
    print(f"glyph cache patch site verified: 0x{site:X} = 0x{expect:08X} -> 0x{patched:08X}")
