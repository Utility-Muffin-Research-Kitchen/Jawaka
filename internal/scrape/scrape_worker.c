#include "internal/scrape/scrape_worker.h"
#include "internal/scrape/scrape_systems.h"
#include "internal/scrape/ss_client.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/storage/sources.h"

#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define JW__SCRAPE_QUEUE_MAX   4096
#define JW__SCRAPE_LIST_MAX    4096   /* games per system when expanding a batch */
#define JW__SCRAPE_PRIO_MAX    16
#define JW__SCRAPE_MAX_WORKERS 4      /* hard cap regardless of account allowance */

typedef struct {
    char system[64];
    char rom_path[512];
} jw__scrape_item;

typedef struct {
    bool            used;
    jw__scrape_item item;
    atomic_int      interrupt;   /* cancels this slot's HTTP work */
} jw__active_slot;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            running;        /* pool alive */
    bool            shutdown;
    pthread_t       threads[JW__SCRAPE_MAX_WORKERS];
    int             thread_count;

    char db_path[PATH_MAX];
    char sdcard_root[PATH_MAX];

    jw__scrape_item *queue;         /* FIFO: [head, head+count) */
    int  head;
    int  count;

    jw__active_slot active[JW__SCRAPE_MAX_WORKERS];
    int  active_count;
    int  permits;                   /* concurrent allowance: 1..MAX_WORKERS,
                                       follows the account's maxthreads */
    bool paused_quota;              /* daily quota exhausted; queue retained */

    /* batch counters; reset when work starts from a fully drained state */
    int total;
    int done;
    int found;
    int not_found;
    int failed;
    char message[256];
} jw__w = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .permits = 1,
};

/* ── Settings: credentials + priorities ──────────────────────────────── */

typedef struct {
    char user[256];
    char pass[256];
    int  max_threads;
    char *artwork[JW__SCRAPE_PRIO_MAX];
    int  artwork_count;
    char *regions[JW__SCRAPE_PRIO_MAX];
    int  region_count;
} jw__scrape_prefs;

/* Split a comma-separated settings value into strdup'd entries. */
static int jw__split_csv(const char *csv, char **out, int max) {
    int n = 0;
    const char *p = csv;
    while (p && *p && n < max) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        if (len > 0) {
            char *entry = malloc(len + 1);
            if (!entry) break;
            memcpy(entry, p, len);
            entry[len] = '\0';
            out[n++] = entry;
        }
        p = comma ? comma + 1 : NULL;
    }
    return n;
}

static void jw__prefs_load(jw__scrape_prefs *prefs) {
    memset(prefs, 0, sizeof(*prefs));

    char artwork_csv[512] = "";
    char region_csv[256] = "";
    char max_threads_buf[32] = "";
    jw_db_setting_query queries[] = {
        { "screenscraper_user", prefs->user, sizeof(prefs->user), 0 },
        { "screenscraper_pass", prefs->pass, sizeof(prefs->pass), 0 },
        { "screenscraper_maxthreads", max_threads_buf, sizeof(max_threads_buf), 0 },
        { "scrape.artwork_priority", artwork_csv, sizeof(artwork_csv), 0 },
        { "scrape.region_priority", region_csv, sizeof(region_csv), 0 },
    };
    jw_db_get_settings(jw__w.db_path, queries, 5);
    prefs->max_threads = atoi(max_threads_buf);

    prefs->artwork_count = jw__split_csv(artwork_csv, prefs->artwork,
                                         JW__SCRAPE_PRIO_MAX);
    if (prefs->artwork_count == 0) {
        for (int i = 0; i < jw_ss_default_artwork_priority_count &&
                        i < JW__SCRAPE_PRIO_MAX; i++) {
            prefs->artwork[i] = strdup(jw_ss_default_artwork_priority[i]);
            prefs->artwork_count = i + 1;
        }
    }
    prefs->region_count = jw__split_csv(region_csv, prefs->regions,
                                        JW__SCRAPE_PRIO_MAX);
    if (prefs->region_count == 0) {
        for (int i = 0; i < jw_ss_default_region_priority_count &&
                        i < JW__SCRAPE_PRIO_MAX; i++) {
            prefs->regions[i] = strdup(jw_ss_default_region_priority[i]);
            prefs->region_count = i + 1;
        }
    }
}

