#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/discovery/discovery.h"
#include "internal/ipc/ipc.h"
#include "internal/platform/device.h"
#include "internal/platform/input_proxy.h"
#include "internal/platform/paths.h"
#include "internal/retroarch/command.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    JW_CHILD_NONE = 0,
    JW_CHILD_LAUNCHER,
    JW_CHILD_MENU,
    JW_CHILD_RETROARCH,
    JW_CHILD_APP
} jw_child_kind;

typedef struct {
    bool active;
    pid_t pid;
    time_t started_at;
    char system[64];
    char rom_path[PATH_MAX];
    char core_path[PATH_MAX];
    char config_path[PATH_MAX];
} jw_retroarch_session;

typedef struct {
    char *runtime_dir;
    char *sdcard_root;
    char *socket_path;
    char *osd_socket_path;
    char *db_path;
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
    bool retroarch_resume_on_menu_exit;
    pid_t osd_pid;
    int cached_brightness_percent;
    int cached_volume_percent;
    jw_retroarch_session retroarch_session;
    bool pending_menu;
    bool pending_launch;
    char pending_launch_system[64];
    char pending_launch_rom_path[PATH_MAX];
    bool pending_app;
    char pending_app_pak_dir[PATH_MAX];
    bool daemon_only;
    bool shutdown_requested;
} jw_daemon_state;

static volatile sig_atomic_t g_shutdown_requested = 0;

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
    jw__json_add_int_or_null(root, "wifi_connected", status->wifi_connected);
    jw__json_add_int_or_null(root, "wifi_strength", status->wifi_strength);
    jw__json_add_int_or_null(root, "bluetooth_connected", status->bluetooth_connected);
    return root;
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
    cJSON_AddItemToObject(root, "status", jw__platform_status_json(&status));
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

static const char *jw__child_name(jw_child_kind kind) {
    switch (kind) {
        case JW_CHILD_LAUNCHER: return "jawaka-launcher";
        case JW_CHILD_MENU: return "jawaka-menu";
        case JW_CHILD_RETROARCH: return "RetroArch";
        case JW_CHILD_APP: return "app";
        default: return NULL;
    }
}

static int jw__spawn_child(jw_daemon_state *state, jw_child_kind kind);
static int jw__spawn_in_game_menu(jw_daemon_state *state, bool show_now);
static int jw__spawn_osd(jw_daemon_state *state);
static int jw__spawn_retroarch(jw_daemon_state *state);
static int jw__spawn_app(jw_daemon_state *state);
static int jw__request_open_in_game_menu(jw_daemon_state *state);
static int jw__request_close_in_game_menu(jw_daemon_state *state);

