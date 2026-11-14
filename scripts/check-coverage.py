#!/usr/bin/env python3
"""Fail when core coverage drops below the agreed floors.

Reads the JSON report produced by gcovr in the CI `core` job:

    gcovr --root . --filter 'core/' --json coverage.json

A single global number is easy to game with a large, lightly tested module, so
the crypto and wire-format files that every projection backend depends on get
their own stricter floors. Update the numbers when coverage genuinely improves;
do not lower them to make a red build green.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

GLOBAL_MIN = 25.0

# Per-file floors for modules the protocol path cannot work without. Values are
# a little below the measured coverage so a genuine improvement is not punished
# and a real regression is caught.
PER_FILE_MIN = {
    "core/crypto/ed25519.cpp": 95.0,
    "core/crypto/chacha20poly1305.cpp": 95.0,
    "core/crypto/srp6a.cpp": 94.0,
    "core/crypto/tlv8.cpp": 93.0,
    "core/crypto/x25519.cpp": 88.0,
    "core/crypto/hkdf.cpp": 85.0,
    "core/projection/androidauto/aap.cpp": 90.0,
    "core/projection/carplay/iap2/wire.cpp": 88.0,
    "core/projection/carplay/iap2/wireless_client.cpp": 72.0,
}


def percent(covered: int, total: int) -> float:
    return 100.0 * covered / total if total else 100.0


def line_stats(entry: dict) -> tuple[int, int]:
    """Return (covered, total) for gcovr JSON in either schema."""
    if "line_total" in entry:
        return int(entry.get("line_covered", 0)), int(entry["line_total"])
    lines = entry.get("lines", [])
    return sum(1 for line in lines if line.get("count", 0) > 0), len(lines)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: check-coverage.py coverage.json", file=sys.stderr)
        return 2
    report = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    files = {entry["file"].replace("\\", "/"): entry for entry in report.get("files", [])}

    failures: list[str] = []
    global_covered = 0
    global_total = 0
    for entry in report.get("files", []):
        covered, total = line_stats(entry)
        global_covered += covered
        global_total += total
    global_pct = percent(global_covered, global_total)
    print(
        f"core coverage: {global_pct:.1f}% "
        f"({global_covered}/{global_total}, floor {GLOBAL_MIN:.1f}%)"
    )
    if global_pct < GLOBAL_MIN:
        failures.append(f"global: {global_pct:.1f}% < {GLOBAL_MIN:.1f}%")

    for path, floor in PER_FILE_MIN.items():
        entry = files.get(path)
        if entry is None:
            failures.append(f"{path}: not present in the coverage report")
            continue
        covered, total = line_stats(entry)
        pct = percent(covered, total)
        state = "ok" if pct >= floor else "below floor"
        print(f"{path}: {pct:.1f}% (floor {floor:.1f}%) {state}")
        if pct < floor:
            failures.append(f"{path}: {pct:.1f}% < {floor:.1f}%")

    if failures:
        print("\ncoverage gate failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("coverage gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
