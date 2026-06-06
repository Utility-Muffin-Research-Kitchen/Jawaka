#include "internal/ipc/ipc_client.h"
#include "internal/ipc/ipc.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Send request (takes ownership of req), parse the JSON response.
 * Caller must cJSON_Delete(*out_response) on success. */
static int ipc__request(const char *socket_path, cJSON *req, cJSON **out_resp) {
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    char *resp_json = NULL;
    size_t resp_len = 0;
    int rc = jw_ipc_request(socket_path, body, strlen(body), &resp_json, &resp_len);
    cJSON_free(body);
    if (rc != 0) return -1;

    cJSON *resp = cJSON_Parse(resp_json);
    free(resp_json);
    if (!resp) return -1;

    *out_resp = resp;
    return 0;
}

static int ipc__type_is(const cJSON *resp, const char *expected) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(resp, "type");
    return cJSON_IsString(t) && t->valuestring &&
           strcmp(t->valuestring, expected) == 0;
}

static void ipc__copy_string(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", value ? value : "");
}

int jw_ipc_hello(const char *socket_path, const char *role) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "hello");
    cJSON_AddStringToObject(req, "role", role);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) return -1;
    int ok = ipc__type_is(resp, "hello-ok");
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_scan_library(const char *socket_path, char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scan-library");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "scan failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        if (status) snprintf(status, (size_t)status_len, "%s", "scan failed: daemon returned error");
        cJSON_Delete(resp);
        return -1;
    }

    if (status) {
        const cJSON *gc = cJSON_GetObjectItemCaseSensitive(resp, "game_count");
        const cJSON *ac = cJSON_GetObjectItemCaseSensitive(resp, "app_count");
        if (cJSON_IsNumber(gc) && cJSON_IsNumber(ac))
            snprintf(status, (size_t)status_len,
                     "scan complete: %d games, %d apps", gc->valueint, ac->valueint);
        else
            snprintf(status, (size_t)status_len, "%s", "scan complete");
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_library_status(const char *socket_path, int *out_generation) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "library-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    if (!ipc__type_is(resp, "library-status")) {
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(resp, "generation");
    if (out_generation) {
        *out_generation = cJSON_IsNumber(generation) ? generation->valueint : 0;
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_get_storage_status(const char *socket_path, const char *source,
                              jw_ipc_storage_status_info *out,
                              char *status, int status_len) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "storage-status");
    cJSON_AddStringToObject(req, "source", source && source[0] ? source : "secondary_sd");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "storage status unavailable");
        return -1;
    }
    if (!ipc__type_is(resp, "storage-status")) {
        if (status) snprintf(status, (size_t)status_len, "%s", "storage status failed");
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        const cJSON *v = NULL;
        v = cJSON_GetObjectItemCaseSensitive(resp, "source");
        if (cJSON_IsString(v)) ipc__copy_string(out->source, sizeof(out->source), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "label");
        if (cJSON_IsString(v)) ipc__copy_string(out->label, sizeof(out->label), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "mount_path");
        if (cJSON_IsString(v)) ipc__copy_string(out->mount_path, sizeof(out->mount_path), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(v)) ipc__copy_string(out->message, sizeof(out->message), v->valuestring);
        out->present = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "present"));
        out->mounted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "mounted"));
        out->busy = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "busy"));
        out->can_unmount = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "can_unmount"));
    }
    if (status) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "storage status ready");
        }
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_safe_unmount_storage(const char *socket_path, const char *source,
                                char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "storage-action");
    cJSON_AddStringToObject(req, "source", source && source[0] ? source : "secondary_sd");
    cJSON_AddStringToObject(req, "action", "safe-unmount");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "unmount failed: daemon unavailable");
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "Secondary SD unmounted" : "unmount failed");
        }
    }
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_open_menu(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "open-menu");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

static int ipc__launch_game(const char *socket_path, const char *system,
                            const char *rom_path, const char *resume_policy,
                            char *status, int status_len) {
    if (!system || !system[0] || !rom_path || !rom_path[0]) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: missing game");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "launch-game");
    cJSON_AddStringToObject(req, "system", system);
    cJSON_AddStringToObject(req, "rom_path", rom_path);
    if (resume_policy && resume_policy[0]) {
        cJSON_AddStringToObject(req, "resume_policy", resume_policy);
    }

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "launch failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "launch failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "launch requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_launch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len) {
    return ipc__launch_game(socket_path, system, rom_path, NULL, status, status_len);
}

