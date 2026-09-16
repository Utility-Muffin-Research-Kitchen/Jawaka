#ifndef JW_POWER_HOLD_SAVE_H
#define JW_POWER_HOLD_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Save-before-power-off policy for a physical power-key long press.

   Saves run only after the player releases the key: releasing resets the
   RK817 long-press timer, and while held there is too little time left before
   the PMIC hard cut to save and still finish the clean shutdown. All times are
   CLOCK_MONOTONIC milliseconds; key times are the input events' own kernel
   timestamps, never the time an edge was processed.

   Pure logic: no I/O, clocks, or IPC. jawakad feeds it edges and ticks and
   acts on the returned decisions. Constants come from the MLP1 Step 0
   measurements (umrk-workspace plans/Jawaka/save-state-on-power-hold.md). */

/* PMIC hard cut measured 5.7-6.0 s after key-down (register says 6 s). */
#define JW_POWER_HOLD_SAVE_PMIC_CUT_MS          5700
/* Long press to kernel power-off entry, worst of four measured runs. */
#define JW_POWER_HOLD_SAVE_TEARDOWN_RESERVE_MS  2150
/* Kernel power-off entry to the physical cut is unmeasured. */
#define JW_POWER_HOLD_SAVE_MARGIN_MS            500
/* A release must land within this long of key-down for a save to open. */
#define JW_POWER_HOLD_SAVE_RELEASE_BOUNDARY_MS \
    (JW_POWER_HOLD_SAVE_PMIC_CUT_MS - JW_POWER_HOLD_SAVE_TEARDOWN_RESERVE_MS - \
     JW_POWER_HOLD_SAVE_MARGIN_MS)
/* Software cap on the whole save once released. */
#define JW_POWER_HOLD_SAVE_WINDOW_MS            8000
/* Conservative flush rate for both MLP1 cards (measured worst 51 and 67). */
#define JW_POWER_HOLD_SAVE_FLUSH_MS_PER_MIB     70
/* rename (<= 70 ms), directory fsync (<= 25 ms), thumbnail removal, slack. */
#define JW_POWER_HOLD_SAVE_PUBLISH_MS           150

typedef enum {
    JW_POWER_HOLD_SAVE_IDLE = 0,
    JW_POWER_HOLD_SAVE_AWAIT_RELEASE,  /* prompt shown; key still down */
    JW_POWER_HOLD_SAVE_RELEASED,       /* window open; admission pending */
    JW_POWER_HOLD_SAVE_SAVING,         /* request admitted and in flight */
    JW_POWER_HOLD_SAVE_DONE            /* outcome latched; shut down */
} jw_power_hold_save_phase;

typedef enum {
    JW_POWER_HOLD_SAVE_OUTCOME_NONE = 0,
    JW_POWER_HOLD_SAVE_OUTCOME_SAVED,
    JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED,
    JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD,
    JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT,
    JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR,
    JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT,
    JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED,
    JW_POWER_HOLD_SAVE_OUTCOME_DURABILITY_UNCERTAIN,
    JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED   /* shutdown signal overrode the save */
} jw_power_hold_save_outcome;

typedef enum {
    JW_POWER_HOLD_SAVE_WAIT = 0,      /* keep ticking; defer child teardown */
    JW_POWER_HOLD_SAVE_ADMIT_NOW,     /* released: estimate and call admit() */
    JW_POWER_HOLD_SAVE_SHUT_DOWN      /* outcome latched; continue shutdown */
} jw_power_hold_save_decision;

typedef struct {
    jw_power_hold_save_phase phase;
    jw_power_hold_save_outcome outcome;
    long long press_ms;          /* key-down that armed the attempt */
    long long release_ms;        /* first qualifying release */
    long long window_deadline_ms;
    bool key_down;               /* physical key state from edges */
    long long repress_ms;        /* latest key-down after release, if down */
    long long admitted_deadline_ms;
} jw_power_hold_save;

typedef struct {
    const char *core_id;  /* catalog core id of the running session */
    uint64_t state_bytes; /* serialize size plus format overhead; 0 = unknown */
} jw_power_hold_save_request;

void jw_power_hold_save_init(jw_power_hold_save *s);

/* The long press was recognized. `eligible` covers the setting, a Leaf-managed
   RetroArch game with state support, a safe namespace, writable storage, and
   the synchronous-save capability. `last_resume_ms` is the monotonic time of
   the latest resume from suspend (0 if none): a key-down before it spans
   suspend and is not timing evidence. Returns WAIT to show the release prompt
   or SHUT_DOWN when no attempt starts. Duplicate calls keep the first attempt. */
jw_power_hold_save_decision jw_power_hold_save_long_press(jw_power_hold_save *s,
                                                         long long press_ms,
                                                         bool eligible,
                                                         long long last_resume_ms);

/* Feed every power-key edge with its event timestamp, in queue order. For a
   released hold recognized late, call long_press() first, then feed the
   queued release. */
void jw_power_hold_save_key_edge(jw_power_hold_save *s, bool down, long long edge_ms);

/* Advance on each daemon tick. */
jw_power_hold_save_decision jw_power_hold_save_tick(jw_power_hold_save *s,
                                                   long long now_ms);

/* Serialization allowance for a measured core, or 0 when unmeasured. */
int jw_power_hold_save_serialize_allowance_ms(const char *core_id);

/* Conservative duration for the request, or -1 when it cannot be estimated. */
long long jw_power_hold_save_estimate_ms(const jw_power_hold_save_request *req);

/* Admit or reject the save once released. On admission the phase becomes
   SAVING and `*deadline_ms` receives the absolute bound RetroArch must recheck
   before opening its temporary output. Returns true when admitted; otherwise
   the outcome is BUDGET_INSUFFICIENT and the next tick shuts down. */
bool jw_power_hold_save_admit(jw_power_hold_save *s,
                              const jw_power_hold_save_request *req,
                              long long now_ms, long long *deadline_ms);

/* Current bound on waiting, honoring a re-press; 0 when no window is open. */
long long jw_power_hold_save_effective_deadline_ms(const jw_power_hold_save *s);

/* True while an attempt defers shutdown teardown. */
bool jw_power_hold_save_active(const jw_power_hold_save *s);

/* End any active attempt with `outcome` (child exit, shutdown signal).
   Ignored when nothing is active. */
void jw_power_hold_save_abort(jw_power_hold_save *s,
                              jw_power_hold_save_outcome outcome);

/* Report how the admitted save ended. Ignored unless SAVING. */
void jw_power_hold_save_finish(jw_power_hold_save *s,
                               jw_power_hold_save_outcome outcome);

/* Log token for an outcome, e.g. "budget-insufficient". */
const char *jw_power_hold_save_outcome_name(jw_power_hold_save_outcome outcome);

#endif
