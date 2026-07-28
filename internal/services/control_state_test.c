#define _GNU_SOURCE

#include "internal/services/control_state.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void jw__test_mkdtemp_db_path(char *out, size_t out_size) {
    char dir[] = "/tmp/jw-control-state-test.XXXXXX";
    assert(mkdtemp(dir) != NULL);
    int n = snprintf(out, out_size, "%s/control.db", dir);
    assert(n > 0 && (size_t)n < out_size);
}

static void jw__test_missing_row_is_not_found_not_error(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    char reason[64] = {0};
    assert(jw_svc_control_store_open(db_path, &store, reason, sizeof(reason)));
    assert(store != NULL);

    jw_svc_control_state out;
    bool found = true;
    memset(reason, 0, sizeof(reason));
    assert(jw_svc_control_store_get(store, "org.umrk.nope", &out, &found, reason, sizeof(reason)));
    assert(!found);
    assert(!out.start_with_leaf && !out.session_run);
    assert(reason[0] == '\0');

    jw_svc_control_store_close(store);
    puts("PASS control-state-test a missing row reports not-found, not an error");
}

static void jw__test_round_trip_every_field(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    state.start_with_leaf = true;
    state.session_run = true;
    state.last_transition_at_us = 1234567890123LL;
    strcpy(state.last_transition_reason, "manual-run");
    state.has_last_exit = true;
    state.last_exit_code = 137;
    state.last_exit_at_us = 999888777LL;
    state.restart_count = 4;
    state.breaker_open = false;
    state.backoff_failure_count = 3;
    state.backoff_failure_times_us[0] = 100;
    state.backoff_failure_times_us[1] = 200;
    state.backoff_failure_times_us[2] = 300;
    strcpy(state.installed_package_id, "org.umrk.syncthing");
    strcpy(state.installed_package_version, "1.2.3");

    assert(jw_svc_control_store_put(store, "org.umrk.syncthing", &state, NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store, "org.umrk.syncthing", &out, &found, NULL, 0));
    assert(found);
    assert(out.start_with_leaf == state.start_with_leaf);
    assert(out.session_run == state.session_run);
    assert(out.last_transition_at_us == state.last_transition_at_us);
    assert(strcmp(out.last_transition_reason, state.last_transition_reason) == 0);
    assert(out.has_last_exit == state.has_last_exit);
    assert(out.last_exit_code == state.last_exit_code);
    assert(out.last_exit_at_us == state.last_exit_at_us);
    assert(out.restart_count == state.restart_count);
    assert(out.breaker_open == state.breaker_open);
    assert(out.backoff_failure_count == state.backoff_failure_count);
    for (int i = 0; i < state.backoff_failure_count; i++) {
        assert(out.backoff_failure_times_us[i] == state.backoff_failure_times_us[i]);
    }
    assert(strcmp(out.installed_package_id, state.installed_package_id) == 0);
    assert(strcmp(out.installed_package_version, state.installed_package_version) == 0);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test round trip preserves every field");
}

static void jw__test_put_overwrites_not_merges(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state first;
    memset(&first, 0, sizeof(first));
    first.start_with_leaf = true;
    first.restart_count = 9;
    assert(jw_svc_control_store_put(store, "org.umrk.a", &first, NULL, 0));

    jw_svc_control_state second;
    memset(&second, 0, sizeof(second));
    second.start_with_leaf = false;
    second.restart_count = 0;
    assert(jw_svc_control_store_put(store, "org.umrk.a", &second, NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store, "org.umrk.a", &out, &found, NULL, 0));
    assert(found);
    assert(!out.start_with_leaf);
    assert(out.restart_count == 0);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test a second put replaces the row instead of merging fields");
}

