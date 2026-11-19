#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PvZ2-lzt-settings-framework APK 重新打包工具（参数化版本）。

流程：
1. [可缓存] apktool 解包原 APK → 注入 loadLibrary("settingsframework") → 重编译 → 提取 dex
   - 缓存键 = 源 APK SHA1 + 注入版本；源未变时直接复用缓存 dex
2. 重打包：原 APK + 注入后 dex + libsettingsframework.so（双架构）+ resources.arsc 未压缩
3. zipalign（page-align .so）
4. apksigner v1+v2+v3 签名
5. 精简验证

用法示例：
  python tools/repack.py --src-apk 原版.apk \\
      --keystore 我的签名.jks --keystore-pass 123456 --key-alias mykey --key-pass 123456 \\
      --out 输出.apk

工具路径（java/apktool/zipalign/apksigner）优先取命令行参数，
其次取环境变量（JAVA_HOME / APKTOOL_JAR / BUILD_TOOLS_DIR），
最后尝试 PATH。keystore 密码也支持环境变量 KEYSTORE_PASS / KEY_PASS。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path


PROJECT = Path(__file__).resolve().parent.parent
LOAD_LIBRARY_NAME = "settingsframework"
DEX_CACHE_VERSION = "settingsframework-v1"
# 历史库名一并清理（LawnZoomTab / MaxZoomHook 为更早的项目名）
DEFAULT_SO_NAMES = ("libsettingsframework.so", "libLawnZoomTab.so", "libMaxZoomHook.so")
DEFLATE_LEVEL = 1
COPY_BUF = 1 << 20

_step_t0 = time.time()


def step(msg: str) -> None:
    global _step_t0
    now = time.time()
    print(f"\n=== {msg} === [{now - _step_t0:.1f}s]")
    _step_t0 = now


def resolve_tool(name: str, cli: str | None, env_keys: tuple[str, ...]) -> Path:
    """按 命令行参数 → 环境变量 → PATH 顺序定位外部工具。

    env 值可能是可执行文件本身，也可能是其所在目录（如 JAVA_HOME）；
    目录场景自动拼接平台对应的可执行文件名。"""
    if cli:
        return Path(cli)
    # Windows 下常见工具名映射（目录拼接时使用）
    exe_map = {"java": "java.exe", "zipalign": "zipalign.exe", "apksigner": "apksigner.bat"}
    exe = exe_map.get(name, name) if os.name == "nt" else name
    for key in env_keys:
        val = os.environ.get(key)
        if not val:
            continue
        candidate = Path(val)
        if candidate.is_file():
            return candidate
        if candidate.is_dir():
            candidate = candidate / exe
            if candidate.is_file():
                return candidate
    found = shutil.which(name)
    if found:
        return Path(found)
    raise SystemExit(f"找不到工具 {name}，请用 --{name} 参数或环境变量指定。")


def set_compress_level(ni) -> None:
    """兼容设置压缩级别（不同 CPython 版本 ZipInfo 属性名不同）。"""
    for attr in ("_compresslevel", "compresslevel"):
        try:
            setattr(ni, attr, DEFLATE_LEVEL)
        except AttributeError:
            pass


def run(cmd: str) -> int:
    # 签名密码只传给子进程，命令日志始终脱敏。
    safe_cmd = re.sub(r"(pass:)[^\s\"]+", r"\1***", cmd)
    print(f"$ {safe_cmd}")
    r = subprocess.run(
        cmd, shell=True, capture_output=True, text=True,
        encoding="utf-8", errors="replace")
    if r.stdout:
        print(r.stdout[-500:])
    if r.returncode != 0 and r.stderr:
        print(f"STDERR: {r.stderr[-500:]}")
    return r.returncode


# ---------------------------------------------------------------------------
# 输入校验
# ---------------------------------------------------------------------------

def validate_native_library(path: Path, elf_class: int, machine: int, abi: str) -> None:
    assert path.is_file(), f"找不到 {abi} 原生库: {path}"
    with open(path, "rb") as f:
        header = f.read(20)
    assert len(header) >= 20, f"{abi} 原生库 ELF 头不完整: {path}"
    assert header[:4] == b"\x7fELF", f"{abi} 原生库不是 ELF: {path}"
    assert header[4] == elf_class, f"{abi} ELF class 错误: {header[4]}"
    assert header[5] == 1, f"{abi} ELF endian 不是 little-endian: {header[5]}"
    actual_machine = struct.unpack_from("<H", header, 18)[0]
    assert actual_machine == machine, (
        f"{abi} ELF machine 错误: {actual_machine}, expected {machine}")
    print(f"  {abi}: file/ELF/ABI OK ({path.name})")


