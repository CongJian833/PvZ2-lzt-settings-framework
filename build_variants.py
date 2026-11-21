#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PvZ2-lzt-settings-framework 双版本构建工具。

只负责用 NDK 编译出正式版与 Debug 版的原生库，不打包 APK。
两种模式使用同一工程、同一功能代码；模式差异是
jni/lzt_settings_framework_config.h 中的编译期布尔开关 kDebugMode。

功能开关（模块/组件/其他可开关功能）统一声明在仓库根目录的
feature_flags.conf 中——该文件是纯数据、不含构建逻辑，可随仓库分发。
本脚本读取它并把每个开关映射到 lzt_settings_framework_config.h 的
同名编译期常量，然后编译；构建结束后源码开关恢复为配置文件声明的状态。

用法:
    python build_variants.py                  # 使用环境变量或 PATH 中的 ndk-build
    python build_variants.py --ndk <路径>     # 显式指定 NDK 目录
    python build_variants.py --no-clean       # 跳过 clean（增量编译）
    python build_variants.py --fast-channel   # 临时覆盖：本次纳入 CDN-json 快速读取通道

--fast-channel 只影响本次构建，不修改 feature_flags.conf；不加该参数时，
是否开启快速读取通道完全以配置文件的 fast_read_channel 为准。

NDK 定位顺序:
    1. --ndk 参数
    2. 环境变量 ANDROID_NDK_HOME / ANDROID_NDK_ROOT
    3. PATH 中的 ndk-build / ndk-build.cmd
