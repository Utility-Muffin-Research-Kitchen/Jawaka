#include "internal/ipc/delete_client.h"
#include "internal/ipc/ipc.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *ready_json =
    "{\"type\":\"rom-delete-status\",\"phase\":1,\"token\":\"reviewed-once\","
    "\"name\":\"Synthetic Game\",\"source_id\":\"primary\",\"bytes\":10,"
    "\"disc_count\":0,\"file_count\":1,\"keep_count\":0,\"shared_count\":0,"
    "\"removed_count\":0,\"absent_count\":0,\"missing_descriptors\":false,"
    "\"writable_progress\":false,\"files\":[{\"source_id\":\"primary\","
    "\"rom_relpath\":\"GBA/Game.gba\",\"size\":10,\"keep\":0,"
    "\"missing\":false,\"removed\":false}]}";

static void reply(jw_ipc_client *peer, const cJSON *object) {
    char *json = cJSON_PrintUnformatted(object);
    assert(json && jw_ipc_client_send(peer, json, strlen(json)) == 0);
    cJSON_free(json);
}

static cJSON *receive(jw_ipc_client *peer, const char *type) {
    char *json = NULL;
    size_t length;
    assert(jw_ipc_client_recv(peer, &json, &length) == 0);
    cJSON *object = cJSON_ParseWithLength(json, length);
    free(json);
    const cJSON *actual = cJSON_GetObjectItemCaseSensitive(object, "type");
    assert(cJSON_IsString(actual) && !strcmp(actual->valuestring, type));
    return object;
}

static void receive_preview(jw_ipc_client *peer, bool disc) {
    cJSON *request = receive(peer, "rom-delete-preview");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "source_id")->valuestring, "primary"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "rom_relpath")->valuestring,
                   disc ? "PS/Adventure.m3u" : "GBA/Game.gba"));
    assert(cJSON_GetArraySize(request) == (disc ? 4 : 3));
    if (disc) {
        const cJSON *selection = cJSON_GetObjectItemCaseSensitive(request, "disc");
        assert(cJSON_IsObject(selection) && cJSON_GetArraySize(selection) == 3);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(selection, "source_id")->valuestring, "secondary_sd"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(selection, "rom_relpath")->valuestring, "PS/Archive.zip"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(selection, "member")->valuestring, "Disc One"));
    }
    cJSON_Delete(request);
}

static void wait_child(pid_t child) {
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int begin(const char *socket, bool disc, jw_ipc_delete_session **session,
                  jw_ipc_delete_status *status) {
    return disc ? jw_ipc_delete_disc_begin(socket, "primary", "PS/Adventure.m3u",
        "secondary_sd", "PS/Archive.zip", "Disc One", session, status)
        : jw_ipc_delete_begin(socket, "primary", "GBA/Game.gba", session, status);
}

static void check_reply_scope(const char *socket, const cJSON *message, bool accepted, bool disc) {
    jw_ipc_server *server = NULL;
    assert(jw_ipc_server_listen(socket, &server) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        jw_ipc_client *peer = NULL;
        assert(jw_ipc_server_accept(server, &peer, 1000) == 0);
        receive_preview(peer, disc);
        reply(peer, message);
        char *unexpected = NULL;
        size_t length;
        assert(jw_ipc_client_recv(peer, &unexpected, &length) != 0);
        free(unexpected);
        jw_ipc_client_close(peer);
        _exit(0);
    }
    jw_ipc_delete_session *session = NULL;
    jw_ipc_delete_status status = {0};
    int rc = begin(socket, disc, &session, &status);
    assert((rc == 0) == accepted);
    if (!accepted) {
        assert(status.phase == JW_IPC_DELETE_ERROR && status.error[0] && !status.token[0] && !status.files);
        assert(jw_ipc_delete_poll(session, &status) == -1); /* no frame sent on failed session */
        assert(jw_ipc_delete_commit(session, "reviewed-once", &status) == -1);
    }
    jw_ipc_delete_status_free(&status);
    jw_ipc_delete_close(session);
    wait_child(child);
    jw_ipc_server_close(server);
}

static void check_reply(const char *socket, const cJSON *message, bool accepted) {
    check_reply_scope(socket, message, accepted, false);
}

static void disc_scope(cJSON *message, bool final) {
    cJSON_AddBoolToObject(message, "playlist_edit", !final);
    cJSON_AddBoolToObject(message, "final_disc", final);
    cJSON_AddBoolToObject(message, "playlist_replaced", false);
    cJSON_AddStringToObject(message, "disc_name", "Opening");
    cJSON_AddNumberToObject(message, "remaining_discs", final ? 0 : 1);
}

static void lifecycle(const char *socket, bool lose_commit_response, int scope) {
    jw_ipc_server *server = NULL;
    assert(jw_ipc_server_listen(socket, &server) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        jw_ipc_client *peer = NULL;
        assert(jw_ipc_server_accept(server, &peer, 1000) == 0);
        receive_preview(peer, scope != 0);
        cJSON *message = cJSON_Parse("{\"type\":\"rom-delete-status\",\"phase\":0,\"source_id\":\"primary\"}");
        reply(peer, message);
        cJSON_Delete(message);
        cJSON_Delete(receive(peer, "rom-delete-status"));
        message = cJSON_Parse(ready_json);
        if (scope) disc_scope(message, scope == 2);
        reply(peer, message);
        cJSON *request = receive(peer, "rom-delete-commit");
        assert(cJSON_GetArraySize(request) == 2);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "token")->valuestring, "reviewed-once"));
        cJSON_Delete(request);
        if (!lose_commit_response) {
            cJSON *busy = cJSON_Parse("{\"type\":\"rom-delete-status\",\"phase\":2,\"source_id\":\"primary\"}");
            reply(peer, busy);
            cJSON_Delete(busy);
            cJSON_Delete(receive(peer, "rom-delete-status"));
            cJSON_ReplaceItemInObjectCaseSensitive(message, "phase", cJSON_CreateNumber(3));
            cJSON_ReplaceItemInObjectCaseSensitive(message, "token", cJSON_CreateString(""));
            cJSON_ReplaceItemInObjectCaseSensitive(message, "removed_count", cJSON_CreateNumber(1));
            if (scope == 1)
                cJSON_ReplaceItemInObjectCaseSensitive(message, "playlist_replaced", cJSON_CreateBool(true));
            cJSON *file = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(message, "files"), 0);
            cJSON_ReplaceItemInObjectCaseSensitive(file, "removed", cJSON_CreateBool(true));
            reply(peer, message);
        }
        cJSON_Delete(message);
        jw_ipc_client_close(peer);
        _exit(0);
    }
    jw_ipc_delete_session *session = NULL;
    jw_ipc_delete_status status = {0};
    assert(begin(socket, scope != 0, &session, &status) == 0);
    assert(status.phase == JW_IPC_DELETE_PREPARING && !status.files);
    assert(jw_ipc_delete_poll(session, &status) == 0 && status.phase == JW_IPC_DELETE_READY);
    assert(status.files_count == 1 && status.bytes == 10);
    int rc = jw_ipc_delete_commit(session, status.token, &status);
    assert((rc == -1) == lose_commit_response);
    if (lose_commit_response) {
        assert(status.phase == JW_IPC_DELETE_ERROR && !status.token[0]);
        assert(jw_ipc_delete_commit(session, "reviewed-once", &status) == -1);
    } else {
        assert(status.phase == JW_IPC_DELETE_COMMITTING);
        assert(status.playlist_edit == (scope == 1) && status.final_disc == (scope == 2));
        assert(jw_ipc_delete_poll(session, &status) == 0);
        assert(status.phase == JW_IPC_DELETE_DONE && status.removed_count == 1 && status.files[0].removed);
        assert(status.playlist_replaced == (scope == 1));
    }
    jw_ipc_delete_status_free(&status);
    jw_ipc_delete_close(session);
    wait_child(child);
    jw_ipc_server_close(server);
}