int jw_ipc_launch_game_switcher(const char *socket_path, const char *system,
                                const char *rom_path, char *status,
                                int status_len) {
    return ipc__launch_game(socket_path, system, rom_path, "switcher-latest",
                            status, status_len);
}

int jw_ipc_open_switcher(const char *socket_path, char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "open-switcher");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s",
                             "open-switcher failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "open-switcher failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "open-switcher failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "switcher requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_switch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len) {
    if (!system || !system[0] || !rom_path || !rom_path[0]) {
        if (status) snprintf(status, (size_t)status_len, "%s",
                             "switch failed: missing game");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "switch-game");
    cJSON_AddStringToObject(req, "system", system);
    cJSON_AddStringToObject(req, "rom_path", rom_path);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s",
                             "switch failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "switch failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "switch failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "switch requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_launch_app(const char *socket_path, const char *pak_dir,
                      char *status, int status_len) {
    if (!pak_dir || !pak_dir[0]) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: missing app");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "launch-app");
    cJSON_AddStringToObject(req, "pak_dir", pak_dir);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "launch failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "launch failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "app launch requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_get_retroarch_session(const char *socket_path,
                                 jw_ipc_retroarch_session_info *out,
                                 char *status, int status_len) {
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->disk_count = 0;
    out->disk_slot = -1;
    out->state_slot = -1;
    ipc__copy_string(out->command_result, sizeof(out->command_result), "unavailable");

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "retroarch-session");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch session unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "retroarch-session")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "RetroArch session failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "RetroArch session failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *active = cJSON_GetObjectItemCaseSensitive(resp, "active");
    const cJSON *command_ok = cJSON_GetObjectItemCaseSensitive(resp, "command_ok");
    const cJSON *command_result = cJSON_GetObjectItemCaseSensitive(resp, "command_result");
    const cJSON *system = cJSON_GetObjectItemCaseSensitive(resp, "system");
    const cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(resp, "rom_path");
    const cJSON *core_path = cJSON_GetObjectItemCaseSensitive(resp, "core_path");
    const cJSON *disk_count = cJSON_GetObjectItemCaseSensitive(resp, "disk_count");
    const cJSON *disk_slot = cJSON_GetObjectItemCaseSensitive(resp, "disk_slot");
    const cJSON *savestate_supported =
        cJSON_GetObjectItemCaseSensitive(resp, "savestate_supported");
    const cJSON *state_slot = cJSON_GetObjectItemCaseSensitive(resp, "state_slot");

    out->active = cJSON_IsTrue(active);
    out->command_ok = cJSON_IsTrue(command_ok);
    if (cJSON_IsString(command_result) && command_result->valuestring) {
        ipc__copy_string(out->command_result, sizeof(out->command_result),
                         command_result->valuestring);
    }
    if (cJSON_IsString(system) && system->valuestring) {
        ipc__copy_string(out->system, sizeof(out->system), system->valuestring);
    }
    if (cJSON_IsString(rom_path) && rom_path->valuestring) {
        ipc__copy_string(out->rom_path, sizeof(out->rom_path), rom_path->valuestring);
    }
    if (cJSON_IsString(core_path) && core_path->valuestring) {
        ipc__copy_string(out->core_path, sizeof(out->core_path), core_path->valuestring);
    }
    if (cJSON_IsNumber(disk_count)) {
        out->disk_count = disk_count->valueint;
    }
    if (cJSON_IsNumber(disk_slot)) {
        out->disk_slot = disk_slot->valueint;
    }
    out->savestate_supported = cJSON_IsTrue(savestate_supported);
    if (cJSON_IsNumber(state_slot)) {
        out->state_slot = state_slot->valueint;
    }

    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "%s",
                 out->active ? "RetroArch session active" : "No active RetroArch session");
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_retroarch_action(const char *socket_path, const char *action,
                            int value, char *status, int status_len) {
    if (!action || !action[0]) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch action missing");
        }
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "retroarch-action");
    cJSON_AddStringToObject(req, "action", action);
    cJSON_AddNumberToObject(req, "value", value);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch action failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "RetroArch action failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "RetroArch action failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status && status_len > 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "RetroArch action requested");
        }
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_reset_retroarch_config(const char *socket_path,
                                  char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "reset-retroarch-config");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch reset failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "RetroArch reset failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "RetroArch reset failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "%s", "RetroArch config reset");
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_shutdown(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "shutdown");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