static void jw__test_clear_all_sessions_only_touches_session_run(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state a;
    memset(&a, 0, sizeof(a));
    a.start_with_leaf = true;
    a.session_run = true;
    a.restart_count = 5;
    assert(jw_svc_control_store_put(store, "org.umrk.a", &a, NULL, 0));

    jw_svc_control_state b;
    memset(&b, 0, sizeof(b));
    b.start_with_leaf = false;
    b.session_run = true;
    assert(jw_svc_control_store_put(store, "org.umrk.b", &b, NULL, 0));

    assert(jw_svc_control_store_clear_all_sessions(store, NULL, 0));

    jw_svc_control_state out_a, out_b;
    bool found_a = false, found_b = false;
    assert(jw_svc_control_store_get(store, "org.umrk.a", &out_a, &found_a, NULL, 0));
    assert(jw_svc_control_store_get(store, "org.umrk.b", &out_b, &found_b, NULL, 0));
    assert(found_a && found_b);
    assert(!out_a.session_run && out_a.start_with_leaf && out_a.restart_count == 5);
    assert(!out_b.session_run && !out_b.start_with_leaf);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test clearing sessions resets session_run without touching persistent fields");
}

static void jw__test_reopen_survives_close(void) {
    /* Simulates a daemon restart: close the handle, reopen the same
     * file, and confirm persistent data (but not session data, once the
     * caller clears it) is still there. */
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store1 = NULL;
    assert(jw_svc_control_store_open(db_path, &store1, NULL, 0));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    state.start_with_leaf = true;
    state.session_run = true;
    state.restart_count = 2;
    strcpy(state.installed_package_id, "org.umrk.persisted");
    assert(jw_svc_control_store_put(store1, "org.umrk.persisted", &state, NULL, 0));
    jw_svc_control_store_close(store1);

    jw_svc_control_store *store2 = NULL;
    assert(jw_svc_control_store_open(db_path, &store2, NULL, 0));
    assert(jw_svc_control_store_clear_all_sessions(store2, NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store2, "org.umrk.persisted", &out, &found, NULL, 0));
    assert(found);
    assert(out.start_with_leaf);
    assert(!out.session_run);
    assert(out.restart_count == 2);
    assert(strcmp(out.installed_package_id, "org.umrk.persisted") == 0);

    jw_svc_control_store_close(store2);
    puts("PASS control-state-test data survives a close/reopen cycle, session_run only clears when asked");
}

static void jw__test_invalid_arguments(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    char reason[64] = {0};

    assert(!jw_svc_control_store_open(NULL, &store, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);
    assert(store == NULL);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_open("", &store, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_get(store, NULL, &state, &(bool){false}, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_get(store, "", &state, &(bool){false}, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_put(store, "org.umrk.a", NULL, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    state.backoff_failure_count = JW_SVC_CONTROL_BACKOFF_TRACKED + 1;
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);
    state.backoff_failure_count = -1;
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason, sizeof(reason)));
    state.backoff_failure_count = 0;

    char oversized_id[JW_SVC_CONTROL_ID_MAX + 2];
    memset(oversized_id, 'a', sizeof(oversized_id) - 1);
    oversized_id[sizeof(oversized_id) - 1] = '\0';
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_put(store, oversized_id, &state, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    /* A string field with no NUL terminator within its fixed buffer must
     * be rejected rather than let a bounded strlen() run past the end. */
    memset(state.last_transition_reason, 'x', sizeof(state.last_transition_reason));
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test rejects invalid arguments across open/get/put");
}

static void jw__test_close_null_is_a_no_op(void) {
    jw_svc_control_store_close(NULL);
    puts("PASS control-state-test closing a NULL store is a no-op");
}

int main(void) {
    jw__test_missing_row_is_not_found_not_error();
    jw__test_round_trip_every_field();
    jw__test_put_overwrites_not_merges();
    jw__test_clear_all_sessions_only_touches_session_run();
    jw__test_reopen_survives_close();
    jw__test_invalid_arguments();
    jw__test_close_null_is_a_no_op();
    puts("PASS control-state-test");
    return 0;
}
