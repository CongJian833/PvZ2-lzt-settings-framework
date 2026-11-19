from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "jni" / "lzt_settings_framework.cpp"
text = SOURCE.read_text(encoding="utf-8")
# v7.x 设置框架迁移后,ARM32 容器校验改名并迁至 settings_widgets.cpp
WIDGETS = ROOT / "jni" / "settings" / "settings_widgets.cpp"
widgets_text = WIDGETS.read_text(encoding="utf-8")


def require(fragment: str, reason: str) -> None:
    if fragment not in text:
        raise AssertionError(reason)


require("compute_left_align", "A3：缺少纯计算阶段")
require("commit_board_alignment", "A3：缺少校验后的提交阶段")
require("g_align_mutex", "A2：缺少 H1/DEFER 互斥锁")
require("arm32_defer_paused_risk", "A1：缺少 ARM32 DEFER 震屏风险保护")
if "validate_tab_container32" not in widgets_text:
    raise AssertionError("B4：缺少 ARM32 page+160 容器类型校验（现名 validate_tab_container32，settings_widgets.cpp）")
require("fix_direction_start32", "v44：缺少 ARM32 方向表起点修正")
require("selector=%ld ret=%ld", "v44：方向表日志仍未区分输入 selector 与返回值")
require("HIGH_VIEW_ALIGN_ASPECT = 1.69335f", "高视角阈值必须保持 1.69335")
require("LOW_VIEW_ALIGN_ASPECT = 2.16666f", "低视角阈值必须为 2.16666")
require("should_align_for_view", "缺少统一视角宽高比判定函数")
if text.count("should_align_for_view(") < 5:
    raise AssertionError("主对齐与三条附属路径必须统一调用判定函数")

print("v43 guards present")