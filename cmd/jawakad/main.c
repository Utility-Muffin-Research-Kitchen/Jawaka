#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/discovery/discovery.h"
#include "internal/ipc/ipc.h"
#include "internal/platform/device.h"
#include "internal/platform/input_proxy.h"
#include "internal/platform/paths.h"
#include "internal/platform/wifi.h"
#include "internal/retroarch/command.h"
#include "internal/retroarch/catalog.h"
#include "internal/retroarch/states.h"
#include "internal/scrape/scrape_worker.h"
#include "internal/scrape/ss_client.h"
#include "internal/settings/appearance.h"
#include "internal/storage/sources.h"
#include "internal/update/update.h"

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* CLOCK_BOOTTIME (counts time spent suspended) is Linux-only. The native/mock
   build never suspends, so fall back to CLOCK_MONOTONIC there: the boot-minus-
   monotonic gap is then always ~0 ("never suspended"), which is the correct
   off-device behavior and keeps the resume-detection code portable. */
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif

#define JW_SWITCHER_RESUME_RETRY_MS 100LL
#define JW_SWITCHER_RESUME_MAX_ATTEMPTS 40
/* The load-state command does real work (RA reads + applies the state) and can
   need longer than the readiness probe, especially in the first second of boot.
   Generous enough to catch a slow reply, short enough not to stall the daemon. */
#define JW_SWITCHER_RESUME_LOAD_TIMEOUT_MS 400u
#define JW_INGAME_MENU_PREWARM_DELAY_MS 1200LL
#define JW_INGAME_MENU_PREWARM_AFTER_RESUME_MS 250LL
#define JW_RETROARCH_AUDIO_REINIT_RETRY_MS 500LL
#define JW_RETROARCH_AUDIO_REINIT_TIMEOUT_MS 10000LL
#define JW_RETROARCH_QUIT_GRACE_MS 700LL
#define JW_RETROARCH_KILL_GRACE_MS 700LL
#define JW_RESIDENT_SWITCH_MAX_DEFAULT (-1)
#define JW_RESIDENT_SWITCH_MAX_DEFAULT_LABEL "unlimited"
#define JW_PERF_SETTING_KEY "platform.performance.game_profile"
#define JW_CONTENT_SETTING_CORE_ID "core_id"
#define JW_CONTENT_SETTING_PERFORMANCE_PROFILE "performance_profile"
#define JW_STARTUP_MAINT_GRACE_MS 500LL    /* after frontend-ready */
#define JW_STARTUP_MAINT_FALLBACK_MS 15000LL /* if frontend-ready never arrives */

typedef enum {
    JW_CHILD_NONE = 0,
    JW_CHILD_LAUNCHER,
    JW_CHILD_MENU,
    JW_CHILD_RETROARCH,
    JW_CHILD_EMULATOR,
    JW_CHILD_APP
} jw_child_kind;

typedef enum {
    JW_LAUNCH_TARGET_NONE = 0,
    JW_LAUNCH_TARGET_RETROARCH,
    JW_LAUNCH_TARGET_STANDALONE
} jw_launch_target_kind;

typedef enum {
    JW_STANDBY_NONE = 0,
    JW_STANDBY_AUTOSLEEP,
    JW_STANDBY_POWER_CHARGING
} jw_standby_reason;

typedef struct {
    bool active;
    pid_t pid;
    time_t started_at;
    char system[64];
    char rom_path[PATH_MAX];
    char db_rom_path[PATH_MAX];
    char source_root[PATH_MAX];
    char core_path[PATH_MAX];
    char core_id[64];
    char config_path[PATH_MAX];
    int resident_switches;
    bool persist_config;
    bool audio_bluetooth;
} jw_retroarch_session;

typedef struct {
    jw_launch_target_kind kind;
    char path[PATH_MAX];
    char core_id[64];
    char diagnostic[256];
} jw_launch_target;

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mu;
    bool            initialized;
    bool            thread_started;
    bool            running;
    bool            completed;
    bool            ok;
    bool            pending_rescan;
    char            reason[96];
    char            pending_reason[96];
    char            db_path[PATH_MAX];
    char            sdcard_root[PATH_MAX];
    long long       started_ms;
    long long       finished_ms;
    jw_scan_result  result;
    char            error[160];
} jw_scan_job;

typedef struct {
    char *runtime_dir;
    char *sdcard_root;
    char *socket_path;
    char *osd_socket_path;
    char *db_path;
    char *state_dir;
    char  bin_dir[PATH_MAX];
    sqlite3 *db;
    jw_ipc_server *server;
    jw_platform_context platform;
    jw_input_proxy input_proxy;
    pid_t child_pid;          /* foreground launcher, normal menu, RetroArch, or app */
    jw_child_kind child_kind;
    pid_t menu_pid;           /* resident warm-standby in-game menu while RetroArch is alive */
    bool menu_in_game;
    bool menu_visible;        /* standby menu is currently shown (RetroArch paused under it) */
    int menu_standby_attempts;/* respawn guard for a crashing standby within one session */
    long long standalone_quit_request_ms; /* Menu-tap quit sent to a standalone
                                 emulator without a native menu signal (0 = none);
                                 a second tap after the grace period escalates
                                 to SIGKILL */
    bool retroarch_resume_on_menu_exit;
    pid_t osd_pid;
    pid_t ledd_pid;            /* jawaka-ledd custom LED effect engine, -1 when idle */
    int cached_brightness_percent;
    int cached_volume_percent;
    long long audio_reconcile_last_ms;
    jw_led_config cached_led;
    bool led_configured;       /* true once a user LED setting has been persisted/applied */
    jw_retroarch_session retroarch_session;
    int library_generation;
    bool library_populated;
    jw_scan_job scan_job;
    /* Startup maintenance (wifi restore/harden + library scan) deferred past
       the launcher's first frame so it doesn't sit between boot animation and
       launcher; phase 0 = wifi, phase 1 = scan, fired on separate loop
       iterations so a queued launcher IPC poll can drain between them. */
    bool startup_maintenance_pending;
    int startup_maintenance_phase;
    long long startup_maintenance_next_ms;
    bool library_scanned_since_boot;
    bool pending_menu;
    bool pending_launch;
    char pending_launch_system[64];
    char pending_launch_rom_path[PATH_MAX];
    bool pending_launch_resume_switcher;
    /* Standalone emulator asked (via Menu+Select marker) to reopen the launcher
       straight into the switcher carousel, seeded on the just-exited game. */
    bool launcher_open_switcher;
    char launcher_switcher_system[64];
    char launcher_switcher_rom[PATH_MAX];
    bool post_launch_resume_pending;
    int post_launch_resume_attempts;
    long long post_launch_resume_next_ms;
    bool in_game_menu_prewarm_pending;
    long long in_game_menu_prewarm_next_ms;
    bool retroarch_audio_reinit_pending;
    long long retroarch_audio_reinit_next_ms;
    long long retroarch_audio_reinit_deadline_ms;
    char retroarch_audio_reinit_reason[64];
    bool pending_app;
    char pending_app_pak_dir[PATH_MAX];
    bool daemon_only;
    bool shutdown_requested;
    /* Auto-sleep: idle → screen off (bl_power) → suspend (mem). */
    int       autosleep_timeout_s;        /* cached from DB; 0 = disabled */
    int       autosleep_platform_synced_s;/* last value mirrored to stock power policy */
    long long autosleep_setting_next_ms;  /* throttle for re-reading the DB setting */
    jw_standby_reason standby_reason;     /* screen-off standby state, or NONE when lit */
    long long standby_entered_ms;         /* monotonic ms for wake-input detection */
    bool      autosleep_charging_logged;  /* log the charging hold once per standby */
    int       charging_cached;            /* -1 unknown, 0 unplugged, 1 charging */
    long long charging_next_poll_ms;       /* throttle platform status reads in standby */
    bool      power_sleep_armed;          /* power pressed while screen on → sleep on release */
    bool      power_held;                 /* power key currently held (for long-press detect) */
    long long power_down_ms;              /* when the current power press started */
    int       hdmi_last_connected;        /* -1 unknown, 0/1; for hotplug edge detection */
    long long hdmi_next_poll_ms;          /* throttle for the HDMI hotplug poll */
    long long hdmi_revert_deadline_ms;    /* 0 = none; auto-revert 1080p120 if not kept */
    int       hdmi_was_120;               /* live-1080p120 edge tracker */
    jw_platform_perf_profile perf_global_profile;
    jw_platform_perf_profile perf_active_profile;
    jw_platform_perf_profile perf_session_profile;
    bool perf_session_override;
    bool perf_custom_valid;
    jw_platform_perf_request perf_custom_request;
    char perf_last_error[JW_PLATFORM_MAX_MESSAGE];
    jw_update_status update_status;
    jw_update_download_job update_download_job;
    jw_update_install_job update_install_job;
    jw_update_check_job update_check_job;
} jw_daemon_state;

static volatile sig_atomic_t g_shutdown_requested = 0;

static bool jw__has_retroarch_session(const jw_daemon_state *state) {
    return state &&
           state->retroarch_session.active &&
           state->child_kind == JW_CHILD_RETROARCH;
}

static bool jw__has_standalone_session(const jw_daemon_state *state) {
    return state &&
           state->retroarch_session.active &&
           state->child_kind == JW_CHILD_EMULATOR;
}

static bool jw__standalone_session_is_ppsspp(const jw_daemon_state *state) {
    if (!jw__has_standalone_session(state)) {
        return false;
    }

    const jw_retroarch_session *session = &state->retroarch_session;
    return strcmp(session->core_id, "ppsspp") == 0 ||
           strstr(session->core_path, "/ppsspp/") != NULL ||
           strstr(session->core_path, "/PPSSPP") != NULL;
}

static bool jw__standalone_session_is_drastic(const jw_daemon_state *state) {
    if (!jw__has_standalone_session(state)) {
        return false;
    }

    const jw_retroarch_session *session = &state->retroarch_session;
    return strcmp(session->core_id, "drastic") == 0 ||
           strstr(session->core_path, "/drastic/") != NULL ||
           strstr(session->core_path, "/DraStic") != NULL;
}

static bool jw__standalone_session_is_mupen64plus(const jw_daemon_state *state) {
    if (!jw__has_standalone_session(state)) {
        return false;
    }

    const jw_retroarch_session *session = &state->retroarch_session;
    return strcmp(session->core_id, "mupen64plus_standalone") == 0 ||
           strcmp(session->core_id, "mupen64plus") == 0 ||
           strstr(session->core_path, "/mupen64plus/") != NULL ||
           strstr(session->core_path, "/Mupen64Plus") != NULL;
}

static bool jw__standalone_target_is_mupen64plus(const jw_launch_target *target) {
    if (!target || target->kind != JW_LAUNCH_TARGET_STANDALONE) {
        return false;
    }
    return strcmp(target->core_id, "mupen64plus_standalone") == 0 ||
           strcmp(target->core_id, "mupen64plus") == 0 ||
           strstr(target->path, "/mupen64plus/") != NULL ||
           strstr(target->path, "/Mupen64Plus") != NULL;
}

static bool jw__standalone_target_is_ports(const jw_launch_target *target) {
    if (!target || target->kind != JW_LAUNCH_TARGET_STANDALONE) {
        return false;
    }
    return strcmp(target->core_id, "ports") == 0 ||
           strstr(target->path, "/emulators/ports/") != NULL ||
           strstr(target->path, "/Roms/PORTS") != NULL;
}

static bool jw__standalone_target_uses_calibrated_virtual_input(
        const jw_launch_target *target) {
    return jw__standalone_target_is_mupen64plus(target) ||
           jw__standalone_target_is_ports(target);
}

static long long jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static void jw__handle_signal(int signo) {
    (void)signo;
    g_shutdown_requested = 1;
}

static int jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int jw__resident_switch_max(void) {
    static bool cached = false;
    static int max_switches = JW_RESIDENT_SWITCH_MAX_DEFAULT;

    if (cached) {
        return max_switches;
    }
    cached = true;

    const char *value = getenv("JAWAKA_RESIDENT_SWITCH_MAX");
    if (!value || !value[0]) {
        return max_switches;
    }
    if (strcmp(value, "unlimited") == 0 || strcmp(value, "-1") == 0) {
        max_switches = -1;
        return max_switches;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno == 0 && end && *end == '\0' && parsed >= 0 && parsed <= 1000000L) {
        max_switches = (int)parsed;
    } else {
        jw_log_warn("invalid JAWAKA_RESIDENT_SWITCH_MAX=%s; using default=%s",
                    value, JW_RESIDENT_SWITCH_MAX_DEFAULT_LABEL);
    }
    return max_switches;
}

static const char *jw__env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0]) ? value : NULL;
}

static void jw__setenv_default(const char *name, const char *value) {
    if (!name || !value || jw__env_value(name)) {
        return;
    }
    setenv(name, value, 0);
}

static void jw__setenvf_default(const char *name, const char *fmt, ...) {
    if (!name || jw__env_value(name) || !fmt) {
        return;
    }

    char value[PATH_MAX];
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(value, sizeof(value), fmt, args);
    va_end(args);
    if (needed >= 0 && needed < (int)sizeof(value)) {
        setenv(name, value, 0);
    }
}

static bool jw__format_default_system_path(char *out, size_t out_size,
                                           const char *sdcard_root,
                                           const char *platform) {
    if (!out || out_size == 0 || !sdcard_root || !sdcard_root[0] ||
        !platform || !platform[0]) {
        return false;
    }

    int needed = snprintf(out, out_size, "%s/.system/leaf/platforms/%s",
                          sdcard_root, platform);
    return needed >= 0 && needed < (int)out_size;
}

static int jw__env_or_join(char *out, size_t out_size,
                           const char *path_env,
                           const char *base_env_a,
                           const char *base_env_b,
                           const char *fallback_base,
                           const char *leaf) {
    const char *path = jw__env_value(path_env);
    if (path) {
        return snprintf(out, out_size, "%s", path) < (int)out_size ? 0 : -1;
    }

    const char *base = jw__env_value(base_env_a);
    if (!base && base_env_b) {
        base = jw__env_value(base_env_b);
    }
    if (!base) {
        base = fallback_base;
    }
    if (!base || !leaf) {
        return -1;
    }
    return snprintf(out, out_size, "%s/%s", base, leaf) < (int)out_size ? 0 : -1;
}

static int jw__is_regular_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int jw__set_bin_dir(char *argv0, char *out, size_t out_size) {
    char resolved[PATH_MAX];
    if (!realpath(argv0, resolved)) {
        return -1;
    }

    char temp[PATH_MAX];
    snprintf(temp, sizeof(temp), "%s", resolved);
    char *dir = dirname(temp);
    if (!dir) {
        return -1;
    }

    snprintf(out, out_size, "%s", dir);
    return 0;
}

static void jw__print_usage(FILE *stream) {
    fprintf(stream, "Usage: jawakad [--daemon-only] [--help]\n");
}

static void jw__publish_runtime_path_env(const jw_daemon_state *state) {
    if (!state || !state->runtime_dir || !state->sdcard_root) {
        return;
    }

    const char *platform = state->platform.platform_id[0]
        ? state->platform.platform_id
        : "mac";

    jw__setenv_default("UMRK_ENV_VERSION", "1");
    jw__setenv_default("PLATFORM", platform);
    jw__setenv_default("DEVICE", platform);
    jw__setenv_default("SDCARD_PATH", state->sdcard_root);
    jw__setenv_default("UMRK_RUNTIME_PATH", state->runtime_dir);
    char system_path[PATH_MAX];
    if (jw__format_default_system_path(system_path, sizeof(system_path),
                                       state->sdcard_root, platform)) {
        jw__setenv_default("SYSTEM_PATH", system_path);
    }
    if (getenv("SYSTEM_PATH")) {
        jw__setenv_default("UMRK_PLATFORM_PATH", getenv("SYSTEM_PATH"));
    }

    char launcher_path[PATH_MAX];
    if (snprintf(launcher_path, sizeof(launcher_path), "%s", state->bin_dir) <
        (int)sizeof(launcher_path)) {
        char *slash = strrchr(launcher_path, '/');
        if (slash && strcmp(slash + 1, "bin") == 0) {
            *slash = '\0';
            jw__setenv_default("UMRK_LAUNCHER_PATH", launcher_path);
        }
    }
    jw__setenv_default("UMRK_BIN_PATH", state->bin_dir);
    if (getenv("UMRK_LAUNCHER_PATH")) {
        jw__setenvf_default("UMRK_ENV_FILE", "%s/env.sh",
                            getenv("UMRK_LAUNCHER_PATH"));
    }

    if (getenv("SYSTEM_PATH")) {
        jw__setenvf_default("UMRK_MARKER_PATH", "%s/enabled",
                            getenv("SYSTEM_PATH"));
    }
    /* Durable data lives at the SD root, split by ownership, not under the
       release-managed .system payload (matches device/umrk-env.sh). */
    jw__setenvf_default("USERDATA_PATH", "%s/.userdata/%s",
                        state->sdcard_root, platform);
    jw__setenvf_default("UMRK_INTERNAL_DATA_PATH", "%s/.umrk/%s",
                        state->sdcard_root, platform);
    jw__setenvf_default("SHARED_USERDATA_PATH", "%s/.userdata/shared",
                        state->sdcard_root);
    if (getenv("USERDATA_PATH")) {
        jw__setenvf_default("LOGS_PATH", "%s/logs", getenv("USERDATA_PATH"));
    }
    if (getenv("UMRK_INTERNAL_DATA_PATH")) {
        jw__setenvf_default("UMRK_ADB_MARKER_PATH", "%s/adb-enabled",
                            getenv("UMRK_INTERNAL_DATA_PATH"));
    }
    jw__setenvf_default("ROMS_PATH", "%s/Roms", state->sdcard_root);
    jw__setenvf_default("IMAGES_PATH", "%s/Images", state->sdcard_root);
    jw__setenvf_default("APPS_PATH", "%s/Apps", state->sdcard_root);
    jw__setenvf_default("BIOS_PATH", "%s/BIOS", state->sdcard_root);
    jw__setenvf_default("SAVES_PATH", "%s/Saves", state->sdcard_root);
    jw__setenvf_default("STATES_PATH", "%s/States", state->sdcard_root);
    jw__setenvf_default("CHEATS_PATH", "%s/Cheats", state->sdcard_root);
    if (getenv("SYSTEM_PATH")) {
        jw__setenvf_default("CORES_PATH", "%s/cores", getenv("SYSTEM_PATH"));
        jw__setenvf_default("INFO_PATH", "%s/info", getenv("SYSTEM_PATH"));
    }

    char *retroarch_bin = jw_retroarch_bin_path();
    if (retroarch_bin) {
        jw__setenv_default("UMRK_RETROARCH_BIN", retroarch_bin);
        free(retroarch_bin);
    }

    setenv("JAWAKA_RUNTIME_DIR", state->runtime_dir, 1);
    setenv("JAWAKA_SDCARD_ROOT", state->sdcard_root, 1);
    if (getenv("UMRK_RETROARCH_BIN")) {
        setenv("JAWAKA_RETROARCH_BIN", getenv("UMRK_RETROARCH_BIN"), 1);
    }
    if (getenv("CORES_PATH")) {
        setenv("JAWAKA_RETROARCH_CORES_DIR", getenv("CORES_PATH"), 1);
    }
}

static int jw__reply_json(jw_ipc_client *client, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        cJSON_Delete(root);
        return -1;
    }

    int rc = jw_ipc_client_send(client, json, strlen(json));
    cJSON_free(json);
    cJSON_Delete(root);
    return rc;
}

static int jw__reply_hello_ok(jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello-ok");
    cJSON_AddStringToObject(root, "version", "0.0.1");
    return jw__reply_json(client, root);
}

static int jw__reply_ok(jw_ipc_client *client, const char *action, const jw_scan_result *scan_result) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", action);
    if (scan_result) {
        cJSON_AddNumberToObject(root, "game_count", scan_result->game_count);
        cJSON_AddNumberToObject(root, "app_count", scan_result->app_count);
        cJSON_AddNumberToObject(root, "system_count", scan_result->system_count);
    }
    return jw__reply_json(client, root);
}

static int jw__reply_error(jw_ipc_client *client, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "message", message);
    return jw__reply_json(client, root);
}

static void jw__json_add_int_or_null(cJSON *root, const char *name, int value) {
    if (value < 0) {
        cJSON_AddNullToObject(root, name);
    } else {
        cJSON_AddNumberToObject(root, name, value);
    }
}

static cJSON *jw__platform_capabilities_json(const jw_platform_capabilities *cap) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "battery", cap && cap->battery);
    cJSON_AddBoolToObject(root, "charging", cap && cap->charging);
    cJSON_AddBoolToObject(root, "sleep", cap && cap->sleep);
    cJSON_AddBoolToObject(root, "poweroff", cap && cap->poweroff);
    cJSON_AddBoolToObject(root, "reboot", cap && cap->reboot);
    cJSON_AddBoolToObject(root, "brightness", cap && cap->brightness);
    cJSON_AddBoolToObject(root, "volume", cap && cap->volume);
    cJSON_AddBoolToObject(root, "wifi", cap && cap->wifi);
    cJSON_AddBoolToObject(root, "bluetooth", cap && cap->bluetooth);
    cJSON_AddBoolToObject(root, "adb", cap && cap->adb);
    cJSON_AddBoolToObject(root, "boot_splash", cap && cap->boot_splash);
    cJSON_AddBoolToObject(root, "refresh_rate", cap && cap->refresh_rate);
    cJSON_AddBoolToObject(root, "hdmi_output", cap && cap->hdmi_output);
    cJSON_AddBoolToObject(root, "led", cap && cap->led);
    cJSON_AddBoolToObject(root, "performance", cap && cap->performance);
    return root;
}

static cJSON *jw__platform_status_json(const jw_platform_status *status) {
    cJSON *root = cJSON_CreateObject();
    if (!status) {
        return root;
    }

    jw__json_add_int_or_null(root, "battery_percent", status->battery_percent);
    jw__json_add_int_or_null(root, "charging", status->charging);
    jw__json_add_int_or_null(root, "brightness_percent", status->brightness_percent);
    jw__json_add_int_or_null(root, "volume_percent", status->volume_percent);
    if (status->audio_output >= 0 &&
        status->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        cJSON_AddStringToObject(root, "audio_output",
                                jw_platform_audio_output_name(status->audio_output));
    } else {
        cJSON_AddNullToObject(root, "audio_output");
    }
    cJSON *audio_outputs = cJSON_CreateArray();
    if (audio_outputs) {
        for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
            if (status->audio_available_outputs & JW_PLATFORM_AUDIO_OUTPUT_BIT(i)) {
                cJSON_AddItemToArray(audio_outputs,
                                     cJSON_CreateString(jw_platform_audio_output_name((jw_platform_audio_output)i)));
            }
        }
        cJSON_AddItemToObject(root, "audio_available_outputs", audio_outputs);
    }
    cJSON *audio_volumes = cJSON_CreateObject();
    if (audio_volumes) {
        for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
            const char *name = jw_platform_audio_output_name((jw_platform_audio_output)i);
            if (status->audio_volume_percent[i] >= 0) {
                cJSON_AddNumberToObject(audio_volumes, name,
                                        status->audio_volume_percent[i]);
            } else {
                cJSON_AddNullToObject(audio_volumes, name);
            }
        }
        cJSON_AddItemToObject(root, "audio_volumes", audio_volumes);
    }
    cJSON_AddNumberToObject(root, "audio_test_playing", status->audio_test_playing);
    jw__json_add_int_or_null(root, "wifi_connected", status->wifi_connected);
    jw__json_add_int_or_null(root, "wifi_strength", status->wifi_strength);
    jw__json_add_int_or_null(root, "bluetooth_connected", status->bluetooth_connected);
    jw__json_add_int_or_null(root, "adb_enabled", status->adb_enabled);
    jw__json_add_int_or_null(root, "adb_intent_enabled", status->adb_intent_enabled);
    jw__json_add_int_or_null(root, "boot_splash_enabled", status->boot_splash_enabled);
    jw__json_add_int_or_null(root, "refresh_rate_hz", status->refresh_rate_hz);
    jw__json_add_int_or_null(root, "hdmi_connected", status->hdmi_connected);
    jw__json_add_int_or_null(root, "hdmi_output_mode", status->hdmi_output_mode);
    return root;
}

static void jw__perf_request_init(jw_platform_perf_request *request) {
    if (!request) {
        return;
    }
    memset(request, 0, sizeof(*request));
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        request->domains[i].frequency = -1;
    }
}

static void jw__perf_request_set(jw_platform_perf_request *request,
                                 jw_platform_perf_domain domain,
                                 const char *governor,
                                 int frequency) {
    if (!request || domain < 0 || domain >= JW_PLATFORM_PERF_DOMAIN_COUNT) {
        return;
    }
    jw_platform_perf_domain_request *d = &request->domains[domain];
    snprintf(d->governor, sizeof(d->governor), "%s", governor ? governor : "");
    d->frequency = frequency;
}

static void jw__perf_request_for_profile(jw_platform_perf_profile profile,
                                         jw_platform_perf_request *request) {
    jw__perf_request_init(request);
    switch (profile) {
        case JW_PLATFORM_PERF_PROFILE_PERFORMANCE:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "performance", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "performance", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "performance", -1);
            break;
        case JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "userspace", 600000);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "userspace", 300000000);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "userspace", 528000000);
            break;
        case JW_PLATFORM_PERF_PROFILE_SLEEP:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "powersave", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "powersave", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "powersave", -1);
            break;
        case JW_PLATFORM_PERF_PROFILE_FRONTEND:
        case JW_PLATFORM_PERF_PROFILE_BALANCED:
        case JW_PLATFORM_PERF_PROFILE_AUTO:
        case JW_PLATFORM_PERF_PROFILE_CUSTOM:
        default:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "schedutil", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "simple_ondemand", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "dmc_ondemand", -1);
            break;
    }
}

static bool jw__perf_system_prefers_performance(const char *system) {
    if (!system || !system[0]) {
        return false;
    }
    return strcasecmp(system, "N64") == 0 ||
           strcasecmp(system, "PSP") == 0 ||
           strcasecmp(system, "DC") == 0 ||
           strcasecmp(system, "DREAMCAST") == 0 ||
           strcasecmp(system, "SATURN") == 0 ||
           strcasecmp(system, "NDS") == 0;
}

static jw_platform_perf_profile jw__perf_resolve_game_profile(
        const jw_daemon_state *state,
        jw_platform_perf_profile profile,
        const char *system) {
    (void)state;
    if (profile != JW_PLATFORM_PERF_PROFILE_AUTO) {
        return profile;
    }
    return jw__perf_system_prefers_performance(system)
        ? JW_PLATFORM_PERF_PROFILE_PERFORMANCE
        : JW_PLATFORM_PERF_PROFILE_BALANCED;
}

static jw_platform_perf_profile jw__perf_current_requested_profile(
        const jw_daemon_state *state) {
    if (!state) {
        return JW_PLATFORM_PERF_PROFILE_AUTO;
    }
    return state->perf_session_override
        ? state->perf_session_profile
        : state->perf_global_profile;
}

static int jw__perf_apply_profile(jw_daemon_state *state,
                                  jw_platform_perf_profile requested,
                                  const char *system,
                                  const char *reason) {
    if (!state || !state->platform.capabilities.performance) {
        return 0;
    }

    jw_platform_perf_profile profile =
        jw__perf_resolve_game_profile(state, requested, system);
    jw_platform_perf_request request;
    if (profile == JW_PLATFORM_PERF_PROFILE_CUSTOM && state->perf_custom_valid) {
        request = state->perf_custom_request;
    } else {
        if (profile == JW_PLATFORM_PERF_PROFILE_CUSTOM) {
            profile = JW_PLATFORM_PERF_PROFILE_BALANCED;
        }
        jw__perf_request_for_profile(profile, &request);
    }

    jw_platform_result result;
    jw_platform_apply_performance(&state->platform, &request, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        state->perf_active_profile = profile;
        state->perf_last_error[0] = '\0';
        jw_log_info("performance: applied profile=%s requested=%s reason=%s system=%s",
                    jw_platform_perf_profile_name(profile),
                    jw_platform_perf_profile_name(requested),
                    reason ? reason : "unknown",
                    system && system[0] ? system : "(none)");
        return 0;
    }

    snprintf(state->perf_last_error, sizeof(state->perf_last_error), "%s",
             result.message[0] ? result.message
                               : jw_platform_result_code_name(result.code));
    jw_log_warn("performance: apply failed profile=%s reason=%s: %s",
                jw_platform_perf_profile_name(profile),
                reason ? reason : "unknown",
                state->perf_last_error);
    return -1;
}

static int jw__perf_apply_game(jw_daemon_state *state, const char *system,
                               const char *reason) {
    return jw__perf_apply_profile(state,
                                  jw__perf_current_requested_profile(state),
                                  system, reason);
}

static int jw__perf_apply_frontend(jw_daemon_state *state, const char *reason) {
    return jw__perf_apply_profile(state, JW_PLATFORM_PERF_PROFILE_FRONTEND,
                                  NULL, reason);
}

static int jw__perf_apply_current_context(jw_daemon_state *state,
                                          const char *reason) {
    if (state && state->retroarch_session.active) {
        return jw__perf_apply_game(state, state->retroarch_session.system, reason);
    }
    return jw__perf_apply_frontend(state, reason);
}

static void jw__perf_load_global(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    state->perf_global_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    char value[64];
    if (state->db_path &&
        jw_db_get_setting(state->db_path, JW_PERF_SETTING_KEY,
                          value, sizeof(value)) == 0 &&
        value[0]) {
        jw_platform_perf_profile parsed;
        if (jw_platform_parse_perf_profile(value, &parsed) &&
            parsed != JW_PLATFORM_PERF_PROFILE_CUSTOM &&
            parsed != JW_PLATFORM_PERF_PROFILE_SLEEP &&
            parsed != JW_PLATFORM_PERF_PROFILE_FRONTEND) {
            state->perf_global_profile = parsed;
        }
    }
}

