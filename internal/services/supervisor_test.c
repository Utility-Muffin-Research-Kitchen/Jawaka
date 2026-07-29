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
#include "internal/services/lease.h"
#include "internal/services/reservation.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
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
static const char *g_program_path = NULL;
extern char **environ;

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
            "\"pak_version\":\"1.2.3\","
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

    CHECK(wait_for_state(sup, "org.umrk.test.clean",
                         JW_SVC_STATE_DISABLED, 3000));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.clean");
    CHECK(e->pgid <= 0);
    CHECK(e->lease_fd < 0);
    CHECK(e->control.has_last_exit);
    CHECK(e->control.last_exit_code == 0);
    CHECK(e->backoff.count == 0);
    CHECK(!e->backoff.breaker_open);

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
    const jw_svc_supervised *started =
        jw_svc_supervisor_find(sup, "org.umrk.test.orphan");
    pid_t leader = started ? started->pgid : -1;

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

    /* Observing the exit must leave the leader waitable: waitpid(WNOHANG)
     * would have consumed it and freed the pgid before the descendant was
     * verified absent. WNOWAIT confirms the zombie reservation still exists. */
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    errno = 0;
    int wait_rc = waitid(P_PID, (id_t)leader, &info,
                         WEXITED | WNOHANG | WNOWAIT);
    CHECK(wait_rc == 0);
    CHECK(info.si_pid == leader);

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
    for (int i = 0; i < 75; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    const jw_svc_supervised *enabled =
        jw_svc_supervisor_find(sup, "org.umrk.test.persist");
    CHECK(enabled != NULL && enabled->pgid <= 0);
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

    /* A malformed manifest using the valid service's exact id must not win
     * reconciliation based on readdir() order. */
    fixture_write_pak(&f, "attacker.pak", "org.umrk.test.attacker",
                      "exit 0", NULL);
    jw__join(pak, sizeof(pak), f.apps_primary, "attacker.pak");
    jw__join(pj, sizeof(pj), pak, "pak.json");
    fp = fopen(pj, "w");
    CHECK(fp != NULL);
    if (fp) {
        fprintf(fp,
                "{\"id\":\"org.umrk.test.attacker\",\"platform\":\"mac\","
                "\"service\":{\"schema\":1,\"id\":\"org.umrk.test.good\","
                "\"run\":{\"path\":\"bin/run.sh\",\"args\":[]}}}\n");
        fclose(fp);
    }

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
    CHECK(bad != NULL && bad->state == JW_SVC_STATE_UNAVAILABLE);
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
                      "while :; do sleep 1; done",
                      ",\"restart\":\"on-failure\","
                      "\"lifecycle\":{\"game\":\"stop\"}");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *first = fixture_open(&f, reason);
    CHECK(first != NULL);
    CHECK(jw_svc_supervisor_scan(first) == 1);
    CHECK(jw_svc_supervisor_run(first, "org.umrk.test.lease", reason, sizeof(reason)));
    CHECK(wait_for_state(first, "org.umrk.test.lease", JW_SVC_STATE_RUNNING, 3000));

    jw_svc_supervisor *second = fixture_open(&f, reason);
    CHECK(second != NULL);
    CHECK(jw_svc_supervisor_scan(second) == 1);
    const jw_svc_supervised *e2 =
        jw_svc_supervisor_find(second, "org.umrk.test.lease");
    CHECK(e2 != NULL && e2->state == JW_SVC_STATE_STALE_GENERATION);
    CHECK(e2 != NULL && !e2->desired_enabled && !e2->session_run);
    char stuck[JW_SVC_SUPERVISOR_ID_BUF];
    CHECK(jw_svc_supervisor_game_launch_begin(second, stuck,
                                               sizeof(stuck)) == 0);
    CHECK(strcmp(stuck, "org.umrk.test.lease") == 0);
    CHECK(jw_svc_supervisor_stop_all(second) == 1);

    CHECK(!jw_svc_supervisor_run(second, "org.umrk.test.lease",
                                 reason, sizeof(reason)));
    CHECK(strcmp(reason, "stale-generation") == 0);
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
    e2 = jw_svc_supervisor_find(second, "org.umrk.test.lease");
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

