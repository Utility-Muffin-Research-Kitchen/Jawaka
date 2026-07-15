SHELL := /bin/bash

CC ?= cc
CSTD := -std=c11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
BUILD ?= build
CFLAGS_PLATFORM ?=
LDFLAGS_PLATFORM ?=
PLATFORM ?= mac
MLP1_TOOLCHAIN_IMAGE ?= ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local
MLP1_BUILD_PROFILE ?= release
WORKSPACE_ROOT ?= $(abspath ..)

ifeq ($(PLATFORM),mlp1)
MLP1_FLAGS_MK ?= $(firstword $(wildcard /opt/mlp1-toolchain/umrk/mlp1-build-flags.mk $(WORKSPACE_ROOT)/mlp1-toolchain/flags/mlp1-build-flags.mk ../mlp1-toolchain/flags/mlp1-build-flags.mk))
ifneq ($(MLP1_FLAGS_MK),)
include $(MLP1_FLAGS_MK)
else
UMRK_MLP1_PROFILE_CFLAGS ?= -O2 -mcpu=cortex-a55 -mtune=cortex-a55 -ffunction-sections -fdata-sections -DNDEBUG
UMRK_MLP1_PROFILE_LDFLAGS ?= -Wl,--gc-sections
endif
CDEBUG ?= $(UMRK_MLP1_PROFILE_CFLAGS)
LDFLAGS_PLATFORM += $(UMRK_MLP1_PROFILE_LDFLAGS)
else
CDEBUG ?= -g -O0
endif

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
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)
# The Mac lane links the system libcurl when pkg-config has no entry; the
# mlp1 branch below still demands a pkg-config hit from the toolchain.
ifneq ($(PLATFORM),mlp1)
ifeq ($(strip $(CURL_LDFLAGS)),)
CURL_LDFLAGS := -lcurl
endif
endif

# ScreenScraper dev credentials, injected from the git-ignored .env.local
# (copy .env.example). Builds without credentials still compile; scraping
# reports itself unavailable at runtime.
-include .env.local
SCRAPE_DEFINES :=
ifdef SCREENSCRAPER_DEV_ID
SCRAPE_DEFINES += -DSCREENSCRAPER_DEV_ID=\"$(SCREENSCRAPER_DEV_ID)\"
endif
ifdef SCREENSCRAPER_DEV_PASSWORD
SCRAPE_DEFINES += -DSCREENSCRAPER_DEV_PASSWORD=\"$(SCREENSCRAPER_DEV_PASSWORD)\"
endif
ifdef SCREENSCRAPER_DEBUG_PASSWORD
SCRAPE_DEFINES += -DSCREENSCRAPER_DEBUG_PASSWORD=\"$(SCREENSCRAPER_DEBUG_PASSWORD)\"
endif

CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) $(CFLAGS_PLATFORM) -I. -Iinternal -Ithird_party/cjson
CFLAGS_DAEMON := $(CFLAGS_COMMON)
CFLAGS_UI := $(CFLAGS_COMMON) -I$(CATASTROPHE_INCLUDE) -Ithird_party/miniz $(SDL_CFLAGS) $(CURL_CFLAGS)
LDLIBS_COMMON := $(LDFLAGS_PLATFORM) -lsqlite3
LDLIBS_DAEMON := $(LDLIBS_COMMON)
LDLIBS_UI := $(LDLIBS_COMMON) $(SDL_LDFLAGS) $(CURL_LDFLAGS) -lm -lpthread
ifeq ($(shell uname -s),Darwin)
LDLIBS_UI += -lobjc
endif
ifeq ($(PLATFORM),mlp1)
ifeq ($(strip $(CURL_LDFLAGS)),)
$(error PLATFORM=mlp1 requires libcurl in the toolchain; rebuild mlp1-toolchain)
endif
CFLAGS_DAEMON += -DJW_UPDATE_USE_LIBCURL=1 $(CURL_CFLAGS)
LDLIBS_DAEMON += $(CURL_LDFLAGS)
# MLP1 dismisses the stock boot transition from jawakad's platform backend by
# dlopen-ing libloong_sdk.so at runtime, which needs libdl.
LDLIBS_DAEMON += -ldl
PLATFORM_BACKEND_SRC := internal/platform/device_mlp1.c
PLATFORM_ID_SRC := internal/platform/platform_id_mlp1.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mlp1.c
BLUETOOTH_SRC := internal/platform/bluetooth_mlp1.c
WIFI_SRC := internal/platform/wifi_mlp1.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_wayland.c $(BUILD)/generated/xdg-shell-protocol.c
OSD_DEPS := $(BUILD)/generated/xdg-shell-client-protocol.h
OSD_CFLAGS := $(CFLAGS_COMMON) $(WAYLAND_CFLAGS) -I$(BUILD)/generated
OSD_LDLIBS := $(LDLIBS_COMMON) $(WAYLAND_LDFLAGS)
else
PLATFORM_BACKEND_SRC := internal/platform/device_mock.c
PLATFORM_ID_SRC := internal/platform/platform_id_mock.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mock.c
BLUETOOTH_SRC := internal/platform/bluetooth_unsupported.c
WIFI_SRC := internal/platform/wifi_unsupported.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_sdl.c
OSD_DEPS :=
OSD_CFLAGS := $(CFLAGS_UI)
OSD_LDLIBS := $(LDLIBS_UI)
endif
PLATFORM_COMMON_SRC := internal/platform/platform_common.c

