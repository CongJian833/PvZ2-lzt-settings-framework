from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(ROOT.name == "PvZ2-lzt-settings-framework",
        "项目目录必须为 PvZ2-lzt-settings-framework")
require((ROOT / "NOTICE").exists(), "缺少上游 MIT 来源说明 NOTICE")
license_text = (ROOT / "LICENSE").read_text(encoding="utf-8")
notice_text = (ROOT / "NOTICE").read_text(encoding="utf-8")
require(license_text.startswith("PvZ2-lzt-settings-framework License Scope"),
        "LICENSE 必须以前置适用范围声明开头")
require(license_text.count("PolyForm Noncommercial License 1.0.0") >= 3,
        "LICENSE 必须保留 PolyForm Noncommercial 1.0.0 原文并说明适用范围")
require("Covered Material" in license_text and "Excluded Material" in license_text,
        "LICENSE 必须明确受许可材料与排除材料")
require("object-code or compiled form" in license_text,
        "LICENSE 必须明确编译形式仍在许可范围内")
require("PvZ2-MaxZoomHook" in notice_text,
        "NOTICE 必须保留上游项目来源")
require("And64InlineHook" in notice_text and "MIT License" in notice_text,
        "NOTICE 必须保留第三方 And64InlineHook MIT 声明")
require("libPVZ2.so" in notice_text and "not included as licensed material" in notice_text,
        "NOTICE 必须明确排除游戏及第三方材料")
require("lzt-settings-framework" in notice_text,
        "NOTICE 必须标明当前项目名与历史名对应关系")

mk = (ROOT / "jni" / "Android.mk").read_text(encoding="utf-8")
require("LOCAL_MODULE := settingsframework" in mk,
        "NDK 模块必须命名为 settingsframework（Java 侧加载名，设备实证）")

source = (ROOT / "jni" / "lzt_settings_framework.cpp").read_text(encoding="utf-8")
# 源码不再自带品牌标签：日志/初始化委托 lzt_core 单例，
# 日志文件名由 lzt_core.cpp 定义为 settingsframework.log
require('#include "lzt_settings_framework_config.h"' in source, "主实现未引用唯一配置头")
require("lzt_core::log_init()" in source, "日志初始化必须委托 lzt_core")
_core = (ROOT / "jni" / "lzt_core.cpp").read_text(encoding="utf-8")
require('kLogFileName = "settingsframework.log"' in _core,
        "日志文件必须为 settingsframework.log")

repack = ROOT / "tools" / "repack.py"
if repack.exists():  # 打包工具仅本地使用，不随仓库分发；克隆仓库后不存在
    repack_text = repack.read_text(encoding="utf-8")
    require('LOAD_LIBRARY_NAME = "settingsframework"' in repack_text,
            "APK 打包流程必须注入 loadLibrary(\"settingsframework\")")
    require("libsettingsframework.so" in repack_text,
            "APK 打包流程必须使用 libsettingsframework.so")
    require("src_apk" in repack_text and "keystore" in repack_text,
            "打包工具路径必须参数化，禁止硬编码工作区路径")
    for bad in ("C:\\\\Users", "Downloads", "workspace2", "Piggy"):
        require(bad not in repack_text, f"打包工具不得包含工作区路径: {bad}")
else:
    print("tools/repack.py 不存在（仅本地开发工具，跳过打包约束检查）")

builder = (ROOT / "build_variants.py").read_text(encoding="utf-8")
require("ANDROID_NDK_HOME" in builder or "ANDROID_NDK_ROOT" in builder,
        "构建脚本必须通过环境变量或参数定位 NDK")
for bad in ("C:\\\\Users", "Downloads", "workspace2", "Piggy"):
    require(bad not in builder, f"构建脚本不得包含工作区路径: {bad}")

offsets = (ROOT / "jni" / "offsets.h").read_text(encoding="utf-8")
require("OFF_ReleaseTitle" in offsets, "offsets.h 必须集中所有地址常量")

print("lzt-settings-framework branding and licensing constraints present")