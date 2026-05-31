#include "internal/retroarch/command.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

const char *jw_ra_result_string(jw_ra_result result) {
    switch (result) {
        case JW_RA_OK: return "ok";
        case JW_RA_TIMEOUT: return "timeout";
        case JW_RA_UNSUPPORTED: return "unsupported";
        case JW_RA_PARSE_ERROR: return "parse_error";
        case JW_RA_SOCKET_ERROR: return "socket_error";
        default: return "unknown";
    }
}

const char *jw_ra_play_state_string(jw_ra_play_state state) {
    switch (state) {
        case JW_RA_STATE_CONTENTLESS: return "contentless";
        case JW_RA_STATE_PLAYING: return "playing";
        case JW_RA_STATE_PAUSED: return "paused";
        case JW_RA_STATE_UNKNOWN:
        default: return "unknown";
    }
}

jw_ra_client jw_ra_client_default(void) {
    jw_ra_client client;
    client.host = JW_RA_DEFAULT_HOST;
    client.port = JW_RA_DEFAULT_PORT;
    client.timeout_ms = JW_RA_DEFAULT_TIMEOUT_MS;
    return client;
}

static bool jw_ra__starts_with_word(const char *command, const char *prefix) {
    size_t len;
    if (!command || !prefix) {
        return false;
    }

    len = strlen(prefix);
    return strncmp(command, prefix, len) == 0 &&
           (command[len] == '\0' || isspace((unsigned char)command[len]));
}

static bool jw_ra__exact_supported(const char *command) {
    static const char *const supported[] = {
        "CLOSE_CONTENT",
        "DISK_EJECT_TOGGLE",
        "DISK_NEXT",
        "DISK_PREV",
        "FPS_TOGGLE",
        "FULLSCREEN_TOGGLE",
        "GET_DISK_COUNT",
        "GET_DISK_SLOT",
        "GET_INFO",
        "GET_STATE_SLOT",
        "GET_STATUS",
        "LOAD_STATE",
        "MENU_TOGGLE",
        "MUTE",
        "OPEN_MENU",
        "PAUSE",
        "PAUSE_TOGGLE",
        "QUIT",
        "RESET",
        "SAVE_STATE",
        "SCREENSHOT",
        "STATE_SLOT_MINUS",
        "STATE_SLOT_PLUS",
        "UNPAUSE",
        "VOLUME_DOWN",
        "VOLUME_UP",
    };
    size_t i;

    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strcmp(command, supported[i]) == 0) {
            return true;
        }
    }

    return false;
}

bool jw_ra_raw_command_supported(const char *command) {
    const char *p = command;

    if (!command || !command[0]) {
        return false;
    }

    while (*p) {
        if ((unsigned char)*p < 0x20 && *p != '\t') {
            return false;
        }
        p++;
    }

    if (jw_ra__exact_supported(command)) {
        return true;
    }

    return jw_ra__starts_with_word(command, "GET_CONFIG_PARAM") ||
           jw_ra__starts_with_word(command, "GET_PATH") ||
           jw_ra__starts_with_word(command, "LOAD_CORE") ||
           jw_ra__starts_with_word(command, "LOAD_STATE_SLOT") ||
           jw_ra__starts_with_word(command, "PLAY_REPLAY_SLOT") ||
           jw_ra__starts_with_word(command, "READ_CORE_MEMORY") ||
           jw_ra__starts_with_word(command, "SEEK_REPLAY") ||
           jw_ra__starts_with_word(command, "SAVE_STATE_SLOT") ||
           jw_ra__starts_with_word(command, "SET_DISK_SLOT") ||
           jw_ra__starts_with_word(command, "SET_STATE_SLOT") ||
           jw_ra__starts_with_word(command, "SHOW_MSG") ||
           jw_ra__starts_with_word(command, "WRITE_CORE_MEMORY");
}

