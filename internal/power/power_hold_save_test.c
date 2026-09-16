#include "internal/power/power_hold_save.h"

#include <assert.h>
#include <string.h>

#define MIB (1024ull * 1024ull)
#define PRESS 100000LL
#define LONG_PRESS (PRESS + 2000)
#define BOUNDARY (PRESS + JW_POWER_HOLD_SAVE_RELEASE_BOUNDARY_MS)

static const jw_power_hold_save_request ps_state = { "pcsx_rearmed", 4456472ull };
static const jw_power_hold_save_request saturn_state = { "yabasanshiro", 9068872ull };

static void armed(jw_power_hold_save *s) {
    jw_power_hold_save_init(s);
    assert(jw_power_hold_save_long_press(s, PRESS, true, 0) == JW_POWER_HOLD_SAVE_WAIT);
    assert(s->phase == JW_POWER_HOLD_SAVE_AWAIT_RELEASE && s->key_down);
}

static void released(jw_power_hold_save *s, long long release_ms) {
    armed(s);
    jw_power_hold_save_key_edge(s, false, release_ms);
    assert(s->phase == JW_POWER_HOLD_SAVE_RELEASED);
    assert(jw_power_hold_save_effective_deadline_ms(s) ==
           release_ms + JW_POWER_HOLD_SAVE_WINDOW_MS);
}

static void test_constants(void) {
    /* 5.7 s cut - 2.15 s teardown - 0.5 s margin. */
    assert(JW_POWER_HOLD_SAVE_RELEASE_BOUNDARY_MS == 3050);
    assert(strcmp(jw_power_hold_save_outcome_name(
                      JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT),
                  "budget-insufficient") == 0);
    assert(strcmp(jw_power_hold_save_outcome_name(
                      JW_POWER_HOLD_SAVE_OUTCOME_DURABILITY_UNCERTAIN),
                  "durability-uncertain") == 0);
    assert(strcmp(jw_power_hold_save_outcome_name(JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD),
                  "still-held") == 0);
}

static void test_ineligible_and_suspend(void) {
    jw_power_hold_save s;
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_tick(&s, PRESS) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(jw_power_hold_save_long_press(&s, PRESS, false, 0) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
    /* A later release cannot revive a latched attempt. */
    jw_power_hold_save_key_edge(&s, false, PRESS + 2500);
    assert(s.phase == JW_POWER_HOLD_SAVE_DONE);

    /* Key-down before the last resume spans suspend: not timing evidence. */
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_long_press(&s, PRESS, true, PRESS + 1) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_long_press(&s, PRESS, true, PRESS - 1) ==
           JW_POWER_HOLD_SAVE_WAIT);
}

static void test_never_saves_while_held(void) {
    jw_power_hold_save s;
    armed(&s);
    /* Duplicate hold ticks keep the first attempt. */
    assert(jw_power_hold_save_long_press(&s, PRESS + 50, true, 0) ==
           JW_POWER_HOLD_SAVE_WAIT);
    assert(s.press_ms == PRESS);
    assert(jw_power_hold_save_tick(&s, LONG_PRESS) == JW_POWER_HOLD_SAVE_WAIT);
    assert(!jw_power_hold_save_admit(&s, &ps_state, LONG_PRESS, NULL));
    assert(s.phase == JW_POWER_HOLD_SAVE_AWAIT_RELEASE);
    assert(jw_power_hold_save_tick(&s, BOUNDARY - 1) == JW_POWER_HOLD_SAVE_WAIT);
    assert(jw_power_hold_save_tick(&s, BOUNDARY) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD);
}

static void test_release_timestamps(void) {
    jw_power_hold_save s;
    /* Release exactly at the boundary still qualifies. */
    released(&s, BOUNDARY);

    /* A release queued behind a stalled tick uses its own timestamp: the edge
       says 2.6 s even though it is processed after the boundary. */
    armed(&s);
    jw_power_hold_save_key_edge(&s, false, PRESS + 2600);
    assert(s.phase == JW_POWER_HOLD_SAVE_RELEASED);
    assert(jw_power_hold_save_tick(&s, BOUNDARY + 400) == JW_POWER_HOLD_SAVE_ADMIT_NOW);
    assert(jw_power_hold_save_effective_deadline_ms(&s) ==
           PRESS + 2600 + JW_POWER_HOLD_SAVE_WINDOW_MS);

    /* A release stamped after the boundary is too late. */
    armed(&s);
    jw_power_hold_save_key_edge(&s, false, BOUNDARY + 1);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD);

    /* Out-of-order stamps are not trusted. */
    armed(&s);
    jw_power_hold_save_key_edge(&s, false, PRESS - 1);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
}

static void test_estimates(void) {
    assert(jw_power_hold_save_serialize_allowance_ms("gambatte") == 50);
    assert(jw_power_hold_save_serialize_allowance_ms("mupen64plus_next") == 0);
    assert(jw_power_hold_save_serialize_allowance_ms(NULL) == 0);
    /* 100 + 5 MiB * 70 + 150 */
    assert(jw_power_hold_save_estimate_ms(&ps_state) == 600);
    /* 500 + 9 MiB * 70 + 150 */
    assert(jw_power_hold_save_estimate_ms(&saturn_state) == 1280);
    jw_power_hold_save_request exact = { "snes9x", 2 * MIB };
    assert(jw_power_hold_save_estimate_ms(&exact) == 50 + 2 * 70 + 150);

    jw_power_hold_save_request unknown_size = { "gambatte", 0 };
    jw_power_hold_save_request unmeasured = { "flycast", MIB };
    jw_power_hold_save_request huge = { "gambatte", 2048ull * MIB };
    assert(jw_power_hold_save_estimate_ms(&unknown_size) == -1);
    assert(jw_power_hold_save_estimate_ms(&unmeasured) == -1);
    assert(jw_power_hold_save_estimate_ms(&huge) == -1);
    assert(jw_power_hold_save_estimate_ms(NULL) == -1);
}

