SHELL := /bin/bash

CC ?= cc
CSTD := -std=c11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
CDEBUG ?= -g -O0
BUILD ?= build
CFLAGS_PLATFORM ?=
PLATFORM ?= mac
MLP1_TOOLCHAIN_IMAGE ?= ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local
WORKSPACE_ROOT ?= $(abspath ..)

ifeq ($(PLATFORM),mlp1)
CFLAGS_PLATFORM += -DPLATFORM_MLP1
endif

DEFAULT_CATASTROPHE_DIR := $(if $(wildcard ../Catastrophe/include/catastrophe.h),$(abspath ../Catastrophe),third_party/catastrophe)
CATASTROPHE_DIR ?= $(DEFAULT_CATASTROPHE_DIR)
CATASTROPHE_INCLUDE := $(CATASTROPHE_DIR)/include
CATASTROPHE_HEADER := $(CATASTROPHE_INCLUDE)/catastrophe.h
CATASTROPHE_RES := $(CATASTROPHE_DIR)/res

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image)
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client 2>/dev/null)
WAYLAND_LDFLAGS := $(shell pkg-config --libs wayland-client 2>/dev/null)
WAYLAND_PROTOCOLS_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)
WAYLAND_SCANNER ?= wayland-scanner

CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) $(CFLAGS_PLATFORM) -I. -Iinternal -Ithird_party/cjson
CFLAGS_DAEMON := $(CFLAGS_COMMON)
CFLAGS_UI := $(CFLAGS_COMMON) -I$(CATASTROPHE_INCLUDE) $(SDL_CFLAGS)
LDLIBS_COMMON := -lsqlite3
LDLIBS_DAEMON := $(LDLIBS_COMMON)
LDLIBS_UI := $(LDLIBS_COMMON) $(SDL_LDFLAGS) -lm -lpthread
ifeq ($(shell uname -s),Darwin)
LDLIBS_UI += -lobjc
endif
ifeq ($(PLATFORM),mlp1)
# MLP1 dismisses the stock boot transition from jawakad's platform backend by
# dlopen-ing libloong_sdk.so at runtime, which needs libdl.
LDLIBS_DAEMON += -ldl
PLATFORM_BACKEND_SRC := internal/platform/device_mlp1.c
PLATFORM_ID_SRC := internal/platform/platform_id_mlp1.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mlp1.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_wayland.c $(BUILD)/generated/xdg-shell-protocol.c
OSD_DEPS := $(BUILD)/generated/xdg-shell-client-protocol.h
OSD_CFLAGS := $(CFLAGS_COMMON) $(WAYLAND_CFLAGS) -I$(BUILD)/generated
OSD_LDLIBS := $(LDLIBS_COMMON) $(WAYLAND_LDFLAGS)
else
PLATFORM_BACKEND_SRC := internal/platform/device_mock.c
PLATFORM_ID_SRC := internal/platform/platform_id_mock.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mock.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_sdl.c
OSD_DEPS :=
OSD_CFLAGS := $(CFLAGS_UI)
OSD_LDLIBS := $(LDLIBS_UI)
endif
PLATFORM_COMMON_SRC := internal/platform/platform_common.c

