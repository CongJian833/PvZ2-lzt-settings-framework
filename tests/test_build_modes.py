from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "jni" / "lzt_settings_framework_config.h"
SOURCE = ROOT / "jni" / "lzt_settings_framework.cpp"
BUILDER = ROOT / "build_variants.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(CONFIG.exists(), "缺少唯一编译配置 lzt_settings_framework_config.h")
config = CONFIG.read_text(encoding="utf-8")
source = SOURCE.read_text(encoding="utf-8")

matches = re.findall(r"kDebugMode\s*=\s*(true|false)", config)
require(matches == ["false"], "kDebugMode 必须唯一且交付状态为 false")
require("#define LZT_DBG_LOG" in config, "缺少集中 Debug 日志宏")
require('#include "lzt_settings_framework_config.h"' in source, "主实现未引用唯一配置")
require("if constexpr (lzt_config::kDebugMode)" in source,
        "纯诊断 Hook/线程未受编译期开关控制")
require("static std::atomic<uintptr_t>& g_base = lzt_core::base_ref()" in source,
        "g_base 必须绑定 lzt_core 原子单例")
require("std::atomic<bool> g_view_hooks_enabled" in source,
        "g_view_hooks_enabled 必须原子化")
require("std::atomic<uint32_t> g_aspect_ratio_bits" in source,
        "宽高比必须通过原子位模式跨线程发布")
require("std::atomic<int> g_orig_b270" in source and
        "std::atomic<uintptr_t> g_orig_b270_board" in source,
        "恐龙补偿快照必须原子化")
require("if constexpr (lzt_config::kDebugMode) {\n        snapshot_note_shake_call" in source,
        "正式版必须裁剪 ARM32 ShakeBoard 快照入口")
require("std::mutex g_snapshot_mutex" in source,
        "Debug 快照共享状态必须受互斥锁保护")
require("uintptr_t g_snapshot_board" in source,
        "Debug 快照窗口必须绑定触发它的 Board")
require("if (g_snapshot_board != board)" in source,
        "Debug 快照必须在 Board 改变时终止旧窗口")
require("!std::isfinite(scale)" in source,
        "Debug 快照必须过滤 NaN/Inf scale")

require(BUILDER.exists(), "缺少正式版/Debug 版统一构建脚本")
builder = BUILDER.read_text(encoding="utf-8")
require("replacement = f\"kDebugMode = {'true' if enabled else 'false'}\"" in builder,
        "构建脚本必须只替换唯一布尔开关")
require("ANDROID_NDK_HOME" in builder and "ANDROID_NDK_ROOT" in builder,
        "构建脚本必须通过环境变量定位 NDK")
require("set_debug_mode(False)" in builder,
        "构建脚本必须在结束后恢复正式版开关")

# v47-DEBUG-SNAPSHOT-HARDENING 字面量标签已随重构消失,其保护实体
# (g_snapshot_mutex/g_snapshot_board/snapshot_note_shake_call)由上方
# 独立断言逐项锁定,此处不再重复要求字面量。
for fragment in (
    "fix_direction_start32",
    "board_layout_ready",
    "board_scale_ready",
    "g_align_mutex",
):
    require(fragment in source, f"关键功能约束缺失：{fragment}")
# ARM32 容器校验已随设置框架迁移改名(v7.x):现居 settings_widgets.cpp
_widgets = (ROOT / "jni" / "settings" / "settings_widgets.cpp").read_text(encoding="utf-8")
require("validate_tab_container32" in _widgets,
        "关键功能约束缺失：validate_tab_container32（settings_widgets.cpp）")

print("build mode and concurrency constraints present")