static jw_ra_result jw_ra__resolve(const jw_ra_client *client,
                                   struct sockaddr_storage *out_addr,
                                   socklen_t *out_addr_len) {
    char port[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it = NULL;
    int rc;

    if (!client || !client->host || !out_addr || !out_addr_len) {
        return JW_RA_SOCKET_ERROR;
    }

    snprintf(port, sizeof(port), "%u", client->port ? client->port : JW_RA_DEFAULT_PORT);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(client->host, port, &hints, &res);
    if (rc != 0 || !res) {
        return JW_RA_SOCKET_ERROR;
    }

    for (it = res; it; it = it->ai_next) {
        if (it->ai_addrlen <= sizeof(*out_addr)) {
            memset(out_addr, 0, sizeof(*out_addr));
            memcpy(out_addr, it->ai_addr, it->ai_addrlen);
            *out_addr_len = (socklen_t)it->ai_addrlen;
            freeaddrinfo(res);
            return JW_RA_OK;
        }
    }

    freeaddrinfo(res);
    return JW_RA_SOCKET_ERROR;
}

static jw_ra_result jw_ra__exchange(const jw_ra_client *client,
                                    const char *command,
                                    bool expect_reply,
                                    char *reply,
                                    size_t reply_size) {
    jw_ra_result result;
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    int fd = -1;
    ssize_t sent;
    unsigned timeout_ms;

    if (!jw_ra_raw_command_supported(command)) {
        return JW_RA_UNSUPPORTED;
    }

    if (expect_reply && (!reply || reply_size == 0)) {
        return JW_RA_PARSE_ERROR;
    }

    if (reply && reply_size > 0) {
        reply[0] = '\0';
    }

    result = jw_ra__resolve(client, &addr, &addr_len);
    if (result != JW_RA_OK) {
        return result;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return JW_RA_SOCKET_ERROR;
    }

    sent = sendto(fd, command, strlen(command), 0, (struct sockaddr *)&addr, addr_len);
    if (sent < 0 || (size_t)sent != strlen(command)) {
        close(fd);
        return JW_RA_SOCKET_ERROR;
    }

    if (!expect_reply) {
        close(fd);
        return JW_RA_OK;
    }

    timeout_ms = client && client->timeout_ms ? client->timeout_ms : JW_RA_DEFAULT_TIMEOUT_MS;
    for (;;) {
        fd_set read_fds;
        struct timeval tv;
        int ready;

        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        tv.tv_sec = (time_t)(timeout_ms / 1000u);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);

        ready = select(fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready == 0) {
            close(fd);
            return JW_RA_TIMEOUT;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return JW_RA_SOCKET_ERROR;
        }
        break;
    }

    {
        ssize_t nread = recvfrom(fd, reply, reply_size - 1u, 0, NULL, NULL);
        if (nread < 0) {
            close(fd);
            return JW_RA_SOCKET_ERROR;
        }
        reply[nread] = '\0';
    }

    close(fd);
    return JW_RA_OK;
}

jw_ra_result jw_ra_send_raw(const jw_ra_client *client, const char *command) {
    return jw_ra__exchange(client, command, false, NULL, 0);
}

jw_ra_result jw_ra_request_raw(const jw_ra_client *client, const char *command,
                               char *reply, size_t reply_size) {
    return jw_ra__exchange(client, command, true, reply, reply_size);
}

static void jw_ra__trim_line(char *s) {
    size_t len;
    if (!s) {
        return;
    }

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

static void jw_ra__copy_token(char *dst, size_t dst_size, const char *start, size_t len) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!start) {
        dst[0] = '\0';
        return;
    }
    if (len >= dst_size) {
        len = dst_size - 1u;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
}

static jw_ra_result jw_ra__parse_status(const char *reply, jw_ra_status *status) {
    const char *p;
    const char *token_end;
    const char *comma;
    const char *next_comma;

    if (!reply || !status) {
        return JW_RA_PARSE_ERROR;
    }

    memset(status, 0, sizeof(*status));
    status->state = JW_RA_STATE_UNKNOWN;
    snprintf(status->raw, sizeof(status->raw), "%s", reply);
    jw_ra__trim_line(status->raw);

    if (strcmp(status->raw, "GET_STATUS CONTENTLESS") == 0) {
        status->state = JW_RA_STATE_CONTENTLESS;
        return JW_RA_OK;
    }

    p = status->raw;
    if (strncmp(p, "GET_STATUS ", 11) != 0) {
        return JW_RA_PARSE_ERROR;
    }
    p += 11;

    token_end = strchr(p, ' ');
    if (!token_end) {
        return JW_RA_PARSE_ERROR;
    }

    if ((size_t)(token_end - p) == strlen("PLAYING") &&
        strncmp(p, "PLAYING", (size_t)(token_end - p)) == 0) {
        status->state = JW_RA_STATE_PLAYING;
    } else if ((size_t)(token_end - p) == strlen("PAUSED") &&
               strncmp(p, "PAUSED", (size_t)(token_end - p)) == 0) {
        status->state = JW_RA_STATE_PAUSED;
    } else {
        return JW_RA_PARSE_ERROR;
    }

    p = token_end + 1;
    comma = strchr(p, ',');
    if (!comma) {
        jw_ra__copy_token(status->system, sizeof(status->system), p, strlen(p));
        return JW_RA_OK;
    }

    jw_ra__copy_token(status->system, sizeof(status->system), p, (size_t)(comma - p));

    p = comma + 1;
    next_comma = strchr(p, ',');
    if (next_comma) {
        jw_ra__copy_token(status->content, sizeof(status->content), p, (size_t)(next_comma - p));
    } else {
        jw_ra__copy_token(status->content, sizeof(status->content), p, strlen(p));
    }

    return JW_RA_OK;
}

