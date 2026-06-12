#ifndef JW_SCRAPE_WORKER_H
#define JW_SCRAPE_WORKER_H

#include <stdbool.h>

/* Daemon-side scrape queue + worker. One global queue (the daemon owns one
   scrape pipeline); items are deduped by (system, rom_path). Art lands at
   the rom's own storage source under Images/<SYS>/<rom-base>.png, the game
   row's image_path is updated, and library.generation is bumped per file so
   the launcher refreshes live. */

#define JW_SCRAPE_MAX_DIM 1000   /* longest-side cap for saved art */

typedef enum {
    JW_SCRAPE_IDLE = 0,
    JW_SCRAPE_RUNNING,
    JW_SCRAPE_PAUSED_QUOTA,   /* daily quota exhausted; queue retained */
} jw_scrape_state;

typedef struct {
    jw_scrape_state state;
    int  total;               /* items accepted since the queue last drained */
    int  done;                /* processed (found + not_found + failed) */
    int  found;
    int  not_found;
    int  failed;
    int  queued;              /* still waiting (excludes in-flight) */
    char current_name[256];   /* rom filename being processed ("" when idle) */
    char current_system[64];
    char message[256];        /* last error / quota message ("" when none) */
} jw_scrape_status_info;

/* Start/stop the worker thread. Both are idempotent; stop joins the thread
   and drops any queued items. */
int  jw_scrape_worker_start(const char *db_path, const char *sdcard_root);
void jw_scrape_worker_stop(void);

/* Enqueue one game (always re-fetches and replaces existing art). Returns
   the number of items enqueued (0 when deduped), or -1 with *error set to a
   static message (unmapped system, queue full, worker not running). */
int jw_scrape_enqueue_game(const char *system, const char *rom_path,
                           const char **error);

/* Enqueue every game of a system; missing_only skips games whose art file
   already exists. Same return convention as enqueue_game. */
int jw_scrape_enqueue_system(const char *system, bool missing_only,
                             const char **error);

void jw_scrape_status(jw_scrape_status_info *out);

/* Cancel queued work (returns the number of items removed). In-flight items
   finish normally, matching the IPC contract. */
int jw_scrape_cancel_all(void);
int jw_scrape_cancel_system(const char *system);
int jw_scrape_cancel_game(const char *system, const char *rom_path);

/* True when the game/system has queued or in-flight work (drives the
   Actions-menu "Cancel Scraping" swap). */
bool jw_scrape_is_pending_game(const char *system, const char *rom_path);
bool jw_scrape_is_pending_system(const char *system);

#endif /* JW_SCRAPE_WORKER_H */
