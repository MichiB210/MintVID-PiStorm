# MintVID for PiStorm – BerryVP / BerryCore (P35)

Hardware-accelerated playback for MintVID on a **PiStorm with Emu68** (Raspberry Pi 3A+ / BCM2837):

- **H.264 up to 1080p**, decoded by the VideoCore of the Raspberry Pi (MMAL/VCHIQ).
  - Output as a hard full-screen picture on the Pi's HDMI output (mrberry), or in a P96 window on the Workbench (VPberry).
- **AAC, AC3 and E-AC3 audio**, decoded on the second ARM core (CPU1) of the Pi.
  - Output as PCM over the Pi's HDMI audio.
- Or **sound through the Amiga** (`--paula`): AAC, MP3, MP2, AC3 and PCM,
  decoded on the 68k by MintVID's own decoders and played through Paula.

> [!IMPORTANT]
> **Sound over HDMI requires the patched Emu68 kernel
> `Emu68-pistorm-BerryCore`.** The unmodified Emu68 kernel has no BerryCore
> service on CPU1. The picture alone (`--no-audio`) and the Amiga sound
> (`--paula`) do not use CPU1 and should work on the normal kernel; this has
> not yet been confirmed on hardware.
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
| MintVID GUI | `../player/amiga/mrgui*.c` | C (68k) | changed since P35 (starts mrberry as "MintVID player") |
| mrberry | `mrberry/` | C (68k) | changed since P35 (controls, Amiga sound) |
| VPberry (VPr8 P48, window player) | `vpberry/` | 68k assembler (vasm) | byte-identical to P35 |
| berryvp.library | `berryvp_library/` | 68k assembler (vasm) | changed since P35 (N80, see below) |
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

**N80 (berryvp.library): video without HDMI audio.** The library shows
frames through the VideoCore scheduler, which waits for the Pi's
Audio-Master clock, and only `ConfigAudio` (HDMI audio, BerryCore kernel)
starts that clock. With `--no-audio` or `--paula` the first frame therefore
waited forever. Such streams now use the direct decoder-to-renderer tunnel,
timed by the library's own frame grid, the same path standalone C mode uses
for MP4 files. Streams with HDMI audio are unchanged. The trace shows
`N80_NO_HDMI_AUDIO_DIRECT_RENDERER` when it is taken. CI builds the library
with vasm 2.0f and puts it in the MintVID-AmigaOS artifact; copy it to
`LIBS:`.

## Playing and controls

The fullscreen mrberry launch now forwards the GUI's **No audio** checkbox
as `--no-audio` in both ReAction and GadTools. Video-only playback bypasses
BerryCore audio setup; previously the checkbox was ignored by these fork
launchers. This does not establish compatibility with every Pi/Emu68 version.

Startup output from GUI-launched mrberry is saved to
`T:MintVID-mrberry.log`, overwritten on the next launch. Both controllers
also display the player's saved terminal status after a quick exit. If no
status is published, the controller points to the log after an early exit
or roughly 60 seconds, without terminating the process.

In MintVID, choose the display **PiStorm / VC Hard Fullscreen** and press
Play. MintVID's **Pause** and **Stop** buttons work, like with the normal
player.

From the Shell:

    mrberry <film.mkv|film.mp4|film.ts> [--start=H:MM:SS] [--no-audio]
            [--paula [--audio-rate=low] [--audio-mono] [--av-offset=MS]]

`--paula` plays the sound through the Amiga instead of the Pi's HDMI.
The VideoCore still shows the picture. The 68k decodes the sound with
MintVID's decoders and keeps it in step with the Pi's timer.
`--audio-rate=low` and `--audio-mono` save CPU time, as in MintVID.
If picture and sound are not quite together, `--av-offset=100` plays
the sound 100 ms later (a negative number plays it earlier). E-AC3 is not
supported this way.

Keys while the film plays:

| Key | Action |
|---|---|
| ESC or 0 | stop |
| Space or P | pause / continue |
| Cursor right / left | jump 10 s forward / back |
| Cursor up / down | jump 60 s forward / back |

- Jumps and `--start` work with MKV and MP4. TS files always play from the
  start.
- A jump restarts the VideoCore stream at the nearest keyframe before the
  new time, so the picture goes away for a moment.
- If the audio track cannot be played (not AAC, AC3 or stereo E-AC3; with
  `--paula`: not AAC, MP3, MP2, AC3 or PCM), the film plays without sound.

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
- exakt gleich (bytegleich) zum Release: VPberry und der Emu68-BerryCore-Kern (berryvp.library hat die Änderung N80, siehe oben)
- MintVID und mrberry sind seit P35 geändert (Steuerung, Ton über den Amiga)
- Die drei CPU1-Module funktionieren gleich, ihr Maschinencode ist aber etwas anders, weil damals clang 17 statt clang 18 benutzt wurde. Die Neubauten wurden am 14.10.2026 auf der Hardware getestet: AAC, AC3 und E-AC3 laufen genau wie die P32-Originale.

**Wichtig:** Ton über HDMI braucht den gepatchten Emu68-Kern `Emu68-pistorm-BerryCore`. Zur Zeit gibt es ihn nur für den PiStorm Classic mit Raspberry Pi 3A+ und Emu68 1.0.7. Nur Bild (`--no-audio`) oder Ton über den Amiga (`--paula`) brauchen ihn nicht (auf der Hardware noch nicht bestätigt).

**Ton über den Amiga:** `--paula` spielt den Ton über Paula statt über HDMI. Der 68k dekodiert ihn mit den Decodern von MintVID (AAC, MP3, MP2, AC3, PCM, kein E-AC3). `--audio-rate=low` und `--audio-mono` sparen Rechenzeit. Passen Bild und Ton nicht ganz zusammen, verschiebt `--av-offset=100` den Ton um 100 ms nach hinten (negativ: nach vorne).

**Bedienung:** In MintVID die Anzeige **PiStorm / VC Hard Fullscreen** wählen.
Pause und Stopp in MintVID funktionieren. Tasten: ESC = Stopp, Leertaste =
Pause, Cursor rechts/links = 10 s vor/zurück, Cursor hoch/runter = 60 s.
Springen und `--start=H:MM:SS` gehen bei MKV und MP4.

**Installation:**
1. Die sieben Dateien `MintVID`, `mrberry`, `VPberry`, `berryvp.library` und die drei `.module`-Dateien in die MintVID-Schublade kopieren.
2. `Emu68-pistorm-BerryCore` als Emu68-Kern auf die SD-Karte legen.
3. In der `config.txt` auf der SD-Karte `gpu_mem=64` eintragen (die Zeile ergänzen, falls sie fehlt).