static jw_ra_result jw_ra__parse_prefixed_int(const char *reply,
                                              const char *prefix,
                                              int *out) {
    char *end = NULL;
    long value;
    size_t prefix_len;

    if (!reply || !prefix || !out) {
        return JW_RA_PARSE_ERROR;
    }

    prefix_len = strlen(prefix);
    if (strncmp(reply, prefix, prefix_len) != 0) {
        return JW_RA_PARSE_ERROR;
    }

    errno = 0;
    value = strtol(reply + prefix_len, &end, 10);
    if (errno != 0 || end == reply + prefix_len ||
        (end && *end != '\0' && *end != '\n' && *end != '\r')) {
        return JW_RA_PARSE_ERROR;
    }
    if (value < -1 || value > 999999L) {
        return JW_RA_PARSE_ERROR;
    }

    *out = (int)value;
    return JW_RA_OK;
}

static jw_ra_result jw_ra__parse_info(const char *reply, jw_ra_info *info) {
    char raw[JW_RA_REPLY_MAX];
    char state_token[32];
    int nread;

    if (!reply || !info) {
        return JW_RA_PARSE_ERROR;
    }

    memset(info, 0, sizeof(*info));
    info->disk_count = 0;
    info->disk_slot = 0;
    info->savestate_supported = false;
    info->state_slot = 0;
    snprintf(info->raw, sizeof(info->raw), "%s", reply);
    jw_ra__trim_line(info->raw);
    snprintf(raw, sizeof(raw), "%s", info->raw);

    nread = sscanf(raw, "GET_INFO %d %d %31s",
                   &info->disk_count, &info->disk_slot, state_token);
    if (nread != 3 || info->disk_count < 0 || info->disk_slot < 0) {
        return JW_RA_PARSE_ERROR;
    }

    if (strcmp(state_token, "NO") == 0) {
        info->savestate_supported = false;
        info->state_slot = 0;
        return JW_RA_OK;
    }

    {
        char *end = NULL;
        long value;
        errno = 0;
        value = strtol(state_token, &end, 10);
        if (errno != 0 || end == state_token || (end && *end != '\0') ||
            value < -1 || value > 999999L) {
            return JW_RA_PARSE_ERROR;
        }
        info->savestate_supported = true;
        info->state_slot = (int)value;
    }

    return JW_RA_OK;
}

jw_ra_result jw_ra_get_status(const jw_ra_client *client, jw_ra_status *status) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_STATUS", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_status(reply, status);
}

jw_ra_result jw_ra_get_info(const jw_ra_client *client, jw_ra_info *info) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_INFO", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_info(reply, info);
}

jw_ra_result jw_ra_pause(const jw_ra_client *client) {
    jw_ra_status status;
    jw_ra_result result = jw_ra_get_status(client, &status);
    if (result != JW_RA_OK) {
        return result;
    }
    if (status.state == JW_RA_STATE_PAUSED) {
        return JW_RA_OK;
    }
    if (status.state != JW_RA_STATE_PLAYING) {
        return JW_RA_UNSUPPORTED;
    }
    return jw_ra_send_raw(client, "PAUSE_TOGGLE");
}

jw_ra_result jw_ra_resume(const jw_ra_client *client) {
    jw_ra_status status;
    jw_ra_result result = jw_ra_get_status(client, &status);
    if (result != JW_RA_OK) {
        return result;
    }
    if (status.state == JW_RA_STATE_PLAYING) {
        return JW_RA_OK;
    }
    if (status.state != JW_RA_STATE_PAUSED) {
        return JW_RA_UNSUPPORTED;
    }
    return jw_ra_send_raw(client, "PAUSE_TOGGLE");
}

jw_ra_result jw_ra_pause_direct(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "PAUSE");
}

jw_ra_result jw_ra_resume_direct(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "UNPAUSE");
}

jw_ra_result jw_ra_menu_toggle(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "MENU_TOGGLE");
}

jw_ra_result jw_ra_open_menu(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "OPEN_MENU");
}

jw_ra_result jw_ra_quit(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "QUIT");
}

jw_ra_result jw_ra_reset(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "RESET");
}

jw_ra_result jw_ra_screenshot(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "SCREENSHOT");
}

jw_ra_result jw_ra_save_state(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "SAVE_STATE");
}

jw_ra_result jw_ra_load_state(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "LOAD_STATE");
}

jw_ra_result jw_ra_load_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size) {
    char command[64];

    if (slot < 0) {
        return JW_RA_UNSUPPORTED;
    }

    snprintf(command, sizeof(command), "LOAD_STATE_SLOT %d", slot);
    if (reply && reply_size > 0) {
        return jw_ra_request_raw(client, command, reply, reply_size);
    }
    return jw_ra_send_raw(client, command);
}

