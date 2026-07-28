#define _GNU_SOURCE

#include "internal/services/stop.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* This file's absence checks are deliberately NOT jw_svc_group_absent()
 * (internal/services/ownership.c) -- that function lives on a separate,
 * independently-reviewed branch/PR and is meant to be wired in by
 * whatever assembles the real supervisor. stop.c's own contract is that
 * it works with ANY absence_check a caller supplies; this test suite's
 * job is to verify the SEQUENCE (signal, wait, escalate, wait, report),
 * not to re-validate a general-purpose absence primitive that has its
 * own test suite already. */

/* Real (not fake) absence signal for a single test-controlled child with
 * no descendants: WNOHANG-waits it. Returns true exactly once the child
 * has exited (reaping it in the process) or was already reaped. This is
 * a correct absence signal for this narrow scenario -- it does not
 * generalize to orphaned descendants or multiple members, which is
 * exactly the harder problem jw_svc_group_absent() exists to solve. */
static bool jw__test_real_absent(pid_t pgid) {
    int status = 0;
    pid_t r = waitpid(pgid, &status, WNOHANG);
    if (r == pgid) {
        return true; /* just exited; reaped now */
    }
    if (r < 0 && errno == ECHILD) {
        return true; /* no such child: already reaped */
    }
    return false;
}

/* Fault injection for the "stop cannot be verified" path -- see
 * phase-a2-supervisor.md: "the deterministic contract test is fault
 * injection at the absence-verification point... do not spend a blocker
 * trying to make FUSE emulate a kernel hang." This always lies that the
 * group is still present, regardless of what actually happened to the
 * real child stop.c's own kill() calls act on. */
static bool jw__test_always_present(pid_t pgid) {
    (void)pgid;
    return false;
}

static pid_t jw__test_fork_group_leader(void (*child_body)(void)) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(setpgid(0, 0) == 0);
        child_body();
        _exit(1); /* child_body() must not return */
    }

    /* setpgid() in the child races this parent's kill(-pid, ...) calls:
     * until it has actually run, the child is still in THIS process's
     * group, not its own, and a signal aimed at group `pid` would miss
     * it entirely (or hit nothing, if that pgid has no members yet).
     * Poll briefly rather than assume it already happened -- the same
     * fix ownership_test.c needed for its simple-child fixture. */
    for (int i = 0; i < 200; i++) {
        if (getpgid(pid) == pid) {
            return pid;
        }
        usleep(1000);
    }
    assert(getpgid(pid) == pid);
    return pid;
}

static void jw__test_child_default_term(void) {
    pause(); /* default SIGTERM disposition terminates us immediately */
}

static void jw__test_child_ignores_term(void) {
    assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
    for (;;) {
        pause(); /* SIGTERM is ignored; only SIGKILL can end this */
    }
}

static void jw__test_rejects_invalid_args(void) {
    jw_svc_stop_result r;

    r = jw_svc_stop_group(0, 100, jw__test_real_absent);
    assert(!r.verified_absent && !r.escalated_to_kill);

    r = jw_svc_stop_group(-1, 100, jw__test_real_absent);
    assert(!r.verified_absent && !r.escalated_to_kill);

    r = jw_svc_stop_group(12345, 100, NULL);
    assert(!r.verified_absent && !r.escalated_to_kill);

    puts("PASS stop-test rejects invalid pgid/absence_check");
}

static void jw__test_cooperative_exit_on_term(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_default_term);

    jw_svc_stop_result r = jw_svc_stop_group(child, 3000, jw__test_real_absent);
    assert(r.verified_absent);
    assert(!r.escalated_to_kill); /* TERM alone must have sufficed */

    puts("PASS stop-test cooperative exit: SIGTERM alone verified absent, no escalation");
}

static void jw__test_ignore_term_needs_kill(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_ignores_term);

    /* Short grace window: the child truly ignores SIGTERM, so this must
     * escalate every time, not merely "might". */
    jw_svc_stop_result r = jw_svc_stop_group(child, 100, jw__test_real_absent);
    assert(r.escalated_to_kill);
    assert(r.verified_absent); /* SIGKILL cannot be ignored or blocked */

    puts("PASS stop-test ignore-TERM: escalates to SIGKILL, still verifies absent");
}

static void jw__test_unverifiable_stop_via_fault_injection(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_default_term);

    /* The real child actually dies for real (stop.c's own kill() calls
     * are unmocked), but the injected absence_check lies that it never
     * went away -- this is what makes the "cannot be verified" path
     * deterministic to test without a genuine kernel hang. */
    jw_svc_stop_result r = jw_svc_stop_group(child, 50, jw__test_always_present);
    assert(r.escalated_to_kill);
    assert(!r.verified_absent);

    /* Clean up the real child ourselves: jw__test_always_present never
     * reaped it. */
    int status = 0;
    assert(waitpid(child, &status, 0) == child);

    puts("PASS stop-test fault injection: unverifiable stop reported honestly, "
         "never claimed absent");
}

int main(void) {
    jw__test_rejects_invalid_args();
    jw__test_cooperative_exit_on_term();
    jw__test_ignore_term_needs_kill();
    jw__test_unverifiable_stop_via_fault_injection();
    puts("PASS stop-test");
    return 0;
}
