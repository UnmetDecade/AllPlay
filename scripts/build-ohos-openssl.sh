#!/bin/sh
# ============================================================================
# build-ohos-openssl.sh —— 给鸿蒙交叉编译一份 OpenSSL（只要 libcrypto）
# ============================================================================
# 为什么需要：core/mfi 的 MFi 签名要"对 32 字节摘要直接做 ECDSA"，而鸿蒙系统
# crypto 的 ECDSA 只有 "ECC|SHA256" 这种会再哈希一次的算法名，没有 NoHash
# （实测 OH_CryptoSign_Create("ECC|NoHash") 返回 17620003）。所以只能自带
# OpenSSL —— 这也是项目在桌面/Android 上一直用的那份依赖，不算新增。
#
# 产物：tmp/ohos-ssl/install-aarch64/{include,lib/libcrypto.a}
# （tmp/ 在 .gitignore 里，二进制不进库；换台机器重跑本脚本即可）
#
# 用法：
#   ./scripts/build-ohos-openssl.sh
set -e

VERSION=${OPENSSL_VERSION:-3.0.16}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${DEVECO_SDK_HOME:-/Applications/DevEco-Studio.app/Contents/sdk}
TOOLCHAIN="$SDK/default/openharmony/native/llvm/bin"
WORK="$ROOT/tmp/ohos-ssl"
PREFIX="$WORK/install-aarch64"

if [ ! -x "$TOOLCHAIN/aarch64-unknown-linux-ohos-clang" ]; then
    echo "找不到 OHOS 工具链：$TOOLCHAIN" >&2
    echo "设 DEVECO_SDK_HOME 指向 DevEco 的 sdk 目录" >&2
    exit 1
fi

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "openssl-$VERSION.tar.gz" ]; then
    echo "下载 openssl-$VERSION …"
    curl -L --fail --retry 3 -o "openssl-$VERSION.tar.gz" \
        "https://github.com/openssl/openssl/releases/download/openssl-$VERSION/openssl-$VERSION.tar.gz"
fi

if [ ! -d "openssl-$VERSION" ]; then
    tar xzf "openssl-$VERSION.tar.gz"
fi

cd "openssl-$VERSION"

# no-asm：跳过 perl 汇编，省掉"目标名/汇编文件对不上"的一整类问题
# no-shared：静态链进 liballplay_core.so，应用包里不额外带 .so
# 工具链里只有 aarch64-unknown-linux-ohos-clang 这一个带前缀的包装（编解码器），
# ar/ranlib 只有不带前缀的 llvm-*，所以显式指定，别指望 --cross-compile-prefix。
export PATH="$TOOLCHAIN:$PATH"
./Configure linux-aarch64 \
    CC=aarch64-unknown-linux-ohos-clang \
    AR=llvm-ar RANLIB=llvm-ranlib NM=llvm-nm \
    no-asm no-shared no-tests \
    --prefix="$PREFIX" \
    --libdir=lib

make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
make install_sw

echo
echo "完成：$PREFIX/lib/libcrypto.a"
echo "重新构建 HAP 时会自动用上（CMake 里检测到这个路径就编真 MFi）"