static void jw__prefs_free(jw__scrape_prefs *prefs) {
    for (int i = 0; i < prefs->artwork_count; i++) free(prefs->artwork[i]);
    for (int i = 0; i < prefs->region_count; i++) free(prefs->regions[i]);
    prefs->artwork_count = prefs->region_count = 0;
}

/* ── Path resolution ─────────────────────────────────────────────────── */

/* Strip the extension from a rom filename to get the art basename
   (matches discovery's jw__title_from_filename). */
static void jw__art_base(const char *rom_name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", rom_name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* Resolve everything needed to land art for one item. dest_abs is the target
   PNG; image_db is the games.image_path value (relative on the primary
   source, absolute elsewhere). */
static int jw__resolve_paths(const jw__scrape_item *item,
                             char *rom_abs, size_t rom_abs_size,
                             char *rom_name, size_t rom_name_size,
                             char *dest_abs, size_t dest_abs_size,
                             char *image_db, size_t image_db_size) {
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(jw__w.sdcard_root, &sources) != 0 ||
        sources.count <= 0) {
        return -1;
    }
    if (jw_storage_resolve_path(&sources, item->rom_path,
                                rom_abs, rom_abs_size) != 0) {
        return -1;
    }

    char rom_copy[512];
    snprintf(rom_copy, sizeof(rom_copy), "%s", item->rom_path);
    snprintf(rom_name, rom_name_size, "%s", basename(rom_copy));

    char base[256];
    jw__art_base(rom_name, base, sizeof(base));

    const jw_storage_source *source =
        jw_storage_sources_find_for_path(&sources, rom_abs);
    if (!source) source = jw_storage_sources_primary(&sources);
    if (!source) return -1;

    int n = snprintf(dest_abs, dest_abs_size, "%s/%s/%s.png",
                     source->images_path, item->system, base);
    if (n <= 0 || n >= (int)dest_abs_size) return -1;

    char image_rel[512];
    snprintf(image_rel, sizeof(image_rel), "Images/%s/%s.png",
             item->system, base);
    return jw_storage_db_path_for_source(source, image_rel, dest_abs,
                                         image_db, image_db_size);
}

/* ── Queue helpers (mutex held) ──────────────────────────────────────── */

