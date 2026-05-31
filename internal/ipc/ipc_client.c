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

int jw_ipc_open_menu(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "open-menu");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

int jw_ipc_launch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len) {
    if (!system || !system[0] || !rom_path || !rom_path[0]) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: missing game");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "launch-game");
    cJSON_AddStringToObject(req, "system", system);
    cJSON_AddStringToObject(req, "rom_path", rom_path);

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
