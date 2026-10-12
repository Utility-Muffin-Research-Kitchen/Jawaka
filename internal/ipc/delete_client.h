#ifndef JW_IPC_DELETE_CLIENT_H
#define JW_IPC_DELETE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct jw_ipc_delete_session jw_ipc_delete_session;

typedef enum {
    JW_IPC_DELETE_PREPARING,
    JW_IPC_DELETE_READY,
    JW_IPC_DELETE_COMMITTING,
    JW_IPC_DELETE_DONE,
    JW_IPC_DELETE_ERROR,
    JW_IPC_DELETE_CANCELLED
} jw_ipc_delete_phase;

typedef struct {
    char source_id[32];
    char rom_relpath[512];
    uint64_t size;
    int keep; /* 0: remove, 1: shared, 2: preserved user data */
    bool missing;
    bool removed;
} jw_ipc_delete_file;

typedef struct {
    jw_ipc_delete_phase phase;
    char token[65];
    char name[512];
    char source_id[32];
    char error[512];
    char readonly_source[32];
    char missing_sources[512];
    bool missing_descriptors;
    bool writable_progress;
    uint64_t bytes;
    size_t disc_count;
    size_t file_count; /* proposed removal, including already missing files */
    size_t keep_count;
    size_t shared_count;
    size_t removed_count;
    size_t absent_count;
    jw_ipc_delete_file *files;
    size_t files_count;
} jw_ipc_delete_status;

/* One retained connection owns a preview and its one-use token. No reconnect
   or automatic retry: a transport failure requires a fresh preview. Initialize
   status to zero; each call replaces it. A daemon error is phase ERROR with
   return 0; -1 means transport/protocol failure, with status.error populated. */
int jw_ipc_delete_begin(const char *socket_path, const char *source_id,
                        const char *rom_relpath, jw_ipc_delete_session **session,
                        jw_ipc_delete_status *status);
int jw_ipc_delete_poll(jw_ipc_delete_session *session, jw_ipc_delete_status *status);
int jw_ipc_delete_commit(jw_ipc_delete_session *session, const char *token,
                         jw_ipc_delete_status *status);
void jw_ipc_delete_cancel(jw_ipc_delete_session *session);
void jw_ipc_delete_close(jw_ipc_delete_session *session);
void jw_ipc_delete_status_free(jw_ipc_delete_status *status);

#endif
