#ifndef JW_SERVICES_UNVERIFIED_STOP_H
#define JW_SERVICES_UNVERIFIED_STOP_H

/* contracts.md's SVC-1 "When a stop cannot be verified" table (a group
 * that outlived SIGKILL, or an earlier Jawaka generation still holding
 * the lease -- either way, the reserved group is not provably absent):
 *
 *   | Caller                                   | Behavior |
 *   | ----------------------------------------- | -------- |
 *   | Shutdown, reboot, exit-to-stock           | Continue the platform transition; record the warning. |
 *   | Suspend                                   | Continue; record the warning. |
 *   | Safe unmount (stop_on_storage_change)      | Fail the unmount and tell the user which service is stuck. |
 *   | Game launch (LIFE-1 fallback)             | Do not launch silently. Surface the stuck service and require an explicit user override. |
 *   | PKG-1 / TXN-1                              | Fail the package operation. Never change bytes under a live process. |
 *
 * This module is that table alone, as a pure lookup: given which caller
 * is asking, it returns what to do when the caller already knows a
 * stop could not be verified absent. It does not decide whether a stop
 * IS verified (internal/services/stop.c and ownership.c do that); it
 * has no I/O and holds no state.
 */

typedef enum {
    /* Shutdown, reboot, or exit-to-stock: a platform transition that
     * must not be blocked by a stuck service. */
    JW_SVC_STOP_CALLER_SHUTDOWN = 0,
    /* Device suspend. */
    JW_SVC_STOP_CALLER_SUSPEND,
    /* A safe-unmount request for a service whose manifest sets
     * stop_on_storage_change. */
    JW_SVC_STOP_CALLER_SAFE_UNMOUNT,
    /* LIFE-1's fallback stop of a lifecycle.game != "ignore" service
     * ahead of a game launch. */
    JW_SVC_STOP_CALLER_GAME_LAUNCH,
    /* A PKG-1 install/update/remove or TXN-1 transaction that would
     * mutate a service pak's files. */
    JW_SVC_STOP_CALLER_PACKAGE_OP,
} jw_svc_stop_caller;

typedef enum {
    /* Proceed with the caller's own operation regardless; the caller
     * must still record a warning naming the stuck service. */
    JW_SVC_UNVERIFIED_STOP_CONTINUE_WITH_WARNING = 0,
    /* Abort the caller's own operation entirely and report which
     * service is stuck. */
    JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION,
    /* Do not proceed silently; surface the stuck service and require
     * an explicit user override before continuing. */
    JW_SVC_UNVERIFIED_STOP_REQUIRE_OVERRIDE,
} jw_svc_unverified_stop_action;

/* Returns the table's action for `caller`. Every jw_svc_stop_caller
 * value is covered; there is no "unknown caller" case for this
 * function to handle since the enum is closed and every value is a
 * real, named row in the contract's table above.
 *
 * Callers use this identically whether the underlying reason a stop
 * could not be verified was "outlived SIGKILL" or "an earlier
 * generation still holds the lease" -- contracts.md is explicit that
 * "the same table applies" to both. */
jw_svc_unverified_stop_action jw_svc_unverified_stop_action_for(jw_svc_stop_caller caller);

#endif
