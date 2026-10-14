# MintVID for PiStorm – BerryVP / BerryCore (P35)

Hardware-accelerated playback for MintVID on a **PiStorm with Emu68** (Raspberry Pi 3A+ / BCM2837):

- **H.264 up to 1080p**, decoded by the VideoCore of the Raspberry Pi (MMAL/VCHIQ).
  - Output as a hard full-screen picture on the Pi's HDMI output (mrberry), or in a P96 window on the Workbench (VPberry).
- **AAC, AC3 and E-AC3 audio**, decoded on the second ARM core (CPU1) of the Pi.
  - Output as PCM over the Pi's HDMI audio.

> [!IMPORTANT]
> **Requires the patched Emu68 kernel `Emu68-pistorm-BerryCore`.** Playback only
> works with it: the unmodified Emu68 kernel has no BerryCore service on CPU1,
> so nothing will play.
>
> **Currently supported: only PiStorm Classic with a Raspberry Pi 3A+ and
> Emu68 version 1.0.7.** The patch is made for exactly that kernel. Other Pi
> models, other PiStorm boards and newer Emu68 versions are not supported yet.
>
> **`config.txt` on the SD card must contain `gpu_mem=64`.** The VideoCore
> needs this memory for hardware decoding. Add the line if it is not there.

This directory holds the complete source of all PiStorm parts of the P35 release (P32 plus VPberry and the window mode). The MintVID GUI changes themselves live in `player/` (see the commit history of this fork).

> Deutsch: siehe [unten](#deutsch).

## How it fits together

```
MintVID (GUI, 68k) --starts--> mrberry (68k) --uses--> berryvp.library (68k)
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
| MintVID GUI | `../player/amiga/mrgui*.c` | C (68k) | byte-identical to P35 |
| mrberry | `mrberry/` | C (68k) | byte-identical to P32/P35 |
| VPberry (VPr8 P48, window player) | `vpberry/` | 68k assembler (vasm) | byte-identical to P35 |
| berryvp.library | `berryvp_library/` | 68k assembler (vasm) | byte-identical to P32/P35 |
| berry_ac3.module | `modules/ac3/` | C, liba52 (AArch64 BE) | same size, codegen differs, hardware-tested * |
| berry_faad03.module | `modules/aac_faad03/` | C, FAAD2 2.11.2 (AArch64 BE) | padded to P32 size, codegen differs, hardware-tested * |
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

Everything ends up in `pistorm/out/P35/`, and each file is compared against `P35_SHA256SUMS`. Every part also has its own `build.sh`.

## Installation (Amiga side)

1. Copy `MintVID`, `mrberry`, `VPberry`, `berryvp.library`, `berry_faad03.module`, `berry_ac3.module` and `berry_eac3.module` into the MintVID drawer.
2. **Required:** use `Emu68-pistorm-BerryCore` as the Emu68 kernel on the SD card, instead of `Emu68-pistorm`. It is only available for Emu68 1.0.7 on PiStorm Classic / Raspberry Pi 3A+.
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
  - Bild als Vollbild am HDMI-Ausgang des Pi (mrberry) oder im P96-Fenster auf der Workbench (VPberry)
- Ton in AAC, AC3 und E-AC3 auf dem zweiten ARM-Kern (CPU1)
  - Ausgabe über HDMI-Audio

**Bauen:** `sh pistorm/build_all.sh`. Benötigt werden m68k-amigaos-gcc, vasm, clang/LLVM 17 oder 18 und python3. Das Ergebnis liegt in `pistorm/out/P35/`.

**Ergebnis der Neubauten:**
- exakt gleich (bytegleich) zum Release: MintVID, VPberry, mrberry, berryvp.library und der Emu68-BerryCore-Kern
- Die drei CPU1-Module funktionieren gleich, ihr Maschinencode ist aber etwas anders, weil damals clang 17 statt clang 18 benutzt wurde. Die Neubauten wurden am 14.10.2026 auf der Hardware getestet: AAC, AC3 und E-AC3 laufen genau wie die P32-Originale.

**Wichtig:** Ohne den gepatchten Emu68-Kern `Emu68-pistorm-BerryCore` geht nichts. Zur Zeit gibt es ihn nur für den PiStorm Classic mit Raspberry Pi 3A+ und Emu68 1.0.7.

**Installation:**
1. Die sieben Dateien `MintVID`, `mrberry`, `VPberry`, `berryvp.library` und die drei `.module`-Dateien in die MintVID-Schublade kopieren.
2. `Emu68-pistorm-BerryCore` als Emu68-Kern auf die SD-Karte legen.
3. In der `config.txt` auf der SD-Karte `gpu_mem=64` eintragen (die Zeile ergänzen, falls sie fehlt).
