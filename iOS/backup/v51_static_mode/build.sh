#!/var/jb/bin/sh
# ============================================================
# build.sh — iOS 侧编译脚本（方案甲：以 Android/jni 为地基）
#
# 产物：iOS/build/liblztsettingsframework.dylib（arm64）
#
# 分层与 Android 严格对应，平台差异全部由本脚本与 iOS/port 垫片承担：
#   · 共享源码：Android/jni/*（基础层接口、框架层、业务模块）——【零改动】
#   · 平台实现：iOS/src/*（基址、日志、内存检查、沙盒路径、屏幕、hook 门面）
#   · 头文件垫片：iOS/port/include（android/log.h、offsets.h → offsets_ios.h）
# ============================================================
set -e

export PATH=/var/jb/usr/bin:/var/jb/usr/lib/llvm-16/bin:$PATH
SDK=/var/jb/usr/share/SDKs/iPhoneOS.sdk

IOS_ROOT=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$IOS_ROOT/.." && pwd)
SHARED_JNI="$REPO_ROOT/jni"
OUT="$IOS_ROOT/build"

LIBNAME=lztsettingsframework
MIN_IOS=11.0

mkdir -p "$OUT"

CFLAGS="-isysroot $SDK -arch arm64 -mios-version-min=$MIN_IOS -fPIC -O2
        -std=c++17 -fexceptions -frtti
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable"
INCLUDES="-I$IOS_ROOT/include -I$IOS_ROOT/port/include -I$IOS_ROOT/src -I$SHARED_JNI"
[ -n "$LZT_STATIC_ONLY" ] && CFLAGS="$CFLAGS -DLZT_STATIC_ONLY"
[ -n "$LZT_BASELINE_CTOR_ONLY" ] && CFLAGS="$CFLAGS -DLZT_BASELINE_CTOR_ONLY"
[ -n "$LZT_EXTRA_CFLAGS" ] && CFLAGS="$CFLAGS $LZT_EXTRA_CFLAGS"

# 共享源码（与 Android 完全同一份）
SHARED_SRC="
  $SHARED_JNI/And64InlineHook.cpp
  $SHARED_JNI/settings/settings_framework.cpp
  $SHARED_JNI/settings/settings_widgets.cpp
  $SHARED_JNI/settings/localizer.cpp
  $SHARED_JNI/settings/table_loader.cpp
  $SHARED_JNI/settings/game_abi.cpp
  $SHARED_JNI/view_angle_module.cpp
  $SHARED_JNI/language_module.cpp
  $SHARED_JNI/language_types.cpp
  $SHARED_JNI/language_types_rton.cpp
  $SHARED_JNI/resource_file.cpp
  $IOS_ROOT/src/lzt_fishhook.cpp
"

# iOS 平台实现
IOS_SRC="
  $IOS_ROOT/src/lzt_core_ios.mm
  $IOS_ROOT/src/lzt_main_ios.cpp
  $IOS_ROOT/src/lzt_fileprobe.cpp
"

echo "[1/3] 编译共享源码（Android/jni，零改动）"
OBJS=""
for f in $SHARED_SRC; do
    o="$OUT/$(basename "$f").o"
    clang++ $CFLAGS $INCLUDES -c "$f" -o "$o"
    OBJS="$OBJS $o"
    echo "      ok $(basename "$f")"
done

echo "[2/3] 编译 iOS 平台层"
for f in $IOS_SRC; do
    o="$OUT/$(basename "$f").o"
    clang++ $CFLAGS $INCLUDES -c "$f" -o "$o"
    OBJS="$OBJS $o"
    echo "      ok $(basename "$f")"
done

echo "[3/3] 链接 dylib"
clang++ -isysroot "$SDK" -arch arm64 -mios-version-min=$MIN_IOS -dynamiclib \
    -install_name "@executable_path/Frameworks/lib$LIBNAME.dylib" \
    -framework Foundation -framework UIKit \
    -framework QuartzCore -framework Metal -framework OpenGLES \
    -o "$OUT/lib$LIBNAME.dylib" $OBJS

echo "[4/4] 签名（ldid ad-hoc；主程序重签由 inject.sh 处理）"
ldid -S "$OUT/lib$LIBNAME.dylib"

ls -la "$OUT/lib$LIBNAME.dylib"
echo "构建完成：$OUT/lib$LIBNAME.dylib"

# ------------------------------------------------------------
# 模式自检
#
# LZT_STATIC_ONLY 决定 dylib 是否走「打包期静态 gate」。
# 这是最容易漏的环境变量：漏了就静默退化成运行时 inline hook，
# 与已打桩的主程序叠加，在 LiveContainer 下死循环卡 EA 图标（v50 实测）。
# 因此每次构建都明确打印当前模式，避免打到包里才发现。
# ------------------------------------------------------------
if grep -qa "STATIC-ONLY" "$OUT/lib$LIBNAME.dylib"; then
    echo ""
    echo "   模式: 【静态】LZT_STATIC_ONLY 已启用"
    echo "         dylib 只写槽位数据，不改任何代码页。"
    echo "         打包请用: OUTNAME=xxx.ipa MANIFEST=... sh tools/pack.sh"
else
    echo ""
    echo "   模式: 【运行时】inline hook（未定义 LZT_STATIC_ONLY）"
    echo "         dylib 会在运行期 mprotect 改代码页装 trampoline。"
    echo "         ⚠️  此模式的 dylib 不能与打了静态 gate 的包混用！"
    echo "         若要静态模式:  LZT_STATIC_ONLY=1 sh build.sh"
    echo "         若确实要运行时 hook 打包:  LZT_RUNTIME_HOOK=1 sh tools/pack.sh"
fi
