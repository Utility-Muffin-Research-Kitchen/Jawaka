#define _GNU_SOURCE

#include "internal/services/reservation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JW_TEST_REASON_BUF 64

static void jw__test_mkdtemp(const char *suffix, char *out, size_t out_size) {
    int n = snprintf(out, out_size, "/tmp/jw-reservation-test-%s.XXXXXX", suffix);
    assert(n > 0 && (size_t)n < out_size);
    assert(mkdtemp(out) != NULL);
}

static void jw__test_make_service_dir(const char *runtime_dir, const char *service_id,
                                      char *dir_out, size_t dir_out_size) {
    char services_dir[700];
    int n = snprintf(services_dir, sizeof(services_dir), "%s/services", runtime_dir);
    assert(n > 0 && (size_t)n < sizeof(services_dir));
    assert(mkdir(services_dir, 0700) == 0);

    n = snprintf(dir_out, dir_out_size, "%s/%s", services_dir, service_id);
    assert(n > 0 && (size_t)n < dir_out_size);
    assert(mkdir(dir_out, 0700) == 0);
}

static void jw__test_round_trip(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("round-trip", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation written = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_NOTIFY,
        .stop_on_storage_change = true,
        .stop_on_suspend = false,
    };

    char reason[JW_TEST_REASON_BUF] = {0};
    assert(jw_svc_reservation_write(runtime_dir, "org.umrk.test", &written,
                                    reason, sizeof(reason)));

    /* The temp file must not linger after a successful write -- rename()
     * replaces it in place. */
    char temp_path[700];
    snprintf(temp_path, sizeof(temp_path), "%s/.reservation.tmp", service_dir);
    struct stat st;
    assert(lstat(temp_path, &st) != 0);

    jw_svc_reservation read_back;
    memset(reason, 0, sizeof(reason));
    assert(jw_svc_reservation_read(runtime_dir, "org.umrk.test", &read_back,
                                   reason, sizeof(reason)));
    assert(read_back.pgid == written.pgid);
    assert(read_back.launch_instant_us == written.launch_instant_us);
    assert(read_back.game_policy == written.game_policy);
    assert(read_back.stop_on_storage_change == written.stop_on_storage_change);
    assert(read_back.stop_on_suspend == written.stop_on_suspend);

    puts("PASS reservation-test round trip preserves every field");
}

static void jw__test_every_game_policy_round_trips(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("policies", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation_game_policy policies[] = {
        JW_SVC_RESERVATION_GAME_IGNORE,
        JW_SVC_RESERVATION_GAME_STOP,
        JW_SVC_RESERVATION_GAME_NOTIFY,
    };
    for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); i++) {
        jw_svc_reservation written = {
            .pgid = 100 + (pid_t)i,
            .launch_instant_us = jw_svc_reservation_now_us(),
            .game_policy = policies[i],
            .stop_on_storage_change = (i % 2) == 0,
            .stop_on_suspend = (i % 2) == 1,
        };
        char reason[JW_TEST_REASON_BUF] = {0};
        assert(jw_svc_reservation_write(runtime_dir, "org.umrk.test", &written,
                                        reason, sizeof(reason)));

        jw_svc_reservation read_back;
        assert(jw_svc_reservation_read(runtime_dir, "org.umrk.test", &read_back,
                                       reason, sizeof(reason)));
        assert(read_back.game_policy == written.game_policy);
        assert(read_back.pgid == written.pgid);
        assert(read_back.stop_on_storage_change == written.stop_on_storage_change);
        assert(read_back.stop_on_suspend == written.stop_on_suspend);
    }

    puts("PASS reservation-test every game policy round-trips, replacement overwrites cleanly");
}

static void jw__test_missing_vs_corrupt(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("missing-corrupt", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation out;
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_read(runtime_dir, "org.umrk.test", &out, reason, sizeof(reason)));
    assert(strcmp(reason, "reservation-missing") == 0);
    /* *out must be zeroed on failure, not left with garbage a careless
     * caller might mistake for a real record. */
    assert(out.pgid == 0);

    char reservation_path[700];
    snprintf(reservation_path, sizeof(reservation_path), "%s/reservation", service_dir);
    int fd = open(reservation_path, O_WRONLY | O_CREAT, 0600);
    assert(fd >= 0);
    assert(write(fd, "{not json", 9) == 9);
    assert(close(fd) == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_read(runtime_dir, "org.umrk.test", &out, reason, sizeof(reason)));
    assert(strcmp(reason, "reservation-corrupt") == 0);

    /* Valid JSON, wrong shape (missing fields) must also be corrupt, not
     * silently accepted with zeroed/default fields. */
    fd = open(reservation_path, O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);
    static const char *shape_wrong = "{\"pgid\":4242}";
    assert(write(fd, shape_wrong, strlen(shape_wrong)) == (ssize_t)strlen(shape_wrong));
    assert(close(fd) == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_read(runtime_dir, "org.umrk.test", &out, reason, sizeof(reason)));
    assert(strcmp(reason, "reservation-corrupt") == 0);

    /* An unknown "game" value must also be corrupt, not silently mapped
     * to some default policy. */
    fd = open(reservation_path, O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);
    static const char *unknown_game =
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"sleep\","
        "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}";
    assert(write(fd, unknown_game, strlen(unknown_game)) == (ssize_t)strlen(unknown_game));
    assert(close(fd) == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_read(runtime_dir, "org.umrk.test", &out, reason, sizeof(reason)));
    assert(strcmp(reason, "reservation-corrupt") == 0);

    puts("PASS reservation-test distinguishes missing from corrupt, "
         "rejects wrong-shape and unknown-policy records");
}

static void jw__test_write_without_service_dir(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("no-service-dir", runtime_dir, sizeof(runtime_dir));

    jw_svc_reservation reservation = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = false,
        .stop_on_suspend = false,
    };
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test", &reservation,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "service-dir-unavailable") == 0);

    puts("PASS reservation-test refuses to write without an existing service directory");
}

static void jw__test_invalid_arguments(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("invalid-args", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation reservation = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = false,
        .stop_on_suspend = false,
    };
    char reason[JW_TEST_REASON_BUF] = {0};

    assert(!jw_svc_reservation_write(runtime_dir, "a/b", &reservation, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_write(runtime_dir, "..", &reservation, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    jw_svc_reservation zero_pgid = reservation;
    zero_pgid.pgid = 0;
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test", &zero_pgid,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_write(NULL, "org.umrk.test", &reservation,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    jw_svc_reservation out;
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_read(runtime_dir, "a/b", &out, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    puts("PASS reservation-test rejects invalid arguments on both write and read");
}

int main(void) {
    jw__test_round_trip();
    jw__test_every_game_policy_round_trips();
    jw__test_missing_vs_corrupt();
    jw__test_write_without_service_dir();
    jw__test_invalid_arguments();
    puts("PASS reservation-test");
    return 0;
}
