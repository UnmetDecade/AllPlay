#!/usr/bin/env bash
# ============================================================================
# deploy.sh — sync the project to a target machine and (optionally) build / run M1 acceptance remotely
# ============================================================================
# Usage:
#   scripts/deploy.sh                  # sync to the Ubuntu VM by default (via the 2222 tunnel)
#   scripts/deploy.sh --build          # sync, then build remotely
#   scripts/deploy.sh --build --run-m1 # sync + build + remote M1 acceptance
#   scripts/deploy.sh --mac ...        # switch the target to Mac (via the 2223 tunnel)
#
# Environment variables (all overridable):
#   ALLPLAY_TARGET=ubuntu|mac  ALLPLAY_HOST=...  ALLPLAY_SSH_PORT=...  ALLPLAY_SSH_KEY=...
#   ALLPLAY_DEST=...
#
# ⚠️ identity/ is synced across (required to run on the target), but it **is a sensitive project file**:
#    do not copy it anywhere else or add it to any repository.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"

TARGET="${ALLPLAY_TARGET:-ubuntu}"
DO_BUILD=0
DO_M1=0
for a in "$@"; do
    case "$a" in
        --mac)      TARGET=mac ;;
        --ubuntu)   TARGET=ubuntu ;;
        --build)    DO_BUILD=1 ;;
        --run-m1)   DO_BUILD=1; DO_M1=1 ;;
        *) echo "Unknown argument: $a" >&2; exit 2 ;;
    esac
done

if [ "$TARGET" = "mac" ]; then
    : "${ALLPLAY_HOST:=hewei@127.0.0.1}"
    : "${ALLPLAY_SSH_PORT:=2223}"
    : "${ALLPLAY_DEST:=/Users/hewei/AllPlay}"
else
    : "${ALLPLAY_HOST:=blt@127.0.0.1}"
    : "${ALLPLAY_SSH_PORT:=2222}"
    : "${ALLPLAY_DEST:=/home/blt/AllPlay}"
fi
KEY="${ALLPLAY_SSH_KEY:-/root/.ssh/id_allplay}"
HOST="$ALLPLAY_HOST"; PORT="$ALLPLAY_SSH_PORT"; DEST="$ALLPLAY_DEST"
SSH_OPTS="-i ${KEY} -p ${PORT} -o StrictHostKeyChecking=no -o UserKnownHostsFile=/root/.ssh/known_hosts_remote"

echo "== Syncing $ROOT → ${HOST}:${DEST} (target=$TARGET) =="
ssh $SSH_OPTS "$HOST" "mkdir -p '${DEST}'"
rsync -az --delete -e "ssh ${SSH_OPTS}" \
      --exclude 'build/' --exclude '*.o' \
      "${ROOT}/" "${HOST}:${DEST}/"
echo "== Sync complete =="

if [ "$DO_BUILD" = 1 ]; then
    echo "== Remote build =="
    ssh $SSH_OPTS "$HOST" "bash '${DEST}/scripts/build.sh'" 2>&1 | tail -25
fi

if [ "$DO_M1" = 1 ]; then
    echo "== Run M1 acceptance remotely =="
    ssh $SSH_OPTS "$HOST" "bash '${DEST}/scripts/m1-check.sh'"
fi