int main(void) {
    char root[] = "/tmp/jw-delete-client-XXXXXX";
    assert(mkdtemp(root));
    char socket[128];
    snprintf(socket, sizeof(socket), "%s/ipc", root);
    cJSON *message = cJSON_Parse(ready_json);
    assert(message);
    check_reply(socket, message, true);
    cJSON_Delete(message);
    const struct { const char *field; const char *value; bool file; } invalid[] = {
        {"bytes", "-1", false}, {"bytes", "1.5", false},
        {"bytes", "9007199254740992", false}, {"file_count", "\"1\"", false},
        {"file_count", "0", false}, {"token", "\"\"", false},
        {"missing_descriptors", "\"false\"", false}, {"files", "[false]", false},
        {"keep", "2147483648", true}, {"keep", "-1", true}, {"keep", "0.5", true},
        {"rom_relpath", "\"\"", true}, {"missing", "0", true}, {"removed", "true", true},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        message = cJSON_Parse(ready_json);
        cJSON *object = invalid[i].file
            ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(message, "files"), 0) : message;
        cJSON_ReplaceItemInObjectCaseSensitive(object, invalid[i].field, cJSON_Parse(invalid[i].value));
        check_reply(socket, message, false);
        cJSON_Delete(message);
    }
    message = cJSON_Parse(ready_json);
    cJSON_DeleteItemFromObjectCaseSensitive(message, "file_count");
    check_reply(socket, message, false);
    cJSON_Delete(message);
    message = cJSON_Parse("{\"type\":\"rom-delete-status\",\"phase\":4,\"error\":\"Card is read-only.\",\"readonly_source\":\"secondary_sd\"}");
    check_reply(socket, message, true);
    cJSON_Delete(message);
    message = cJSON_Parse(ready_json);
    check_reply_scope(socket, message, false, true); /* old whole-game reply cannot authorize disc request */
    disc_scope(message, false);
    check_reply_scope(socket, message, true, true);
    check_reply(socket, message, false); /* game request cannot quietly change scope */
    cJSON_Delete(message);
    const struct { const char *field; const char *value; } bad_scope[] = {
        {"final_disc", "true"}, {"remaining_discs", "0"}, {"remaining_discs", "1.5"},
        {"disc_name", "\"\""}, {"playlist_replaced", "true"}, {"playlist_edit", "\"true\""}
    };
    for (size_t i = 0; i < sizeof(bad_scope) / sizeof(bad_scope[0]); ++i) {
        message = cJSON_Parse(ready_json);
        disc_scope(message, false);
        cJSON_ReplaceItemInObjectCaseSensitive(message, bad_scope[i].field, cJSON_Parse(bad_scope[i].value));
        check_reply_scope(socket, message, false, true);
        cJSON_Delete(message);
    }
    message = cJSON_Parse(ready_json);
    disc_scope(message, true);
    check_reply_scope(socket, message, true, true);
    cJSON_ReplaceItemInObjectCaseSensitive(message, "remaining_discs", cJSON_CreateNumber(1));
    check_reply_scope(socket, message, false, true);
    cJSON_Delete(message);
    lifecycle(socket, false, 0);
    lifecycle(socket, true, 0);
    lifecycle(socket, false, 1);
    lifecycle(socket, true, 1);
    lifecycle(socket, false, 2);
    rmdir(root);
    puts("delete-client-test: retained session, identity-only requests, lost-response invalidation and malformed preview rejection passed");
    return 0;
}
