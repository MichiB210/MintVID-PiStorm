# MintVID for PiStorm – BerryVP / BerryCore (v96.79)

Hardware-accelerated playback for MintVID on a **PiStorm with Emu68** (Raspberry Pi 3A+ / BCM2837):

- **H.264 up to 1080p**, decoded by the VideoCore of the Raspberry Pi (MMAL/VCHIQ).
  - Output as a hard full-screen picture on the Pi's HDMI output, or in a P96 window on the Workbench (both mrberry).
- **AAC, AC3 and E-AC3 audio**, decoded on the second ARM core (CPU1) of the Pi.
  - Output as PCM over the Pi's HDMI audio.
- **Paula sound** in full screen (mrberry `PAULA`, Paula player by Darren 'boingball' Banfi): AAC, MP3, MP2, AC3 and PCM decoded on the Amiga, synchronised to the Pi's video clock.
  - Pause with Paula: mrberry stops at the picture on screen and writes the spot to `film.t`; MintVID-pi shows it as a still picture (mrberry `W=50 STILL`: window without sound) and continues in full screen with Paula.

> [!IMPORTANT]
> **HDMI sound needs the patched Emu68 kernel `Emu68-pistorm-BerryCore`.** The
> unmodified Emu68 kernel has no BerryCore service on CPU1, so the AAC/AC3/E-AC3
> decoders for HDMI cannot run. Video with Paula sound (`PAULA`) also works with
> the original Emu68 1.0.7 kernel (tested on hardware, 23.10.2026).
>
> **Currently supported: only PiStorm Classic with a Raspberry Pi 3A+ and
> Emu68 version 1.0.7.** The patch is made for exactly that kernel. Other Pi
> models, other PiStorm boards and newer Emu68 versions are not supported yet.
>
> **`config.txt` on the SD card must contain `gpu_mem=64`.** The VideoCore
> needs this memory for hardware decoding. Add the line if it is not there.

This directory holds the complete source of all PiStorm parts of the v96.79 release. The MintVID-pi GUI changes themselves live in `player/` (see the commit history of this fork). VPberry (`vpberry/`) is kept as source only; since v96.76 the window mode is played by mrberry.