static int jw__perf_persist_global(jw_daemon_state *state,
                                   jw_platform_perf_profile profile) {
    if (!state || !state->db_path) {
        return -1;
    }
    return jw_db_set_setting(state->db_path, JW_PERF_SETTING_KEY,
                             jw_platform_perf_profile_name(profile));
}

static cJSON *jw__perf_domain_json(const jw_platform_perf_domain_status *domain) {
    cJSON *root = cJSON_CreateObject();
    if (!domain) {
        return root;
    }
    cJSON_AddBoolToObject(root, "supported", domain->supported);
    cJSON_AddStringToObject(root, "name", domain->name);
    cJSON_AddStringToObject(root, "governor", domain->governor);
    jw__json_add_int_or_null(root, "current_freq", domain->current_freq);
    jw__json_add_int_or_null(root, "set_freq", domain->set_freq);
    cJSON_AddStringToObject(root, "available_governors",
                            domain->available_governors);
    cJSON_AddStringToObject(root, "available_frequencies",
                            domain->available_frequencies);
    return root;
}

static int jw__reply_performance_status(jw_daemon_state *state,
                                        jw_ipc_client *client) {
    jw_platform_perf_status status;
    jw_platform_get_performance_status(&state->platform, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "performance-status");
    cJSON_AddBoolToObject(root, "supported", status.supported);
    cJSON_AddStringToObject(root, "active_profile",
                            jw_platform_perf_profile_name(state->perf_active_profile));
    cJSON_AddStringToObject(root, "global_profile",
                            jw_platform_perf_profile_name(state->perf_global_profile));
    cJSON_AddStringToObject(root, "session_profile",
                            jw_platform_perf_profile_name(state->perf_session_profile));
    cJSON_AddBoolToObject(root, "session_override", state->perf_session_override);
    cJSON_AddStringToObject(root, "message", status.message);
    cJSON_AddStringToObject(root, "last_error", state->perf_last_error);
    jw__json_add_int_or_null(root, "soc_temp_c", status.soc_temp_c);

    cJSON *domains = cJSON_CreateObject();
    if (domains) {
        cJSON_AddItemToObject(domains, "cpu",
                              jw__perf_domain_json(&status.domains[JW_PLATFORM_PERF_DOMAIN_CPU]));
        cJSON_AddItemToObject(domains, "gpu",
                              jw__perf_domain_json(&status.domains[JW_PLATFORM_PERF_DOMAIN_GPU]));
        cJSON_AddItemToObject(domains, "dmc",
                              jw__perf_domain_json(&status.domains[JW_PLATFORM_PERF_DOMAIN_DMC]));
        cJSON_AddItemToObject(root, "domains", domains);
    }
    return jw__reply_json(client, root);
}

static int jw__handle_performance_set_profile(jw_daemon_state *state,
                                              jw_ipc_client *client,
                                              cJSON *root) {
    cJSON *profile_json = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (!cJSON_IsString(profile_json) || !profile_json->valuestring) {
        return jw__reply_error(client, "missing performance profile");
    }

    jw_platform_perf_profile profile;
    if (!jw_platform_parse_perf_profile(profile_json->valuestring, &profile)) {
        return jw__reply_error(client, "unknown performance profile");
    }

    cJSON *scope_json = cJSON_GetObjectItemCaseSensitive(root, "scope");
    const char *scope = cJSON_IsString(scope_json) && scope_json->valuestring
                      ? scope_json->valuestring
                      : "session";

    if (strcmp(scope, "global") == 0) {
        if (profile == JW_PLATFORM_PERF_PROFILE_CUSTOM ||
            profile == JW_PLATFORM_PERF_PROFILE_SLEEP ||
            profile == JW_PLATFORM_PERF_PROFILE_FRONTEND) {
            return jw__reply_error(client, "profile cannot be global");
        }
        state->perf_global_profile = profile;
        (void)jw__perf_persist_global(state, profile);
        if (state->retroarch_session.active && !state->perf_session_override) {
            (void)jw__perf_apply_game(state, state->retroarch_session.system,
                                      "global-profile");
        }
        return jw__reply_ok(client, "performance-set-profile", NULL);
    }

    if (strcmp(scope, "session") != 0) {
        return jw__reply_error(client, "unknown performance scope");
    }
    if (profile == JW_PLATFORM_PERF_PROFILE_FRONTEND ||
        profile == JW_PLATFORM_PERF_PROFILE_SLEEP) {
        return jw__reply_error(client, "profile cannot be a session override");
    }
    state->perf_session_profile = profile;
    state->perf_session_override = true;
    state->perf_custom_valid = state->perf_custom_valid &&
                               profile == JW_PLATFORM_PERF_PROFILE_CUSTOM;
    if (state->retroarch_session.active) {
        (void)jw__perf_apply_game(state, state->retroarch_session.system,
                                  "session-profile");
    }
    return jw__reply_ok(client, "performance-set-profile", NULL);
}

static void jw__perf_parse_domain_request(cJSON *root,
                                          const char *prefix,
                                          jw_platform_perf_domain_request *out) {
    if (!root || !prefix || !out) {
        return;
    }
    char key[64];
    snprintf(key, sizeof(key), "%s_governor", prefix);
    cJSON *governor = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(governor) && governor->valuestring) {
        snprintf(out->governor, sizeof(out->governor), "%s", governor->valuestring);
    }
    snprintf(key, sizeof(key), "%s_frequency", prefix);
    cJSON *frequency = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(frequency)) {
        out->frequency = frequency->valueint;
    }
}

static int jw__handle_performance_set_custom(jw_daemon_state *state,
                                             jw_ipc_client *client,
                                             cJSON *root) {
    jw_platform_perf_request request;
    jw__perf_request_init(&request);
    jw__perf_parse_domain_request(root, "cpu",
                                  &request.domains[JW_PLATFORM_PERF_DOMAIN_CPU]);
    jw__perf_parse_domain_request(root, "gpu",
                                  &request.domains[JW_PLATFORM_PERF_DOMAIN_GPU]);
    jw__perf_parse_domain_request(root, "dmc",
                                  &request.domains[JW_PLATFORM_PERF_DOMAIN_DMC]);
    state->perf_custom_request = request;
    state->perf_custom_valid = true;
    state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_CUSTOM;
    state->perf_session_override = true;
    if (state->retroarch_session.active) {
        (void)jw__perf_apply_game(state, state->retroarch_session.system,
                                  "session-custom");
    }
    return jw__reply_ok(client, "performance-set-custom", NULL);
}

static int jw__handle_performance_reset_session(jw_daemon_state *state,
                                                jw_ipc_client *client) {
    state->perf_session_override = false;
    state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    state->perf_custom_valid = false;
    jw__perf_request_init(&state->perf_custom_request);
    (void)jw__perf_apply_current_context(state, "session-reset");
    return jw__reply_ok(client, "performance-reset-session", NULL);
}

static void jw__platform_sleep_with_performance(jw_daemon_state *state,
                                               jw_platform_result *out) {
    (void)jw__perf_apply_profile(state, JW_PLATFORM_PERF_PROFILE_SLEEP,
                                 NULL, "sleep");
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SLEEP, 0, out);
    (void)jw__perf_apply_current_context(state, "wake");
}

static void jw__cache_platform_status(jw_daemon_state *state,
                                      const jw_platform_status *status) {
    if (!state || !status) {
        return;
    }
    if (status->brightness_percent >= 0) {
        state->cached_brightness_percent =
            jw_platform_clamp_brightness_percent(status->brightness_percent);
    }
    if (status->volume_percent >= 0) {
        int volume = status->volume_percent;
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        state->cached_volume_percent = volume;
    }
}

static void jw__refresh_platform_cache(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);
}

/* Stored RetroAchievements credentials (Settings > Accounts). Resolved from the
   DB in the daemon parent, then applied to the environment of the forked
   RetroArch child only — never the long-lived daemon — so the plaintext
   password is not inherited by the launcher, OSD, ledd, or app-store apps. */
typedef struct {
    char user[64];
    char pass[128];
} jw_cheevos_creds;

/* Read the credentials from the DB. Opening SQLite happens in the parent before
   fork(); the resulting struct is applied child-side via jw__cheevos_apply_env. */
static void jw__cheevos_resolve(jw_daemon_state *state, jw_cheevos_creds *creds) {
    creds->user[0] = '\0';
    creds->pass[0] = '\0';
    if (state && state->db_path) {
        (void)jw_db_get_setting(state->db_path, "retroachievements_user",
                                creds->user, sizeof(creds->user));
        (void)jw_db_get_setting(state->db_path, "retroachievements_pass",
                                creds->pass, sizeof(creds->pass));
    }
}

/* Apply the credentials to the CURRENT process environment. The RetroArch
   session config writer (jw_prepare_retroarch_config) reads JAWAKA_CHEEVOS_* via
   getenv to put cheevos_username/password into the per-launch config that
   RetroArch validates at launch. Empty credentials clear the vars, leaving
   whatever the user configured inside RetroArch untouched. Because the writer
   runs in the daemon parent, callers there must clear the env again right after
   the config is written so the plaintext password does not persist. */
static void jw__cheevos_apply_env(const jw_cheevos_creds *creds) {
    if (creds->user[0] && creds->pass[0]) {
        setenv("JAWAKA_CHEEVOS_USERNAME", creds->user, 1);
        setenv("JAWAKA_CHEEVOS_PASSWORD", creds->pass, 1);
    } else {
        unsetenv("JAWAKA_CHEEVOS_USERNAME");
        unsetenv("JAWAKA_CHEEVOS_PASSWORD");
    }
}

/* Drop any cheevos credentials from the current process environment. Paired with
   jw__cheevos_apply_env around a config write in the parent. */
static void jw__cheevos_clear_env(void) {
    unsetenv("JAWAKA_CHEEVOS_USERNAME");
    unsetenv("JAWAKA_CHEEVOS_PASSWORD");
}

/* True when the pak dir refers to the bundled RetroArch app — the one app whose
   runner builds its own RetroArch config and so legitimately consumes the
   cheevos credentials. Case-insensitive match on "retroarch". */
static bool jw__pak_dir_is_retroarch(const char *pak_dir) {
    if (!pak_dir || !pak_dir[0]) {
        return false;
    }
    for (const char *p = pak_dir; *p; p++) {
        if ((*p == 'r' || *p == 'R') && strncasecmp(p, "retroarch", 9) == 0) {
            return true;
        }
    }
    return false;
}

static void jw__publish_audio_env(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    jw_platform_status status;
    jw_platform_get_audio_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);

    jw_platform_audio_output output = status.audio_output;
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        output = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    }
    const char *name = jw_platform_audio_output_name(output);
    const char *device = "default";
    setenv("UMRK_AUDIO_OUTPUT", name, 1);
    setenv("JAWAKA_AUDIO_OUTPUT", name, 1);
    setenv("UMRK_AUDIO_DEVICE", device, 1);
    setenv("JAWAKA_AUDIO_DEVICE", device, 1);
}

#define JW_AUDIO_RECONCILE_WAKE_THROTTLE_MS 1000

static void jw__reconcile_audio(jw_daemon_state *state, const char *reason,
                                bool throttle) {
    if (!state) {
        return;
    }

    long long now = jw__monotonic_ms();
    if (throttle) {
        if (state->audio_reconcile_last_ms > 0 &&
            now - state->audio_reconcile_last_ms < JW_AUDIO_RECONCILE_WAKE_THROTTLE_MS) {
            return;
        }
        state->audio_reconcile_last_ms = now;
    }
    jw_platform_audio_reconcile(&state->platform, reason);
}

static bool jw__retroarch_bluetooth_audio_active(jw_daemon_state *state) {
    if (!state) {
        return false;
    }

    jw_platform_status status;
    jw_platform_get_audio_status(&state->platform, &status);
    return status.audio_output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH;
}

static void jw__schedule_retroarch_audio_reinit(jw_daemon_state *state,
                                                const char *reason) {
    if (!state || !jw__has_retroarch_session(state) ||
        !state->retroarch_session.audio_bluetooth) {
        return;
    }

    long long now = jw__monotonic_ms();
    state->retroarch_audio_reinit_pending = true;
    state->retroarch_audio_reinit_next_ms = now;
    state->retroarch_audio_reinit_deadline_ms =
        now + JW_RETROARCH_AUDIO_REINIT_TIMEOUT_MS;
    snprintf(state->retroarch_audio_reinit_reason,
             sizeof(state->retroarch_audio_reinit_reason),
             "%s", (reason && reason[0]) ? reason : "unknown");
    jw_log_info("RetroArch audio: scheduled reinit after %s",
                state->retroarch_audio_reinit_reason);
}

static void jw__schedule_retroarch_audio_reinit_if_bluetooth(jw_daemon_state *state,
                                                             const char *reason) {
    if (!state || !jw__has_retroarch_session(state) ||
        !state->retroarch_session.audio_bluetooth) {
        return;
    }
    if (jw__retroarch_bluetooth_audio_active(state)) {
        jw__schedule_retroarch_audio_reinit(state, reason);
    }
}

static void jw__tick_retroarch_audio_reinit(jw_daemon_state *state) {
    if (!state || !state->retroarch_audio_reinit_pending) {
        return;
    }
    if (!jw__has_retroarch_session(state) ||
        !state->retroarch_session.audio_bluetooth) {
        state->retroarch_audio_reinit_pending = false;
        return;
    }

    long long now = jw__monotonic_ms();
    if (now < state->retroarch_audio_reinit_next_ms) {
        return;
    }
    if (state->retroarch_audio_reinit_deadline_ms > 0 &&
        now > state->retroarch_audio_reinit_deadline_ms) {
        jw_log_warn("RetroArch audio: Bluetooth reinit timed out after %s",
                    state->retroarch_audio_reinit_reason[0]
                        ? state->retroarch_audio_reinit_reason
                        : "unknown");
        state->retroarch_audio_reinit_pending = false;
        return;
    }

    jw__reconcile_audio(state, "retroarch-audio-reinit", false);
    if (!jw__retroarch_bluetooth_audio_active(state)) {
        state->retroarch_audio_reinit_next_ms =
            now + JW_RETROARCH_AUDIO_REINIT_RETRY_MS;
        return;
    }

    jw_ra_client client = jw_ra_client_default();
    jw_ra_result result = jw_ra_audio_reinit(&client);
    if (result == JW_RA_OK) {
        jw_log_info("RetroArch audio: reinit sent after %s",
                    state->retroarch_audio_reinit_reason[0]
                        ? state->retroarch_audio_reinit_reason
                        : "unknown");
        state->retroarch_audio_reinit_pending = false;
        return;
    }

    jw_log_warn("RetroArch audio: reinit failed result=%s",
                jw_ra_result_string(result));
    state->retroarch_audio_reinit_next_ms =
        now + JW_RETROARCH_AUDIO_REINIT_RETRY_MS;
}

/* Publish the live panel refresh (read from the active DRM mode) so the
   RetroArch config writer can pin video_refresh_rate to it. RA's pacing and
   Black Frame Insertion break when it believes 60Hz while the panel runs
   90/120. Re-read per launch so a runtime refresh-rate change is reflected. */
static void jw__publish_display_env(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    if (status.refresh_rate_hz > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", status.refresh_rate_hz);
        setenv("JAWAKA_REFRESH_RATE_HZ", buf, 1);
    } else {
        unsetenv("JAWAKA_REFRESH_RATE_HZ");
    }

    /* Black Frame Insertion: publish only when the user enabled it AND the panel
       is at 120Hz (one black frame per 60fps content frame). The RA config
       writer turns JAWAKA_BFI=1 into video_black_frame_insertion. At 60/90Hz
       BFI would flicker badly, so it stays off there regardless of the setting. */
    bool bfi_enabled = false;
    if (state->db_path) {
        char val[8] = "";
        if (jw_db_get_setting(state->db_path, "bfi_enabled", val, sizeof(val)) == 0 &&
            val[0] && strcmp(val, "0") != 0) {
            bfi_enabled = true;
        }
    }
    if (bfi_enabled && status.refresh_rate_hz == 120) {
        setenv("JAWAKA_BFI", "1", 1);
    } else {
        unsetenv("JAWAKA_BFI");
    }
}

static int jw__reply_platform_status(jw_daemon_state *state, jw_ipc_client *client) {
    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "platform-status");
    cJSON_AddStringToObject(root, "platform_id", state->platform.platform_id);
    cJSON_AddStringToObject(root, "platform_name", state->platform.platform_name);
    cJSON_AddStringToObject(root, "script_dir", state->platform.script_dir);
    cJSON_AddItemToObject(root, "capabilities",
                          jw__platform_capabilities_json(&state->platform.capabilities));
    cJSON *status_json = jw__platform_status_json(&status);
    if (state->led_configured) {
        cJSON *led = cJSON_CreateObject();
        cJSON_AddBoolToObject(led, "enabled", state->cached_led.enabled);
        cJSON_AddStringToObject(led, "mode", jw_led_mode_name(state->cached_led.mode));
        cJSON_AddNumberToObject(led, "r", state->cached_led.r);
        cJSON_AddNumberToObject(led, "g", state->cached_led.g);
        cJSON_AddNumberToObject(led, "b", state->cached_led.b);
        cJSON_AddNumberToObject(led, "brightness", state->cached_led.brightness);
        cJSON_AddNumberToObject(led, "speed", state->cached_led.speed);
        cJSON_AddItemToObject(status_json, "led", led);
    }
    cJSON_AddItemToObject(root, "status", status_json);
    return jw__reply_json(client, root);
}

static int jw__reply_platform_audio_status(jw_daemon_state *state, jw_ipc_client *client) {
    jw_platform_status status;
    jw_platform_get_audio_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "platform-audio-status");
    cJSON_AddItemToObject(root, "status", jw__platform_status_json(&status));
    return jw__reply_json(client, root);
}

static int jw__reply_update_status(jw_daemon_state *state, jw_ipc_client *client) {
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw_update_install_poll(&state->update_status, &state->update_install_job);
    jw_update_refresh_installed(&state->update_status, state->state_dir);
    if (!state->update_install_job.active) {
        jw_update_refresh_install_result(&state->update_status, state->state_dir,
                                         state->sdcard_root);
    }
    cJSON *root = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, root);
}

static int jw__reply_scrape_status(jw_ipc_client *client, cJSON *request) {
    jw_scrape_status_info info;
    jw_scrape_status(&info);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scrape-status");
    const char *state_name =
        info.state == JW_SCRAPE_RUNNING ? "running" :
        info.state == JW_SCRAPE_PAUSED_QUOTA ? "paused-quota" : "idle";
    cJSON_AddStringToObject(root, "state", state_name);
    cJSON_AddNumberToObject(root, "total", info.total);
    cJSON_AddNumberToObject(root, "done", info.done);
    cJSON_AddNumberToObject(root, "found", info.found);
    cJSON_AddNumberToObject(root, "not_found", info.not_found);
    cJSON_AddNumberToObject(root, "failed", info.failed);
    cJSON_AddNumberToObject(root, "cancelled", info.cancelled);
    cJSON_AddNumberToObject(root, "queued", info.queued);
    cJSON_AddNumberToObject(root, "active", info.active);
    cJSON_AddStringToObject(root, "current_name", info.current_name);
    cJSON_AddStringToObject(root, "current_system", info.current_system);
    cJSON_AddStringToObject(root, "message", info.message);

    /* Optional target: with "system" (and optionally "rom_path") in the
       request, also report whether that target has queued/in-flight work —
       drives the Actions menu's Scrape vs Cancel entries. */
    cJSON *system = request ? cJSON_GetObjectItemCaseSensitive(request, "system") : NULL;
    if (cJSON_IsString(system) && system->valuestring[0]) {
        cJSON *rom = cJSON_GetObjectItemCaseSensitive(request, "rom_path");
        bool pending = (cJSON_IsString(rom) && rom->valuestring[0])
            ? jw_scrape_is_pending_game(system->valuestring, rom->valuestring)
            : jw_scrape_is_pending_system(system->valuestring);
        cJSON_AddBoolToObject(root, "pending", pending);
    }
    return jw__reply_json(client, root);
}

static const char *jw__scrape_row_state_name(jw_scrape_row_state state) {
    switch (state) {
        case JW_SCRAPE_ROW_QUEUED:    return "queued";
        case JW_SCRAPE_ROW_HASH:      return "hashing";
        case JW_SCRAPE_ROW_SEARCH:    return "searching";
        case JW_SCRAPE_ROW_DOWNLOAD:  return "downloading";
        case JW_SCRAPE_ROW_SAVE:      return "saving";
        case JW_SCRAPE_ROW_DONE:      return "done";
        case JW_SCRAPE_ROW_NOT_FOUND: return "not-found";
        case JW_SCRAPE_ROW_ERROR:     return "error";
        case JW_SCRAPE_ROW_CANCELLED: return "cancelled";
        default:                      return "queued";
    }
}

static int jw__reply_scrape_queue(jw_ipc_client *client, cJSON *request) {
    int offset = 0;
    int limit = JW_SCRAPE_QUEUE_SNAPSHOT_MAX;
    cJSON *offset_json = request
        ? cJSON_GetObjectItemCaseSensitive(request, "offset") : NULL;
    cJSON *limit_json = request
        ? cJSON_GetObjectItemCaseSensitive(request, "limit") : NULL;
    if (cJSON_IsNumber(offset_json)) offset = offset_json->valueint;
    if (cJSON_IsNumber(limit_json)) limit = limit_json->valueint;

    jw_scrape_queue_info *info =
        (jw_scrape_queue_info *)calloc(1, sizeof(*info));
    if (!info) {
        return jw__reply_error(client, "out of memory");
    }
    jw_scrape_queue_snapshot(info, offset, limit);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scrape-queue");
    const char *state_name =
        info->state == JW_SCRAPE_RUNNING ? "running" :
        info->state == JW_SCRAPE_PAUSED_QUOTA ? "paused-quota" : "idle";
    cJSON_AddStringToObject(root, "state", state_name);
    cJSON_AddNumberToObject(root, "total", info->total);
    cJSON_AddNumberToObject(root, "done", info->done);
    cJSON_AddNumberToObject(root, "found", info->found);
    cJSON_AddNumberToObject(root, "not_found", info->not_found);
    cJSON_AddNumberToObject(root, "failed", info->failed);
    cJSON_AddNumberToObject(root, "cancelled", info->cancelled);
    cJSON_AddNumberToObject(root, "queued", info->queued);
    cJSON_AddNumberToObject(root, "active", info->active);
    cJSON_AddNumberToObject(root, "requests_today", info->requests_today);
    cJSON_AddNumberToObject(root, "max_requests", info->max_requests);
    cJSON_AddNumberToObject(root, "max_threads", info->max_threads);
    cJSON_AddNumberToObject(root, "permits", info->permits);
    cJSON_AddNumberToObject(root, "eta_seconds", info->eta_seconds);
    cJSON_AddStringToObject(root, "message", info->message);
    cJSON_AddNumberToObject(root, "offset", offset);
    cJSON_AddNumberToObject(root, "row_count", info->row_count);

    cJSON *rows = cJSON_CreateArray();
    for (int i = 0; i < info->row_count; i++) {
        jw_scrape_queue_row *row = &info->rows[i];
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", (double)row->id);
        cJSON_AddStringToObject(r, "state",
                                jw__scrape_row_state_name(row->state));
        cJSON_AddStringToObject(r, "display_name", row->display_name);
        cJSON_AddStringToObject(r, "system", row->system);
        cJSON_AddStringToObject(r, "rom_path", row->rom_path);
        cJSON_AddStringToObject(r, "output_path", row->output_path);
        cJSON_AddStringToObject(r, "message", row->message);
        cJSON_AddItemToArray(rows, r);
    }
    cJSON_AddItemToObject(root, "rows", rows);
    int rc = jw__reply_json(client, root);
    free(info);
    return rc;
}

static int jw__handle_scrape_start(jw_daemon_state *state,
                                   jw_ipc_client *client, cJSON *request) {
    (void)state;
    cJSON *scope = cJSON_GetObjectItemCaseSensitive(request, "scope");
    cJSON *system = cJSON_GetObjectItemCaseSensitive(request, "system");
    cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(request, "rom_path");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(request, "mode");
    if (!cJSON_IsString(scope)) {
        return jw__reply_error(client, "missing scope");
    }
    bool scope_all = strcmp(scope->valuestring, "all") == 0;
    if (!scope_all && (!cJSON_IsString(system) || !system->valuestring[0])) {
        return jw__reply_error(client, "missing system");
    }

    /* Gate on connectivity when the platform exposes Wi-Fi state, instead of
       letting the whole batch burn down as transport errors. */
    if (jw_wifi_available()) {
        jw_wifi_status_t wifi;
        if (jw_wifi_status(&wifi) == 0 && wifi.valid && !wifi.connected) {
            return jw__reply_error(client, "Wi-Fi is not connected");
        }
    }

    const char *error = NULL;
    int enqueued = -1;
    jw_scrape_enqueue_result result;
    memset(&result, 0, sizeof(result));
    if (strcmp(scope->valuestring, "game") == 0) {
        if (!cJSON_IsString(rom_path) || !rom_path->valuestring[0]) {
            return jw__reply_error(client, "missing rom_path");
        }
        enqueued = jw_scrape_enqueue_game(system->valuestring,
                                          rom_path->valuestring, &error);
        if (enqueued >= 0) {
            result.requested = 1;
            result.enqueued = enqueued;
            result.already_queued = enqueued == 0 ? 1 : 0;
        }
    } else if (strcmp(scope->valuestring, "system") == 0) {
        bool missing_only = !cJSON_IsString(mode) ||
                            strcmp(mode->valuestring, "all") != 0;
        enqueued = jw_scrape_enqueue_system_full(system->valuestring,
                                                 missing_only, &result,
                                                 &error);
    } else if (scope_all) {
        bool missing_only = !cJSON_IsString(mode) ||
                            strcmp(mode->valuestring, "all") != 0;
        enqueued = jw_scrape_enqueue_all_full(missing_only, &result, &error);
    } else {
        return jw__reply_error(client, "unknown scope");
    }

    if (enqueued < 0) {
        return jw__reply_error(client, error ? error : "scrape-start failed");
    }
    jw_log_info("scrape-start scope=%s system=%s requested=%d enqueued=%d already=%d skipped=%d full=%d",
                scope->valuestring,
                (cJSON_IsString(system) && system->valuestring[0])
                    ? system->valuestring : "*",
                result.requested, result.enqueued, result.already_queued,
                result.skipped_existing, result.queue_full ? 1 : 0);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-start");
    cJSON_AddNumberToObject(root, "requested", result.requested);
    cJSON_AddNumberToObject(root, "enqueued", result.enqueued);
    cJSON_AddNumberToObject(root, "already_queued", result.already_queued);
    cJSON_AddNumberToObject(root, "skipped_existing", result.skipped_existing);
    cJSON_AddBoolToObject(root, "queue_full", result.queue_full);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_missing_counts(jw_daemon_state *state,
                                            jw_ipc_client *client,
                                            cJSON *request) {
    (void)state;
    (void)request;
    enum { JW__MISSING_CAP = 256 };
    jw_scrape_missing_row *rows = calloc(JW__MISSING_CAP, sizeof(*rows));
    if (!rows) return jw__reply_error(client, "out of memory");

    int count = 0, total_missing = 0;
    if (jw_scrape_missing_counts(rows, JW__MISSING_CAP, &count,
                                 &total_missing) != 0) {
        free(rows);
        return jw__reply_error(client, "could not compute missing counts");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-missing-counts");
    cJSON_AddNumberToObject(root, "total_missing", total_missing);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "system", rows[i].system);
        cJSON_AddNumberToObject(o, "missing", rows[i].missing);
        cJSON_AddNumberToObject(o, "total", rows[i].total);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "systems", arr);
    free(rows);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_cancel(jw_daemon_state *state,
                                    jw_ipc_client *client, cJSON *request) {
    (void)state;
    cJSON *scope = cJSON_GetObjectItemCaseSensitive(request, "scope");
    cJSON *system = cJSON_GetObjectItemCaseSensitive(request, "system");
    cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(request, "rom_path");

    int removed = 0;
    if (!cJSON_IsString(scope) || strcmp(scope->valuestring, "all") == 0) {
        removed = jw_scrape_cancel_all();
    } else if (strcmp(scope->valuestring, "system") == 0 &&
               cJSON_IsString(system) && system->valuestring[0]) {
        removed = jw_scrape_cancel_system(system->valuestring);
    } else if (strcmp(scope->valuestring, "game") == 0 &&
               cJSON_IsString(system) && system->valuestring[0] &&
               cJSON_IsString(rom_path) && rom_path->valuestring[0]) {
        removed = jw_scrape_cancel_game(system->valuestring,
                                        rom_path->valuestring);
    } else {
        return jw__reply_error(client, "invalid cancel scope");
    }

    jw_log_info("scrape-cancel removed=%d", removed);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-cancel");
    cJSON_AddNumberToObject(root, "removed", removed);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_stop_all(jw_daemon_state *state,
                                      jw_ipc_client *client) {
    (void)state;
    int stopped = jw_scrape_stop_all();
    jw_log_info("scrape-stop-all stopped=%d", stopped);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-stop-all");
    cJSON_AddNumberToObject(root, "stopped", stopped);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_clear_done(jw_daemon_state *state,
                                        jw_ipc_client *client) {
    (void)state;
    int cleared = jw_scrape_clear_done();
    jw_log_info("scrape-clear-done cleared=%d", cleared);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-clear-done");
    cJSON_AddNumberToObject(root, "cleared", cleared);
    return jw__reply_json(client, root);
}

/* Validate ScreenScraper user credentials for the settings UI. Synchronous
   network call in the handler, same trade-off as update-check: a few seconds
   of daemon-loop blocking for a rare, user-initiated action. */
static int jw__handle_scrape_validate(jw_daemon_state *state,
                                      jw_ipc_client *client,
                                      cJSON *request) {
    (void)state;
    cJSON *user = cJSON_GetObjectItemCaseSensitive(request, "username");
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(request, "password");
    if (!cJSON_IsString(user) || !user->valuestring[0] ||
        !cJSON_IsString(pass) || !pass->valuestring[0]) {
        return jw__reply_error(client, "missing credentials");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scrape-validate");

    if (!jw_ss_available()) {
        cJSON_AddBoolToObject(root, "valid", false);
        cJSON_AddBoolToObject(root, "rejected", false);
        cJSON_AddStringToObject(root, "message",
                                "scraping unavailable in this build");
        return jw__reply_json(client, root);
    }

    jw_ss_client ss = {0};
    snprintf(ss.username, sizeof(ss.username), "%s", user->valuestring);
    snprintf(ss.password, sizeof(ss.password), "%s", pass->valuestring);

    jw_ss_user info;
    int rc = jw_ss_validate_user(&ss, &info);

    cJSON_AddBoolToObject(root, "valid", rc == 0);
    cJSON_AddBoolToObject(root, "rejected", rc == 1);
    if (rc == 0) {
        cJSON_AddNumberToObject(root, "max_threads", info.max_threads);
        cJSON_AddNumberToObject(root, "requests_today", info.requests_today);
        cJSON_AddNumberToObject(root, "max_requests", info.max_requests);
        cJSON_AddNumberToObject(root, "user_level", info.user_level);
        jw_log_info("scrape-validate ok user=%s maxthreads=%d",
                    ss.username, info.max_threads);
    } else {
        const char *msg = jw_ss_last_error();
        cJSON_AddStringToObject(root, "message",
                                msg ? msg : "validation failed");
        jw_log_info("scrape-validate failed user=%s rc=%d", ss.username, rc);
    }
    return jw__reply_json(client, root);
}

static int jw__reply_update_status_raw(jw_daemon_state *state,
                                       jw_ipc_client *client) {
    /* Used only while an async release check is active: preserve CHECKING without
       refreshing installed/result fields or touching download/install state. */
    cJSON *reply = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, reply);
}

static bool jw__update_check_busy(jw_daemon_state *state) {
    if (!state) {
        return false;
    }
    jw_update_check_poll(&state->update_status, &state->update_check_job);
    return state->update_check_job.active;
}

static int jw__handle_update_check(jw_daemon_state *state,
                                   jw_ipc_client *client,
                                   cJSON *request) {
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw_update_install_poll(&state->update_status, &state->update_install_job);
    jw__update_check_busy(state);
    if (state->update_download_job.active) {
        return jw__reply_update_status(state, client);
    }
    if (state->update_install_job.active) {
        return jw__reply_update_status(state, client);
    }
    if (state->update_check_job.active) {
        /* A check is already running; report CHECKING without starting another. */
        return jw__reply_update_status_raw(state, client);
    }

    cJSON *manifest_json = cJSON_GetObjectItemCaseSensitive(request, "manifest_path");
    const char *manifest_path = NULL;
    if (cJSON_IsString(manifest_json) && manifest_json->valuestring &&
        manifest_json->valuestring[0]) {
        manifest_path = manifest_json->valuestring;
    } else {
        const char *env = getenv("JAWAKA_UPDATE_MANIFEST");
        if (env && env[0]) {
            manifest_path = env;
        }
    }

    if (manifest_path && manifest_path[0]) {
        jw_update_check_local_manifest(&state->update_status,
                                       state->state_dir,
                                       state->platform.platform_id,
                                       manifest_path);
    } else {
        /* Run the GitHub release check on a worker thread so the blocking fetch
           doesn't freeze the launcher; reply immediately with state=checking and
           let the launcher's status poll pick up the result. */
        jw_update_check_start(&state->update_status,
                              &state->update_check_job,
                              state->state_dir);
    }
    cJSON *root = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, root);
}

static int jw__handle_update_download(jw_daemon_state *state,
                                      jw_ipc_client *client) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw_update_install_poll(&state->update_status, &state->update_install_job);
    if (!state->update_download_job.active &&
        !state->update_install_job.active &&
        state->update_status.status != JW_UPDATE_STATUS_DOWNLOADED) {
        jw_update_download_start(&state->update_status,
                                 &state->update_download_job,
                                 state->state_dir);
    }
    return jw__reply_update_status(state, client);
}

static int jw__handle_update_select(jw_daemon_state *state,
                                    jw_ipc_client *client,
                                    cJSON *request) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw_update_install_poll(&state->update_status, &state->update_install_job);
    if (state->update_download_job.active || state->update_install_job.active) {
        return jw__reply_update_status(state, client);
    }

    const cJSON *index_json = cJSON_GetObjectItemCaseSensitive(request, "option_index");
    int option_index = cJSON_IsNumber(index_json) ? index_json->valueint : -1;
    jw_update_select_option(&state->update_status, option_index);
    return jw__reply_update_status(state, client);
}

