#ifndef JW_IPC_H
#define JW_IPC_H

#include <stddef.h>
#include <sys/types.h>

#define JW_IPC_MAX_FRAME (16u * 1024u * 1024u)

typedef struct jw_ipc_server jw_ipc_server;
typedef struct jw_ipc_client jw_ipc_client;

int  jw_ipc_server_listen(const char *socket_path, jw_ipc_server **out);
/* Returns: 0 = client accepted (*out_client set)
 *          1 = no client yet (timeout or EINTR — caller should retry)
 *         -1 = unrecoverable error */
int  jw_ipc_server_accept(jw_ipc_server *server, jw_ipc_client **out_client, int timeout_ms);
void jw_ipc_server_close(jw_ipc_server *server);

int  jw_ipc_client_connect(const char *socket_path, jw_ipc_client **out);
int  jw_ipc_client_send(jw_ipc_client *client, const char *json, size_t len);
int  jw_ipc_client_recv(jw_ipc_client *client, char **out_json, size_t *out_len);
/* Resolves the server-side Unix peer pid from kernel credentials. */
int  jw_ipc_client_peer_pid(jw_ipc_client *client, pid_t *out_pid);
void jw_ipc_client_close(jw_ipc_client *client);

int  jw_ipc_request(const char *socket_path, const char *json, size_t len, char **out_json, size_t *out_len);

#endif
