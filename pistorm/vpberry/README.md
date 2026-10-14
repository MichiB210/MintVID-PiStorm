# VPberry – stand-alone PiStorm VideoCore player (VPr8 P48)

VPberry is VPr8 ("Video Player, revision 8"), a stand-alone Shell player for
PiStorm / Emu68. It is the same development line as berryvp.library:
H.264 on the Raspberry Pi VideoCore, audio on CPU1 through BerryCore.
MintVID starts it for the display mode **PiStorm / VC Hard Fenster** (window,
`W:25`); the full-screen mode uses mrberry + berryvp.library instead.

Audio: AAC (`berry_faad03.module`) and, since P48, AC3 (`berry_ac3.module`).
Both modules must be in the same drawer as VPberry (PROGDIR:).
E-AC3 is not supported by VPberry.

## Usage

    VPberry <film.mkv> [R|resume] [10|25|50|75|C|W:10|W:25|W:50|W:75|W:C] [T:MM|T:MM:SS|T:HH:MM:SS] [log|log:<file.txt>]

| Option | Meaning |
|---|---|
| `W:10`, `W:25`, `W:50`, `W:75` (or `10` … `75`) | window at 10/25/50/75 % size (default 25) |
| `W:C` (or `C`) | cinema mode: hard full screen on the Pi's HDMI |
| `T:1`, `T:01:10:00` | start at 1 minute, or at 1 h 10 min |
| `R` / `resume` | continue where you stopped: reads `<film>.t` (an explicit `T:` or `W:` wins) |
| `log`, `log:<file>` | write a log |

Keys during playback: `C`/`c` = cinema mode, `Space` = pause / resume,
`Esc`/`0`/`Ctrl-C` = stop. Stopping before the end writes `<film>.t` with the
position, for `R`.

A file name with spaces needs quotes:
`VPberry "USB:My Film.mkv" R`

## Building

    sh build.sh

`VPberry.s` is the P48 AC3 source. Compared with the original
`VPr8_P48_AC3_ORIGINAL.s`, only branch size suffixes were normalised to `.l`,
so that it assembles for 68030; the program logic is unchanged. The result is
byte-identical to the released VPberry (`EXPECTED_SHA256`).
