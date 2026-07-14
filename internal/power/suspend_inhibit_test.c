#include "internal/power/suspend_inhibit.h"

#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    jw_suspend_inhibitor inhibitor;
    jw_suspend_inhibitor_init(&inhibitor);
    char one[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    char two[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                        "download", 10, one) == JW_SUSPEND_LEASE_OK);
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                        "archive", 20, two) == JW_SUSPEND_LEASE_OK);
    assert(strcmp(one, two) != 0 && jw_suspend_inhibitor_count(&inhibitor) == 2);

    bool released = false;
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid() + 1, one, &released) ==
           JW_SUSPEND_LEASE_WRONG_OWNER);
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid(), "bad", &released) ==
           JW_SUSPEND_LEASE_INVALID);
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid(), one, &released) ==
           JW_SUSPEND_LEASE_OK && released);
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid(), one, &released) ==
           JW_SUSPEND_LEASE_OK && !released); /* duplicate release is idempotent */

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) pause();
    char dead[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, child, "block-suspend",
                                        "dead holder", 30, dead) == JW_SUSPEND_LEASE_OK);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    assert(jw_suspend_inhibitor_reap(&inhibitor) == 1);

    jw_suspend_policy policy;
    jw_suspend_policy_init(&policy);
    assert(jw_suspend_policy_auto_stage2(&policy, 1) == JW_SUSPEND_DECISION_SCREEN_OFF);
    assert(policy.pending == JW_SUSPEND_PENDING_AUTO);
    assert(jw_suspend_policy_cancel_for_activity(&policy) == JW_SUSPEND_PENDING_AUTO);
    assert(jw_suspend_policy_power_tap(&policy, 1) == JW_SUSPEND_DECISION_SCREEN_OFF);
    assert(policy.pending == JW_SUSPEND_PENDING_EXPLICIT);
    assert(jw_suspend_policy_cancel_for_activity(&policy) == JW_SUSPEND_PENDING_EXPLICIT);
    assert(jw_suspend_policy_power_tap(&policy, 1) == JW_SUSPEND_DECISION_SCREEN_OFF);
    assert(jw_suspend_policy_leases_changed(&policy, 0) == JW_SUSPEND_DECISION_DEEP_SLEEP);
    assert(jw_suspend_policy_long_press(&policy) == JW_SUSPEND_DECISION_POWEROFF);

    jw_suspend_inhibitor_clear(&inhibitor);
    assert(jw_suspend_inhibitor_count(&inhibitor) == 0);
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        char token[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
        assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                            "capacity", i, token) == JW_SUSPEND_LEASE_OK);
    }
    char overflow[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                        "overflow", 99, overflow) == JW_SUSPEND_LEASE_FULL);
    jw_suspend_inhibitor_clear(&inhibitor);
    return 0;
}
