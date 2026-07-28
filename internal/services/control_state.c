#include "internal/services/control_state.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct jw_svc_control_store {
    sqlite3 *db;
};

static const char *const JW__CONTROL_SCHEMA_SQL =
    "PRAGMA foreign_keys = OFF;\n"
    "CREATE TABLE IF NOT EXISTS service_control_state (\n"
    "  service_id TEXT PRIMARY KEY NOT NULL,\n"
    "  start_with_leaf INTEGER NOT NULL DEFAULT 0,\n"
    "  session_run INTEGER NOT NULL DEFAULT 0,\n"
    "  last_transition_at_us INTEGER NOT NULL DEFAULT 0,\n"
    "  last_transition_reason TEXT NOT NULL DEFAULT '',\n"
    "  has_last_exit INTEGER NOT NULL DEFAULT 0,\n"
    "  last_exit_code INTEGER NOT NULL DEFAULT 0,\n"
    "  last_exit_at_us INTEGER NOT NULL DEFAULT 0,\n"
    "  restart_count INTEGER NOT NULL DEFAULT 0,\n"
    "  breaker_open INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_count INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_1_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_2_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_3_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_4_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_5_us INTEGER NOT NULL DEFAULT 0,\n"
    "  installed_package_id TEXT NOT NULL DEFAULT '',\n"
    "  installed_package_version TEXT NOT NULL DEFAULT ''\n"
    ");\n"
    "PRAGMA user_version = 1;\n";

static void jw__control_set_reason(char *reason, size_t reason_size, const char *value) {
    if (!reason || reason_size == 0) {
        return;
    }
    size_t n = strlen(value);
    if (n >= reason_size) {
        n = reason_size - 1;
    }
    memcpy(reason, value, n);
    reason[n] = '\0';
}

/* Returns the length of the NUL-terminated string in `buf` (whose
 * declared array size is `buf_size`), or -1 if no NUL terminator exists
 * within the first buf_size bytes -- guards every strlen() this module
 * would otherwise perform on a caller-supplied fixed buffer. */
static long jw__control_bounded_strlen(const char *buf, size_t buf_size) {
    for (size_t i = 0; i < buf_size; i++) {
        if (buf[i] == '\0') {
            return (long)i;
        }
    }
    return -1;
}

bool jw_svc_control_store_open(const char *db_path, jw_svc_control_store **out,
                                char *reason, size_t reason_size) {
    if (out) {
        *out = NULL;
    }
    if (!db_path || !db_path[0] || !out) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        jw__control_set_reason(reason, reason_size, "open-failed");
        return false;
    }
    sqlite3_busy_timeout(db, 2000);

    char *errmsg = NULL;
    if (sqlite3_exec(db, JW__CONTROL_SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        sqlite3_close(db);
        jw__control_set_reason(reason, reason_size, "open-failed");
        return false;
    }

    jw_svc_control_store *store = (jw_svc_control_store *)malloc(sizeof(*store));
    if (!store) {
        sqlite3_close(db);
        jw__control_set_reason(reason, reason_size, "open-failed");
        return false;
    }
    store->db = db;
    *out = store;
    return true;
}

void jw_svc_control_store_close(jw_svc_control_store *store) {
    if (!store) {
        return;
    }
    sqlite3_close(store->db);
    free(store);
}

