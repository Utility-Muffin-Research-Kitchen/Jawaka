#include "internal/ipc/ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct jw_ipc_server {
    int fd;
    char *socket_path;
};

struct jw_ipc_client {
    int fd;
};

static int jw__write_all(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t written = write(fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        ptr += (size_t)written;
        remaining -= (size_t)written;
    }

    return 0;
}

static int jw__read_all(int fd, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t nread = read(fd, ptr, remaining);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nread == 0) {
            return -1;
        }
        ptr += (size_t)nread;
        remaining -= (size_t)nread;
    }

    return 0;
}

static int jw__make_unix_addr(const char *socket_path, struct sockaddr_un *addr, socklen_t *addr_len) {
    if (!socket_path || !addr || !addr_len) {
        return -1;
    }

    if (strlen(socket_path) >= sizeof(addr->sun_path)) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    strcpy(addr->sun_path, socket_path);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(addr->sun_path) + 1u);
    return 0;
}

int jw_ipc_server_listen(const char *socket_path, jw_ipc_server **out) {
    if (!socket_path || !out) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (jw__make_unix_addr(socket_path, &addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }

    unlink(socket_path);

    if (bind(fd, (struct sockaddr *)&addr, addr_len) != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 8) != 0) {
        close(fd);
        unlink(socket_path);
        return -1;
    }

    jw_ipc_server *server = (jw_ipc_server *)calloc(1, sizeof(*server));
    if (!server) {
        close(fd);
        unlink(socket_path);
        return -1;
    }

    server->fd = fd;
    server->socket_path = strdup(socket_path);
    if (!server->socket_path) {
        close(fd);
        unlink(socket_path);
        free(server);
        return -1;
    }

    *out = server;
    return 0;
}

int jw_ipc_server_accept(jw_ipc_server *server, jw_ipc_client **out_client, int timeout_ms) {
    if (!server || !out_client) {
        return -1;
    }

    if (timeout_ms >= 0) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server->fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ready = select(server->fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready == 0) {
            return 1;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                return 1;
            }
            return -1;
        }
    }

    int client_fd = accept(server->fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EINTR) {
            return 1;
        }
        return -1;
    }

    jw_ipc_client *client = (jw_ipc_client *)calloc(1, sizeof(*client));
    if (!client) {
        close(client_fd);
        return -1;
    }

    client->fd = client_fd;
    *out_client = client;
    return 0;
}

void jw_ipc_server_close(jw_ipc_server *server) {
    if (!server) {
        return;
    }

    if (server->fd >= 0) {
        close(server->fd);
    }
    if (server->socket_path) {
        unlink(server->socket_path);
        free(server->socket_path);
    }
    free(server);
}

int jw_ipc_client_connect(const char *socket_path, jw_ipc_client **out) {
    if (!socket_path || !out) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (jw__make_unix_addr(socket_path, &addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, addr_len) != 0) {
        close(fd);
        return -1;
    }

    jw_ipc_client *client = (jw_ipc_client *)calloc(1, sizeof(*client));
    if (!client) {
        close(fd);
        return -1;
    }

    client->fd = fd;
    *out = client;
    return 0;
}

int jw_ipc_client_send(jw_ipc_client *client, const char *json, size_t len) {
    if (!client || !json || len > JW_IPC_MAX_FRAME) {
        return -1;
    }

    uint32_t frame_len = htonl((uint32_t)len);
    if (jw__write_all(client->fd, &frame_len, sizeof(frame_len)) != 0) {
        return -1;
    }
    if (jw__write_all(client->fd, json, len) != 0) {
        return -1;
    }
    return 0;
}

int jw_ipc_client_recv(jw_ipc_client *client, char **out_json, size_t *out_len) {
    if (!client || !out_json || !out_len) {
        return -1;
    }

    uint32_t frame_len = 0;
    if (jw__read_all(client->fd, &frame_len, sizeof(frame_len)) != 0) {
        return -1;
    }

    uint32_t payload_len = ntohl(frame_len);
    if (payload_len > JW_IPC_MAX_FRAME) {
        return -1;
    }

    char *json = (char *)malloc((size_t)payload_len + 1u);
    if (!json) {
        return -1;
    }

    if (jw__read_all(client->fd, json, payload_len) != 0) {
        free(json);
        return -1;
    }

    json[payload_len] = '\0';
    *out_json = json;
    *out_len = (size_t)payload_len;
    return 0;
}

void jw_ipc_client_close(jw_ipc_client *client) {
    if (!client) {
        return;
    }
    if (client->fd >= 0) {
        close(client->fd);
    }
    free(client);
}

int jw_ipc_request(const char *socket_path, const char *json, size_t len, char **out_json, size_t *out_len) {
    jw_ipc_client *client = NULL;
    if (jw_ipc_client_connect(socket_path, &client) != 0) {
        return -1;
    }

    if (jw_ipc_client_send(client, json, len) != 0) {
        jw_ipc_client_close(client);
        return -1;
    }

    int rc = jw_ipc_client_recv(client, out_json, out_len);
    jw_ipc_client_close(client);
    return rc;
}
