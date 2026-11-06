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
> **Supported boards (Emu68 1.0.7):**
>
> | Board | Pi | HDMI-sound kernel | Tested |
> |---|---|---|---|
> | PiStorm Classic (A500/A600/A2000) | Raspberry Pi 3A+ | `Emu68-pistorm-BerryCore` | up to v96.88 (A2000); v96.97 not yet re-tested |
> | **PiStorm32 lite (A1200)** | **Compute Module 4** | `Emu68-pistorm32lite-BerryCore` | **v96.97, A1200, CM4 1 GB** |
>
> **HDMI sound needs the patched kernel for your board.** The unmodified Emu68
> kernel has no BerryCore service on CPU1, so the AAC/AC3/E-AC3 decoders for
> HDMI cannot run. Video with Paula sound (`PAULA`) also works with the
> original Emu68 1.0.7 kernels. Each patch is made for exactly one kernel file;
> other PiStorm boards (e.g. PiStorm16) and newer Emu68 versions are not
> supported yet.
>
> **`config.txt` on the SD card must contain `gpu_mem=64` or more on the
> Pi 3A+, and `gpu_mem=128` or more on the CM4** (CM4 tested with 128 and 256).
> USB on the CM4 under Emu68 1.0.7: [berrypi4cm.device](https://github.com/MichiB210/berryPi4CM.device)
> (Pi 3A+: [berrypi3ap.device](https://github.com/MichiB210/berryPi3Ap.device)). The VideoCore needs this memory for
> hardware decoding.

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
| Subtitles | **SRT** (file or MKV track), ASS/SSA in MKV as plain text | drawn by the Amiga (mrberry) | second picture layer of the Pi above the film, full screen only |

The Pi 3A+ decodes H.264 up to 1080p at 30 fps, the Pi 4 / CM4 up to 1080p
at 60 fps (Raspberry Pi product briefs). H.265/HEVC and 4K are not possible:
the Pi 4's HEVC block is not reachable through the firmware (MMAL), and its
H.264 decoder also stops at 1080p.
For smooth playback use up to 1080p25/30 or 720p up to 60 fps with a moderate
bitrate. Films smaller than 1280x720 are upscaled by the Pi's ISP to the full
1080 lines before they reach the screen (16:9 to exactly 1920x1080, 4:3 with
borders left and right); the display hardware alone drew green grid lines or
stripes when it had to enlarge a picture, even by a few lines.

Multichannel E-AC3 (5.1) is refused, because the CPU1 decoder is not fast
enough for it in real time yet. Tested on PiStorm Classic with a Raspberry Pi 3A+
and on PiStorm32 lite with a CM4, each with its patched Emu68 1.0.7 kernel.

**Pi 4 / CM4 (since v96.97):** berryvp.library asks the firmware which board it
runs on. On the BCM2711 it writes the VideoCore data addresses in the Pi 4 form
("36-bit pagelists", as the Linux driver does), reads the Workbench picture
from the Pi 4 display list and asks the firmware for the screen size. On the
Pi 3 nothing changes.

## How to use it

From the Shell:

    mrberry movie.mkv

Window, start time, resume and Paula sound:

    mrberry movie.mkv W=50
    mrberry movie.mkv T=1:10
    mrberry movie.mkv R
    mrberry movie.mkv PAULA
    mrberry movie.mkv AUDIO=2      (audio track 2 of an MKV)

Or with the GUI **MintVID-pi**: pick a film and choose the display mode
**PiStorm / VC Hard Fullscreen**, **PiStorm / VC Hard Fenster** or
**PiStorm / VC Hard Fullscreen (Paula)**. MintVID-pi has Play/Pause,
Stop, a time line and ">>" to jump to the time set on the slider.
The Paula sound offset base is set in the menu ("Paula sound base");
during playback + and - fine-tune it per film. Space pauses the film
also with Paula sound: MintVID-pi shows the paused spot as a still
picture in a window, and Play (or Space) continues in full screen with
Paula sound.

In the PiStorm window: Space pauses, **F** switches to full screen at the
same spot (when the film was started from MintVID-pi), Esc or 0 stops.

**Tab** jumps to the next chapter of an MKV, **Shift-Tab** back to the start
of the chapter (or the previous one). Films without chapters jump 10 minutes
forward / back. Works in full screen and in the window when the film was
started from MintVID-pi (the film restarts at the new spot).

**M** switches the sound off / on (mute), in full screen and in the window.
Over HDMI the sound keeps running silently (picture and sound stay in sync),
with Paula the volume goes to 0. Films with subtitles show "Mute" /
"Sound on" for two seconds.

MintVID-pi refuses films whose sound cannot be played with the chosen
output and says why (e.g. "Not playable: audio format DTS is not
supported" - DTS, Opus, FLAC, Vorbis; MP3/MP2/PCM only with Paula; E-AC3
only over HDMI). With several audio tracks it is refused only if none of
them fits. Films without any audio track play without sound.

Small films (below 1280x720) are upscaled by the Pi to the real size of
the HDMI screen (since v96.88; before always 1920x1080, which drew green
lines on screens smaller than 1080p, e.g. 1280x1024).

MKV films with several audio tracks: the "Audio" chooser in MintVID-pi
lists every track (language, format, name). Without a choice the track
marked as default in the MKV plays. The chosen track is remembered in
film.t. Tracks that cannot play with the chosen output are refused (e.g.
MP3 over HDMI, E-AC3 through Paula, DTS/Opus/FLAC anywhere); a film with
no HDMI-capable track plays without sound in full screen.

Subtitles (full screen only): text subtitle tracks inside an MKV (SRT, and
ASS/SSA as plain text) are listed as "<language> ... (MKV)". Or put an SRT
file with the film's name next to it (`movie.srt`, or `movie.<name>.srt`, e.g. `movie.de_forced.srt`,
`movie.englisch_CC.srt`). The "Subtitles" chooser in MintVID-pi lists them.
At the first start the "forced" file in the language of the audio track is
chosen (else the first forced file, else none); your choice is remembered in
film.t. The text is drawn by the Amiga (white, black outline, up to two lines)
on a second picture layer of the Pi above the film. Key S switches to the next
subtitle file while the film plays (Off, 1, 2, ...; the name is shown for two
seconds), key O switches the subtitles off/on, + and - move them by 0.1 s
(HDMI sound). SRT files in UTF-8
or Windows encoding work. Image subtitles (VobSub .idx/.sub, PGS) are not
supported yet.

Note: the subtitle font has the Latin-1 characters only (Western European
languages: English, German, French, Spanish, Italian, ... with ä ö ü ß é ñ).
Other letters - Eastern European (ł č ş ő), Greek, Cyrillic, Asian, Arabic,
Hebrew - are shown as "?". Shell: `mrberry movie.mkv SUB=2`, `SUB=OFF`, `SUB=file.srt`, `NOSUB`.

Tips:

- Start films from the SD card if you can; USB sticks can be slower. On an
  NTFS stick do not rename files on the Amiga (the NTFS driver crashed in a
  test) - rename them on the PC.
- Full screen is recommended. There the Pi decodes and shows the picture
  itself. In the P96 window every picture is copied to the Amiga and drawn
  by it, so depending on the resolution, the file, the audio tracks it
  contains and the audio format (e.g. AC3) the window playback may not stay
  smooth.

Since v96.76 VPberry is no longer needed. All other MintVID playback
modes work as in upstream MintVID.

## Installation

1. Copy `mrberry`, `berryvp.library`, `berry_faad03.module`, `berry_ac3.module` and `berry_eac3.module` into the same drawer.
   - Add `MintVID-pi` too if you want the GUI.
   - Copy `berryvp.library` to `LIBS:` too if an older one is there, and reboot.
2. **For HDMI sound:** put the patched kernel for your board into the folder `KERNEL` of the SD card and enter it in `config.txt`:
   - PiStorm Classic / Pi 3A+: `kernel=kernel/Emu68-pistorm-BerryCore` instead of `Emu68-pistorm`.
   - PiStorm32 lite / CM4: `kernel=kernel/Emu68-pistorm32lite-BerryCore` instead of `Emu68-pistorm32lite` (CaffeineOS has two such lines, `[gpio4=0]` and `[gpio24=0]`; change both).
   - Both are Emu68 1.0.7 with the BerryCore service on CPU1. Keep the old kernel to go back.
   - Other way, without touching `config.txt`: rename the original kernel in `KERNEL` (e.g. `Emu68-pistorm32lite` → `Emu68-pistorm32lite_ori`) and copy the BerryCore kernel under the original name (`Emu68-pistorm32lite-BerryCore` → `Emu68-pistorm32lite`; Pi 3A+: `Emu68-pistorm-BerryCore` → `Emu68-pistorm`). To go back, rename `_ori` back.
   - Without it there is no HDMI sound. Video with Paula sound works with the original kernel.
3. **Required:** in `config.txt` on the SD card, set `gpu_mem=64` or more (CM4: `gpu_mem=128` or more, tested with 128 and 256). If the line is missing, add it.

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
| Emu68 BerryCore kernel patches (Pi 3 and PiStorm32 lite) | `pistorm/emu68_berrycore` |
| GUI changes ("PiStorm / VC Hard Fullscreen" / "Fenster") | `player/amiga/mrgui*.c`, `player/core/mr_play_options.*` |

    sh pistorm/build_all.sh

This builds all runtime files and compares them with the v96.99
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

**Unterstützt (Emu68 1.0.7):** PiStorm Classic mit Raspberry Pi 3A+ und – neu seit v96.97 – **PiStorm32 lite mit Compute Module 4** (getestet im A1200, CM4 1 GB). Die berryvp.library erkennt selbst, auf welchem Pi sie läuft. v96.97 ist auf dem PiStorm Classic noch nicht neu getestet.

**Wichtig:** Für den Ton über HDMI braucht es den gepatchten Kern für die eigene Karte: `Emu68-pistorm-BerryCore` (PiStorm Classic) bzw. `Emu68-pistorm32lite-BerryCore` (PiStorm32 lite). In der `config.txt` die `kernel=`-Zeile(n) darauf umstellen, den alten Kern aufheben. Bild mit Ton über Paula läuft auch mit den Original-Kernen.

Außerdem muss in der `config.txt` auf der SD-Karte `gpu_mem=64` oder mehr stehen (CM4: `gpu_mem=128` oder mehr, getestet mit 128 und 256). USB auf dem CM4 unter Emu68 1.0.7: [berrypi4cm.device](https://github.com/MichiB210/berryPi4CM.device). Fehlt die Zeile, bitte ergänzen. H.265 und 4K gehen nicht, auch der Pi 4 decodiert H.264 nur bis 1080p.

Start aus der Shell mit `mrberry film.mkv` oder bequem über MintVID mit
„PiStorm / VC Hard Fullscreen“. Grundlage ist MintVID von Darren Banfi. Der PiStorm läuft mit Emu68 von Michal Schulz (michalsc). Die PiStorm-Hardware stammt von Claude Schwarz.