static int jw__handle_update_cancel(jw_daemon_state *state,
                                    jw_ipc_client *client) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw_update_install_poll(&state->update_status, &state->update_install_job);
    jw_update_download_cancel(&state->update_status, &state->update_download_job);
    return jw__reply_update_status(state, client);
}

static bool jw__update_install_idle(const jw_daemon_state *state) {
    if (!state) {
        return false;
    }
    if (state->shutdown_requested ||
        state->pending_launch ||
        state->pending_app ||
        state->post_launch_resume_pending ||
        state->in_game_menu_prewarm_pending ||
        state->retroarch_session.active ||
        state->menu_in_game ||
        state->menu_visible ||
        state->update_download_job.active ||
        state->update_install_job.active ||
        state->update_check_job.active) {
        return false;
    }

    return state->child_kind == JW_CHILD_NONE ||
           state->child_kind == JW_CHILD_LAUNCHER ||
           state->child_kind == JW_CHILD_MENU;
}

static int jw__handle_update_install_preflight(jw_daemon_state *state,
                                               jw_ipc_client *client,
                                               cJSON *request) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw_update_install_poll(&state->update_status, &state->update_install_job);

    bool confirm_unknown_battery =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,
                                                      "confirm_unknown_battery"));
    jw_platform_status platform_status;
    jw_platform_get_status(&state->platform, &platform_status);
    jw__cache_platform_status(state, &platform_status);

    jw_update_install_preflight(&state->update_status,
                                state->state_dir,
                                state->sdcard_root,
                                jw__update_install_idle(state),
                                platform_status.battery_percent,
                                platform_status.charging,
                                confirm_unknown_battery);
    return jw__reply_update_status(state, client);
}

static int jw__handle_update_install(jw_daemon_state *state,
                                     jw_ipc_client *client,
                                     cJSON *request) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw_update_install_poll(&state->update_status, &state->update_install_job);
    if (state->update_install_job.active) {
        return jw__reply_update_status(state, client);
    }

    bool confirm_unknown_battery =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,
                                                      "confirm_unknown_battery"));
    jw_platform_status platform_status;
    jw_platform_get_status(&state->platform, &platform_status);
    jw__cache_platform_status(state, &platform_status);

    jw_update_install_preflight(&state->update_status,
                                state->state_dir,
                                state->sdcard_root,
                                jw__update_install_idle(state),
                                platform_status.battery_percent,
                                platform_status.charging,
                                confirm_unknown_battery);

    if (state->update_status.install_ready) {
        char runner_path[PATH_MAX];
        if (snprintf(runner_path, sizeof(runner_path),
                     "%s/jawaka-update-runner", state->bin_dir) <
            (int)sizeof(runner_path)) {
            jw_update_install_start(&state->update_status,
                                    &state->update_install_job,
                                    state->state_dir,
                                    state->sdcard_root,
                                    runner_path);
        } else {
            state->update_status.status = JW_UPDATE_STATUS_ERROR;
            snprintf(state->update_status.message,
                     sizeof(state->update_status.message),
                     "%s", "Update runner path is too long");
        }
    }

    cJSON *root = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, root);
}

static int jw__reply_platform_result(jw_ipc_client *client, const char *action,
                                     const jw_platform_result *result) {
    bool ok = result && result->code == JW_PLATFORM_RESULT_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", ok ? "ok" : "error");
    if (action) {
        cJSON_AddStringToObject(root, "action", action);
    }
    cJSON_AddStringToObject(root, "code",
                            jw_platform_result_code_name(result ? result->code
                                                                : JW_PLATFORM_RESULT_FAILED));
    cJSON_AddStringToObject(root, "message", result ? result->message : "platform action failed");
    if (result && result->has_value) {
        cJSON_AddNumberToObject(root, "value", result->value);
    }
    return jw__reply_json(client, root);
}

static void jw__load_library_generation(jw_daemon_state *state) {
    if (!state || !state->db_path) {
        return;
    }
    char value[32];
    if (jw_db_get_setting(state->db_path, "library.generation",
                          value, sizeof(value)) == 0 && value[0]) {
        state->library_generation = atoi(value);
        if (state->library_generation < 0) {
            state->library_generation = 0;
        }
    }
}

static void jw__persist_library_generation(jw_daemon_state *state) {
    if (!state || !state->db_path) {
        return;
    }
    char value[32];
    snprintf(value, sizeof(value), "%d", state->library_generation);
    if (jw_db_set_setting(state->db_path, "library.generation", value) != 0) {
        jw_log_warn("could not persist library generation");
    }
}

static void jw__bump_library_generation(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    state->library_generation += 1;
    if (state->library_generation <= 0) {
        state->library_generation = 1;
    }
    jw__persist_library_generation(state);
}

static void jw__scan_job_init(jw_daemon_state *state) {
    if (!state || state->scan_job.initialized) {
        return;
    }
    pthread_mutex_init(&state->scan_job.mu, NULL);
    state->scan_job.initialized = true;
}

static void *jw__scan_job_main(void *arg) {
    jw_scan_job *job = (jw_scan_job *)arg;
    char db_path[PATH_MAX];
    char sdcard_root[PATH_MAX];
    char reason[96];

    pthread_mutex_lock(&job->mu);
    snprintf(db_path, sizeof(db_path), "%s", job->db_path);
    snprintf(sdcard_root, sizeof(sdcard_root), "%s", job->sdcard_root);
    snprintf(reason, sizeof(reason), "%s", job->reason);
    pthread_mutex_unlock(&job->mu);

    jw_scan_result result;
    memset(&result, 0, sizeof(result));
    char error[160] = "";
    int ok = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        snprintf(error, sizeof(error), "%s", "could not open scan database");
        if (db) {
            jw_db_close(db);
        }
        ok = 0;
    } else if (jw_scan_library(db, sdcard_root, &result) != 0) {
        snprintf(error, sizeof(error), "scan failed reason=%s",
                 reason[0] ? reason : "unknown");
        jw_db_close(db);
        ok = 0;
    } else {
        jw_db_close(db);
        ok = 1;
    }

    pthread_mutex_lock(&job->mu);
    job->result = result;
    job->ok = ok != 0;
    snprintf(job->error, sizeof(job->error), "%s",
             error[0] ? error : (ok ? "" : "scan failed"));
    job->finished_ms = jw__monotonic_ms();
    job->running = false;
    job->completed = true;
    pthread_mutex_unlock(&job->mu);
    return NULL;
}

static int jw__start_scan_job(jw_daemon_state *state, const char *reason) {
    if (!state || !state->scan_job.initialized) {
        return -1;
    }

    jw_scan_job *job = &state->scan_job;
    pthread_mutex_lock(&job->mu);
    if (job->running || job->completed) {
        job->pending_rescan = true;
        snprintf(job->pending_reason, sizeof(job->pending_reason), "%s",
                 reason && reason[0] ? reason : "pending");
        pthread_mutex_unlock(&job->mu);
        jw_log_info("scan-library queued reason=%s",
                    reason && reason[0] ? reason : "pending");
        return 1;
    }

    memset(&job->result, 0, sizeof(job->result));
    job->ok = false;
    job->completed = false;
    job->running = true;
    job->thread_started = true;
    job->started_ms = jw__monotonic_ms();
    job->finished_ms = 0;
    job->error[0] = '\0';
    snprintf(job->reason, sizeof(job->reason), "%s",
             reason && reason[0] ? reason : "requested");
    snprintf(job->db_path, sizeof(job->db_path), "%s", state->db_path);
    snprintf(job->sdcard_root, sizeof(job->sdcard_root), "%s", state->sdcard_root);

    int rc = pthread_create(&job->thread, NULL, jw__scan_job_main, job);
    if (rc != 0) {
        job->running = false;
        job->thread_started = false;
        snprintf(job->error, sizeof(job->error), "%s", "could not start scan worker");
        pthread_mutex_unlock(&job->mu);
        return -1;
    }
    char started_reason[96];
    snprintf(started_reason, sizeof(started_reason), "%s", job->reason);
    pthread_mutex_unlock(&job->mu);

    jw_log_info("scan-library started reason=%s", started_reason);
    return 0;
}

static void jw__tick_scan_job(jw_daemon_state *state) {
    if (!state || !state->scan_job.initialized) {
        return;
    }

    jw_scan_job *job = &state->scan_job;
    pthread_t thread;
    bool join = false;
    bool ok = false;
    bool pending = false;
    jw_scan_result result;
    char reason[96];
    char pending_reason[96];
    char error[160];
    long long elapsed_ms = 0;

    pthread_mutex_lock(&job->mu);
    if (job->completed && job->thread_started) {
        thread = job->thread;
        join = true;
        ok = job->ok;
        result = job->result;
        snprintf(reason, sizeof(reason), "%s", job->reason);
        snprintf(error, sizeof(error), "%s", job->error);
        pending = job->pending_rescan;
        snprintf(pending_reason, sizeof(pending_reason), "%s",
                 job->pending_reason[0] ? job->pending_reason : "pending");
        elapsed_ms = job->finished_ms > job->started_ms
                         ? job->finished_ms - job->started_ms
                         : 0;

        job->thread_started = false;
        job->completed = false;
        job->pending_rescan = false;
        job->pending_reason[0] = '\0';
    }
    pthread_mutex_unlock(&job->mu);

    if (!join) {
        return;
    }

    pthread_join(thread, NULL);

    if (ok) {
        jw__bump_library_generation(state);
        state->library_scanned_since_boot = true;
        state->library_populated = result.game_count > 0 || result.app_count > 0;
        jw_log_info("scan-library %s", reason[0] ? reason : "completed");
        jw_log_info("scan-library indexed %d games across %d systems and %d apps generation=%d",
                    result.game_count, result.system_count, result.app_count,
                    state->library_generation);
        jw_log_info("scan-library timings reason=%s total_ms=%lld",
                    reason[0] ? reason : "completed", elapsed_ms);
    } else {
        jw_log_warn("scan-library failed reason=%s error=%s",
                    reason[0] ? reason : "unknown",
                    error[0] ? error : "unknown");
    }

    if (pending && !state->shutdown_requested) {
        (void)jw__start_scan_job(state, pending_reason);
    }
}

static void jw__scan_job_shutdown(jw_daemon_state *state) {
    if (!state || !state->scan_job.initialized) {
        return;
    }

    jw_scan_job *job = &state->scan_job;
    pthread_t thread;
    bool join = false;
    pthread_mutex_lock(&job->mu);
    if (job->thread_started) {
        thread = job->thread;
        join = true;
        job->thread_started = false;
    }
    pthread_mutex_unlock(&job->mu);

    if (join) {
        jw_log_info("waiting for scan worker to finish");
        pthread_join(thread, NULL);
    }

    pthread_mutex_destroy(&job->mu);
    job->initialized = false;
}

static int jw__reply_library_status(jw_daemon_state *state, jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "library-status");
    cJSON_AddNumberToObject(root, "generation", state ? state->library_generation : 0);
    if (state && state->scan_job.initialized) {
        bool running = false;
        bool pending = false;
        char reason[96] = "";
        char error[160] = "";
        pthread_mutex_lock(&state->scan_job.mu);
        running = state->scan_job.running;
        pending = state->scan_job.pending_rescan;
        snprintf(reason, sizeof(reason), "%s", state->scan_job.reason);
        snprintf(error, sizeof(error), "%s", state->scan_job.error);
        pthread_mutex_unlock(&state->scan_job.mu);
        cJSON_AddBoolToObject(root, "scan_running", running);
        cJSON_AddBoolToObject(root, "pending_rescan", pending);
        cJSON_AddStringToObject(root, "scan_reason", reason);
        cJSON_AddStringToObject(root, "scan_error", error);
        cJSON_AddBoolToObject(root, "library_populated", state->library_populated);
    }
    return jw__reply_json(client, root);
}

static int jw__reply_storage_status(jw_daemon_state *state, jw_ipc_client *client,
                                    const char *source_id) {
    jw_platform_storage_status status;
    jw_platform_get_storage_status(&state->platform, source_id, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "storage-status");
    cJSON_AddStringToObject(root, "source", status.source_id);
    cJSON_AddStringToObject(root, "label", status.label);
    cJSON_AddStringToObject(root, "mount_path", status.mount_path);
    cJSON_AddStringToObject(root, "device_path", status.device_path);
    cJSON_AddBoolToObject(root, "present", status.present);
    cJSON_AddBoolToObject(root, "mounted", status.mounted);
    cJSON_AddBoolToObject(root, "busy", status.busy);
    cJSON_AddBoolToObject(root, "can_unmount", status.can_unmount);
    cJSON_AddStringToObject(root, "message", status.message);
    return jw__reply_json(client, root);
}

static int jw__handle_storage_action(jw_daemon_state *state, jw_ipc_client *client,
                                     cJSON *root) {
    cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root, "source");
    cJSON *action_json = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *source = cJSON_IsString(source_json) && source_json->valuestring
        ? source_json->valuestring
        : "secondary_sd";
    if (!cJSON_IsString(action_json) || !action_json->valuestring ||
        strcmp(action_json->valuestring, "safe-unmount") != 0) {
        return jw__reply_error(client, "missing storage action");
    }

    jw_platform_result result;
    jw_platform_safe_unmount_storage(&state->platform, source, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        int scan_rc = jw__start_scan_job(state, "after safe-unmount");
        if (scan_rc < 0) {
            jw_log_warn("safe-unmount: library rescan could not start");
        } else {
            snprintf(result.message, sizeof(result.message), "%s",
                     scan_rc > 0 ? "Secondary SD unmounted; library rescan queued"
                                 : "Secondary SD unmounted; library update started");
        }
    }

    jw_log_info("storage-action requested source=%s action=%s code=%s",
                source, action_json->valuestring,
                jw_platform_result_code_name(result.code));
    return jw__reply_platform_result(client, action_json->valuestring, &result);
}

static const char *jw__child_name(jw_child_kind kind) {
    switch (kind) {
        case JW_CHILD_LAUNCHER: return "jawaka-launcher";
        case JW_CHILD_MENU: return "jawaka-menu";
        case JW_CHILD_RETROARCH: return "RetroArch";
        case JW_CHILD_EMULATOR: return "standalone emulator";
        case JW_CHILD_APP: return "app";
        default: return NULL;
    }
}

static int jw__spawn_child(jw_daemon_state *state, jw_child_kind kind);
static int jw__spawn_in_game_menu(jw_daemon_state *state, bool show_now);
static int jw__spawn_osd(jw_daemon_state *state);
static int jw__spawn_retroarch(jw_daemon_state *state);
static int jw__spawn_standalone_emulator(jw_daemon_state *state,
                                         const jw_launch_target *target);
static int jw__spawn_pending_game(jw_daemon_state *state);
static int jw__spawn_app(jw_daemon_state *state);
static int jw__request_open_in_game_menu(jw_daemon_state *state);
static int jw__request_open_in_game_switcher(jw_daemon_state *state);
static int jw__request_close_in_game_menu(jw_daemon_state *state);
static void jw__handle_child_exit(jw_daemon_state *state);

static void jw__schedule_in_game_menu_prewarm(jw_daemon_state *state,
                                              long long delay_ms) {
    if (!state || state->menu_pid > 0) {
        return;
    }
    state->in_game_menu_prewarm_pending = true;
    state->in_game_menu_prewarm_next_ms = jw__monotonic_ms() + delay_ms;
    jw_log_info("scheduled in-game menu prewarm delay_ms=%lld", delay_ms);
}

static void jw__cancel_in_game_menu_prewarm(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    state->in_game_menu_prewarm_pending = false;
    state->in_game_menu_prewarm_next_ms = 0;
}

static void jw__tick_in_game_menu_prewarm(jw_daemon_state *state) {
    if (!state || !state->in_game_menu_prewarm_pending) {
        return;
    }
    if (!state->retroarch_session.active || state->child_kind != JW_CHILD_RETROARCH) {
        jw__cancel_in_game_menu_prewarm(state);
        return;
    }
    if (state->menu_pid > 0) {
        jw__cancel_in_game_menu_prewarm(state);
        return;
    }

    long long now = jw__monotonic_ms();
    if (state->in_game_menu_prewarm_next_ms > now) {
        return;
    }

    jw__cancel_in_game_menu_prewarm(state);
    if (jw__spawn_in_game_menu(state, false) != 0) {
        jw_log_warn("could not pre-spawn standby in-game menu; will spawn on demand");
    }
}

static void jw__publish_retroarch_input_env(jw_daemon_state *state) {
    if (!state || !state->input_proxy.enabled ||
        !state->input_proxy.virtual_event_path[0]) {
        unsetenv("CAT_INPUT_WAKE_EVENT");
        unsetenv("JAWAKA_INPUT_VIRTUAL_EVENT");
        unsetenv("JAWAKA_RETROARCH_VIRTUAL_EVENT");
        unsetenv("JAWAKA_RETROARCH_INPUT_DEVICE");
        unsetenv("JAWAKA_RETROARCH_JOYPAD_INDEX");
        return;
    }

    setenv("CAT_INPUT_WAKE_EVENT", state->input_proxy.virtual_event_path, 1);
    setenv("JAWAKA_INPUT_VIRTUAL_EVENT",
           state->input_proxy.virtual_event_path, 1);
    setenv("JAWAKA_RETROARCH_VIRTUAL_EVENT",
           state->input_proxy.virtual_event_path, 1);
    if (state->input_proxy.device_name[0]) {
        setenv("JAWAKA_RETROARCH_INPUT_DEVICE",
               state->input_proxy.device_name, 1);
    } else {
        unsetenv("JAWAKA_RETROARCH_INPUT_DEVICE");
    }

    int joypad_index = jw_input_proxy_retroarch_joypad_index(&state->input_proxy);
    if (joypad_index >= 0) {
        char joypad_text[16];
        snprintf(joypad_text, sizeof(joypad_text), "%d", joypad_index);
        setenv("JAWAKA_RETROARCH_JOYPAD_INDEX", joypad_text, 1);
        jw_log_info("RetroArch input proxy env: virtual=%s joypad_index=%d",
                    state->input_proxy.virtual_event_path, joypad_index);
    } else {
        unsetenv("JAWAKA_RETROARCH_JOYPAD_INDEX");
        jw_log_warn("RetroArch input proxy env: could not resolve joypad index for %s",
                    state->input_proxy.virtual_event_path);
    }
}

static void jw__publish_direct_input_env(void) {
    unsetenv("CAT_INPUT_WAKE_EVENT");
    unsetenv("JAWAKA_INPUT_VIRTUAL_EVENT");
    unsetenv("JAWAKA_RETROARCH_VIRTUAL_EVENT");
    unsetenv("JAWAKA_RETROARCH_INPUT_DEVICE");
    setenv("JAWAKA_RETROARCH_JOYPAD_INDEX", "0", 1);
}

static void jw__retroarch_session_clear(jw_retroarch_session *session) {
    if (!session) {
        return;
    }

    memset(session, 0, sizeof(*session));
}

static void jw__retroarch_session_start(jw_daemon_state *state, pid_t pid,
                                        const char *system, const char *rom_path,
                                        const char *db_rom_path,
                                        const char *source_root,
                                        const char *core_path,
                                        const char *core_id,
                                        const char *config_path,
                                        bool persist_config,
                                        bool audio_bluetooth) {
    if (!state || pid <= 0) {
        return;
    }

    jw_retroarch_session *session = &state->retroarch_session;
    jw__retroarch_session_clear(session);
    session->active = true;
    session->pid = pid;
    session->started_at = time(NULL);
    snprintf(session->system, sizeof(session->system), "%s", system ? system : "");
    snprintf(session->rom_path, sizeof(session->rom_path), "%s", rom_path ? rom_path : "");
    snprintf(session->db_rom_path, sizeof(session->db_rom_path), "%s",
             db_rom_path ? db_rom_path : "");
    snprintf(session->source_root, sizeof(session->source_root), "%s",
             source_root ? source_root : "");
    snprintf(session->core_path, sizeof(session->core_path), "%s", core_path ? core_path : "");
    snprintf(session->core_id, sizeof(session->core_id), "%s", core_id ? core_id : "");
    snprintf(session->config_path, sizeof(session->config_path), "%s",
             config_path ? config_path : "");
    session->persist_config = persist_config;
    session->audio_bluetooth = audio_bluetooth;

    /* Fresh session: no menu shown yet, reset the standby respawn guard. */
    state->menu_visible = false;
    state->menu_standby_attempts = 0;

    jw_log_info("RetroArch session started pid=%d system=%s source=%s core=%s core_id=%s config=%s rom=%s",
                (int)pid, session->system, session->source_root,
                session->core_path, session->core_id[0] ? session->core_id : "(unknown)",
                session->config_path, session->rom_path);
}

static long jw__retroarch_session_runtime_s(const jw_retroarch_session *session) {
    if (!session || session->started_at <= 0) {
        return 0;
    }

    time_t ended_at = time(NULL);
    if (ended_at == (time_t)-1 || ended_at < session->started_at) {
        return 0;
    }
    return (long)(ended_at - session->started_at);
}

static void jw__retroarch_session_record_play(jw_daemon_state *state,
                                              const jw_retroarch_session *session,
                                              long runtime_s) {
    if (!state || !session || runtime_s <= 0 ||
        !state->db_path || !session->rom_path[0]) {
        return;
    }

    const char *db_rom = session->db_rom_path[0]
        ? session->db_rom_path
        : session->rom_path;
    if (jw_db_record_play(state->db_path, db_rom, (int)runtime_s) == 0) {
        jw_log_info("recorded play rom=%s duration_s=%ld", db_rom, runtime_s);
    } else {
        jw_log_warn("could not record play for rom=%s", db_rom);
    }
}

/* On a standalone (mupen64plus) Menu+Select quit, the emulator drops
   "<runtime_dir>/standalone-switcher-request" (system + absolute ROM, one per
   line) before it exits. Consume it here, *before* the session is cleared:
   always unlink so a leftover can't leak to a later/foreign session, and honor
   it only when it belongs to the still-active mupen64plus session that just
   exited (core, ROM, and a mtime newer than the session start). On honor, arm
   the launcher to reopen straight into the switcher carousel seeded on this
   game, and make sure the game is at the front of Recents even for an instant
   chord (record_play skips runtime_s <= 0, so touch with duration 0 to bump
   last_opened without inflating playtime). */
static void jw__consume_standalone_switcher_marker(jw_daemon_state *state, pid_t exited_pid) {
    if (!state || !state->runtime_dir) {
        return;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/standalone-switcher-request",
                 state->runtime_dir) >= (int)sizeof(path)) {
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return;
    }

    char marker_system[64] = {0};
    char marker_rom[PATH_MAX] = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(marker_system, sizeof(marker_system), f)) {
            marker_system[strcspn(marker_system, "\r\n")] = '\0';
        }
        if (fgets(marker_rom, sizeof(marker_rom), f)) {
            marker_rom[strcspn(marker_rom, "\r\n")] = '\0';
        }
        fclose(f);
    }
    /* Consume unconditionally so it can never affect a later or foreign exit. */
    unlink(path);

    /* NOTE: jw__handle_child_exit() has already reset child_kind to
       JW_CHILD_NONE by the time we run, so jw__standalone_session_is_mupen64plus()
       (which gates on child_kind == JW_CHILD_EMULATOR) would wrongly report
       false. Check the still-intact session's core directly instead. */
    const jw_retroarch_session *session = &state->retroarch_session;
    bool session_is_mupen64plus = session->active &&
        (strcmp(session->core_id, "mupen64plus_standalone") == 0 ||
         strcmp(session->core_id, "mupen64plus") == 0 ||
         strstr(session->core_path, "/mupen64plus/") != NULL ||
         strstr(session->core_path, "/Mupen64Plus") != NULL);
    if (!session_is_mupen64plus || session->pid != exited_pid) {
        return;
    }

    bool rom_matches = marker_rom[0] &&
        (strcmp(marker_rom, session->rom_path) == 0 ||
         strcmp(marker_rom, session->db_rom_path) == 0);
    if (!rom_matches || st.st_mtime < session->started_at) {
        jw_log_warn("switcher marker rejected rom='%s' session_rom='%s' mtime=%ld started_at=%ld",
                    marker_rom, session->rom_path, (long)st.st_mtime,
                    (long)session->started_at);
        return;
    }

    if (jw__retroarch_session_runtime_s(session) <= 0 && state->db_path) {
        const char *db_rom = session->db_rom_path[0]
            ? session->db_rom_path
            : session->rom_path;
        if (db_rom[0]) {
            jw_db_record_play(state->db_path, db_rom, 0);
        }
    }

    state->launcher_open_switcher = true;
    snprintf(state->launcher_switcher_system, sizeof(state->launcher_switcher_system),
             "%s", marker_system[0] ? marker_system : session->system);
    snprintf(state->launcher_switcher_rom, sizeof(state->launcher_switcher_rom),
             "%s", session->rom_path);
    jw_log_info("switcher marker honored: reopen launcher in switcher system=%s rom=%s",
                state->launcher_switcher_system, state->launcher_switcher_rom);
}

