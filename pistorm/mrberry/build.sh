#!/bin/sh
# Builds mrberry (the BerryVP/PiStorm playback backend that MintVID starts).
# mrberry uses the MintVID core from ../../player/core plus the changed and
# new files in ./core (overlay), so the upstream mrplay is not affected.
# Needs m68k-amigaos-gcc (bebbo toolchain, tested with 16.2) in PATH.
# Result is byte-identical to mrberry of the P32 release with DEBUG_LOGS=1.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PLAYER="$HERE/../../player"
OUT=${1:-$HERE/out}
: "${CC:=m68k-amigaos-gcc}"
: "${DEBUG_LOGS:=1}"
rm -rf "$OUT/src"; mkdir -p "$OUT/src/core" "$OUT/src/amiga" "$OUT/src/iptv"
cp "$PLAYER"/core/* "$OUT/src/core/"
cp "$HERE"/core/* "$OUT/src/core/"
cp "$HERE"/amiga/* "$OUT/src/amiga/"
# MintVID's player status port (shared with mrplay and the GUIs).
cp "$PLAYER/amiga/mr_player_status.h" "$OUT/src/amiga/"
cp "$PLAYER/iptv/mr_iptv.h" "$OUT/src/iptv/"
cp -r "$PLAYER/amiga/include" "$OUT/src/amiga/include"
cp "$HERE/net_stubs.c" "$OUT/src/"
cd "$OUT/src"
$CC -O2 -std=c99 -m68040 -noixemul -fomit-frame-pointer -Wall -Wextra \
  -DAMIGA_M68K -DMR_YUV_NO_ASM -DMR_DEBUG_LOGS=${DEBUG_LOGS} -Iamiga/include \
  -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
  -o "$OUT/mrberry" \
  amiga/mrberry.c amiga/mr_berryvp.c amiga/mr_berryvp_stubs.S \
  core/mr_avc_annexb.c \
  core/mr_demux.c core/mr_mkv.c core/mr_mov.c core/mr_ts.c core/mr_ps.c \
  core/mr_avi.c core/mr_raw_mjpeg.c core/mr_raw_mpeg4.c \
  core/mr_source.c core/mr_latm.c core/mr_mpeg4.c core/mr_rawvideo.c core/mr_yuv.c \
  net_stubs.c \
  -noixemul
echo "mrberry: $OUT/mrberry"
