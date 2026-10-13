# =====================================================================================================================
#  Sayonara o Oshiete (SiglusEngine VN) — native C11 + SDL3 engine: build, data, test and packaging.
#
#  Quick start:      make check        (what is missing and how to install it)
#                    make build        (self-contained game for this OS in dist/<platform>/)
#                    make run          (play from the dev build; RUN_ARGS="--lang en" for the English patch)
#  Everything else:  make help
#
#  GNU make 3.81 or newer (the stock make of macOS works). Out-of-tree: all outputs go to build/ and dist/.
# =====================================================================================================================

SHELL := /bin/sh
empty :=
space := $(empty) $(empty)
comma := ,
.DEFAULT_GOAL := help
.SUFFIXES:
.DELETE_ON_ERROR:

ROOT    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ENGINE  := $(ROOT)/engine
BUILD   := $(ROOT)/build
DIST    := $(ROOT)/dist
SELF    := $(MAKE) --no-print-directory -f $(ROOT)/Makefile

# ---------------------------------------------------------------------------------------------------------------------
#  Pinned third-party downloads (sha256 from the GitHub release asset digests)
# ---------------------------------------------------------------------------------------------------------------------
SDL_VERSION        := 3.4.16
SDL_SHA256         := 7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
SDL_URL            := https://github.com/libsdl-org/SDL/releases/download/release-$(SDL_VERSION)/SDL3-$(SDL_VERSION).tar.gz
LLVM_MINGW_VERSION := 20260922
LLVM_MINGW_SHA256_linux-x86_64 := bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
LLVM_MINGW_SHA256_linux-arm64  := 07d21263c56bfe9a713db6fdb3f7434bf4c121a005e40397d3b4c0170fb06769
LLVM_MINGW_SHA256_macos        := 52e5f5a7b131021d0c39a37a38fa380a1da7885cd04bd61afd0cd4ecfb8bc1f3

# ---------------------------------------------------------------------------------------------------------------------
#  User settings (override on the command line: make build JA_ZIP=/path/to/game.zip)
# ---------------------------------------------------------------------------------------------------------------------
ASSETS   ?= $(abspath $(ROOT)/../assets)
JA_ZIP   ?=
EN_ZIP   ?=
GAMEDATA ?=
NO_EN    ?=
FONT     ?=
SDL      ?= static
SAN      ?=
DEBUG    ?=
VG       ?=
V        ?=
RUN_ARGS ?=
MACOSX_DEPLOYMENT_TARGET ?= 11.0

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# ---------------------------------------------------------------------------------------------------------------------
#  Host detection
# ---------------------------------------------------------------------------------------------------------------------
UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
UNAME_M := $(shell uname -m 2>/dev/null || echo unknown)
ifeq ($(UNAME_S),Linux)
  HOST_OS := linux
else ifeq ($(UNAME_S),Darwin)
  HOST_OS := macos
else ifneq ($(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)),)
  HOST_OS := windows
else
  HOST_OS := unknown
endif
ifneq ($(filter x86_64 amd64 AMD64,$(UNAME_M)),)
  HOST_ARCH := x86_64
else ifneq ($(filter aarch64 arm64 ARM64,$(UNAME_M)),)
  HOST_ARCH := arm64
else
  HOST_ARCH := $(UNAME_M)
endif
ifeq ($(HOST_OS),macos)
  HOST_PLATFORM := macos-universal
else
  HOST_PLATFORM := $(HOST_OS)-$(HOST_ARCH)
endif
ifeq ($(HOST_OS),windows)
  HOST_EXE := .exe
else
  HOST_EXE :=
endif
NPROC := $(shell (nproc || sysctl -n hw.ncpu || echo 4) 2>/dev/null | head -n 1)

# host C compiler for the pack tool (never the cross compiler)
HOST_CC ?= cc

HOSTTOOL     := $(BUILD)/host/sayopack$(HOST_EXE)
DATA_DIR     := $(BUILD)/data
DATA_STAMP   := $(DATA_DIR)/.stamp
ICON_ICO     := $(BUILD)/icon/ADIEU.ico
PAYLOAD      := $(BUILD)/payload/game.pak
TESTS        := $(ENGINE)/tests
# The test, analysis and site-test suites live in the development tree only; a build-only checkout (engine
# sources, Makefile, site) has none of them, and their targets say so instead of failing obscurely.
DEVTOOLS     := $(if $(wildcard $(TESTS)/run_m0.sh),$(wildcard $(ROOT)/tools/lint/banned.sh),)
NEED_DEVTOOLS = $(if $(DEVTOOLS),,@printf '*** make %s: the test and analysis suites are not part of this checkout (build-only tree)\n' '$@'; exit 2)
# Valgrind / Helgrind / DRD need only tools/valgrind (they play the user's own game data from build/data).
VGTOOLS      := $(wildcard $(ROOT)/tools/valgrind/summary.sh)
NEED_VGTOOLS  = $(if $(VGTOOLS),,@printf '*** make %s: tools/valgrind is not part of this checkout\n' '$@'; exit 2)

# osxcross (macOS SDK cross toolchain on Linux): OSXCROSS_ROOT=/path/to/osxcross or o64-clang in PATH
OSXCROSS_ROOT ?=
OSXCROSS_BIN  := $(if $(OSXCROSS_ROOT),$(OSXCROSS_ROOT)/target/bin,$(patsubst %/,%,$(dir $(shell command -v o64-clang 2>/dev/null))))

# ---------------------------------------------------------------------------------------------------------------------
#  Shared shell helpers (expanded at the start of recipes)
# ---------------------------------------------------------------------------------------------------------------------
define SH_LIB
set -e; \
say()  { printf '%s\n' "$$*"; }; \
step() { printf '==> %s\n' "$$*"; }; \
fail() { printf '\n*** ERROR: %s\n' "$$1" >&2; shift; for l in "$$@"; do printf '    %s\n' "$$l" >&2; done; printf '\n' >&2; exit 1; }; \
have() { command -v "$$1" >/dev/null 2>&1; }; \
sha256_of() { if have sha256sum; then sha256sum "$$1" | cut -d' ' -f1; elif have shasum; then shasum -a 256 "$$1" | cut -d' ' -f1; else openssl dgst -sha256 -r "$$1" | cut -d' ' -f1; fi; }; \
fetch() { url="$$1"; out="$$2"; want="$$3"; mkdir -p "$$(dirname "$$out")"; \
  if [ -f "$$out" ] && [ "$$(sha256_of "$$out")" = "$$want" ]; then return 0; fi; \
  step "download $$url"; rm -f "$$out.part"; \
  have curl || have wget || fail "neither curl nor wget is installed" "run 'make check' for install commands"; \
  n=0; until if have curl; then curl -fsSL --connect-timeout 30 -o "$$out.part" "$$url"; else wget -q -O "$$out.part" "$$url"; fi; do \
    n=$$((n+1)); [ $$n -lt 4 ] || fail "download failed: $$url" "check your network connection and run make again"; \
    say "    retrying ($$n/3)..."; sleep 2; done; \
  got="$$(sha256_of "$$out.part")"; \
  if [ "$$got" != "$$want" ]; then rm -f "$$out.part"; fail "checksum mismatch for $$url" "expected $$want" "got      $$got"; fi; \
  mv "$$out.part" "$$out"; }
endef

# ---------------------------------------------------------------------------------------------------------------------
#  Help
# ---------------------------------------------------------------------------------------------------------------------
.PHONY: help
help:
	@printf '%s\n' \
	  'Sayonara o Oshiete - native engine (C11 + SDL3).   Host: $(HOST_PLATFORM)' \
	  '' \
	  'Quick start:  make check   ->   make build   ->   ./dist/$(HOST_PLATFORM)/... (or: make run)' \
	  '' \
	  'Setup' \
	  '  make check              verify tools for a native build + data; prints install commands per distro' \
	  '  make check-windows      same for build-windows (mingw-w64 or downloadable llvm-mingw)' \
	  '  make check-macos        same for build-macos (Xcode CLT on a Mac, or osxcross on Linux)' \
	  '  make data               canonical data tree build/data/{ja,en} from the original archives in ../assets' \
	  '  make deps               download (sha256-pinned) and build static SDL $(SDL_VERSION) for PLATFORM' \
	  '  make toolchain-windows  download the pinned llvm-mingw $(LLVM_MINGW_VERSION) (unless mingw-w64 is installed)' \
	  '' \
	  'Build' \
	  '  make build              self-contained game for this OS (engine + JA data + EN patch in one file)' \
	  '  make build-linux        dist/linux-<arch>/sayooshi' \
	  '  make build-windows      dist/windows-x86_64/Sayonara.exe   (cross-compiles from Linux/macOS)' \
	  '  make build-macos        dist/macos-universal/Sayonara.app  (native Mac, or osxcross)' \
	  '  make build-all          every target this host can build' \
	  '  make run                build the dev binary (no payload) and run it on build/data; RUN_ARGS=...' \
	  '' \
	  'Web builder (site/: static page that builds the games in the browser from the user'"'"'s archives)' \
	  '  make site               build this host + Windows, stage their payload-free engines in site/engines/' \
	  '  make site-serve         serve site/ at http://localhost:8000 (SITE_PORT=...)' \
	  '  make website            assemble the deployed site in build/website (what Netlify publishes)' \
	  '  make website-serve      serve build/website at http://localhost:8000 (the builder is at /sayooshi/)' \
	  '' \
	  'Housekeeping' \
	  '  make info               show the configuration (platform, compilers, pins, paths)' \
	  '  make clean              remove objects and intermediate build dirs (keeps deps, data, dist)' \
	  '  make fclean             clean + dist/ + downloaded deps/toolchains + build/data' \
	  '  make re                 clean, then build' \
	  '' \
	  'Variables' \
	  '  JA_ZIP=FILE   original JA download-edition zip   (default: ../assets/*JA*Download*.zip)' \
	  '  EN_ZIP=FILE   English patch v1.1 zip              (default: ../assets/*EnglishPatch*.zip)' \
	  '  GAMEDATA=DIR  use an extracted GameData folder instead of JA_ZIP' \
	  '  NO_EN=1       build without the English patch     FONT=FILE  bundle a Japanese TTF/OTF/TTC as font.ttf' \
	  '  SDL=system    link the system SDL3 dynamically (dev only; dist is then not self-contained)' \
	  '  SAN=1         AddressSanitizer + UBSan for native dev builds (SAN=address,undefined,... passes a list)' \
	  '  DEBUG=1       -O1 -g + SAYO_DEBUG memory checks (own build dir)   VG=1  the valgrind build   VG_FLAGS=...' \
	  '  CC=...  CFLAGS=...  LDFLAGS=...  V=1 (verbose)  RUN_ARGS="--lang en --scale 2"  OSXCROSS_ROOT=DIR'
	@printf '%s\n' '' 'Debug builds' \
	  '  make dev DEBUG=1        debug build build/<platform>-debug/ (memory guards, poisoning, leak report)' \
	  '  make dev SAN=1          ASan/UBSan dev binary build/<platform>-san/sayooshi (native only, no payload)'
