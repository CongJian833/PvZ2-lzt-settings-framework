#!/var/jb/bin/sh
# ============================================================
# func1.sh — 从底包生成可安装 ipa（带静态 gate 站点）
#
# 用法：
#   OUTNAME=xxx.ipa MANIFEST=/tmp/m_I.txt TNAME=build_X sh /tmp/func1.sh
#
# 流程：抽 arm64 -> 注入 dylib -> 打 gate 站点 -> 校验静态补丁
#       -> 重签 -> 合回 fat -> 打包
# ============================================================
set -e

export PATH=/var/jb/usr/bin:/var/jb/usr/lib/llvm-16/bin:$PATH

D=/var/mobile/Documents/SCodeProjects/PvZ2-lzt-settings-framework/iOS
AG="/var/mobile/Containers/Shared/AppGroup/B0708F7E-9D9F-4625-832A-F3BF84D5E346/File Provider Storage/PvZ2"
BASE="$AG/安装包/原版+Clean/PvZ 2_Clean"
OUTDIR="$AG/安装包"

OUTNAME=${OUTNAME:-PvZ2_test.ipa}
MANIFEST=${MANIFEST:-$D/backup/v43_banner/manifest.txt}
TNAME=${TNAME:-build_tmp}
W=/tmp/$TNAME

echo "== [0/6] 准备 =="
echo "   底包: $BASE"
echo "   manifest: $MANIFEST ($(wc -l < "$MANIFEST") 站点)"
echo "   输出: $OUTDIR/$OUTNAME"

[ -d "$BASE/Payload/PvZ2.app" ] || { echo "!! 底包不存在"; exit 1; }
[ -f "$MANIFEST" ] || { echo "!! manifest 不存在"; exit 1; }
[ -f "$D/build/liblztsettingsframework.dylib" ] || { echo "!! 未编译 dylib"; exit 1; }

# ------------------------------------------------------------
# 【硬性校验】静态模式一致性
#
# 打包会把 22 个 hook 入口改成 B gate（静态桩）。若 dylib 未以
# LZT_STATIC_ONLY 编译，运行期会再装一遍 inline hook，两套机制叠加：
#   · LiveContainer 把 dylib 加载到 140MB 外 → 超 B 的 ±32MB
#   · And64InlineHook 退化为 20 字节长跳
#   · tramp 备份的是「B gate」，执行后跳回 入口+4（长跳序列中部）
#   · BR X17 → replacement → 无限循环 → 卡在 EA 图标（实测 v50）
#
# 因此这里在打桩前就检查 dylib 是否含静态模式标志，不一致直接退出。
# ------------------------------------------------------------
DYLIB="$D/build/liblztsettingsframework.dylib"
if grep -qa "STATIC-ONLY" "$DYLIB"; then
    HAVE_STATIC=1
else
    HAVE_STATIC=0
fi

if [ -n "$LZT_RUNTIME_HOOK" ]; then
    # 显式要求运行时 hook：此时不应打静态桩
    if [ "$HAVE_STATIC" = "1" ]; then
        echo "!! dylib 是静态模式，但要求运行时 hook（LZT_RUNTIME_HOOK）"
        echo "   请用不带 LZT_STATIC_ONLY 的方式重编 dylib"
        exit 1
    fi
    echo "   模式: 运行时 inline hook（不打静态桩）"
    SKIP_GATE=1
else
    if [ "$HAVE_STATIC" != "1" ]; then
        echo "!! dylib 未以 LZT_STATIC_ONLY 编译"
        echo ""
        echo "   静态 gate 需要静态模式的 dylib 配合。当前 dylib 会走运行时"
        echo "   inline hook，与打包期写入的 B gate 叠加，在 LiveContainer 下"
        echo "   会死循环卡在 EA 图标（v50 实测）。"
        echo ""
        echo "   请这样重编："
        echo "       cd $D && LZT_STATIC_ONLY=1 sh build.sh"
        exit 1
    fi
    echo "   模式: 静态 gate（dylib 含 STATIC-ONLY 标志）"
    SKIP_GATE=0
fi

