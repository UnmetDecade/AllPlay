#!/bin/sh
# ============================================================================
# sync-harmony-identity.sh — copy the local identity/ into the HarmonyOS project's rawfile
# ============================================================================
# Why this step is needed: a HarmonyOS app can only read its own sandbox and rawfile inside the HAP, not
# arbitrary paths like identity/; and identity/ must never enter git. So it is done as "copy in temporarily
# before the build": the copied content is in .gitignore (rawfile/identity/),
# and the produced HAP is in harmony/entry/build/ (also ignored).
#
# Usage:
#   ./scripts/sync-harmony-identity.sh
#   then build as usual (DevEco or hvigorw assembleHap)
#
# Note: the HAP now contains your MFi private key — install this build only on your own test devices and do not share it.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/identity"
DST="$ROOT/harmony/entry/src/main/resources/rawfile/identity"

for f in identity.pk8 certificate.p7b; do
    if [ ! -f "$SRC/$f" ]; then
        echo "Missing $SRC/$f (run scripts/extract-identity.sh first)" >&2
        exit 1
    fi
done

mkdir -p "$DST"
cp "$SRC/identity.pk8" "$DST/identity.pk8"
cp "$SRC/certificate.p7b" "$DST/certificate.p7b"
chmod 600 "$DST/identity.pk8"

echo "Copied to $DST"
echo "(these files never enter git; after packaging, remember this machine's HAP contains the private key)"