ifneq ($(VGTOOLS),)
	@printf '%s\n' '' 'Memory and thread checks (play your game data from build/data)' \
	  '  make valgrind           memcheck (leaks = errors) on selftests, seen101 ja/en, save/load, selector, sayopack' \
	  '  make helgrind | drd     the same runs under the thread checkers, with the audio callback thread running'
endif
ifneq ($(DEVTOOLS),)
	@printf '%s\n' '' \
	  'Quality' \
	  '  make test               unit selftests + milestone tests M0-M3 + English edition + payload smoke test' \
	  '  make lint               -Werror gcc+clang, cppcheck, clang-tidy, banned calls, symbol prefixes, include layers' \
	  '  make fuzz               fuzz every file-format parser under ASan/UBSan (FUZZ_TIME=120 s in total, FUZZ_TARGET=a,b)' \
	  '  make fuzz LIBFUZZER=1   coverage-guided libFuzzer run of one target (clang; FUZZ_TARGET=name, FUZZ_TIME per run)' \
	  '  make format FILES=...   clang-format the named files (advisory style, .clang-format)' \
	  '  make test-saves         save-format acceptance (engine/tests/run_saves.sh: migration, fuzz under ASan, M3)' \
	  '  make test SAN=1         unit selftests + M0-M3 on the sanitizer build (payload smoke test skipped)' \
	  '' \
	  '  make site-test          the page'"'"'s build code in Node, then the real page in headless Chromium, vs dist/'
endif

# =====================================================================================================================
#  OUTER LEVEL: user-facing targets. They run sequentially and call this Makefile again with INNER=1 PLATFORM=...,
#  where the real (parallel-safe, -j) rules live.
# =====================================================================================================================
ifneq ($(INNER),1)

.NOTPARALLEL:

.PHONY: build build-linux build-windows build-macos build-all run test lint format data deps payload toolchain-windows \
        clean fclean re info check check-windows check-macos check-linux dev test-saves valgrind helgrind drd fuzz \
        site site-test site-serve website website-serve

build:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-quiet dist

build-linux:
	$(Q)if [ "$(HOST_OS)" != linux ]; then \
	  printf '\n*** build-linux needs a Linux host (host is %s).\n\n' '$(HOST_PLATFORM)' >&2; exit 2; fi
	+$(Q)$(SELF) INNER=1 PLATFORM=linux-$(HOST_ARCH) check-quiet dist

build-windows:
	+$(Q)$(SELF) INNER=1 PLATFORM=windows-x86_64 check-quiet dist

build-macos:
	$(Q)if [ "$(HOST_OS)" != macos ] && [ -z "$(OSXCROSS_BIN)" ]; then \
	  printf '%s\n' '' \
	  '*** build-macos: cannot build a macOS app on this host ($(HOST_PLATFORM)).' \
	  '' \
	  '    A macOS binary needs Apple'"'"'s SDK, which may only be used on Apple hardware / under its license:' \
	  '      * On a Mac (recommended):  xcode-select --install ; brew install cmake sevenzip ; make build' \
	  '        -> dist/macos-universal/Sayonara.app (arm64 + x86_64, ad-hoc signed)' \
	  '      * On Linux with osxcross (https://github.com/tpoechtrager/osxcross) built from your own SDK:' \
	  '        make build-macos OSXCROSS_ROOT=/path/to/osxcross   (or put o64-clang in PATH)' \
	  '' >&2; exit 2; fi
	+$(Q)$(SELF) INNER=1 PLATFORM=$(if $(filter macos,$(HOST_OS)),macos-universal,macos-x86_64) check-quiet dist

build-all:
	$(Q)built=; skipped=; \
	for t in linux windows macos; do \
	  case $$t in \
	    linux) [ "$(HOST_OS)" = linux ] || { skipped="$$skipped linux(needs a Linux host)"; continue; } ;; \
	    macos) [ "$(HOST_OS)" = macos ] || [ -n "$(OSXCROSS_BIN)" ] || { skipped="$$skipped macos(needs a Mac or osxcross)"; continue; } ;; \
	  esac; \
	  $(SELF) build-$$t || exit 1; built="$$built $$t"; \
	done; \
	printf '\nbuild-all: built:%s\n' "$$built"; [ -z "$$skipped" ] || printf 'build-all: skipped:%s\n' "$$skipped"

run:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-quiet run-dev

dev:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-quiet devbin-info

test-saves:
	$(NEED_DEVTOOLS)
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-quiet test-saves-inner

test:
	$(NEED_DEVTOOLS)
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-quiet test-all

lint:
	$(NEED_DEVTOOLS)
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) lint-all

format:
	$(NEED_DEVTOOLS)
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) format-files

valgrind helgrind drd:
	$(NEED_VGTOOLS)
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) VG=1 check-quiet vg-$@

# portable driver on the SAN=1 objects, or libFuzzer (clang, its own -lf objects with coverage instrumentation)
fuzz:
	$(NEED_DEVTOOLS)
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) $(if $(LIBFUZZER),LIBFUZZER=1 CC=clang SAN=fuzzer-no-link$(comma)address$(comma)undefined,SAN=$(or $(SAN),1)) check-quiet fuzz-run

data:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-data-quiet data-tree

payload:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-data-quiet payload-file

deps:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(or $(PLATFORM),$(HOST_PLATFORM)) sdl

# site/: the engines carry no game data, so a static page can build the games from the user's own archives.
SITE_PORT ?= 8000
site: build build-windows
	@printf '\n==> site: stage the payload-free engines\n'
	$(Q)python3 -I $(ROOT)/tools/site/stage.py $(HOSTTOOL) $(ROOT)
	@printf 'Ready: site/ (make site-serve to try it; commit and push to deploy)\n'

site-test: site
	$(NEED_DEVTOOLS)
	@command -v node >/dev/null 2>&1 || { echo '*** make site-test needs node (22 or newer)'; exit 1; }
	@ja="$(JA_ZIP)"; [ -n "$$ja" ] || ja="$$(ls "$(ASSETS)"/*JA*Download*.zip 2>/dev/null | head -n 1)"; \
	en="$(EN_ZIP)"; [ -n "$$en" ] || en="$$(ls "$(ASSETS)"/*EnglishPatch*.zip 2>/dev/null | head -n 1)"; \
	[ -f "$$ja" ] && [ -f "$$en" ] || { echo '*** make site-test needs both original archives (JA_ZIP=, EN_ZIP=)'; exit 1; }; \
	printf '\n==> site: the build code in Node (tools/site/build.test.mjs)\n'; \
	node $(ROOT)/tools/site/build.test.mjs "$$ja" "$$en" $(ROOT) || exit 1; \
	browser="$$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)"; \
	if [ -z "$$browser" ]; then echo 'SKIP  page test: no chromium / google-chrome installed'; exit 0; fi; \
	printf '\n==> site: the page in headless %s (tools/site/browser.test.mjs)\n' "$$(basename "$$browser")"; \
	node $(ROOT)/tools/site/browser.test.mjs "$$ja" "$$en" $(ROOT) "$$browser"

site-serve:
	@printf 'Serving site/ at http://localhost:$(SITE_PORT)/  (Ctrl+C to stop)\n'
	$(Q)cd $(ROOT)/site && python3 -m http.server $(SITE_PORT) --bind 127.0.0.1

# build/website: the deployed site (netlify.toml runs the same script).
website:
	$(Q)sh $(ROOT)/tools/site/website.sh

website-serve: website
	@printf 'Serving build/website at http://localhost:$(SITE_PORT)/  (Ctrl+C to stop)\n'
	$(Q)python3 -I $(ROOT)/tools/site/serve.py $(BUILD)/website $(SITE_PORT)

toolchain-windows:
	+$(Q)$(SELF) INNER=1 PLATFORM=windows-x86_64 toolchain

info:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(or $(PLATFORM),$(HOST_PLATFORM)) info-inner

check check-linux:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(HOST_PLATFORM) check-inner
check-windows:
	+$(Q)$(SELF) INNER=1 PLATFORM=windows-x86_64 check-inner
check-macos:
	+$(Q)$(SELF) INNER=1 PLATFORM=$(if $(filter macos,$(HOST_OS)),macos-universal,macos-x86_64) check-inner

clean:
	@printf 'clean: removing objects and intermediate dirs (build/<platform>/, build/host, build/payload, build/lint, build/test)\n'
	$(Q)for d in $(BUILD)/*-*; do case "$$d" in */data.tmp) ;; *) [ -d "$$d" ] && rm -rf "$$d" || true ;; esac; done
	$(Q)rm -rf $(BUILD)/host $(BUILD)/payload $(BUILD)/lint $(BUILD)/test $(BUILD)/test-* $(BUILD)/data.tmp $(BUILD)/valgrind

fclean: clean
	@printf 'fclean: removing dist/ (built games), build/deps (SDL source + builds), build/toolchains (llvm-mingw), build/data (extracted game data)\n'
	$(Q)rm -rf $(DIST) $(BUILD)

re:
	$(Q)$(SELF) clean
	+$(Q)$(SELF) build

# =====================================================================================================================
#  INNER LEVEL (INNER=1 PLATFORM=<os>-<arch>)
# =====================================================================================================================
else

PLATFORM      ?= $(HOST_PLATFORM)
PLATFORM_OS   := $(word 1,$(subst -, ,$(PLATFORM)))
PLATFORM_ARCH := $(word 2,$(subst -, ,$(PLATFORM)))
# SDL=system builds get their own dirs (objects compiled against other SDL headers; not self-contained)
VARIANT       := $(if $(filter system,$(SDL)),-syssdl)$(if $(SAN),-san)$(if $(LIBFUZZER),-lf)$(if $(DEBUG),-debug)$(if $(VG),-vg)
PBUILD        := $(BUILD)/$(PLATFORM)$(VARIANT)
OBJDIR        := $(PBUILD)/obj
DEPS_DIR      := $(BUILD)/deps
SDL_TARBALL   := $(DEPS_DIR)/src/SDL3-$(SDL_VERSION).tar.gz
SDL_SRC       := $(DEPS_DIR)/src/SDL3-$(SDL_VERSION)
SDL_BUILD     := $(DEPS_DIR)/$(PLATFORM)/sdl-build
SDL_PREFIX    := $(DEPS_DIR)/$(PLATFORM)/sdl
TOOLCHAIN_DEP :=
WINDRES       :=
STRIP_CMD     :=
CMAKE_EXTRA   :=
CMAKE_ENV     :=
ARCHFLAGS     :=
PLAT_LDFLAGS  :=
PLAT_OBJS     :=
EXE           :=