bool jw_svc_control_store_clear_all_sessions(jw_svc_control_store *store,
                                              char *reason, size_t reason_size) {
    if (!store) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    char *errmsg = NULL;
    if (sqlite3_exec(store->db, "UPDATE service_control_state SET session_run = 0;",
                      NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    return true;
}

static bool jw__control_service_id_is_valid(const char *service_id) {
    if (!service_id || !service_id[0]) {
        return false;
    }
    size_t len = strlen(service_id);
    return len <= (size_t)JW_SVC_CONTROL_ID_MAX;
}

bool jw_svc_control_store_get(jw_svc_control_store *store, const char *service_id,
                               jw_svc_control_state *out, bool *out_found,
                               char *reason, size_t reason_size) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (out_found) {
        *out_found = false;
    }
    if (!store || !out || !out_found || !jw__control_service_id_is_valid(service_id)) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    static const char *const kSql =
        "SELECT start_with_leaf, session_run, last_transition_at_us,\n"
        "       last_transition_reason, has_last_exit, last_exit_code,\n"
        "       last_exit_at_us, restart_count, breaker_open,\n"
        "       backoff_failure_count, backoff_failure_time_1_us,\n"
        "       backoff_failure_time_2_us, backoff_failure_time_3_us,\n"
        "       backoff_failure_time_4_us, backoff_failure_time_5_us,\n"
        "       installed_package_id, installed_package_version\n"
        "  FROM service_control_state WHERE service_id = ?1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, kSql, -1, &stmt, NULL) != SQLITE_OK) {
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }
    sqlite3_bind_text(stmt, 1, service_id, -1, SQLITE_STATIC);

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return true; /* out_found already false, out already zeroed */
    }
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }

    out->start_with_leaf = sqlite3_column_int(stmt, 0) != 0;
    out->session_run = sqlite3_column_int(stmt, 1) != 0;
    out->last_transition_at_us = sqlite3_column_int64(stmt, 2);

    const unsigned char *reason_text = sqlite3_column_text(stmt, 3);
    const char *reason_str = reason_text ? (const char *)reason_text : "";
    size_t reason_len = strlen(reason_str);
    if (reason_len > JW_SVC_CONTROL_REASON_MAX) {
        reason_len = JW_SVC_CONTROL_REASON_MAX;
    }
    memcpy(out->last_transition_reason, reason_str, reason_len);
    out->last_transition_reason[reason_len] = '\0';

    out->has_last_exit = sqlite3_column_int(stmt, 4) != 0;
    out->last_exit_code = sqlite3_column_int(stmt, 5);
    out->last_exit_at_us = sqlite3_column_int64(stmt, 6);
    out->restart_count = sqlite3_column_int(stmt, 7);
    out->breaker_open = sqlite3_column_int(stmt, 8) != 0;

    int backoff_count = sqlite3_column_int(stmt, 9);
    if (backoff_count < 0) {
        backoff_count = 0;
    } else if (backoff_count > JW_SVC_CONTROL_BACKOFF_TRACKED) {
        backoff_count = JW_SVC_CONTROL_BACKOFF_TRACKED;
    }
    out->backoff_failure_count = backoff_count;
    for (int i = 0; i < JW_SVC_CONTROL_BACKOFF_TRACKED; i++) {
        out->backoff_failure_times_us[i] = sqlite3_column_int64(stmt, 10 + i);
    }

    const unsigned char *pkg_id_text = sqlite3_column_text(stmt, 15);
    const char *pkg_id_str = pkg_id_text ? (const char *)pkg_id_text : "";
    size_t pkg_id_len = strlen(pkg_id_str);
    if (pkg_id_len > JW_SVC_CONTROL_PACKAGE_ID_MAX) {
        pkg_id_len = JW_SVC_CONTROL_PACKAGE_ID_MAX;
    }
    memcpy(out->installed_package_id, pkg_id_str, pkg_id_len);
    out->installed_package_id[pkg_id_len] = '\0';

    const unsigned char *pkg_ver_text = sqlite3_column_text(stmt, 16);
    const char *pkg_ver_str = pkg_ver_text ? (const char *)pkg_ver_text : "";
    size_t pkg_ver_len = strlen(pkg_ver_str);
    if (pkg_ver_len > JW_SVC_CONTROL_PACKAGE_VERSION_MAX) {
        pkg_ver_len = JW_SVC_CONTROL_PACKAGE_VERSION_MAX;
    }
    memcpy(out->installed_package_version, pkg_ver_str, pkg_ver_len);
    out->installed_package_version[pkg_ver_len] = '\0';

    sqlite3_finalize(stmt);
    *out_found = true;
    return true;
}