static void jw__retroarch_session_retarget(jw_daemon_state *state,
                                           const char *system,
                                           const char *rom_path,
                                           const char *db_rom_path,
                                           const char *source_root,
                                           const char *core_path,
                                           const char *core_id) {
    if (!state || !state->retroarch_session.active) {
        return;
    }

    jw_retroarch_session *session = &state->retroarch_session;
    long runtime_s = jw__retroarch_session_runtime_s(session);
    jw__retroarch_session_record_play(state, session, runtime_s);
    int resident_switches = session->resident_switches + 1;

    session->started_at = time(NULL);
    snprintf(session->system, sizeof(session->system), "%s", system ? system : "");
    snprintf(session->rom_path, sizeof(session->rom_path), "%s", rom_path ? rom_path : "");
    snprintf(session->db_rom_path, sizeof(session->db_rom_path), "%s",
             db_rom_path ? db_rom_path : "");
    snprintf(session->source_root, sizeof(session->source_root), "%s",
             source_root ? source_root : "");
    snprintf(session->core_path, sizeof(session->core_path), "%s", core_path ? core_path : "");
    snprintf(session->core_id, sizeof(session->core_id), "%s", core_id ? core_id : "");
    session->resident_switches = resident_switches;

    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    state->post_launch_resume_next_ms = 0;
    state->retroarch_resume_on_menu_exit = false;
    state->menu_visible = false;
    state->menu_standby_attempts = 0;

    jw_log_info("RetroArch session retargeted in-process pid=%d runtime_s=%ld resident_switches=%d system=%s source=%s core=%s core_id=%s rom=%s",
                (int)session->pid, runtime_s, session->resident_switches,
                session->system, session->source_root,
                session->core_path, session->core_id[0] ? session->core_id : "(unknown)",
                session->rom_path);
}

static void jw__retroarch_session_finish(jw_daemon_state *state, pid_t pid, int status) {
    if (!state) {
        return;
    }

    jw__cancel_in_game_menu_prewarm(state);

    jw_retroarch_session *session = &state->retroarch_session;
    if (!session->active) {
        jw_log_warn("RetroArch child exited without active session pid=%d", (int)pid);
        return;
    }

    long runtime_s = jw__retroarch_session_runtime_s(session);
    if (session->pid != pid) {
        jw_log_warn("RetroArch session pid mismatch tracked=%d exited=%d",
                    (int)session->pid, (int)pid);
    }

    if (WIFEXITED(status)) {
        jw_log_info("RetroArch session ended pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, WEXITSTATUS(status),
                    session->system, session->rom_path);
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("RetroArch session terminated pid=%d runtime_s=%ld signal=%d system=%s rom=%s",
                    (int)pid, runtime_s, WTERMSIG(status),
                    session->system, session->rom_path);
    } else {
        jw_log_warn("RetroArch session changed state pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, status, session->system, session->rom_path);
    }

    bool switcher_transition = state->pending_launch && state->pending_launch_resume_switcher;
    if (session->config_path[0] && session->persist_config && !switcher_transition) {
        char error[256];
        if (jw_backup_retroarch_config(session->config_path, state->sdcard_root,
                                       error, sizeof(error)) != 0) {
            jw_log_warn("RetroArch shared config backup failed: %s",
                        error[0] ? error : session->config_path);
        } else {
            jw_log_info("RetroArch shared config backed up from %s", session->config_path);
        }
    } else if (session->config_path[0]) {
        jw_log_info("RetroArch shared config backup skipped persist=%s switcher_transition=%s",
                    session->persist_config ? "true" : "false",
                    switcher_transition ? "true" : "false");
    }

    /* Drop the per-session runtime config; it holds the plaintext cheevos
       password and is regenerated on the next launch. Ignore errors (the file
       may already be gone). */
    if (session->config_path[0]) {
        (void)unlink(session->config_path);
    }

    /* Record recents + playtime for real sessions only. A crash at launch gives
       runtime_s=0, so it never pollutes the list or playtime totals. The session
       stores the same path form that games.rom_path used at launch. */
    jw__retroarch_session_record_play(state, session, runtime_s);

    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    jw__retroarch_session_clear(session);
    if (!state->pending_launch) {
        state->perf_session_override = false;
        state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
        state->perf_custom_valid = false;
        jw__perf_request_init(&state->perf_custom_request);
        (void)jw__perf_apply_frontend(state, "retroarch-exit");
    }
}

static void jw__standalone_session_start(jw_daemon_state *state, pid_t pid,
                                         const char *system, const char *rom_path,
                                         const char *db_rom_path,
                                         const char *source_root,
                                         const char *launcher_path,
                                         const char *core_id) {
    if (!state || pid <= 0) {
        return;
    }

    jw_retroarch_session *session = &state->retroarch_session;
    jw__retroarch_session_clear(session);
    session->active = true;
    session->pid = pid;
    session->started_at = time(NULL);
    snprintf(session->system, sizeof(session->system), "%s", system ? system : "");
    snprintf(session->rom_path, sizeof(session->rom_path), "%s", rom_path ? rom_path : "");
    snprintf(session->db_rom_path, sizeof(session->db_rom_path), "%s",
             db_rom_path ? db_rom_path : "");
    snprintf(session->source_root, sizeof(session->source_root), "%s",
             source_root ? source_root : "");
    snprintf(session->core_path, sizeof(session->core_path), "%s",
             launcher_path ? launcher_path : "");
    snprintf(session->core_id, sizeof(session->core_id), "%s", core_id ? core_id : "");

    state->menu_visible = false;
    state->menu_standby_attempts = 0;
    state->standalone_quit_request_ms = 0;

    jw_log_info("standalone emulator session started pid=%d system=%s source=%s launcher=%s core_id=%s rom=%s",
                (int)pid, session->system, session->source_root,
                session->core_path, session->core_id[0] ? session->core_id : "(unknown)",
                session->rom_path);
}

static void jw__standalone_session_finish(jw_daemon_state *state, pid_t pid, int status) {
    if (!state) {
        return;
    }
    state->standalone_quit_request_ms = 0;

    jw_retroarch_session *session = &state->retroarch_session;
    if (!session->active) {
        jw_log_warn("standalone emulator child exited without active session pid=%d", (int)pid);
        return;
    }

    long runtime_s = jw__retroarch_session_runtime_s(session);
    if (session->pid != pid) {
        jw_log_warn("standalone emulator session pid mismatch tracked=%d exited=%d",
                    (int)session->pid, (int)pid);
    }

    if (WIFEXITED(status)) {
        jw_log_info("standalone emulator session ended pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, WEXITSTATUS(status),
                    session->system, session->rom_path);
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("standalone emulator session terminated pid=%d runtime_s=%ld signal=%d system=%s rom=%s",
                    (int)pid, runtime_s, WTERMSIG(status),
                    session->system, session->rom_path);
    } else {
        jw_log_warn("standalone emulator session changed state pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, status, session->system, session->rom_path);
    }

    jw__retroarch_session_record_play(state, session, runtime_s);
    jw__retroarch_session_clear(session);
    state->perf_session_override = false;
    state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    state->perf_custom_valid = false;
    jw__perf_request_init(&state->perf_custom_request);
    (void)jw__perf_apply_frontend(state, "standalone-emulator-exit");
}

static int jw__request_open_menu(jw_daemon_state *state) {
    if (!state) {
        return -1;
    }

    if (state->child_pid <= 0) {
        return jw__spawn_child(state, JW_CHILD_MENU);
    }

    state->pending_menu = true;
    return 0;
}

/* Select which surface the resident in-game UI shows on its next reveal. The
   menu process reads this on each show wake; missing/invalid defaults to "menu".
   Written before SIGUSR1 (and before a cold spawn) so the reveal picks it up. */
static void jw__write_ingame_ui_mode(const char *mode) {
    char *path = jw_ingame_ui_mode_path();
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(mode, f);
        fclose(f);
    } else {
        jw_log_warn("in-game ui: could not write mode file %s: %s",
                    path, strerror(errno));
    }
    free(path);
}

/* Reveal the resident in-game UI in either "menu" or "switcher" mode. Pauses
   RetroArch, records the desired mode, then reveals the warm standby (SIGUSR1)
   or cold-spawns it. Reversible: this never saves or quits. */
static int jw__request_open_in_game_ui(jw_daemon_state *state, const char *mode) {
    long long start_ms = jw__monotonic_ms();
    if (!jw__has_retroarch_session(state)) {
        return -1;
    }

    /* Already showing: ignore the request (the surface is closed with B/Continue
       or the Menu toggle). */
    if (state->menu_visible) {
        return 0;
    }

    jw_ra_client client = jw_ra_client_default();
    long long pause_start_ms = jw__monotonic_ms();
    jw_ra_result pause_result = jw_ra_pause_direct(&client);
    long long pause_done_ms = jw__monotonic_ms();
    if (pause_result != JW_RA_OK) {
        jw_log_warn("in-game menu: pause failed result=%s",
                    jw_ra_result_string(pause_result));
        return -1;
    }

    /* Tell the resident UI which surface to show before we wake it. */
    jw__write_ingame_ui_mode(mode);

    /* No screenshot round-trip: the resident menu grabs the paused frame itself
       from the DRM scanout (kmsgrab) before it maps, so the background is in the
       first visible frame. RetroArch is already paused above. */
    bool warm = state->menu_pid > 0;
    long long show_start_ms = jw__monotonic_ms();
    if (warm) {
        /* Warm standby is resident: reveal it with a show signal. Near-instant —
           no fork/exec, no cat_init. This is the common path. */
        state->menu_visible = true;
        if (kill(state->menu_pid, SIGUSR1) != 0) {
            jw_log_warn("in-game menu: show signal failed pid=%d: %s",
                        (int)state->menu_pid, strerror(errno));
            state->menu_visible = false;
            jw_ra_resume_direct(&client);
            return -1;
        }
    } else {
        /* No standby (pre-spawn failed or it crashed): spawn on demand and let
           it reveal itself once initialized. Pays the old cold-start cost, but
           only in this rare fallback. */
        if (jw__spawn_in_game_menu(state, true) != 0) {
            jw_ra_resume_direct(&client);
            return -1;
        }
        state->menu_visible = true;
    }
    long long done_ms = jw__monotonic_ms();
    jw_log_info("in-game ui open timings: mode=%s pause_ms=%lld show_ms=%lld total_ms=%lld standby=%d",
                mode,
                pause_done_ms - pause_start_ms,
                done_ms - show_start_ms,
                done_ms - start_ms,
                warm);

    return 0;
}

static int jw__request_open_in_game_menu(jw_daemon_state *state) {
    return jw__request_open_in_game_ui(state, "menu");
}

static int jw__request_open_in_game_switcher(jw_daemon_state *state) {
    return jw__request_open_in_game_ui(state, "switcher");
}

/* Unpause the game when leaving the in-game menu. First release any button still
   held on the virtual pad — the press that triggered the menu action (e.g. A on
   Save State) is usually still physically down, and without this the core reads
   it as a fresh in-game input on resume (the "character jumps after save state"
   bug). evdev is edge-based, so a still-held physical button won't re-fire until
   it is released and pressed again. */
static jw_ra_result jw__resume_game_after_menu(jw_daemon_state *state, jw_ra_client *ra) {
    jw_input_proxy_release_buttons(&state->input_proxy);
    return jw_ra_resume_direct(ra);
}

/* Close the visible standby menu (the Menu button toggles it shut): resume the
   game and tell the menu to hide back to standby. Mirror image of the open
   path; uses explicit UNPAUSE so the game always resumes. */
static int jw__request_close_in_game_menu(jw_daemon_state *state) {
    if (!state || !state->menu_visible) {
        return -1;
    }

    jw_ra_client client = jw_ra_client_default();
    jw__resume_game_after_menu(state, &client);
    state->menu_visible = false;
    if (state->menu_pid > 0) {
        kill(state->menu_pid, SIGUSR2);
    }
    jw_log_info("in-game menu closed via Menu toggle");
    return 0;
}

static int jw__resolve_rom_path(const jw_daemon_state *state, const char *rom_path,
                                char *out, size_t out_size) {
    if (!state || !rom_path || !rom_path[0] || !out || out_size == 0) {
        return -1;
    }

    char candidate[PATH_MAX];
    if (rom_path[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", rom_path);
    } else {
        if (snprintf(candidate, sizeof(candidate), "%s/%s", state->sdcard_root, rom_path) >= (int)sizeof(candidate)) {
            return -1;
        }
    }

    char resolved[PATH_MAX];
    if (!realpath(candidate, resolved)) {
        return -1;
    }

    if (snprintf(out, out_size, "%s", resolved) >= (int)out_size) {
        return -1;
    }
    return 0;
}

static int jw__storage_sources(jw_daemon_state *state, jw_storage_source_list *out) {
    if (!state || !out) {
        return -1;
    }
    return jw_storage_sources_resolve(state->sdcard_root, out);
}

static bool jw__same_resolved_path(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    if (strcmp(a, b) == 0) {
        return true;
    }

    char real_a[PATH_MAX];
    char real_b[PATH_MAX];
    if (realpath(a, real_a) && realpath(b, real_b)) {
        return strcmp(real_a, real_b) == 0;
    }
    return false;
}

static int jw__wait_for_retroarch_content(const jw_ra_client *ra,
                                          const char *rom_abs,
                                          long long timeout_ms) {
    if (!ra || !rom_abs || !rom_abs[0]) {
        return -1;
    }

    long long deadline = jw__monotonic_ms() + timeout_ms;
    for (;;) {
        jw_ra_client poll = *ra;
        poll.timeout_ms = 100u;

        char content[PATH_MAX];
        jw_ra_result result = jw_ra_get_path(&poll, "content",
                                             content, sizeof(content));
        if (result == JW_RA_OK && jw__same_resolved_path(content, rom_abs)) {
            return 0;
        }

        long long now = jw__monotonic_ms();
        if (now >= deadline) {
            jw_log_warn("resident switch: content wait timed out target=%s last_result=%s last_content=%s",
                        rom_abs, jw_ra_result_string(result),
                        result == JW_RA_OK ? content : "");
            return -1;
        }
        usleep(50000);
    }
}

static void jw__publish_source_content_env(const jw_storage_source *source) {
    if (!source) {
        return;
    }
    /* Keep SDCARD_PATH/JAWAKA_SDCARD_ROOT pointed at the primary card so app
       durable state follows USERDATA_PATH. Only content roots become source-specific. */
    setenv("ROMS_PATH", source->roms_path, 1);
    setenv("IMAGES_PATH", source->images_path, 1);
    setenv("APPS_PATH", source->apps_path, 1);
    setenv("BIOS_PATH", source->bios_path, 1);
    setenv("SAVES_PATH", source->saves_path, 1);
    setenv("STATES_PATH", source->states_path, 1);
    setenv("CHEATS_PATH", source->cheats_path, 1);
}

typedef struct {
    const char *name;
    char *value;
    bool had_value;
} jw_saved_env;

static void jw__save_env(jw_saved_env *saved, const char *name) {
    if (!saved || !name) {
        return;
    }
    saved->name = name;
    const char *value = getenv(name);
    saved->had_value = value != NULL;
    saved->value = value ? strdup(value) : NULL;
}

static void jw__restore_env(jw_saved_env *saved, int count) {
    if (!saved || count <= 0) {
        return;
    }
    for (int i = count - 1; i >= 0; i--) {
        if (!saved[i].name) {
            continue;
        }
        if (saved[i].had_value) {
            setenv(saved[i].name, saved[i].value ? saved[i].value : "", 1);
        } else {
            unsetenv(saved[i].name);
        }
        free(saved[i].value);
        saved[i].value = NULL;
        saved[i].had_value = false;
    }
}

static void jw__publish_retroarch_source_dirs(const jw_storage_source *source) {
    if (!source) {
        return;
    }
    setenv("BIOS_PATH", source->bios_path, 1);
    setenv("SAVES_PATH", source->saves_path, 1);
    setenv("STATES_PATH", source->states_path, 1);
}

static int jw__path_is_within(const char *path, const char *root) {
    if (!path || !root) {
        return 0;
    }
    size_t root_len = strlen(root);
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static int jw__resolve_app_launch_path(jw_daemon_state *state, const char *pak_dir,
                                       char *pak_abs, size_t pak_abs_size,
                                       char *launch_abs, size_t launch_abs_size,
                                       const char **out_error) {
    if (!state || !pak_dir || !pak_dir[0] || !pak_abs || !launch_abs) {
        if (out_error) *out_error = "missing app payload";
        return -1;
    }

    jw_storage_source_list sources;
    if (jw__storage_sources(state, &sources) != 0) {
        if (out_error) *out_error = "storage sources unavailable";
        return -1;
    }
    const jw_storage_source *source = NULL;
    char candidate[PATH_MAX];
    if (pak_dir[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", pak_dir);
    } else {
        source = jw_storage_sources_primary(&sources);
        if (!source) {
            if (out_error) *out_error = "primary storage source missing";
            return -1;
        }
        const char *rel = strncmp(pak_dir, "Apps/", 5) == 0 ? pak_dir + 5 : pak_dir;
        if (snprintf(candidate, sizeof(candidate), "%s/%s", source->apps_path, rel) >=
            (int)sizeof(candidate)) {
            if (out_error) *out_error = "app path too long";
            return -1;
        }
    }

    char resolved_pak[PATH_MAX];
    if (!realpath(candidate, resolved_pak)) {
        if (out_error) *out_error = "app pak missing";
        return -1;
    }

    if (!source) {
        source = jw_storage_sources_find_for_path(&sources, resolved_pak);
    }
    if (!source) {
        if (out_error) *out_error = "app storage source missing";
        return -1;
    }

    char apps_abs[PATH_MAX];
    if (!realpath(source->apps_path, apps_abs)) {
        if (out_error) *out_error = "Apps directory missing";
        return -1;
    }

    if (!jw__path_is_within(resolved_pak, apps_abs)) {
        if (out_error) *out_error = "app pak outside Apps";
        return -1;
    }

    char launch_candidate[PATH_MAX];
    if (snprintf(launch_candidate, sizeof(launch_candidate), "%s/launch.sh", resolved_pak) >= (int)sizeof(launch_candidate)) {
        if (out_error) *out_error = "app launch path too long";
        return -1;
    }

    if (!jw__is_regular_file(launch_candidate)) {
        if (out_error) *out_error = "app launch.sh missing or not executable";
        return -1;
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(launch_candidate, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch app from SD: %s", exec_error);
        if (out_error) *out_error = "SD-card mounted noexec; switcher remount failed or regressed";
        return -1;
    }

    if (access(launch_candidate, X_OK) != 0) {
        if (out_error) *out_error = "app launch.sh missing or not executable";
        return -1;
    }

    if (snprintf(pak_abs, pak_abs_size, "%s", resolved_pak) >= (int)pak_abs_size ||
        snprintf(launch_abs, launch_abs_size, "%s", launch_candidate) >= (int)launch_abs_size) {
        if (out_error) *out_error = "app path too long";
        return -1;
    }

    return 0;
}

static int jw__lookup_launch_game(jw_daemon_state *state, const char *rom_path,
                                  jw_game_entry *out) {
    if (!state || !state->db_path || !rom_path || !rom_path[0] || !out) {
        return -1;
    }
    if (jw_db_get_game_by_rom_path(state->db_path, rom_path, out) == 0) {
        return 0;
    }

    char rel_path[PATH_MAX];
    if (state->sdcard_root && state->sdcard_root[0]) {
        size_t root_len = strlen(state->sdcard_root);
        if (strncmp(rom_path, state->sdcard_root, root_len) == 0 &&
            rom_path[root_len] == '/') {
            snprintf(rel_path, sizeof(rel_path), "%s", rom_path + root_len + 1);
            return jw_db_get_game_by_rom_path(state->db_path, rel_path, out);
        }
    }
    return -1;
}

static const char *jw__launch_system_key(const char *requested_system,
                                         const jw_game_entry *game) {
    if (game && game->system[0]) {
        return game->system;
    }
    return requested_system;
}

static char *jw__resolve_launch_core_path(jw_daemon_state *state,
                                          const char *system,
                                          const char *rom_path,
                                          char *out_core_id,
                                          size_t out_core_id_size,
                                          char *diagnostic,
                                          size_t diagnostic_size) {
    if (out_core_id && out_core_id_size > 0) {
        out_core_id[0] = '\0';
    }
    if (diagnostic && diagnostic_size > 0) {
        diagnostic[0] = '\0';
    }

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system, have_game ? &game : NULL);

    char preferred[64];
    preferred[0] = '\0';
    const char *source = NULL;
    if (have_game && game.id > 0 && state && state->db_path) {
        if (jw_db_get_game_setting(state->db_path, game.id,
                                   JW_CONTENT_SETTING_CORE_ID,
                                   preferred, sizeof(preferred)) == 0 &&
            preferred[0]) {
            source = "game";
        }
    }
    if (!preferred[0] && state && state->db_path && system_key && system_key[0]) {
        if (jw_db_get_system_setting(state->db_path, system_key,
                                     JW_CONTENT_SETTING_CORE_ID,
                                     preferred, sizeof(preferred)) == 0 &&
            preferred[0]) {
            source = "system";
        }
    }

    char *core = jw_retroarch_core_path_for_system_choice(system_key,
                                                          preferred[0] ? preferred : NULL,
                                                          out_core_id,
                                                          out_core_id_size,
                                                          diagnostic,
                                                          diagnostic_size);
    if (core && source && preferred[0]) {
        jw_log_info("launch resolver: %s core override system=%s rom=%s preferred=%s effective=%s",
                    source, system_key ? system_key : "(none)",
                    rom_path ? rom_path : "(none)", preferred,
                    out_core_id && out_core_id[0] ? out_core_id : "(unknown)");
    }
    if (core && diagnostic && diagnostic[0]) {
        jw_log_warn("launch resolver: %s", diagnostic);
    }
    return core;
}

static bool jw__catalog_system_allows_core(const jw_ra_system *system,
                                           const char *core_id) {
    if (!system || !core_id || !core_id[0]) {
        return false;
    }
    if (system->default_core && strcmp(system->default_core, core_id) == 0) {
        return true;
    }
    return jw_ra_string_list_contains(&system->alternate_cores, core_id);
}

static bool jw__core_is_packaged_path(const jw_ra_core *core) {
    return core &&
           core->type &&
           strcmp(core->type, "path") == 0 &&
           core->status &&
           strcmp(core->status, "packaged") == 0 &&
           core->path &&
           core->path[0];
}

static int jw__platform_path(char *out, size_t out_size, const jw_daemon_state *state) {
    const char *platform_path = jw__env_value("UMRK_PLATFORM_PATH");
    if (!platform_path) {
        platform_path = jw__env_value("SYSTEM_PATH");
    }
    if (platform_path) {
        return snprintf(out, out_size, "%s", platform_path) < (int)out_size ? 0 : -1;
    }
    if (!state || !state->sdcard_root || !state->sdcard_root[0]) {
        return -1;
    }
    return snprintf(out, out_size, "%s/.system/leaf/platforms/mlp1",
                    state->sdcard_root) < (int)out_size ? 0 : -1;
}

static int jw__resolve_path_core_executable(const jw_daemon_state *state,
                                            const jw_ra_core *core,
                                            char *out,
                                            size_t out_size) {
    if (!jw__core_is_packaged_path(core) || !out || out_size == 0) {
        return -1;
    }

    char candidate[PATH_MAX];
    if (core->path[0] == '/') {
        if (snprintf(candidate, sizeof(candidate), "%s", core->path) >=
            (int)sizeof(candidate)) {
            return -1;
        }
    } else {
        char platform_path[PATH_MAX];
        if (jw__platform_path(platform_path, sizeof(platform_path), state) != 0 ||
            snprintf(candidate, sizeof(candidate), "%s/%s",
                     platform_path, core->path) >= (int)sizeof(candidate)) {
            return -1;
        }
    }

    if (access(candidate, X_OK) != 0) {
        return -1;
    }
    return snprintf(out, out_size, "%s", candidate) < (int)out_size ? 0 : -1;
}

static const jw_ra_system *jw__catalog_find_launch_system(const jw_ra_catalog *catalog,
                                                          const char *system_id) {
    const jw_ra_system *system = jw_ra_catalog_find_system(catalog, system_id);
    if (!system) {
        system = jw_ra_catalog_match_system_folder(catalog, system_id);
    }
    return system;
}

static bool jw__try_path_core(const jw_daemon_state *state,
                              const jw_ra_catalog *catalog,
                              const jw_ra_core *core,
                              jw_launch_target *target) {
    if (!target || !jw__core_is_packaged_path(core)) {
        return false;
    }

    char exec_path[PATH_MAX];
    if (jw__resolve_path_core_executable(state, core, exec_path, sizeof(exec_path)) != 0) {
        return false;
    }

    memset(target, 0, sizeof(*target));
    (void)catalog;
    target->kind = JW_LAUNCH_TARGET_STANDALONE;
    snprintf(target->path, sizeof(target->path), "%s", exec_path);
    snprintf(target->core_id, sizeof(target->core_id), "%s", core->id ? core->id : "");
    return true;
}

static bool jw__resolve_standalone_launch_target(jw_daemon_state *state,
                                                 const char *system,
                                                 const char *rom_path,
                                                 jw_launch_target *target) {
    if (!state || !target || !system || !system[0]) {
        return false;
    }

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system, have_game ? &game : NULL);

    char preferred[64];
    preferred[0] = '\0';
    if (have_game && game.id > 0 && state->db_path) {
        (void)jw_db_get_game_setting(state->db_path, game.id,
                                     JW_CONTENT_SETTING_CORE_ID,
                                     preferred, sizeof(preferred));
    }
    if (!preferred[0] && state->db_path && system_key && system_key[0]) {
        (void)jw_db_get_system_setting(state->db_path, system_key,
                                       JW_CONTENT_SETTING_CORE_ID,
                                       preferred, sizeof(preferred));
    }

    char error[256];
    const jw_ra_catalog *catalog = jw_ra_catalog_get(state->sdcard_root,
                                                     error, sizeof(error));
    if (!catalog) {
        if (error[0]) {
            jw_log_warn("standalone launch metadata unavailable: %s", error);
        }
        return false;
    }

    const jw_ra_system *ra_system = jw__catalog_find_launch_system(catalog, system_key);
    if (!ra_system) {
        return false;
    }

    if (preferred[0] && jw__catalog_system_allows_core(ra_system, preferred)) {
        const jw_ra_core *core = jw_ra_catalog_find_core(catalog, preferred);
        if (jw__try_path_core(state, catalog, core, target)) {
            return true;
        }
        if (core && core->type && strcmp(core->type, "retroarch") == 0 &&
            core->status && strcmp(core->status, "packaged") == 0) {
            return false;
        }
    }

    const jw_ra_core *core = jw_ra_catalog_find_core(catalog, ra_system->default_core);
    if (jw__try_path_core(state, catalog, core, target)) {
        return true;
    }

    for (size_t i = 0; i < ra_system->alternate_cores.count; i++) {
        core = jw_ra_catalog_find_core(catalog, ra_system->alternate_cores.items[i]);
        if (jw__try_path_core(state, catalog, core, target)) {
            return true;
        }
    }

    return false;
}

static int jw__resolve_launch_target(jw_daemon_state *state,
                                     const char *system,
                                     const char *rom_path,
                                     jw_launch_target *target) {
    if (!target) {
        return -1;
    }
    memset(target, 0, sizeof(*target));

    if (jw__resolve_standalone_launch_target(state, system, rom_path, target)) {
        return 0;
    }

    char core_id[64];
    char diagnostic[256];
    char *core = jw__resolve_launch_core_path(state, system, rom_path,
                                              core_id, sizeof(core_id),
                                              diagnostic, sizeof(diagnostic));
    if (!core) {
        if (diagnostic[0]) {
            snprintf(target->diagnostic, sizeof(target->diagnostic), "%s", diagnostic);
        }
        return -1;
    }

    target->kind = JW_LAUNCH_TARGET_RETROARCH;
    snprintf(target->path, sizeof(target->path), "%s", core);
    snprintf(target->core_id, sizeof(target->core_id), "%s", core_id);
    if (diagnostic[0]) {
        snprintf(target->diagnostic, sizeof(target->diagnostic), "%s", diagnostic);
    }
    free(core);
    return 0;
}

static jw_platform_perf_profile jw__perf_requested_for_launch(
        jw_daemon_state *state, const char *system, const char *rom_path) {
    if (!state) {
        return JW_PLATFORM_PERF_PROFILE_AUTO;
    }
    if (state->perf_session_override) {
        return state->perf_session_profile;
    }

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system, have_game ? &game : NULL);

    char value[64];
    if (have_game && game.id > 0 && state->db_path &&
        jw_db_get_game_setting(state->db_path, game.id,
                               JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                               value, sizeof(value)) == 0 &&
        value[0]) {
        jw_platform_perf_profile profile;
        if (jw_platform_parse_perf_profile(value, &profile)) {
            return profile;
        }
        jw_log_warn("performance: ignoring invalid game override %s for rom=%s",
                    value, rom_path ? rom_path : "(none)");
    }

    if (state->db_path && system_key && system_key[0] &&
        jw_db_get_system_setting(state->db_path, system_key,
                                 JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                 value, sizeof(value)) == 0 &&
        value[0]) {
        jw_platform_perf_profile profile;
        if (jw_platform_parse_perf_profile(value, &profile)) {
            return profile;
        }
        jw_log_warn("performance: ignoring invalid system override %s for system=%s",
                    value, system_key);
    }

    return state->perf_global_profile;
}

static int jw__perf_apply_launch_game(jw_daemon_state *state, const char *system,
                                      const char *rom_path, const char *reason) {
    return jw__perf_apply_profile(state,
                                  jw__perf_requested_for_launch(state, system, rom_path),
                                  system, reason);
}

static int jw__validate_launch_request(jw_daemon_state *state, const char *system,
                                       const char *rom_path, const char **out_error) {
    if (!state || !system || !system[0] || !rom_path || !rom_path[0]) {
        if (out_error) *out_error = "missing launch payload";
        return -1;
    }

    jw_launch_target target;
    if (jw__resolve_launch_target(state, system, rom_path, &target) != 0) {
        if (out_error) *out_error = "unsupported system";
        return -1;
    }

    if (target.kind == JW_LAUNCH_TARGET_RETROARCH) {
        char *retroarch = jw_retroarch_bin_path();
        if (!retroarch || !jw__path_exists(retroarch)) {
            free(retroarch);
            if (out_error) *out_error = "RetroArch binary missing";
            return -1;
        }
        free(retroarch);
    }

    if (!jw__path_exists(target.path)) {
        if (out_error) {
            *out_error = target.kind == JW_LAUNCH_TARGET_STANDALONE
                ? "standalone emulator missing"
                : "libretro core missing";
        }
        return -1;
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(target.path, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch game target from SD: %s", exec_error);
        if (out_error) *out_error = "SD-card mounted noexec; switcher remount failed or regressed";
        return -1;
    }

    char rom_abs[PATH_MAX];
    if (jw__resolve_rom_path(state, rom_path, rom_abs, sizeof(rom_abs)) != 0 ||
        !jw__path_exists(rom_abs)) {
        if (out_error) *out_error = "ROM path missing";
        return -1;
    }

    return 0;
}

static int jw__request_launch_game(jw_daemon_state *state, const char *system,
                                   const char *rom_path, bool switcher_resume,
                                   const char **out_error) {
    if (jw__validate_launch_request(state, system, rom_path, out_error) != 0) {
        return -1;
    }

    snprintf(state->pending_launch_system, sizeof(state->pending_launch_system), "%s", system);
    snprintf(state->pending_launch_rom_path, sizeof(state->pending_launch_rom_path), "%s", rom_path);
    state->pending_launch_resume_switcher = switcher_resume;
    state->pending_launch = true;

    if (state->child_pid <= 0) {
        if (jw__spawn_pending_game(state) != 0) {
            state->pending_launch_resume_switcher = false;
            if (out_error) *out_error = "game spawn failed";
            return -1;
        }
    }

    return 0;
}

static bool jw__wait_for_tracked_child_exit(jw_daemon_state *state, pid_t pid,
                                            long long timeout_ms) {
    if (!state || pid <= 0) {
        return true;
    }

    long long deadline = jw__monotonic_ms() + timeout_ms;
    for (;;) {
        jw__handle_child_exit(state);
        if (state->child_pid != pid || state->child_kind != JW_CHILD_RETROARCH) {
            return true;
        }

        long long now = jw__monotonic_ms();
        if (now >= deadline) {
            return false;
        }
        usleep(50000);
    }
}

static bool jw__force_retroarch_exit_if_needed(jw_daemon_state *state, pid_t pid,
                                               const char *reason) {
    if (jw__wait_for_tracked_child_exit(state, pid,
                                        JW_RETROARCH_QUIT_GRACE_MS)) {
        return true;
    }

    jw_log_warn("%s: RetroArch did not exit after QUIT; forcing pid=%d",
                reason ? reason : "retroarch", (int)pid);
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        jw_log_warn("%s: SIGKILL failed pid=%d: %s",
                    reason ? reason : "retroarch", (int)pid,
                    strerror(errno));
    }

    return jw__wait_for_tracked_child_exit(state, pid,
                                           JW_RETROARCH_KILL_GRACE_MS);
}

/* True when system + resolved ROM path match the running session — used so a
   switch-game request targeting the current game resumes instead of switching. */
static bool jw__is_current_session_game(const jw_daemon_state *state,
                                        const char *system, const char *rom_path) {
    if (!jw__has_retroarch_session(state) || !rom_path) {
        return false;
    }
    const jw_retroarch_session *s = &state->retroarch_session;
    if (system && system[0] && s->system[0] && strcmp(system, s->system) != 0) {
        return false;
    }
    char target_abs[PATH_MAX];
    if (jw__resolve_rom_path(state, rom_path, target_abs, sizeof(target_abs)) != 0) {
        return false;
    }

    char current_abs[PATH_MAX];
    if (realpath(s->rom_path, current_abs)) {
        return strcmp(target_abs, current_abs) == 0;
    }
    return strcmp(target_abs, s->rom_path) == 0;
}

/* RetroArch writes save states asynchronously — the SAVE_STATE command returns
   well before the file is on disk. Tearing the emulator down right after (quit,
   or a relaunch for a game switch) truncates the write, and the partial state
   then crashes the core on the next resume (seen on N64: a ~16MB mupen state cut
   to 100KB/5.7MB). After issuing a save to `slot`, wait until that slot's file
   has stopped growing before proceeding, then sync. A short warm-up avoids
   mistaking a *pre-existing* (already-stable) state file for the new write.
   Best-effort: bounded by a timeout so a user is never stranded. */
static void jw__wait_for_savestate_write(const jw_daemon_state *state, int slot) {
    const char *source_root = state->retroarch_session.source_root[0]
        ? state->retroarch_session.source_root
        : state->sdcard_root;
    char states_dir[PATH_MAX];
    if (!source_root || !source_root[0] ||
        snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) >=
            (int)sizeof(states_dir)) {
        return;
    }
    const char *rom = state->retroarch_session.rom_path;
    if (!rom || !rom[0]) {
        return;
    }

    const int timeout_ms = 8000;
    const int poll_ms = 100;
    const int warmup_ms = 800;     /* let RA truncate + begin the new write */
    const int stable_polls = 3;    /* ~300ms of no growth = write finished */
    long long last_size = -1;
    int stable = 0;
    int elapsed = 0;
    char path[PATH_MAX];
    while (elapsed < timeout_ms) {
        struct timespec ts = { poll_ms / 1000, (long)(poll_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        elapsed += poll_ms;

        struct stat st;
        if (!jw_ra_find_slot_state(states_dir, rom, slot, path, sizeof(path)) ||
            stat(path, &st) != 0) {
            continue;   /* not created yet */
        }
        long long sz = (long long)st.st_size;
        if (sz != last_size) {
            stable = 0;            /* still being written (or just truncated) */
        } else if (elapsed >= warmup_ms && sz > 0 && ++stable >= stable_polls) {
            sync();
            jw_log_info("savestate settled slot=%d bytes=%lld waited_ms=%d",
                        slot, sz, elapsed);
            return;
        }
        last_size = sz;
    }
    jw_log_warn("savestate did not settle slot=%d within %dms; proceeding anyway",
                slot, timeout_ms);
}

/* Commit a switch from the in-game switcher: save the current game into the
   reserved switcher slot, then prefer an in-process same-core/same-source
   content load. If RetroArch cannot do that, queue the selected game and quit;
   the child-exit handler records the old game's playtime and spawns the queued
   game directly — no launcher flash in between. */
static int jw__request_switch_game(jw_daemon_state *state, const char *system,
                                   const char *rom_path, const char **out_error) {
    if (!jw__has_retroarch_session(state)) {
        if (out_error) *out_error = "no active RetroArch session";
        return -1;
    }
    if (jw__validate_launch_request(state, system, rom_path, out_error) != 0) {
        return -1;
    }

    /* Selecting the running game is a resume, not a switch. */
    if (jw__is_current_session_game(state, system, rom_path)) {
        jw_ra_client ra = jw_ra_client_default();
        jw_ra_result resume = jw_ra_resume_direct(&ra);
        if (resume != JW_RA_OK) {
            jw_log_error("switch-game: current-game resume failed result=%s",
                         jw_ra_result_string(resume));
            if (out_error) *out_error = "resume failed";
            return -1;
        }
        state->retroarch_resume_on_menu_exit = false;
        state->menu_visible = false;
        if (state->menu_pid > 0) {
            kill(state->menu_pid, SIGUSR2);
        }
        return 0;
    }

    jw_ra_client ra = jw_ra_client_default();

    jw_ra_info info;
    memset(&info, 0, sizeof(info));
    jw_ra_result info_result = jw_ra_get_info(&ra, &info);
    if (info_result != JW_RA_OK) {
        jw_log_error("switch-game: state support probe failed result=%s",
                     jw_ra_result_string(info_result));
        if (out_error) *out_error = "savestate unavailable";
        return -1;
    }
    if (!info.savestate_supported) {
        jw_log_error("switch-game: core does not support savestates");
        if (out_error) *out_error = "savestates unsupported";
        return -1;
    }

    char reply[JW_RA_REPLY_MAX];
    jw_ra_result sv = jw_ra_save_state_slot(&ra, JW_RA_GAME_SWITCHER_STATE_SLOT,
                                            reply, sizeof(reply));
    if (sv != JW_RA_OK) {
        jw_log_error("switch-game: slot %d save-state failed result=%s",
                     JW_RA_GAME_SWITCHER_STATE_SLOT, jw_ra_result_string(sv));
        if (out_error) *out_error = "save-state failed";
        return -1;
    }
    jw_log_info("switch-game: saved current game state slot=%d",
                JW_RA_GAME_SWITCHER_STATE_SLOT);
    jw__wait_for_savestate_write(state, JW_RA_GAME_SWITCHER_STATE_SLOT);

    char target_rom_abs[PATH_MAX];
    target_rom_abs[0] = '\0';
    char target_source_root[PATH_MAX];
    target_source_root[0] = '\0';
    char target_core_id[64];
    target_core_id[0] = '\0';
    char *target_core = NULL;
    bool resident_eligible = false;
    int resident_switch_max = jw__resident_switch_max();

    if (jw__resolve_rom_path(state, rom_path,
                             target_rom_abs, sizeof(target_rom_abs)) == 0) {
        jw_storage_source_list sources;
        const jw_storage_source *target_source = NULL;
        if (jw__storage_sources(state, &sources) == 0) {
            target_source = jw_storage_sources_find_for_path(&sources,
                                                             target_rom_abs);
        }
        if (target_source) {
            snprintf(target_source_root, sizeof(target_source_root), "%s",
                     target_source->root);
        } else {
            snprintf(target_source_root, sizeof(target_source_root), "%s",
                     state->sdcard_root ? state->sdcard_root : "");
        }

        jw_launch_target target;
        bool target_is_retroarch =
            jw__resolve_launch_target(state, system, rom_path, &target) == 0 &&
            target.kind == JW_LAUNCH_TARGET_RETROARCH;
        if (target_is_retroarch) {
            snprintf(target_core_id, sizeof(target_core_id), "%s", target.core_id);
            target_core = strdup(target.path);
        }
        resident_eligible =
            target_core && target_core[0] &&
            (resident_switch_max < 0 ||
             state->retroarch_session.resident_switches < resident_switch_max) &&
            state->retroarch_session.core_path[0] &&
            strcmp(target_core, state->retroarch_session.core_path) == 0 &&
            target_source_root[0] &&
            state->retroarch_session.source_root[0] &&
            strcmp(target_source_root, state->retroarch_session.source_root) == 0;
    }

    if (resident_eligible) {
        (void)jw__perf_apply_launch_game(state, system, rom_path, "resident-switch");
        long long resident_start_ms = jw__monotonic_ms();
        char load_reply[JW_RA_REPLY_MAX];
        jw_ra_result load_content =
            jw_ra_load_content_current_core(&ra, target_rom_abs,
                                            load_reply, sizeof(load_reply));
        if (load_content == JW_RA_OK &&
            jw__wait_for_retroarch_content(&ra, target_rom_abs, 2500LL) == 0) {
            int slot = 0;
            char state_path[PATH_MAX];
            char states_dir[PATH_MAX];
            bool have_resume_state = false;
            state_path[0] = '\0';

            if (snprintf(states_dir, sizeof(states_dir), "%s/States",
                         target_source_root) < (int)sizeof(states_dir)) {
                have_resume_state =
                    jw_ra_find_resume_state(states_dir, target_rom_abs,
                                            JW_RA_GAME_SWITCHER_STATE_SLOT,
                                            &slot,
                                            state_path, sizeof(state_path));
            }

            jw__retroarch_session_retarget(state, system, target_rom_abs,
                                           rom_path, target_source_root,
                                           target_core,
                                           target_core_id);

            if (have_resume_state) {
                char load_state_reply[JW_RA_REPLY_MAX];
                jw_ra_result load_state =
                    jw_ra_load_state_slot(&ra, slot,
                                          load_state_reply,
                                          sizeof(load_state_reply));
                if (load_state == JW_RA_OK) {
                    jw_log_info("resident switch: loaded slot=%d path=%s",
                                slot, state_path);
                } else {
                    jw_log_warn("resident switch: state load failed slot=%d path=%s result=%s",
                                slot, state_path,
                                jw_ra_result_string(load_state));
                }
            } else {
                jw_log_info("resident switch: no resume state found for %s",
                            target_rom_abs);
            }

            jw_ra_resume_direct(&ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
            jw_log_info("resident switch timings: total_ms=%lld rom=%s",
                        jw__monotonic_ms() - resident_start_ms,
                        target_rom_abs);
            free(target_core);
            return 0;
        }

        jw_log_warn("resident switch unavailable result=%s reply=%s; falling back to cold switch",
                    jw_ra_result_string(load_content),
                    load_content == JW_RA_OK ? load_reply : "");
    } else {
        jw_log_info("resident switch skipped: eligible=%s resident_switches=%d resident_switch_max=%d target_source=%s target_core=%s current_source=%s current_core=%s",
                    resident_eligible ? "true" : "false",
                    state->retroarch_session.resident_switches,
                    resident_switch_max,
                    target_source_root[0] ? target_source_root : "(unknown)",
                    target_core ? target_core : "(unknown)",
                    state->retroarch_session.source_root[0]
                        ? state->retroarch_session.source_root
                        : "(unknown)",
                    state->retroarch_session.core_path[0]
                        ? state->retroarch_session.core_path
                        : "(unknown)");
    }
    free(target_core);

    snprintf(state->pending_launch_system, sizeof(state->pending_launch_system),
             "%s", system);
    snprintf(state->pending_launch_rom_path, sizeof(state->pending_launch_rom_path),
             "%s", rom_path);
    state->pending_launch_resume_switcher = true;
    state->pending_launch = true;

    pid_t old_retroarch_pid = state->child_pid;
    jw_ra_result q = jw_ra_quit(&ra);
    if (q != JW_RA_OK) {
        jw_log_error("switch-game: quit failed result=%s", jw_ra_result_string(q));
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        if (out_error) *out_error = "RetroArch quit failed";
        return -1;
    }
    if (!jw__force_retroarch_exit_if_needed(state, old_retroarch_pid,
                                            "switch-game")) {
        jw_log_error("switch-game: RetroArch did not exit after forced kill pid=%d",
                     (int)old_retroarch_pid);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        if (out_error) *out_error = "RetroArch exit failed";
        return -1;
    }

    state->retroarch_resume_on_menu_exit = false;
    state->menu_visible = false; /* committed: do not resume the old game */
    return 0;
}

static int jw__request_launch_app(jw_daemon_state *state, const char *pak_dir,
                                  const char **out_error) {
    char pak_abs[PATH_MAX];
    char launch_abs[PATH_MAX];
    if (jw__resolve_app_launch_path(state, pak_dir, pak_abs, sizeof(pak_abs),
                                    launch_abs, sizeof(launch_abs), out_error) != 0) {
        return -1;
    }

    snprintf(state->pending_app_pak_dir, sizeof(state->pending_app_pak_dir), "%s", pak_dir);
    state->pending_app = true;

    if (state->child_pid <= 0) {
        if (jw__spawn_app(state) != 0) {
            if (out_error) *out_error = "app spawn failed";
            return -1;
        }
    }

    return 0;
}

static bool jw__env_is_disabled(const char *name) {
    const char *value = getenv(name);
    return value && strcmp(value, "0") == 0;
}

static bool jw__env_is_truthy(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "no") != 0;
}

static int jw__spawn_osd(jw_daemon_state *state) {
    if (!state || state->osd_pid > 0 || jw__env_is_disabled("JAWAKA_OSD")) {
        return 0;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/jawaka-osd", state->bin_dir) >=
        (int)sizeof(path)) {
        jw_log_warn("osd binary path too long: %s/jawaka-osd", state->bin_dir);
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_warn("osd binary missing: %s", path);
        return -1;
    }

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). */
    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_warn("osd fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        char *const argv[] = { (char *)path, NULL };
        execv(path, argv);
        perror("execv");
        _exit(127);
    }

    state->osd_pid = pid;
    jw_log_info("spawned jawaka-osd pid=%d", (int)pid);
    return 0;
}

static void jw__handle_osd_exit(jw_daemon_state *state) {
    if (!state || state->osd_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t waited = waitpid(state->osd_pid, &status, WNOHANG);
    if (waited == 0 || waited < 0) {
        return;
    }

    state->osd_pid = -1;
    if (WIFEXITED(status)) {
        jw_log_info("jawaka-osd exited status=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("jawaka-osd terminated signal=%d", WTERMSIG(status));
    }

    if (!state->shutdown_requested && !g_shutdown_requested) {
        jw__spawn_osd(state);
    }
}

static void jw__apply_led_config(jw_daemon_state *state, const jw_led_config *led);

static void jw__handle_ledd_exit(jw_daemon_state *state) {
    if (!state || state->ledd_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t waited = waitpid(state->ledd_pid, &status, WNOHANG);
    if (waited == 0 || waited < 0) {
        return;
    }

    state->ledd_pid = -1;
    if (WIFEXITED(status)) {
        jw_log_info("jawaka-ledd exited status=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("jawaka-ledd terminated signal=%d", WTERMSIG(status));
    }

    /* Re-apply the cached LED config so a mid-session crash recovers the effect.
       jw__apply_led_config calls jw__stop_ledd first, which no-ops now that
       ledd_pid is cleared. */
    if (!state->shutdown_requested && !g_shutdown_requested &&
        state->led_configured) {
        jw__apply_led_config(state, &state->cached_led);
    }
}

static int jw__osd_show_brightness(jw_daemon_state *state, int percent) {
    if (!state || !state->osd_socket_path || jw__env_is_disabled("JAWAKA_OSD")) {
        return -1;
    }

    if (state->osd_pid <= 0) {
        jw__spawn_osd(state);
    }

    char request[128];
    snprintf(request, sizeof(request),
             "{\"type\":\"show-brightness\",\"percent\":%d}", percent);

    for (int attempt = 0; attempt < 2; attempt++) {
        char *response = NULL;
        size_t response_len = 0;
        if (jw_ipc_request(state->osd_socket_path, request, strlen(request),
                           &response, &response_len) == 0) {
            free(response);
            return 0;
        }
        free(response);
        if (attempt == 0) {
            usleep(100000);
        }
    }
    jw_log_warn("osd brightness request failed");
    return -1;
}

static void jw__persist_brightness(jw_daemon_state *state, int percent) {
    if (!state || !state->db_path) {
        return;
    }

    char value[16];
    snprintf(value, sizeof(value), "%d", percent);
    if (jw_db_set_setting(state->db_path, "platform.brightness_percent", value) != 0) {
        jw_log_warn("could not persist brightness setting");
    }
}

static int jw__set_brightness(jw_daemon_state *state, int percent,
                              bool persist, bool show_osd,
                              jw_platform_result *out) {
    if (!state) {
        if (out) {
            out->code = JW_PLATFORM_RESULT_INVALID;
            snprintf(out->message, sizeof(out->message), "%s", "daemon state missing");
            out->has_value = false;
            out->value = 0;
        }
        return -1;
    }

    int clamped = jw_platform_clamp_brightness_percent(percent);
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_BRIGHTNESS,
                               clamped, out);
    if (!out || out->code != JW_PLATFORM_RESULT_OK) {
        return -1;
    }

    int resolved = out->has_value ? out->value : clamped;
    state->cached_brightness_percent = jw_platform_clamp_brightness_percent(resolved);
    if (persist) {
        jw__persist_brightness(state, resolved);
    }
    if (show_osd) {
        jw__osd_show_brightness(state, resolved);
    }
    return 0;
}

static void jw__apply_persisted_brightness(jw_daemon_state *state) {
    char value[32];
    if (!state || !state->db_path ||
        jw_db_get_setting(state->db_path, "platform.brightness_percent",
                          value, sizeof(value)) != 0 ||
        !value[0]) {
        return;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        jw_log_warn("ignoring invalid persisted brightness: %s", value);
        return;
    }

    jw_platform_result result;
    if (jw__set_brightness(state, (int)parsed, false, false, &result) == 0) {
        jw_log_info("applied persisted brightness value=%d", result.value);
    } else {
        jw_log_warn("persisted brightness apply failed: %s", result.message);
    }
}

static void jw__persist_volume(jw_daemon_state *state, int percent) {
    if (!state || !state->db_path) {
        return;
    }

    char value[16];
    snprintf(value, sizeof(value), "%d", percent);
    if (jw_db_set_setting(state->db_path, "platform.volume_percent", value) != 0) {
        jw_log_warn("could not persist volume setting");
    }
}

static void jw__apply_persisted_volume(jw_daemon_state *state) {
    char value[32];
    if (!state || !state->db_path ||
        jw_db_get_setting(state->db_path, "platform.volume_percent",
                          value, sizeof(value)) != 0 ||
        !value[0]) {
        return;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        jw_log_warn("ignoring invalid persisted volume: %s", value);
        return;
    }

    int percent = (int)parsed;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    jw_platform_result result;
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_VOLUME,
                               percent, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        int resolved = result.has_value ? result.value : percent;
        if (resolved < 0) resolved = 0;
        if (resolved > 100) resolved = 100;
        state->cached_volume_percent = resolved;
        jw_log_info("applied persisted volume value=%d", resolved);
    } else {
        jw_log_warn("persisted volume apply failed: %s", result.message);
    }
}

static void jw__persist_led(jw_daemon_state *state, const jw_led_config *led) {
    if (!state || !state->db_path[0] || !led) {
        return;
    }
    char vr[8], vg[8], vb[8], vbr[8], vsp[8];
    snprintf(vr,  sizeof(vr),  "%u", (unsigned)led->r);
    snprintf(vg,  sizeof(vg),  "%u", (unsigned)led->g);
    snprintf(vb,  sizeof(vb),  "%u", (unsigned)led->b);
    snprintf(vbr, sizeof(vbr), "%d", led->brightness);
    snprintf(vsp, sizeof(vsp), "%d", led->speed);
    const char *keys[] = {
        "platform.led_enabled", "platform.led_mode", "platform.led_r",
        "platform.led_g", "platform.led_b", "platform.led_brightness",
        "platform.led_speed",
    };
    const char *vals[] = {
        led->enabled ? "1" : "0", jw_led_mode_name(led->mode), vr, vg, vb, vbr, vsp,
    };
    if (jw_db_set_settings(state->db_path, keys, vals,
                           (int)(sizeof(keys) / sizeof(keys[0]))) != 0) {
        jw_log_warn("could not persist led settings");
    }
}

static void jw__stop_ledd(jw_daemon_state *state) {
    if (!state || state->ledd_pid <= 0) return;
    kill(state->ledd_pid, SIGTERM);   /* ledd's handler restores platform LED ownership */
    waitpid(state->ledd_pid, NULL, 0);
    state->ledd_pid = -1;
}

static int jw__spawn_ledd(jw_daemon_state *state, const char *effect,
                          int r, int g, int b, int brightness, int speed) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/jawaka-ledd", state->bin_dir) >= (int)sizeof(path)) {
        jw_log_warn("ledd binary path too long");
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_warn("ledd binary missing: %s", path);
        return -1;
    }
    char sr[8], sg[8], sb[8], sbr[8], ssp[8];
    snprintf(sr,  sizeof(sr),  "%d", r);
    snprintf(sg,  sizeof(sg),  "%d", g);
    snprintf(sb,  sizeof(sb),  "%d", b);
    snprintf(sbr, sizeof(sbr), "%d", brightness);
    snprintf(ssp, sizeof(ssp), "%d", speed);
    pid_t pid = fork();
    if (pid < 0) { jw_log_warn("ledd fork failed: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        char *const argv[] = { path, (char *)effect, sr, sg, sb, sbr, ssp, NULL };
        execv(path, argv);
        _exit(127);
    }
    state->ledd_pid = pid;
    jw_log_info("spawned jawaka-ledd %s pid=%d", effect, (int)pid);
    return 0;
}

static const char *jw__leaf_ledd_effect_name(const jw_led_config *led) {
    if (!led || !jw__env_is_truthy("UMRK_LEAF_MODE")) return NULL;
    if (!led->enabled) return "off";

    switch (led->mode) {
        case JW_LED_MODE_STATIC:  return "static";
        case JW_LED_MODE_BREATH:  return "breath";
        case JW_LED_MODE_RAINBOW: return "rainbow";
        default:
            return jw_led_mode_is_effect(led->mode) ? jw_led_mode_name(led->mode) : NULL;
    }
}

/* Apply an LED config: stop any running effect, write the platform baseline
   config, then spawn the custom effect engine when the selected mode needs it. */
static void jw__apply_led_config(jw_daemon_state *state, const jw_led_config *led) {
    if (!state || !led) return;
    jw__stop_ledd(state);

    jw_led_config base = *led;
    if (jw_led_mode_is_effect(base.mode)) base.mode = JW_LED_MODE_STATIC;
    jw_platform_result result;
    jw_platform_set_led(&state->platform, &base, &result);

    const char *leaf_effect = jw__leaf_ledd_effect_name(led);
    if (leaf_effect) {
        jw__spawn_ledd(state, leaf_effect,
                       led->r, led->g, led->b, led->brightness, led->speed);
    } else if (led->enabled && jw_led_mode_is_effect(led->mode)) {
        jw__spawn_ledd(state, jw_led_mode_name(led->mode),
                       led->r, led->g, led->b, led->brightness, led->speed);
    }
    state->cached_led = *led;
    state->led_configured = true;
}

static void jw__apply_persisted_led(jw_daemon_state *state) {
    if (!state || !state->db_path[0]) return;

    char value[32];
    jw_led_config led;
    memset(&led, 0, sizeof(led));

    if (jw_db_get_setting(state->db_path, "platform.led_mode", value, sizeof(value)) == 0 && value[0]) {
        /* Restore the user's saved LED config. */
        led.mode = JW_LED_MODE_STATIC;
        jw_led_mode_parse(value, &led.mode);
        led.brightness = 5;
        led.speed = 5;
        if (jw_db_get_setting(state->db_path, "platform.led_enabled", value, sizeof(value)) == 0)
            led.enabled = (strcmp(value, "0") != 0);
        if (jw_db_get_setting(state->db_path, "platform.led_r", value, sizeof(value)) == 0) led.r = (unsigned char)atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_g", value, sizeof(value)) == 0) led.g = (unsigned char)atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_b", value, sizeof(value)) == 0) led.b = (unsigned char)atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_brightness", value, sizeof(value)) == 0) led.brightness = atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_speed", value, sizeof(value)) == 0) led.speed = atoi(value);
    } else {
        /* Fresh install → a calm, subdued-green breath. Matches the boot logo's
           breathing finish, so the LED stays consistent from boot into the
           launcher. Persist it so it becomes the user's setting and shows in
           Lighting. */
        led.enabled = true;
        led.mode = JW_LED_MODE_BREATH;
        led.r = 0x0B; led.g = 0x28; led.b = 0x00;   /* #0B2800 — subdued green */
        led.brightness = 1;
        led.speed = 10;
        jw__persist_led(state, &led);
    }

    jw__apply_led_config(state, &led);
    jw_log_info("applied led mode=%s enabled=%d",
                jw_led_mode_name(led.mode), led.enabled);
}

