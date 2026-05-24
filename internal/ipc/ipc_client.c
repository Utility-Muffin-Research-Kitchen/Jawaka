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

int jw_ipc_shutdown(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "shutdown");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}
