# Third-party components

| Component | License | How it is used |
|---|---|---|
| [FFmpeg](https://ffmpeg.org) n6.1.1 (libavcodec, libavutil, libswresample) | LGPL 2.1 or later | Statically linked audio/subtitle decoders only (AC-3, E-AC-3, AAC, Opus, DTS core, FLAC, MP3, MP2, Vorbis, PGS). Built with `tools/build_ffmpeg_ps4.sh`; the exact configure line is in that script. |
| [libsmb2](https://github.com/sahlberg/libsmb2) | LGPL 2.1 | SMB client, linked from the OpenOrbis toolchain environment. |
| `src/stb_truetype.h` (Sean Barrett) | Public domain / MIT | Text rendering. |
| `src/inflate.h` | Written for this project, structured after zlib's public `puff` reference decoder | zlib-compressed Matroska tracks. |
| [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) | See its repository | Used to build; nothing from it is redistributed here. |

Because FFmpeg and libsmb2 are LGPL and linked statically, anyone who receives a
binary must be able to relink it: this repository contains all of this
project's source and the script that rebuilds FFmpeg, so that is satisfied as
long as you publish the matching source with any binary release.

Not included on purpose: a font (supply any TTF as `font.ttf`, e.g. DejaVu Sans),
`sce_sys/param.sfo` and `icon0.png` (your own title metadata and icon).
