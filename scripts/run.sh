#!/usr/bin/env bash
# Run the host program (defaults to build/allplay_host).
# Usage: scripts/run.sh --screen 1280x720 [--fullscreen] ...
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BIN="${ALLPLAY_HOST_BIN:-${ROOT}/build/allplay_host}"

if [ ! -x "$BIN" ]; then
    echo "Cannot find $BIN — run scripts/build.sh first" >&2
    exit 1
fi
exec "$BIN" "$@"
