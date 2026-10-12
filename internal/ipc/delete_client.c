#include "internal/ipc/delete_client.h"
#include "internal/ipc/ipc.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct jw_ipc_delete_session { jw_ipc_client *client; bool failed; };

void jw_ipc_delete_status_free(jw_ipc_delete_status *status) {
    if (!status) return;
    free(status->files);
    memset(status, 0, sizeof(*status));
}

static bool jw__string(const cJSON *object, const char *key, char *out, size_t size) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value) { out[0] = '\0'; return true; }
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= size) return false;
    memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}

static bool jw__number(const cJSON *object, const char *key, uint64_t limit,
                        bool required, uint64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    *out = 0;
    if (!v) return !required;
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0) ||
        !(v->valuedouble < 9007199254740992.0)) return false;
    uint64_t number = (uint64_t)v->valuedouble;
    if ((double)number != v->valuedouble || number > limit) return false;
    *out = number;
    return true;
}

static bool jw__boolean(const cJSON *object, const char *key, bool required, bool *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    *out = false;
    if (!v) return !required;
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v);
    return true;
}

static int jw__exchange(jw_ipc_delete_session *session, cJSON *request,
                         jw_ipc_delete_status *status) {
    jw_ipc_delete_status_free(status);
    status->phase = JW_IPC_DELETE_ERROR;
    char *encoded = request ? cJSON_PrintUnformatted(request) : NULL;
    char *raw = NULL;
    size_t length = 0;
    cJSON *reply = NULL;
    int rc = -1;
    if (!session || session->failed || !encoded ||
        jw_ipc_client_send(session->client, encoded, strlen(encoded)) != 0 ||
        jw_ipc_client_recv(session->client, &raw, &length) != 0) goto done;
    reply = cJSON_ParseWithLength(raw, length);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(reply, "type");
    const cJSON *phase = cJSON_GetObjectItemCaseSensitive(reply, "phase");
    if (!cJSON_IsObject(reply) || !cJSON_IsString(type) || strcmp(type->valuestring, "rom-delete-status") ||
        !cJSON_IsNumber(phase) || phase->valuedouble != phase->valueint ||
        phase->valueint < JW_IPC_DELETE_PREPARING || phase->valueint > JW_IPC_DELETE_CANCELLED)
        goto done;
    status->phase = (jw_ipc_delete_phase)phase->valueint;
#define READ_STRING(field) if (!jw__string(reply, #field, status->field, sizeof(status->field))) goto done
    READ_STRING(token); READ_STRING(name); READ_STRING(source_id); READ_STRING(error);
    READ_STRING(readonly_source); READ_STRING(missing_sources);
#undef READ_STRING
    bool full = status->phase == JW_IPC_DELETE_READY || status->phase == JW_IPC_DELETE_DONE;
    if ((full && (!status->name[0] || !status->source_id[0])) ||
        (status->phase == JW_IPC_DELETE_READY && !status->token[0]) ||
        ((status->phase == JW_IPC_DELETE_PREPARING || status->phase == JW_IPC_DELETE_COMMITTING) &&
            !status->source_id[0]) ||
        (status->phase == JW_IPC_DELETE_ERROR && !status->error[0])) goto done;
    uint64_t number;
#define READ_COUNT(field) \
    if (!jw__number(reply, #field, SIZE_MAX, full, &number)) goto done; \
    status->field = (size_t)number
    READ_COUNT(disc_count); READ_COUNT(file_count); READ_COUNT(keep_count);
    READ_COUNT(shared_count); READ_COUNT(removed_count); READ_COUNT(absent_count);
#undef READ_COUNT
    if (!jw__number(reply, "bytes", UINT64_MAX, full, &status->bytes) ||
        !jw__boolean(reply, "missing_descriptors", full, &status->missing_descriptors) ||
        !jw__boolean(reply, "writable_progress", full, &status->writable_progress)) goto done;
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(reply, "files");
    if ((files && !cJSON_IsArray(files)) || (full && !files)) goto done;
    int count = cJSON_GetArraySize(files);
    if (count > 65536) goto done;
    if (count) {
        status->files = calloc((size_t)count, sizeof(*status->files));
        if (!status->files) goto done;
    }
    status->files_count = (size_t)count;
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(files, i);
        jw_ipc_delete_file *file = &status->files[i];
        if (!cJSON_IsObject(item) ||
            !jw__string(item, "source_id", file->source_id, sizeof(file->source_id)) ||
            !jw__string(item, "rom_relpath", file->rom_relpath, sizeof(file->rom_relpath)) ||
            !file->source_id[0] || !file->rom_relpath[0] ||
            !jw__number(item, "size", UINT64_MAX, true, &file->size) ||
            !jw__number(item, "keep", 2, true, &number) ||
            !jw__boolean(item, "missing", true, &file->missing) ||
            !jw__boolean(item, "removed", true, &file->removed)) goto done;
        file->keep = (int)number;
    }
    if (files) {
        size_t remove = 0, keep = 0, shared = 0, removed = 0;
        uint64_t estimated_bytes = 0;
        for (size_t i = 0; i < status->files_count; ++i) {
            const jw_ipc_delete_file *file = &status->files[i];
            if (file->keep) ++keep; else ++remove;
            if (file->keep == 1) ++shared;
            if (file->removed && (file->keep || file->missing)) goto done;
            if (file->removed) ++removed;
            if (!file->keep && !file->missing) {
                if (UINT64_MAX - estimated_bytes < file->size) goto done;
                estimated_bytes += file->size;
            }
        }
        if (status->file_count != remove || status->keep_count != keep ||
            status->shared_count != shared || status->removed_count != removed ||
            status->absent_count > remove - status->removed_count) goto done;
        if (status->phase == JW_IPC_DELETE_READY &&
            (status->removed_count || status->absent_count || status->bytes != estimated_bytes)) goto done;
    }
    rc = 0;
done:
    free(raw);
    cJSON_free(encoded);
    cJSON_Delete(request);
    cJSON_Delete(reply);
    if (rc != 0) {
        if (session) session->failed = true;
        jw_ipc_delete_status_free(status);
        status->phase = JW_IPC_DELETE_ERROR;
        snprintf(status->error, sizeof(status->error), "%s",
                 "Connection lost. Check the library and request a fresh deletion preview.");
    }
    return rc;
}

static cJSON *jw__request(const char *type) {
    cJSON *request = cJSON_CreateObject();
    if (request) cJSON_AddStringToObject(request, "type", type);
    return request;
}

int jw_ipc_delete_begin(const char *socket_path, const char *source_id,
                        const char *rom_relpath, jw_ipc_delete_session **out,
                        jw_ipc_delete_status *status) {
    *out = calloc(1, sizeof(**out));
    cJSON *request = jw__request("rom-delete-preview");
    if (request) {
        cJSON_AddStringToObject(request, "source_id", source_id);
        cJSON_AddStringToObject(request, "rom_relpath", rom_relpath);
    }
    if (!*out || jw_ipc_client_connect(socket_path, &(*out)->client) != 0) {
        if (*out) (*out)->failed = true;
    } else if (jw_ipc_client_set_nonblocking((*out)->client, false, 500) != 0) {
        (*out)->failed = true;
    }
    return jw__exchange(*out, request, status);
}

int jw_ipc_delete_poll(jw_ipc_delete_session *session, jw_ipc_delete_status *status) {
    return jw__exchange(session, jw__request("rom-delete-status"), status);
}

int jw_ipc_delete_commit(jw_ipc_delete_session *session, const char *token,
                         jw_ipc_delete_status *status) {
    cJSON *request = jw__request("rom-delete-commit");
    if (request) cJSON_AddStringToObject(request, "token", token);
    return jw__exchange(session, request, status);
}

void jw_ipc_delete_cancel(jw_ipc_delete_session *session) {
    jw_ipc_delete_status status = {0};
    (void)jw__exchange(session, jw__request("rom-delete-cancel"), &status);
    jw_ipc_delete_status_free(&status);
}

void jw_ipc_delete_close(jw_ipc_delete_session *session) {
    if (!session) return;
    if (session->client) jw_ipc_client_close(session->client);
    free(session);
}