static int jw__osd_show_volume(jw_daemon_state *state, int percent) {
    if (!state || !state->osd_socket_path || jw__env_is_disabled("JAWAKA_OSD")) {
        return -1;
    }

    if (state->osd_pid <= 0) {
        jw__spawn_osd(state);
    }

    char request[128];
    snprintf(request, sizeof(request),
             "{\"type\":\"show-volume\",\"percent\":%d}", percent);

    for (int attempt = 0; attempt < 2; attempt++) {
        char *response = NULL;
        size_t response_len = 0;
        if (jw_ipc_request(state->osd_socket_path, request, strlen(request),
                           &response, &response_len) == 0) {
            free(response);
            return 0;
        }
        free(response);
        if (attempt == 0) {
            usleep(100000);
        }
    }
    jw_log_warn("osd volume request failed");
    return -1;
}

static void jw__input_volume_delta(void *userdata, int delta_percent) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state) {
        return;
    }

    if (state->cached_volume_percent < 0) {
        jw__refresh_platform_cache(state);
    }
    int current = state->cached_volume_percent >= 0 ? state->cached_volume_percent : 50;

    int target = current + delta_percent;
    if (target < 0) target = 0;
    if (target > 100) target = 100;

    jw_platform_result result;
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_VOLUME,
                               target, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        int resolved = result.has_value ? result.value : target;
        if (resolved < 0) resolved = 0;
        if (resolved > 100) resolved = 100;
        state->cached_volume_percent = resolved;
        jw__persist_volume(state, resolved);
        /* No OSD over a kmsdrm standalone emulator: the Wayland overlay can
           only steal one stray frame from the emulator's page flips. The
           audible change is the feedback. */
        if (!jw__has_standalone_session(state)) {
            jw__osd_show_volume(state, resolved);
        }
        jw_log_info("volume hotkey delta=%d value=%d", delta_percent, resolved);
    } else {
        jw_log_warn("volume hotkey failed: %s", result.message);
    }
}

static void jw__input_brightness_delta(void *userdata, int delta_percent) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state) {
        return;
    }

    if (state->cached_brightness_percent < 0) {
        jw__refresh_platform_cache(state);
    }
    int current = state->cached_brightness_percent >= 0
                    ? state->cached_brightness_percent
                    : 50;

    jw_platform_result result;
    bool show_osd = !jw__has_standalone_session(state);
    if (jw__set_brightness(state, current + delta_percent, true, show_osd, &result) == 0) {
        jw_log_info("brightness hotkey delta=%d value=%d",
                    delta_percent, result.has_value ? result.value : current + delta_percent);
    } else {
        jw_log_warn("brightness hotkey failed: %s", result.message);
    }
}