static void test_admission(void) {
    jw_power_hold_save s;
    long long deadline = -1;
    long long release = PRESS + 2700;

    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &saturn_state, release + 100, &deadline));
    assert(s.phase == JW_POWER_HOLD_SAVE_SAVING);
    assert(deadline == release + JW_POWER_HOLD_SAVE_WINDOW_MS);
    assert(s.admitted_deadline_ms == deadline);
    /* Admission happens once. */
    assert(!jw_power_hold_save_admit(&s, &saturn_state, release + 100, NULL));
    assert(s.phase == JW_POWER_HOLD_SAVE_SAVING);
    assert(jw_power_hold_save_tick(&s, deadline - 1) == JW_POWER_HOLD_SAVE_WAIT);
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
    assert(jw_power_hold_save_tick(&s, deadline - 1) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    /* A late finish cannot rewrite the latched outcome. */
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_SAVED);

    /* Fits exactly at the bound. */
    released(&s, release);
    deadline = release + JW_POWER_HOLD_SAVE_WINDOW_MS;
    assert(jw_power_hold_save_admit(&s, &ps_state, deadline - 600, NULL));

    /* One millisecond short fails closed without sending anything. */
    released(&s, release);
    assert(!jw_power_hold_save_admit(&s, &ps_state, deadline - 599, &deadline));
    assert(deadline == 0);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
    assert(jw_power_hold_save_tick(&s, release + 1) == JW_POWER_HOLD_SAVE_SHUT_DOWN);

    /* Unmeasured core. */
    jw_power_hold_save_request unmeasured = { "mupen64plus_next", 16 * MIB };
    released(&s, release);
    assert(!jw_power_hold_save_admit(&s, &unmeasured, release, NULL));
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);

    /* Never admitted before the window closed. */
    released(&s, release);
    assert(jw_power_hold_save_tick(&s, release + JW_POWER_HOLD_SAVE_WINDOW_MS) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);

    /* finish() without an outcome is a storage error, not a success. */
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &ps_state, release, NULL));
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_NONE);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR);
}

static void test_timeout(void) {
    jw_power_hold_save s;
    long long release = PRESS + 2500;
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &ps_state, release, NULL));
    assert(jw_power_hold_save_tick(&s, release + JW_POWER_HOLD_SAVE_WINDOW_MS) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT);
    /* A reply that lands after the timeout cannot turn into "saved". */
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT);
}

static void test_repress_caps_the_window(void) {
    jw_power_hold_save s;
    long long release = PRESS + 2500;
    long long window = release + JW_POWER_HOLD_SAVE_WINDOW_MS;
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &saturn_state, release + 50, NULL));

    /* Re-press 1 s after release: fresh PMIC timer, cap at repress + 3050. */
    long long repress = release + 1000;
    jw_power_hold_save_key_edge(&s, true, repress);
    assert(jw_power_hold_save_effective_deadline_ms(&s) ==
           repress + JW_POWER_HOLD_SAVE_RELEASE_BOUNDARY_MS);

    /* Releasing again removes that cap but never extends past the window. */
    jw_power_hold_save_key_edge(&s, false, repress + 400);
    assert(jw_power_hold_save_effective_deadline_ms(&s) == window);

    /* Repeated presses cannot push waiting past the window either. */
    for (int i = 0; i < 20; i++) {
        long long t = repress + 1000 + i * 400;
        jw_power_hold_save_key_edge(&s, true, t);
        assert(jw_power_hold_save_effective_deadline_ms(&s) <= window);
        jw_power_hold_save_key_edge(&s, false, t + 100);
        assert(jw_power_hold_save_effective_deadline_ms(&s) == window);
    }

    /* A re-press late in the window keeps the window as the tighter bound. */
    jw_power_hold_save_key_edge(&s, true, window - 1000);
    assert(jw_power_hold_save_effective_deadline_ms(&s) == window);

    /* Held re-press that runs out its cap times out the save. */
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &ps_state, release, NULL));
    jw_power_hold_save_key_edge(&s, true, release + 200);
    long long cap = release + 200 + JW_POWER_HOLD_SAVE_RELEASE_BOUNDARY_MS;
    assert(jw_power_hold_save_tick(&s, cap - 1) == JW_POWER_HOLD_SAVE_WAIT);
    assert(jw_power_hold_save_tick(&s, cap) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT);

    /* A re-press before admission shrinks the budget admission sees. */
    released(&s, release);
    jw_power_hold_save_key_edge(&s, true, release + 100);
    long long held_cap = release + 100 + JW_POWER_HOLD_SAVE_RELEASE_BOUNDARY_MS;
    assert(!jw_power_hold_save_admit(&s, &saturn_state, held_cap - 1279, NULL));
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
}

int main(void) {
    test_constants();
    test_ineligible_and_suspend();
    test_never_saves_while_held();
    test_release_timestamps();
    test_estimates();
    test_admission();
    test_timeout();
    test_repress_caps_the_window();
    return 0;
}
