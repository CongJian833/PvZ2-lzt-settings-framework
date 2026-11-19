#!/var/jb/bin/sh
# ============================================================
# inject_pkg.sh — 把 dylib 注入【指定的已解压包体】并打包成可安装 ipa
#
# 用途：对外发布（用户指定包体目录，而非设备上的已安装包）
#   sh inject_pkg.sh         注入 + 打包 ipa
#
# 与 inject.sh 的差异：
#   · 目标包路径由 PKG 指定（Payload/PvZ2.app 结构）
#   · 结束时打包成 ipa（zip 形式，Payload 在根）
#   · 不做 ldid 之外的签名（对外安装需用户自己用证书重签）
# ============================================================
set -e

export PATH=/var/jb/usr/bin:/var/jb/usr/lib/llvm-16/bin:$PATH

IOS_ROOT=$(cd "$(dirname "$0")" && pwd)
DYLIB_SRC="$IOS_ROOT/build/liblztsettingsframework.dylib"
DYLIB_NAME=liblztsettingsframework.dylib
LOAD_PATH="@executable_path/Frameworks/$DYLIB_NAME"
ENT="$IOS_ROOT/entitlements.plist"

PKG="/var/mobile/Containers/Shared/AppGroup/B0708F7E-9D9F-4625-832A-F3BF84D5E346/File Provider Storage/PvZ2/PvZ 2 701 9.8.1"
APP="$PKG/Payload/PvZ2.app"
BIN="$APP/PvZ2"
OUT_IPA="$IOS_ROOT/build/PvZ2-9.8.1-lzt.ipa"
APP_IDENTIFIER=com.popcap.ios.PvZ2

[ -d "$APP" ] || { echo "!! 未找到包体: $APP"; exit 1; }
[ -f "$DYLIB_SRC" ] || { echo "!! 未找到 dylib，请先 build.sh"; exit 1; }

mkdir -p "$APP/Frameworks"
cp -f "$DYLIB_SRC" "$APP/Frameworks/$DYLIB_NAME"
chmod 755 "$APP/Frameworks/$DYLIB_NAME"
echo "[1/4] dylib 已放入 Frameworks/"

if llvm-otool-16 -arch arm64 -L "$BIN" 2>/dev/null | grep -q "$DYLIB_NAME"; then
    echo "[2/4] 主二进制已有加载项，跳过"
else
    "$IOS_ROOT/build/ldcmd_add" "$BIN" "$LOAD_PATH"
    echo "[2/4] 已添加 LC_LOAD_DYLIB"
fi

echo "[3/4] 重签（拆切片，避免 fat 爆内存）"
TMPD="$IOS_ROOT/build/inj_tmp"
mkdir -p "$TMPD"
cp -a "$BIN" "$TMPD/PvZ2.work"
llvm-lipo-16 -thin armv7 "$TMPD/PvZ2.work" -output "$TMPD/s.armv7" 2>/dev/null || true
llvm-lipo-16 -thin arm64 "$TMPD/PvZ2.work" -output "$TMPD/s.arm64"
# 用【目标包自身的】entitlements 与其原有标识重签（标识取自 Info.plist：com.popcap.ios.PvZ2）
ldid -e "$TMPD/PvZ2.work" > "$TMPD/pkg_ent.plist" 2>/dev/null || true
if [ -s "$TMPD/pkg_ent.plist" ]; then
    ldid -I"$APP_IDENTIFIER" -S"$TMPD/pkg_ent.plist" "$TMPD/s.arm64"
else
    ldid -I"$APP_IDENTIFIER" -S "$TMPD/s.arm64"
fi
if [ -s "$TMPD/s.armv7" ]; then
    llvm-lipo-16 -create -arch armv7 "$TMPD/s.armv7" -arch arm64 "$TMPD/s.arm64" -output "$TMPD/PvZ2.new"
else
    cp -f "$TMPD/s.arm64" "$TMPD/PvZ2.new"
fi
cp -f "$TMPD/PvZ2.new" "$BIN"
chmod 755 "$BIN"
ldid -S "$APP/Frameworks/$DYLIB_NAME"
echo "      ok"

echo "[4/4] 打包 ipa"
rm -f "$OUT_IPA"
( cd "$PKG" && zip -q -r -X "$OUT_IPA" Payload iTunesMetadata.plist 2>/dev/null ) || \
( cd "$PKG" && zip -q -r -X "$OUT_IPA" Payload )
ls -la "$OUT_IPA"
echo "完成：$OUT_IPA"