static bool jw__input_menu_tap(void *userdata) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;

    /* Standalone emulators own the display, so Jawaka's overlay menu cannot
       appear above them. PPSSPP has a patched SIGUSR2 pause-menu hook. DraStic
       reads the real pad directly and has an internal menu binding, so let the
       Menu tap pass through to the emulator. Standalone emulators without a
       menu hook keep Menu as the exit key. */
    if (jw__has_standalone_session(state)) {
        pid_t pid = state->retroarch_session.pid;
        if (jw__standalone_session_is_ppsspp(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: opening PPSSPP pause menu pid=%d", (int)pid);
            if (kill(pid, SIGUSR2) != 0 && errno != ESRCH) {
                jw_log_warn("PPSSPP menu: SIGUSR2 failed pid=%d: %s",
                            (int)pid, strerror(errno));
            }
            return true;
        }
        if (jw__standalone_session_is_drastic(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: letting DraStic handle native menu pid=%d", (int)pid);
            return true;
        }
        if (jw__standalone_session_is_mupen64plus(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: forwarding to Mupen64Plus embedded menu pid=%d", (int)pid);
            return false;
        }

        long long now = jw__monotonic_ms();
        if (state->standalone_quit_request_ms == 0) {
            jw_log_info("menu tap: quitting standalone emulator pid=%d", (int)pid);
            if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
                jw_log_warn("standalone quit: SIGTERM failed pid=%d: %s",
                            (int)pid, strerror(errno));
            }
            state->standalone_quit_request_ms = now;
        } else if (now - state->standalone_quit_request_ms >= 2000) {
            jw_log_warn("standalone emulator ignored quit; forcing pid=%d", (int)pid);
            if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
                jw_log_warn("standalone quit: SIGKILL failed pid=%d: %s",
                            (int)pid, strerror(errno));
            }
        }
        return true;
    }

    if (!jw__has_retroarch_session(state)) {
        return false;
    }

    /* Menu toggles: tap to open, tap again to close. */
    if (state->menu_visible) {
        jw__request_close_in_game_menu(state);
        return true;
    }

    if (jw__request_open_in_game_menu(state) != 0) {
        jw_log_warn("in-game menu: open request failed, forwarding Menu tap");
        return false;
    }

    return true;
}

/* Menu + Select chord (input proxy): open the reversible in-game switcher
   overlay. Returns false when there is no game to switch in, so the proxy
   forwards the events normally. Opening only pauses + overlays — it never saves
   or quits. */
static bool jw__input_game_switcher(void *userdata) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!jw__has_retroarch_session(state)) {
        return false;
    }

    /* A surface is already up: swallow the chord rather than stacking. */
    if (state->menu_visible) {
        return true;
    }

    if (jw__request_open_in_game_switcher(state) != 0) {
        jw_log_warn("in-game switcher: open request failed, forwarding chord");
        return false;
    }

    return true;
}

static void jw__start_input_proxy(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    if (state->input_proxy.enabled) {
        jw__publish_retroarch_input_env(state);
        return;
    }

    if (jw_input_proxy_init(&state->input_proxy, jw__input_brightness_delta,
                            jw__input_volume_delta, jw__input_menu_tap,
                            jw__input_game_switcher, state) == 0) {
        jw__publish_retroarch_input_env(state);
    }
}

static void jw__suspend_input_proxy_for_app(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    if (state->input_proxy.enabled) {
        jw_log_info("input proxy: releasing grab for app launch");
        jw_input_proxy_shutdown(&state->input_proxy);
    }
    jw__publish_direct_input_env();
}

static int jw__spawn_child(jw_daemon_state *state, jw_child_kind kind) {
    const char *name = jw__child_name(kind);
    if (!state || !name || kind == JW_CHILD_RETROARCH ||
        kind == JW_CHILD_EMULATOR || kind == JW_CHILD_APP) {
        return -1;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", state->bin_dir, name) >=
        (int)sizeof(path)) {
        jw_log_error("child binary path too long: %s/%s", state->bin_dir, name);
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_error("child binary missing: %s", path);
        return -1;
    }

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). */
    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        char *const argv[] = { (char *)path, NULL };
        execv(path, argv);
        perror("execv");
        _exit(127);
    }

    state->child_pid = pid;
    state->child_kind = kind;
    jw_log_info("spawned %s pid=%d", name, (int)pid);
    return 0;
}

/* Spawn the in-game menu process. show_now=false parks it hidden as a warm
   standby (revealed later by a SIGUSR1 show signal); show_now=true makes it
   reveal itself as soon as it finishes cat_init (on-demand fallback path). */
static int jw__spawn_in_game_menu(jw_daemon_state *state, bool show_now) {
    if (!state || state->menu_pid > 0) {
        return state && state->menu_pid > 0 ? 0 : -1;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/jawaka-menu", state->bin_dir) >=
        (int)sizeof(path)) {
        jw_log_error("in-game menu binary path too long: %s/jawaka-menu",
                     state->bin_dir);
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_error("in-game menu binary missing: %s", path);
        return -1;
    }

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). */
    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("in-game menu fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        if (jw__has_retroarch_session(state)) {
            setenv("JAWAKA_INGAME_ACTIVE", "1", 1);
            setenv("JAWAKA_INGAME_SYSTEM", state->retroarch_session.system, 1);
            setenv("JAWAKA_INGAME_ROM", state->retroarch_session.rom_path, 1);
            setenv("JAWAKA_INGAME_CORE", state->retroarch_session.core_path, 1);
            setenv("JAWAKA_INGAME_CORE_ID", state->retroarch_session.core_id, 1);
        }
        setenv("JAWAKA_INGAME_AUTOSHOW", show_now ? "1" : "0", 1);
        char dir_buf[PATH_MAX];
        if (state->runtime_dir &&
            snprintf(dir_buf, sizeof(dir_buf), "%s/shots", state->runtime_dir) <
                (int)sizeof(dir_buf)) {
            setenv("JAWAKA_INGAME_SHOTDIR", dir_buf, 1);
        }
        const char *state_root = state->retroarch_session.source_root[0]
            ? state->retroarch_session.source_root
            : state->sdcard_root;
        const char *states_dir = jw__env_value("STATES_PATH");
        if (state->retroarch_session.source_root[0] ||
            !states_dir || strcmp(states_dir, state->sdcard_root) == 0) {
            if (state_root &&
                snprintf(dir_buf, sizeof(dir_buf), "%s/States", state_root) <
                    (int)sizeof(dir_buf)) {
                setenv("JAWAKA_INGAME_STATEDIR", dir_buf, 1);
            }
        } else if (states_dir &&
            snprintf(dir_buf, sizeof(dir_buf), "%s", states_dir) <
                    (int)sizeof(dir_buf)) {
            setenv("JAWAKA_INGAME_STATEDIR", dir_buf, 1);
        }
        char *const argv[] = { (char *)path, "--in-game", NULL };
        execv(path, argv);
        perror("execv");
        _exit(127);
    }

    state->menu_pid = pid;
    state->menu_in_game = true;
    jw_log_info("spawned in-game menu pid=%d standby=%d", (int)pid, !show_now);
    return 0;
}

static int jw__spawn_app(jw_daemon_state *state) {
    if (!state || !state->pending_app) {
        return -1;
    }

    const char *error_message = NULL;
    char pak_abs[PATH_MAX];
    char launch_abs[PATH_MAX];
    if (jw__resolve_app_launch_path(state, state->pending_app_pak_dir,
                                    pak_abs, sizeof(pak_abs),
                                    launch_abs, sizeof(launch_abs),
                                    &error_message) != 0) {
        jw_log_error("could not resolve app launch path: %s",
                     error_message ? error_message : state->pending_app_pak_dir);
        state->pending_app = false;
        return -1;
    }

    jw_storage_source_list sources;
    const jw_storage_source *app_source = NULL;
    if (jw__storage_sources(state, &sources) == 0) {
        app_source = jw_storage_sources_find_for_path(&sources, pak_abs);
    }

    jw__suspend_input_proxy_for_app(state);
    /* Watch the pad in watch-only mode so the hardware volume / brightness keys
       keep working while a generic app is foreground — the same as standalone
       emulators. The app reads the pad directly for its own input; jawakad only
       watches for the system hotkeys (volume/brightness; Menu and the switcher
       no-op without a game session). The app-exit handler tears this watch proxy
       down and restores the full grab. */
    if (jw_input_proxy_init_watch(&state->input_proxy, jw__input_brightness_delta,
                                  jw__input_volume_delta, jw__input_menu_tap,
                                  jw__input_game_switcher, state) != 0) {
        jw_log_warn("input watch: init failed; volume/brightness keys unavailable this app session");
    }
    jw__reconcile_audio(state, "app-launch", false);
    jw__publish_audio_env(state);
    (void)jw__perf_apply_frontend(state, "app-launch");

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). */
    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    /* The RetroArch.pak runner builds its own RA config (reading JAWAKA_CHEEVOS_*),
       so it legitimately needs the cheevos creds; every other app must not see
       them. Resolve in the parent (SQLite open is not fork-safe post-fork) and
       apply child-side only for the RetroArch app. */
    jw_cheevos_creds cheevos;
    jw__cheevos_resolve(state, &cheevos);
    bool app_is_retroarch = jw__pak_dir_is_retroarch(state->pending_app_pak_dir);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        jw_input_proxy_shutdown(&state->input_proxy);   /* drop the watch-only proxy */
        jw__start_input_proxy(state);
        state->pending_app = false;
        return -1;
    }

    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        jw__publish_source_content_env(app_source);
        /* Only the RetroArch runner gets cheevos creds (it writes its own RA
           config); every other app has them explicitly cleared so the plaintext
           password never reaches third-party app code. */
        if (app_is_retroarch) {
            jw__cheevos_apply_env(&cheevos);
        } else {
            jw__cheevos_clear_env();
        }
        if (chdir(pak_abs) != 0) {
            perror("chdir");
            _exit(127);
        }
        char *const argv[] = { (char *)launch_abs, NULL };
        execv(launch_abs, argv);
        perror("execv");
        _exit(127);
    }

    state->child_pid = pid;
    state->child_kind = JW_CHILD_APP;
    state->pending_app = false;
    jw_log_info("spawned app pid=%d pak=%s", (int)pid, pak_abs);
    return 0;
}

static int jw__spawn_standalone_emulator(jw_daemon_state *state,
                                         const jw_launch_target *target) {
    if (!state || !state->pending_launch || !target ||
        target->kind != JW_LAUNCH_TARGET_STANDALONE) {
        return -1;
    }

    char rom_abs[PATH_MAX];
    if (jw__resolve_rom_path(state, state->pending_launch_rom_path,
                             rom_abs, sizeof(rom_abs)) != 0) {
        jw_log_error("could not resolve ROM path: %s", state->pending_launch_rom_path);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        return -1;
    }

    jw_storage_source_list sources;
    const jw_storage_source *rom_source = NULL;
    if (jw__storage_sources(state, &sources) == 0) {
        rom_source = jw_storage_sources_find_for_path(&sources, rom_abs);
    }

    char source_root[PATH_MAX];
    if (rom_source) {
        snprintf(source_root, sizeof(source_root), "%s", rom_source->root);
    } else {
        snprintf(source_root, sizeof(source_root), "%s", state->sdcard_root);
    }

    bool switcher_resume = state->pending_launch_resume_switcher;
    bool have_standalone_resume = false;
    int standalone_resume_slot = 0;
    char standalone_resume_path[PATH_MAX];
    standalone_resume_path[0] = '\0';
    if (switcher_resume && jw__standalone_target_is_mupen64plus(target)) {
        char states_dir[PATH_MAX];
        if (snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) <
                (int)sizeof(states_dir) &&
            jw_ra_find_resume_state(states_dir, rom_abs,
                                    JW_RA_GAME_SWITCHER_STATE_SLOT,
                                    &standalone_resume_slot,
                                    standalone_resume_path,
                                    sizeof(standalone_resume_path))) {
            have_standalone_resume = true;
            jw_log_info("standalone resume: state slot=%d path=%s",
                        standalone_resume_slot, standalone_resume_path);
        } else {
            jw_log_info("standalone resume: no prelaunch state found for %s", rom_abs);
        }
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(target->path, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch standalone emulator from SD: %s", exec_error);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        return -1;
    }

    jw_platform_result ready_result;
    jw_platform_frontend_ready(&state->platform, "launcher", &ready_result);
    jw_log_info("standalone emulator launch transition readiness code=%s",
                jw_platform_result_code_name(ready_result.code));

    if (jw__standalone_target_uses_calibrated_virtual_input(target)) {
        /* Mupen64Plus and PortMaster ports need the same calibrated virtual
           gamepad path as RetroArch. Keep the full grab-and-forward proxy
           active so Joe's calibration is applied before SDL sees the axes. */
        jw__start_input_proxy(state);
        if (state->input_proxy.enabled && state->input_proxy.virtual_event_path[0]) {
            int joypad_index = jw_input_proxy_retroarch_joypad_index(&state->input_proxy);
            jw_log_info("standalone input proxy: physical=%s virtual=%s joypad_index=%d",
                        state->input_proxy.physical_event_path[0]
                            ? state->input_proxy.physical_event_path
                            : "(unknown)",
                        state->input_proxy.virtual_event_path,
                        joypad_index);
        } else {
            jw_log_warn("standalone input proxy: calibrated virtual gamepad unavailable; falling back to direct SDL input");
            jw__publish_direct_input_env();
        }
    } else {
        /* Other standalone emulators read the physical pad directly (suspend the
           grab, like app launches), but jawakad still needs the hotkeys: re-open
           the pad in watch-only mode so volume/brightness keep working and Menu
           can route to the emulator's native menu or generic exit path. */
        jw__suspend_input_proxy_for_app(state);
        if (jw_input_proxy_init_watch(&state->input_proxy, jw__input_brightness_delta,
                                      jw__input_volume_delta, jw__input_menu_tap,
                                      jw__input_game_switcher, state) != 0) {
            jw_log_warn("input watch: init failed; Menu exit unavailable this session");
        }
    }
    jw__reconcile_audio(state, "standalone-launch", false);
    jw__publish_audio_env(state);
    (void)jw__perf_apply_launch_game(state, state->pending_launch_system,
                                     state->pending_launch_rom_path,
                                     "standalone-emulator-launch");

    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        jw_input_proxy_shutdown(&state->input_proxy);  /* drop the watch-only proxy */
        jw__start_input_proxy(state);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        return -1;
    }

    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        jw__publish_source_content_env(rom_source);
        setenv("JAWAKA_GAME_SYSTEM", state->pending_launch_system, 1);
        setenv("JAWAKA_GAME_ROM", state->pending_launch_rom_path, 1);
        setenv("JAWAKA_GAME_ROM_ABS", rom_abs, 1);
        setenv("JAWAKA_GAME_CORE_ID", target->core_id, 1);
        if (have_standalone_resume) {
            char slot_env[16];
            snprintf(slot_env, sizeof(slot_env), "%d", standalone_resume_slot);
            setenv("EMU_RESUME_SLOT", slot_env, 1);
        } else {
            unsetenv("EMU_RESUME_SLOT");
        }

        char *const argv[] = {
            (char *)target->path,
            rom_abs,
            NULL,
        };
        execv(target->path, argv);
        perror("execv");
        _exit(127);
    }

    state->child_pid = pid;
    state->child_kind = JW_CHILD_EMULATOR;
    state->pending_launch = false;
    state->pending_launch_resume_switcher = false;
    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    state->post_launch_resume_next_ms = 0;
    jw_log_info("spawned standalone emulator pid=%d launcher=%s core_id=%s",
                (int)pid, target->path,
                target->core_id[0] ? target->core_id : "(unknown)");
    jw__standalone_session_start(state, pid, state->pending_launch_system, rom_abs,
                                 state->pending_launch_rom_path, source_root,
                                 target->path, target->core_id);
    return 0;
}

static int jw__spawn_pending_game(jw_daemon_state *state) {
    if (!state || !state->pending_launch) {
        return -1;
    }

    jw_launch_target target;
    if (jw__resolve_launch_target(state, state->pending_launch_system,
                                  state->pending_launch_rom_path,
                                  &target) != 0) {
        jw_log_error("could not resolve launch target for system=%s rom=%s",
                     state->pending_launch_system,
                     state->pending_launch_rom_path);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        return -1;
    }

    if (target.kind == JW_LAUNCH_TARGET_STANDALONE) {
        return jw__spawn_standalone_emulator(state, &target);
    }
    return jw__spawn_retroarch(state);
}

static int jw__spawn_retroarch(jw_daemon_state *state) {
    if (!state || !state->pending_launch) {
        return -1;
    }
    long long launch_start_ms = jw__monotonic_ms();
    bool switcher_resume = state->pending_launch_resume_switcher;

    char rom_abs[PATH_MAX];
    if (jw__resolve_rom_path(state, state->pending_launch_rom_path, rom_abs, sizeof(rom_abs)) != 0) {
        jw_log_error("could not resolve ROM path: %s", state->pending_launch_rom_path);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        return -1;
    }
    jw_storage_source_list sources;
    const jw_storage_source *rom_source = NULL;
    if (jw__storage_sources(state, &sources) == 0) {
        rom_source = jw_storage_sources_find_for_path(&sources, rom_abs);
    }
    char source_root[PATH_MAX];
    if (rom_source) {
        snprintf(source_root, sizeof(source_root), "%s", rom_source->root);
    } else {
        snprintf(source_root, sizeof(source_root), "%s", state->sdcard_root);
    }

    char *retroarch = jw_retroarch_bin_path();
    char core_id[64];
    char core_diagnostic[256];
    char *core = jw__resolve_launch_core_path(state,
                                              state->pending_launch_system,
                                              state->pending_launch_rom_path,
                                              core_id, sizeof(core_id),
                                              core_diagnostic,
                                              sizeof(core_diagnostic));
    char *runtime_config = NULL;
    char *ra_home = NULL;

    jw__publish_retroarch_input_env(state);
    jw__reconcile_audio(state, "retroarch-launch", false);
    jw__publish_audio_env(state);
    bool audio_bluetooth =
        strcmp(getenv("JAWAKA_AUDIO_OUTPUT") ? getenv("JAWAKA_AUDIO_OUTPUT") : "",
               "BLUETOOTH") == 0;
    jw__publish_display_env(state);

    /* Resolve cheevos creds here (SQLite open is not fork-safe between fork and
       execv). They are applied to the parent env only briefly around the config
       write below — jw_prepare_retroarch_config reads JAWAKA_CHEEVOS_* to bake
       cheevos_username/password into the per-launch config — then cleared, so the
       plaintext password reaches the RA config but never persists in the daemon
       env or leaks to later children. */
    jw_cheevos_creds cheevos;
    jw__cheevos_resolve(state, &cheevos);

    if (!retroarch || !jw__path_exists(retroarch)) {
        jw_log_error("RetroArch binary missing: %s", retroarch ? retroarch : "(null)");
        goto fail;
    }
    if (!core || !jw__path_exists(core)) {
        jw_log_error("libretro core missing for %s: %s",
                     state->pending_launch_system, core ? core : "(null)");
        goto fail;
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(core, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch RetroArch core from SD: %s", exec_error);
        goto fail;
    }

    jw_platform_result ready_result;
    jw_platform_frontend_ready(&state->platform, "launcher", &ready_result);
    jw_log_info("RetroArch launch transition readiness code=%s",
                jw_platform_result_code_name(ready_result.code));

    int player1_joypad_index = jw_input_proxy_retroarch_joypad_index(&state->input_proxy);
    if (state->input_proxy.enabled && state->input_proxy.virtual_event_path[0]) {
        if (player1_joypad_index >= 0) {
            jw_log_info("RetroArch input proxy: physical=%s virtual=%s joypad_index=%d",
                        state->input_proxy.physical_event_path[0]
                            ? state->input_proxy.physical_event_path
                            : "(unknown)",
                        state->input_proxy.virtual_event_path,
                        player1_joypad_index);
        } else {
            jw_log_warn("RetroArch input proxy: could not resolve virtual joypad index for %s",
                        state->input_proxy.virtual_event_path);
        }
    }

    jw_saved_env storage_env[] = {
        { .name = NULL, .value = NULL, .had_value = false },
        { .name = NULL, .value = NULL, .had_value = false },
        { .name = NULL, .value = NULL, .had_value = false },
    };
    if (rom_source) {
        jw__save_env(&storage_env[0], "BIOS_PATH");
        jw__save_env(&storage_env[1], "SAVES_PATH");
        jw__save_env(&storage_env[2], "STATES_PATH");
        jw__publish_retroarch_source_dirs(rom_source);
    }

    char config_error[256];
    /* Persist RetroArch config changes on a clean exit regardless of how the game
       was launched. Previously resume launches (Recents/Switcher) used
       !switcher_resume = false, so a global RA setting changed while playing a
       resumed game was silently dropped on quit, while the same change from a cold
       Games-tab launch stuck. Per-game/core options already persist via RA's durable
       HOME; this makes global settings consistent too. (The rapid switcher-hop case
       is still excluded separately via switcher_transition at the write-back.) */
    bool persist_config = true;
    long long config_start_ms = jw__monotonic_ms();
    /* Bake cheevos creds into the config write, then clear them from the parent
       env immediately so the plaintext password never persists in the daemon. */
    jw__cheevos_apply_env(&cheevos);
    runtime_config = jw_prepare_retroarch_config(state->runtime_dir,
                                                source_root,
                                                core,
                                                player1_joypad_index,
                                                persist_config,
                                                config_error,
                                                sizeof(config_error));
    jw__cheevos_clear_env();
    jw__restore_env(storage_env, 3);
    if (!runtime_config) {
        jw_log_error("could not prepare RetroArch config: %s",
                     config_error[0] ? config_error : "unknown error");
        goto fail;
    }
    long long config_done_ms = jw__monotonic_ms();

    bool entryslot_resume = false;
    int entryslot = JW_RA_GAME_SWITCHER_STATE_SLOT;
    char entryslot_arg[16];
    char entry_state_path[PATH_MAX];
    entryslot_arg[0] = '\0';
    entry_state_path[0] = '\0';
    long long state_resolve_start_ms = jw__monotonic_ms();
    if (switcher_resume) {
        char states_dir[PATH_MAX];
        int resolved_slot = 0;
        char resolved_path[PATH_MAX];
        resolved_path[0] = '\0';
        if (snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) <
                (int)sizeof(states_dir) &&
            jw_ra_find_resume_state(states_dir, rom_abs,
                                    JW_RA_GAME_SWITCHER_STATE_SLOT,
                                    &resolved_slot,
                                    resolved_path, sizeof(resolved_path))) {
            /* Resume via the post-launch network load-state command (below), NOT
               RetroArch's --entryslot. RA 1.22.2's --entryslot resolves a separate
               "<rom>.stateN.entry" file and does not reliably load our regular
               savestate when sort_savestates sorts states into per-core subfolders
               (verified on device: -e <slot> with a valid .state99 on disk still
               cold-started). The command path (jw_ra_load_state_slot) uses RA's
               normal slot loader, which honors the subfolder. Leave entryslot_resume
               false so post_launch_resume drives the load. */
            entryslot = resolved_slot;
            snprintf(entry_state_path, sizeof(entry_state_path), "%s", resolved_path);
            jw_log_info("switcher resume: state slot=%d path=%s (post-launch load)",
                        entryslot, entry_state_path);
        } else {
            jw_log_info("switcher resume: no prelaunch state found for %s", rom_abs);
        }
    }
    long long state_resolve_done_ms = jw__monotonic_ms();

    (void)jw__perf_apply_launch_game(state, state->pending_launch_system,
                                     state->pending_launch_rom_path,
                                     "retroarch-launch");

    /* RetroArch resolves its config dir / per-core option files (e.g.
       FCEUmm.opt) under $HOME. Point HOME at the SD state dir the runner uses so
       all RA config lives on the SD card jawaka launched from, not on device
       internal storage. Computed here (it mkdir's) and applied in the child only
       — setting it on the daemon parent would change HOME for every later child.
       Uses sdcard_root so all games share one canonical RA config location. */
    ra_home = jw_retroarch_state_dir(state->sdcard_root);
    if (!ra_home) {
        jw_log_warn("could not resolve RA HOME (SD state dir); RetroArch config "
                    "may fall back to device internal storage");
    }

    long long fork_start_ms = jw__monotonic_ms();
    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        goto fail;
    }

    if (pid == 0) {
        if (ra_home && ra_home[0]) {
            setenv("HOME", ra_home, 1);
        }
        char *argv[9];
        int argc = 0;
        argv[argc++] = retroarch;
        argv[argc++] = (char *)"-L";
        argv[argc++] = core;
        argv[argc++] = (char *)"--config";
        argv[argc++] = runtime_config;
        if (entryslot_resume) {
            argv[argc++] = (char *)"-e";
            argv[argc++] = entryslot_arg;
        }
        argv[argc++] = rom_abs;
        argv[argc] = NULL;
        execv(retroarch, argv);
        perror("execv");
        _exit(127);
    }
    long long fork_done_ms = jw__monotonic_ms();

    state->child_pid = pid;
    state->child_kind = JW_CHILD_RETROARCH;
    state->pending_launch = false;
    state->pending_launch_resume_switcher = false;
    jw_log_info("spawned RetroArch pid=%d retroarch=%s", (int)pid, retroarch);
    jw__retroarch_session_start(state, pid, state->pending_launch_system, rom_abs,
                                state->pending_launch_rom_path, source_root,
                                core, core_id, runtime_config, persist_config,
                                audio_bluetooth);
    bool post_launch_resume = switcher_resume && !entryslot_resume;
    if (post_launch_resume) {
        state->post_launch_resume_pending = true;
        state->post_launch_resume_attempts = 0;
        state->post_launch_resume_next_ms = jw__monotonic_ms();
    } else {
        state->post_launch_resume_pending = false;
        state->post_launch_resume_attempts = 0;
        state->post_launch_resume_next_ms = 0;
    }

    /* Let RetroArch own the first startup window before cold-starting the
       hidden standby menu's SDL/GL/input stack. */
    if (!post_launch_resume) {
        jw__schedule_in_game_menu_prewarm(state, JW_INGAME_MENU_PREWARM_DELAY_MS);
    }
    jw_log_info("RetroArch launch timings: total_ms=%lld config_ms=%lld state_resolve_ms=%lld fork_ms=%lld entryslot=%s post_resume=%s",
                fork_done_ms - launch_start_ms,
                config_done_ms - config_start_ms,
                state_resolve_done_ms - state_resolve_start_ms,
                fork_done_ms - fork_start_ms,
                entryslot_resume ? entryslot_arg : "none",
                post_launch_resume ? "true" : "false");

    free(retroarch);
    free(core);
    free(runtime_config);
    free(ra_home);
    return 0;

fail:
    state->pending_launch = false;
    state->pending_launch_resume_switcher = false;
    state->post_launch_resume_pending = false;
    jw__cancel_in_game_menu_prewarm(state);
    jw__retroarch_session_clear(&state->retroarch_session);
    free(retroarch);
    free(core);
    free(runtime_config);
    free(ra_home);
    return -1;
}

/* ── Auto-sleep ───────────────────────────────────────────────────────────
   Idle (no button input, tracked globally by the input proxy so it covers games
   too) → blank the screen → suspend-to-RAM. Tiered: the screen-off stage wakes
   on any button; the suspend stage wakes on the power button. */
#define JW_AUTOSLEEP_DEFAULT_S        0       /* Off when the setting is unset.
                                                 Deep-suspend wake is not yet
                                                 reliable; auto-sleep is opt-in. */
#define JW_AUTOSLEEP_SUSPEND_GRACE_MS 30000   /* screen-off → suspend after this much more idle */
#define JW_AUTOSLEEP_SETTING_POLL_MS  2000    /* re-read the DB setting at most this often */
#define JW_CHARGING_STATUS_POLL_MS    1000    /* while in standby, notice unplug promptly */
#define JW_POWER_LONGPRESS_MS         2000    /* power held this long → clean power off (before
                                                 the PMIC hard-cut at ~6s) */

static int jw__autosleep_read_timeout_s(jw_daemon_state *state) {
    if (!state->db_path) {
        return JW_AUTOSLEEP_DEFAULT_S;
    }
    char val[32];
    if (jw_db_get_setting(state->db_path, "auto_sleep_seconds", val, sizeof(val)) == 0 &&
        val[0]) {
        int seconds = atoi(val);
        return seconds > 0 ? seconds : 0;   /* 0 = explicitly off */
    }
    return JW_AUTOSLEEP_DEFAULT_S;
}

static void jw__screen_set(jw_daemon_state *state, bool on) {
    jw_platform_result result;
    jw_platform_perform_action(&state->platform,
                               on ? JW_PLATFORM_ACTION_SCREEN_ON
                                  : JW_PLATFORM_ACTION_SCREEN_OFF,
                               0, &result);
}

static bool jw__standby_active(const jw_daemon_state *state) {
    return state && state->standby_reason != JW_STANDBY_NONE;
}

static bool jw__charging_online(jw_daemon_state *state, bool force) {
    if (!state) {
        return false;
    }

    long long now = jw__monotonic_ms();
    if (!force && state->charging_cached >= 0 && now < state->charging_next_poll_ms) {
        return state->charging_cached == 1;
    }

    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);
    state->charging_cached = status.charging;
    state->charging_next_poll_ms = now + JW_CHARGING_STATUS_POLL_MS;
    return status.charging == 1;
}

static void jw__enter_standby_screen_off(jw_daemon_state *state,
                                         jw_standby_reason reason,
                                         bool reset_idle) {
    if (!state) {
        return;
    }

    if (reset_idle) {
        jw_input_proxy_mark_activity(&state->input_proxy);
    }
    if (!jw__standby_active(state)) {
        jw__screen_set(state, false);
        jw_input_proxy_set_swallow(&state->input_proxy, true);
    }
    state->standby_reason = reason;
    state->standby_entered_ms = jw__monotonic_ms();
    state->power_sleep_armed = false;
    state->autosleep_charging_logged = false;
}

static void jw__leave_standby_screen_off(jw_daemon_state *state) {
    if (!jw__standby_active(state)) {
        return;
    }

    jw__screen_set(state, true);
    jw_input_proxy_set_swallow(&state->input_proxy, false);
    state->standby_reason = JW_STANDBY_NONE;
    state->standby_entered_ms = 0;
    state->autosleep_charging_logged = false;
}

static bool jw__standby_has_wake_activity(jw_daemon_state *state) {
    if (!jw__standby_active(state) || state->standby_entered_ms <= 0) {
        return false;
    }

    long long now = jw__monotonic_ms();
    if (now <= state->standby_entered_ms) {
        return false;
    }
    uint64_t since_standby_ms = (uint64_t)(now - state->standby_entered_ms);
    uint64_t idle_ms = jw_input_proxy_idle_ms(&state->input_proxy);
    return idle_ms < since_standby_ms;
}

