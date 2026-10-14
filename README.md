<h1>
  <img src="player/amiga/icons/MintVID.png" width="48" alt="MintVID icon">
  MintVID-PiStorm
</h1>

![AmigaOS](https://img.shields.io/badge/AmigaOS-3.x-orange)
![PiStorm](https://img.shields.io/badge/PiStorm-Emu68-red)
![Video](https://img.shields.io/badge/H.264-1080p%20real%20time-purple)
![Audio](https://img.shields.io/badge/Audio-AAC%20%7C%20AC3%20%7C%20E--AC3-green)

**Full HD H.264 playback on a classic Amiga with a PiStorm accelerator.**

The video is decoded by the Raspberry Pi's VideoCore hardware decoder. The
audio is decoded on the Pi's second ARM core. The Amiga only reads the file
and feeds both. This is how a PiStorm Amiga plays 1080p H.264 in real time
with synchronised sound, as a hard full-screen picture on the Pi's HDMI
output (a P96 window on the Workbench is planned).

This project is built on **[MintVID](https://github.com/boingball/MintVID)
by Darren "boingball" Banfi**, the modern 68k video player for AmigaOS, and
includes it in full (MIT licence). MintVID's own README is in
[`docs/MintVID-README.md`](docs/MintVID-README.md).

> [!IMPORTANT]
> **Requires the patched Emu68 kernel `Emu68-pistorm-BerryCore`.** Playback only
> works with it: the unmodified Emu68 kernel has no BerryCore service on CPU1,
> so nothing will play.
>
> **Currently supported: only PiStorm Classic with a Raspberry Pi 3A+ and
> Emu68 version 1.0.7.** The patch is made for exactly that kernel. Other Pi
> models, other PiStorm boards and newer Emu68 versions are not supported yet.

## What it plays

| | Format | Decoded by | Output |
|---|---|---|---|
| Video | **H.264 / AVC up to 1920x1080** | Raspberry Pi VideoCore (MMAL) | HDMI hard full screen ("PiStorm / VC Hard Fullscreen"), real time, A/V sync; P96 window planned |
| Audio | **AAC-LC** | FAAD2 on CPU1 of the Pi | stereo PCM over the Pi's HDMI |
| Audio | **AC3 (Dolby Digital)**, up to 5.1 | liba52 on CPU1 | stereo downmix over HDMI |
| Audio | **E-AC3 (Dolby Digital Plus)**, stereo | FFmpeg on CPU1 | stereo PCM over HDMI |
| Container | MKV, MP4/MOV, MPEG-TS | mrberry (68k) | |

Multichannel E-AC3 (5.1) is refused, because the CPU1 decoder is not fast
enough for it in real time yet. Tested on PiStorm Classic with a Raspberry Pi 3A+
and the patched Emu68 1.0.7 kernel.

## How to use it

From the Shell:

    mrberry movie.mkv

Or from the MintVID GUI: pick a film and choose the display mode **PiStorm / VC Hard Fullscreen**.
MintVID then starts mrberry for you. All other MintVID playback modes work
as in upstream MintVID.

## Installation

1. Copy `mrberry`, `berryvp.library`, `berry_faad03.module`, `berry_ac3.module` and `berry_eac3.module` into the same drawer.
   - Add `MintVID` too if you want the GUI.
2. **Required:** replace the Emu68 kernel on the PiStorm's SD card (`Emu68-pistorm`) with the patched `Emu68-pistorm-BerryCore`.
   - This is Emu68 1.0.7 for PiStorm Classic / Raspberry Pi 3A+, with the BerryCore service on CPU1.
   - Without it, nothing plays.
   - It only exists for this Emu68 version and this hardware.

Ready-made files are under **Releases**.

## How it fits together

```
MintVID GUI (optional) ──starts──> mrberry ──uses──> berryvp.library
                                  (reads MKV/MP4/TS)     │
                                                         ├─ H.264 ──> VideoCore ──> HDMI
                                                         └─ audio ──> BerryCore mailbox
                                                                         │
                                         Emu68 BerryCore kernel, CPU1: berry_faad03 / berry_ac3 / berry_eac3.module
```

## Source and building

Everything is in [`pistorm/`](pistorm/README.md), with build scripts for each part:

| Part | Directory |
|---|---|
| mrberry | `pistorm/mrberry` |
| berryvp.library (68k assembler) | `pistorm/berryvp_library` |
| CPU1 audio modules | `pistorm/modules` |
| Emu68 BerryCore kernel patch | `pistorm/emu68_berrycore` |
| GUI changes ("PiStorm / VC Hard Fullscreen") | `player/amiga/mrgui*.c`, `player/core/mr_play_options.*` |

    sh pistorm/build_all.sh

This builds all runtime files and compares them with the released P32
binaries. Tools needed:

- m68k-amigaos-gcc
- vasm
- clang/LLVM 17 or 18
- python3

## Credits and licences

- **MintVID** © Darren "boingball" Banfi, MIT: the GUI, and the file readers inside mrberry.
- **PiStorm parts** (mrberry, berryvp.library, BerryCore) © Michael (MichiB210), MIT.
- **CPU1 audio modules:**
  - FAAD2 and liba52: GPL-2.0-or-later
  - FFmpeg: LGPL-2.1-or-later
- **Emu68** © Michal Schulz, MPL-2.0. The BerryCore patch is under the same licence.

Details are in [`pistorm/LICENSES.md`](pistorm/LICENSES.md) and
[`THIRD-PARTY-LICENSES.txt`](THIRD-PARTY-LICENSES.txt).

---

**Deutsch:** MintVID-PiStorm spielt H.264-Videos bis 1080p in Echtzeit und
synchron auf einem Amiga mit PiStorm ab:

- Das Bild entschlüsselt die VideoCore des Raspberry Pi. Es erscheint als Vollbild am HDMI-Ausgang des Pi (P96-Fenster in Planung).
- Den Ton in AAC, AC3 oder E-AC3 entschlüsselt der zweite ARM-Kern des Pi.

**Wichtig:** Es läuft nur mit dem gepatchten Emu68-Kern `Emu68-pistorm-BerryCore`, und zur Zeit nur auf dem PiStorm Classic mit Raspberry Pi 3A+ und Emu68 1.0.7.

Start aus der Shell mit `mrberry film.mkv` oder bequem über MintVID mit
„PiStorm / VC Hard Fullscreen“. Grundlage ist MintVID von Darren Banfi.