def validate_inputs(args) -> None:
    for path, label in [(args.src_apk, "源 APK"), (args.apktool, "apktool"),
                        (args.java, "Java"), (args.zipalign, "zipalign"),
                        (args.apksigner, "apksigner"), (args.keystore, "keystore")]:
        assert path.is_file(), f"找不到{label}: {path}"
    so_arm64 = args.libs_dir / "arm64-v8a" / "libsettingsframework.so"
    so_arm32 = args.libs_dir / "armeabi-v7a" / "libsettingsframework.so"
    validate_native_library(so_arm64, 2, 183, "arm64-v8a")
    validate_native_library(so_arm32, 1, 40, "armeabi-v7a")


def file_sha1(path: Path) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# 阶段 A：dex 缓存
# ---------------------------------------------------------------------------

def inject_load_library(decoded_dir: Path) -> None:
    smali = decoded_dir / "smali" / "androidx" / "multidex" / "MultiDexApplication.smali"
    assert smali.exists(), f"找不到 {smali}"
    content = smali.read_text(encoding="utf-8")

    old = """.method protected attachBaseContext(Landroid/content/Context;)V
    .locals 0

    .line 38
    invoke-super {p0, p1}, Landroid/app/Application;->attachBaseContext(Landroid/content/Context;)V

    .line 39
    invoke-static {p0}, Landroidx/multidex/MultiDex;->install(Landroid/content/Context;)V

    return-void
.end method"""

    new = """.method protected attachBaseContext(Landroid/content/Context;)V
    .locals 1

    .line 38
    invoke-super {p0, p1}, Landroid/app/Application;->attachBaseContext(Landroid/content/Context;)V

    .line 39
    invoke-static {p0}, Landroidx/multidex/MultiDex;->install(Landroid/content/Context;)V

    const-string v0, "settingsframework"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V

    return-void
.end method"""

    if old in content:
        content = content.replace(old, new)
        smali.write_text(content, encoding="utf-8")
        print("注入成功")
    elif LOAD_LIBRARY_NAME in content:
        print(f"已含 {LOAD_LIBRARY_NAME}，跳过注入")
    elif '"LawnZoomTab"' in content:
        content = content.replace('"LawnZoomTab"', f'"{LOAD_LIBRARY_NAME}"')
        smali.write_text(content, encoding="utf-8")
        print(f"已将旧 LawnZoomTab 加载指令迁移为 {LOAD_LIBRARY_NAME}")
    elif "MaxZoomHook" in content:
        content = content.replace('"MaxZoomHook"', f'"{LOAD_LIBRARY_NAME}"')
        smali.write_text(content, encoding="utf-8")
        print(f"已将旧 MaxZoomHook 加载指令迁移为 {LOAD_LIBRARY_NAME}")
    else:
        raise SystemExit("找不到 attachBaseContext 方法模板")


