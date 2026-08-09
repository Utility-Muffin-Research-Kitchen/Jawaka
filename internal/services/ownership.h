#ifndef JW_SERVICES_OWNERSHIP_H
#define JW_SERVICES_OWNERSHIP_H

#include <stdbool.h>
#include <sys/types.h>

/* SVC-1's absence test (contracts.md#svc-1--service-manifest, "why the
 * pgid is a stable reference" and the stop-sequence table): true iff no
 * member of process group `pgid` is anywhere on this system in a
 * non-zombie state, right now.
 *
 * This is meant to be THE ONLY function anything in this codebase uses to
 * decide "safe to treat a stale group as gone," "the stop sequence
 * verified," or "report this service as stopped." A bare
 * `kill(pgid, 0) == -1` is forbidden by the contract because a recycled
 * pgid answers it just as happily as a live one; a single `waitpid()` on
 * one known child is insufficient because a service's descendant can be
 * orphaned and reparented (still a member of the same pgid, invisible to
 * a direct-child wait). "Absent" specifically means every member is
 * either gone entirely or a zombie -- a zombie LEADER with a live
 * descendant is not absent; only after every member is gone-or-zombie is
 * the group actually free.
 *
 * pgid <= 0 does not identify a specific group and always returns false:
 * this function never claims absence for a query that cannot mean
 * exactly one thing.
 *
 * A failure to enumerate the system's process table (e.g. /proc
 * unreadable) also returns false -- inability to prove absence is never
 * treated as absence.
 */
bool jw_svc_group_absent(pid_t pgid);

#if defined(__linux__) && defined(JW_SVC_OWNERSHIP_TESTING)
/* Parser seam for synthetic proc(5) rows that ordinary PID namespaces do not
 * expose, notably MLP1 kernel threads whose process-group field is zero. */
bool jw_svc_proc_stat_group_state_for_test(const char *line, pid_t *out_pgid,
                                           bool *out_zombie,
                                           int *out_threads);
#endif

#endif