int jw_ipc_exit_stock(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "exit-stock");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

int jw_ipc_platform_action(const char *socket_path, const char *action, int value) {
    if (!action || !action[0]) {
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", action);
    cJSON_AddNumberToObject(req, "value", value);
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

int jw_ipc_frontend_ready(const char *socket_path, const char *role) {
    if (!role || !role[0]) {
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "frontend-ready");
    cJSON_AddStringToObject(req, "role", role);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int ok = ipc__type_is(resp, "ok");
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_platform_brightness(const char *socket_path, int *out_percent) {
    if (out_percent) {
        *out_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(status, "brightness_percent");
    if (cJSON_IsNumber(brightness)) {
        if (out_percent) {
            *out_percent = brightness->valueint;
        }
        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_brightness(const char *socket_path, int percent,
                          int *out_percent, char *status, int status_len) {
    if (out_percent) {
        *out_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-brightness");
    cJSON_AddNumberToObject(req, "value", percent);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "brightness failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "brightness failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "brightness failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(resp, "value");
    int resolved = cJSON_IsNumber(value) ? value->valueint : percent;
    if (out_percent) {
        *out_percent = resolved;
    }
    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "brightness: %d%%", resolved);
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_platform_volume(const char *socket_path, int *out_percent) {
    if (out_percent) {
        *out_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(status, "volume_percent");
    if (cJSON_IsNumber(volume)) {
        if (out_percent) {
            *out_percent = volume->valueint;
        }
        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_volume(const char *socket_path, int percent,
                      int *out_percent, char *status, int status_len) {
    if (out_percent) {
        *out_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-volume");
    cJSON_AddNumberToObject(req, "value", percent);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "volume failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "volume failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "volume failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(resp, "value");
    int resolved = cJSON_IsNumber(value) ? value->valueint : percent;
    if (out_percent) {
        *out_percent = resolved;
    }
    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "volume: %d%%", resolved);
    }

    cJSON_Delete(resp);
    return 0;
}

static void ipc__audio_status_init(jw_ipc_audio_status *out_status) {
    if (!out_status) {
        return;
    }
    out_status->output = JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN;
    out_status->available_outputs = 0;
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        out_status->volume_percent[i] = -1;
    }
}

int jw_ipc_platform_audio_status(const char *socket_path,
                                 jw_ipc_audio_status *out_status) {
    ipc__audio_status_init(out_status);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-audio-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    if (cJSON_IsObject(status)) {
        const cJSON *output = cJSON_GetObjectItemCaseSensitive(status, "audio_output");
        jw_platform_audio_output parsed = JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN;
        if (cJSON_IsString(output) && output->valuestring &&
            jw_platform_parse_audio_output(output->valuestring, &parsed)) {
            out_status->output = parsed;
        }

        const cJSON *available = cJSON_GetObjectItemCaseSensitive(status,
                                                                  "audio_available_outputs");
        if (cJSON_IsArray(available)) {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, available) {
                if (!cJSON_IsString(item) || !item->valuestring) {
                    continue;
                }
                if (jw_platform_parse_audio_output(item->valuestring, &parsed) &&
                    parsed >= 0 && parsed < JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
                    out_status->available_outputs |= JW_PLATFORM_AUDIO_OUTPUT_BIT(parsed);
                }
            }
        }

        const cJSON *volumes = cJSON_GetObjectItemCaseSensitive(status, "audio_volumes");
        if (cJSON_IsObject(volumes)) {
            for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
                const char *name = jw_platform_audio_output_name((jw_platform_audio_output)i);
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(volumes, name);
                if (cJSON_IsNumber(v)) {
                    int percent = v->valueint;
                    if (percent < 0) percent = 0;
                    if (percent > 100) percent = 100;
                    out_status->volume_percent[i] = percent;
                }
            }
        }

        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_audio_output(const char *socket_path,
                            jw_platform_audio_output output,
                            char *status, int status_len) {
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "audio output invalid");
        }
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-audio-output");
    cJSON_AddStringToObject(req, "output", jw_platform_audio_output_name(output));
    cJSON_AddNumberToObject(req, "value", (int)output);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "audio output failed: daemon unavailable");
        }
        return -1;
    }

    int ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "audio output set" : "audio output failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_get_adb(const char *socket_path, int *out_enabled,
                   int *out_intent_enabled) {
    if (out_enabled) {
        *out_enabled = -1;
    }
    if (out_intent_enabled) {
        *out_intent_enabled = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(status, "adb_enabled");
    const cJSON *intent = cJSON_GetObjectItemCaseSensitive(status, "adb_intent_enabled");
    if (cJSON_IsNumber(enabled) || cJSON_IsNumber(intent)) {
        if (out_enabled) {
            *out_enabled = cJSON_IsNumber(enabled) ? enabled->valueint : -1;
        }
        if (out_intent_enabled) {
            *out_intent_enabled = cJSON_IsNumber(intent) ? intent->valueint : -1;
        }
        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_adb(const char *socket_path, int enabled,
                   char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", enabled ? "enable-adb" : "disable-adb");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "ADB failed: daemon unavailable");
        }
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? (enabled ? "ADB enabled" : "ADB disabled") : "ADB failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_set_led(const char *socket_path, int enabled, const char *mode,
                   int r, int g, int b, int brightness, int speed,
                   char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "set-led");
    cJSON_AddBoolToObject(req, "enabled", enabled ? 1 : 0);
    cJSON_AddStringToObject(req, "mode", mode ? mode : "FOREVER");
    cJSON_AddNumberToObject(req, "r", r);
    cJSON_AddNumberToObject(req, "g", g);
    cJSON_AddNumberToObject(req, "b", b);
    cJSON_AddNumberToObject(req, "brightness", brightness);
    cJSON_AddNumberToObject(req, "speed", speed);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0)
            snprintf(status, (size_t)status_len, "%s", "led failed: daemon unavailable");
        return -1;
    }
    int rc = ipc__type_is(resp, "ok") ? 0 : -1;
    if (status && status_len > 0)
        snprintf(status, (size_t)status_len, "%s", rc == 0 ? "led applied" : "led failed");
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_get_led(const char *socket_path, int *enabled, char *mode, int mode_len,
                   int *r, int *g, int *b, int *brightness, int *speed) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) return -1;

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *led = cJSON_GetObjectItemCaseSensitive(status, "led");
    int rc = -1;
    if (cJSON_IsObject(led)) {
        const cJSON *v;
        if (enabled    && (v = cJSON_GetObjectItemCaseSensitive(led, "enabled")))    *enabled = cJSON_IsTrue(v) ? 1 : (cJSON_IsNumber(v) ? v->valueint : 0);
        if (mode && mode_len > 0 && (v = cJSON_GetObjectItemCaseSensitive(led, "mode")) && cJSON_IsString(v)) snprintf(mode, (size_t)mode_len, "%s", v->valuestring);
        if (r          && (v = cJSON_GetObjectItemCaseSensitive(led, "r")))           *r = v->valueint;
        if (g          && (v = cJSON_GetObjectItemCaseSensitive(led, "g")))           *g = v->valueint;
        if (b          && (v = cJSON_GetObjectItemCaseSensitive(led, "b")))           *b = v->valueint;
        if (brightness && (v = cJSON_GetObjectItemCaseSensitive(led, "brightness")))  *brightness = v->valueint;
        if (speed      && (v = cJSON_GetObjectItemCaseSensitive(led, "speed")))       *speed = v->valueint;
        rc = 0;
    }
    cJSON_Delete(resp);
    return rc;
}
