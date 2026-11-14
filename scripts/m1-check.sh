#!/usr/bin/env bash
# ============================================================================
# m1-check.sh — M1 acceptance: RTSP + /info (bplist) + Bonjour advertising
# ============================================================================
# Run on any machine with dependencies installed (container / target machine both work).
#
# ⚠️ Why not use curl to check /info: AirPlay's response status line is `RTSP/1.0 200 OK`,
#    curl only understands HTTP/1.x and silently reports "Received HTTP/0.9 when not allowed" and drops
#    the response — the server is fine, the tool just does not accept it. Use a raw TCP client here (which also closely matches
#    the phone's actual behavior).
#
# Steps:
#   1) start core_selftest (--no-avahi) → 2) raw RTSP: GET /info + OPTIONS
#   3) per-field plist assertions → 4) if avahi is active and avahi-browse exists: also verify advertisement visibility
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BIN="${ALLPLAY_SELFTEST:-${ROOT}/build/core_selftest}"
PORT="${M1_PORT:-17000}"
STATE_DIR="${ALLPLAY_STATE_DIR:-${ROOT}/build/m1-state}"
LOG=/tmp/m1-check.log
FAIL=0

say()  { printf '%s\n' "$*"; }
fail() { printf '❌ %s\n' "$*"; FAIL=1; }
ok()   { printf '✅ %s\n' "$*"; }

[ -x "$BIN" ] || { say "Cannot find ${BIN} (run scripts/build.sh first)"; exit 2; }

cleanup() { if [ -n "${PID:-}" ]; then kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; fi; }
trap cleanup EXIT

say "== ① Start core_selftest (--no-avahi, port=${PORT}) =="
"$BIN" --port "$PORT" --no-avahi --state-dir "$STATE_DIR" >"$LOG" 2>&1 &
PID=$!
for i in $(seq 1 50); do
    grep -q "READY" "$LOG" && break
    sleep 0.2
done
grep -q "READY" "$LOG" || { say "---- Log ----"; cat "$LOG"; fail "core did not become ready"; exit 1; }
grep -E "^\[selftest\]" "$LOG" | sed 's/^/   /'
DEVID=$(grep -oE "deviceid=[0-9A-F:]+" "$LOG" | head -1 | cut -d= -f2)

say "== ②③ Raw RTSP: GET /info + OPTIONS, and validate bplist =="
if python3 - "$PORT" "$DEVID" <<'PY'
import plistlib, socket, sys

port, devid = int(sys.argv[1]), sys.argv[2]

def rtsp(method, uri):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    req = f"{method} {uri} RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: m1-check\r\n\r\n"
    s.sendall(req.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(65536)
        if not chunk: break
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode(errors="replace").split("\r\n")
    hdr = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            hdr[k.strip().lower()] = v.strip()
    clen = int(hdr.get("content-length", "0"))
    while len(body) < clen:
        chunk = s.recv(65536)
        if not chunk: break
        body += chunk
    s.close()
    return lines[0], hdr, body

failures = []
def chk(cond, msg):
    print(("   ✅ " if cond else "   ❌ ") + msg)
    if not cond: failures.append(msg)

# ---- GET /info ----
status, hdr, body = rtsp("GET", "/info")
chk(status.startswith("RTSP/1.0 200"), f"/info status line: {status}")
chk(hdr.get("content-type") == "application/x-apple-binary-plist",
    f"/info Content-Type: {hdr.get('content-type')}")
try:
    d = plistlib.loads(body)
except Exception as e:
    chk(False, f"plist parse failed: {e}")
    d = {}
chk(d.get("deviceID") == devid, f"deviceID == {devid} (actual {d.get('deviceID')})")
chk(d.get("features") == 0x615653AEE2, "features == 0x615653AEE2")
chk(len(d.get("pi", "")) == 36, "pi is a UUID")
chk(len(d.get("pk", "")) == 64, "pk is 32-byte hex")
disps = d.get("displays") or []
chk(len(disps) == 1 and disps[0].get("widthPixels") == 1280
    and disps[0].get("heightPixels") == 720, "displays[0] = 1280x720")
chk(d.get("srcvers") == "950.7.1" and d.get("protovers") == "1.1", "srcvers/protovers")

# ---- OPTIONS ----
status, hdr, _ = rtsp("OPTIONS", "/")
chk(status.startswith("RTSP/1.0 200"), f"OPTIONS status line: {status}")
chk("public" in hdr and "SETUP" in hdr.get("public", ""), "OPTIONS includes Public (with SETUP)")

sys.exit(1 if failures else 0)
PY
then ok "/info + OPTIONS all passed"; else fail "/info or OPTIONS validation failed"; fi

say "== ④ Bonjour (pick verification tool per platform) =="
if [ "$(uname -s)" = "Darwin" ]; then
    # macOS: dns_sd backend; browse with the system dns-sd. There is no timeout command, so background + kill.
    if command -v dns-sd >/dev/null 2>&1; then
        kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
        "$BIN" --port "$PORT" --state-dir "$STATE_DIR" >"$LOG" 2>&1 &
        PID=$!
        for i in $(seq 1 50); do grep -q "READY" "$LOG" && break; sleep 0.2; done
        grep -E "Bonjour" "$LOG" | sed 's/^/   /'
        dns-sd -B _airplay._tcp > /tmp/m1-dns-sd.log 2>&1 &
        DSPID=$!
        sleep 3
        kill "$DSPID" 2>/dev/null
        grep -qF 'AllPlay' /tmp/m1-dns-sd.log && ok "dns-sd can see the local _airplay._tcp" \
                                              || fail "dns-sd did not find the local service"
    else
        say "   (skipped: no dns-sd)"
    fi
elif command -v avahi-browse >/dev/null 2>&1 && systemctl is-active avahi-daemon >/dev/null 2>&1; then
    kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    "$BIN" --port "$PORT" --state-dir "$STATE_DIR" >"$LOG" 2>&1 &
    PID=$!
    for i in $(seq 1 50); do grep -q "READY" "$LOG" && break; sleep 0.2; done
    grep -E "Bonjour" "$LOG" | sed 's/^/   /'

    # Use "service instance visible" (the + line) as the criterion rather than relying on TXT parsing:
    # the office LAN has many AirPlay devices, -r resolution queues up, and with a short timeout the TXT may not have arrived.
    # Spaces in instance names are escaped as \032 by avahi-browse -p.
    FOUND=0; TXTOK=0
    for i in 1 2 3; do
        out="$(timeout 8 avahi-browse -r -t -p _airplay._tcp 2>/dev/null)"
        printf '%s' "$out" | grep -qF 'AllPlay' && FOUND=1
        printf '%s' "$out" | grep -qF "${DEVID}" && TXTOK=1
        [ "$FOUND" = 1 ] && break
    done
    [ "$FOUND" = 1 ] && ok "avahi-browse can see the local _airplay._tcp service" \
                     || fail "avahi-browse did not find the local service"
    [ "$TXTOK" = 1 ] && ok "TXT resolved deviceid=${DEVID}" \
                     || say "   (TXT resolution did not get its turn this time; service visibility is enough)"
else
    say "   (skipped: no avahi-browse, or avahi-daemon is not running)"
fi

cleanup; PID=""
echo
if [ "$FAIL" = 0 ]; then say "==== M1 acceptance: all passed ===="; else say "==== M1 acceptance: failures present ===="; tail -30 "$LOG"; fi
exit "$FAIL"
