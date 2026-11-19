#!/var/jb/bin/sh
# ============================================================
# inject.sh — 把 liblztsettingsframework.dylib 注入 PvZ2.app（包内注入）
#
# 与 Android「把 so 打进 APK 的 lib/<abi>/」等价：
#   1. dylib 放入 PvZ2.app/Frameworks/
#   2. 主二进制 arm64 切片加一条 LC_LOAD_DYLIB（@executable_path 相对路径）
#   3. ldid 用原 entitlements 重签主二进制与 dylib
#   4. 重启游戏生效
#
# 用法：
#   sh inject.sh          注入/更新（首次会备份主二进制）
#   sh inject.sh restore  从备份还原（卸下 hook，回到原状）
# ============================================================
set -e

export PATH=/var/jb/usr/bin:/var/jb/usr/lib/llvm-16/bin:$PATH

IOS_ROOT=$(cd "$(dirname "$0")" && pwd)
BACKUP="$IOS_ROOT/backup"
DYLIB_SRC="$IOS_ROOT/build/liblztsettingsframework.dylib"
DYLIB_NAME=liblztsettingsframework.dylib
LOAD_PATH="@executable_path/Frameworks/$DYLIB_NAME"
ENT="$IOS_ROOT/entitlements.plist"

APP_ID=D59F5916-2310-4762-AF40-03DDA143EEB1
APP_IDENTIFIER=com.popcap.ios.PvZ2.cln   # 原二进制签名标识，重签必须沿用
APP="/var/containers/Bundle/Application/$APP_ID/PvZ2.app"
BIN="$APP/PvZ2"

[ -d "$APP" ] || { echo "!! 未找到 PvZ2.app: $APP"; exit 1; }

SUDO="sudo -S"
run_root() { printf '123456\n' | $SUDO sh -c "$1"; }

# ---------------- restore ----------------
if [ "$1" = "restore" ]; then
    [ -f "$BACKUP/PvZ2.orig" ] || { echo "!! 无备份，无法还原"; exit 1; }
    echo "[restore] 还原主二进制..."
    run_root "cp -a '$BACKUP/PvZ2.orig' '$BIN' && chown root:wheel '$BIN' && chmod 755 '$BIN'"
    run_root "rm -f '$APP/Frameworks/$DYLIB_NAME'"
    echo "[restore] 完成，重启游戏即恢复原状"
    exit 0
fi

# ---------------- backup ----------------
mkdir -p "$BACKUP"
if [ ! -f "$BACKUP/PvZ2.orig" ]; then
    echo "[1/5] 备份主二进制与 entitlements"
    run_root "cp -a '$BIN' '$BACKUP/PvZ2.orig'"
    run_root "chown mobile:wheel '$BACKUP/PvZ2.orig'"
    ldid -e "$BIN" > "$ENT"
    echo "      备份: $BACKUP/PvZ2.orig ($(ls -la "$BACKUP/PvZ2.orig" | tr -s ' ' | cut -d' ' -f5) 字节)"
    echo "      权限: $ENT"
else
    echo "[1/5] 备份已存在，跳过"
    [ -f "$ENT" ] || ldid -e "$BIN" > "$ENT"
fi

[ -f "$DYLIB_SRC" ] || { echo "!! 未找到 dylib，请先运行 build.sh"; exit 1; }

echo "[2/5] 安装 dylib 到 Frameworks/"
run_root "cp -f '$DYLIB_SRC' '$APP/Frameworks/$DYLIB_NAME'"
run_root "chown _installd:_installd '$APP/Frameworks/$DYLIB_NAME'; chmod 755 '$APP/Frameworks/$DYLIB_NAME'"

echo "[3/5] 给主二进制 arm64 切片添加 LC_LOAD_DYLIB"
if llvm-otool-16 -arch arm64 -L "$BIN" 2>/dev/null | grep -q "$DYLIB_NAME"; then
    echo "      已存在加载项，跳过"
else
    run_root "'$IOS_ROOT/build/ldcmd_add' '$BIN' '$LOAD_PATH'"
fi

echo "[4/5] ldid 重签（避免 fat 爆内存：拆切片单独签后回填）"
# 注意：本机 ldid 直接签 75MB fat 会被系统 SIGKILL（内存不足，实测 Killed:9）。
# 因此改为：拆出 armv7/arm64 两个单切片 → 只对改过的 arm64 用原标识重签
# → 用 lipo 重新合成 fat。armv7 未改动，原签名继续有效。
TMPD="$IOS_ROOT/build/inj_tmp"
mkdir -p "$TMPD"
run_root "cp -a '$BIN' '$TMPD/PvZ2.work'"
run_root "chown mobile:wheel '$TMPD/PvZ2.work'"
llvm-lipo-16 -thin armv7 "$TMPD/PvZ2.work" -output "$TMPD/s.armv7"
llvm-lipo-16 -thin arm64 "$TMPD/PvZ2.work" -output "$TMPD/s.arm64"
ldid -I"$APP_IDENTIFIER" -S"$ENT" "$TMPD/s.arm64"
llvm-lipo-16 -create -arch armv7 "$TMPD/s.armv7" -arch arm64 "$TMPD/s.arm64" -output "$TMPD/PvZ2.new"
run_root "cp -f '$TMPD/PvZ2.new' '$BIN'"
run_root "chown _installd:_installd '$BIN'; chmod 755 '$BIN'"
run_root "ldid -S '$APP/Frameworks/$DYLIB_NAME'"

echo "[5/5] 校验"
llvm-otool-16 -arch arm64 -L "$BIN" | grep "$DYLIB_NAME" || { echo "!! 加载项未写入"; exit 1; }
llvm-lipo-16 -thin arm64 "$BIN" -output "$TMPD/verify.arm64"
ldid -h "$TMPD/verify.arm64" | grep -q "$APP_IDENTIFIER" || { echo "!! 签名标识不符"; exit 1; }
echo "完成。重启 PvZ2 后生效。"
echo "日志位置：<PvZ2 数据容器>/Documents/lztsettingsframework.log"
