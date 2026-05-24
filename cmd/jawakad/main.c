#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/discovery/discovery.h"
#include "internal/ipc/ipc.h"
#include "internal/platform/paths.h"

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
#include <unistd.h>

typedef enum {
    JW_CHILD_NONE = 0,
    JW_CHILD_LAUNCHER,
    JW_CHILD_MENU,
    JW_CHILD_RETROARCH
} jw_child_kind;

typedef struct {
    char *runtime_dir;
    char *sdcard_root;
    char *socket_path;
    char *db_path;
    char  bin_dir[PATH_MAX];
    sqlite3 *db;
    jw_ipc_server *server;
    pid_t child_pid;          /* exactly one daemon-owned child at a time */
    jw_child_kind child_kind;
    bool pending_menu;
    bool pending_launch;
    char pending_launch_system[64];
    char pending_launch_rom_path[PATH_MAX];
    bool daemon_only;
    bool shutdown_requested;
} jw_daemon_state;

static volatile sig_atomic_t g_shutdown_requested = 0;

static void jw__handle_signal(int signo) {
    (void)signo;
    g_shutdown_requested = 1;
}

static int jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
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

static const char *jw__child_name(jw_child_kind kind) {
    switch (kind) {
        case JW_CHILD_LAUNCHER: return "jawaka-launcher";
        case JW_CHILD_MENU: return "jawaka-menu";
        case JW_CHILD_RETROARCH: return "RetroArch";
        default: return NULL;
    }
}

static int jw__spawn_child(jw_daemon_state *state, jw_child_kind kind);
static int jw__spawn_retroarch(jw_daemon_state *state);

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

static int jw__spawn_child(jw_daemon_state *state, jw_child_kind kind) {
    const char *name = jw__child_name(kind);
    if (!state || !name || kind == JW_CHILD_RETROARCH) {
        return -1;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", state->bin_dir, name);
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
    char *append_config = NULL;

    if (!retroarch || !jw__path_exists(retroarch)) {
        jw_log_error("RetroArch binary missing: %s", retroarch ? retroarch : "(null)");
        goto fail;
    }
    if (!core || !jw__path_exists(core)) {
        jw_log_error("libretro core missing for %s: %s",
                     state->pending_launch_system, core ? core : "(null)");
        goto fail;
    }

    append_config = jw_write_retroarch_append_config(state->runtime_dir, state->sdcard_root, core);
    if (!append_config) {
        jw_log_error("could not write RetroArch appendconfig");
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
            "--appendconfig", append_config,
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
    jw_log_info("spawned RetroArch pid=%d system=%s rom=%s",
                (int)pid, state->pending_launch_system, rom_abs);

    free(retroarch);
    free(core);
    free(append_config);
    return 0;

fail:
    state->pending_launch = false;
    free(retroarch);
    free(core);
    free(append_config);
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
        if (jw__request_open_menu(state) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, "open-menu failed");
        }
        jw_log_info("open-menu requested");
        cJSON_Delete(root);
        return jw__reply_ok(client, "open-menu", NULL);
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
    int rc = jw_ipc_server_accept(state->server, &client, 200);
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
    state->child_pid = -1;
    state->child_kind = JW_CHILD_NONE;

    if (state->shutdown_requested || g_shutdown_requested) {
        return;
    }

    /* Spawn-on-exit model: the launcher sends a pending action, then exits
     * voluntarily. The daemon detects the exit here and owns the next process. */
    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_launch) {
        if (jw__spawn_retroarch(state) != 0 && !state->daemon_only) {
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

    jw__spawn_child(state, JW_CHILD_LAUNCHER);
}

static void jw__cleanup(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    if (state->child_pid > 0) {
        kill(state->child_pid, SIGTERM);
        waitpid(state->child_pid, NULL, 0);
    }

    jw_ipc_server_close(state->server);
    jw_db_close(state->db);
    free(state->runtime_dir);
    free(state->sdcard_root);
    free(state->socket_path);
    free(state->db_path);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, jw__handle_signal);
    signal(SIGTERM, jw__handle_signal);
    signal(SIGPIPE, SIG_IGN);

    jw_daemon_state state;
    memset(&state, 0, sizeof(state));
    state.child_pid = -1;

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
    state.db_path = jw_db_path();
    if (!state.runtime_dir || !state.sdcard_root || !state.socket_path || !state.db_path) {
        jw_log_error("could not resolve runtime paths");
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

    if (jw_ipc_server_listen(state.socket_path, &state.server) != 0) {
        jw_log_error("could not bind socket: %s", state.socket_path);
        jw__cleanup(&state);
        return 1;
    }

    /* Exported so child processes receive them via execv's inherited environment */
    setenv("JAWAKA_RUNTIME_DIR", state.runtime_dir, 1);
    setenv("JAWAKA_SDCARD_ROOT", state.sdcard_root, 1);

    jw_log_info("jawakad starting");
    jw_log_info("runtime dir: %s", state.runtime_dir);
    jw_log_info("sdcard root: %s", state.sdcard_root);
    jw_log_info("socket path: %s", state.socket_path);
    jw_log_info("db path: %s", state.db_path);
    if (state.daemon_only) {
        jw_log_info("daemon-only mode enabled");
    }

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

        if (state.shutdown_requested && state.child_pid <= 0) {
            break;
        }

        if (state.shutdown_requested && state.child_pid > 0) {
            kill(state.child_pid, SIGTERM);
        }

        int rc = jw__accept_and_process(&state);
        if (rc < 0 && !state.shutdown_requested && !g_shutdown_requested) {
            jw_log_warn("ipc loop iteration failed");
        }
    }

    jw_log_info("jawakad exiting");
    jw__cleanup(&state);
    return 0;
}