# CC is the knob for native builds; cross targets use WINDOWS_CC / MACOS_CC (an exported CC must not leak in)
USER_CC := $(if $(filter default undefined,$(origin CC)),,$(CC))
# DEBUG=1 / VG=1 (the valgrind build): low optimisation so reports map to source lines
CFLAGS  ?= $(if $(DEBUG)$(VG),-O1 -g -fno-omit-frame-pointer,-O2 -g)
LDFLAGS ?=

# ---------------------------------------------------------------------------------------------------- per platform --
ifeq ($(PLATFORM_OS),linux)
  TCC         := $(or $(USER_CC),cc)
  STRIP_CMD   := $(shell command -v strip 2>/dev/null)
  # SDL loads X11/Wayland/ALSA/PulseAudio/PipeWire/udev/dbus/... with dlopen: the binary needs only libc/libm.
  # Optional X11 extensions whose headers are missing are switched off (SDL 3.4 stops with an error otherwise).
  X11_EXT_OFF  = $(foreach m,xtst:XTEST xscrnsaver:XSCRNSAVER xcursor:XCURSOR xi:XINPUT xfixes:XFIXES xrandr:XRANDR,$(if $(shell pkg-config --exists $(word 1,$(subst :, ,$(m))) 2>/dev/null && echo y),,-DSDL_X11_$(word 2,$(subst :, ,$(m)))=OFF))
  CMAKE_EXTRA  = -DSDL_DEPS_SHARED=ON -DSDL_RPATH=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF $(X11_EXT_OFF)
  DIST_DIR    := $(DIST)/$(PLATFORM)$(VARIANT)
  DIST_BIN    := $(DIST_DIR)/sayooshi
  DEVBIN      := $(PBUILD)/sayooshi
else ifeq ($(PLATFORM_OS),windows)
  MINGW_TRIPLE := $(if $(filter arm64,$(PLATFORM_ARCH)),aarch64,$(PLATFORM_ARCH))-w64-mingw32
  LLVM_MINGW_HOST := $(if $(filter macos,$(HOST_OS)),macos-universal,ubuntu-22.04-$(if $(filter arm64,$(HOST_ARCH)),aarch64,x86_64))
  LLVM_MINGW_NAME := llvm-mingw-$(LLVM_MINGW_VERSION)-ucrt-$(LLVM_MINGW_HOST)
  LLVM_MINGW_DIR  := $(BUILD)/toolchains/$(LLVM_MINGW_NAME)
  LLVM_MINGW_SHA  := $(LLVM_MINGW_SHA256_$(if $(filter macos,$(HOST_OS)),macos,$(HOST_PLATFORM)))
  LLVM_MINGW_URL  := https://github.com/mstorsjo/llvm-mingw/releases/download/$(LLVM_MINGW_VERSION)/$(LLVM_MINGW_NAME).tar.xz
  ifeq ($(HOST_OS),windows)
    TCC       := $(or $(WINDOWS_CC),$(USER_CC),gcc)
    WINDRES   := windres
  else ifneq ($(WINDOWS_CC),)
    TCC       := $(WINDOWS_CC)
    WINDRES   := $(or $(shell command -v $(MINGW_TRIPLE)-windres 2>/dev/null),$(shell command -v llvm-windres 2>/dev/null))
  else ifneq ($(shell command -v $(MINGW_TRIPLE)-gcc 2>/dev/null),)
    TCC       := $(MINGW_TRIPLE)-gcc
    WINDRES   := $(MINGW_TRIPLE)-windres
    STRIP_CMD := $(MINGW_TRIPLE)-strip
  else ifneq ($(shell command -v $(MINGW_TRIPLE)-clang 2>/dev/null),)
    TCC       := $(MINGW_TRIPLE)-clang
    WINDRES   := $(MINGW_TRIPLE)-windres
    STRIP_CMD := $(MINGW_TRIPLE)-strip
  else
    TCC           := $(LLVM_MINGW_DIR)/bin/$(MINGW_TRIPLE)-clang
    WINDRES       := $(LLVM_MINGW_DIR)/bin/$(MINGW_TRIPLE)-windres
    STRIP_CMD     := $(LLVM_MINGW_DIR)/bin/$(MINGW_TRIPLE)-strip
    TOOLCHAIN_DEP := $(LLVM_MINGW_DIR)/.stamp
  endif
  ifneq ($(HOST_OS),windows)
    CMAKE_EXTRA := -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=$(if $(filter arm64,$(PLATFORM_ARCH)),ARM64,AMD64) \
                   -DCMAKE_C_COMPILER=$(TCC) -DCMAKE_CXX_COMPILER=$(patsubst %gcc,%g++,$(patsubst %clang,%clang++,$(TCC))) \
                   -DCMAKE_RC_COMPILER=$(WINDRES) -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
                   -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
  endif
  # GUI subsystem (no console window); --console / a parent console still get the log (engine/src/plat/sysdep.c)
  PLAT_LDFLAGS := -static -mwindows -Wl,--no-insert-timestamp   # no link time in the PE header: reproducible
  PLAT_OBJS    := $(PBUILD)/sayooshi_res.o
  EXE          := .exe
  DIST_DIR     := $(DIST)/$(PLATFORM)$(VARIANT)
  DIST_BIN     := $(DIST_DIR)/Sayonara.exe
  DEVBIN       := $(PBUILD)/sayooshi.exe
else ifeq ($(PLATFORM_OS),macos)
  ifeq ($(HOST_OS),macos)
    TCC         := $(or $(MACOS_CC),$(USER_CC),clang)
    MACOS_ARCHS ?= $(if $(filter universal,$(PLATFORM_ARCH)),arm64 x86_64,$(PLATFORM_ARCH))
  else
    TCC         := $(or $(MACOS_CC),$(OSXCROSS_BIN)/o64-clang)
    MACOS_ARCHS ?= $(if $(filter universal,$(PLATFORM_ARCH)),arm64 x86_64,$(PLATFORM_ARCH))
    CMAKE_EXTRA := -DCMAKE_TOOLCHAIN_FILE=$(OSXCROSS_BIN)/../toolchain.cmake
    CMAKE_ENV   := eval "$$($(OSXCROSS_BIN)/osxcross-conf)"; export OSXCROSS_HOST OSXCROSS_TARGET_DIR OSXCROSS_TARGET OSXCROSS_SDK;
  endif
  ARCHFLAGS    := $(foreach a,$(MACOS_ARCHS),-arch $(a)) -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)
  CMAKE_EXTRA  += "-DCMAKE_OSX_ARCHITECTURES=$(subst $(space),;,$(strip $(MACOS_ARCHS)))" \
                  -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOSX_DEPLOYMENT_TARGET)
  DIST_DIR     := $(DIST)/$(PLATFORM)$(VARIANT)
  DIST_APP     := $(DIST_DIR)/Sayonara.app
  DIST_BIN     := $(DIST_APP)/Contents/MacOS/Sayonara
  DEVBIN       := $(PBUILD)/sayooshi
else
  $(error unknown PLATFORM '$(PLATFORM)' (expected linux-<arch>, windows-x86_64, macos-universal))
endif

# ------------------------------------------------------------------------------------------------- SDL selection --
ifeq ($(SDL),system)
  SDL_LIB     :=
  SDL_CFLAGS  := $(shell pkg-config --cflags sdl3 2>/dev/null)
  SDL_LIBS     = $(shell pkg-config --libs sdl3 2>/dev/null)
else
  SDL_LIB     := $(SDL_PREFIX)/lib/libSDL3.a
  SDL_PC      := $(SDL_PREFIX)/lib/pkgconfig/sdl3.pc
  SDL_CFLAGS  := -I$(SDL_PREFIX)/include
  # link flags of the installed sdl3.pc (Libs + Libs.private minus -L/-lSDL3; read at link time, after SDL is built)
  SDL_LIBS     = $(SDL_LIB) $(filter-out -L% -lSDL3 -mwindows,$(shell sed -n 's/^Libs[.a-z]*://p' $(SDL_PC) 2>/dev/null))
endif

# ---------------------------------------------------------------------------------------------------------- flags --
# SAN=1: ASan + UBSan (abort on the first UB report). SAN=<list> is passed to -fsanitize= as is.
# Native dev builds only; the static SDL library itself is not instrumented.
SAN_FLAGS :=
SAN_LIST  :=
ifneq ($(SAN),)
  ifneq ($(PLATFORM_OS),$(HOST_OS))
    $(error SAN=$(SAN) is only supported for native builds (PLATFORM=$(PLATFORM), host $(HOST_PLATFORM)))
  endif
  SAN_LIST     := $(if $(filter 1 yes on,$(SAN)),address$(comma)undefined,$(SAN))
  SAN_FLAGS    := -fsanitize=$(SAN_LIST) -fno-sanitize-recover=undefined -fno-omit-frame-pointer
  PLAT_LDFLAGS += $(SAN_FLAGS)
  STRIP_CMD    :=
endif
# SAYO_DEBUG: memory guards, poisoning, leak report (core/mem.h). SAYO_VALGRIND: memcheck client requests.
DBG_DEFS  := $(if $(DEBUG)$(SAN)$(VG),-DSAYO_DEBUG) $(if $(VG),-DSAYO_VALGRIND)
WARN      := -Wall -Wextra -Wno-unused-parameter
# Source paths in __FILE__ (assert and allocation-site messages) and debug info are made relative to the repository,
# so no build-machine path ends up in a binary and builds are reproducible wherever the tree is checked out.
REPRO     := -ffile-prefix-map=$(ROOT)=.
ALL_CFLAGS := -std=c11 $(ARCHFLAGS) $(REPRO) $(WARN) $(CFLAGS) $(SAN_FLAGS) $(DBG_DEFS) -MMD -MP
ALL_CPPFLAGS := -I$(ENGINE)/src -I$(ENGINE)/third_party/stb $(SDL_CFLAGS) $(CPPFLAGS)
ALL_LDFLAGS := $(ARCHFLAGS) $(PLAT_LDFLAGS) $(LDFLAGS)

# one directory per module (docs/CODE_STANDARD.md §2); objects mirror the tree under $(OBJDIR)
SRCS := $(sort $(wildcard $(ENGINE)/src/*/*.c))
HDRS := $(sort $(wildcard $(ENGINE)/src/*/*.h))
# The vendored fallback font (Kosugi, Apache-2.0) is linked into every engine as a byte array.
FALLBACK_FONT := $(ENGINE)/third_party/fonts/Kosugi-Regular.ttf
OBJS := $(patsubst $(ENGINE)/src/%.c,$(OBJDIR)/%.o,$(SRCS)) $(OBJDIR)/third_party/stb_impl.o \
        $(OBJDIR)/third_party/font_fallback.o
DEPFILES := $(OBJS:.o=.d)

ifeq ($(V),1)
  show =
else
  show = @printf '  %-7s %s\n' '$(1)' '$(patsubst $(ROOT)/%,%,$(2))';
endif