"""

from __future__ import annotations

from pathlib import Path
import argparse
import os
import re
import shutil
import subprocess
import sys


PROJECT = Path(__file__).resolve().parent
CONFIG = PROJECT / "jni" / "lzt_settings_framework_config.h"
FLAGS_FILE = PROJECT / "feature_flags.conf"

# 功能开关配置项 → 编译期常量映射；键顺序即构建日志与应用的顺序。
# 新增可开关功能时：在 feature_flags.conf 增加一行，并在此登记对应常量。
# 注意：本表必须与**本工程**的 feature_flags.conf 和
# jni/lzt_settings_framework_config.h 严格对应。两者任缺一个开关，
# read_feature_flags() 都会直接报错退出——宁可报错，也不静默漏配。
FEATURE_SWITCHES: dict[str, str] = {
    "glyph_cache_size": "kEnableGlyphCacheSize2048",
    "glyph_cache_flush": "kEnableAlmanacGlyphFlush",
    "news_localization": "kEnableNewsLocalization",
    "fast_read_channel": "kEnableFastReadChannel",
    "empty_lang_page_fallback": "kEnableEmptyLangPageFallback",
}


def find_ndk_build(explicit: str | None) -> Path:
    """返回 ndk-build 可执行文件路径，找不到时抛出清晰错误。"""
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
        # 允许直接传 ndk-build 或 NDK 根目录
        if candidates[0].is_dir():
            candidates[0] = candidates[0] / ("ndk-build.cmd" if os.name == "nt" else "ndk-build")
    for env_name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "NDK_HOME"):
        env_val = os.environ.get(env_name)
        if env_val:
            root = Path(env_val)
            if root.is_dir():
                candidates.append(root / ("ndk-build.cmd" if os.name == "nt" else "ndk-build"))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    # 回退：PATH 中的 ndk-build
    found = shutil.which("ndk-build.cmd") or shutil.which("ndk-build")
    if found:
        return Path(found)
    raise SystemExit(
        "找不到 ndk-build。请通过 --ndk 参数或 ANDROID_NDK_HOME 环境变量指定 "
        "NDK 目录，或将 ndk-build 加入 PATH。"
    )


def read_feature_flags() -> dict[str, bool]:
    """解析 feature_flags.conf，返回 {开关名: 是否启用}。

    仅接受 ``<键> = true|false`` 形式的行；``#`` 之后为行尾注释，
    ``[分组]`` 标题行忽略。未知键或缺失键均报错，避免静默漏配。
    """
    if not FLAGS_FILE.is_file():
        raise SystemExit(f"找不到功能开关配置：{FLAGS_FILE}")
    flags: dict[str, bool] = {}
    for raw in FLAGS_FILE.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("["):
            continue
        key, sep, value = line.partition("=")
        if not sep:
            raise SystemExit(f"无法解析功能开关配置行：{raw!r}")
        key = key.strip()
        value = value.strip().lower()
        if value not in ("true", "false"):
            raise SystemExit(f"功能开关取值必须为 true/false：{raw!r}")
        flags[key] = value == "true"
    unknown = sorted(set(flags) - set(FEATURE_SWITCHES))
    if unknown:
        raise SystemExit(f"feature_flags.conf 含未登记的开关：{unknown}")
    missing = sorted(set(FEATURE_SWITCHES) - set(flags))
    if missing:
        raise SystemExit(f"feature_flags.conf 缺少开关：{missing}")
    return flags


def _write_config(text: str) -> None:
    """写回配置头。

    必须显式 newline="\n"：Path.write_text 在 Windows 上默认把 \n 翻译成
    \r\n，会让每次构建都把整个头文件的行尾改成 CRLF，产生整文件级的伪 diff。
    本工程的源码与配置文件均为 LF。"""
    with open(CONFIG, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def set_switch(macro: str, enabled: bool) -> None:
    """精确替换配置头中的单个布尔开关赋值。

    一般情况该常量只出现一次，直接替换。例外：kEnableFastReadChannel 写成
    #if defined(__APPLE__) / #else 的平台条件分支，出现两次。

    对一个“本次只为 Android 构建”的开关，正确语义是**只改 Android 分支**
    （即 #else 那一处，文本序最后出现的那处），Apple 分支保持不变。
    旧实现只允许「Android 分支恰好已是目标值」的幂等通过，一旦要把开关改回
    false 就会直接报错——那等于开关只能单向改，不可接受。"""
    text = CONFIG.read_text(encoding="utf-8")
    target = "true" if enabled else "false"
    pattern = rf"{re.escape(macro)}\s*=\s*(?:true|false)"
    matches = list(re.finditer(pattern, text))
    if not matches:
        raise RuntimeError(
            f"{macro} 在 {CONFIG.name} 中找不到赋值，无法应用功能开关；"
            f"请确认本工程的配置头与 FEATURE_SWITCHES 一致。")
    replacement = f"{macro} = {target}"
    if len(matches) == 1:
        _write_config(text[:matches[0].start()] + replacement + text[matches[0].end():])
        return
    # 多处赋值 = 平台条件分支：只改写最后一处（#else / Android 分支）。
    last = matches[-1]
    print(f"  {macro} 有 {len(matches)} 处平台条件赋值，仅改写 Android（#else）分支为 {target}")
    _write_config(text[:last.start()] + replacement + text[last.end():])


def apply_feature_flags(flags: dict[str, bool]) -> None:
    for key, macro in FEATURE_SWITCHES.items():
        set_switch(macro, flags[key])


def set_debug_mode(enabled: bool) -> None:
    text = CONFIG.read_text(encoding="utf-8")
    replacement = f"kDebugMode = {'true' if enabled else 'false'}"
    updated, count = re.subn(r"kDebugMode\s*=\s*(?:true|false)", replacement, text)
    if count != 1:
        raise RuntimeError(f"kDebugMode 应恰好出现一次赋值，实际为 {count}")
    _write_config(updated)


def print_flags(title: str, flags: dict[str, bool]) -> None:
    print(f"\n{title}")
    for key in FEATURE_SWITCHES:
        print(f"  {key} = {'true' if flags[key] else 'false'}")


def build_mode(ndk_build: Path, debug: bool, clean: bool) -> None:
    mode = "DEBUG" if debug else "RELEASE"
    print(f"\n=== 构建 {mode} 版 ===")
    set_debug_mode(debug)
    if clean:
        subprocess.run([str(ndk_build), "-C", str(PROJECT), "clean"], check=True)
    subprocess.run([str(ndk_build), "-C", str(PROJECT)], check=True)

    # 输出到独立目录，避免 Debug 与 Release 互相覆盖
    dist_dir = PROJECT / "dist" / mode.lower()
    shutil.rmtree(dist_dir, ignore_errors=True)
    libs = PROJECT / "libs"
    for so in sorted(libs.rglob("*.so")):
        rel = so.relative_to(libs)
        target = dist_dir / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(so, target)
        print(f"{mode} 产物: {target}")


def main() -> None:
    parser = argparse.ArgumentParser(description="PvZ2-lzt-settings-framework 双版本原生库构建")
    parser.add_argument("--ndk", help="NDK 根目录或 ndk-build 可执行文件路径")
    parser.add_argument("--no-clean", action="store_true", help="跳过 clean，增量编译")
    parser.add_argument("--fast-channel", action="store_true",
                        help="临时覆盖：本次构建纳入 CDN-json 快速读取通道（不改配置文件）")
    args = parser.parse_args()

    ndk_build = find_ndk_build(args.ndk)
    print(f"ndk-build: {ndk_build}")

    # 配置文件声明的开关（也是构建结束后的源码恢复基线）
    config_flags = read_feature_flags()
    build_flags = dict(config_flags)
    if args.fast_channel:
        build_flags["fast_read_channel"] = True
    print_flags(f"功能开关（来自 {FLAGS_FILE.name}）:", build_flags)

    try:
        # 确定性起点：先按配置写好所有功能开关，再编译两种模式。
        apply_feature_flags(build_flags)
        build_mode(ndk_build, debug=True, clean=not args.no_clean)
        build_mode(ndk_build, debug=False, clean=not args.no_clean)
    finally:
        # 无论构建是否成功，源码开关恢复为交付基线：kDebugMode 关闭 + 配置文件声明值。
        set_debug_mode(False)
        apply_feature_flags(config_flags)

    print("\n完成：正式版与 Debug 版均已编译；kDebugMode 已置 false，"
          f"各功能开关已恢复为 {FLAGS_FILE.name} 声明值。")


if __name__ == "__main__":
    sys.exit(main())