/* kill() and usleep() need broader-than-bare-C11 visibility on glibc; see
 * the matching comment in internal/services/lease.c. Must precede every
 * #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/stop.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>

#define JW_SVC_STOP_POLL_INTERVAL_US 10000 /* 10ms: fine enough not to overshoot
                                             * a short test bound by much, coarse
                                             * enough not to spin needlessly. */

static long jw__stop_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

/* Polls absence_check(pgid) until it reports true or timeout_ms elapses.
 * Always performs at least one check, even for timeout_ms <= 0 -- a zero
 * or negative budget still deserves one immediate look, matching "an
 * already-absent group costs nothing" rather than being skipped outright.
 *
 * The deadline is computed once from a monotonic clock and re-checked
 * against a fresh "now" each iteration, rather than accumulating the
 * requested sleep durations. absence_check() itself takes real time
 * (the production implementation scans /proc), and usleep() can oversleep;
 * accumulating only the requested duration would silently let the actual
 * wall-clock budget run over stop_grace_ms + JW_SVC_STOP_KILL_WAIT_MS,
 * which contracts.md states as a hard worst-case bound, not a suggestion. */
static bool jw__stop_poll_absent(pid_t pgid, int timeout_ms,
                                 jw_svc_absence_check_fn absence_check) {
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    long deadline_us = jw__stop_now_us() + (long)timeout_ms * 1000L;
    for (;;) {
        if (absence_check(pgid)) {
            return true;
        }
        long now_us = jw__stop_now_us();
        if (now_us >= deadline_us) {
            return false;
        }
        long remaining_us = deadline_us - now_us;
        long sleep_us = remaining_us < JW_SVC_STOP_POLL_INTERVAL_US
                            ? remaining_us
                            : JW_SVC_STOP_POLL_INTERVAL_US;
        usleep((unsigned int)sleep_us);
    }
}

jw_svc_stop_result jw_svc_stop_group(pid_t pgid, int stop_grace_ms,
                                     jw_svc_absence_check_fn absence_check) {
    jw_svc_stop_result result = {false, false};
    /* pgid == 1 is rejected alongside <= 0: POSIX defines kill(-1, sig) as
     * "every process the caller is permitted to signal", not "process
     * group 1". Treating 1 as an ordinary pgid would turn both signals
     * below into a system-wide broadcast instead of a targeted stop. */
    if (pgid <= 1 || !absence_check) {
        return result;
    }

    /* kill()'s return value is deliberately not checked: ESRCH (group
     * already gone) is not a failure here -- the poll below simply
     * confirms absence immediately. Any other errno still leaves
     * absence_check as the sole source of truth. */
    kill(-pgid, SIGTERM);
    if (jw__stop_poll_absent(pgid, stop_grace_ms, absence_check)) {
        result.verified_absent = true;
        return result;
    }

    result.escalated_to_kill = true;
    kill(-pgid, SIGKILL);
    result.verified_absent =
        jw__stop_poll_absent(pgid, JW_SVC_STOP_KILL_WAIT_MS, absence_check);
    return result;
}