DAEMON_SRCS := \
	cmd/jawakad/main.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	$(PLATFORM_COMMON_SRC) \
	internal/platform/device.c \
	$(PLATFORM_BACKEND_SRC) \
	$(PLATFORM_ID_SRC) \
	$(INPUT_PROXY_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	internal/retroarch/command.c \
	internal/db/db.c \
	internal/discovery/discovery.c \
	third_party/cjson/cJSON.c

RETROARCH_CTL_SRCS := \
	cmd/jawaka-retroarchctl/main.c \
	internal/retroarch/command.c

RETROARCH_RUNNER_SRCS := \
	cmd/jawaka-retroarch-runner/main.c \
	internal/core/log.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

PLATFORM_CTL_SRCS := \
	cmd/jawaka-platformctl/main.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

OSD_SRCS := \
	cmd/jawaka-osd/main.c \
	$(OSD_BACKEND_SRC) \
	internal/core/log.c \
	internal/ipc/ipc.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

SCAN_SMOKE_SRCS := \
	cmd/jawaka-scan-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/discovery/discovery.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

UI_SRCS := \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	$(PLATFORM_COMMON_SRC) \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	internal/db/db.c \
	internal/launcher/console_colors.c \
	internal/settings/settings.c \
	internal/settings/theme_resolve.c \
	third_party/cjson/cJSON.c

.PHONY: all jawakad jawaka-launcher jawaka-menu jawaka-osd jawaka-retroarchctl jawaka-retroarch-runner jawaka-platformctl jawaka-scan-smoke mockgen run-daemon run-daemon-interactive run-daemon-only run-launcher run-menu run-interactive clean help tg5040 tg5050 my355 mlp1 mlp1-adb-smoke mlp1-adb-input-capture mlp1-adb-ra-command-smoke phase3-fixture-scan-smoke check-catastrophe check-sdl

all: $(BUILD)/bin/jawakad $(BUILD)/bin/jawaka-launcher $(BUILD)/bin/jawaka-menu $(BUILD)/bin/jawaka-osd $(BUILD)/bin/jawaka-retroarchctl $(BUILD)/bin/jawaka-retroarch-runner $(BUILD)/bin/jawaka-platformctl

jawakad: $(BUILD)/bin/jawakad
jawaka-launcher: $(BUILD)/bin/jawaka-launcher
jawaka-menu: $(BUILD)/bin/jawaka-menu
jawaka-osd: $(BUILD)/bin/jawaka-osd
jawaka-retroarchctl: $(BUILD)/bin/jawaka-retroarchctl
jawaka-retroarch-runner: $(BUILD)/bin/jawaka-retroarch-runner
jawaka-platformctl: $(BUILD)/bin/jawaka-platformctl
jawaka-scan-smoke: $(BUILD)/bin/jawaka-scan-smoke

$(BUILD)/bin:
	@mkdir -p $(BUILD)/bin

check-catastrophe:
	@test -f "$(CATASTROPHE_HEADER)" || \
		( echo "Catastrophe headers not found. Set CATASTROPHE_DIR=/path/to/Catastrophe and retry." && exit 1 )

check-sdl:
	@pkg-config --exists sdl2 SDL2_ttf SDL2_image 2>/dev/null || \
		( echo "SDL2 libraries not found. Install with: brew install sdl2 sdl2_ttf sdl2_image" && exit 1 )

$(BUILD)/bin/jawakad: $(DAEMON_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_DAEMON) -o $@ $(DAEMON_SRCS) $(LDLIBS_DAEMON)

$(BUILD)/bin/jawaka-launcher: cmd/jawaka-launcher/main.c $(UI_SRCS) $(CATASTROPHE_HEADER) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-launcher/main.c $(UI_SRCS) $(LDLIBS_UI)

$(BUILD)/bin/jawaka-menu: cmd/jawaka-menu/main.c $(UI_SRCS) $(CATASTROPHE_HEADER) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-menu/main.c $(UI_SRCS) $(LDLIBS_UI)

$(BUILD)/generated:
	@mkdir -p $(BUILD)/generated

$(BUILD)/generated/xdg-shell-client-protocol.h: | $(BUILD)/generated
	@test -n "$(WAYLAND_PROTOCOLS_DIR)" || { echo "wayland-protocols pkg-config data dir missing" >&2; exit 1; }
	$(WAYLAND_SCANNER) client-header "$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml" $@

$(BUILD)/generated/xdg-shell-protocol.c: $(BUILD)/generated/xdg-shell-client-protocol.h
	@test -n "$(WAYLAND_PROTOCOLS_DIR)" || { echo "wayland-protocols pkg-config data dir missing" >&2; exit 1; }
	$(WAYLAND_SCANNER) private-code "$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml" $@

$(BUILD)/bin/jawaka-osd: $(OSD_SRCS) $(OSD_DEPS) $(CATASTROPHE_HEADER) | $(BUILD)/bin
	$(CC) $(OSD_CFLAGS) -o $@ $(OSD_SRCS) $(OSD_LDLIBS)

$(BUILD)/bin/jawaka-retroarchctl: $(RETROARCH_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(RETROARCH_CTL_SRCS)

$(BUILD)/bin/jawaka-retroarch-runner: $(RETROARCH_RUNNER_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(RETROARCH_RUNNER_SRCS)

$(BUILD)/bin/jawaka-platformctl: $(PLATFORM_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(PLATFORM_CTL_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-scan-smoke: $(SCAN_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(SCAN_SMOKE_SRCS) $(LDLIBS_COMMON)

phase3-fixture-scan-smoke:
	scripts/phase3-fixture-scan-smoke.sh

mockgen:
	bash scripts/mockgen.sh

run-daemon: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO="$${JAWAKA_AUTODEMO:-1}" \
	JAWAKA_AUTODEMO_DELAY_MS="$${JAWAKA_AUTODEMO_DELAY_MS:-1200}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad

run-daemon-interactive: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO=0 \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad

run-daemon-only: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad --daemon-only

run-launcher: $(BUILD)/bin/jawaka-launcher mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawaka-launcher

run-interactive: run-daemon-interactive

run-menu: $(BUILD)/bin/jawaka-menu mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawaka-menu

clean:
	rm -rf $(BUILD)

tg5040 tg5050 my355:
	$(MAKE) -C ports/$@ all

mlp1:
	docker run --rm \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile all

mlp1-adb-smoke:
	scripts/adb-mlp1-smoke.sh

mlp1-adb-input-capture:
	scripts/adb-mlp1-input-capture.sh

mlp1-adb-ra-command-smoke:
	scripts/adb-mlp1-retroarch-command-smoke.sh

help:
	@echo ""
	@echo "Jawaka build targets"
	@echo "===================="
	@echo "  make               Build jawakad, jawaka-launcher, jawaka-menu"
	@echo "  make mockgen       Create/update the mock SD-card tree"
	@echo "  make run-daemon              Run the daemon-driven phase-0/1 demo (auto-transitions)"
	@echo "  make run-daemon-interactive  Run daemon without auto-demo (stays open for testing)"
	@echo "  make run-daemon-only         Run jawakad without spawning launcher/menu"
	@echo "  make run-launcher            Run jawaka-launcher directly (requires daemon)"
	@echo "  make run-interactive         Alias for run-daemon-interactive"
	@echo "  make run-menu                Run jawaka-menu directly"
	@echo "  make jawaka-osd              Build the daemon-owned brightness OSD"
	@echo "  make jawaka-platformctl      Build platform status/control helper"
	@echo "  make jawaka-retroarch-runner Build RetroArch app/config runner"
	@echo "  make clean         Remove build artifacts"
	@echo "  make tg5040        Placeholder cross-compile target"
	@echo "  make tg5050        Placeholder cross-compile target"
	@echo "  make my355         Placeholder cross-compile target"
	@echo "  make mlp1          Cross-compile for Miniloong Pocket 1"
	@echo "  make mlp1-adb-smoke  Build, push to /tmp, and run an ADB UI smoke"
	@echo "  make mlp1-adb-input-capture  Record Loong Gamepad evtest labels over ADB"
	@echo "  make mlp1-adb-ra-command-smoke  Run RetroArch command feature smoke over ADB"
	@echo "  make phase3-fixture-scan-smoke  Run metadata-aware scan fixture checks"
	@echo ""
	@echo "Environment variables"
	@echo "====================="
	@echo "  JAWAKA_SDCARD_ROOT            Path to SD-card root (default: ./mock-sdcard)"
	@echo "  JAWAKA_AUTODEMO=1             Auto-transition launcher→menu→shutdown (default in run-daemon)"
	@echo "  JAWAKA_AUTODEMO_DELAY_MS=N    Delay before auto-demo action fires, ms (default: 1200)"
	@echo "  JAWAKA_THEME=Jawaka-Tabs      Launcher theme: Jawaka-Tabs, Jawaka-Vertical, Jawaka-Horizontal"
	@echo ""
	@echo "Catastrophe include root: $(CATASTROPHE_INCLUDE)"
	@echo ""
