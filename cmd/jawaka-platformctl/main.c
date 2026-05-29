#include "internal/ipc/ipc.h"
#include "internal/platform/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage: jawaka-platformctl [--socket PATH] status\n"
            "       jawaka-platformctl [--socket PATH] brightness PERCENT\n"
            "\n"
            "Commands:\n"
            "  status              Print jawakad platform-status JSON\n"
            "  brightness PERCENT  Set platform brightness and print daemon JSON\n");
}

int main(int argc, char *argv[]) {
    const char *socket_override = NULL;
    const char *command = NULL;
    const char *brightness_value = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            socket_override = argv[++i];
            continue;
        }
        if (!command) {
            command = argv[i];
            continue;
        }
        if (strcmp(command, "brightness") == 0 && !brightness_value) {
            brightness_value = argv[i];
            continue;
        }
        usage(stderr);
        return 2;
    }

    if (command && strcmp(command, "brightness") == 0) {
        if (!brightness_value || !brightness_value[0]) {
            usage(stderr);
            return 2;
        }
    } else if (!command || strcmp(command, "status") != 0) {
        usage(stderr);
        return 2;
    }

    char *socket_path = socket_override ? NULL : jw_socket_path();
    const char *socket = socket_override ? socket_override : socket_path;
    if (!socket || !socket[0]) {
        fprintf(stderr, "could not resolve jawakad socket path\n");
        free(socket_path);
        return 1;
    }

    char request[256];
    if (strcmp(command, "brightness") == 0) {
        char *end = NULL;
        long parsed = strtol(brightness_value, &end, 10);
        if (end == brightness_value || (end && *end != '\0')) {
            fprintf(stderr, "invalid brightness percent: %s\n", brightness_value);
            free(socket_path);
            return 2;
        }
        snprintf(request, sizeof(request),
                 "{\"type\":\"platform-action\",\"action\":\"set-brightness\",\"value\":%ld}",
                 parsed);
    } else {
        snprintf(request, sizeof(request), "%s", "{\"type\":\"platform-status\"}");
    }

    char *response = NULL;
    size_t response_len = 0;
    if (jw_ipc_request(socket, request, strlen(request), &response, &response_len) != 0) {
        fprintf(stderr, "platform request failed: %s\n", socket);
        free(socket_path);
        return 1;
    }

    fwrite(response, 1, response_len, stdout);
    fputc('\n', stdout);

    free(response);
    free(socket_path);
    return 0;
}