.PHONY: dist sdl toolchain run-dev test-all lint-all format-files vg-check vg-valgrind vg-helgrind vg-drd data-tree payload-file info-inner check-inner check-quiet \
        check-data-quiet devbin devbin-info test-saves-inner fuzz-run FORCE
FORCE:

# ------------------------------------------------------------------------------------------------ requirement checks --
#  check-inner: full report.  check-quiet / check-data-quiet: silent unless something is missing (run before builds).
define SH_CHECK
$(SH_LIB); \
missing=0; warned=0; quiet=$${QUIET:-0}; \
osid=; if [ -r /etc/os-release ]; then osid="$$(. /etc/os-release; echo "$$ID $$ID_LIKE")"; fi; \
case "$(HOST_OS)" in macos) osid=macos;; windows) osid=msys2;; esac; \
row() { [ "$$quiet" = 1 ] && [ "$$1" = ok ] && return 0; printf '  %-9s %-20s %s\n' "[$$1]" "$$2" "$$3"; }; \
hint() { \
  printf '         install it with:\n'; \
  case "$$osid" in *debian*|*ubuntu*) m='>';; *) m=' ';; esac; printf '        %s Debian/Ubuntu: sudo apt install %s\n' "$$m" "$$1"; \
  case "$$osid" in *fedora*|*rhel*|*centos*) m='>';; *) m=' ';; esac; printf '        %s Fedora:        sudo dnf install %s\n' "$$m" "$$2"; \
  case "$$osid" in *arch*|*manjaro*) m='>';; *) m=' ';; esac; printf '        %s Arch:          sudo pacman -S --needed %s\n' "$$m" "$$3"; \
  case "$$osid" in macos) m='>';; *) m=' ';; esac; printf '        %s macOS:         %s\n' "$$m" "$$4"; \
  if [ -n "$$5" ]; then case "$$osid" in msys2) m='>';; *) m=' ';; esac; printf '        %s MSYS2:         pacman -S --needed %s\n' "$$m" "$$5"; fi; }; \
need() { if [ "$$quiet" = 1 ]; then printf '\n'; fi; row MISSING "$$1" "$$2"; missing=$$((missing+1)); }; \
warn() { row warn "$$1" "$$2"; warned=$$((warned+1)); }
endef

define SH_CHECK_DATA
if have 7z; then row ok 7z "$$(7z 2>/dev/null | sed -n 's/^7-Zip[^0-9]*\([0-9][0-9.]*\).*/\1/p' | head -n1) (extracts the original archives)"; \
elif have 7zz; then row ok 7z "7zz (extracts the original archives)"; \
else need 7z "extracts the original game zip and the EN patch installer"; \
  hint "7zip   (older releases: p7zip-full)" "p7zip p7zip-plugins" "7zip" "brew install sevenzip" "p7zip"; fi; \
if [ -n "$(GAMEDATA)" ]; then \
  if [ -f "$(GAMEDATA)/Scene.pck" ]; then row ok "game data" "GAMEDATA=$(GAMEDATA)"; else need "game data" "GAMEDATA=$(GAMEDATA) has no Scene.pck"; fi; \
else \
  ja="$(JA_ZIP)"; [ -n "$$ja" ] || ja="$$(ls "$(ASSETS)"/*JA*Download*.zip 2>/dev/null | head -n 1)"; \
  if [ -n "$$ja" ] && [ -f "$$ja" ]; then row ok "JA game zip" "$$ja"; \
  elif [ -f "$(DATA_STAMP)" ]; then row ok "game data" "$(DATA_DIR) (already built; source archive not found)"; \
  else need "JA game zip" "not found: set JA_ZIP=/path/to/Sayonara-o-Oshiete-...JA_Download-Edition.zip"; \
    printf '         (or GAMEDATA=/path/to/GameData for an already extracted copy; the default looks in %s)\n' "$(ASSETS)"; fi; \
fi; \
if [ -z "$(NO_EN)" ]; then \
  en="$(EN_ZIP)"; [ -n "$$en" ] || en="$$(ls "$(ASSETS)"/*EnglishPatch*.zip 2>/dev/null | head -n 1)"; \
  if [ -n "$$en" ] && [ -f "$$en" ]; then row ok "EN patch zip" "$$en"; \
  elif [ -f "$(DATA_STAMP)" ]; then row ok "EN patch" "(data already built)"; \
  else warn "EN patch zip" "not found (EN_ZIP=...); the build will be Japanese only (NO_EN=1 silences this)"; fi; \
fi
endef

define SH_CHECK_BUILD
if have "$(firstword $(TCC))" || [ -x "$(firstword $(TCC))" ]; then \
  row ok "C compiler" "$(TCC) ($$($(TCC) --version 2>/dev/null | head -n 1))"; \
elif [ -n "$(TOOLCHAIN_DEP)" ]; then \
  row ok "C compiler" "llvm-mingw $(LLVM_MINGW_VERSION) will be downloaded to build/toolchains (or install mingw-w64)"; \
elif [ "$(PLATFORM_OS)" = macos ] && [ "$(HOST_OS)" != macos ]; then \
  need "C compiler" "no macOS toolchain: osxcross (o64-clang) not found; 'make build-macos' explains the options"; \
else need "C compiler" "$(TCC) not found"; \
  case "$(PLATFORM_OS)" in \
    windows) hint "mingw-w64" "mingw64-gcc" "mingw-w64-gcc" "brew install mingw-w64" "mingw-w64-x86_64-gcc";; \
    macos) printf '         on a Mac: xcode-select --install ; on Linux: build osxcross and set OSXCROSS_ROOT\n';; \
    *) hint "build-essential" "gcc make" "base-devel" "xcode-select --install" "mingw-w64-x86_64-gcc make";; \
  esac; fi; \
mv="$$($(MAKE) --version 2>/dev/null | head -n 1)"; \
case "$$mv" in "GNU Make"*) row ok "GNU make" "$${mv#GNU Make }";; *) need "GNU make" "this is not GNU make ($$mv)";; esac; \
if have "$(HOST_CC)"; then row ok "host C compiler" "$(HOST_CC) (pack tool)"; else need "host C compiler" "$(HOST_CC) (builds the pack tool)"; \
  hint "build-essential" "gcc" "base-devel" "xcode-select --install" "gcc"; fi; \
if [ "$(SDL)" = system ]; then \
  if have pkg-config && pkg-config --exists sdl3; then row ok "SDL3 (system)" "$$(pkg-config --modversion sdl3) (dynamic: dist is NOT self-contained)"; \
  else need "SDL3 (system)" "SDL=system needs SDL3 development files + pkg-config"; \
    hint "libsdl3-dev pkg-config" "SDL3-devel pkgconf" "sdl3 pkgconf" "brew install sdl3 pkgconf" "mingw-w64-x86_64-SDL3"; fi; \
elif [ -f "$(SDL_LIB)" ]; then row ok "SDL $(SDL_VERSION)" "static, already built ($(patsubst $(ROOT)/%,%,$(SDL_PREFIX)))"; \
else \
  if have cmake; then row ok cmake "$$(cmake --version | head -n 1 | sed 's/cmake version //') (only builds the pinned SDL $(SDL_VERSION))"; \
  else need cmake "builds the pinned static SDL $(SDL_VERSION) (only needed once)"; \
    hint cmake cmake cmake "brew install cmake" "mingw-w64-x86_64-cmake"; fi; \
  if have curl; then row ok curl "downloads SDL$(if $(TOOLCHAIN_DEP), and llvm-mingw)"; elif have wget; then row ok wget "downloads SDL"; \
  else need "curl or wget" "downloads the pinned SDL source"; hint curl curl curl "(preinstalled)" curl; fi; \
  if have tar; then :; else need tar "unpacks downloads"; fi; \
  if [ "$(PLATFORM_OS)" = linux ]; then \
    if have pkg-config; then \
      miss=; for p in x11 xext xrandr xcursor xi xfixes xscrnsaver xtst wayland-client wayland-egl xkbcommon egl alsa libpulse libpipewire-0.3 dbus-1 libudev libdrm gbm libdecor-0; do \
        pkg-config --exists $$p || miss="$$miss $$p"; done; \
      if [ -z "$$miss" ]; then row ok "SDL backends" "X11, Wayland, ALSA, PulseAudio, PipeWire headers present (loaded at runtime via dlopen)"; \
      else warn "SDL backends" "missing development headers:$$miss"; \
        printf '         SDL is built without the matching feature (a missing x11/wayland/alsa/libpulse/libpipewire\n         backend means no window or no sound on systems that use it). To get every backend:\n'; \
        hint "libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev libwayland-dev libxkbcommon-dev wayland-protocols libegl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libdrm-dev libgbm-dev libdecor-0-dev" \
             "libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel libXScrnSaver-devel libXtst-devel wayland-devel libxkbcommon-devel wayland-protocols-devel mesa-libEGL-devel alsa-lib-devel pulseaudio-libs-devel pipewire-devel dbus-devel systemd-devel libdrm-devel mesa-libgbm-devel libdecor-devel" \
             "libx11 libxext libxrandr libxcursor libxi libxfixes libxss libxtst wayland libxkbcommon wayland-protocols mesa alsa-lib libpulse pipewire dbus systemd-libs libdrm libdecor" "-" ""; fi; \
    else warn pkg-config "optional; used here only to report missing SDL backend headers"; \
      hint pkg-config pkgconf-pkg-config pkgconf "brew install pkgconf" pkgconf; fi; \
  fi; \
fi; \
if [ "$(PLATFORM_OS)" = windows ] && [ "$(HOST_OS)" != windows ] && [ -z "$(TOOLCHAIN_DEP)" ] && [ -z "$(WINDRES)" ]; then \
  need windres "resource compiler for the icon ($(MINGW_TRIPLE)-windres or llvm-windres)"; fi
endef

define SH_CHECK_END
if [ $$missing -gt 0 ]; then printf '\n*** %s requirement(s) missing (see above). Re-run "make check$(if $(filter windows macos,$(PLATFORM_OS)),-$(PLATFORM_OS))" after installing.\n\n' $$missing >&2; exit 1; fi; \
if [ "$$quiet" != 1 ]; then printf '\nAll required tools found%s.\n' "$$( [ $$warned -gt 0 ] && echo " ($$warned warning(s))")"; fi
endef

check-inner:
	@printf 'Requirements for PLATFORM=%s (host %s)\n' '$(PLATFORM)' '$(HOST_PLATFORM)'
	@$(SH_CHECK); $(SH_CHECK_BUILD); $(SH_CHECK_DATA); \
	if have python3; then row ok python3 "$$(python3 --version 2>&1) (make test)"; else warn python3 "only needed by make test"; fi; \
	for t in cppcheck clang-tidy; do if have $$t; then row ok $$t "(make lint)"; else row skip $$t "optional (make lint)"; fi; done; \
	$(SH_CHECK_END)

check-quiet:
	@QUIET=1; $(SH_CHECK); $(SH_CHECK_BUILD); $(SH_CHECK_DATA); $(SH_CHECK_END)

