/* kill() and usleep() need broader-than-bare-C11 visibility on glibc; see
 * the matching comment in internal/services/lease.c. Must precede every
 * #include. */
#define _GNU_SOURCE

#include "internal/services/stop.h"

#include <signal.h>
#include <unistd.h>

#define JW_SVC_STOP_POLL_INTERVAL_US 10000 /* 10ms: fine enough not to overshoot
                                             * a short test bound by much, coarse
                                             * enough not to spin needlessly. */

/* Polls absence_check(pgid) until it reports true or timeout_ms elapses.
 * Always performs at least one check, even for timeout_ms <= 0 -- a zero
 * or negative budget still deserves one immediate look, matching "an
 * already-absent group costs nothing" rather than being skipped outright. */
static bool jw__stop_poll_absent(pid_t pgid, int timeout_ms,
                                 jw_svc_absence_check_fn absence_check) {
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    long elapsed_us = 0;
    long budget_us = (long)timeout_ms * 1000;
    for (;;) {
        if (absence_check(pgid)) {
            return true;
        }
        if (elapsed_us >= budget_us) {
            return false;
        }
        long remaining_us = budget_us - elapsed_us;
        long sleep_us = remaining_us < JW_SVC_STOP_POLL_INTERVAL_US
                            ? remaining_us
                            : JW_SVC_STOP_POLL_INTERVAL_US;
        usleep((unsigned int)sleep_us);
        elapsed_us += sleep_us;
    }
}

jw_svc_stop_result jw_svc_stop_group(pid_t pgid, int stop_grace_ms,
                                     jw_svc_absence_check_fn absence_check) {
    jw_svc_stop_result result = {false, false};
    if (pgid <= 0 || !absence_check) {
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
