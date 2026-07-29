/* Integration tests for internal/services/supervisor.c: real fixture paks
 * on disk, real forked processes, real leases, real stop sequences. This is
 * the first suite in the series that runs a supervised service END TO END,
 * not an isolated primitive.
 *
 * flock()/kill()/usleep()/mkdtemp()/PATH_MAX need a broader feature-test
 * macro than bare -std=c11 on glibc. Must precede every #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/supervisor.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

typedef struct {
    char root[PATH_MAX];
    char runtime[PATH_MAX];
    char logs[PATH_MAX];
    char state[PATH_MAX];
    char userdata[PATH_MAX];
    char apps_primary[PATH_MAX];
    const char *scan_roots[2];
} fixture;

static void jw__mkdir(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

/* gcc -Wformat-truncation rejects snprintf(dst, sizeof dst, "%s/...", root)
 * because root is an unbounded PATH_MAX array; join with explicit bounds. */
static void jw__join(char *out, size_t out_size, const char *dir,
                     const char *leaf) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    strncat(out, dir ? dir : "", out_size - 1u);
    size_t len = strlen(out);
    if (len < out_size - 1u) {
        strncat(out, "/", out_size - 1u - len);
    }
    len = strlen(out);
    if (len < out_size - 1u) {
        strncat(out, leaf ? leaf : "", out_size - 1u - len);
    }
}

/* Three-component variant: dir/sub/leaf. */
static void jw__join3(char *out, size_t out_size, const char *dir,
                      const char *sub, const char *leaf) {
    char mid[PATH_MAX];
    jw__join(mid, sizeof(mid), dir, sub);
    jw__join(out, out_size, mid, leaf);
}

static void fixture_setup(fixture *f) {
    snprintf(f->root, sizeof(f->root), "/tmp/jw-supv-test-XXXXXX");
    if (!mkdtemp(f->root)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        exit(2);
    }
    jw__join(f->runtime, sizeof(f->runtime), f->root, "runtime");
    jw__join(f->logs, sizeof(f->logs), f->root, "logs");
    jw__join(f->state, sizeof(f->state), f->root, "state");
    jw__join(f->userdata, sizeof(f->userdata), f->root, "userdata");
    jw__join(f->apps_primary, sizeof(f->apps_primary), f->root, "apps");
    jw__mkdir(f->runtime);
    jw__mkdir(f->logs);
    jw__mkdir(f->state);
    jw__mkdir(f->userdata);
    jw__mkdir(f->apps_primary);
    f->scan_roots[0] = f->apps_primary;
    f->scan_roots[1] = NULL;
}

static void fixture_teardown(fixture *f) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->root);
    if (system(cmd) != 0) {
        fprintf(stderr, "warning: could not remove %s\n", f->root);
    }
}

static void fixture_write_pak(fixture *f, const char *pak_dir_name,
                              const char *service_id, const char *script_body,
                              const char *extra_manifest_fields) {
    char pak[PATH_MAX];
    jw__join(pak, sizeof(pak), f->apps_primary, pak_dir_name);
    jw__mkdir(pak);
    char bin[PATH_MAX];
    jw__join(bin, sizeof(bin), pak, "bin");
    jw__mkdir(bin);

    char run[PATH_MAX];
    jw__join3(run, sizeof(run), pak, "bin", "run.sh");
    FILE *fp = fopen(run, "w");
    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", run, strerror(errno));
        exit(2);
    }
    fprintf(fp, "#!/bin/sh\n%s\n", script_body);
    fclose(fp);
    if (chmod(run, 0700) != 0) {
        fprintf(stderr, "chmod %s: %s\n", run, strerror(errno));
        exit(2);
    }

    char pj[PATH_MAX];
    jw__join(pj, sizeof(pj), pak, "pak.json");
    fp = fopen(pj, "w");
    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", pj, strerror(errno));
        exit(2);
    }
    fprintf(fp,
            "{\"id\":\"%s\",\"name\":\"%s\",\"platform\":\"%s\","
            "\"service\":{\"schema\":1,\"id\":\"%s\","
            "\"run\":{\"path\":\"bin/run.sh\",\"args\":[]},"
            "\"default_enabled\":false,"
            "\"stop_grace_ms\":300%s}}\n",
            service_id, pak_dir_name,
            getenv("PLATFORM") ? getenv("PLATFORM") : "mac",
            service_id, extra_manifest_fields ? extra_manifest_fields : "");
    fclose(fp);
}

static jw_svc_supervisor *fixture_open(fixture *f, char *reason) {
    return jw_svc_supervisor_open(f->runtime, f->logs, f->state,
                                  f->scan_roots, f->userdata, reason,
                                  JW_SVC_REASON_BUF);
}

static bool wait_for_state(jw_svc_supervisor *sup, const char *id,
                           jw_svc_effective_state want, int timeout_ms) {
    int waited = 0;
    const int step = 20;
    while (waited <= timeout_ms) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
        if (e && e->state == want) {
            return true;
        }
        usleep((useconds_t)step * 1000u);
        waited += step;
    }
    return false;
}

