#!/bin/bash
# Rebuilds the FFmpeg libraries used by Jaja Video Player (PS4 target + host test build).
set -e
SRC=/home/claude/ffmpeg-src
DECODERS=eac3,ac3,aac,pgssub,opus,dca,flac,mp3float,mp2float,vorbis
# ---- PS4 ----
mkdir -p /home/claude/ffbuild && cd /home/claude/ffbuild
make distclean >/dev/null 2>&1 || true
$SRC/configure --prefix=/home/claude/ffinst --enable-cross-compile --arch=x86_64 --target-os=linux --cc=clang --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm \
  --extra-cflags="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -DPS4 -isystem /home/claude/oo-sysroot/include" --extra-ldflags="-fuse-ld=lld" \
  --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swscale --disable-avfilter \
  --disable-network --disable-autodetect --disable-asm --disable-x86asm --disable-pthreads --disable-w32threads --disable-os2threads --disable-debug \
  --disable-runtime-cpudetect --disable-iconv --disable-stripping --disable-shared --enable-static \
  --enable-decoder=$DECODERS --enable-parser=ac3,aac,opus > /tmp/ffcfg.log 2>&1
sed -i -E 's/#define HAVE_(SCHED_GETAFFINITY|SYSCONF|MKSTEMP|GETTIMEOFDAY|CLOCK_GETTIME|NANOSLEEP|USLEEP|FCNTL|ISATTY|MMAP|MPROTECT|GETRUSAGE|GETPROCESSMEMORYINFO|SETRLIMIT) 1/#define HAVE_\1 0/' config.h
make -j8 > /tmp/ffmake.log 2>&1
rm -rf /home/claude/ffinst; make install > /dev/null 2>&1
# ---- host (for tests) ----
mkdir -p /home/claude/ffbuild-host && cd /home/claude/ffbuild-host
make distclean >/dev/null 2>&1 || true
$SRC/configure --prefix=/home/claude/ffinst-host --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-swscale \
  --disable-avfilter --disable-network --disable-autodetect --disable-asm --disable-x86asm --disable-pthreads --disable-debug \
  --disable-stripping --enable-static --disable-shared --enable-decoder=$DECODERS --enable-parser=ac3,aac,opus > /tmp/ffcfgh.log 2>&1
make -j8 > /tmp/ffmakeh.log 2>&1
make install > /dev/null 2>&1
echo done
