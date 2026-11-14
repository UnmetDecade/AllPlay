#!/usr/bin/env bash
# ============================================================================
# bootstrap.sh — install build/runtime dependencies (works on the target Ubuntu machine & this dev container)
# ============================================================================
# Dependencies covered:
#   - Build: build-essential / cmake / pkg-config
#   - Core: libssl-dev (AES/SHA/ECDSA); libbluetooth-dev + bluez (RFCOMM/SDP for wireless triggering)
#   - Host: libsdl2-dev + libsdl2-ttf-dev (window/status bar); libavcodec/libavutil/libswscale-dev (H.264 decode + pixel format conversion)
#   - Reserved: libusb-1.0-0-dev (future wired CarPlay/USBMUX), libdbus-1-dev (alternative BlueZ D-Bus path)
# Notes:
#   · **No** fdk-aac — Apple-side audio uses AAC-LC/Opus/LPCM, always via ffmpeg's built-in decoders.
#   · **No** libswresample-dev — the host needs only one conversion ("planar float → interleaved s16") and implements it itself,
#     which also sidesteps its channel-layout parameter signature differences between ffmpeg 4.4 and 5.1+.
#   · libavformat-dev is unused by this project (no containers/demuxing) but harmless to keep.
set -euo pipefail

SUDO=""
if [ "$(id -u)" != 0 ]; then SUDO="sudo"; fi

echo "== apt update =="
$SUDO apt-get update -y

echo "== Installing dependencies =="
$SUDO apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libssl-dev libusb-1.0-0-dev libdbus-1-dev libbluetooth-dev bluez \
    libavahi-client-dev \
    libsdl2-dev libsdl2-ttf-dev \
    libavcodec-dev libavformat-dev libswscale-dev libavutil-dev \
    fonts-dejavu-core

# mDNS uses the system avahi (advertise _airplay._tcp, discover _carplay-ctrl._tcp):
# make sure avahi-daemon is enabled; if it is not running, the M1 Bonjour assertions fail.
if command -v systemctl >/dev/null 2>&1 && ! systemctl is-active avahi-daemon >/dev/null 2>&1; then
    echo
    echo "⚠️  avahi-daemon is not running. M1's Bonjour advertisement needs it (the core calls avahi over D-Bus):"
    echo "      sudo systemctl enable --now avahi-daemon"
fi

echo
echo "== Done. Next steps =="
echo "  1) Prepare the MFi identity: identity/identity.pk8 + identity/certificate.p7b (see README.md)"
echo "  2) bash scripts/build.sh         # build (the script has no exec bit, so invoke it with bash)"
echo "  3) bash scripts/run.sh --help"