rm -rf "$W"; mkdir -p "$W"
cp -a "$BASE" "$W/pkg"
APP="$W/pkg/Payload/PvZ2.app"
BIN="$APP/PvZ2"

ARCHS=$(llvm-lipo-16 -info "$BIN" 2>/dev/null | sed -n 's/^Architectures in the fat file: .* are: //p')
echo "   架构: ${ARCHS:-arm64(single)}"

echo "== [1/6] 抽 arm64 切片 =="
WORK="$W/work.arm64"
if [ -n "$ARCHS" ] && [ "$(echo "$ARCHS" | wc -w)" -gt 1 ]; then
    # 只保留 arm64：iOS 17 设备全部 arm64，armv7 切片永远不会被加载，
    # 保留只会让 ipa 多 35MB。v42/v43 均为此形态（44,067,264 B）。
    llvm-lipo-16 -thin arm64 "$BIN" -output "$WORK"
    echo "   已抽出 arm64（丢弃 armv7，减小体积）"
else
    cp -f "$BIN" "$WORK"
    echo "   单架构，直接使用"
fi

echo "== [2/6] 注入 dylib =="
mkdir -p "$APP/Frameworks"
cp -f "$D/build/liblztsettingsframework.dylib" "$APP/Frameworks/liblztsettingsframework.dylib"
chmod 755 "$APP/Frameworks/liblztsettingsframework.dylib"
if llvm-otool-16 -arch arm64 -L "$WORK" 2>/dev/null | grep -q liblztsettingsframework.dylib; then
    echo "   已有加载项，跳过"
else
    "$D/build/ldcmd_add" "$WORK" "@executable_path/Frameworks/liblztsettingsframework.dylib"
    echo "   已添加 LC_LOAD_DYLIB"
fi

if [ "$SKIP_GATE" = "1" ]; then
    echo "== [3/6] 跳过静态 gate 站点（运行时 hook 模式）=="
else
    echo "== [3/6] 打静态 gate 站点 =="
    "$D/build/lztgate" "$WORK" "$MANIFEST" || { echo "!! lztgate 失败"; exit 1; }
fi

