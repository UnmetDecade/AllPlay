#!/bin/sh
# ============================================================================
# build-ohos-openssl.sh — cross-compile OpenSSL for HarmonyOS (libcrypto only)
# ============================================================================
# Why this is needed: core/mfi's MFi signing needs "ECDSA directly over a 32-byte digest", but the HarmonyOS system
# crypto ECDSA only offers algorithm names like "ECC|SHA256" that hash again; there is no NoHash
# (measured: OH_CryptoSign_Create("ECC|NoHash") returns 17620003). So we have to bundle
# OpenSSL — the same dependency the project already uses on desktop/Android, so not a new addition.
#
# Output: tmp/ohos-ssl/install-aarch64/{include,lib/libcrypto.a}
# (tmp/ is in .gitignore, so binaries never enter the repo; just rerun this script on another machine)
#
# Usage:
#   ./scripts/build-ohos-openssl.sh
set -e

VERSION=${OPENSSL_VERSION:-3.0.16}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${DEVECO_SDK_HOME:-/Applications/DevEco-Studio.app/Contents/sdk}
TOOLCHAIN="$SDK/default/openharmony/native/llvm/bin"
WORK="$ROOT/tmp/ohos-ssl"
PREFIX="$WORK/install-aarch64"

if [ ! -x "$TOOLCHAIN/aarch64-unknown-linux-ohos-clang" ]; then
    echo "OHOS toolchain not found: $TOOLCHAIN" >&2
    echo "Set DEVECO_SDK_HOME to point at DevEco's sdk directory" >&2
    exit 1
fi

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "openssl-$VERSION.tar.gz" ]; then
    echo "Downloading openssl-$VERSION …"
    curl -L --fail --retry 3 -o "openssl-$VERSION.tar.gz" \
        "https://github.com/openssl/openssl/releases/download/openssl-$VERSION/openssl-$VERSION.tar.gz"
fi

if [ ! -d "openssl-$VERSION" ]; then
    tar xzf "openssl-$VERSION.tar.gz"
fi

cd "openssl-$VERSION"

# no-asm: skip perl assembly, avoiding a whole class of "target name / assembly file mismatch" problems
# no-shared: link statically into liballplay_core.so so the app bundle carries no extra .so
# The toolchain has only one prefixed wrapper, aarch64-unknown-linux-ohos-clang (the codec),
# while ar/ranlib are unprefixed llvm-*; specify them explicitly and do not rely on --cross-compile-prefix.
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
echo "Done: $PREFIX/lib/libcrypto.a"
echo "The next HAP build will pick it up automatically (CMake builds real MFi when it sees this path)"