static void jw__publish_retroarch_input_env(jw_daemon_state *state) {
    if (!state || !state->input_proxy.enabled ||
        !state->input_proxy.virtual_event_path[0]) {
        unsetenv("CAT_INPUT_WAKE_EVENT");
        unsetenv("JAWAKA_RETROARCH_VIRTUAL_EVENT");
        unsetenv("JAWAKA_RETROARCH_INPUT_DEVICE");
        unsetenv("JAWAKA_RETROARCH_JOYPAD_INDEX");
        return;
    }

    setenv("CAT_INPUT_WAKE_EVENT", state->input_proxy.virtual_event_path, 1);
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
                                        const char *core_path,
                                        const char *config_path) {
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
    snprintf(session->core_path, sizeof(session->core_path), "%s", core_path ? core_path : "");
    snprintf(session->config_path, sizeof(session->config_path), "%s",
             config_path ? config_path : "");

    /* Fresh session: no menu shown yet, reset the standby respawn guard. */
    state->menu_visible = false;
    state->menu_standby_attempts = 0;

    jw_log_info("RetroArch session started pid=%d system=%s core=%s config=%s rom=%s",
                (int)pid, session->system, session->core_path,
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

static void jw__retroarch_session_finish(jw_daemon_state *state, pid_t pid, int status) {
    if (!state) {
        return;
    }

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

    if (session->config_path[0]) {
        char error[256];
        if (jw_backup_retroarch_config(session->config_path, state->sdcard_root,
                                       error, sizeof(error)) != 0) {
            jw_log_warn("RetroArch shared config backup failed: %s",
                        error[0] ? error : session->config_path);
        } else {
            jw_log_info("RetroArch shared config backed up from %s", session->config_path);
        }
    }

    jw__retroarch_session_clear(session);
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

static int jw__request_open_in_game_menu(jw_daemon_state *state) {
    long long start_ms = jw__monotonic_ms();
    if (!state || !state->retroarch_session.active) {
        return -1;
    }

    /* Already showing: ignore the tap (the menu is closed with B/Continue). */
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
    jw_log_info("in-game menu open timings: pause_ms=%lld show_ms=%lld total_ms=%lld standby=%d",
                pause_done_ms - pause_start_ms,
                done_ms - show_start_ms,
                done_ms - start_ms,
                warm);

    return 0;
}

/* Close the visible standby menu (the Menu button toggles it shut): resume the
   game and tell the menu to hide back to standby. Mirror image of the open
   path; uses explicit UNPAUSE so the game always resumes. */
static int jw__request_close_in_game_menu(jw_daemon_state *state) {
    if (!state || !state->menu_visible) {
        return -1;
    }

    jw_ra_client client = jw_ra_client_default();
    jw_ra_resume_direct(&client);
    state->menu_visible = false;
    if (state->menu_pid > 0) {
        kill(state->menu_pid, SIGUSR2);
    }
    jw_log_info("in-game menu closed via Menu toggle");
    return 0;
}

static int jw__resolve_rom_path(jw_daemon_state *state, const char *rom_path,
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

    char sdcard_abs[PATH_MAX];
    char apps_abs[PATH_MAX];
    if (!realpath(state->sdcard_root, sdcard_abs)) {
        if (out_error) *out_error = "SD-card root missing";
        return -1;
    }

    char apps_candidate[PATH_MAX];
    if (snprintf(apps_candidate, sizeof(apps_candidate), "%s/Apps", sdcard_abs) >= (int)sizeof(apps_candidate) ||
        !realpath(apps_candidate, apps_abs)) {
        if (out_error) *out_error = "Apps directory missing";
        return -1;
    }

    char candidate[PATH_MAX];
    if (pak_dir[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", pak_dir);
    } else if (snprintf(candidate, sizeof(candidate), "%s/%s", sdcard_abs, pak_dir) >= (int)sizeof(candidate)) {
        if (out_error) *out_error = "app path too long";
        return -1;
    }

    char resolved_pak[PATH_MAX];
    if (!realpath(candidate, resolved_pak)) {
        if (out_error) *out_error = "app pak missing";
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

static int jw__validate_launch_request(jw_daemon_state *state, const char *system,
                                       const char *rom_path, const char **out_error) {
    if (!state || !system || !system[0] || !rom_path || !rom_path[0]) {
        if (out_error) *out_error = "missing launch payload";
        return -1;
    }

    char *retroarch = jw_retroarch_bin_path();
    if (!retroarch || !jw__path_exists(retroarch)) {
        free(retroarch);
        if (out_error) *out_error = "RetroArch binary missing";
        return -1;
    }
    free(retroarch);

    char *core = jw_retroarch_core_path_for_system(system);
    if (!core) {
        if (out_error) *out_error = "unsupported system";
        return -1;
    }
    if (!jw__path_exists(core)) {
        free(core);
        if (out_error) *out_error = "libretro core missing";
        return -1;
    }
    free(core);

    char rom_abs[PATH_MAX];
    if (jw__resolve_rom_path(state, rom_path, rom_abs, sizeof(rom_abs)) != 0 ||
        !jw__path_exists(rom_abs)) {
        if (out_error) *out_error = "ROM path missing";
        return -1;
    }

    return 0;
}

static int jw__request_launch_game(jw_daemon_state *state, const char *system,
                                   const char *rom_path, const char **out_error) {
    if (jw__validate_launch_request(state, system, rom_path, out_error) != 0) {
        return -1;
    }

    snprintf(state->pending_launch_system, sizeof(state->pending_launch_system), "%s", system);
    snprintf(state->pending_launch_rom_path, sizeof(state->pending_launch_rom_path), "%s", rom_path);
    state->pending_launch = true;

    if (state->child_pid <= 0) {
        if (jw__spawn_retroarch(state) != 0) {
            if (out_error) *out_error = "RetroArch spawn failed";
            return -1;
        }
    }

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

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_warn("osd fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
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
        jw__osd_show_volume(state, resolved);
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
    if (jw__set_brightness(state, current + delta_percent, true, true, &result) == 0) {
        jw_log_info("brightness hotkey delta=%d value=%d",
                    delta_percent, result.has_value ? result.value : current + delta_percent);
    } else {
        jw_log_warn("brightness hotkey failed: %s", result.message);
    }
}

static bool jw__input_menu_tap(void *userdata) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state || !state->retroarch_session.active) {
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

static void jw__start_input_proxy(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    if (state->input_proxy.enabled) {
        jw__publish_retroarch_input_env(state);
        return;
    }

    if (jw_input_proxy_init(&state->input_proxy, jw__input_brightness_delta,
                            jw__input_volume_delta, jw__input_menu_tap, state) == 0) {
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
    if (!state || !name || kind == JW_CHILD_RETROARCH || kind == JW_CHILD_APP) {
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

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
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

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("in-game menu fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        if (state->retroarch_session.active) {
            setenv("JAWAKA_INGAME_ACTIVE", "1", 1);
            setenv("JAWAKA_INGAME_SYSTEM", state->retroarch_session.system, 1);
            setenv("JAWAKA_INGAME_ROM", state->retroarch_session.rom_path, 1);
            setenv("JAWAKA_INGAME_CORE", state->retroarch_session.core_path, 1);
        }
        setenv("JAWAKA_INGAME_AUTOSHOW", show_now ? "1" : "0", 1);
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

    jw__suspend_input_proxy_for_app(state);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        jw__start_input_proxy(state);
        state->pending_app = false;
        return -1;
    }

    if (pid == 0) {
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

static int jw__spawn_retroarch(jw_daemon_state *state) {
    if (!state || !state->pending_launch) {
        return -1;
    }

    char rom_abs[PATH_MAX];
    if (jw__resolve_rom_path(state, state->pending_launch_rom_path, rom_abs, sizeof(rom_abs)) != 0) {
        jw_log_error("could not resolve ROM path: %s", state->pending_launch_rom_path);
        state->pending_launch = false;
        return -1;
    }

    char *retroarch = jw_retroarch_bin_path();
    char *core = jw_retroarch_core_path_for_system(state->pending_launch_system);
    char *runtime_config = NULL;

    jw__publish_retroarch_input_env(state);

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

    char config_error[256];
    runtime_config = jw_prepare_retroarch_config(state->runtime_dir,
                                                state->sdcard_root,
                                                core,
                                                player1_joypad_index,
                                                true,
                                                config_error,
                                                sizeof(config_error));
    if (!runtime_config) {
        jw_log_error("could not prepare RetroArch config: %s",
                     config_error[0] ? config_error : "unknown error");
        goto fail;
    }

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        goto fail;
    }

    if (pid == 0) {
        char *const argv[] = {
            retroarch,
            "-L", core,
            "--config", runtime_config,
            rom_abs,
            NULL
        };
        execv(retroarch, argv);
        perror("execv");
        _exit(127);
    }

    state->child_pid = pid;
    state->child_kind = JW_CHILD_RETROARCH;
    state->pending_launch = false;
    jw_log_info("spawned RetroArch pid=%d retroarch=%s", (int)pid, retroarch);
    jw__retroarch_session_start(state, pid, state->pending_launch_system, rom_abs, core, runtime_config);

    /* Pre-spawn the in-game menu as a hidden warm standby so the first Menu
       tap only has to pause + reveal an already-built window, not cold-start a
       whole SDL/GL process. If this fails we fall back to on-demand spawn. */
    if (jw__spawn_in_game_menu(state, false) != 0) {
        jw_log_warn("could not pre-spawn standby in-game menu; will spawn on demand");
    }

    free(retroarch);
    free(core);
    free(runtime_config);
    return 0;

fail:
    state->pending_launch = false;
    jw__retroarch_session_clear(&state->retroarch_session);
    free(retroarch);
    free(core);
    free(runtime_config);
    return -1;
}

static int jw__handle_scan(jw_daemon_state *state, jw_ipc_client *client) {
    jw_scan_result scan_result;
    if (jw_scan_library(state->db, state->sdcard_root, &scan_result) != 0) {
        jw_log_error("scan-library failed");
        return jw__reply_error(client, "scan-library failed");
    }

    jw_log_info("scan-library requested");
    jw_log_info("scan-library indexed %d games across %d systems and %d apps",
        scan_result.game_count, scan_result.system_count, scan_result.app_count);
    return jw__reply_ok(client, "scan-library", &scan_result);
}

static int jw__reply_retroarch_session(jw_daemon_state *state, jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "retroarch-session");
    cJSON_AddBoolToObject(root, "active",
                          state && state->retroarch_session.active);

    if (!state || !state->retroarch_session.active) {
        cJSON_AddBoolToObject(root, "command_ok", false);
        cJSON_AddStringToObject(root, "command_result", "inactive");
        return jw__reply_json(client, root);
    }

    cJSON_AddStringToObject(root, "system", state->retroarch_session.system);
    cJSON_AddStringToObject(root, "rom_path", state->retroarch_session.rom_path);
    cJSON_AddStringToObject(root, "core_path", state->retroarch_session.core_path);

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
    if (!state || !state->retroarch_session.active) {
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
        result = jw_ra_resume_direct(&ra);
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
    } else if (strcmp(action, "reset") == 0) {
        result = jw_ra_reset(&ra);
        if (result == JW_RA_OK) {
            /* Reset/Save/Load close the menu (the UI sets running=false), so
               resume the game now. The resident menu never exits, so the old
               resume-on-menu-exit path can't do it for us. */
            jw_ra_resume_direct(&ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "save-state") == 0) {
        if (has_value && value >= 0) {
            char reply[JW_RA_REPLY_MAX];
            result = jw_ra_save_state_slot(&ra, value, reply, sizeof(reply));
        } else {
            result = jw_ra_save_state(&ra);
        }
        if (result == JW_RA_OK) {
            jw_ra_resume_direct(&ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "load-state") == 0) {
        if (has_value && value >= 0) {
            char reply[JW_RA_REPLY_MAX];
            result = jw_ra_load_state_slot(&ra, value, reply, sizeof(reply));
        } else {
            result = jw_ra_load_state(&ra);
        }
        if (result == JW_RA_OK) {
            jw_ra_resume_direct(&ra);
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

    if (strcmp(type->valuestring, "scan-library") == 0) {
        cJSON_Delete(root);
        return jw__handle_scan(state, client);
    }

    if (strcmp(type->valuestring, "open-menu") == 0) {
        bool in_game = state && state->retroarch_session.active;
        if ((in_game ? jw__request_open_in_game_menu(state)
                     : jw__request_open_menu(state)) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, "open-menu failed");
        }
        jw_log_info("open-menu requested mode=%s", in_game ? "in-game" : "frontend");
        cJSON_Delete(root);
        return jw__reply_ok(client, "open-menu", NULL);
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
        const char *error_message = NULL;
        if (!cJSON_IsString(system) || !system->valuestring ||
            !cJSON_IsString(rom_path) || !rom_path->valuestring ||
            jw__request_launch_game(state, system->valuestring, rom_path->valuestring, &error_message) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, error_message ? error_message : "launch-game failed");
        }

        jw_log_info("launch-game requested system=%s rom=%s",
                    system->valuestring, rom_path->valuestring);
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
            }
        } else {
            jw_platform_perform_action(&state->platform, action, value, &result);
        }
        jw_log_info("platform-action requested action=%s code=%s",
                    jw_platform_action_name(action),
                    jw_platform_result_code_name(result.code));
        cJSON_Delete(root);
        return jw__reply_platform_result(client, jw_platform_action_name(action), &result);
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
        cJSON_Delete(root);
        return jw__reply_platform_result(client, "frontend-ready", &result);
    }

    /* EXIT-TO-STOCK: temporary dev/test feature. Writes a sentinel so the
       wrapper falls back to stock for this session only. The sentinel lives
       in /tmp and is cleared on reboot. See loong_pangu.wrapper sentinel
       check and jawaka-menu EXIT_STOCK case. May be removed after testing. */
    if (strcmp(type->valuestring, "shutdown") == 0) {
        state->shutdown_requested = true;
        /* Path must match CRASH_STATE in loong_pangu.wrapper. */
        static const char *crash_state = "/userdata/umrk-launcher-crash-state";
        unlink(crash_state);
        /* Sentinel path must match loong_pangu.wrapper sentinel check. */
        static const char *exit_sentinel = "/tmp/umrk-exit-to-stock";
        FILE *fp = fopen(exit_sentinel, "w");
        if (fp) fclose(fp);
        jw_log_info("shutdown requested — exiting to stock (this session only)");
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

    if (state->shutdown_requested || g_shutdown_requested) {
        return;
    }

    if (exited_kind == JW_CHILD_APP) {
        jw__start_input_proxy(state);
    }

    /* Spawn-on-exit model: the launcher sends a pending action, then exits
     * voluntarily. The daemon detects the exit here and owns the next process. */
    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_launch) {
        if (jw__spawn_retroarch(state) != 0 && !state->daemon_only) {
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

    if (exited_kind == JW_CHILD_RETROARCH) {
        jw__spawn_child(state, JW_CHILD_LAUNCHER);
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

    jw_input_proxy_shutdown(&state->input_proxy);
    jw_platform_shutdown(&state->platform);
    jw_ipc_server_close(state->server);
    jw_db_close(state->db);
    free(state->runtime_dir);
    free(state->sdcard_root);
    free(state->socket_path);
    free(state->osd_socket_path);
    free(state->db_path);
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
    if (!state.runtime_dir || !state.sdcard_root || !state.socket_path ||
        !state.osd_socket_path || !state.db_path) {
        jw_log_error("could not resolve runtime paths");
        jw__cleanup(&state);
        return 1;
    }

    if (jw_platform_init(&state.platform, state.runtime_dir, state.sdcard_root) != 0) {
        jw_log_error("could not initialize platform service");
        jw__cleanup(&state);
        return 1;
    }

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
    jw__apply_persisted_brightness(&state);
    jw__apply_persisted_volume(&state);

    if (jw_ipc_server_listen(state.socket_path, &state.server) != 0) {
        jw_log_error("could not bind socket: %s", state.socket_path);
        jw__cleanup(&state);
        return 1;
    }

    /* Exported so child processes receive them via execv's inherited environment */
    setenv("JAWAKA_RUNTIME_DIR", state.runtime_dir, 1);
    setenv("JAWAKA_SDCARD_ROOT", state.sdcard_root, 1);
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

        jw__handle_child_exit(&state);
        jw__handle_menu_exit(&state);
        jw__handle_osd_exit(&state);
        jw_input_proxy_tick(&state.input_proxy);

        if (state.shutdown_requested && state.child_pid <= 0 &&
            state.menu_pid <= 0) {
            break;
        }

        if (state.shutdown_requested && state.child_pid > 0) {
            kill(state.child_pid, SIGTERM);
        }
        if (state.shutdown_requested && state.menu_pid > 0) {
            kill(state.menu_pid, SIGTERM);
        }

        int rc = jw__accept_and_process(&state);
        if (rc < 0 && !state.shutdown_requested && !g_shutdown_requested) {
            jw_log_warn("ipc loop iteration failed");
        }
    }

    /* Write clean-exit marker so the wrapper's crash-loop guard knows this
       was an intentional shutdown, not a crash. The marker lives in /tmp so
       it clears on reboot. See loong_pangu.wrapper check_crash_loop(). */
    { FILE *fp = fopen("/tmp/umrk-clean-exit", "w"); if (fp) fclose(fp); }

    jw_log_info("jawakad exiting");
    jw__cleanup(&state);
    return 0;
}