# ScreenScraper scrape engine (daemon-side; curl + vendored stb/miniz/md5).
SCRAPE_SRCS := \
	internal/scrape/ss_client.c \
	internal/scrape/scrape_catalog.c \
	internal/scrape/scrape_md5.c \
	internal/scrape/scrape_systems.c \
	internal/scrape/scrape_worker.c \
	third_party/md5/md5.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c
SCRAPE_CFLAGS := $(SCRAPE_DEFINES) $(CURL_CFLAGS) \
	-Ithird_party/stb -Ithird_party/miniz -Ithird_party/md5

CFLAGS_DAEMON += $(SCRAPE_CFLAGS)
LDLIBS_DAEMON += $(CURL_LDFLAGS) -lpthread -lm

DAEMON_SRCS := \
	cmd/jawakad/main.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	$(PLATFORM_COMMON_SRC) \
	internal/platform/device.c \
	$(BLUETOOTH_SRC) \
	$(WIFI_SRC) \
	$(PLATFORM_BACKEND_SRC) \
	$(PLATFORM_ID_SRC) \
	$(INPUT_PROXY_SRC) \
	internal/platform/calibration.c \
	internal/platform/paths.c \
	internal/power/suspend_inhibit.c \
	internal/retroarch/catalog.c \
	internal/retroarch/command.c \
	internal/retroarch/states.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	internal/update/update.c \
	internal/update/sha256.c \
	internal/db/db.c \
	internal/settings/appearance.c \
	internal/settings/theme_resolve.c \
	internal/discovery/discovery.c \
	$(SCRAPE_SRCS) \
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

UPDATE_RUNNER_SRCS := \
	cmd/jawaka-update-runner/main.c \
	internal/update/sha256.c \
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
	internal/storage/sources.c \
	third_party/cjson/cJSON.c

SCRAPE_SMOKE_SRCS := \
	cmd/jawaka-scrape-smoke/main.c \
	$(SCRAPE_SRCS) \
	internal/core/log.c \
	internal/db/db.c \
	internal/storage/sources.c \
	third_party/cjson/cJSON.c

PAKRAT_SMOKE_SRCS := \
	cmd/jawaka-pakrat-smoke/main.c \
	internal/store/pakrat.c \
	internal/store/pakrat_state.c \
	internal/ipc/ipc.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/discovery/discovery.c \
	internal/retroarch/catalog.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c

CATALOG_SMOKE_SRCS := \
	cmd/jawaka-catalog-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

CORE_OVERRIDE_SMOKE_SRCS := \
	cmd/jawaka-core-override-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/retroarch/catalog.c \
	third_party/cjson/cJSON.c

UPDATE_SMOKE_SRCS := \
	cmd/jawaka-update-smoke/main.c \
	internal/update/update.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c

INHIBIT_CTL_SRCS := \
	cmd/jawaka-inhibitctl/main.c \
	internal/ipc/ipc.c \
	third_party/cjson/cJSON.c

UI_SRCS := \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	$(PLATFORM_COMMON_SRC) \
	$(PLATFORM_ID_SRC) \
	internal/platform/cat_services.c \
	internal/platform/paths.c \
	$(BLUETOOTH_SRC) \
	$(WIFI_SRC) \
	internal/retroarch/catalog.c \
	internal/retroarch/states.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	internal/store/pakrat.c \
	internal/store/pakrat_state.c \
	internal/update/sha256.c \
	internal/discovery/discovery.c \
	internal/db/db.c \
	internal/launcher/console_colors.c \
	internal/launcher/coverflow.c \
	internal/launcher/game_switcher.c \
	internal/launcher/system_names.c \
	internal/scrape/scrape_catalog.c \
	internal/settings/appearance.c \
	internal/settings/settings.c \
	internal/settings/theme_resolve.c \
	third_party/cjson/cJSON.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c

