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
output or in a P96 window on the Workbench.

This project is built on **[MintVID](https://github.com/boingball/MintVID)
by Darren "boingball" Banfi**, the modern 68k video player for AmigaOS, and
includes it in full (MIT licence). MintVID's own README is in
[`docs/MintVID-README.md`](docs/MintVID-README.md).

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

## What it plays

| | Format | Decoded by | Output |
|---|---|---|---|
| Video | **H.264 / AVC up to 1920x1080** | Raspberry Pi VideoCore (MMAL) | HDMI hard full screen ("PiStorm / VC Hard Fullscreen", mrberry), real time, A/V sync |
| Video | same | Raspberry Pi VideoCore (MMAL) | P96 window on the Workbench ("PiStorm / VC Hard Fenster", mrberry W=10/25/50/75) |
| Audio | **AAC-LC** | FAAD2 on CPU1 of the Pi | stereo PCM over the Pi's HDMI |
| Audio | **AC3 (Dolby Digital)**, up to 5.1 | liba52 on CPU1 | stereo downmix over HDMI |
| Audio | **E-AC3 (Dolby Digital Plus)**, stereo | FFmpeg on CPU1 | stereo PCM over HDMI |
| Audio | AAC, MP3, MP2, AC3, PCM | Amiga (Paula player by Darren 'boingball' Banfi) | Paula, full screen only ("PiStorm / VC Hard Fullscreen (Paula)", mrberry PAULA) |
| Container | MKV, MP4/MOV, MPEG-TS | mrberry (68k) | |

Multichannel E-AC3 (5.1) is refused, because the CPU1 decoder is not fast
enough for it in real time yet. Tested on PiStorm Classic with a Raspberry Pi 3A+
and the patched Emu68 1.0.7 kernel.

## How to use it

From the Shell:

    mrberry movie.mkv

Window, start time, resume and Paula sound:

    mrberry movie.mkv W=50
    mrberry movie.mkv T=1:10
    mrberry movie.mkv R
    mrberry movie.mkv PAULA

Or with the GUI **MintVID-pi**: pick a film and choose the display mode
**PiStorm / VC Hard Fullscreen**, **PiStorm / VC Hard Fenster** or
**PiStorm / VC Hard Fullscreen (Paula)**. MintVID-pi has Play/Pause,
Stop, a time line and ">>" to jump to the time set on the slider.
The Paula sound offset base is set in the menu ("Paula sound base");
during playback + and - fine-tune it per film.

Since v96.76 VPberry is no longer needed. All other MintVID playback
modes work as in upstream MintVID.

## Installation

1. Copy `mrberry`, `berryvp.library`, `berry_faad03.module`, `berry_ac3.module` and `berry_eac3.module` into the same drawer.
   - Add `MintVID-pi` too if you want the GUI.
2. **For HDMI sound:** replace the Emu68 kernel on the PiStorm's SD card (`Emu68-pistorm`) with the patched `Emu68-pistorm-BerryCore`.
   - This is Emu68 1.0.7 for PiStorm Classic / Raspberry Pi 3A+, with the BerryCore service on CPU1.
   - Without it there is no HDMI sound. Video with Paula sound works with the original kernel.
   - It only exists for this Emu68 version and this hardware.
3. **Required:** in `config.txt` on the SD card, set `gpu_mem=64`. If the line is missing, add it.

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
| VPberry (old window player, not needed since v96.76) | `pistorm/vpberry` |
| berryvp.library (68k assembler) | `pistorm/berryvp_library` |
| CPU1 audio modules | `pistorm/modules` |
| Emu68 BerryCore kernel patch | `pistorm/emu68_berrycore` |
| GUI changes ("PiStorm / VC Hard Fullscreen" / "Fenster") | `player/amiga/mrgui*.c`, `player/core/mr_play_options.*` |

    sh pistorm/build_all.sh

This builds all runtime files and compares them with the released v96.76
binaries. Tools needed:

- m68k-amigaos-gcc
- vasm
- clang/LLVM 17 or 18
- python3

## Credits and licences

This project would not exist without:

- **[MintVID](https://github.com/boingball/MintVID)** by Darren "boingball" Banfi: the player this project is built on.
- **[Emu68](https://github.com/michalsc/Emu68)** by Michal Schulz ([michalsc](https://github.com/michalsc)): the 68k JIT emulator for the Raspberry Pi that the PiStorm runs. BerryCore is a small patch to Emu68 1.0.7 that uses the Pi's second core.
- **[PiStorm](https://github.com/captain-amygdala/pistorm)** by Claude Schwarz ([captain-amygdala](https://github.com/captain-amygdala)): the accelerator board that puts a Raspberry Pi into the Amiga's CPU socket.

Licences:

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

- Das Bild entschlüsselt die VideoCore des Raspberry Pi. Es erscheint als Vollbild am HDMI-Ausgang des Pi oder im P96-Fenster auf der Workbench (mrberry).
- Im Vollbild kann der Ton auch über Paula laufen (Paula-Player von Darren 'boingball' Banfi).
- Den Ton in AAC, AC3 oder E-AC3 entschlüsselt der zweite ARM-Kern des Pi.

**Wichtig:** Für den Ton über HDMI braucht es den gepatchten Emu68-Kern `Emu68-pistorm-BerryCore`. Bild mit Ton über Paula läuft auch mit dem Original-Kern Emu68 1.0.7. Zur Zeit läuft alles nur auf dem PiStorm Classic mit Raspberry Pi 3A+ und Emu68 1.0.7.

Außerdem muss in der `config.txt` auf der SD-Karte `gpu_mem=64` stehen. Fehlt die Zeile, bitte ergänzen.

Start aus der Shell mit `mrberry film.mkv` oder bequem über MintVID mit
„PiStorm / VC Hard Fullscreen“. Grundlage ist MintVID von Darren Banfi. Der PiStorm läuft mit Emu68 von Michal Schulz (michalsc). Die PiStorm-Hardware stammt von Claude Schwarz.