jw_ra_result jw_ra_set_state_slot(const jw_ra_client *client, int slot) {
    char command[64];
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result;

    if (slot < -1) {
        return JW_RA_UNSUPPORTED;
    }

    snprintf(command, sizeof(command), "SET_STATE_SLOT %d", slot);
    result = jw_ra_request_raw(client, command, reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "SET_STATE_SLOT ", &slot);
}

jw_ra_result jw_ra_get_state_slot(const jw_ra_client *client, int *out_slot,
                                  bool *out_supported) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result;

    if (out_slot) {
        *out_slot = 0;
    }
    if (out_supported) {
        *out_supported = false;
    }

    result = jw_ra_request_raw(client, "GET_STATE_SLOT", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }

    jw_ra__trim_line(reply);
    if (strcmp(reply, "GET_STATE_SLOT NO") == 0) {
        return JW_RA_OK;
    }

    result = jw_ra__parse_prefixed_int(reply, "GET_STATE_SLOT ", out_slot);
    if (result == JW_RA_OK && out_supported) {
        *out_supported = true;
    }
    return result;
}

jw_ra_result jw_ra_save_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size) {
    char command[64];

    if (slot < -1) {
        return JW_RA_UNSUPPORTED;
    }

    snprintf(command, sizeof(command), "SAVE_STATE_SLOT %d", slot);
    if (reply && reply_size > 0) {
        return jw_ra_request_raw(client, command, reply, reply_size);
    }
    return jw_ra_send_raw(client, command);
}

jw_ra_result jw_ra_state_slot_plus(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "STATE_SLOT_PLUS");
}

jw_ra_result jw_ra_state_slot_minus(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "STATE_SLOT_MINUS");
}

jw_ra_result jw_ra_get_disk_count(const jw_ra_client *client, int *out_count) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_DISK_COUNT", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "GET_DISK_COUNT ", out_count);
}

jw_ra_result jw_ra_get_disk_slot(const jw_ra_client *client, int *out_slot) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_DISK_SLOT", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "GET_DISK_SLOT ", out_slot);
}

jw_ra_result jw_ra_set_disk_slot(const jw_ra_client *client, int slot) {
    char command[64];
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result;

    if (slot < 0) {
        return JW_RA_UNSUPPORTED;
    }

    snprintf(command, sizeof(command), "SET_DISK_SLOT %d", slot);
    result = jw_ra_request_raw(client, command, reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "SET_DISK_SLOT ", &slot);
}

jw_ra_result jw_ra_disk_eject_toggle(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "DISK_EJECT_TOGGLE");
}

jw_ra_result jw_ra_disk_next(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "DISK_NEXT");
}

jw_ra_result jw_ra_disk_prev(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "DISK_PREV");
}

jw_ra_result jw_ra_get_path(const jw_ra_client *client, const char *kind,
                            char *out, size_t out_size) {
    char command[64];
    char reply[JW_RA_REPLY_MAX];
    char prefix[96];
    size_t prefix_len;
    jw_ra_result result;
    const char *value;

    if (!kind || !kind[0] || !out || out_size == 0) {
        return JW_RA_PARSE_ERROR;
    }

    out[0] = '\0';
    if (snprintf(command, sizeof(command), "GET_PATH %s", kind) >= (int)sizeof(command) ||
        snprintf(prefix, sizeof(prefix), "GET_PATH %s ", kind) >= (int)sizeof(prefix)) {
        return JW_RA_PARSE_ERROR;
    }

    result = jw_ra_request_raw(client, command, reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }

    jw_ra__trim_line(reply);
    prefix_len = strlen(prefix);
    if (strncmp(reply, prefix, prefix_len) != 0) {
        return JW_RA_PARSE_ERROR;
    }

    value = reply + prefix_len;
    snprintf(out, out_size, "%s", value);
    return out[0] ? JW_RA_OK : JW_RA_UNSUPPORTED;
}

jw_ra_result jw_ra_get_savestate_path(const jw_ra_client *client,
                                      char *out, size_t out_size) {
    return jw_ra_get_path(client, "savestate", out, out_size);
}

jw_ra_result jw_ra_show_message(const jw_ra_client *client, const char *message) {
    char command[JW_RA_REPLY_MAX];
    if (!message || !message[0]) {
        return JW_RA_PARSE_ERROR;
    }
    if (snprintf(command, sizeof(command), "SHOW_MSG %s", message) >= (int)sizeof(command)) {
        return JW_RA_PARSE_ERROR;
    }
    return jw_ra_send_raw(client, command);
}