ALL_BINS := \
	$(BUILD)/bin/jawakad \
	$(BUILD)/bin/jawaka-launcher \
	$(BUILD)/bin/jawaka-menu \
	$(BUILD)/bin/jawaka-osd \
	$(BUILD)/bin/jawaka-retroarchctl \
	$(BUILD)/bin/jawaka-retroarch-runner \
	$(BUILD)/bin/jawaka-update-runner \
	$(BUILD)/bin/jawaka-platformctl

ifeq ($(PLATFORM),mlp1)
ALL_BINS += $(BUILD)/bin/jawaka-ledd
endif
ifeq ($(PLATFORM),mlp1)
ALL_OUTPUTS := $(ALL_BINS) $(BUILD)/build-manifest.json
else
ALL_OUTPUTS := $(ALL_BINS)
endif

.PHONY: all jawakad jawaka-launcher jawaka-menu jawaka-osd jawaka-retroarchctl jawaka-retroarch-runner jawaka-update-runner jawaka-platformctl jawaka-ledd jawaka-scan-smoke jawaka-scrape-smoke jawaka-pakrat-smoke jawaka-catalog-smoke jawaka-core-override-smoke jawaka-update-smoke jawaka-inhibitctl storage-sources-test imported-title-test imported-title-ipc-smoke states-core-test suspend-inhibit-test suspend-inhibit-ipc-smoke update-local-manifest-smoke pakrat-state-smoke mockgen run-daemon run-daemon-interactive run-daemon-only run-launcher run-menu run-interactive clean help tg5040 tg5050 my355 mlp1 mlp1-pakrat-smoke mlp1-inhibit-smoke mlp1-adb-smoke mlp1-adb-inhibit-smoke mlp1-adb-input-capture mlp1-adb-ra-command-smoke phase3-fixture-scan-smoke phase3-core-choice-smoke check-catastrophe check-sdl FORCE

all: $(ALL_OUTPUTS)

jawakad: $(BUILD)/bin/jawakad
jawaka-launcher: $(BUILD)/bin/jawaka-launcher
jawaka-menu: $(BUILD)/bin/jawaka-menu
jawaka-osd: $(BUILD)/bin/jawaka-osd
jawaka-retroarchctl: $(BUILD)/bin/jawaka-retroarchctl
jawaka-retroarch-runner: $(BUILD)/bin/jawaka-retroarch-runner
jawaka-update-runner: $(BUILD)/bin/jawaka-update-runner
jawaka-platformctl: $(BUILD)/bin/jawaka-platformctl
ifeq ($(PLATFORM),mlp1)
jawaka-ledd: $(BUILD)/bin/jawaka-ledd
else
jawaka-ledd:
	@echo "jawaka-ledd is MLP1-only; build with PLATFORM=mlp1." >&2
	@exit 1
endif
jawaka-scan-smoke: $(BUILD)/bin/jawaka-scan-smoke
jawaka-scrape-smoke: $(BUILD)/bin/jawaka-scrape-smoke
jawaka-pakrat-smoke: $(BUILD)/bin/jawaka-pakrat-smoke
jawaka-catalog-smoke: $(BUILD)/bin/jawaka-catalog-smoke
jawaka-core-override-smoke: $(BUILD)/bin/jawaka-core-override-smoke
jawaka-update-smoke: $(BUILD)/bin/jawaka-update-smoke
jawaka-inhibitctl: $(BUILD)/bin/jawaka-inhibitctl

storage-sources-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/storage-sources-test \
		internal/storage/sources_test.c internal/storage/sources.c
	$(BUILD)/bin/storage-sources-test

imported-title-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/imported-title-test \
		internal/db/imported_title_test.c internal/db/db.c $(LDLIBS_COMMON)
	$(BUILD)/bin/imported-title-test

imported-title-ipc-smoke:
	scripts/imported-title-ipc-smoke.sh

states-core-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/states-core-test \
		internal/retroarch/states_core_test.c internal/retroarch/states.c
	$(BUILD)/bin/states-core-test

suspend-inhibit-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/suspend-inhibit-test \
		internal/power/suspend_inhibit_test.c internal/power/suspend_inhibit.c
	$(BUILD)/bin/suspend-inhibit-test

suspend-inhibit-ipc-smoke:
	scripts/suspend-inhibit-ipc-smoke.sh

update-local-manifest-smoke:
	@scripts/update-local-manifest-smoke.sh

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

