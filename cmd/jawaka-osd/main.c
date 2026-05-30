#include "cmd/jawaka-osd/osd_backend.h"

#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/ipc/ipc.h"
#include "internal/platform/paths.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop;

static void jw__handle_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static uint64_t jw__now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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

static int jw__reply_ok(jw_ipc_client *client, const char *action) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", action ? action : "");
    return jw__reply_json(client, root);
}

static int jw__reply_error(jw_ipc_client *client, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "message", message ? message : "osd error");
    return jw__reply_json(client, root);
}

static int jw__handle_message(jw_ipc_client *client, const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return jw__reply_error(client, "invalid json");
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return jw__reply_error(client, "missing type");
    }

    if (strcmp(type->valuestring, "show-brightness") == 0) {
        cJSON *percent = cJSON_GetObjectItemCaseSensitive(root, "percent");
        if (!cJSON_IsNumber(percent)) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing brightness percent");
        }
        jw_osd_backend_show_brightness(percent->valueint, jw__now_ms());
        cJSON_Delete(root);
        return jw__reply_ok(client, "show-brightness");
    }

    if (strcmp(type->valuestring, "show-volume") == 0) {
        cJSON *percent = cJSON_GetObjectItemCaseSensitive(root, "percent");
        if (!cJSON_IsNumber(percent)) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing volume percent");
        }
        jw_osd_backend_show_volume(percent->valueint, jw__now_ms());
        cJSON_Delete(root);
        return jw__reply_ok(client, "show-volume");
    }

    if (strcmp(type->valuestring, "shutdown") == 0) {
        g_stop = 1;
        cJSON_Delete(root);
        return jw__reply_ok(client, "shutdown");
    }

    cJSON_Delete(root);
    return jw__reply_error(client, "unknown type");
}

int main(void) {
    signal(SIGINT, jw__handle_signal);
    signal(SIGTERM, jw__handle_signal);
    signal(SIGPIPE, SIG_IGN);

    char *socket_path = jw_osd_socket_path();
    if (!socket_path) {
        jw_log_error("osd: could not resolve socket path");
        return 1;
    }

    if (jw_osd_backend_init() != 0) {
        jw_log_error("osd: backend init failed");
        free(socket_path);
        return 1;
    }

    jw_ipc_server *server = NULL;
    if (jw_ipc_server_listen(socket_path, &server) != 0) {
        jw_log_error("osd: could not bind socket: %s", socket_path);
        jw_osd_backend_shutdown();
        free(socket_path);
        return 1;
    }

    jw_log_info("osd: listening on %s", socket_path);
    while (!g_stop) {
        jw_osd_backend_tick(jw__now_ms());

        jw_ipc_client *client = NULL;
        int rc = jw_ipc_server_accept(server, &client, 50);
        if (rc == 1) {
            continue;
        }
        if (rc < 0) {
            jw_log_warn("osd: accept failed");
            continue;
        }

        char *body = NULL;
        size_t len = 0;
        if (jw_ipc_client_recv(client, &body, &len) == 0) {
            jw__handle_message(client, body);
        }
        free(body);
        jw_ipc_client_close(client);
    }

    jw_ipc_server_close(server);
    jw_osd_backend_shutdown();
    free(socket_path);
    return 0;
}
