#include "internal/services/ownership.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Real fork()/setpgid() fixtures, not mocks -- the whole point of this
 * module is to be trustworthy against the actual kernel's process table,
 * on whichever platform the test runs (see ownership.c: Linux/proc(5) and
 * macOS/libproc both get a real implementation, not a stub, precisely so
 * this test exercises the real thing on a dev machine too). */

static bool jw__wait_until(bool (*pred)(pid_t), pid_t arg, int timeout_ms) {
    int elapsed = 0;
    for (;;) {
        if (pred(arg)) {
            return true;
        }
        if (elapsed >= timeout_ms) {
            return false;
        }
        usleep(5000);
        elapsed += 5;
    }
}

static bool jw__is_absent(pid_t pgid) {
    return jw_svc_group_absent(pgid);
}

static void jw__test_rejects_nonpositive_pgid(void) {
    assert(jw_svc_group_absent(0) == false);
    assert(jw_svc_group_absent(-1) == false);
    puts("PASS ownership-test rejects non-positive pgid");
}

static void jw__test_simple_child(void) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        setpgid(0, 0); /* becomes its own group leader: pgid == its own pid */
        pause();
        _exit(0);
    }

    /* setpgid() in the child is a race against this parent reading its
     * pgid via getpgid(); poll briefly rather than assume it has already
     * taken effect. */
    pid_t child_pgid = 0;
    for (int i = 0; i < 200; i++) {
        child_pgid = getpgid(child);
        if (child_pgid == child) {
            break;
        }
        usleep(1000);
    }
    assert(child_pgid == child);

    assert(jw_svc_group_absent(child) == false);

    kill(child, SIGKILL);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);

    assert(jw__wait_until(jw__is_absent, child, 2000));
    puts("PASS ownership-test simple child: alive=not-absent, reaped=absent");
}

/* Isolates the SZOMB/'Z' handling itself: a leader with NO live descendant
 * at all must be reported absent as soon as it becomes a zombie, without
 * requiring the test process to reap it first. The other two scenarios in
 * this file only check absence AFTER an explicit waitpid() has fully
 * reaped every process involved, which would pass even if zombie status
 * were mishandled (a fully reaped process is gone from the table either
 * way) -- this one specifically would fail if a live zombie entry were
 * miscounted as a writer. */
static void jw__test_zombie_alone_is_absent(void) {
    pid_t leader = fork();
    assert(leader >= 0);
    if (leader == 0) {
        setpgid(0, 0);
        _exit(0);
    }

    pid_t pgid = leader;
    /* Deliberately no waitpid() here yet: leader is exiting/exited but
     * unreaped, i.e. either still running for a moment or already a
     * zombie -- exactly the state this checks. */
    bool became_absent = jw__wait_until(jw__is_absent, pgid, 2000);
    int status = 0;
    pid_t reaped = waitpid(leader, &status, 0); /* cleanup */
    assert(reaped == leader);
    assert(became_absent);
    puts("PASS ownership-test zombie alone (no descendant): absent without being reaped first");
}

/* The scenario contracts.md's "why the pgid is a stable reference" section
 * exists for: a zombie LEADER with a live DESCENDANT is not absent. Only
 * once every member -- leader included -- is gone or zombie is the group
 * actually free. */
static void jw__test_zombie_leader_live_descendant(void) {
    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t leader = fork();
    assert(leader >= 0);
    if (leader == 0) {
        close(ready_pipe[0]);
        setpgid(0, 0); /* leader's pgid becomes its own pid */

        pid_t grandchild = fork();
        assert(grandchild >= 0);
        if (grandchild == 0) {
            /* Inherits the leader's pgid automatically; no setpgid needed
             * here. Report our own pid so the test process can kill us by
             * pid later (the leader forked us, not the test process, so
             * the test process never otherwise learns this pid). */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
            write(ready_pipe[1], buf, (size_t)n);
            close(ready_pipe[1]);
            pause();
            _exit(0);
        }
        close(ready_pipe[1]);
        /* Exit immediately WITHOUT the test process having reaped us yet:
         * we become a zombie the moment the test process notices (via the
         * pipe closing) that we're gone, while our descendant is still
         * live and still a member of this same pgid. */
        _exit(0);
    }

    close(ready_pipe[1]);
    char buf[32] = {0};
    ssize_t n = read(ready_pipe[0], buf, sizeof(buf) - 1);
    close(ready_pipe[0]);
    assert(n > 0);
    pid_t grandchild_pid = (pid_t)atoi(buf);
    assert(grandchild_pid > 0);

    /* The leader's pgid equals its own pid (it made itself a group leader
     * before forking); the test process's parent-of-leader relationship
     * lets it identify that pgid without racing on getpgid() the way the
     * simple-child case above does, since the leader already reported
     * readiness (via the pipe write happening strictly after setpgid) by
     * the time we get here. */
    pid_t pgid = leader;

    /* Grandchild is confirmed alive (it wrote to the pipe) and the leader
     * has not been reaped by anyone -- the group must not be absent. */
    assert(jw_svc_group_absent(pgid) == false);

    kill(grandchild_pid, SIGKILL);

    /* Reap the leader now. It has certainly already called _exit() (it
     * did so before the grandchild could run far enough to write the
     * pipe... actually the reverse ordering is fine either way: this
     * waitpid blocks until the leader has exited, whenever that happens.) */
    int status = 0;
    assert(waitpid(leader, &status, 0) == leader);

    assert(jw__wait_until(jw__is_absent, pgid, 2000));
    puts("PASS ownership-test zombie leader + live descendant: "
         "not absent until BOTH are gone, reaping the leader last");
}

int main(void) {
    jw__test_rejects_nonpositive_pgid();
    jw__test_simple_child();
    jw__test_zombie_alone_is_absent();
    jw__test_zombie_leader_live_descendant();
    puts("PASS ownership-test");
    return 0;
}