static bool jw__queue_contains(const char *system, const char *rom_path) {
    for (int i = 0; i < jw__w.count; i++) {
        const jw__scrape_item *it =
            &jw__w.queue[(jw__w.head + i) % JW__SCRAPE_QUEUE_MAX];
        if (strcmp(it->system, system) == 0 &&
            strcmp(it->rom_path, rom_path) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__active_matches(const char *system, const char *rom_path) {
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        const jw__active_slot *slot = &jw__w.active[i];
        if (!slot->used) continue;
        if ((!system || strcmp(slot->item.system, system) == 0) &&
            (!rom_path || strcmp(slot->item.rom_path, rom_path) == 0)) {
            return true;
        }
    }
    return false;
}

static int jw__queue_push(const char *system, const char *rom_path) {
    if (jw__w.count >= JW__SCRAPE_QUEUE_MAX) return -1;
    if (jw__queue_contains(system, rom_path) ||
        jw__active_matches(system, rom_path)) {
        return 0;
    }
    if (jw__w.count == 0 && jw__w.active_count == 0 && !jw__w.paused_quota) {
        /* New batch from idle: drop the previous batch's summary (it stays
           readable while idle so "scrape finished" is observable). */
        jw__w.total = jw__w.done = 0;
        jw__w.found = jw__w.not_found = jw__w.failed = 0;
        jw__w.message[0] = '\0';
    }
    /* A user-initiated start is the retry gesture that clears a quota pause
       (e.g. the day rolled over). */
    jw__w.paused_quota = false;
    jw__scrape_item *slot =
        &jw__w.queue[(jw__w.head + jw__w.count) % JW__SCRAPE_QUEUE_MAX];
    snprintf(slot->system, sizeof(slot->system), "%s", system);
    snprintf(slot->rom_path, sizeof(slot->rom_path), "%s", rom_path);
    jw__w.count++;
    jw__w.total++;
    return 1;
}

/* Remove queued items matching system (NULL = all) and rom_path (NULL = any
   in system). Returns the number removed. */
static int jw__queue_remove(const char *system, const char *rom_path) {
    int removed = 0;
    int kept = 0;
    for (int i = 0; i < jw__w.count; i++) {
        jw__scrape_item *it =
            &jw__w.queue[(jw__w.head + i) % JW__SCRAPE_QUEUE_MAX];
        bool match = (!system || strcmp(it->system, system) == 0) &&
                     (!rom_path || strcmp(it->rom_path, rom_path) == 0);
        if (match) {
            removed++;
            continue;
        }
        jw__scrape_item *dst =
            &jw__w.queue[(jw__w.head + kept) % JW__SCRAPE_QUEUE_MAX];
        if (dst != it) *dst = *it;
        kept++;
    }
    jw__w.count = kept;
    jw__w.total -= removed;
    return removed;
}

/* ── Worker pool ─────────────────────────────────────────────────────── */

static bool jw__error_is_quota(const char *msg) {
    return msg && (strstr(msg, "Daily quota exceeded") != NULL);
}

static bool jw__error_is_thread_limit(const char *msg) {
    return msg && (strstr(msg, "Thread limit reached") != NULL);
}

typedef enum {
    JW__SCRAPE_ITEM_DONE = 0,
    JW__SCRAPE_ITEM_PAUSED_QUOTA,
} jw__scrape_item_result;

static void jw__apply_thread_allowance(int max_threads) {
    if (max_threads <= 0)
        return;
    int permits = max_threads;
    if (permits > JW__SCRAPE_MAX_WORKERS) permits = JW__SCRAPE_MAX_WORKERS;
    if (permits < 1) permits = 1;

    pthread_mutex_lock(&jw__w.mu);
    if (permits != jw__w.permits) {
        jw__w.permits = permits;
        pthread_cond_broadcast(&jw__w.cv);
    }
    pthread_mutex_unlock(&jw__w.mu);
}

static int jw__queue_prepend_locked(const jw__scrape_item *item) {
    if (jw__w.count >= JW__SCRAPE_QUEUE_MAX)
        return -1;
    jw__w.head = (jw__w.head + JW__SCRAPE_QUEUE_MAX - 1) % JW__SCRAPE_QUEUE_MAX;
    jw__w.queue[jw__w.head] = *item;
    jw__w.count++;
    return 0;
}

static jw__scrape_item_result jw__process_item(const jw__scrape_item *item,
                                               atomic_int *interrupt) {
    char rom_abs[PATH_MAX], rom_name[256];
    char dest_abs[PATH_MAX], image_db[512];
    if (jw__resolve_paths(item, rom_abs, sizeof(rom_abs),
                          rom_name, sizeof(rom_name),
                          dest_abs, sizeof(dest_abs),
                          image_db, sizeof(image_db)) != 0) {
        pthread_mutex_lock(&jw__w.mu);
        jw__w.failed++;
        snprintf(jw__w.message, sizeof(jw__w.message),
                 "Could not resolve paths for %.200s", item->rom_path);
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_DONE;
    }

    int system_id = jw_scrape_platform_id(item->system);
    if (system_id < 0) {
        /* enqueue validates this; only reachable through races */
        pthread_mutex_lock(&jw__w.mu);
        jw__w.failed++;
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_DONE;
    }

    jw__scrape_prefs prefs;
    jw__prefs_load(&prefs);
    jw__apply_thread_allowance(prefs.max_threads);

    jw_ss_client client = {0};
    snprintf(client.username, sizeof(client.username), "%s", prefs.user);
    snprintf(client.password, sizeof(client.password), "%s", prefs.pass);
    client.interrupt = interrupt;

    jw_ss_result result;
    int rc = jw_ss_search_rom(&client, rom_name, rom_abs, system_id,
                              (const char *const *)prefs.artwork,
                              prefs.artwork_count,
                              (const char *const *)prefs.regions,
                              prefs.region_count, &result);
    if (rc == 0) {
        rc = jw_ss_download_media(&client, result.media_url, dest_abs,
                                  JW_SCRAPE_MAX_DIM);
    }
    jw__prefs_free(&prefs);

    if (rc == 0) {
        int db_rc = jw_db_set_game_image(jw__w.db_path, item->rom_path,
                                         image_db);
        if (db_rc != 0) {
            jw_log_info("scrape: image saved but no game row for %s (rc=%d)",
                        item->rom_path, db_rc);
        }
        jw_db_increment_setting(jw__w.db_path, "library.generation");

        pthread_mutex_lock(&jw__w.mu);
        jw__w.found++;
        /* Follow the account's thread allowance, capped by the pool. */
        if (result.max_threads > 0) {
            int permits = result.max_threads;
            if (permits > JW__SCRAPE_MAX_WORKERS) permits = JW__SCRAPE_MAX_WORKERS;
            if (permits != jw__w.permits) {
                jw__w.permits = permits;
                pthread_cond_broadcast(&jw__w.cv);
            }
        }
        /* Out of daily requests: pause before burning failed lookups. */
        if (result.max_requests > 0 &&
            result.requests_today >= result.max_requests) {
            jw__w.paused_quota = true;
            snprintf(jw__w.message, sizeof(jw__w.message),
                     "Daily quota exhausted (%d/%d)",
                     result.requests_today, result.max_requests);
        }
        pthread_mutex_unlock(&jw__w.mu);
        jw_log_info("scrape: %s/%s -> %s", item->system, rom_name, dest_abs);
        return JW__SCRAPE_ITEM_DONE;
    }

    pthread_mutex_lock(&jw__w.mu);
    if (rc == 1) {
        jw__w.not_found++;
    } else if (rc == -2) {
        /* cancelled mid-flight: counted as failed without a message */
        jw__w.failed++;
    } else {
        const char *msg = jw_ss_last_error();
        if (msg) {
            snprintf(jw__w.message, sizeof(jw__w.message), "%s", msg);
            if (jw__error_is_quota(msg)) {
                jw__w.paused_quota = true;
                pthread_mutex_unlock(&jw__w.mu);
                return JW__SCRAPE_ITEM_PAUSED_QUOTA;
            } else if (jw__error_is_thread_limit(msg) && jw__w.permits > 1) {
                /* The account allows fewer concurrent requests than we run;
                   back off a permit. */
                jw__w.permits--;
            }
        }
        jw__w.failed++;
    }
    pthread_mutex_unlock(&jw__w.mu);
    return JW__SCRAPE_ITEM_DONE;
}

static void *jw__worker_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&jw__w.mu);
    for (;;) {
        while (!jw__w.shutdown &&
               (jw__w.count == 0 || jw__w.paused_quota ||
                jw__w.active_count >= jw__w.permits)) {
            pthread_cond_wait(&jw__w.cv, &jw__w.mu);
        }
        if (jw__w.shutdown) break;

        jw__active_slot *slot = NULL;
        for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
            if (!jw__w.active[i].used) {
                slot = &jw__w.active[i];
                break;
            }
        }
        if (!slot) {
            /* active_count < permits <= MAX_WORKERS guarantees a free slot;
               defensive only. */
            pthread_cond_wait(&jw__w.cv, &jw__w.mu);
            continue;
        }

        slot->item = jw__w.queue[jw__w.head];
        slot->used = true;
        atomic_store(&slot->interrupt, 0);
        jw__w.head = (jw__w.head + 1) % JW__SCRAPE_QUEUE_MAX;
        jw__w.count--;
        jw__w.active_count++;
        pthread_mutex_unlock(&jw__w.mu);

        jw__scrape_item_result result =
            jw__process_item(&slot->item, &slot->interrupt);

        pthread_mutex_lock(&jw__w.mu);
        if (result == JW__SCRAPE_ITEM_PAUSED_QUOTA &&
            jw__queue_prepend_locked(&slot->item) != 0) {
            jw__w.failed++;
            result = JW__SCRAPE_ITEM_DONE;
        }
        slot->used = false;
        jw__w.active_count--;
        if (result == JW__SCRAPE_ITEM_DONE) {
            jw__w.done++;
        }
        /* Wake siblings: a permit freed up, or the drain just completed. */
        pthread_cond_broadcast(&jw__w.cv);
    }
    pthread_mutex_unlock(&jw__w.mu);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int jw_scrape_worker_start(const char *db_path, const char *sdcard_root) {
    pthread_mutex_lock(&jw__w.mu);
    if (jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        return 0;
    }
    if (!db_path || !db_path[0] || !sdcard_root || !sdcard_root[0]) {
        pthread_mutex_unlock(&jw__w.mu);
        return -1;
    }
    snprintf(jw__w.db_path, sizeof(jw__w.db_path), "%s", db_path);
    snprintf(jw__w.sdcard_root, sizeof(jw__w.sdcard_root), "%s", sdcard_root);
    if (!jw__w.queue) {
        jw__w.queue = calloc(JW__SCRAPE_QUEUE_MAX, sizeof(*jw__w.queue));
        if (!jw__w.queue) {
            pthread_mutex_unlock(&jw__w.mu);
            return -1;
        }
    }
    jw__w.shutdown = false;
    jw__w.permits = 1;
    jw__w.thread_count = 0;
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        if (pthread_create(&jw__w.threads[i], NULL, jw__worker_main, NULL) != 0) {
            break;
        }
        jw__w.thread_count++;
    }
    jw__w.running = jw__w.thread_count > 0;
    pthread_mutex_unlock(&jw__w.mu);
    return jw__w.running ? 0 : -1;
}