/* Keep STOCK loong_power's auto-sleep DISABLED. jawakad owns auto-sleep through the
   input proxy, which tracks the gamepad input our EVIOCGRAB hides from stock. If we
   mirrored our timeout into stock, stock would auto-suspend on its own timer that
   never sees button/d-pad presses, so the device would sleep regardless of input
   (the bug). We therefore always force stock to 0 and govern sleep ourselves. */
static void jw__autosleep_sync_platform(jw_daemon_state *state) {
    if (!state || state->autosleep_platform_synced_s == 0) {
        return;   /* stock auto-sleep already disabled */
    }

    jw_platform_result result;
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_AUTO_SLEEP,
                               0, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        jw_log_info("auto-sleep: stock power auto-sleep disabled (jawakad governs)");
    } else if (result.code != JW_PLATFORM_RESULT_UNSUPPORTED) {
        jw_log_warn("auto-sleep: disabling stock power auto-sleep failed: %s",
                    result.message[0] ? result.message
                                      : jw_platform_result_code_name(result.code));
        return;
    }
    state->autosleep_platform_synced_s = 0;
}

/* Real suspend-to-RAM and wake handling, shared by the auto-sleep timer and a power
   press. The proxy holds the power key exclusively, so loong_power can't re-suspend
   us on the wake press and the kernel wakes on a single press; we drop any input
   that queued while asleep (gamepad replay + the wake press itself) and reset the
   idle timer so the countdown restarts fresh after waking. */
static void jw__deep_suspend(jw_daemon_state *state) {
    jw__schedule_retroarch_audio_reinit_if_bluetooth(state, "wake-bluetooth");
    jw__screen_set(state, false);
    jw_input_proxy_set_swallow(&state->input_proxy, true);
    jw_platform_result result;
    jw__platform_sleep_with_performance(state, &result);   /* blocks until resume */
    jw_log_info("sleep: resumed");
    jw__screen_set(state, true);
    jw__reconcile_audio(state, "wake", true);
    jw_input_proxy_set_swallow(&state->input_proxy, false);
    jw_input_proxy_flush(&state->input_proxy);             /* drop queued gamepad + wake press */
    /* Clear the wake press's edges + disarm, so resuming doesn't read it as a new
       sleep request. */
    jw_power_edge edge;
    while (jw_input_proxy_take_power_edge(&state->input_proxy, &edge)) {
        /* discard */
    }
    state->power_sleep_armed = false;
    state->power_held = false;
    jw_input_proxy_mark_activity(&state->input_proxy);
    state->standby_reason = JW_STANDBY_NONE;
    state->standby_entered_ms = 0;
    state->autosleep_charging_logged = false;
    state->autosleep_setting_next_ms = 0;   /* re-read the setting promptly after wake */
}

/* HDMI hotplug: on plug, apply the persisted HDMI output mode (4:3/stretch); on
   unplug, revert to the panel (single-head TV mode would otherwise leave the
   device black). Reads the cable status straight from the DRM connector and the
   chosen mode from the settings DB (shared with the launcher). ~1s poll. */
#define JW_HDMI_POLL_MS 1000
static int jw__hdmi_connected_now(void) {
    FILE *fp = fopen("/sys/class/drm/card0-HDMI-A-1/status", "r");
    if (!fp) {
        return 0;
    }
    char s[32] = { 0 };
    char *got = fgets(s, sizeof(s), fp);
    fclose(fp);
    if (!got) {
        return 0;
    }
    s[strcspn(s, "\r\n")] = '\0';
    return strcmp(s, "connected") == 0 ? 1 : 0;
}

/* Is the live HDMI scanout 1080p120? 120Hz only exists at 1080p on this chain, so
   the "1920x1080p120" mode string is unique to an HDMI 120Hz output. */
static int jw__hdmi_live_is_1080p120(void) {
    FILE *fp = fopen("/sys/kernel/debug/dri/0/summary", "r");
    if (!fp) {
        return 0;
    }
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "1920x1080p120")) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static void jw__tick_hdmi(jw_daemon_state *state) {
    long long now = jw__monotonic_ms();
    if (now < state->hdmi_next_poll_ms) {
        return;
    }
    state->hdmi_next_poll_ms = now + JW_HDMI_POLL_MS;

    /* ── Auto-revert safety for 1080p120 ──
       A 1080p120 switch can black out a TV/cable that can't carry the 297MHz signal,
       stranding the user on a screen they can't see to navigate back. When 1080p120
       goes live by a deliberate change (skip the boot-apply window), arm a 15s
       deadline; the launcher's "keep" press clears it, otherwise revert to the safe,
       universal 720p60 and drop the saved rate to 60 so it sticks. */
    int is120 = jw__hdmi_live_is_1080p120();
    if (is120 && !state->hdmi_was_120 && now > 30000) {
        state->hdmi_revert_deadline_ms = now + 15000;
        jw_log_info("HDMI 1080p120 live -> auto-revert armed (15s)");
    } else if (!is120) {
        state->hdmi_revert_deadline_ms = 0;
    }
    state->hdmi_was_120 = is120;
    if (state->hdmi_revert_deadline_ms != 0 && now > state->hdmi_revert_deadline_ms) {
        state->hdmi_revert_deadline_ms = 0;
        state->hdmi_was_120 = 0;
        jw_log_info("HDMI 1080p120 not kept -> reverting to 720p60");
        jw_platform_result rres;
        jw_platform_perform_action(&state->platform,
                                   JW_PLATFORM_ACTION_SET_REFRESH_RATE, 60, &rres);
        if (state->db_path) {
            jw_db_set_setting(state->db_path, "refresh_rate_hz", "60");
        }
    }

    int cur = jw__hdmi_connected_now();
    int prev = state->hdmi_last_connected;
    if (cur == prev) {
        return;
    }
    state->hdmi_last_connected = cur;

    /* The persisted HDMI Output setting (0 off / 1 4:3 / 2 stretch). */
    int mode = 0;
    if (state->db_path) {
        char val[16];
        if (jw_db_get_setting(state->db_path, "hdmi_output_mode", val, sizeof(val)) == 0) {
            mode = atoi(val);
            if (mode < 0 || mode > 2) mode = 0;
        }
    }
    if (mode == 0) {
        return;   /* feature disabled -> ignore plug/unplug */
    }

    jw_platform_result res;
    if (cur == 1) {
        /* plugged in (incl. the first poll after boot) -> apply the chosen mode */
        jw_platform_perform_action(&state->platform,
                                   JW_PLATFORM_ACTION_SET_HDMI_OUTPUT, mode, &res);
        jw_log_info("HDMI hotplug: connected -> applying mode %d", mode);
    } else if (prev == 1) {
        /* was connected, now unplugged -> back to the panel */
        jw_platform_perform_action(&state->platform,
                                   JW_PLATFORM_ACTION_SET_HDMI_OUTPUT, 0, &res);
        jw_log_info("HDMI hotplug: disconnected -> reverting to panel");
    }
}

static void jw__tick_auto_sleep(jw_daemon_state *state) {
    long long now = jw__monotonic_ms();

    if (now >= state->autosleep_setting_next_ms) {
        state->autosleep_timeout_s = jw__autosleep_read_timeout_s(state);
        state->autosleep_setting_next_ms = now + JW_AUTOSLEEP_SETTING_POLL_MS;
        jw__autosleep_sync_platform(state);
    }

    /* Power button — jawakad owns the key exclusively, so it must do everything
       loong_power used to: short tap = sleep, long hold = clean power off.
         - dark screen + press -> wake immediately (responsive).
         - lit screen  + press -> arm; sleep on RELEASE, so a long hold doesn't flash
                                  to sleep first.
         - held >= JW_POWER_LONGPRESS_MS -> clean power off (sync + reboot) BEFORE the
                                  PMIC hard-cuts (a hard cut can lose unsynced writes
                                  / corrupt the settings DB).
       Works regardless of the auto-sleep timeout setting. Hold durations come
       from the edges' own kernel timestamps (CLOCK_MONOTONIC, same domain as
       `now`), so a press and release that both queued behind a stalled tick
       still classify by their true spacing instead of reading as a 0ms tap. */
    {
        jw_power_edge edge;
        while (jw_input_proxy_take_power_edge(&state->input_proxy, &edge)) {
            if (edge.down) {
                state->power_held = true;
                state->power_down_ms = (long long)edge.ms;
                if (jw__standby_active(state)) {
                    jw_input_proxy_mark_activity(&state->input_proxy);  /* wake the dark screen */
                    state->power_sleep_armed = false;
                } else {
                    state->power_sleep_armed = true;                    /* sleep on release */
                }
                continue;
            }
            /* Release edge. */
            bool was_held = state->power_held;
            long long held_ms = was_held ? (long long)edge.ms - state->power_down_ms : 0;
            state->power_held = false;
            if (was_held && held_ms >= JW_POWER_LONGPRESS_MS) {
                state->power_sleep_armed = false;
                jw_log_info("power: long-press (%lldms) -> clean power off", held_ms);
                jw_platform_result poff;
                jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_POWEROFF, 0, &poff);
                return;
            }
            if (state->power_sleep_armed) {
                state->power_sleep_armed = false;
                if (jw__charging_online(state, true)) {
                    jw_log_info("power: standby while charging (tap)");
                    jw__enter_standby_screen_off(state, JW_STANDBY_POWER_CHARGING, true);
                } else {
                    jw_log_info("power: sleep (tap)");
                    jw__deep_suspend(state);
                }
                return;
            }
        }
        /* Still held with no release edge yet: power off the moment the hold
           crosses the threshold rather than waiting for the release. */
        if (state->power_held && now - state->power_down_ms >= JW_POWER_LONGPRESS_MS) {
            state->power_held = false;
            state->power_sleep_armed = false;
            jw_log_info("power: long-press -> clean power off");
            jw_platform_result poff;
            jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_POWEROFF, 0, &poff);
            return;
        }
    }

    if (jw__standby_has_wake_activity(state)) {
        jw_log_info("standby: wake on input");
        jw__leave_standby_screen_off(state);
        return;
    }

    /* Shutting down: ensure the screen is on and bail. */
    if (state->shutdown_requested) {
        jw__leave_standby_screen_off(state);
        return;
    }

    if (state->standby_reason == JW_STANDBY_POWER_CHARGING &&
        !jw__charging_online(state, false)) {
        long long standby_ms = state->standby_entered_ms > 0
            ? now - state->standby_entered_ms
            : JW_AUTOSLEEP_SUSPEND_GRACE_MS;
        if (standby_ms >= JW_AUTOSLEEP_SUSPEND_GRACE_MS) {
            jw_log_info("standby: charger unplugged -> sleep (standby %lldms)",
                        standby_ms);
            jw__deep_suspend(state);
        }
        return;
    }

    /* Auto-sleep disabled: undo only an auto-sleep blank. Power-button charging
       standby is handled above: it stays dark while plugged in, or suspends once
       unplugged after the usual grace window. */
    if (state->autosleep_timeout_s <= 0) {
        if (state->standby_reason == JW_STANDBY_AUTOSLEEP) {
            jw__leave_standby_screen_off(state);
        }
        return;
    }

    uint64_t idle_ms = jw_input_proxy_idle_ms(&state->input_proxy);
    uint64_t screen_off_at = (uint64_t)state->autosleep_timeout_s * 1000u;
    uint64_t suspend_at = screen_off_at + JW_AUTOSLEEP_SUSPEND_GRACE_MS;

    if (idle_ms < screen_off_at) {
        /* Active: undo an auto-sleep blank. Manual charging standby stays dark
           until the wake-activity check above sees a new button press. */
        if (state->standby_reason == JW_STANDBY_AUTOSLEEP) {
            jw__leave_standby_screen_off(state);
        }
        return;
    }

    if (idle_ms < suspend_at) {
        /* Stage 1 — blank the backlight. The proxy swallows input while blanked: a
           press resets the idle timer (waking the screen next tick) but is NOT
           forwarded, so the wake press only wakes the screen instead of also firing
           a navigation action. A power press here is routed above and wakes too. */
        if (!jw__standby_active(state)) {
            jw_log_info("auto-sleep: screen off (idle %llums)",
                        (unsigned long long)idle_ms);
            jw__enter_standby_screen_off(state, JW_STANDBY_AUTOSLEEP, false);
        }
        return;
    }

    if (jw__charging_online(state, false)) {
        if (!jw__standby_active(state)) {
            jw__enter_standby_screen_off(state, JW_STANDBY_AUTOSLEEP, false);
        }
        if (!state->autosleep_charging_logged) {
            jw_log_info("auto-sleep: standby while charging (idle %llums)",
                        (unsigned long long)idle_ms);
            state->autosleep_charging_logged = true;
        }
        return;
    }

    /* Stage 2 — real suspend-to-RAM. */
    jw_log_info("auto-sleep: suspending (idle %llums)", (unsigned long long)idle_ms);
    jw__deep_suspend(state);
}

static void jw__tick_post_launch_resume(jw_daemon_state *state) {
    if (!state || !state->post_launch_resume_pending) {
        return;
    }
    if (!state->retroarch_session.active || state->child_kind != JW_CHILD_RETROARCH) {
        state->post_launch_resume_pending = false;
        state->post_launch_resume_attempts = 0;
        state->post_launch_resume_next_ms = 0;
        return;
    }

    long long now = jw__monotonic_ms();
    if (state->post_launch_resume_next_ms > now) {
        return;
    }

    state->post_launch_resume_attempts++;
    jw_ra_client ra = jw_ra_client_default();
    ra.timeout_ms = 150u;

    jw_ra_info info;
    memset(&info, 0, sizeof(info));
    jw_ra_result info_result = jw_ra_get_info(&ra, &info);
    if (info_result != JW_RA_OK) {
        if (state->post_launch_resume_attempts >= JW_SWITCHER_RESUME_MAX_ATTEMPTS) {
            jw_log_warn("switcher resume: RetroArch command interface not ready result=%s",
                        jw_ra_result_string(info_result));
            state->post_launch_resume_pending = false;
            state->post_launch_resume_attempts = 0;
            state->post_launch_resume_next_ms = 0;
            jw__schedule_in_game_menu_prewarm(state,
                                              JW_INGAME_MENU_PREWARM_AFTER_RESUME_MS);
        } else {
            state->post_launch_resume_next_ms = now + JW_SWITCHER_RESUME_RETRY_MS;
        }
        return;
    }

    /* Don't commit (pending=false) until the load actually succeeds. The readiness
       probe (jw_ra_get_info) can answer before the core+content are ready to accept
       a state load, so the first load can time out; in that case keep retrying
       within the same attempt budget rather than cold-starting. Only the permanent
       failures below (no savestate support / no state on disk) give up immediately. */
    bool give_up = false;

    if (!info.savestate_supported) {
        jw_log_warn("switcher resume: core does not support savestates");
        give_up = true;
    }

    char states_dir[PATH_MAX];
    const char *source_root = state->retroarch_session.source_root[0]
        ? state->retroarch_session.source_root
        : state->sdcard_root;
    if (!give_up &&
        (!source_root ||
         snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) >=
             (int)sizeof(states_dir))) {
        jw_log_warn("switcher resume: states path unavailable");
        give_up = true;
    }

    int slot = 0;
    char state_path[PATH_MAX];
    state_path[0] = '\0';
    if (!give_up &&
        !jw_ra_find_resume_state(states_dir, state->retroarch_session.rom_path,
                                 JW_RA_GAME_SWITCHER_STATE_SLOT,
                                 &slot, state_path, sizeof(state_path))) {
        jw_log_info("switcher resume: no state found for %s",
                    state->retroarch_session.rom_path);
        give_up = true;
    }

    if (!give_up) {
        char reply[JW_RA_REPLY_MAX];
        ra.timeout_ms = JW_SWITCHER_RESUME_LOAD_TIMEOUT_MS;
        jw_ra_result load = jw_ra_load_state_slot(&ra, slot, reply, sizeof(reply));
        if (load == JW_RA_OK) {
            jw_ra_resume_direct(&ra);
            jw_log_info("switcher resume: loaded slot=%d path=%s", slot, state_path);
        } else if (state->post_launch_resume_attempts < JW_SWITCHER_RESUME_MAX_ATTEMPTS) {
            /* RA accepted commands but isn't ready to load the state yet — retry
               next tick; pending stays set, attempts already counted at the top. */
            state->post_launch_resume_next_ms = now + JW_SWITCHER_RESUME_RETRY_MS;
            return;
        } else {
            jw_log_warn("switcher resume: load failed slot=%d path=%s result=%s",
                        slot, state_path, jw_ra_result_string(load));
        }
    }

    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    state->post_launch_resume_next_ms = 0;
    jw__schedule_in_game_menu_prewarm(state,
                                      JW_INGAME_MENU_PREWARM_AFTER_RESUME_MS);
}

/* One-shot startup maintenance, armed before the launcher spawns and pulled
   forward by the frontend-ready handler (fallback deadline covers a launcher
   that never reports ready). Tiny maintenance still runs inline; the library
   scan is handed to a worker so the daemon IPC loop stays responsive. */
static void jw__tick_startup_maintenance(jw_daemon_state *state) {
    if (!state || !state->startup_maintenance_pending) {
        return;
    }
    if (state->shutdown_requested) {
        state->startup_maintenance_pending = false;
        return;
    }
    long long now = jw__monotonic_ms();
    if (state->startup_maintenance_next_ms > now) {
        return;
    }

    if (state->startup_maintenance_phase == 0) {
        long long wifi_start_ms = jw__monotonic_ms();
        int restored = jw_wifi_restore();
        int hardened = jw_wifi_harden();
        jw_log_info("wifi startup maintenance restore=%d harden=%d total_ms=%lld",
                    restored, hardened, jw__monotonic_ms() - wifi_start_ms);
        state->startup_maintenance_phase = 1;
        state->startup_maintenance_next_ms = jw__monotonic_ms();
        return;
    }

    state->startup_maintenance_pending = false;
    if (state->library_scanned_since_boot) {
        jw_log_info("startup library scan skipped; library already scanned this boot");
        return;
    }

    bool scan_active = false;
    if (state->scan_job.initialized) {
        pthread_mutex_lock(&state->scan_job.mu);
        scan_active = state->scan_job.running || state->scan_job.completed;
        pthread_mutex_unlock(&state->scan_job.mu);
    }
    if (scan_active) {
        jw_log_info("startup library scan already running");
        return;
    }

    if (jw__start_scan_job(state, "at startup (deferred)") < 0) {
        jw_log_warn("startup library scan could not start; launcher will use existing cache if available");
    }
}

static int jw__handle_scan(jw_daemon_state *state, jw_ipc_client *client) {
    int rc = jw__start_scan_job(state, "requested");
    if (rc < 0) {
        return jw__reply_error(client, "scan-library could not start");
    }

    return jw__reply_ok(client, rc > 0 ? "scan-library queued" : "scan-library started", NULL);
}

static int jw__reply_retroarch_session(jw_daemon_state *state, jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "retroarch-session");
    cJSON_AddBoolToObject(root, "active",
                          jw__has_retroarch_session(state));
    cJSON_AddNumberToObject(root, "resident_switch_max",
                            jw__resident_switch_max());

    if (!jw__has_retroarch_session(state)) {
        cJSON_AddBoolToObject(root, "command_ok", false);
        cJSON_AddStringToObject(root, "command_result", "inactive");
        return jw__reply_json(client, root);
    }

    cJSON_AddStringToObject(root, "system", state->retroarch_session.system);
    cJSON_AddStringToObject(root, "rom_path", state->retroarch_session.rom_path);
    cJSON_AddStringToObject(root, "core_path", state->retroarch_session.core_path);
    cJSON_AddStringToObject(root, "core_id", state->retroarch_session.core_id);
    cJSON_AddNumberToObject(root, "resident_switches",
                            state->retroarch_session.resident_switches);

    jw_ra_client ra = jw_ra_client_default();
    jw_ra_info info;
    jw_ra_result result = jw_ra_get_info(&ra, &info);
    cJSON_AddBoolToObject(root, "command_ok", result == JW_RA_OK);
    cJSON_AddStringToObject(root, "command_result", jw_ra_result_string(result));

    if (result == JW_RA_OK) {
        cJSON_AddNumberToObject(root, "disk_count", info.disk_count);
        if (info.disk_slot >= 0) {
            cJSON_AddNumberToObject(root, "disk_slot", info.disk_slot);
        } else {
            cJSON_AddNullToObject(root, "disk_slot");
        }
        cJSON_AddBoolToObject(root, "savestate_supported",
                              info.savestate_supported);
        cJSON_AddNumberToObject(root, "state_slot", info.state_slot);
    }

    return jw__reply_json(client, root);
}

static int jw__reply_retroarch_result(jw_ipc_client *client, const char *action,
                                      jw_ra_result result) {
    bool ok = result == JW_RA_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", ok ? "ok" : "error");
    cJSON_AddStringToObject(root, "action", action ? action : "");
    cJSON_AddStringToObject(root, "result", jw_ra_result_string(result));
    cJSON_AddStringToObject(root, "message", jw_ra_result_string(result));
    return jw__reply_json(client, root);
}

static jw_ra_result jw__set_state_slot_delta(const jw_ra_client *ra, int delta) {
    int slot = -1;
    bool supported = false;
    jw_ra_result result = jw_ra_get_state_slot(ra, &slot, &supported);
    if (result != JW_RA_OK) {
        return result;
    }
    if (!supported) {
        return JW_RA_UNSUPPORTED;
    }

    slot += delta;
    if (slot < -1) {
        slot = -1;
    }
    return jw_ra_set_state_slot(ra, slot);
}

static jw_ra_result jw__set_disk_slot_delta(const jw_ra_client *ra, int delta) {
    int count = 0;
    int slot = -1;
    jw_ra_result result = jw_ra_get_disk_count(ra, &count);
    if (result != JW_RA_OK) {
        return result;
    }
    if (count <= 1) {
        return JW_RA_UNSUPPORTED;
    }

    result = jw_ra_get_disk_slot(ra, &slot);
    if (result != JW_RA_OK) {
        return result;
    }
    if (slot < 0 || slot >= count) {
        slot = 0;
    }

    int next = (slot + delta) % count;
    if (next < 0) {
        next += count;
    }
    return jw_ra_set_disk_slot(ra, next);
}

