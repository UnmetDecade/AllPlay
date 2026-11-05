#!/bin/sh
# ============================================================================
# sync-harmony-identity.sh —— 把本地 identity/ 拷进鸿蒙工程的 rawfile
# ============================================================================
# 为什么需要这一步：鸿蒙应用只能读自己的沙箱和 HAP 里的 rawfile，读不到
# identity/ 这种任意路径；而 identity/ 又绝对不能进 git。所以做成"构建前
# 临时拷进去"：拷完的内容在 .gitignore 里（rawfile/identity/），
# 打出来的 HAP 在 harmony/entry/build/（同样被忽略）。
#
# 用法：
#   ./scripts/sync-harmony-identity.sh
#   然后正常构建（DevEco 或 hvigorw assembleHap）
#
# 注意：HAP 里就带着你的 MFi 私钥了 —— 这个包只装自己的测试机，别外传。
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/identity"
DST="$ROOT/harmony/entry/src/main/resources/rawfile/identity"

for f in identity.pk8 certificate.p7b; do
    if [ ! -f "$SRC/$f" ]; then
        echo "缺少 $SRC/$f（先跑 scripts/extract-identity.sh）" >&2
        exit 1
    fi
done

mkdir -p "$DST"
cp "$SRC/identity.pk8" "$DST/identity.pk8"
cp "$SRC/certificate.p7b" "$DST/certificate.p7b"
chmod 600 "$DST/identity.pk8"

echo "已拷到 $DST"
echo "（这些文件不会进 git；打完包记得这台机器的 HAP 里有私钥）"