void jw_scrape_worker_stop(void) {
    pthread_mutex_lock(&jw__w.mu);
    if (!jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        return;
    }
    jw__w.shutdown = true;
    jw__w.count = 0;
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        if (jw__w.active[i].used) {
            atomic_store(&jw__w.active[i].interrupt, 1);
        }
    }
    pthread_cond_broadcast(&jw__w.cv);
    int thread_count = jw__w.thread_count;
    pthread_mutex_unlock(&jw__w.mu);

    for (int i = 0; i < thread_count; i++) {
        pthread_join(jw__w.threads[i], NULL);
    }

    pthread_mutex_lock(&jw__w.mu);
    jw__w.running = false;
    jw__w.thread_count = 0;
    pthread_mutex_unlock(&jw__w.mu);
}

int jw_scrape_enqueue_game(const char *system, const char *rom_path,
                           const char **error) {
    if (error) *error = NULL;
    if (!system || !system[0] || !rom_path || !rom_path[0]) {
        if (error) *error = "missing system or rom_path";
        return -1;
    }
    if (!jw_ss_available()) {
        if (error) *error = "scraping unavailable in this build";
        return -1;
    }
    if (jw_scrape_platform_id(system) < 0) {
        if (error) *error = "no ScreenScraper mapping for this system";
        return -1;
    }

    pthread_mutex_lock(&jw__w.mu);
    if (!jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        if (error) *error = "scrape worker not running";
        return -1;
    }
    int rc = jw__queue_push(system, rom_path);
    if (rc > 0) pthread_cond_broadcast(&jw__w.cv);
    pthread_mutex_unlock(&jw__w.mu);
    if (rc < 0) {
        if (error) *error = "scrape queue is full";
        return -1;
    }
    return rc;
}

