# Jaja Video Player

A video player for the PlayStation 4 (jailbroken, homebrew). It plays video from a **Windows/Samba (SMB) network share**, the **internal drive** and **USB drives**, using the PS4's hardware video decoder.

Written in C with the [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) toolchain. Tested on a PS4 Pro.

## Features

- **Video:** H.264 and HEVC (including 10-bit) in MKV, MP4, M4V and MOV, hardware decoded. Phone videos (rotation flag) and fragmented MP4 are supported.
- **Audio:** E-AC-3, AC-3, AAC, Opus, DTS (core), FLAC, MP3, MP2 and Vorbis, decoded in software and mixed down to stereo.
- **Subtitles:** SRT, ASS/SSA and MP4 text subtitles, plus PGS (Blu-ray picture subtitles). Choose audio and subtitle tracks while playing; your choice is remembered by language. Adjustable subtitle size and delay.
- **Browsing:** SMB share, internal drive and USB drives (switch with L2/R2), a "Continue watching" list, sorting (name / newest / largest) and search. The app reopens where you left off.
- **Playback:** seeking, resume where you stopped, automatic next episode, audio sync adjustment, SMB auto-reconnect, keeps the console awake.
- **Settings screen** (Start) with the on-screen keyboard: server, share, folder, user, password, a connection test, subtitle size, auto-play and a debug log switch.

Not supported: AV1/VP9 (the PS4 has no hardware decoder; re-encode with `ffmpeg -c:v libx264 -c:a copy`), HDR output, surround output (audio is stereo).

## Controls

| Browser | |
|---|---|
| Up/Down, L1/R1 | move, page |
| L2/R2 | switch source (SMB, Internal, USB, Continue watching) |
| Cross / Circle | open / back (hold Circle: quit) |
| Square | speed test of the selected file (SMB) |
| Triangle | refresh |
| Touchpad / R3 | search / sort order |
| Start | settings |

| Playback | |
|---|---|
| Cross | pause |
| L1/R1, L2/R2 | seek ±10 s, ±60 s |
| Up / Down | next audio track / next subtitle track (then off) |
| Left/Right | audio sync (10 ms steps), or subtitle delay after pressing Options |
| Triangle | next file |
| Square | information overlay |
| Circle | back (hold: quit) |

## Building

You need the OpenOrbis PS4 toolchain (`OO_PS4_TOOLCHAIN` set), and:

1. **FFmpeg libraries** in `ffinst/` (static, decoders only): run `tools/build_ffmpeg_ps4.sh` after adjusting the paths at its top (it expects an FFmpeg n6.1.1 source tree and the toolchain's libc headers). This produces `ffinst/lib/*.a` and `ffinst/include`.
2. **libsmb2** for the PS4 (`-lsmb2`) installed in the toolchain's `lib` and `include`.
3. **A font:** put any TrueType font at `font.ttf` (for example DejaVu Sans).
4. **Title metadata:** `sce_sys/param.sfo` and `sce_sys/icon0.png`, as in the OpenOrbis samples. `python3 tools/set_title.py sce_sys/param.sfo "Jaja Video Player"` sets the title shown on the home screen.

Then:

```
make
```

builds the `.pkg`. Install it on the PS4 as usual (delete the old version first when reinstalling).

## Configuration

On first start the app writes `/data/jajasamba/config.ini` (the folder name is kept from an earlier name of the project). Edit it by hand, or use the Settings screen:

```
server=192.168.1.50
share=Videos
user=guest
password=
path=
```

Resume positions, remembered tracks and the log live in the same folder.

## Reporting problems

Turn on **Debug log** in Settings, reproduce the problem, leave the player, then fetch `/data/jajasamba/log.txt` (for example over FTP) and attach it to your issue.

## License

MIT for this project's own code (see `LICENSE`); third-party components keep their licenses (see `THIRD-PARTY.md`). This project is not affiliated with Sony.
