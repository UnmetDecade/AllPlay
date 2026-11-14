#!/usr/bin/env bash
# Build. Usage: scripts/build.sh [extra cmake args...]
#   e.g.:  scripts/build.sh -DCMAKE_BUILD_TYPE=Debug
# Environment variables:
#   BUILD_DIR   build directory (default <repo root>/build)
#   TARGET      build only one target (e.g. allplay_host)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build}"

# macOS: brew installs to /opt/homebrew; non-interactive shells (scripts run over ssh) do not have its PATH /
# PKG_CONFIG_PATH. openssl@3 is keg-only, so OPENSSL_ROOT_DIR must be passed explicitly.
CMAKE_EXTRA=()
if [ "$(uname -s)" = "Darwin" ]; then
    BREW="$(command -v brew || echo /opt/homebrew/bin/brew)"
    if [ -x "$BREW" ]; then
        export PATH="$(dirname "$BREW"):$PATH"
        BREW_PREFIX="$("$BREW" --prefix)"
        export PKG_CONFIG_PATH="${BREW_PREFIX}/lib/pkgconfig:${BREW_PREFIX}/share/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
        CMAKE_EXTRA+=("-DOPENSSL_ROOT_DIR=$("$BREW" --prefix openssl@3)")
    fi
fi

JOBS="$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )"

# Note: macOS ships bash 3.2; expanding an empty array directly under set -u raises unbound variable,
# hence the ${arr[@]+...} form.
cmake -S "$ROOT" -B "$BUILD_DIR" ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"} "$@"
if [ -n "${TARGET:-}" ]; then
    cmake --build "$BUILD_DIR" --target "$TARGET" -- -j"$JOBS"
else
    cmake --build "$BUILD_DIR" -- -j"$JOBS"
fi
echo "== Output: $BUILD_DIR/ =="