echo "== [4/6] 主程序静态补丁 =="
# 三处字节补丁必须写入（底包为原版，不含）。
# 十六进制串是指令字，写入时需按【小端】字节序：
#   52810017 → 17 00 81 52
# 曾因按字符串顺序直接写（52 81 00 17），指令反转，代码段损坏 → 启动闪退（实测）。
for spec in "1cb7db0:52810017:字形缓存2048" "1cb7e1c:52800081:页上限4" "1c34674:d2800022:60fps"; do
    off=${spec%%:*}; rest=${spec#*:}; want=${rest%%:*}; desc=${rest#*:}
    # 目标字节序（小端）：把 want 每 2 字符切分后倒序（busybox sh 不支持 ${var:off:len}）
    b1=$(echo "$want" | cut -c7-8)
    b2=$(echo "$want" | cut -c5-6)
    b3=$(echo "$want" | cut -c3-4)
    b4=$(echo "$want" | cut -c1-2)
    wantLE="$b1$b2$b3$b4"
    cur=$(dd if="$WORK" bs=1 skip=$((0x$off)) count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$cur" = "$wantLE" ]; then
        echo "   ok  $desc @0x$off 已是目标值"
        continue
    fi
    printf "%s" "" > "$W/patch.bin"
    for h in $b1 $b2 $b3 $b4; do
        printf "\\$(printf '%03o' $((0x$h)))" >> "$W/patch.bin"
    done
    dd if="$W/patch.bin" of="$WORK" bs=1 seek=$((0x$off)) conv=notrunc 2>/dev/null
    now=$(dd if="$WORK" bs=1 skip=$((0x$off)) count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$now" = "$wantLE" ]; then
        echo "   ok  $desc @0x$off : $cur -> $now"
    else
        echo "   !!  $desc @0x$off 写入失败 (现在=$now 期望=$wantLE)"
        exit 1
    fi
done

echo "== [4b/6] 组件补丁（戴夫动画 / 传送带计数）=="
# 这两处原本由运行时写代码页完成，但 LZT_STATIC_ONLY 禁止 mprotect，
# 因此改为打包期静态写入（与上面三处补丁同机制）。
DAVECONV="$D/tools/daveconv"
if [ -x "$DAVECONV" ]; then
    echo "   生成器: $DAVECONV"
    # 记录打补丁前的原始指令，便于核对
    for spec in "1a7440:戴夫入口" "31582c:传送带跳过分支" "31585c:传送带入口"; do
        o=${spec%%:*}; d=${spec#*:}
        echo "   before $d @0x$o = $(dd if="$WORK" bs=1 skip=$((0x$o)) count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    done
    "$DAVECONV" "$WORK" | while IFS= read -r line; do echo "   $line"; done
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "   !! 组件补丁失败 rc=$rc（原始字节不符或洞穴被占用）"
        exit 1
    fi
    echo "   --- 写入后核对 ---"
    for spec in "1a7440:戴夫入口" "31582c:传送带跳过" "31585c:传送带入口" "18bea1c:洞穴首条" "18bea44:字符串"; do
        o=${spec%%:*}; d=${spec#*:}
        echo "   after  $d @0x$o = $(dd if="$WORK" bs=1 skip=$((0x$o)) count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    done
else
    echo "   !! 未找到 $DAVECONV（需在设备上编译，见 README）"
    exit 1
fi

echo "== [4c/6] 对齐 Info.plist 最低系统版本 =="
# dylib 用 -mios-version-min=11.0 编译，Info.plist 却写 10.0 会造成
# 「看起来支持 iOS 10、实际 dylib 加载失败」的静默失效。这里统一成 11.0。
IPL="$APP/Info.plist"
if [ -f "$IPL" ]; then
    old_min=$(grep -A1 "<key>MinimumOSVersion</key>" "$IPL" 2>/dev/null | grep "<string>" | sed 's/.*<string>\(.*\)<\/string>.*/\1/')
    # 设备无 awk：用 sed 按行号区间替换（key 行之后的那行）
    ln=$(grep -n "<key>MinimumOSVersion</key>" "$IPL" | head -1 | cut -d: -f1)
    if [ -n "$ln" ]; then
        sed -i "$((ln+1))s|.*|\t<string>11.0</string>|" "$IPL"
    fi
    new_min=$(grep -A1 "<key>MinimumOSVersion</key>" "$IPL" 2>/dev/null | grep "<string>" | sed 's/.*<string>\(.*\)<\/string>.*/\1/')
    echo "   MinimumOSVersion: ${old_min:-?} -> $new_min"
else
    echo "   !! 未找到 Info.plist"
    exit 1
fi

echo "== [5/6] 重签 =="
# ⚠️ 必须用项目自带的 entitlements.plist（含 jb.pmap_cs.custom_trust 越狱信任标记）。
# 用「从底包 ldid -e 导出」的那份会少 4 条关键权限，签名 blob 数从 6 降到 5，
# 体积少 563392 字节，dyld 加载 dylib 失败 → 启动闪退（实测）。
ENT_FILE="$D/entitlements.plist"
[ -f "$ENT_FILE" ] || { echo "!! 缺少 entitlements: $ENT_FILE"; exit 1; }
ldid -Icom.popcap.ios.PvZ2 -S"$ENT_FILE" "$WORK"
ldid -S "$APP/Frameworks/liblztsettingsframework.dylib"
cp -f "$WORK" "$BIN"
chmod 755 "$BIN"
echo "   ok ($(stat -c %s "$BIN") B, arm64 only)"

echo "== [6/6] 打包 =="
rm -f "$OUTDIR/$OUTNAME"
( cd "$W/pkg" && zip -q -r -X "$OUTDIR/$OUTNAME" Payload iTunesMetadata.plist 2>/dev/null ) || \
( cd "$W/pkg" && zip -q -r -X "$OUTDIR/$OUTNAME" Payload )

ls -la "$OUTDIR/$OUTNAME"
sha256sum "$OUTDIR/$OUTNAME"
echo "完成：$OUTDIR/$OUTNAME"
