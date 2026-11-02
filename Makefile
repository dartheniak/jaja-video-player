TARGET := jajasamba

ifndef OO_PS4_TOOLCHAIN
$(error OO_PS4_TOOLCHAIN is not set)
endif

TC  := $(OO_PS4_TOOLCHAIN)
BIN := $(TC)/bin/linux

# Read the content ID directly from param.sfo so the two always match.
CONTENT_ID := $(shell grep -a -o -E '[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_00-[A-Z0-9]{16}' sce_sys/param.sfo 2>/dev/null | head -n 1)

ifeq ($(CONTENT_ID),)
$(error Could not find a content ID in sce_sys/param.sfo. Check that the file exists and is valid)
endif

# Files that go inside the package (font.ttf ends up at /app0/font.ttf)
PKG_FILES := eboot.bin sce_sys/param.sfo sce_sys/icon0.png font.ttf \
             sce_module/libc.prx sce_module/libSceFios2.prx

# FFmpeg libavcodec/libavutil (audio decoders only), built for the PS4 - see ffinst/
FF      := ffinst

CFLAGS  := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -c \
           -isysroot $(TC) -isystem $(TC)/include -I$(FF)/include
LDFLAGS := -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr \
           -L$(TC)/lib -L$(FF)/lib
LIBS    := -lsmb2 -lavcodec -lswresample -lavutil -lc -lSceNet -lkernel -lSceUserService -lScePad -lSceSysUtil -lSceSystemService -lSceVideoOut -lSceSysmodule -lSceAudioOut $(if $(wildcard $(TC)/lib/libSceImeDialog.*),-lSceImeDialog,)

all: $(CONTENT_ID).pkg

main.o: src/main.c $(wildcard src/*.h)
	clang $(CFLAGS) -o $@ src/main.c

$(TARGET).elf: main.o
	ld.lld $(LDFLAGS) $(TC)/lib/crt1.o $< $(LIBS) -o $@

eboot.bin: $(TARGET).elf
	$(BIN)/create-fself -in=$< -out=$(TARGET).oelf -eboot=eboot.bin -paid=0x3800000000000011

pkg.gp4: $(PKG_FILES)
	$(BIN)/create-gp4 -out $@ --content-id="$(CONTENT_ID)" --files "$(PKG_FILES)"

$(CONTENT_ID).pkg: pkg.gp4
	$(BIN)/PkgTool.Core pkg_build $< .

clean:
	rm -f *.o *.elf *.oelf eboot.bin pkg.gp4 *.pkg

.PHONY: all clean
