#include "internal/retroarch/command.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jw__usage(FILE *stream) {
    fprintf(stream,
            "Usage: jawaka-retroarchctl [options] <command> [args]\n"
            "\n"
            "Options:\n"
            "  --host HOST          RetroArch command host (default: 127.0.0.1)\n"
            "  --port PORT          RetroArch command port (default: 55355)\n"
            "  --timeout-ms MS      Reply timeout for request commands (default: 750)\n"
            "  --help               Show this help\n"
            "\n"
            "Commands:\n"
            "  status\n"
            "  pause\n"
            "  resume\n"
            "  menu-toggle\n"
            "  quit\n"
            "  save-state\n"
            "  load-state\n"
            "  load-state-slot SLOT\n"
            "  set-state-slot SLOT       currently reports unsupported unless adapter is added\n"
            "  state-slot-plus\n"
            "  state-slot-minus\n"
            "  disk-eject-toggle\n"
            "  disk-next\n"
            "  disk-prev\n"
            "  show-message TEXT\n"
            "  raw-send COMMAND...\n"
            "  raw-request COMMAND...\n");
}

static int jw__parse_unsigned(const char *text, unsigned *out) {
    char *end = NULL;
    unsigned long value;
    if (!text || !text[0] || !out) {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > 65535ul) {
        return -1;
    }
    *out = (unsigned)value;
    return 0;
}

static int jw__parse_int(const char *text, int *out) {
    char *end = NULL;
    long value;
    if (!text || !text[0] || !out) {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 0 || value > 9999) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int jw__exit_code(jw_ra_result result) {
    switch (result) {
        case JW_RA_OK: return 0;
        case JW_RA_TIMEOUT: return 2;
        case JW_RA_UNSUPPORTED: return 3;
        case JW_RA_PARSE_ERROR: return 4;
        case JW_RA_SOCKET_ERROR:
        default: return 1;
    }
}

static int jw__print_result(jw_ra_result result) {
    printf("result=%s\n", jw_ra_result_string(result));
    return jw__exit_code(result);
}

static int jw__join_args(int argc, char **argv, int start, char *out, size_t out_size) {
    size_t used = 0;
    int i;

    if (!out || out_size == 0 || start >= argc) {
        return -1;
    }

    out[0] = '\0';
    for (i = start; i < argc; i++) {
        int written;
        if (i > start) {
            if (used + 1u >= out_size) {
                return -1;
            }
            out[used++] = ' ';
            out[used] = '\0';
        }
        written = snprintf(out + used, out_size - used, "%s", argv[i]);
        if (written < 0 || (size_t)written >= out_size - used) {
            return -1;
        }
        used += (size_t)written;
    }

    return 0;
}

int main(int argc, char **argv) {
    jw_ra_client client = jw_ra_client_default();
    int argi = 1;
    const char *command;
    jw_ra_result result = JW_RA_PARSE_ERROR;

    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--help") == 0) {
            jw__usage(stdout);
            return 0;
        } else if (strcmp(argv[argi], "--host") == 0) {
            if (++argi >= argc) {
                fprintf(stderr, "--host requires a value\n");
                return 4;
            }
            client.host = argv[argi++];
        } else if (strcmp(argv[argi], "--port") == 0) {
            if (++argi >= argc || jw__parse_unsigned(argv[argi], &client.port) != 0) {
                fprintf(stderr, "--port requires a numeric value\n");
                return 4;
            }
            argi++;
        } else if (strcmp(argv[argi], "--timeout-ms") == 0) {
            if (++argi >= argc || jw__parse_unsigned(argv[argi], &client.timeout_ms) != 0) {
                fprintf(stderr, "--timeout-ms requires a numeric value\n");
                return 4;
            }
            argi++;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[argi]);
            jw__usage(stderr);
            return 4;
        }
    }

    if (argi >= argc) {
        jw__usage(stderr);
        return 4;
    }

    command = argv[argi++];

    if (strcmp(command, "status") == 0) {
        jw_ra_status status;
        result = jw_ra_get_status(&client, &status);
        if (result == JW_RA_OK) {
            printf("result=ok\n");
            printf("state=%s\n", jw_ra_play_state_string(status.state));
            if (status.system[0]) {
                printf("system=%s\n", status.system);
            }
            if (status.content[0]) {
                printf("content=%s\n", status.content);
            }
            printf("raw=%s\n", status.raw);
            return 0;
        }
        return jw__print_result(result);
    } else if (strcmp(command, "pause") == 0) {
        result = jw_ra_pause(&client);
    } else if (strcmp(command, "resume") == 0) {
        result = jw_ra_resume(&client);
    } else if (strcmp(command, "menu-toggle") == 0) {
        result = jw_ra_menu_toggle(&client);
    } else if (strcmp(command, "quit") == 0) {
        result = jw_ra_quit(&client);
    } else if (strcmp(command, "save-state") == 0) {
        result = jw_ra_save_state(&client);
    } else if (strcmp(command, "load-state") == 0) {
        result = jw_ra_load_state(&client);
    } else if (strcmp(command, "load-state-slot") == 0) {
        int slot;
        char reply[JW_RA_REPLY_MAX];
        if (argi >= argc || jw__parse_int(argv[argi], &slot) != 0) {
            fprintf(stderr, "load-state-slot requires a non-negative slot\n");
            return 4;
        }
        result = jw_ra_load_state_slot(&client, slot, reply, sizeof(reply));
        if (result == JW_RA_OK) {
            printf("result=ok\n");
            printf("reply=%s\n", reply);
            return 0;
        }
        return jw__print_result(result);
    } else if (strcmp(command, "set-state-slot") == 0) {
        int slot;
        if (argi >= argc || jw__parse_int(argv[argi], &slot) != 0) {
            fprintf(stderr, "set-state-slot requires a non-negative slot\n");
            return 4;
        }
        result = jw_ra_set_state_slot(&client, slot);
    } else if (strcmp(command, "state-slot-plus") == 0) {
        result = jw_ra_state_slot_plus(&client);
    } else if (strcmp(command, "state-slot-minus") == 0) {
        result = jw_ra_state_slot_minus(&client);
    } else if (strcmp(command, "disk-eject-toggle") == 0) {
        result = jw_ra_disk_eject_toggle(&client);
    } else if (strcmp(command, "disk-next") == 0) {
        result = jw_ra_disk_next(&client);
    } else if (strcmp(command, "disk-prev") == 0) {
        result = jw_ra_disk_prev(&client);
    } else if (strcmp(command, "show-message") == 0) {
        char message[JW_RA_REPLY_MAX];
        if (jw__join_args(argc, argv, argi, message, sizeof(message)) != 0) {
            fprintf(stderr, "show-message requires text\n");
            return 4;
        }
        result = jw_ra_show_message(&client, message);
    } else if (strcmp(command, "raw-send") == 0 || strcmp(command, "raw-request") == 0) {
        char raw[JW_RA_REPLY_MAX];
        if (jw__join_args(argc, argv, argi, raw, sizeof(raw)) != 0) {
            fprintf(stderr, "%s requires a command\n", command);
            return 4;
        }
        if (strcmp(command, "raw-request") == 0) {
            char reply[JW_RA_REPLY_MAX];
            result = jw_ra_request_raw(&client, raw, reply, sizeof(reply));
            if (result == JW_RA_OK) {
                printf("result=ok\n");
                printf("reply=%s\n", reply);
                return 0;
            }
        } else {
            result = jw_ra_send_raw(&client, raw);
        }
    } else {
        fprintf(stderr, "unknown command: %s\n", command);
        jw__usage(stderr);
        return 4;
    }

    return jw__print_result(result);
}