$(BUILD)/bin/jawaka-update-runner: $(UPDATE_RUNNER_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(UPDATE_RUNNER_SRCS)

$(BUILD)/bin/jawaka-platformctl: $(PLATFORM_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(PLATFORM_CTL_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-scan-smoke: $(SCAN_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(SCAN_SMOKE_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-scrape-smoke: $(SCRAPE_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) $(SCRAPE_CFLAGS) -o $@ $(SCRAPE_SMOKE_SRCS) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lpthread -lm

$(BUILD)/bin/jawaka-pakrat-smoke: $(PAKRAT_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) $(CURL_CFLAGS) -Ithird_party/miniz -o $@ $(PAKRAT_SMOKE_SRCS) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lm

$(BUILD)/bin/jawaka-catalog-smoke: $(CATALOG_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(CATALOG_SMOKE_SRCS)

$(BUILD)/bin/jawaka-core-override-smoke: $(CORE_OVERRIDE_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(CORE_OVERRIDE_SMOKE_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-update-smoke: $(UPDATE_SMOKE_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) $(CURL_CFLAGS) -o $@ $(UPDATE_SMOKE_SRCS) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lpthread

$(BUILD)/bin/jawaka-inhibitctl: $(INHIBIT_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(INHIBIT_CTL_SRCS)

ifeq ($(PLATFORM),mlp1)
$(BUILD)/bin/jawaka-ledd: cmd/jawaka-ledd/main.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ cmd/jawaka-ledd/main.c

$(BUILD)/build-manifest.json: $(ALL_BINS) FORCE
	@mkdir -p "$(BUILD)"
	@{ \
		printf '{\n'; \
		printf '  "platform": "mlp1",\n'; \
		printf '  "target_soc": "%s",\n' "$(UMRK_MLP1_TARGET_SOC)"; \
		printf '  "target_cpu": "%s",\n' "$(UMRK_MLP1_TARGET_CPU)"; \
		printf '  "build_profile": "%s",\n' "$(MLP1_BUILD_PROFILE)"; \
		printf '  "cflags": "%s",\n' "$(CDEBUG)"; \
		printf '  "ldflags": "%s",\n' "$(LDFLAGS_PLATFORM)"; \
		printf '  "binaries": ["jawakad", "jawaka-launcher", "jawaka-menu", "jawaka-osd", "jawaka-retroarchctl", "jawaka-retroarch-runner", "jawaka-update-runner", "jawaka-platformctl", "jawaka-ledd"],\n'; \
		printf '  "exceptions": []\n'; \
		printf '}\n'; \
	} > "$@"

FORCE:
endif

phase3-fixture-scan-smoke:
	scripts/phase3-fixture-scan-smoke.sh

phase3-core-choice-smoke:
	scripts/phase3-core-choice-smoke.sh

pakrat-state-smoke:
	scripts/pakrat-state-smoke.sh

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
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile all

mlp1-pakrat-smoke:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile pakrat-smoke

mlp1-inhibit-smoke:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile inhibit-smoke

mlp1-adb-smoke:
	scripts/adb-mlp1-smoke.sh

mlp1-adb-inhibit-smoke:
	scripts/adb-mlp1-suspend-inhibit-smoke.sh

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
	@echo "  make jawaka-inhibitctl       Build suspend-inhibitor diagnostic helper"
	@echo "  make suspend-inhibit-test suspend-inhibit-ipc-smoke  Run native lease/power tests"
	@echo "  make jawaka-retroarch-runner Build RetroArch app/config runner"
	@echo "  make jawaka-update-runner    Build OTA install handoff runner"
	@echo "  make jawaka-pakrat-smoke     Build local Pak Rat install/uninstall smoke helper"
	@echo "  make jawaka-catalog-smoke    Build metadata/core-choice smoke helper"
	@echo "  make update-local-manifest-smoke  Validate developer artifact.url handling"
	@echo "  make pakrat-state-smoke      Exercise Pak Rat stale + managed-state safeguards"
	@echo "  make clean         Remove build artifacts"
	@echo "  make tg5040        Placeholder cross-compile target"
	@echo "  make tg5050        Placeholder cross-compile target"
	@echo "  make my355         Placeholder cross-compile target"
	@echo "  make mlp1          Cross-compile for Miniloong Pocket 1"
	@echo "  make mlp1-pakrat-smoke  Cross-compile local Pak Rat smoke helper for MLP1"
	@echo "  make mlp1-adb-inhibit-smoke  Run the RTC-woken MLP1 suspend/reap smoke"
	@echo "  make mlp1-adb-smoke  Build, push to /tmp, and run an ADB UI smoke"
	@echo "  make mlp1-adb-input-capture  Record Loong Gamepad evtest labels over ADB"
	@echo "  make mlp1-adb-ra-command-smoke  Run RetroArch command feature smoke over ADB"
	@echo "  make phase3-fixture-scan-smoke  Run metadata-aware scan fixture checks"
	@echo "  make phase3-core-choice-smoke  Run core picker path-core fixture checks"
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
