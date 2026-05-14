SHELL := /bin/bash

CC ?= cc
CSTD := -std=c11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
CDEBUG ?= -g -O0
BUILD ?= build

DEFAULT_CATASTROPHE_DIR := $(if $(wildcard ../Catastrophe/include/catastrophe.h),$(abspath ../Catastrophe),third_party/catastrophe)
CATASTROPHE_DIR ?= $(DEFAULT_CATASTROPHE_DIR)
CATASTROPHE_INCLUDE := $(CATASTROPHE_DIR)/include

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image)

CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) -I. -Iinternal -Ithird_party/cjson
CFLAGS_DAEMON := $(CFLAGS_COMMON)
CFLAGS_UI := $(CFLAGS_COMMON) -I$(CATASTROPHE_INCLUDE) $(SDL_CFLAGS)
LDLIBS_COMMON := -lsqlite3
LDLIBS_UI := $(LDLIBS_COMMON) $(SDL_LDFLAGS) -lm -lpthread
ifeq ($(shell uname -s),Darwin)
LDLIBS_UI += -lobjc
endif

DAEMON_SRCS := \
	cmd/jawakad/main.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/platform/paths.c \
	internal/db/db.c \
	internal/discovery/discovery.c \
	third_party/cjson/cJSON.c

UI_SRCS := \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/platform/paths.c \
	internal/db/db.c \
	third_party/cjson/cJSON.c

.PHONY: all jawakad jawaka-launcher jawaka-menu mockgen run-daemon run-daemon-interactive run-launcher run-menu clean help tg5040 tg5050 my355 check-catastrophe check-sdl

all: $(BUILD)/bin/jawakad $(BUILD)/bin/jawaka-launcher $(BUILD)/bin/jawaka-menu

jawakad: $(BUILD)/bin/jawakad
jawaka-launcher: $(BUILD)/bin/jawaka-launcher
jawaka-menu: $(BUILD)/bin/jawaka-menu

$(BUILD)/bin:
	@mkdir -p $(BUILD)/bin

check-catastrophe:
	@test -f "$(CATASTROPHE_INCLUDE)/catastrophe.h" || \
		( echo "Catastrophe headers not found. Set CATASTROPHE_DIR=/path/to/Catastrophe and retry." && exit 1 )

check-sdl:
	@pkg-config --exists sdl2 SDL2_ttf SDL2_image 2>/dev/null || \
		( echo "SDL2 libraries not found. Install with: brew install sdl2 sdl2_ttf sdl2_image" && exit 1 )

$(BUILD)/bin/jawakad: $(DAEMON_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_DAEMON) -o $@ $(DAEMON_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-launcher: cmd/jawaka-launcher/main.c $(UI_SRCS) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-launcher/main.c $(UI_SRCS) $(LDLIBS_UI)

$(BUILD)/bin/jawaka-menu: cmd/jawaka-menu/main.c $(UI_SRCS) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-menu/main.c $(UI_SRCS) $(LDLIBS_UI)

mockgen:
	bash scripts/mockgen.sh

run-daemon: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO="$${JAWAKA_AUTODEMO:-1}" \
	JAWAKA_AUTODEMO_DELAY_MS="$${JAWAKA_AUTODEMO_DELAY_MS:-1200}" \
	$(BUILD)/bin/jawakad

run-daemon-interactive: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO=0 \
	$(BUILD)/bin/jawakad

run-launcher: $(BUILD)/bin/jawaka-launcher mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	$(BUILD)/bin/jawaka-launcher

run-menu: $(BUILD)/bin/jawaka-menu mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	$(BUILD)/bin/jawaka-menu

clean:
	rm -rf $(BUILD)

tg5040 tg5050 my355:
	$(MAKE) -C ports/$@ all

help:
	@echo ""
	@echo "Jawaka build targets"
	@echo "===================="
	@echo "  make               Build jawakad, jawaka-launcher, jawaka-menu"
	@echo "  make mockgen       Create/update the mock SD-card tree"
	@echo "  make run-daemon              Run the daemon-driven phase-0/1 demo (auto-transitions)"
	@echo "  make run-daemon-interactive  Run daemon without auto-demo (stays open for testing)"
	@echo "  make run-launcher            Run jawaka-launcher directly"
	@echo "  make run-menu                Run jawaka-menu directly"
	@echo "  make clean         Remove build artifacts"
	@echo "  make tg5040        Placeholder cross-compile target"
	@echo "  make tg5050        Placeholder cross-compile target"
	@echo "  make my355         Placeholder cross-compile target"
	@echo ""
	@echo "Environment variables"
	@echo "====================="
	@echo "  JAWAKA_SDCARD_ROOT            Path to SD-card root (default: ./mock-sdcard)"
	@echo "  JAWAKA_AUTODEMO=1             Auto-transition launcher→menu→shutdown (default in run-daemon)"
	@echo "  JAWAKA_AUTODEMO_DELAY_MS=N    Delay before auto-demo action fires, ms (default: 1200)"
	@echo ""
	@echo "Catastrophe include root: $(CATASTROPHE_INCLUDE)"
	@echo ""