static int run_descriptor_zero_worker(const char *root) {
    g_failures = 0;

    fixture f;
    memset(&f, 0, sizeof(f));
    snprintf(f.root, sizeof(f.root), "%s", root);
    jw__join(f.runtime, sizeof(f.runtime), f.root, "runtime");
    jw__join(f.logs, sizeof(f.logs), f.root, "logs");
    jw__join(f.state, sizeof(f.state), f.root, "state");
    jw__join(f.userdata, sizeof(f.userdata), f.root, "userdata");
    jw__join(f.apps_primary, sizeof(f.apps_primary), f.root, "apps");
    f.scan_roots[0] = f.apps_primary;
    f.scan_roots[1] = NULL;

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);

    CHECK(close(STDIN_FILENO) == 0);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.fdzero",
                                reason, sizeof(reason)));
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.fdzero");
    CHECK(e != NULL && e->lease_fd == STDIN_FILENO);
    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.fdzero",
                                 reason, sizeof(reason)));
    char ready[PATH_MAX];
    jw__join(ready, sizeof(ready), f.root, "fdzero-ready");
    FILE *ready_fp = fopen(ready, "w");
    CHECK(ready_fp != NULL);
    if (ready_fp) fclose(ready_fp);

    /* Stay alive with the supervisor open so the parent can distinguish a
     * genuinely closed descriptor 0 from a lock released only by process
     * exit. */
    sleep(3);

    jw_svc_supervisor_close(sup);
    return g_failures == 0 ? 0 : 1;
}

static void test_descriptor_zero_lease_is_released(void) {
    /* Spawn with an independent /dev/null stdin. Closing the suite runner's
     * harness pipe itself can be interpreted by some runners as completion,
     * which would hide the lease assertion behind an external SIGKILL. */
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "fdzero.pak", "org.umrk.test.fdzero",
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");

    posix_spawn_file_actions_t actions;
    CHECK(posix_spawn_file_actions_init(&actions) == 0);
    CHECK(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                           O_RDONLY, (mode_t)0) == 0);
    char *const argv[] = {(char *)g_program_path,
                          (char *)"--fd-zero-worker", f.root, NULL};
    pid_t worker = -1;
    int spawn_rc = posix_spawn(&worker, g_program_path, &actions, NULL,
                               argv, environ);
    CHECK(posix_spawn_file_actions_destroy(&actions) == 0);
    CHECK(spawn_rc == 0);
    if (spawn_rc != 0) {
        fixture_teardown(&f);
        return;
    }
    char ready[PATH_MAX];
    jw__join(ready, sizeof(ready), f.root, "fdzero-ready");
    bool worker_ready = false;
    for (int i = 0; i < 300 && !worker_ready; i++) {
        worker_ready = access(ready, F_OK) == 0;
        if (!worker_ready) usleep(10000);
    }
    CHECK(worker_ready);
    char reason[JW_SVC_REASON_BUF];
    int probe = jw_svc_lease_acquire(f.runtime, "org.umrk.test.fdzero",
                                     reason, sizeof(reason));
    CHECK(probe >= 0);
    if (probe >= 0) close(probe);
    int status = 0;
    CHECK(waitpid(worker, &status, 0) == worker);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fixture_teardown(&f);
}

