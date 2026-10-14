#!/bin/sh
# Builds VPberry (VPr8 P48 with AC3), the stand-alone PiStorm VideoCore player
# that MintVID uses for "PiStorm / VC Hard Fenster" (window mode).
# Needs vasmm68k_mot (vasm 2.0f) in PATH. The binary keeps its symbol hunk,
# like the released one.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
vasmm68k_mot -m68030 -Fhunkexe -quiet -o "$OUT/VPberry" "$HERE/VPberry.s"
SUM=$(sha256sum "$OUT/VPberry" | cut -d' ' -f1)
echo "VPberry: $OUT/VPberry"
echo "SHA-256: $SUM"
if [ "$SUM" = "$(cat "$HERE/EXPECTED_SHA256")" ]; then echo "OK: identical to the released binary"
else echo "NOTE: differs from EXPECTED_SHA256"; fi