static int jw__handle_retroarch_action(jw_daemon_state *state, jw_ipc_client *client,
                                       cJSON *root) {
    cJSON *action_json = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action_json) || !action_json->valuestring ||
        !action_json->valuestring[0]) {
        return jw__reply_error(client, "missing RetroArch action");
    }
    if (!jw__has_retroarch_session(state)) {
        return jw__reply_error(client, "no active RetroArch session");
    }

    const char *action = action_json->valuestring;
    cJSON *value_json = cJSON_GetObjectItemCaseSensitive(root, "value");
    bool has_value = cJSON_IsNumber(value_json);
    int value = has_value ? value_json->valueint : 0;
    jw_ra_client ra = jw_ra_client_default();
    jw_ra_result result = JW_RA_UNSUPPORTED;

    if (strcmp(action, "continue") == 0) {
        /* Explicit UNPAUSE, symmetric with the explicit PAUSE on open. The
           status-polling jw_ra_resume() could leave the game stuck paused when
           GET_STATUS misreports (e.g. right after a state load). */
        result = jw__resume_game_after_menu(state, &ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "settings") == 0) {
        result = jw_ra_open_menu(&ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "quit") == 0) {
        result = jw_ra_quit(&ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "save-and-quit") == 0) {
        /* Snapshot into the reserved game-switcher slot so the game returns to
           the switcher/Recents resumable and with a screenshot, then quit. The
           save is best-effort: if savestates are unsupported or the save fails,
           quit anyway so the user is never stranded. Mirrors the save block in
           jw__request_switch_game. */
        jw_ra_info info;
        memset(&info, 0, sizeof(info));
        if (jw_ra_get_info(&ra, &info) == JW_RA_OK && info.savestate_supported) {
            char reply[JW_RA_REPLY_MAX];
            jw_ra_result sv = jw_ra_save_state_slot(
                &ra, JW_RA_GAME_SWITCHER_STATE_SLOT, reply, sizeof(reply));
            if (sv != JW_RA_OK) {
                jw_log_warn("save-and-quit: slot %d save failed result=%s; quitting anyway",
                            JW_RA_GAME_SWITCHER_STATE_SLOT, jw_ra_result_string(sv));
            } else {
                jw_log_info("save-and-quit: saved state slot=%d",
                            JW_RA_GAME_SWITCHER_STATE_SLOT);
                /* Let the async state write finish before we quit RetroArch,
                   otherwise the resume state is truncated and crashes on load. */
                jw__wait_for_savestate_write(state, JW_RA_GAME_SWITCHER_STATE_SLOT);
            }
        } else {
            jw_log_info("save-and-quit: savestates unavailable; quitting without save");
        }
        result = jw_ra_quit(&ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "reset") == 0) {
        result = jw_ra_reset(&ra);
        if (result == JW_RA_OK) {
            /* Reset/Save/Load close the menu (the UI sets running=false), so
               resume the game now. The resident menu never exits, so the old
               resume-on-menu-exit path can't do it for us. */
            jw__resume_game_after_menu(state, &ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "save-state") == 0) {
        if (has_value) {
            /* value >= -1: the slot variant handles the auto slot (-1) too. */
            char reply[JW_RA_REPLY_MAX];
            result = jw_ra_save_state_slot(&ra, value, reply, sizeof(reply));
        } else {
            result = jw_ra_save_state(&ra);
        }
        if (result == JW_RA_OK) {
            jw__resume_game_after_menu(state, &ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "load-state") == 0) {
        if (has_value) {
            /* value >= -1: the slot variant handles the auto slot (-1) too. */
            char reply[JW_RA_REPLY_MAX];
            result = jw_ra_load_state_slot(&ra, value, reply, sizeof(reply));
        } else {
            result = jw_ra_load_state(&ra);
        }
        if (result == JW_RA_OK) {
            jw__resume_game_after_menu(state, &ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "state-slot-prev") == 0) {
        result = jw__set_state_slot_delta(&ra, -1);
    } else if (strcmp(action, "state-slot-next") == 0) {
        result = jw__set_state_slot_delta(&ra, +1);
    } else if (strcmp(action, "disk-prev") == 0) {
        result = jw__set_disk_slot_delta(&ra, -1);
    } else if (strcmp(action, "disk-next") == 0) {
        result = jw__set_disk_slot_delta(&ra, +1);
    }

    jw_log_info("retroarch-action requested action=%s result=%s",
                action, jw_ra_result_string(result));
    return jw__reply_retroarch_result(client, action, result);
}

static int jw__handle_message(jw_daemon_state *state, jw_ipc_client *client, const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        jw_log_error("invalid json message");
        return jw__reply_error(client, "invalid json");
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return jw__reply_error(client, "missing type");
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        if (cJSON_IsString(role) && role->valuestring) {
            if (strcmp(role->valuestring, "launcher") == 0) {
                jw_log_info("launcher hello");
            } else if (strcmp(role->valuestring, "menu") == 0) {
                jw_log_info("menu hello");
            } else {
                jw_log_info("client hello role=%s", role->valuestring);
            }
        }
        cJSON_Delete(root);
        return jw__reply_hello_ok(client);
    }

    if (strcmp(type->valuestring, "hdmi-revert-status") == 0) {
        long long n = jw__monotonic_ms();
        int secs = 0;
        if (state->hdmi_revert_deadline_ms != 0 && state->hdmi_revert_deadline_ms > n) {
            secs = (int)((state->hdmi_revert_deadline_ms - n + 999) / 1000);
        }
        cJSON_Delete(root);
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "ok");
        cJSON_AddNumberToObject(reply, "seconds", secs);
        return jw__reply_json(client, reply);
    }
    if (strcmp(type->valuestring, "hdmi-revert-keep") == 0) {
        state->hdmi_revert_deadline_ms = 0;
        jw_log_info("HDMI 1080p120 kept by user");
        cJSON_Delete(root);
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "ok");
        return jw__reply_json(client, reply);
    }

    if (strcmp(type->valuestring, "scan-library") == 0) {
        cJSON_Delete(root);
        return jw__handle_scan(state, client);
    }

    if (strcmp(type->valuestring, "scrape-validate") == 0) {
        int rc = jw__handle_scrape_validate(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-start") == 0) {
        int rc = jw__handle_scrape_start(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-missing-counts") == 0) {
        int rc = jw__handle_scrape_missing_counts(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-status") == 0) {
        int rc = jw__reply_scrape_status(client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-queue") == 0) {
        int rc = jw__reply_scrape_queue(client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-cancel") == 0) {
        int rc = jw__handle_scrape_cancel(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-stop-all") == 0) {
        cJSON_Delete(root);
        return jw__handle_scrape_stop_all(state, client);
    }

    if (strcmp(type->valuestring, "scrape-clear-done") == 0) {
        cJSON_Delete(root);
        return jw__handle_scrape_clear_done(state, client);
    }

    if (strcmp(type->valuestring, "library-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_library_status(state, client);
    }

    if (strcmp(type->valuestring, "storage-status") == 0) {
        cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root, "source");
        char source[32] = "";
        if (cJSON_IsString(source_json) && source_json->valuestring) {
            snprintf(source, sizeof(source), "%s", source_json->valuestring);
        }
        cJSON_Delete(root);
        return jw__reply_storage_status(state, client, source[0] ? source : NULL);
    }

    if (strcmp(type->valuestring, "storage-action") == 0) {
        int rc = jw__handle_storage_action(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "open-menu") == 0) {
        bool in_game = jw__has_retroarch_session(state);
        if ((in_game ? jw__request_open_in_game_menu(state)
                     : jw__request_open_menu(state)) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, "open-menu failed");
        }
        jw_log_info("open-menu requested mode=%s", in_game ? "in-game" : "frontend");
        cJSON_Delete(root);
        return jw__reply_ok(client, "open-menu", NULL);
    }

    if (strcmp(type->valuestring, "open-switcher") == 0) {
        bool in_game = jw__has_retroarch_session(state);
        if (!in_game || jw__request_open_in_game_switcher(state) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client,
                in_game ? "open-switcher failed" : "no active RetroArch session");
        }
        jw_log_info("open-switcher requested");
        cJSON_Delete(root);
        return jw__reply_ok(client, "open-switcher", NULL);
    }

    if (strcmp(type->valuestring, "switch-game") == 0) {
        cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
        cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(root, "rom_path");
        const char *error_message = NULL;
        if (!cJSON_IsString(system) || !system->valuestring ||
            !cJSON_IsString(rom_path) || !rom_path->valuestring ||
            jw__request_switch_game(state, system->valuestring,
                                    rom_path->valuestring, &error_message) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client,
                error_message ? error_message : "switch-game failed");
        }

        jw_log_info("switch-game requested system=%s rom=%s",
                    system->valuestring, rom_path->valuestring);
        cJSON_Delete(root);
        return jw__reply_ok(client, "switch-game", NULL);
    }

    if (strcmp(type->valuestring, "retroarch-session") == 0) {
        cJSON_Delete(root);
        return jw__reply_retroarch_session(state, client);
    }

    if (strcmp(type->valuestring, "retroarch-action") == 0) {
        int rc = jw__handle_retroarch_action(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "launch-game") == 0) {
        cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
        cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(root, "rom_path");
        cJSON *resume_policy = cJSON_GetObjectItemCaseSensitive(root, "resume_policy");
        bool switcher_resume = false;
        if (cJSON_IsString(resume_policy) && resume_policy->valuestring &&
            resume_policy->valuestring[0]) {
            if (strcmp(resume_policy->valuestring, "switcher-latest") != 0) {
                cJSON_Delete(root);
                return jw__reply_error(client, "unknown resume policy");
            }
            switcher_resume = true;
        }
        const char *error_message = NULL;
        if (!cJSON_IsString(system) || !system->valuestring ||
            !cJSON_IsString(rom_path) || !rom_path->valuestring ||
            jw__request_launch_game(state, system->valuestring, rom_path->valuestring,
                                    switcher_resume, &error_message) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, error_message ? error_message : "launch-game failed");
        }

        jw_log_info("launch-game requested system=%s rom=%s resume=%s",
                    system->valuestring, rom_path->valuestring,
                    switcher_resume ? "switcher-latest" : "none");
        cJSON_Delete(root);
        return jw__reply_ok(client, "launch-game", NULL);
    }

    if (strcmp(type->valuestring, "launch-app") == 0) {
        cJSON *pak_dir = cJSON_GetObjectItemCaseSensitive(root, "pak_dir");
        const char *error_message = NULL;
        if (!cJSON_IsString(pak_dir) || !pak_dir->valuestring ||
            jw__request_launch_app(state, pak_dir->valuestring, &error_message) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, error_message ? error_message : "launch-app failed");
        }

        jw_log_info("launch-app requested pak=%s", pak_dir->valuestring);
        cJSON_Delete(root);
        return jw__reply_ok(client, "launch-app", NULL);
    }

    if (strcmp(type->valuestring, "reset-retroarch-config") == 0) {
        char status[256];
        if (jw_reset_retroarch_shared_config(state->sdcard_root,
                                             status, sizeof(status)) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, status[0] ? status : "reset failed");
        }

        jw_log_info("reset-retroarch-config requested");
        cJSON_Delete(root);
        return jw__reply_ok(client, status[0] ? status : "reset-retroarch-config", NULL);
    }

    if (strcmp(type->valuestring, "platform-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_platform_status(state, client);
    }

    if (strcmp(type->valuestring, "platform-audio-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_platform_audio_status(state, client);
    }

    if (strcmp(type->valuestring, "performance-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_performance_status(state, client);
    }

    if (strcmp(type->valuestring, "performance-set-profile") == 0) {
        int rc = jw__handle_performance_set_profile(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "performance-set-custom") == 0) {
        int rc = jw__handle_performance_set_custom(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "performance-reset-session") == 0) {
        cJSON_Delete(root);
        return jw__handle_performance_reset_session(state, client);
    }

    if (strcmp(type->valuestring, "update-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_update_status(state, client);
    }

    if (strcmp(type->valuestring, "update-check") == 0) {
        int rc = jw__handle_update_check(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "update-download") == 0) {
        cJSON_Delete(root);
        return jw__handle_update_download(state, client);
    }

    if (strcmp(type->valuestring, "update-select") == 0) {
        int rc = jw__handle_update_select(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "update-cancel") == 0) {
        cJSON_Delete(root);
        return jw__handle_update_cancel(state, client);
    }

    if (strcmp(type->valuestring, "update-install-preflight") == 0) {
        int rc = jw__handle_update_install_preflight(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "update-install") == 0) {
        int rc = jw__handle_update_install(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "platform-action") == 0) {
        cJSON *action_json = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (!cJSON_IsString(action_json) || !action_json->valuestring) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing platform action");
        }

        jw_platform_action action;
        if (!jw_platform_parse_action(action_json->valuestring, &action)) {
            jw_platform_result result;
            char action_name[64];
            snprintf(action_name, sizeof(action_name), "%s", action_json->valuestring);
            result.code = JW_PLATFORM_RESULT_INVALID;
            snprintf(result.message, sizeof(result.message), "unknown platform action: %s",
                     action_name);
            cJSON_Delete(root);
            return jw__reply_platform_result(client, action_name, &result);
        }

        int value = 0;
        cJSON *value_json = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (cJSON_IsNumber(value_json)) {
            value = value_json->valueint;
        }
        if (action == JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT) {
            cJSON *output_json = cJSON_GetObjectItemCaseSensitive(root, "output");
            jw_platform_audio_output parsed_output;
            if (cJSON_IsString(output_json) && output_json->valuestring &&
                jw_platform_parse_audio_output(output_json->valuestring, &parsed_output)) {
                value = (int)parsed_output;
            }
        }

        jw_platform_result result;
        if (action == JW_PLATFORM_ACTION_SET_BRIGHTNESS) {
            jw__set_brightness(state, value, true, true, &result);
        } else if (action == JW_PLATFORM_ACTION_SET_VOLUME) {
            jw_platform_perform_action(&state->platform, action, value, &result);
            if (result.code == JW_PLATFORM_RESULT_OK) {
                int resolved = result.has_value ? result.value : value;
                if (resolved < 0) resolved = 0;
                if (resolved > 100) resolved = 100;
                state->cached_volume_percent = resolved;
                jw__persist_volume(state, resolved);
                jw__osd_show_volume(state, resolved);
            }
        } else if (action == JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT) {
            jw_platform_perform_action(&state->platform, action, value, &result);
            if (result.code == JW_PLATFORM_RESULT_OK) {
                jw__publish_audio_env(state);
            }
        } else if (action == JW_PLATFORM_ACTION_SLEEP) {
            jw__platform_sleep_with_performance(state, &result);
        } else {
            jw_platform_perform_action(&state->platform, action, value, &result);
        }
        jw_log_info("platform-action requested action=%s code=%s",
                    jw_platform_action_name(action),
                    jw_platform_result_code_name(result.code));
        cJSON_Delete(root);
        return jw__reply_platform_result(client, jw_platform_action_name(action), &result);
    }

    if (strcmp(type->valuestring, "set-led") == 0) {
        jw_led_config led;
        memset(&led, 0, sizeof(led));
        led.mode = JW_LED_MODE_STATIC;
        led.brightness = 5;
        led.speed = 5;
        cJSON *v;
        v = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        led.enabled = v && (cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valueint));
        v = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (cJSON_IsString(v)) jw_led_mode_parse(v->valuestring, &led.mode);
        v = cJSON_GetObjectItemCaseSensitive(root, "r"); if (cJSON_IsNumber(v)) led.r = (unsigned char)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "g"); if (cJSON_IsNumber(v)) led.g = (unsigned char)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "b"); if (cJSON_IsNumber(v)) led.b = (unsigned char)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "brightness"); if (cJSON_IsNumber(v)) led.brightness = v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "speed"); if (cJSON_IsNumber(v)) led.speed = v->valueint;

        jw__apply_led_config(state, &led);
        jw__persist_led(state, &led);
        jw_log_info("set-led mode=%s enabled=%d", jw_led_mode_name(led.mode), led.enabled);
        jw_platform_result result;
        result.code = JW_PLATFORM_RESULT_OK;
        result.has_value = false;
        result.value = 0;
        snprintf(result.message, sizeof(result.message), "%s", "led applied");
        cJSON_Delete(root);
        return jw__reply_platform_result(client, "set-led", &result);
    }

    if (strcmp(type->valuestring, "frontend-ready") == 0) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        if (!cJSON_IsString(role) || !role->valuestring || !role->valuestring[0]) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing frontend role");
        }

        jw_platform_result result;
        jw_platform_frontend_ready(&state->platform, role->valuestring, &result);
        jw_log_info("frontend-ready role=%s code=%s",
                    role->valuestring, jw_platform_result_code_name(result.code));
        if (state->startup_maintenance_pending) {
            long long accel = jw__monotonic_ms() + JW_STARTUP_MAINT_GRACE_MS;
            if (accel < state->startup_maintenance_next_ms) {
                state->startup_maintenance_next_ms = accel;
            }
        }
        cJSON_Delete(root);
        return jw__reply_platform_result(client, "frontend-ready", &result);
    }

    if (strcmp(type->valuestring, "exit-stock") == 0) {
        state->shutdown_requested = true;
        char crash_state[PATH_MAX];
        if (jw__env_or_join(crash_state, sizeof(crash_state),
                            "UMRK_CRASH_STATE",
                            "UMRK_INTERNAL_DATA_PATH", "USERDATA_PATH",
                            NULL, "umrk-launcher-crash-state") == 0) {
            unlink(crash_state);
        }
        char exit_sentinel[PATH_MAX];
        if (jw__env_or_join(exit_sentinel, sizeof(exit_sentinel),
                            "UMRK_EXIT_TO_STOCK_SENTINEL",
                            "TMPDIR", NULL,
                            "/tmp", "umrk-exit-to-stock") == 0) {
            FILE *fp = fopen(exit_sentinel, "w");
            if (fp) fclose(fp);
        }
        jw_log_info("exit-stock requested - passing this boot to stock");
        cJSON_Delete(root);
        return jw__reply_ok(client, "exit-stock", NULL);
    }

    if (strcmp(type->valuestring, "shutdown") == 0) {
        state->shutdown_requested = true;
        jw_log_info("shutdown requested");
        cJSON_Delete(root);
        return jw__reply_ok(client, "shutdown", NULL);
    }

    cJSON_Delete(root);
    return jw__reply_error(client, "unknown type");
}

static int jw__accept_and_process(jw_daemon_state *state) {
    jw_ipc_client *client = NULL;
    int rc = jw_ipc_server_accept(state->server, &client, 50);
    if (rc != 0) {
        return rc;
    }

    char *body = NULL;
    size_t len = 0;
    int result = jw_ipc_client_recv(client, &body, &len);
    if (result == 0) {
        result = jw__handle_message(state, client, body);
    }

    free(body);
    jw_ipc_client_close(client);
    return result == 0 ? 0 : -1;
}

static void jw__clear_menu_tracking(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    state->menu_pid = -1;
    state->menu_in_game = false;
    state->menu_visible = false;
    state->retroarch_resume_on_menu_exit = false;
}

static void jw__terminate_menu_child(jw_daemon_state *state, bool force) {
    if (!state || state->menu_pid <= 0) {
        return;
    }

    pid_t pid = state->menu_pid;
    kill(pid, SIGTERM);

    int status = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            jw_log_info("in-game menu exited during cleanup status=%d", status);
            jw__clear_menu_tracking(state);
            return;
        }
        if (waited < 0) {
            if (errno != ECHILD) {
                jw_log_warn("in-game menu cleanup wait failed: %s",
                            strerror(errno));
            }
            jw__clear_menu_tracking(state);
            return;
        }
        usleep(50000);
    }

    if (force) {
        jw_log_warn("in-game menu did not exit on SIGTERM; forcing pid=%d",
                    (int)pid);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        jw__clear_menu_tracking(state);
    }
}

static void jw__handle_child_exit(jw_daemon_state *state) {
    if (!state || state->child_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t waited = waitpid(state->child_pid, &status, WNOHANG);
    if (waited == 0 || waited < 0) {
        return;
    }

    jw_child_kind exited_kind = state->child_kind;
    pid_t exited_pid = waited;
    state->child_pid = -1;
    state->child_kind = JW_CHILD_NONE;

    const char *name = jw__child_name(exited_kind);
    if (WIFEXITED(status)) {
        jw_log_info("%s exited status=%d", name ? name : "child", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("%s terminated signal=%d", name ? name : "child", WTERMSIG(status));
    } else {
        jw_log_warn("%s changed state status=%d", name ? name : "child", status);
    }

    if (exited_kind == JW_CHILD_RETROARCH) {
        jw__retroarch_session_finish(state, exited_pid, status);
        if (state->menu_pid > 0) {
            jw__terminate_menu_child(state, true);
        }
    }
    if (exited_kind == JW_CHILD_EMULATOR) {
        /* Inspect the switcher-request marker while the session is still intact,
           so it can be validated against the just-exited game. */
        jw__consume_standalone_switcher_marker(state, exited_pid);
        jw__standalone_session_finish(state, exited_pid, status);
    }

    if (state->shutdown_requested || g_shutdown_requested) {
        return;
    }

    if (exited_kind == JW_CHILD_APP || exited_kind == JW_CHILD_EMULATOR) {
        /* A standalone session leaves the watch-only proxy active; tear it
           down so the full grab-and-forward proxy can come back. */
        jw_input_proxy_shutdown(&state->input_proxy);
        jw__start_input_proxy(state);
    }

    /* Switch-game: the old RetroArch quit with a game already queued, so spawn
     * the selected game directly instead of returning to the launcher. The
     * session finish above already recorded the old game's playtime/recents. */
    if (exited_kind == JW_CHILD_RETROARCH && state->pending_launch) {
        if (jw__spawn_pending_game(state) != 0) {
            jw_log_warn("switch-game: next game spawn failed; returning to launcher");
            if (!state->daemon_only) {
                jw__spawn_child(state, JW_CHILD_LAUNCHER);
            }
        }
        return;
    }

    /* Spawn-on-exit model: the launcher sends a pending action, then exits
     * voluntarily. The daemon detects the exit here and owns the next process. */
    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_launch) {
        if (jw__spawn_pending_game(state) != 0 && !state->daemon_only) {
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
        }
        return;
    }

    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_app) {
        if (jw__spawn_app(state) != 0 && !state->daemon_only) {
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
        }
        return;
    }

    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_menu) {
        state->pending_menu = false;
        jw__spawn_child(state, JW_CHILD_MENU);
        return;
    }

    if (state->daemon_only) {
        return;
    }

    if (exited_kind == JW_CHILD_MENU) {
        jw__spawn_child(state, JW_CHILD_LAUNCHER);
        return;
    }

    if (exited_kind == JW_CHILD_RETROARCH || exited_kind == JW_CHILD_EMULATOR) {
        if (state->launcher_open_switcher) {
            /* A standalone Menu+Select asked us to reopen the launcher straight
               into the switcher, seeded on the just-exited game. The launcher
               inherits these env vars (execv keeps environ) and acts on them on
               its first frame; clear them again so later restarts start normally. */
            setenv("JAWAKA_OPEN_SWITCHER", "1", 1);
            setenv("JAWAKA_SWITCHER_SELECT_SYSTEM", state->launcher_switcher_system, 1);
            setenv("JAWAKA_SWITCHER_SELECT_ROM", state->launcher_switcher_rom, 1);
            state->launcher_open_switcher = false;
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
            unsetenv("JAWAKA_OPEN_SWITCHER");
            unsetenv("JAWAKA_SWITCHER_SELECT_SYSTEM");
            unsetenv("JAWAKA_SWITCHER_SELECT_ROM");
        } else {
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
        }
        return;
    }

    if (exited_kind == JW_CHILD_APP) {
        jw__spawn_child(state, JW_CHILD_LAUNCHER);
        return;
    }

    jw__spawn_child(state, JW_CHILD_LAUNCHER);
}

static void jw__handle_menu_exit(jw_daemon_state *state) {
    if (!state || state->menu_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t waited = waitpid(state->menu_pid, &status, WNOHANG);
    if (waited == 0) {
        return;
    }
    if (waited < 0) {
        if (errno != ECHILD) {
            jw_log_warn("in-game menu wait failed: %s", strerror(errno));
        }
        jw__clear_menu_tracking(state);
        return;
    }

    /* Reaching here means the standby exited on its own — i.e. it crashed,
       because intentional teardown at game exit clears menu_pid in
       jw__handle_child_exit before this runs. */
    bool was_visible = state->menu_visible;
    bool session_active = state->retroarch_session.active;
    bool shutting = state->shutdown_requested || g_shutdown_requested;
    jw__clear_menu_tracking(state);

    if (WIFEXITED(status)) {
        jw_log_info("in-game menu exited status=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("in-game menu terminated signal=%d", WTERMSIG(status));
    } else {
        jw_log_warn("in-game menu changed state status=%d", status);
    }

    if (!session_active || shutting) {
        return;
    }

    /* The game is still running. If the menu died while shown, RetroArch is
       paused under it — resume so the player isn't stuck on a frozen frame. */
    if (was_visible) {
        jw_ra_client client = jw_ra_client_default();
        jw_ra_result result = jw_ra_resume_direct(&client);
        if (result != JW_RA_OK) {
            jw_log_warn("in-game menu crash: resume failed result=%s",
                        jw_ra_result_string(result));
        }
    }

    /* Re-arm a hidden standby for the next Menu tap, with a per-session cap so
       a menu that crashes on startup can't spin. Past the cap we leave menu_pid
       clear and let the next tap take the on-demand fallback path. */
    if (state->menu_standby_attempts < 3) {
        state->menu_standby_attempts++;
        if (jw__spawn_in_game_menu(state, false) != 0) {
            jw_log_warn("standby in-game menu respawn failed (attempt %d/3)",
                        state->menu_standby_attempts);
        } else {
            jw_log_info("standby in-game menu respawned (attempt %d/3)",
                        state->menu_standby_attempts);
        }
    } else {
        jw_log_warn("standby in-game menu respawn cap reached; on-demand fallback only");
    }
}

static void jw__cleanup(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    jw__terminate_menu_child(state, true);

    if (state->child_pid > 0) {
        pid_t child_pid = state->child_pid;
        jw_child_kind child_kind = state->child_kind;
        int status = 0;
        kill(child_pid, SIGTERM);
        if (waitpid(child_pid, &status, 0) > 0 && child_kind == JW_CHILD_RETROARCH) {
            jw__retroarch_session_finish(state, child_pid, status);
        }
        state->child_pid = -1;
        state->child_kind = JW_CHILD_NONE;
    }

    if (state->osd_pid > 0) {
        kill(state->osd_pid, SIGTERM);
        waitpid(state->osd_pid, NULL, 0);
        state->osd_pid = -1;
    }

    jw__stop_ledd(state);

    jw_input_proxy_shutdown(&state->input_proxy);
    if (state->update_download_job.active) {
        jw_update_download_cancel(&state->update_status, &state->update_download_job);
    }
    jw_update_check_job_wait(&state->update_check_job);
    jw_scrape_worker_stop();
    jw__scan_job_shutdown(state);
    jw_platform_shutdown(&state->platform);
    jw_ipc_server_close(state->server);
    jw_db_close(state->db);
    free(state->runtime_dir);
    free(state->sdcard_root);
    free(state->socket_path);
    free(state->osd_socket_path);
    free(state->db_path);
    free(state->state_dir);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, jw__handle_signal);
    signal(SIGTERM, jw__handle_signal);
    signal(SIGPIPE, SIG_IGN);

    jw_daemon_state state;
    memset(&state, 0, sizeof(state));
    state.child_pid = -1;
    state.menu_pid = -1;
    state.osd_pid = -1;
    state.cached_brightness_percent = -1;
    state.cached_volume_percent = -1;
    state.ledd_pid = -1;
    state.led_configured = false;
    state.autosleep_platform_synced_s = -1;
    state.charging_cached = -1;
    state.perf_global_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    state.perf_active_profile = JW_PLATFORM_PERF_PROFILE_FRONTEND;
    state.perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    jw__perf_request_init(&state.perf_custom_request);
    jw_update_download_job_init(&state.update_download_job);
    jw_update_install_job_init(&state.update_install_job);
    jw_update_check_job_init(&state.update_check_job);
    jw__scan_job_init(&state);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon-only") == 0) {
            state.daemon_only = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            jw__print_usage(stdout);
            return 0;
        }

        jw__print_usage(stderr);
        jw_log_error("unknown argument: %s", argv[i]);
        return 2;
    }

    if (jw__set_bin_dir(argv[0], state.bin_dir, sizeof(state.bin_dir)) != 0) {
        jw_log_error("could not resolve binary directory");
        return 1;
    }

    state.runtime_dir = jw_runtime_dir();
    state.sdcard_root = jw_sdcard_root();
    state.socket_path = jw_socket_path();
    state.osd_socket_path = jw_osd_socket_path();
    state.db_path = jw_db_path();
    state.state_dir = jw_state_dir();
    if (!state.runtime_dir || !state.sdcard_root || !state.socket_path ||
        !state.osd_socket_path || !state.db_path || !state.state_dir) {
        jw_log_error("could not resolve runtime paths");
        jw__cleanup(&state);
        return 1;
    }

    if (jw_platform_init(&state.platform, state.runtime_dir, state.sdcard_root) != 0) {
        jw_log_error("could not initialize platform service");
        jw__cleanup(&state);
        return 1;
    }
    jw_update_status_init(&state.update_status, state.platform.platform_id,
                          state.state_dir);

    if (!jw__path_exists(state.sdcard_root)) {
        jw_log_error("sdcard root missing: %s (run 'make mockgen')", state.sdcard_root);
        jw__cleanup(&state);
        return 2;
    }

    if (jw_db_open(state.db_path, &state.db) != 0 || jw_db_apply_schema(state.db) != 0) {
        jw_log_error("could not open or initialize sqlite database: %s", state.db_path);
        jw__cleanup(&state);
        return 1;
    }
    jw__load_library_generation(&state);
    jw__perf_load_global(&state);
    (void)jw__perf_apply_frontend(&state, "startup");
    jw__apply_persisted_brightness(&state);
    jw__apply_persisted_volume(&state);
    jw__apply_persisted_led(&state);
    /* HDMI boot-apply happens via the first hotplug poll; defer it a few seconds
       so the launcher + OSD are up before any TV-switch weston restart. */
    state.hdmi_last_connected = -1;
    state.hdmi_next_poll_ms = jw__monotonic_ms() + 4000;

    if (jw_ipc_server_listen(state.socket_path, &state.server) != 0) {
        jw_log_error("could not bind socket: %s", state.socket_path);
        jw__cleanup(&state);
        return 1;
    }

    /* Exported so child processes receive them via execv's inherited environment. */
    jw__publish_runtime_path_env(&state);
    jw__publish_audio_env(&state);

    /* Export the user's time zone so launched apps (and the daemon's own
       localtime) use it. The launcher re-applies it live when changed. */
    {
        char tz[64] = "";
        if (state.db_path[0] &&
            jw_db_get_setting(state.db_path, "timezone", tz, sizeof(tz)) == 0 &&
            tz[0]) {
            setenv("TZ", tz, 1);
            tzset();
        }
    }

    setenv("JAWAKA_OSD_SOCKET", state.osd_socket_path, 1);
    setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 0);
    {
        char runner_path[PATH_MAX];
        if (snprintf(runner_path, sizeof(runner_path), "%s/jawaka-retroarch-runner",
                     state.bin_dir) < (int)sizeof(runner_path)) {
            setenv("JAWAKA_RETROARCH_RUNNER", runner_path, 1);
        }
    }

    jw__start_input_proxy(&state);

    jw_log_info("jawakad starting");
    jw_log_info("runtime dir: %s", state.runtime_dir);
    jw_log_info("sdcard root: %s", state.sdcard_root);
    jw_log_info("socket path: %s", state.socket_path);
    jw_log_info("osd socket path: %s", state.osd_socket_path);
    jw_log_info("db path: %s", state.db_path);
    jw_log_info("platform: %s (%s)", state.platform.platform_id, state.platform.platform_name);
    jw_log_info("platform script dir: %s", state.platform.script_dir);
    if (state.daemon_only) {
        jw_log_info("daemon-only mode enabled");
    }

    /* Use the cached DB for the first frame whenever possible. If the cache is
       absent/empty, start a background scan instead of blocking launcher spawn;
       otherwise defer a freshness scan past frontend-ready. */
    {
        bool need_initial_scan = state.library_generation <= 0;
        jw_library_summary summary;
        memset(&summary, 0, sizeof(summary));
        if (!need_initial_scan) {
            need_initial_scan = jw_db_read_summary(state.db_path, &summary) != 0 ||
                                (summary.game_count <= 0 && summary.app_count <= 0);
        }
        if (need_initial_scan) {
            state.library_populated = false;
            if (jw__start_scan_job(&state, "at startup") < 0) {
                jw_log_warn("startup library scan could not start; launcher will show cache if available");
            }
        } else {
            state.library_populated = summary.game_count > 0 || summary.app_count > 0;
            jw_log_info("startup library scan deferred (generation=%d)",
                        state.library_generation);
        }
    }

    state.startup_maintenance_pending = true;
    state.startup_maintenance_phase = 0;
    state.startup_maintenance_next_ms = jw__monotonic_ms() +
        (state.daemon_only ? 0 : JW_STARTUP_MAINT_FALLBACK_MS);

    if (jw_scrape_worker_start(state.db_path, state.sdcard_root) != 0) {
        jw_log_warn("scrape worker failed to start; scraping disabled this run");
    }

    jw__spawn_osd(&state);

    if (!state.daemon_only) {
        if (jw__spawn_child(&state, JW_CHILD_LAUNCHER) != 0) {
            jw__cleanup(&state);
            return 1;
        }
    }

    while (1) {
        if (g_shutdown_requested) {
            state.shutdown_requested = true;
        }

        /* Detect a resume from ANY suspend — our auto-sleep OR loong_power's power
           button — by the gap between BOOTTIME (counts suspend time) and MONOTONIC
           (does not). On resume: drop gamepad presses made while asleep so they
           don't replay into the launcher, and treat the wake as fresh activity so
           the auto-sleep countdown restarts (otherwise it resumes mid-count and the
           screen blanks again moments after waking). Runs before the input tick so
           the flush lands before any buffered events are forwarded. */
        {
            static long long prev_mono = -1, prev_boot = -1;
            struct timespec mts, bts;
            clock_gettime(CLOCK_MONOTONIC, &mts);
            clock_gettime(CLOCK_BOOTTIME, &bts);
            long long mono = (long long)mts.tv_sec * 1000 + mts.tv_nsec / 1000000;
            long long boot = (long long)bts.tv_sec * 1000 + bts.tv_nsec / 1000000;
            if (prev_mono >= 0) {
                long long suspended = (boot - prev_boot) - (mono - prev_mono);
                if (suspended > 2000) {
                    jw_log_info("resume: ~%lldms suspended, flushing input + resetting idle",
                                suspended);
                    jw_input_proxy_flush(&state.input_proxy);
                    jw_input_proxy_mark_activity(&state.input_proxy);
                    jw__reconcile_audio(&state, "resume-detect", true);
                    jw__schedule_retroarch_audio_reinit(&state, "resume-detect-bluetooth");
                }
            }
            prev_mono = mono;
            prev_boot = boot;
        }

        jw_update_download_poll(&state.update_status, &state.update_download_job);
        jw_update_install_poll(&state.update_status, &state.update_install_job);
        jw_update_check_poll(&state.update_status, &state.update_check_job);
        jw__handle_child_exit(&state);
        jw__tick_post_launch_resume(&state);
        jw__tick_in_game_menu_prewarm(&state);
        jw__handle_menu_exit(&state);
        jw__handle_osd_exit(&state);
        jw__handle_ledd_exit(&state);
        jw_input_proxy_tick(&state.input_proxy);
        unsigned audio_events = jw_platform_audio_tick(&state.platform);
        if (audio_events & JW_PLATFORM_AUDIO_EVENT_BLUETOOTH_CONNECTED) {
            jw__schedule_retroarch_audio_reinit(&state, "bluetooth-connected");
        }
        jw__tick_retroarch_audio_reinit(&state);
        jw__tick_auto_sleep(&state);
        jw__tick_hdmi(&state);
        if (jw_platform_storage_tick(&state.platform)) {
            if (jw__start_scan_job(&state, "after storage change") < 0) {
                jw_log_warn("storage hotplug: library rescan could not start");
            }
        }
        jw__tick_scan_job(&state);
        jw__tick_startup_maintenance(&state);

        if (state.shutdown_requested && state.child_pid <= 0 &&
            state.menu_pid <= 0) {
            break;
        }

        if (state.shutdown_requested && state.child_pid > 0) {
            kill(state.child_pid, SIGTERM);
            usleep(50000);
            if (kill(state.child_pid, 0) == 0) {
                kill(state.child_pid, SIGKILL);
            }
        }
        if (state.shutdown_requested && state.menu_pid > 0) {
            kill(state.menu_pid, SIGTERM);
            usleep(50000);
            if (kill(state.menu_pid, 0) == 0) {
                kill(state.menu_pid, SIGKILL);
            }
        }

        if (state.shutdown_requested) {
            usleep(50000);
            continue;
        }

        int rc = jw__accept_and_process(&state);
        if (rc < 0 && !state.shutdown_requested && !g_shutdown_requested) {
            jw_log_warn("ipc loop iteration failed");
        }
    }

    /* Write clean-exit marker so the Leaf boot supervisor's crash-loop guard
       knows this was an intentional shutdown, not a crash. The marker lives in
       tmpfs by default so it clears on reboot. */
    {
        char clean_exit[PATH_MAX];
        if (jw__env_or_join(clean_exit, sizeof(clean_exit),
                            "UMRK_CLEAN_EXIT_SENTINEL",
                            "TMPDIR", NULL,
                            "/tmp", "umrk-clean-exit") == 0) {
            FILE *fp = fopen(clean_exit, "w");
            if (fp) fclose(fp);
        }
    }

    jw_log_info("jawakad exiting");
    jw__cleanup(&state);
    return 0;
}