check-data-quiet:
	@QUIET=1; $(SH_CHECK); $(SH_CHECK_DATA); $(SH_CHECK_END)

info-inner:
	@printf '%-22s %s\n' \
	  'host' '$(HOST_PLATFORM) ($(UNAME_S) $(UNAME_M))' \
	  'PLATFORM' '$(PLATFORM)' \
	  'CC' '$(TCC)' \
	  'HOST_CC' '$(HOST_CC)' \
	  'WINDRES' '$(WINDRES)' \
	  'SDL' '$(SDL) $(if $(filter static,$(SDL)),$(SDL_VERSION) -> $(SDL_PREFIX))' \
	  'llvm-mingw' '$(LLVM_MINGW_VERSION) $(if $(TOOLCHAIN_DEP),(will be used: $(LLVM_MINGW_DIR)),(not needed))' \
	  'osxcross' '$(or $(OSXCROSS_BIN),(none))' \
	  'data tree' '$(DATA_DIR)' \
	  'payload' '$(PAYLOAD)' \
	  'dev binary' '$(DEVBIN)' \
	  'dist' '$(DIST_BIN)' \
	  'CFLAGS' '$(ALL_CFLAGS)' \
	  'LDFLAGS' '$(ALL_LDFLAGS)'

# --------------------------------------------------------------------------------------------------- toolchains --
toolchain: $(TOOLCHAIN_DEP)
	@printf 'Windows toolchain: %s\n' '$(TCC)'

$(LLVM_MINGW_DIR)/.stamp:
	@$(SH_LIB); \
	[ -n "$(LLVM_MINGW_SHA)" ] || fail "no pinned llvm-mingw build for host $(HOST_PLATFORM)" "install mingw-w64 (x86_64-w64-mingw32-gcc) instead"; \
	fetch "$(LLVM_MINGW_URL)" "$(BUILD)/toolchains/$(LLVM_MINGW_NAME).tar.xz" "$(LLVM_MINGW_SHA)"; \
	step "unpack $(LLVM_MINGW_NAME)"; rm -rf "$(LLVM_MINGW_DIR)"; \
	tar -xJf "$(BUILD)/toolchains/$(LLVM_MINGW_NAME).tar.xz" -C "$(BUILD)/toolchains"; \
	[ -x "$(TCC)" ] || fail "llvm-mingw unpacked but $(TCC) is missing"; \
	rm -f "$(BUILD)/toolchains/$(LLVM_MINGW_NAME).tar.xz"; touch "$@"

# -------------------------------------------------------------------------------------------------------- SDL3 --
sdl: $(SDL_LIB)

$(SDL_TARBALL):
	@$(SH_LIB); fetch "$(SDL_URL)" "$@" "$(SDL_SHA256)"

$(SDL_SRC)/CMakeLists.txt: $(SDL_TARBALL)
	@$(SH_LIB); step "unpack SDL $(SDL_VERSION)"; rm -rf "$(SDL_SRC)"; tar -xzf "$(SDL_TARBALL)" -C "$(DEPS_DIR)/src"; touch "$@"

$(SDL_PREFIX)/lib/libSDL3.a: $(SDL_SRC)/CMakeLists.txt $(TOOLCHAIN_DEP)
	@$(SH_LIB); step "build static SDL $(SDL_VERSION) for $(PLATFORM) (one time, takes a minute)"; \
	have cmake || fail "cmake is required to build SDL" "run 'make check' for install commands"; \
	rm -rf "$(SDL_BUILD)" "$(SDL_PREFIX)"; $(CMAKE_ENV) \
	cmake -S "$(SDL_SRC)" -B "$(SDL_BUILD)" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
	   -DCMAKE_INSTALL_PREFIX="$(SDL_PREFIX)" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	   -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF \
	   -DCMAKE_C_FLAGS="$(REPRO)" -DCMAKE_OBJC_FLAGS="$(REPRO)" \
	   $(if $(filter macos,$(PLATFORM_OS)),,-DCMAKE_C_COMPILER="$(TCC)") $(CMAKE_EXTRA) \
	   || fail "SDL configure failed" "see the cmake output above"
	+@$(SH_LIB); $(CMAKE_ENV) cmake --build "$(SDL_BUILD)" --config Release \
	   || fail "SDL build failed" "see the cmake output above"; \
	cmake --install "$(SDL_BUILD)" --config Release >/dev/null; \
	[ -f "$@" ] || fail "SDL install did not produce $@"; \
	say "    SDL $(SDL_VERSION) installed to $(patsubst $(ROOT)/%,%,$(SDL_PREFIX))"; touch "$@"

# ------------------------------------------------------------------------------------------------- compilation --
$(OBJDIR)/%.o: $(ENGINE)/src/%.c $(SDL_LIB) $(TOOLCHAIN_DEP)
	@mkdir -p $(@D)
	$(call show,CC,$<)$(TCC) $(ALL_CFLAGS) $(ALL_CPPFLAGS) -c $< -o $@

$(OBJDIR)/third_party/stb_impl.o: $(ENGINE)/third_party/stb_impl.c $(TOOLCHAIN_DEP)
	@mkdir -p $(@D)
	$(call show,CC,$<)$(TCC) -std=c11 $(ARCHFLAGS) $(REPRO) $(CFLAGS) -w -MMD -MP -I$(ENGINE)/third_party/stb -c $< -o $@

$(OBJDIR)/third_party/font_fallback.c: $(FALLBACK_FONT) $(HOSTTOOL)
	@mkdir -p $(@D)
	$(call show,EMBED,$<)$(HOSTTOOL) cembed $< $@ font_fallback_ttf

$(OBJDIR)/third_party/font_fallback.o: $(OBJDIR)/third_party/font_fallback.c $(TOOLCHAIN_DEP)
	$(call show,CC,$<)$(TCC) -std=c11 $(ARCHFLAGS) $(REPRO) $(CFLAGS) -w -c $< -o $@

$(PBUILD)/sayooshi.rc: $(ICON_ICO)
	@mkdir -p $(@D)
	@{ printf '#include <winver.h>\n'; \
	   if [ -s "$(ICON_ICO)" ]; then printf '1 ICON "%s"\n' "$(ICON_ICO)"; fi; \
	   printf 'VS_VERSION_INFO VERSIONINFO\nFILEVERSION 1,0,0,0\nPRODUCTVERSION 1,0,0,0\nFILEOS VOS_NT_WINDOWS32\nFILETYPE VFT_APP\nBEGIN\n'; \
	   printf ' BLOCK "StringFileInfo"\n BEGIN\n  BLOCK "040904b0"\n  BEGIN\n'; \
	   printf '   VALUE "FileDescription", "Sayonara o Oshiete (native engine)\\0"\n   VALUE "ProductName", "Sayonara o Oshiete\\0"\n'; \
	   printf '   VALUE "OriginalFilename", "Sayonara.exe\\0"\n   VALUE "FileVersion", "1.0\\0"\n  END\n END\n'; \
	   printf ' BLOCK "VarFileInfo"\n BEGIN\n  VALUE "Translation", 0x409, 1200\n END\nEND\n'; } > $@

$(PBUILD)/sayooshi_res.o: $(PBUILD)/sayooshi.rc $(TOOLCHAIN_DEP)
	$(call show,WINDRES,$<)$(WINDRES) -O coff -i $< -o $@

$(DEVBIN): $(OBJS) $(PLAT_OBJS) $(SDL_LIB)
	$(call show,LINK,$@)$(TCC) $(ALL_LDFLAGS) -o $@ $(OBJS) $(PLAT_OBJS) $(SDL_LIBS) -lm

devbin: $(DEVBIN)

devbin-info: $(DEVBIN)
	@printf 'Ready: %s%s\n' "$(patsubst $(ROOT)/%,%,$(DEVBIN))" "$(if $(SAN), (sanitizers: $(SAN_LIST)))"

-include $(DEPFILES)

# ----------------------------------------------------------------------------------------------- host pack tool --
# the tool shares the SAYOPAK codec (fmt/sayopak.c) and the checked reader/writer (core/bin.c) with the engine
HOSTTOOL_SRCS := $(ENGINE)/tools/sayopack.c $(ENGINE)/src/core/hash.c $(ENGINE)/src/core/bin.c $(ENGINE)/src/fmt/sayopak.c
HOSTTOOL_HDRS := $(addprefix $(ENGINE)/src/,fmt/sayopak.h core/hash.h core/bin.h core/mem.h core/base.h)
$(HOSTTOOL): $(HOSTTOOL_SRCS) $(HOSTTOOL_HDRS)
	@mkdir -p $(@D)
	$(call show,HOSTCC,$<)$(HOST_CC) -std=c11 -O2 $(REPRO) -Wall -Wextra -I$(ENGINE)/src -I$(ENGINE)/third_party/stb $(HOSTTOOL_SRCS) -o $@

# ------------------------------------------------------------------------------------------------------ data --
#  build/data/{ja,en,manifest.txt}: ja = original GameData files the engine needs, en = EN patch overrides under
#  their original names. Rebuilt only when the sources (JA_ZIP / GAMEDATA, EN_ZIP, FONT) change.
data-tree: $(DATA_STAMP)
	@printf 'data: %s (%s files, %s)\n' "$(patsubst $(ROOT)/%,%,$(DATA_DIR))" "$$(find $(DATA_DIR) -type f ! -name .stamp | wc -l | tr -d ' ')" "$$(du -sh $(DATA_DIR) | cut -f1)"