static void test_clean_exit(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "clean.pak", "org.umrk.test.clean", "exit 0", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);

    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.clean");
    CHECK(e != NULL);
    CHECK(e->manifest_valid);
    CHECK(e->state == JW_SVC_STATE_DISABLED);

    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.clean", reason, sizeof(reason)));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.clean");
    CHECK(e->pgid > 0);

    bool settled = wait_for_state(sup, "org.umrk.test.clean", JW_SVC_STATE_BACKOFF, 5000) ||
                   wait_for_state(sup, "org.umrk.test.clean", JW_SVC_STATE_STOPPED, 1000) ||
                   wait_for_state(sup, "org.umrk.test.clean", JW_SVC_STATE_DISABLED, 1000);
    CHECK(settled);
    e = jw_svc_supervisor_find(sup, "org.umrk.test.clean");
    CHECK(e->pgid <= 0);
    CHECK(e->lease_fd < 0);
    CHECK(e->control.has_last_exit);
    CHECK(e->control.last_exit_code == 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_stop_sequence(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "sleeper.pak", "org.umrk.test.sleeper",
                      "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.sleeper", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.sleeper", JW_SVC_STATE_RUNNING, 3000));

    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.sleeper", reason, sizeof(reason)));
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.sleeper");
    CHECK(e->pgid <= 0);
    CHECK(e->state == JW_SVC_STATE_STOPPED || e->state == JW_SVC_STATE_DISABLED);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_ignore_term_escalates(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "ignorer.pak", "org.umrk.test.ignorer",
                      "trap '' TERM; while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.ignorer", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.ignorer", JW_SVC_STATE_RUNNING, 3000));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.ignorer", reason, sizeof(reason)));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long elapsed_ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000LL +
                           (long long)(t1.tv_nsec - t0.tv_nsec) / 1000000LL;

    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.ignorer");
    CHECK(e->pgid <= 0);
    CHECK(elapsed_ms <= 300 + 2000 + 250);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_orphan_descendant(void) {
    fixture f;
    fixture_setup(&f);
    /* The leader forks a descendant that IGNORES SIGTERM and exits itself.
     * restart is "no" so the supervisor does not start a replacement over
     * the still-live group: this isolates the reservation rule -- the
     * zombie leader holds the pgid, the orphan is a live member, and the
     * group must stay not-absent until the orphan is KILLed and reaped. */
    fixture_write_pak(&f, "orphan.pak", "org.umrk.test.orphan",
                      "( trap '' TERM; sleep 300 ) & exit 0",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.orphan", reason, sizeof(reason)));

    bool noticed = false;
    for (int i = 0; i < 200 && !noticed; i++) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.orphan");
        if (e && e->state == JW_SVC_STATE_STOPPING && e->reap_pending) {
            noticed = true;
        }
        usleep(20000);
    }
    CHECK(noticed);

    bool reaped = false;
    for (int i = 0; i < 400 && !reaped; i++) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.orphan");
        if (e && e->pgid <= 0) {
            reaped = true;
        }
        usleep(20000);
    }
    CHECK(reaped);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_persistent_vs_session_across_restart(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "sleeper.pak", "org.umrk.test.persist",
                      "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.persist", reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.persist", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.persist", JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);

    jw_svc_supervisor *sup2 = fixture_open(&f, reason);
    CHECK(sup2 != NULL);
    CHECK(jw_svc_supervisor_scan(sup2) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup2, "org.umrk.test.persist");
    CHECK(e != NULL);
    CHECK(e->desired_enabled);
    CHECK(!e->session_run);

    CHECK(wait_for_state(sup2, "org.umrk.test.persist", JW_SVC_STATE_RUNNING, 5000));
    CHECK(jw_svc_supervisor_stop_all(sup2) == 0);
    jw_svc_supervisor_close(sup2);
    fixture_teardown(&f);
}