> Deutsch: siehe [unten](#deutsch).

**Picture path (berryvp.library N83):** in full screen a film of 1280x720 or
larger goes from the decoder straight to the display hardware (HVS), or through
the ISP if its coded width is not a multiple of 64. A film smaller than 1280x720
always goes through the ISP, which upscales it to fit 1920x1080; otherwise the
HVS draws green 1-pixel grid lines when it enlarges by an odd factor (tested:
clean at 1.0/1.13/1.5/3.0, lines at 1.33-2.4). mrberry's test switch `ISP`
sends every full-screen film through the ISP.

## How it fits together

```
MintVID-pi (GUI, 68k) --starts--> mrberry (68k) --uses--> berryvp.library (68k)
                                    |                       |
                          demux MKV/MP4/TS          VCHIQ/MMAL: H.264 -> VideoCore -> HDMI
                                                            |
                                         BerryCore mailbox (Fast-RAM)
                                                            v
                         Emu68 BerryCore kernel, CPU1 service (AArch64)
                           loads berry_faad03 / berry_ac3 / berry_eac3.module
```

| Part | Directory | Language | Rebuild |
|---|---|---|---|
| MintVID-pi GUI | `../player/amiga/mrgui*.c`, `mr_gui_menu.c` | C (68k) | byte-identical to v96.79 |
| mrberry | `mrberry/` | C (68k) | byte-identical to v96.79 |
| VPberry (VPr8 P48, old window player, not in the release since v96.76) | `vpberry/` | 68k assembler (vasm) | byte-identical to P35 |
| berryvp.library | `berryvp_library/` | 68k assembler (vasm) | byte-identical to v96.79 |
| berry_ac3.module | `modules/ac3/` | C, liba52 (AArch64 BE) | same size, codegen differs, hardware-tested * |
| berry_faad03.module | `modules/aac_faad03/` | C, FAAD2 2.11.2 (AArch64 BE) | byte-identical to v96.79 (5.1 mixed down to stereo) |
| berry_eac3.module | `modules/eac3/` | C, FFmpeg 9.0.2 (AArch64 BE) | same size, codegen differs, hardware-tested * |
| Emu68 BerryCore kernel | `emu68_berrycore/` | AArch64 assembler patch | byte-identical to P32 |

\* The historical CPU1 modules were built with clang 17. With clang 18 the machine code differs slightly. The rebuilt modules (clang 18) were tested on the hardware on 2026-10-14 (PiStorm Classic, Pi 3A+): AAC, AC3 and E-AC3 all play the same as the P32 originals.

## Building

Tools (in `PATH`):

- `m68k-amigaos-gcc` (bebbo toolchain, tested with 16.2): MintVID, mrberry
- `vasmm68k_mot` (vasm 2.0f): berryvp.library
- `clang`, `ld.lld`, `llvm-objcopy`, `llvm-nm`, `llvm-readelf` (LLVM 17 or 18): CPU1 modules, Emu68 kernel
- `python3`, `make`, `tar`, `patch`

```sh
sh pistorm/build_all.sh
```

Everything ends up in `pistorm/out/v96.79/`, and each file is compared against `v96.79_SHA256SUMS` (berry_ac3/berry_eac3 in the release are still the P35 binaries, see * above). Every part also has its own `build.sh`.

## Installation (Amiga side)

1. Copy `MintVID-pi`, `mrberry`, `berryvp.library`, `berry_faad03.module`, `berry_ac3.module` and `berry_eac3.module` into one drawer.
2. **For HDMI sound:** use `Emu68-pistorm-BerryCore` as the Emu68 kernel on the SD card, instead of `Emu68-pistorm`. It is only available for Emu68 1.0.7 on PiStorm Classic / Raspberry Pi 3A+. Paula sound works with the original kernel.
3. **Required:** in `config.txt` on the SD card, set `gpu_mem=64` (add the line if it is missing).

The modules must have the exact sizes that berryvp.library P32 expects. The build scripts check this.

## Where the sources come from

The PiStorm parts were developed by Michael with AI assistance (ChatGPT) between September and October 2026. For several stages only binaries were kept. Their source was reconstructed later, and every step was checked byte for byte against the binary of that time. See [`docs/SOURCE_RECONSTRUCTION.md`](docs/SOURCE_RECONSTRUCTION.md).

## Licences

See [`LICENSES.md`](LICENSES.md) and [`LICENSE`](LICENSE) (MIT, © Michael / MichiB210). In short:

- MintVID and the PiStorm 68k parts: MIT
- berry_ac3.module (liba52) and berry_faad03.module (FAAD2): GPL-2.0-or-later
- berry_eac3.module (FFmpeg): LGPL-2.1-or-later
- Emu68 BerryCore kernel: MPL-2.0

---

## Deutsch

Hardware-beschleunigte Wiedergabe für MintVID auf einem **PiStorm mit Emu68**:

- H.264 bis 1080p über die VideoCore des Raspberry Pi
  - Bild als Vollbild am HDMI-Ausgang des Pi oder im P96-Fenster auf der Workbench (beides mrberry)
- Ton in AAC, AC3 und E-AC3 auf dem zweiten ARM-Kern (CPU1)
  - Ausgabe über HDMI-Audio
- Im Vollbild Ton auch über Paula (Paula-Player von Darren 'boingball' Banfi)

**Bauen:** `sh pistorm/build_all.sh`. Benötigt werden m68k-amigaos-gcc, vasm, clang/LLVM 17 oder 18 und python3. Das Ergebnis liegt in `pistorm/out/v96.79/`.

**Ergebnis der Neubauten:**
- exakt gleich (bytegleich) zum Release v96.79: MintVID-pi, mrberry, berryvp.library, berry_faad03.module und der Emu68-BerryCore-Kern
- berry_ac3 und berry_eac3 (im Release weiterhin die P35-Dateien) funktionieren gleich, ihr Maschinencode ist aber etwas anders, weil damals clang 17 statt clang 18 benutzt wurde. Die Neubauten wurden am 14.10.2026 auf der Hardware getestet: AAC, AC3 und E-AC3 laufen genau wie die P32-Originale.

**Wichtig:** Ohne den gepatchten Emu68-Kern `Emu68-pistorm-BerryCore` gibt es keinen Ton über HDMI. Bild mit Ton über Paula läuft auch mit dem Original-Kern. Den gepatchten Kern gibt es zur Zeit nur für den PiStorm Classic mit Raspberry Pi 3A+ und Emu68 1.0.7.

**Installation:**
1. Die sechs Dateien `MintVID-pi`, `mrberry`, `berryvp.library` und die drei `.module`-Dateien in eine Schublade kopieren. VPberry wird nicht mehr gebraucht.
2. `Emu68-pistorm-BerryCore` als Emu68-Kern auf die SD-Karte legen.
3. In der `config.txt` auf der SD-Karte `gpu_mem=64` eintragen (die Zeile ergänzen, falls sie fehlt).