$(DATA_STAMP): FORCE
	@$(SH_LIB); \
	ja="$(JA_ZIP)"; [ -n "$$ja" ] || ja="$$(ls "$(ASSETS)"/*JA*Download*.zip 2>/dev/null | head -n 1)"; \
	en="$(EN_ZIP)"; [ -n "$$en" ] || en="$$(ls "$(ASSETS)"/*EnglishPatch*.zip 2>/dev/null | head -n 1)"; \
	[ -z "$(NO_EN)" ] || en=; \
	if [ -n "$(GAMEDATA)" ]; then src_id="dir:$(GAMEDATA)"; \
	elif [ -n "$$ja" ] && [ -f "$$ja" ]; then src_id="zip:$$ja:$$(wc -c < "$$ja" | tr -d ' ')"; \
	elif [ -f "$@" ]; then exit 0; \
	else fail "no game data source" "put the JA download-edition zip in $(ASSETS), or pass JA_ZIP=FILE or GAMEDATA=DIR"; fi; \
	if [ -n "$$en" ] && [ -f "$$en" ]; then en_id="zip:$$en:$$(wc -c < "$$en" | tr -d ' ')"; else en=; en_id=none; fi; \
	id="format=2 ja=$$src_id en=$$en_id font=$(or $(FONT),none)"; \
	if [ -f "$@" ] && [ "$$(cat "$@")" = "$$id" ]; then exit 0; fi; \
	have 7z || have 7zz || [ -n "$(GAMEDATA)" ] || fail "7z is required to extract the game archives" "run 'make check' for install commands"; \
	sz() { if have 7z; then 7z "$$@"; else 7zz "$$@"; fi; }; \
	T="$(BUILD)/data.tmp"; rm -rf "$$T"; mkdir -p "$$T/tree/ja" "$$T/x"; \
	if [ -n "$(GAMEDATA)" ]; then \
	   step "data: copy original GameData from $(GAMEDATA)"; src="$(GAMEDATA)"; act="cp -R"; \
	else \
	   step "data: extract $$(basename "$$ja")"; sz x -y -bd -bso0 -bsp0 -mcp=932 -o"$$T/x/ja" "$$ja"; \
	   src="$$(find "$$T/x/ja" -type f -iname Scene.pck | head -n 1)"; src="$${src%/*}"; act="mv"; \
	fi; \
	[ -n "$$src" ] && [ -f "$$src/Scene.pck" ] || fail "no Scene.pck found in the JA game data" "source: $${ja:-$(GAMEDATA)}"; \
	for f in "$$src"/* ; do b="$$(basename "$$f")"; \
	   case "$$(printf '%s' "$$b" | tr 'A-Z' 'a-z')" in \
	     *.exe|savedata|*.ico|*.ini|*.txt|*.dll) ;; \
	     *) $$act "$$f" "$$T/tree/ja/" ;; esac; done; \
	ico="$$(find "$$src" -maxdepth 1 -type f -iname '*.ico' | head -n 1)"; \
	ng="$$(find "$$T/tree/ja" -type f -iname '*.g00' | wc -l | tr -d ' ')"; \
	[ "$$ng" -ge 600 ] || fail "only $$ng g00 files in the JA data (expected 617)" "is this the Sayonara o Oshiete download edition?"; \
	if [ -n "$$en" ]; then \
	   step "data: extract the EN patch $$(basename "$$en")"; \
	   sz x -y -bd -bso0 -bsp0 -o"$$T/x/en1" "$$en"; \
	   inst="$$(find "$$T/x/en1" -type f -iname '*.exe' | head -n 1)"; [ -n "$$inst" ] || fail "no installer .exe in $$en"; \
	   sz x -y -bd -bso0 -bsp0 -o"$$T/x/en2" "$$inst"; \
	   pick() { find "$$T/x/en2" -type f -name "$$1" | head -n 1; }; \
	   mkdir -p "$$T/tree/en/g00" "$$T/tree/en/font"; \
	   for p in Scene.pck Gameexe.dat; do f="$$(pick "$$p.new")"; [ -n "$$f" ] || fail "EN patch: $$p.new not found"; mv "$$f" "$$T/tree/en/$$p"; done; \
	   f="$$(pick 'sayooshieng-Regular.ttf.new')"; [ -n "$$f" ] && mv "$$f" "$$T/tree/en/font/sayooshieng-Regular.ttf"; \
	   f="$$(pick routeguide.txt)"; [ -n "$$f" ] && cp "$$f" "$$T/tree/en/routeguide.txt"; \
	   find "$$T/x/en2" -type f -name '*.g00.new' > "$$T/x/en-g00.txt"; \
	   while IFS= read -r f; do b="$$(basename "$$f" .new)"; mv "$$f" "$$T/tree/en/g00/$$b"; done < "$$T/x/en-g00.txt"; \
	   say "    EN patch: Scene.pck, Gameexe.dat, font, $$(ls "$$T/tree/en/g00" | wc -l | tr -d ' ') g00 overrides"; \
	else say "    (no EN patch: Japanese only)"; fi; \
	if [ -n "$(FONT)" ]; then [ -f "$(FONT)" ] || fail "FONT=$(FONT) does not exist"; cp "$(FONT)" "$$T/tree/ja/font.ttf"; fi; \
	step "data: manifest"; \
	{ printf '# sayooshi data tree. Format 1. Layout: docs/PAYLOAD.md\n'; \
	   printf 'format: 1\n'; \
	   if [ -n "$(GAMEDATA)" ]; then printf 'ja_source: dir\n'; \
	   else printf 'ja_source: sha256=%s\n' "$$(sha256_of "$$ja")"; fi; \
	   if [ -n "$$en" ]; then printf 'en_source: sha256=%s\n' "$$(sha256_of "$$en")"; else printf 'en_source: none\n'; fi; \
	   printf 'languages: ja%s\n' "$$( [ -n "$$en" ] && echo ' en')"; \
	   printf '# sha256 size path\n'; \
	   (cd "$$T/tree" && find ja en -type f 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do \
	      printf '%s %s %s\n' "$$(sha256_of "$$f")" "$$(wc -c < "$$f" | tr -d ' ')" "$$f"; done); \
	} > "$$T/tree/manifest.txt"; \
	rm -rf "$(DATA_DIR)"; mv "$$T/tree" "$(DATA_DIR)"; \
	mkdir -p "$(dir $(ICON_ICO))"; if [ -n "$$ico" ]; then cp "$$ico" "$(ICON_ICO)"; fi; \
	rm -rf "$$T"; printf '%s\n' "$$id" > "$@"; \
	say "    $(patsubst $(ROOT)/%,%,$(DATA_DIR)): $$(find "$(DATA_DIR)" -type f ! -name .stamp | wc -l | tr -d ' ') files, $$(du -sh "$(DATA_DIR)" | cut -f1)"

$(ICON_ICO): $(DATA_STAMP)
	@[ -f $@ ] || { mkdir -p $(@D); : > $@; }

# --------------------------------------------------------------------------------------------------- payload --
payload-file: $(PAYLOAD)

$(PAYLOAD): $(DATA_STAMP) $(HOSTTOOL)
	@mkdir -p $(@D)
	@printf '==> payload: pack %s\n' "$(patsubst $(ROOT)/%,%,$(DATA_DIR))"
	$(Q)cd $(DATA_DIR) && find . -type f ! -name .stamp | sed 's|^\./||' | LC_ALL=C sort > $(BUILD)/payload/files.txt
	$(Q)$(HOSTTOOL) create $@ $(DATA_DIR) $(BUILD)/payload/files.txt

# ------------------------------------------------------------------------------------------------------ dist --
ifeq ($(PLATFORM_OS),macos)
$(PBUILD)/Sayonara.icns: $(ICON_ICO) $(HOSTTOOL)
	$(Q)if [ -s $(ICON_ICO) ]; then $(HOSTTOOL) icns $(ICON_ICO) $@; else : > $@; fi

$(DIST_BIN): $(DEVBIN) $(PAYLOAD) $(PBUILD)/Sayonara.icns
	@printf '==> dist: %s\n' "$(patsubst $(ROOT)/%,%,$(DIST_APP))"
	$(Q)rm -rf $(DIST_APP) && mkdir -p $(DIST_APP)/Contents/MacOS $(DIST_APP)/Contents/Resources
	$(Q)cp $(DEVBIN) $@ && chmod 755 $@
	$(Q)if [ "$(HOST_OS)" = macos ]; then strip -S -x $@; fi  # the debug map holds absolute build paths
	$(Q)cp $(PAYLOAD) $(DIST_APP)/Contents/Resources/game.pak
	$(Q)if [ -s $(PBUILD)/Sayonara.icns ]; then cp $(PBUILD)/Sayonara.icns $(DIST_APP)/Contents/Resources/Sayonara.icns; fi
	$(Q){ printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">\n<dict>\n'; \
	   printf '  <key>CFBundleName</key><string>Sayonara</string>\n  <key>CFBundleDisplayName</key><string>Sayonara o Oshiete</string>\n'; \
	   printf '  <key>CFBundleIdentifier</key><string>org.sayooshi.engine</string>\n  <key>CFBundleExecutable</key><string>Sayonara</string>\n'; \
	   printf '  <key>CFBundleIconFile</key><string>Sayonara</string>\n  <key>CFBundlePackageType</key><string>APPL</string>\n'; \
	   printf '  <key>CFBundleShortVersionString</key><string>1.0</string>\n  <key>CFBundleVersion</key><string>1</string>\n'; \
	   printf '  <key>LSMinimumSystemVersion</key><string>$(MACOSX_DEPLOYMENT_TARGET)</string>\n  <key>NSHighResolutionCapable</key><true/>\n'; \
	   printf '  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>\n</dict>\n</plist>\n'; } > $(DIST_APP)/Contents/Info.plist
	$(Q)if command -v codesign >/dev/null 2>&1; then codesign --force --sign - $(DIST_APP) && echo '    ad-hoc signed (codesign -s -)'; \
	  elif command -v rcodesign >/dev/null 2>&1; then rcodesign sign $(DIST_APP) >/dev/null && echo '    ad-hoc signed (rcodesign)'; \
	  else echo '    note: not signed (no codesign/rcodesign); on Apple Silicon run: codesign --force --sign - Sayonara.app'; fi
	@printf '    %s: binary %s, game.pak %s\n' "$(patsubst $(ROOT)/%,%,$(DIST_APP))" "$$(du -h $@ | cut -f1)" "$$(du -h $(PAYLOAD) | cut -f1)"
else
$(DIST_BIN): $(DEVBIN) $(PAYLOAD) $(HOSTTOOL)
	@printf '==> dist: %s\n' "$(patsubst $(ROOT)/%,%,$@)"
	@mkdir -p $(@D)
	$(Q)cp $(DEVBIN) $(PBUILD)/stripped$(EXE)
	$(Q)if [ -n "$(STRIP_CMD)" ]; then $(STRIP_CMD) $(PBUILD)/stripped$(EXE); fi
	$(Q)$(HOSTTOOL) append $(PBUILD)/stripped$(EXE) $(PAYLOAD) $@
	$(Q)chmod 755 $@ && rm -f $(PBUILD)/stripped$(EXE)
	@printf '    %s: %s bytes (stripped engine %s bytes + payload %s bytes)\n' "$(patsubst $(ROOT)/%,%,$@)" \
	   "$$(wc -c < $@ | tr -d ' ')" "$$(( $$(wc -c < $@) - $$(wc -c < $(PAYLOAD)) ))" "$$(wc -c < $(PAYLOAD) | tr -d ' ')"
endif

dist: $(DIST_BIN)
	@if [ "$(PLATFORM_OS)" = linux ] && command -v readelf >/dev/null 2>&1; then \
	  libs="$$(readelf -d $(DEVBIN) | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"; \
	  extra="$$(printf '%s\n' $$libs | grep -v -E '^(libc|libm|libdl|libpthread|librt|ld-linux.*|libgcc_s)\.so' || true)"; \
	  printf '    shared libraries: %s\n' "$$libs"; \
	  if [ -n "$$extra" ] && [ "$(SDL)" != system ]; then printf '    WARNING: unexpected shared libraries: %s\n' "$$extra"; fi; fi
	@if [ "$(PLATFORM_OS)" = windows ] && command -v objdump >/dev/null 2>&1; then \
	  printf '    DLL imports: %s\n' "$$(objdump -p $(DEVBIN) 2>/dev/null | sed -n 's/.*DLL Name: //p' | sort -u | tr '\n' ' ')"; fi
	@printf '\nReady: %s\n' "$(patsubst $(ROOT)/%,%,$(if $(DIST_APP),$(DIST_APP),$(DIST_BIN)))"

# ------------------------------------------------------------------------------------------------------- run --
run-dev: $(DEVBIN) $(DATA_STAMP)
	@printf '==> run %s %s\n' "$(patsubst $(ROOT)/%,%,$(DEVBIN))" '$(RUN_ARGS)'
	$(DEVBIN) $(RUN_ARGS)

# ------------------------------------------------------------------------------------------------------ test --
test-all: $(DEVBIN) $(if $(SAN),,$(DIST_BIN)) $(DATA_STAMP) $(HOSTTOOL)
	@if [ "$(PLATFORM)" != "$(HOST_PLATFORM)" ]; then echo "test: $(PLATFORM) binaries cannot run on $(HOST_PLATFORM)"; exit 2; fi
	@command -v python3 >/dev/null 2>&1 || { echo '*** make test needs python3 (and bash)'; exit 1; }
	@rm -rf $(BUILD)/test$(VARIANT) && mkdir -p $(BUILD)/test$(VARIANT)
	@printf '\n==> unit selftests (--selftest all)\n'
	$(Q)SAYO_REF_DIR=$(TESTS)/ref $(DEVBIN) $(DATA_DIR)/ja --selftest all --save-dir $(BUILD)/test$(VARIANT)/selftest > $(BUILD)/test$(VARIANT)/selftest.log 2>&1 \
	  || { tail -n 30 $(BUILD)/test$(VARIANT)/selftest.log; echo '*** FAIL: unit selftests (log: build/test$(VARIANT)/selftest.log)'; exit 1; }
	@grep -E '^SELFTEST|selftest.*(PASS|FAIL)' $(BUILD)/test$(VARIANT)/selftest.log | head -n 20; echo 'PASS  unit selftests'
	$(Q)fail=; for m in m0 m1 m1_5 m2 m3 en mem; do \
	  if [ "$$m" = en ]; then printf '\n==> English edition + language selector (engine/tests/run_en.sh)\n'; \
	  elif [ "$$m" = mem ]; then printf '\n==> memory budgets and heap churn (engine/tests/run_mem.sh)\n'; \
	  else printf '\n==> milestone %s (engine/tests/run_%s.sh)\n' "$$m" "$$m"; fi; \
	  if SAYO_BIN=$(DEVBIN) SAYO_GAMEDATA=$(DATA_DIR)/ja SAYO_OUT=$(BUILD)/test$(VARIANT)/$$m bash $(TESTS)/run_$$m.sh > $(BUILD)/test$(VARIANT)/$$m.log 2>&1; \
	  then tail -n 1 $(BUILD)/test$(VARIANT)/$$m.log; else tail -n 25 $(BUILD)/test$(VARIANT)/$$m.log; fail="$$fail $$m"; fi; done; \
	[ -z "$$fail" ] || { printf '\n*** FAIL:%s (logs in build/test$(VARIANT)/)\n' "$$fail"; exit 1; }
ifeq ($(SAN),)
	@printf '\n==> payload smoke test (engine/tests/run_payload.sh)\n'
	$(Q)SAYO_DIST=$(or $(DIST_APP),$(DIST_BIN)) SAYO_DEVBIN=$(DEVBIN) SAYO_PACK=$(HOSTTOOL) SAYO_OUT=$(BUILD)/test$(VARIANT)/payload $(if $(filter system,$(SDL)),SAYO_ALLOW_DYNAMIC=1) \
	  bash $(TESTS)/run_payload.sh
else
	@printf '\n(SAN=%s: payload smoke test skipped; it checks the stripped dist file)\n' '$(SAN)'
endif
	@printf '\nAll tests passed.\n'

# save format 2 acceptance (engine/tests/run_saves.sh): format-1 migration (old engine built from git rev 0155be2
# with cmake), .bak fallback, export/import, portable mode, golden bytes, fuzz on an ASan/UBSan build, M3.
# The sanitizer binary is SAYO_ASAN_BIN if set, else this make's SAN=1 dev binary (built on demand).
SAN_DEVBIN := $(BUILD)/$(PLATFORM)$(if $(filter system,$(SDL)),-syssdl)-san/sayooshi$(EXE)
test-saves-inner: $(DEVBIN) $(DATA_STAMP)
	@command -v python3 >/dev/null 2>&1 || { echo '*** make test-saves needs python3 (and bash)'; exit 1; }
	+$(Q)if [ -z "$$SAYO_ASAN_BIN" ]; then $(SELF) INNER=1 PLATFORM=$(PLATFORM) SAN=1 devbin; fi
	$(Q)rm -rf $(BUILD)/test$(VARIANT)/saves && mkdir -p $(BUILD)/test$(VARIANT)/saves
	$(Q)SAYO_BIN=$(DEVBIN) SAYO_GAMEDATA=$(DATA_DIR)/ja SAYO_OUT=$(BUILD)/test$(VARIANT)/saves \
	  SAYO_ASAN_BIN="$${SAYO_ASAN_BIN:-$(SAN_DEVBIN)}" bash $(TESTS)/run_saves.sh

# ------------------------------------------------------------------------------------------------------ fuzz --
#  make fuzz: engine/tests/fuzz (fuzz.h) over the core/fmt/plat objects of this build (SAN=1 by default), stb
#  compiled with the same sanitizers. Inputs that once failed (engine/tests/fuzz/regress/<target>/; recipes rebuilt from the data) run first.
#  A finding stops the run and leaves build/fuzz/crash-<target>.bin
#  (replay: build/<platform>-san/fuzz --replay FILE --target NAME). LIBFUZZER=1: one target per run, corpus in
#  build/fuzz/corpus/<target> (first run seeded from the game data), findings as build/fuzz/<target>-crash-*.
FUZZ_TIME   ?= 120
FUZZ_TARGET ?=
FUZZ_SEED   ?=
FUZZ_DIR    := $(BUILD)/fuzz
FUZZ_OBJDIR := $(PBUILD)/fuzzobj
FUZZ_ENGINE := $(filter $(OBJDIR)/core/% $(OBJDIR)/fmt/%,$(OBJS)) $(OBJDIR)/plat/fs.o $(OBJDIR)/plat/sysdep.o \
               $(OBJDIR)/txt/font_sfnt.o \
               $(FUZZ_OBJDIR)/stb_impl.o
FUZZ_BIN    := $(PBUILD)/fuzz$(EXE)
FUZZ_LF_BIN := $(PBUILD)/fuzz-lf$(EXE)

$(FUZZ_OBJDIR)/%.o: $(TESTS)/fuzz/%.c $(TESTS)/fuzz/fuzz.h $(SDL_LIB)
	@mkdir -p $(@D)
	$(call show,CC,$<)$(TCC) $(ALL_CFLAGS) $(ALL_CPPFLAGS) -c $< -o $@

$(FUZZ_OBJDIR)/stb_impl.o: $(ENGINE)/third_party/stb_impl.c $(wildcard $(ENGINE)/third_party/stb/*)
	@mkdir -p $(@D)
	$(call show,CC,$<)$(TCC) -std=c11 $(ARCHFLAGS) $(REPRO) $(CFLAGS) $(SAN_FLAGS) -w -I$(ENGINE)/third_party/stb -c $< -o $@

$(FUZZ_BIN): $(FUZZ_OBJDIR)/fuzz_main.o $(FUZZ_OBJDIR)/fuzz_targets.o $(FUZZ_ENGINE) $(SDL_LIB)
	$(call show,LINK,$@)$(TCC) $(ALL_LDFLAGS) -o $@ $(filter %.o,$^) $(SDL_LIBS) -lm

$(FUZZ_LF_BIN): $(FUZZ_OBJDIR)/fuzz_libfuzzer.o $(FUZZ_OBJDIR)/fuzz_targets.o $(FUZZ_ENGINE) $(SDL_LIB)
	$(call show,LINK,$@)$(TCC) $(ALL_LDFLAGS) -fsanitize=fuzzer -o $@ $(filter %.o,$^) $(SDL_LIBS) -lm

# Regression inputs cut from game files are kept as recipes (tools/fuzz_regress.py) and rebuilt from build/data here.
FUZZ_REGRESS := $(FUZZ_DIR)/regress
fuzz-run: $(if $(LIBFUZZER),$(FUZZ_LF_BIN),$(FUZZ_BIN)) $(DATA_STAMP)
	@mkdir -p $(FUZZ_DIR)
	$(Q)rm -rf $(FUZZ_REGRESS) && python3 -I $(ROOT)/tools/fuzz_regress.py build $(TESTS)/fuzz/regress $(DATA_DIR) $(FUZZ_REGRESS)
ifeq ($(LIBFUZZER),)
	$(Q)ASAN_OPTIONS="abort_on_error=1:$$ASAN_OPTIONS" UBSAN_OPTIONS="abort_on_error=1:print_stacktrace=1:$$UBSAN_OPTIONS" \
	  $(FUZZ_BIN) --data $(DATA_DIR) --regress $(FUZZ_REGRESS) --out $(FUZZ_DIR) --time $(FUZZ_TIME) \
	  $(if $(FUZZ_TARGET),--target $(FUZZ_TARGET)) \
	  $(if $(FUZZ_SEED),--seed $(FUZZ_SEED))
else
	@[ -n "$(FUZZ_TARGET)" ] || { echo '*** make fuzz LIBFUZZER=1 needs FUZZ_TARGET=<name> (see: $(FUZZ_BIN) --list)'; exit 2; }
	@mkdir -p $(FUZZ_DIR)/corpus/$(FUZZ_TARGET)
	$(Q)c=$(FUZZ_DIR)/corpus/$(FUZZ_TARGET); seeds=; [ -n "$$(ls -A $$c)" ] || seeds=$$c; \
	  FUZZ_TARGET=$(FUZZ_TARGET) FUZZ_DATA=$(DATA_DIR) FUZZ_SEED_DIR=$$seeds FUZZ_TMP=$(FUZZ_DIR) \
	  $(FUZZ_LF_BIN) -max_total_time=$(FUZZ_TIME) -artifact_prefix=$(FUZZ_DIR)/$(FUZZ_TARGET)- $$c \
	  $$([ -d $(FUZZ_REGRESS)/$(FUZZ_TARGET) ] && echo $(FUZZ_REGRESS)/$(FUZZ_TARGET))
endif

# ------------------------------------------------------------------------------------------------------ lint --
LINT_WARN := -std=c11 -Werror -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wstrict-prototypes \
             -Wmissing-prototypes -Wformat -Wformat-security -Wundef -Wpointer-arith -Wvla -Wcast-align
LINT_SRCS := $(SRCS) $(ENGINE)/tools/sayopack.c
LINT_CCS  := $(foreach c,gcc clang,$(if $(shell command -v $(c) 2>/dev/null),$(c)))
LINT_INC  := -I$(ENGINE)/src -I$(ENGINE)/third_party/stb $(SDL_CFLAGS)
LINT_OK   := $(foreach c,$(LINT_CCS),$(patsubst $(ENGINE)/%.c,$(BUILD)/lint/$(c)/%.ok,$(LINT_SRCS)))
TIDY      := $(shell command -v clang-tidy 2>/dev/null)
CPPCHECK  := $(shell command -v cppcheck 2>/dev/null)
CLANG_FORMAT := $(shell command -v clang-format 2>/dev/null)
LINT_TOOLS := $(ROOT)/tools/lint
LINT_BASE  := $(LINT_TOOLS)/baseline
# clang-tidy checks live in $(ROOT)/.clang-tidy; findings are ratcheted per module (tools/lint/tidy_ratchet.sh)
TIDY_LOG  := $(if $(TIDY),$(patsubst $(ENGINE)/%.c,$(BUILD)/lint/tidy/%.log,$(LINT_SRCS)))

$(BUILD)/lint/gcc/%.ok: $(ENGINE)/%.c $(SDL_LIB)
	@mkdir -p $(@D)
	$(call show,GCC,$<)gcc $(LINT_WARN) $(LINT_INC) -fsyntax-only $< && touch $@

$(BUILD)/lint/clang/%.ok: $(ENGINE)/%.c $(SDL_LIB)
	@mkdir -p $(@D)
	$(call show,CLANG,$<)clang $(LINT_WARN) $(LINT_INC) -fsyntax-only $< && touch $@

$(BUILD)/lint/tidy/%.log: $(ENGINE)/%.c $(HDRS) $(ROOT)/.clang-tidy $(SDL_LIB)
	@mkdir -p $(@D)
	$(call show,TIDY,$<)$(TIDY) -quiet $< -- -std=c11 -DSDL_DISABLE_ANALYZE_MACROS $(LINT_INC) > $@.tmp 2>/dev/null; \
	  if grep -q ': error: ' $@.tmp; then grep ': error: ' $@.tmp | head -n 5; rm -f $@.tmp; exit 1; fi; mv $@.tmp $@

$(BUILD)/lint/cppcheck.ok: $(LINT_SRCS) $(HDRS)
	@mkdir -p $(@D)
	$(call show,CPPCHK,engine/src engine/tools)$(CPPCHECK) -q --std=c11 --enable=warning,portability --error-exitcode=1 \
	  --inline-suppr -j$(NPROC) -I$(ENGINE)/src --suppress=missingIncludeSystem --suppress=nullPointerOutOfMemory \
	  --suppress=nullPointerArithmeticOutOfMemory --suppress=ctunullpointerOutOfMemory --suppress=normalCheckLevelMaxBranches $(ENGINE)/src $(ENGINE)/tools && touch $@

# lint-all: compiler -Werror builds, cppcheck, clang-tidy (ratcheted), banned calls (ratcheted), exported-symbol
# prefixes (on the objects of the native build), module layers of the includes, and a function/file length report.
# docs/CODE_STANDARD.md §9.
lint-all: $(LINT_OK) $(TIDY_LOG) $(if $(CPPCHECK),$(BUILD)/lint/cppcheck.ok) $(OBJS)
	@[ -n "$(LINT_CCS)" ] || { echo '*** lint: neither gcc nor clang found'; exit 1; }
	@printf 'lint: -Werror builds passed with: %s\n' '$(LINT_CCS)'
	@for c in gcc clang; do case " $(LINT_CCS) " in *" $$c "*) ;; *) echo "lint: $$c not installed - skipped";; esac; done
	@if [ -n "$(CPPCHECK)" ]; then echo 'lint: cppcheck passed'; else echo 'lint: cppcheck not installed - skipped'; fi
	@for l in $$(find $(BUILD)/lint/tidy -name '*.log' 2>/dev/null); do \
	  case " $(TIDY_LOG) " in *" $$l "*) ;; *) rm -f "$$l";; esac; done   # logs of removed sources
	@if [ -n "$(TIDY)" ]; then sh $(LINT_TOOLS)/tidy_ratchet.sh $(BUILD)/lint/tidy $(LINT_BASE); else echo 'lint: clang-tidy not installed - skipped'; fi
	@sh $(LINT_TOOLS)/banned.sh $(ENGINE)/src $(LINT_BASE)
	@for o in $$(find $(OBJDIR) -mindepth 2 -name '*.o' 2>/dev/null); do \
	  case " $(OBJS) " in *" $$o "*) ;; *) rm -f "$$o" "$${o%.o}.d";; esac; done   # objects of removed sources
	@sh $(LINT_TOOLS)/prefix.sh $(OBJDIR)
	@sh $(LINT_TOOLS)/layers.sh $(ENGINE)/src $(LINT_TOOLS)/layers.txt
	@sh $(LINT_TOOLS)/lengths.sh $(ENGINE)/src
	@if [ -n "$(CLANG_FORMAT)" ]; then echo 'lint: formatting is advisory: make format FILES="..." (see docs/CODE_STANDARD.md §9)'; fi

# make format FILES="engine/src/core/str.c ..." re-formats the named files with .clang-format (never the whole tree
# at once: the style is applied when a file is reworked, so history stays readable).
format-files:
	@[ -n "$(CLANG_FORMAT)" ] || { echo '*** make format needs clang-format'; exit 1; }
	@[ -n "$(FILES)" ] || { echo '*** make format FILES="engine/src/<module>/<file>.c ..."'; exit 1; }
	$(CLANG_FORMAT) -i --style=file $(FILES)

# ---------------------------------------------------------------------------------------------------- valgrind --
#  make valgrind | helgrind | drd: the VG=1 build (-O1 -g, SAYO_DEBUG + SAYO_VALGRIND, static SDL) under memcheck,
#  helgrind or drd. Runs: unit selftests, seen101 to TEXT 100 in ja and en, the save/load route, the language
#  selector, sayopack verify. helgrind/drd runs open the audio device on SDL's dummy driver (--audio-dummy) so the
#  device callback thread runs next to the game thread. Logs: build/valgrind/<tool>/<run>.log; summary at the end;
#  any reported error fails the target (exit 99 per run). Parallel: make valgrind -j6.
VALGRIND  := $(shell command -v valgrind 2>/dev/null)
VG_OUT    := $(BUILD)/valgrind
VG_PACK   := $(VG_OUT)/sayopack$(HOST_EXE)
VG_SUPP   := $(ROOT)/tools/valgrind/sayooshi.supp
VG_RUNS   := selftest ja101 en101 save load langsel sayopack
VG_COMMON := --error-exitcode=99 --num-callers=40 --fullpath-after=engine/src/ --suppressions=$(VG_SUPP) $(VG_FLAGS)
VG_TOOL_memcheck := --tool=memcheck --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --track-origins=yes
VG_TOOL_helgrind := --tool=helgrind
VG_TOOL_drd      := --tool=drd
VG_THREADS_memcheck :=
VG_THREADS_helgrind := --audio-dummy
VG_THREADS_drd      := --audio-dummy
# $(1) = output dir of the tool, $(2) = thread option
VG_ARGS_selftest = $(DATA_DIR)/ja --selftest all --save-dir $(1)/selftest.save
VG_ARGS_ja101    = $(DATA_DIR) --lang ja --headless --scene seen101 --until-text 100 --auto-advance 300 $(2) --save-dir $(1)/ja101.save
VG_ARGS_en101    = $(DATA_DIR) --lang en --headless --scene seen101 --until-text 100 --auto-advance 300 $(2) --save-dir $(1)/en101.save
VG_ARGS_save     = $(DATA_DIR) --lang ja --headless --scene seen101 --auto-advance 300 --until-text 45 --save-at-text 40=1 $(2) --save-dir $(1)/route.save
VG_ARGS_load     = $(DATA_DIR) --lang ja --headless --load 1 --auto-advance 300 --until-text 15 $(2) --save-dir $(1)/route.save
VG_ARGS_langsel  = $(DATA_DIR) --headless --select-language --frames 700 --until-text 5 $(2) --save-dir $(1)/langsel.save
VG_ARGS_sayopack = verify $(PAYLOAD)
VG_ENV_selftest  = SAYO_REF_DIR=$(TESTS)/ref

$(VG_PACK): $(HOSTTOOL_SRCS) $(HOSTTOOL_HDRS)
	@mkdir -p $(@D)
	$(call show,HOSTCC,$@)$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -I$(ENGINE)/src -I$(ENGINE)/third_party/stb $(HOSTTOOL_SRCS) -o $@

vg-check:
	@[ -n "$(VALGRIND)" ] || { echo '*** valgrind is not installed (Debian/Ubuntu: apt install valgrind)'; exit 1; }
	@[ "$(PLATFORM)" = "$(HOST_PLATFORM)" ] || { echo '*** valgrind runs native builds only'; exit 2; }

define VG_RULE
$(VG_OUT)/$(1)/$(2).log: vg-check $(if $(filter sayopack,$(2)),$(VG_PACK) $(PAYLOAD),$(DEVBIN) $(DATA_STAMP)) $(if $(filter load,$(2)),$(VG_OUT)/$(1)/save.log) FORCE
	@mkdir -p $$(@D)
	@printf '  %-9s %s\n' '$(1)' '$(2)'
	@rm -rf $(VG_OUT)/$(1)/$(2).save; $(if $(filter save,$(2)),rm -rf $(VG_OUT)/$(1)/route.save;) \
	  $(VG_ENV_$(2)) $(VALGRIND) $(VG_COMMON) $(VG_TOOL_$(1)) --log-file=$$@.tmp \
	  $(if $(filter sayopack,$(2)),$(VG_PACK),$(DEVBIN)) $(call VG_ARGS_$(2),$(VG_OUT)/$(1),$(VG_THREADS_$(1))) \
	  > $(VG_OUT)/$(1)/$(2).out 2>&1; echo $$$$? > $(VG_OUT)/$(1)/$(2).rc; mv $$@.tmp $$@
endef
$(foreach t,memcheck helgrind drd,$(foreach r,$(VG_RUNS),$(eval $(call VG_RULE,$(t),$(r)))))

vg-valgrind: $(foreach r,$(VG_RUNS),$(VG_OUT)/memcheck/$(r).log)
	@sh $(ROOT)/tools/valgrind/summary.sh $(VG_OUT) memcheck
vg-helgrind: $(foreach r,$(VG_RUNS),$(VG_OUT)/helgrind/$(r).log)
	@sh $(ROOT)/tools/valgrind/summary.sh $(VG_OUT) helgrind
vg-drd: $(foreach r,$(VG_RUNS),$(VG_OUT)/drd/$(r).log)
	@sh $(ROOT)/tools/valgrind/summary.sh $(VG_OUT) drd

endif