int jw_scrape_enqueue_system(const char *system, bool missing_only,
                             const char **error) {
    if (error) *error = NULL;
    if (!system || !system[0]) {
        if (error) *error = "missing system";
        return -1;
    }
    if (!jw_ss_available()) {
        if (error) *error = "scraping unavailable in this build";
        return -1;
    }
    if (jw_scrape_platform_id(system) < 0) {
        if (error) *error = "no ScreenScraper mapping for this system";
        return -1;
    }

    jw_game_entry *games = calloc(JW__SCRAPE_LIST_MAX, sizeof(*games));
    if (!games) {
        if (error) *error = "out of memory";
        return -1;
    }
    int game_count = 0;
    if (jw_db_list_games_for_system(jw__w.db_path, system, games,
                                    JW__SCRAPE_LIST_MAX, &game_count) != 0) {
        free(games);
        if (error) *error = "could not list games for this system";
        return -1;
    }

    jw_storage_source_list sources;
    bool have_sources =
        missing_only &&
        jw_storage_sources_resolve(jw__w.sdcard_root, &sources) == 0 &&
        sources.count > 0;

    int enqueued = 0;
    pthread_mutex_lock(&jw__w.mu);
    if (!jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        free(games);
        if (error) *error = "scrape worker not running";
        return -1;
    }
    for (int i = 0; i < game_count; i++) {
        if (missing_only && have_sources) {
            /* Skip games whose art file is already on disk — checked at the
               path the scrape would write, so art that landed without a
               rescan still counts. */
            char rom_abs[PATH_MAX], art_abs[PATH_MAX];
            bool exists = false;
            if (jw_storage_resolve_path(&sources, games[i].rom_path,
                                        rom_abs, sizeof(rom_abs)) == 0) {
                char rom_copy[512], base[256];
                snprintf(rom_copy, sizeof(rom_copy), "%s", games[i].rom_path);
                jw__art_base(basename(rom_copy), base, sizeof(base));
                const jw_storage_source *source =
                    jw_storage_sources_find_for_path(&sources, rom_abs);
                if (!source) source = jw_storage_sources_primary(&sources);
                if (source &&
                    snprintf(art_abs, sizeof(art_abs), "%s/%s/%s.png",
                             source->images_path, system, base) <
                        (int)sizeof(art_abs) &&
                    access(art_abs, R_OK) == 0) {
                    exists = true;
                }
            }
            if (exists) continue;
        }
        int rc = jw__queue_push(system, games[i].rom_path);
        if (rc < 0) break;   /* queue full: enqueue what fit */
        enqueued += rc;
    }
    if (enqueued > 0) pthread_cond_broadcast(&jw__w.cv);
    pthread_mutex_unlock(&jw__w.mu);
    free(games);
    return enqueued;
}