def build_dex_cache(args, cache_dir: Path) -> None:
    step("A1. apktool 解包原 APK")
    if args.work_dir.exists():
        shutil.rmtree(args.work_dir)
    decoded = args.work_dir / "decoded"
    rc = run(f'"{args.java}" -jar "{args.apktool}" d -f -o "{decoded}" "{args.src_apk}"')
    assert rc == 0, "apktool 解包失败"

    step("A2. 注入 loadLibrary")
    inject_load_library(decoded)

    step("A3. apktool 重新编译")
    rebuilt = args.work_dir / "rebuilt.apk"
    if rebuilt.exists():
        rebuilt.unlink()
    rc = run(f'"{args.java}" -jar "{args.apktool}" b "{decoded}" -o "{rebuilt}"')
    assert rc == 0, "apktool 编译失败"

    step("A4. 提取 dex 写入缓存")
    cache_dir.mkdir(parents=True, exist_ok=True)
    dex_names = []
    with zipfile.ZipFile(rebuilt, "r") as z:
        for name in z.namelist():
            if name.endswith(".dex") and "classes" in name:
                data = z.read(name)
                (cache_dir / name).write_bytes(data)
                dex_names.append(name)
                mark = "OK" if LOAD_LIBRARY_NAME.encode() in data else "--"
                print(f"  {name}: {mark} {LOAD_LIBRARY_NAME} ({len(data)} bytes)")
    assert any(LOAD_LIBRARY_NAME.encode() in (cache_dir / n).read_bytes()
               for n in dex_names), f"没有找到含 {LOAD_LIBRARY_NAME} 的 dex"
    (cache_dir / "manifest.json").write_text(json.dumps({
        "dex": sorted(dex_names),
        "cache_version": DEX_CACHE_VERSION,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"缓存已写入: {cache_dir}")


def load_dex_cache(cache_dir: Path) -> dict[str, Path]:
    manifest = cache_dir / "manifest.json"
    assert manifest.is_file(), f"缓存 manifest 缺失: {manifest}"
    info = json.loads(manifest.read_text(encoding="utf-8"))
    result = {}
    for name in info["dex"]:
        p = cache_dir / name
        assert p.is_file(), f"缓存 dex 缺失: {p}"
        result[name] = p
    return result


def verify_existing_injection(src_apk: Path) -> None:
    """确认基础 APK 已有 settingsframework 加载入口，才允许原样复用 DEX。"""
    with zipfile.ZipFile(src_apk, "r") as z:
        dex_names = [name for name in z.namelist() if name.endswith(".dex")]
        matched = [name for name in dex_names if LOAD_LIBRARY_NAME.encode() in z.read(name)]
    assert matched, (
        f"{src_apk} 的 DEX 中未找到 {LOAD_LIBRARY_NAME}，不能跳过 loadLibrary 注入")
    print(f"基础 APK 已含 {LOAD_LIBRARY_NAME}: {', '.join(matched)}")


# ---------------------------------------------------------------------------
# 阶段 B/C/D/E：重打包、zipalign、签名、验证
# ---------------------------------------------------------------------------

def repack(args, dex_files: dict[str, Path]) -> None:
    step("B. 重新打包 APK（流式转发）")
    replaced = {
        f"lib/{arch}/{so}"
        for arch in ("arm64-v8a", "armeabi-v7a")
        for so in DEFAULT_SO_NAMES
    }
    tmp = args.work_dir / "repack.apk"
    with zipfile.ZipFile(args.src_apk, "r") as zin:
        with zipfile.ZipFile(tmp, "w") as zout:
            for info in zin.infolist():
                if info.filename in replaced:
                    continue
                if info.filename.startswith("META-INF/") and (
                    info.filename.endswith((".SF", ".RSA", ".DSA", ".EC"))
                    or info.filename == "META-INF/MANIFEST.MF"
                ):
                    continue

                ni = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                ni.external_attr = info.external_attr

                if info.filename in dex_files:
                    ni.compress_type = zipfile.ZIP_DEFLATED
                    set_compress_level(ni)
                    data = dex_files[info.filename].read_bytes()
                    zout.writestr(ni, data)
                    print(f"  REPLACED: {info.filename} ({len(data)} bytes)")
                elif info.filename == "resources.arsc":
                    ni.compress_type = zipfile.ZIP_STORED
                    with zin.open(info) as src, zout.open(ni, "w") as dst:
                        shutil.copyfileobj(src, dst, COPY_BUF)
                    print(f"  STORED: {info.filename}")
                else:
                    ni.compress_type = zipfile.ZIP_DEFLATED
                    set_compress_level(ni)
                    with zin.open(info) as src, zout.open(ni, "w") as dst:
                        shutil.copyfileobj(src, dst, COPY_BUF)

            for arch, elf in (("arm64-v8a", None), ("armeabi-v7a", None)):
                arcname = f"lib/{arch}/lib{LOAD_LIBRARY_NAME}.so"
                data = (args.libs_dir / arch / f"lib{LOAD_LIBRARY_NAME}.so").read_bytes()
                ni = zipfile.ZipInfo(arcname)
                ni.compress_type = zipfile.ZIP_DEFLATED
                set_compress_level(ni)
                zout.writestr(ni, data)
                print(f"  ADDED: {arcname} ({len(data)} bytes)")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(tmp), str(args.out))
    print(f"打包完成: {args.out}")


def zipalign(args) -> None:
    step("C. zipalign")
    aligned = args.work_dir / "aligned.apk"
    rc = run(f'"{args.zipalign}" -f -p 4 "{args.out}" "{aligned}"')
    assert rc == 0, "zipalign 失败"
    shutil.move(str(aligned), str(args.out))


def sign(args) -> None:
    step("D. apksigner v1+v2+v3")
    signed_tmp = args.work_dir / "signed.apk"
    if signed_tmp.exists():
        signed_tmp.unlink()
    rc = run(f'"{args.apksigner}" sign --ks "{args.keystore}" '
             f'--ks-pass pass:{args.keystore_pass} '
             f'--ks-key-alias {args.key_alias} --key-pass pass:{args.key_pass} '
             f'--v1-signing-enabled true --v2-signing-enabled true '
             f'--v3-signing-enabled true '
             f'--in "{args.out}" --out "{signed_tmp}"')
    assert rc == 0, "签名失败"
    try:
        if args.out.exists():
            args.out.unlink()
        shutil.move(str(signed_tmp), str(args.out))
    except (PermissionError, OSError) as e:
        print(f"  WARNING: 无法替换 {args.out.name}: {e}")
        print(f"  签名后的 APK: {signed_tmp}")
    print("签名完成")


def verify(args, full_verify: bool) -> None:
    step("E. 最终验证")
    ELF_MAGIC = b"\x7fELF"
    with zipfile.ZipFile(args.out, "r") as z:
        for arch in ("arm64-v8a", "armeabi-v7a"):
            n = f"lib/{arch}/lib{LOAD_LIBRARY_NAME}.so"
            info = z.getinfo(n)
            with z.open(n) as f:
                is_elf = f.read(4) == ELF_MAGIC
            print(f"  {n}: {info.file_size} bytes, ELF={is_elf}")
        for d in ("classes.dex", "classes2.dex", "classes3.dex", "classes4.dex"):
            data = z.read(d)
            has = LOAD_LIBRARY_NAME.encode() in data
            print(f"  {d}: {LOAD_LIBRARY_NAME}={'YES' if has else 'no'}")
        arsc = z.getinfo("resources.arsc")
        print(f"  resources.arsc: compress={arsc.compress_type} (0=STORED)")

    vflag = "-v " if full_verify else ""
    rc = run(f'"{args.zipalign}" -c {vflag}4 "{args.out}"')
    print(f"  zipalign: {'OK' if rc == 0 else 'FAIL'}")
    rc = run(f'"{args.apksigner}" verify {vflag}"{args.out}"')
    print(f"  签名验证: {'OK' if rc == 0 else 'FAIL'}")
    print(f"\n最终 APK: {args.out} ({args.out.stat().st_size / 1024 / 1024:.2f} MB)")


def main() -> None:
    global _step_t0
    _step_t0 = time.time()
    parser = argparse.ArgumentParser(description="PvZ2-lzt-settings-framework APK 重新打包工具")
    parser.add_argument("--src-apk", required=True, type=Path, help="原版 APK 路径")
    parser.add_argument("--out", type=Path, default=Path("out/lzt-settings-framework.apk"), help="输出 APK")
    parser.add_argument("--libs-dir", type=Path, default=PROJECT / "libs", help="编译产物目录")
    parser.add_argument("--java", type=Path, help="java 可执行文件")
    parser.add_argument("--apktool", type=Path, help="apktool.jar 路径")
    parser.add_argument("--zipalign", type=Path, help="zipalign 路径")
    parser.add_argument("--apksigner", type=Path, help="apksigner 路径")
    parser.add_argument("--keystore", type=Path, help="签名 keystore 路径")
    parser.add_argument("--keystore-pass", default=os.environ.get("KEYSTORE_PASS", ""))
    parser.add_argument("--key-alias", default=os.environ.get("KEY_ALIAS", ""))
    parser.add_argument("--key-pass", default=os.environ.get("KEY_PASS", ""))
    parser.add_argument("--work-dir", type=Path, default=Path("out/work"), help="临时工作目录")
    parser.add_argument("--cache-dir", type=Path, default=Path("out/dex_cache"), help="dex 缓存目录")
    parser.add_argument("--force", action="store_true", help="强制重建 dex 缓存")
    parser.add_argument("--reuse-existing-dex", action="store_true",
                        help="基础 APK 已有 settingsframework 注入时原样保留 DEX，跳过 apktool")
    parser.add_argument("--full-verify", action="store_true", help="详细验证输出")
    args = parser.parse_args()

    args.java = resolve_tool("java", str(args.java) if args.java else None,
                             ("JAVA_HOME",)) or Path("java")
    args.apktool = resolve_tool("apktool", str(args.apktool) if args.apktool else None,
                                ("APKTOOL_JAR",))
    args.zipalign = resolve_tool("zipalign", str(args.zipalign) if args.zipalign else None,
                                 ("BUILD_TOOLS_DIR",))
    args.apksigner = resolve_tool("apksigner", str(args.apksigner) if args.apksigner else None,
                                  ("BUILD_TOOLS_DIR",))
    if not args.keystore:
        raise SystemExit("必须提供 --keystore 签名文件（或环境变量 KEYSTORE）")

    step("0. 构建输入校验")
    validate_inputs(args)

    src_hash = file_sha1(args.src_apk)
    cache_dir = args.cache_dir / f"{src_hash}_{DEX_CACHE_VERSION}"
    print(f"源 APK SHA1: {src_hash}")

    if args.reuse_existing_dex:
        verify_existing_injection(args.src_apk)
        dex_files = {}
    else:
        if args.force or not (cache_dir / "manifest.json").is_file():
            if args.force:
                print("--force: 重建 dex 缓存")
            build_dex_cache(args, cache_dir)
        else:
            print("dex 缓存命中，跳过 apktool 阶段（如需重建请加 --force）")
        dex_files = load_dex_cache(cache_dir)
    print(f"dex 数量: {len(dex_files)}")

    t_start = time.time()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    repack(args, dex_files)
    zipalign(args)
    sign(args)
    verify(args, args.full_verify)
    print(f"\n总耗时: {time.time() - t_start:.1f}s")


if __name__ == "__main__":
    sys.exit(main())
