# PS5 payload, built with the ps5-payload-dev SDK and its pacbrew libraries
# (zlib, zstd; libsmb2, libnfs and libcurl for the network destinations).
# Run from the repository's root, in the SDK image: tools/ps5-build.sh.
#
#   make -f src/platform/ps5/ps5.mk                 build/ps5/ripper.elf
#   make -f src/platform/ps5/ps5.mk deploy          send it to PS5_HOST's ELF loader (port 9021)

PS5_HOST ?= 10.0.0.24
PS5_PORT ?= 9021
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PKGCONF := $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
OUT := build/ps5
GEN := $(OUT)/gen
ELF := $(OUT)/ripper.elf
# The SDK's Mozilla CA bundle, built in: the console has no CA store for HTTPS (RomM).
CA_BUNDLE := $(PS5_PAYLOAD_SDK)/target/user/homebrew/etc/ca-bundle.crt

VERSION ?= 0.2.0
# Each build its own stamp: RipperPS tells the ripper it carries from the one
# running by it (the "ripper-build:" mark in the ELF, /api/status's build).
BUILD_STAMP := $(shell date -u +%Y%m%d-%H%M%S)
BUILD_ID ?= $(BUILD_STAMP)

CORE := $(filter-out src/core/config.c,$(wildcard src/core/*.c))
SERVICE := $(wildcard src/service/*.c)
PLATFORM := src/platform/ps5/cam_transport.c src/platform/ps5/game_dump.c src/platform/ps5/platform.c src/platform/ps5/main.c \
	third_party/ps5-self-pager/selfpager.c
DVDCSS := $(addprefix third_party/libdvdcss/src/,css.c device.c error.c libdvdcss.c)
SRCS := $(CORE) $(SERVICE) $(PLATFORM) third_party/cJSON.c $(DVDCSS)
HDRS := $(wildcard include/ripper/*.h src/service/*.h src/platform/ps5/*.h)

CFLAGS := -std=gnu11 -O2 -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-unreachable-code-generic-assoc -Iinclude -Isrc/service -Isrc/platform/ps5 -Ithird_party -Ithird_party/libdvdcss/src -Ithird_party/ps5-self-pager \
	-I$(GEN) -DRIPPER_VERSION='"$(VERSION)"' -DRIPPER_BUILD='"$(BUILD_ID)"' $$($(PKGCONF) --cflags zlib libzstd libcurl libsmb2 libnfs flac libavformat libavcodec libavfilter libswresample libswscale libavutil) \
	-DRIPPER_HAVE_CURL -DRIPPER_HAVE_SMB2 -DRIPPER_HAVE_NFS -DRIPPER_HAVE_FLAC -DRIPPER_HAVE_LAME -DRIPPER_HAVE_FFMPEG -DRIPPER_EMBED_CA
LIBS := $$($(PKGCONF) --static --libs libavformat libavcodec libavfilter libswresample libswscale libavutil libcurl libsmb2 libnfs flac zlib libzstd) -lmp3lame -pthread -lkernel_sys -Wl,--wrap=pthread_create

.PHONY: all deploy clean
all: $(ELF)

$(GEN)/inter-font.css: third_party/inter-latin.woff2 tools/font_css.py
	@mkdir -p build/gen $(GEN)
	python3 tools/font_css.py $< build/gen/inter-font.css Inter
	cp build/gen/inter-font.css $@

$(GEN)/ui_index.h: ui/index.html ui/style.css ui/ripper.css $(wildcard ui/themes/*.css) tools/embed.py $(GEN)/inter-font.css
	python3 tools/embed.py ui/index.html $@ UI_INDEX_HTML

$(GEN)/cacert_pem.h: $(CA_BUNDLE) tools/embed.py
	@mkdir -p $(GEN)
	python3 tools/embed.py $< $@ CACERT_PEM

$(GEN)/gamedb_data.h: third_party/gamedb/gamedb.tsv tools/embed.py
	@mkdir -p $(GEN)
	python3 tools/embed.py $< $@ GAMEDB_GZ --gzip

# drakmor's PlayGo and AMPR emulators, for PS5 game dumps' fakelib/.
$(GEN)/emu_playgo.h: third_party/ps5-emulators/libScePlayGo.sprx tools/embed.py
	@mkdir -p $(GEN)
	python3 tools/embed.py $< $@ EMU_PLAYGO --raw

$(GEN)/emu_ampr.h: third_party/ps5-emulators/libSceAmpr.sprx tools/embed.py
	@mkdir -p $(GEN)
	python3 tools/embed.py $< $@ EMU_AMPR --raw

$(ELF): $(SRCS) $(HDRS) $(GEN)/ui_index.h $(GEN)/cacert_pem.h $(GEN)/gamedb_data.h $(GEN)/emu_playgo.h $(GEN)/emu_ampr.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

deploy: $(ELF)
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(ELF)

clean:
	rm -rf $(OUT)