void jw_scrape_status(jw_scrape_status_info *out) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&jw__w.mu);
    if (jw__w.paused_quota) {
        out->state = JW_SCRAPE_PAUSED_QUOTA;
    } else if (jw__w.count > 0 || jw__w.active_count > 0) {
        out->state = JW_SCRAPE_RUNNING;
    } else {
        out->state = JW_SCRAPE_IDLE;
    }
    out->total = jw__w.total;
    out->done = jw__w.done;
    out->found = jw__w.found;
    out->not_found = jw__w.not_found;
    out->failed = jw__w.failed;
    out->queued = jw__w.count;
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        if (jw__w.active[i].used) {
            char rom_copy[512];
            snprintf(rom_copy, sizeof(rom_copy), "%s",
                     jw__w.active[i].item.rom_path);
            snprintf(out->current_name, sizeof(out->current_name), "%s",
                     basename(rom_copy));
            snprintf(out->current_system, sizeof(out->current_system), "%s",
                     jw__w.active[i].item.system);
            break;
        }
    }
    snprintf(out->message, sizeof(out->message), "%s", jw__w.message);
    pthread_mutex_unlock(&jw__w.mu);
}

static int jw__cancel(const char *system, const char *rom_path) {
    pthread_mutex_lock(&jw__w.mu);
    int removed = jw__queue_remove(system, rom_path);
    if (!system && !rom_path) {
        /* Cancel-all is also the "stop showing the pause banner" gesture. */
        jw__w.paused_quota = false;
    }
    pthread_mutex_unlock(&jw__w.mu);
    return removed;
}

int jw_scrape_cancel_all(void) {
    return jw__cancel(NULL, NULL);
}

int jw_scrape_cancel_system(const char *system) {
    return jw__cancel(system, NULL);
}

int jw_scrape_cancel_game(const char *system, const char *rom_path) {
    return jw__cancel(system, rom_path);
}

bool jw_scrape_is_pending_game(const char *system, const char *rom_path) {
    pthread_mutex_lock(&jw__w.mu);
    bool pending = jw__queue_contains(system, rom_path) ||
                   jw__active_matches(system, rom_path);
    pthread_mutex_unlock(&jw__w.mu);
    return pending;
}

bool jw_scrape_is_pending_system(const char *system) {
    pthread_mutex_lock(&jw__w.mu);
    bool pending = jw__active_matches(system, NULL);
    for (int i = 0; i < jw__w.count && !pending; i++) {
        const jw__scrape_item *it =
            &jw__w.queue[(jw__w.head + i) % JW__SCRAPE_QUEUE_MAX];
        if (strcmp(it->system, system) == 0) pending = true;
    }
    pthread_mutex_unlock(&jw__w.mu);
    return pending;
}