static void test_enabled_stop_is_not_immediately_undone(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "stopenabled.pak", "org.umrk.test.stopenabled",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.stopenabled",
                                   reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.stopenabled",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.stopenabled",
                         JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.stopenabled",
                                 reason, sizeof(reason)));
    for (int i = 0; i < 100; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.stopenabled");
    CHECK(e != NULL && e->desired_enabled);
    CHECK(e != NULL && e->pgid <= 0);
    CHECK(e != NULL && e->state == JW_SVC_STATE_STOPPED);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_running_policy_survives_rescan(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "policy.pak", "org.umrk.test.policy",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\","
                      "\"lifecycle\":{\"game\":\"stop\"}");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.policy",
                                   reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.policy",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.policy",
                         JW_SVC_STATE_RUNNING, 3000));

    /* A package rescan may install a new policy for the NEXT generation,
     * but the owned group's reserved launch-time policy remains "stop". */
    fixture_write_pak(&f, "policy.pak", "org.umrk.test.policy",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\","
                      "\"lifecycle\":{\"game\":\"ignore\"}");
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.policy");
    CHECK(e != NULL &&
          e->manifest.lifecycle_game == JW_SVC_LIFECYCLE_GAME_IGNORE);
    CHECK(e != NULL &&
          e->active_lifecycle_game == JW_SVC_LIFECYCLE_GAME_STOP);

    char stuck[JW_SVC_SUPERVISOR_ID_BUF];
    CHECK(jw_svc_supervisor_game_launch_begin(sup, stuck, sizeof(stuck)) == 1);
    CHECK(stuck[0] == '\0');
    for (int i = 0; i < 100; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    e = jw_svc_supervisor_find(sup, "org.umrk.test.policy");
    CHECK(e != NULL && e->pgid <= 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_reservation_write_failure_reaps_child(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.reservationfail";
    fixture_write_pak(&f, "reservationfail.pak", id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);

    int lease = jw_svc_lease_acquire(f.runtime, id, reason, sizeof(reason));
    CHECK(lease >= 0);
    if (lease >= 0) close(lease);
    char service_dir[PATH_MAX];
    jw__join3(service_dir, sizeof(service_dir), f.runtime, "services", id);
    char reservation_path[PATH_MAX];
    jw__join(reservation_path, sizeof(reservation_path), service_dir,
             "reservation");
    jw__mkdir(reservation_path); /* rename(temp, reservation) must fail */

    CHECK(!jw_svc_supervisor_run(sup, id, reason, sizeof(reason)));
    CHECK(strcmp(reason, "reservation-failed") == 0);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    CHECK(e != NULL && e->pgid <= 0);
    CHECK(e != NULL && e->lease_fd < 0);

    /* No child may remain waitable. The buggy cleanup proved absence but
     * forgot the final reap, so this call returned the leaked zombie pid. */
    errno = 0;
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_hostile_id_tail_bound_and_status_shape(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "evil.pak", "../../escape", "exit 0", NULL);
    fixture_write_pak(&f, "status.pak", "org.umrk.test.status",
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    CHECK(jw_svc_supervisor_find(sup, "../../escape") == NULL);
    const jw_svc_supervised *evil = jw_svc_supervisor_find(sup, "evil");
    CHECK(evil != NULL && evil->state == JW_SVC_STATE_UNAVAILABLE);
    CHECK(!jw_svc_supervisor_service_id_is_safe("../../escape"));
    CHECK(jw_svc_supervisor_service_id_is_safe("org.umrk.test.status"));
    CHECK(jw_svc_supervisor_bound_log_tail(-1) == JW_SVC_LOG_TAIL_DEFAULT);
    CHECK(jw_svc_supervisor_bound_log_tail(1) == 1);
    CHECK(jw_svc_supervisor_bound_log_tail(INT_MAX) == JW_SVC_LOG_TAIL_MAX);
    char scan_root[32];
    CHECK(jw_svc_supervisor_join_scan_root(scan_root, sizeof(scan_root),
                                            "/Apps", "mac"));
    CHECK(strcmp(scan_root, "/Apps/mac") == 0);
    CHECK(!jw_svc_supervisor_join_scan_root(scan_root, sizeof(scan_root),
                                             NULL, "mac"));
    CHECK(!jw_svc_supervisor_join_scan_root(scan_root, 4, "/Apps", "shared"));
    CHECK(jw_svc_supervisor_ctl_op_requires_id("status"));
    CHECK(jw_svc_supervisor_ctl_op_requires_id("export-logs"));
    CHECK(!jw_svc_supervisor_ctl_op_requires_id("not-an-operation"));
    CHECK(!jw_svc_supervisor_ctl_op_requires_id(NULL));

    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.status",
                                   reason, sizeof(reason)));
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.status");
    long long epoch_floor_us = ((long long)time(NULL) - 5LL) * 1000000LL;
    CHECK(e != NULL && e->control.last_transition_at_us >= epoch_floor_us);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.status",
                                reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_many_entries_do_not_use_uninitialized_runtime_fields(void) {
    fixture f;
    fixture_setup(&f);
    for (int i = 0; i < 24; i++) {
        char pak[32];
        char id[64];
        snprintf(pak, sizeof(pak), "many-%02d.pak", i);
        snprintf(id, sizeof(id), "org.umrk.test.many-%02d", i);
        fixture_write_pak(&f, pak, id, "exit 0", NULL);
    }
    int sentinel = open("/dev/null", O_RDONLY);
    CHECK(sentinel >= 0);
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 24);
    CHECK(fcntl(sentinel, F_GETFD) >= 0);
    close(sentinel);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_retained_control_row_survives_missing_package(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "retained.pak", "org.umrk.test.retained",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.retained",
                                   reason, sizeof(reason)));
    jw_svc_supervisor_close(sup);

    char installed[PATH_MAX], removed[PATH_MAX];
    jw__join(installed, sizeof(installed), f.apps_primary, "retained.pak");
    jw__join(removed, sizeof(removed), f.root, "removed-retained.pak");
    CHECK(rename(installed, removed) == 0);

    sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.retained");
    CHECK(e != NULL);
    CHECK(e && !e->pak_present);
    CHECK(e && e->desired_enabled);
    CHECK(e && e->state == JW_SVC_STATE_UNAVAILABLE);
    CHECK(e && strcmp(e->reject_reason, "package-missing") == 0);
    CHECK(e && strcmp(e->installed_package_version, "1.2.3") == 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_locked_runtime_lease_survives_without_pak_or_control_row(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.runtime-only";
    char reason[JW_SVC_REASON_BUF];
    int old_lease = jw_svc_lease_acquire(f.runtime, id, reason,
                                         sizeof(reason));
    CHECK(old_lease >= 0);
    jw_svc_reservation reservation = {
        .pgid = getpid(),
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_STOP,
        .stop_on_storage_change = true,
        .stop_on_suspend = true,
    };
    CHECK(jw_svc_reservation_write(f.runtime, id, &reservation,
                                   reason, sizeof(reason)));

    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    CHECK(e != NULL);
    CHECK(e && !e->control_loaded);
    CHECK(e && !e->pak_present);
    CHECK(e && e->state == JW_SVC_STATE_STALE_GENERATION);
    CHECK(e && strcmp(e->reject_reason, "package-missing") == 0);

    jw_svc_supervisor_close(sup);
    if (old_lease >= 0) close(old_lease);
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

int main(int argc, char **argv) {
    g_program_path = argv[0];
    if (argc == 3 && strcmp(argv[1], "--fd-zero-worker") == 0) {
        return run_descriptor_zero_worker(argv[2]);
    }
    test_clean_exit();
    test_stop_sequence();
    test_ignore_term_escalates();
    test_orphan_descendant();
    test_persistent_vs_session_across_restart();
    test_invalid_manifest_isolated();
    test_duplicate_ids_both_unavailable();
    test_stale_generation_blocks_second_daemon();
    test_descriptor_zero_lease_is_released();
    test_enabled_stop_is_not_immediately_undone();
    test_running_policy_survives_rescan();
    test_reservation_write_failure_reaps_child();
    test_hostile_id_tail_bound_and_status_shape();
    test_many_entries_do_not_use_uninitialized_runtime_fields();
    test_retained_control_row_survives_missing_package();
    test_locked_runtime_lease_survives_without_pak_or_control_row();
    test_circuit_breaker_opens();

    if (g_failures == 0) {
        printf("PASS supervisor-test\n");
        return 0;
    }
    fprintf(stderr, "FAIL supervisor-test (%d failures)\n", g_failures);
    return 1;
}