static void test_invalid_manifest_isolated(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "good.pak", "org.umrk.test.good",
                      "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");
    char pak[PATH_MAX];
    jw__join(pak, sizeof(pak), f.apps_primary, "bad.pak");
    jw__mkdir(pak);
    /* A real executable so run.path passes and the ONLY rejection is the
     * top-level-id vs service.id mismatch. */
    char bin[PATH_MAX];
    jw__join(bin, sizeof(bin), pak, "bin");
    jw__mkdir(bin);
    char run[PATH_MAX];
    jw__join3(run, sizeof(run), pak, "bin", "run.sh");
    FILE *rf = fopen(run, "w");
    fprintf(rf, "#!/bin/sh\nexit 0\n");
    fclose(rf);
    chmod(run, 0700);
    char pj[PATH_MAX];
    jw__join(pj, sizeof(pj), pak, "pak.json");
    FILE *fp = fopen(pj, "w");
    fprintf(fp, "{\"id\":\"org.umrk.test.bad\",\"platform\":\"mac\","
                "\"service\":{\"schema\":1,\"id\":\"org.umrk.other\","
                "\"run\":{\"path\":\"bin/x\",\"args\":[]}}}\n");
    fclose(fp);

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    const jw_svc_supervised *good = jw_svc_supervisor_find(sup, "org.umrk.test.good");
    /* An invalid service is tracked under its declared service.id and
     * reported unavailable, keyed to the specific rejection reason. */
    const jw_svc_supervised *bad = jw_svc_supervisor_find(sup, "org.umrk.other");
    CHECK(good != NULL && good->manifest_valid);
    CHECK(bad != NULL && !bad->manifest_valid);
    CHECK(strcmp(bad->reject_reason, "id-mismatch") == 0);

    CHECK(!jw_svc_supervisor_run(sup, "org.umrk.other", reason, sizeof(reason)));
    CHECK(strcmp(reason, "unavailable") == 0);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.good", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.good", JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_duplicate_ids_both_unavailable(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "one.pak", "org.umrk.test.dup", "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");
    fixture_write_pak(&f, "two.pak", "org.umrk.test.dup", "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    /* Both duplicate paks carry the same service.id, so they reconcile to a
     * single tracked entry flagged as a duplicate (unavailable). */
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.dup");
    CHECK(e != NULL);
    CHECK(!e->manifest_valid);
    CHECK(strcmp(e->reject_reason, "duplicate-service-id") == 0);
    CHECK(!jw_svc_supervisor_run(sup, "org.umrk.test.dup", reason, sizeof(reason)));

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_stale_generation_blocks_second_daemon(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "leaseholder.pak", "org.umrk.test.lease",
                      "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *first = fixture_open(&f, reason);
    CHECK(first != NULL);
    CHECK(jw_svc_supervisor_scan(first) == 1);
    CHECK(jw_svc_supervisor_run(first, "org.umrk.test.lease", reason, sizeof(reason)));
    CHECK(wait_for_state(first, "org.umrk.test.lease", JW_SVC_STATE_RUNNING, 3000));

    jw_svc_supervisor *second = fixture_open(&f, reason);
    CHECK(second != NULL);
    CHECK(jw_svc_supervisor_scan(second) == 1);
    CHECK(jw_svc_supervisor_enable(second, "org.umrk.test.lease", reason, sizeof(reason)));
    bool went_stale = false;
    for (int i = 0; i < 200 && !went_stale; i++) {
        jw_svc_supervisor_tick(second);
        const jw_svc_supervised *e = jw_svc_supervisor_find(second, "org.umrk.test.lease");
        if (e && e->state == JW_SVC_STATE_STALE_GENERATION) {
            went_stale = true;
        }
        usleep(20000);
    }
    CHECK(went_stale);
    const jw_svc_supervised *e2 = jw_svc_supervisor_find(second, "org.umrk.test.lease");
    CHECK(e2->pgid <= 0);

    CHECK(jw_svc_supervisor_stop_all(first) == 0);
    jw_svc_supervisor_close(first);
    bool started = wait_for_state(second, "org.umrk.test.lease", JW_SVC_STATE_RUNNING, 5000) ||
                   wait_for_state(second, "org.umrk.test.lease", JW_SVC_STATE_STARTING, 2000);
    CHECK(started);
    CHECK(jw_svc_supervisor_stop_all(second) == 0);
    jw_svc_supervisor_close(second);
    fixture_teardown(&f);
}

static void test_circuit_breaker_opens(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "crasher.pak", "org.umrk.test.crasher", "exit 1", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.crasher", reason, sizeof(reason)));

    /* Tick until the breaker opens. Each crash is a failure; collapsing the
     * backoff delay each tick keeps the test fast while still exercising the
     * real 5-failures-in-5-minutes breaker. */
    bool failed = false;
    for (int i = 0; i < 40000 && !failed; i++) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *ro = jw_svc_supervisor_find(sup, "org.umrk.test.crasher");
        if (ro && ro->state == JW_SVC_STATE_FAILED) {
            failed = true;
            break;
        }
        if (ro && ro->state == JW_SVC_STATE_BACKOFF) {
            jw_svc_supervised *m = (jw_svc_supervised *)ro;
            m->backoff_retry_at_ms = 0;
        }
        usleep(1000);
    }
    CHECK(failed);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.crasher");
    CHECK(e->state == JW_SVC_STATE_FAILED);
    CHECK(e->backoff.breaker_open);
    CHECK(e->pgid <= 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

int main(void) {
    test_clean_exit();
    test_stop_sequence();
    test_ignore_term_escalates();
    test_orphan_descendant();
    test_persistent_vs_session_across_restart();
    test_invalid_manifest_isolated();
    test_duplicate_ids_both_unavailable();
    test_stale_generation_blocks_second_daemon();
    test_circuit_breaker_opens();

    if (g_failures == 0) {
        printf("PASS supervisor-test\n");
        return 0;
    }
    fprintf(stderr, "FAIL supervisor-test (%d failures)\n", g_failures);
    return 1;
}