bool jw_svc_control_store_put(jw_svc_control_store *store, const char *service_id,
                               const jw_svc_control_state *state,
                               char *reason, size_t reason_size) {
    if (!store || !state || !jw__control_service_id_is_valid(service_id)) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (state->backoff_failure_count < 0 ||
        state->backoff_failure_count > JW_SVC_CONTROL_BACKOFF_TRACKED) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (jw__control_bounded_strlen(state->last_transition_reason,
                                    sizeof(state->last_transition_reason)) < 0 ||
        jw__control_bounded_strlen(state->installed_package_id,
                                    sizeof(state->installed_package_id)) < 0 ||
        jw__control_bounded_strlen(state->installed_package_version,
                                    sizeof(state->installed_package_version)) < 0) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    static const char *const kSql =
        "INSERT INTO service_control_state (\n"
        "  service_id, start_with_leaf, session_run, last_transition_at_us,\n"
        "  last_transition_reason, has_last_exit, last_exit_code,\n"
        "  last_exit_at_us, restart_count, breaker_open,\n"
        "  backoff_failure_count, backoff_failure_time_1_us,\n"
        "  backoff_failure_time_2_us, backoff_failure_time_3_us,\n"
        "  backoff_failure_time_4_us, backoff_failure_time_5_us,\n"
        "  installed_package_id, installed_package_version\n"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18)\n"
        "ON CONFLICT(service_id) DO UPDATE SET\n"
        "  start_with_leaf = excluded.start_with_leaf,\n"
        "  session_run = excluded.session_run,\n"
        "  last_transition_at_us = excluded.last_transition_at_us,\n"
        "  last_transition_reason = excluded.last_transition_reason,\n"
        "  has_last_exit = excluded.has_last_exit,\n"
        "  last_exit_code = excluded.last_exit_code,\n"
        "  last_exit_at_us = excluded.last_exit_at_us,\n"
        "  restart_count = excluded.restart_count,\n"
        "  breaker_open = excluded.breaker_open,\n"
        "  backoff_failure_count = excluded.backoff_failure_count,\n"
        "  backoff_failure_time_1_us = excluded.backoff_failure_time_1_us,\n"
        "  backoff_failure_time_2_us = excluded.backoff_failure_time_2_us,\n"
        "  backoff_failure_time_3_us = excluded.backoff_failure_time_3_us,\n"
        "  backoff_failure_time_4_us = excluded.backoff_failure_time_4_us,\n"
        "  backoff_failure_time_5_us = excluded.backoff_failure_time_5_us,\n"
        "  installed_package_id = excluded.installed_package_id,\n"
        "  installed_package_version = excluded.installed_package_version;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, kSql, -1, &stmt, NULL) != SQLITE_OK) {
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    sqlite3_bind_text(stmt, 1, service_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, state->start_with_leaf ? 1 : 0);
    sqlite3_bind_int(stmt, 3, state->session_run ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, state->last_transition_at_us);
    sqlite3_bind_text(stmt, 5, state->last_transition_reason, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 6, state->has_last_exit ? 1 : 0);
    sqlite3_bind_int(stmt, 7, state->last_exit_code);
    sqlite3_bind_int64(stmt, 8, state->last_exit_at_us);
    sqlite3_bind_int(stmt, 9, state->restart_count);
    sqlite3_bind_int(stmt, 10, state->breaker_open ? 1 : 0);
    sqlite3_bind_int(stmt, 11, state->backoff_failure_count);
    for (int i = 0; i < JW_SVC_CONTROL_BACKOFF_TRACKED; i++) {
        long long value = (i < state->backoff_failure_count) ? state->backoff_failure_times_us[i] : 0;
        sqlite3_bind_int64(stmt, 12 + i, value);
    }
    sqlite3_bind_text(stmt, 17, state->installed_package_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 18, state->installed_package_version, -1, SQLITE_STATIC);

    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    return true;
}
