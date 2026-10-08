#include "internal/discovery/delete_job.h"
#include "internal/discovery/delete.h"
#include "internal/db/db.h"
#include "internal/db/relocation.h"
#include "internal/storage/health.h"
#include "internal/scrape/scrape_worker.h"
#include "cJSON.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct jw_delete_job {
    pthread_mutex_t mutex;
    pthread_t thread;
    bool started, cancelled, changed;
    bool attempted;
    jw_ipc_delete_phase phase;
    char db_path[JW_STORAGE_PATH_MAX], sdcard_root[JW_STORAGE_PATH_MAX];
    char source_id[32], rom_relpath[512], name[512], token[65];
    char error[512], readonly_source[32];
    jw_delete_plan plan;
    jw_delete_result result;
};

static bool jw__cancelled(void *context) {
    jw_delete_job *job = context;
    pthread_mutex_lock(&job->mutex);
    bool cancelled = job->cancelled;
    pthread_mutex_unlock(&job->mutex);
    return cancelled;
}

static void jw__finish(jw_delete_job *job, jw_ipc_delete_phase phase) {
    pthread_mutex_lock(&job->mutex);
    job->phase = job->cancelled && phase != JW_IPC_DELETE_DONE
        ? JW_IPC_DELETE_CANCELLED : phase;
    pthread_mutex_unlock(&job->mutex);
}

static int jw__writable(jw_delete_job *job, const char *path, const char *source) {
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *marker = getenv("JAWAKA_TEST_DELETE_READONLY_FILE");
    char blocked[32] = "";
    FILE *fp = marker ? fopen(marker, "r") : NULL;
    if (fp) { (void)fgets(blocked, sizeof(blocked), fp); fclose(fp); }
    blocked[strcspn(blocked, "\r\n")] = '\0';
    bool injected = !strcmp(blocked, source);
#else
    bool injected = false;
#endif
    if (!injected && jw_storage_path_writable(path, NULL, 0)) return 0;
    snprintf(job->readonly_source, sizeof(job->readonly_source), "%s",
             !strcmp(source, "primary") ? "launcher_sd" : source);
    snprintf(job->error, sizeof(job->error), "Your %s storage is unavailable or read-only.", source);
    return -1;
}

static int jw__build(jw_delete_job *job, jw_delete_plan *plan) {
    jw_storage_source_list *sources = calloc(1, sizeof(*sources));
    jw_ra_catalog *catalog = NULL;
    jw_game_entry selected, *games = NULL;
    jw_delete_owner *owners = NULL;
    char **protected = NULL;
    size_t count = 0, protected_count = 0;
    int rc = -1;
    if (!sources) { snprintf(job->error, sizeof(job->error), "Could not allocate storage sources."); return -1; }
    if (jw__writable(job, job->db_path, "primary") != 0) goto done;
    if (jw_storage_sources_resolve(job->sdcard_root, sources) != 0) {
        snprintf(job->error, sizeof(job->error), "Could not resolve storage sources.");
        goto done;
    }
    const jw_storage_source *source = jw_storage_sources_find_by_id(sources, job->source_id);
    if (!source || !source->available) {
        snprintf(job->error, sizeof(job->error), "Your selected ROM card is not mounted.");
        goto done;
    }
    if (jw__writable(job, source->roms_path, source->id) != 0) goto done;
    if (jw_db_get_game_by_source_relpath(job->db_path, job->source_id, job->rom_relpath, &selected) != 0 ||
        jw_db_list_indexed_games(job->db_path, &games, &count) != 0) {
        snprintf(job->error, sizeof(job->error), "Could not read your game's library identity.");
        goto done;
    }
    catalog = jw_ra_catalog_load(job->sdcard_root, job->error, sizeof(job->error));
    if (!catalog) goto done;
    owners = calloc(count ? count : 1, sizeof(*owners));
    protected = calloc(count ? count : 1, sizeof(*protected));
    if (!owners || !protected) {
        snprintf(job->error, sizeof(job->error), "Could not allocate the deletion preview.");
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        snprintf(owners[i].source_id, sizeof(owners[i].source_id), "%s", games[i].source_id);
        snprintf(owners[i].rom_relpath, sizeof(owners[i].rom_relpath), "%s", games[i].rom_relpath);
        snprintf(owners[i].name, sizeof(owners[i].name), "%s", games[i].name);
        snprintf(owners[i].system_id, sizeof(owners[i].system_id), "%s", games[i].system);
        const jw_storage_source *art_source = jw_storage_sources_find_by_id(sources, games[i].source_id);
        if (!art_source || !art_source->available) continue;
        const char *art_root = !strcmp(games[i].image_root_kind, "roms") ? art_source->roms_path :
            !strcmp(games[i].image_root_kind, "images") ? art_source->images_path : NULL;
        char path[JW_STORAGE_PATH_MAX];
        if (art_root && games[i].image_relpath[0]) {
            int n = snprintf(path, sizeof(path), "%s/%s", art_root, games[i].image_relpath);
            if (n < 0 || (size_t)n >= sizeof(path)) {
                snprintf(job->error, sizeof(job->error), "Your artwork path is too long to protect.");
                goto done;
            }
        } else if (games[i].image_path[0]) {
            if (jw_storage_resolve_path(sources, games[i].image_path, path, sizeof(path)) != 0) {
                snprintf(job->error, sizeof(job->error), "Could not resolve an artwork path to preserve.");
                goto done;
            }
        } else continue;
        protected[protected_count] = strdup(path);
        if (!protected[protected_count++]) goto done;
    }
    snprintf(job->name, sizeof(job->name), "%s", selected.name);
    rc = jw_delete_plan_build(sources, catalog, job->source_id, job->rom_relpath,
        selected.system, owners, count, (const char *const *)protected, protected_count,
        jw__cancelled, job, plan, job->error, sizeof(job->error));
    if (rc == 0) {
        for (size_t i = 0; i < plan->file_count; ++i) {
            if (plan->files[i].keep == JW_DELETE_REMOVE &&
                jw__writable(job, plan->files[i].path, plan->files[i].source_id) != 0) {
                rc = -1; break;
            }
        }
    }
done:
    for (size_t i = 0; i < protected_count; ++i) free(protected[i]);
    free(protected); free(owners); free(games);
    free(sources);
    jw_ra_catalog_free(catalog);
    return rc;
}

static void *jw__prepare(void *context) {
    jw_delete_job *job = context;
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *delay = getenv("JAWAKA_TEST_DELETE_PREPARE_DELAY_MS");
    int milliseconds = delay ? atoi(delay) : 0;
    if (milliseconds > 3000) milliseconds = 3000;
    for (int elapsed = 0; elapsed < milliseconds && !jw__cancelled(job); elapsed += 10)
        usleep(10000);
#endif
    int rc = jw__build(job, &job->plan);
    jw__finish(job, rc == 0 ? JW_IPC_DELETE_READY : JW_IPC_DELETE_ERROR);
    return NULL;
}

static int jw__guard(void *context, const jw_delete_file *file, char *error, size_t size) {
    jw_delete_job *job = context;
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *marker = getenv("JAWAKA_TEST_DELETE_FAIL_AFTER_FILE");
    FILE *fp = marker ? fopen(marker, "r") : NULL;
    unsigned limit = 0;
    bool fail = fp && fscanf(fp, "%u", &limit) == 1 && job->result.removed_count >= limit;
    if (fp) fclose(fp);
    if (fail) {
        snprintf(error, size, "Injected removal failure before %.160s", file->rom_relpath);
        return -1;
    }
#endif
    jw_storage_source_list *current = malloc(sizeof(*current));
    bool same = current && jw_storage_sources_resolve(job->sdcard_root, current) == 0 &&
        jw_delete_plan_sources_match(&job->plan, current);
    free(current);
    if (!same) {
        snprintf(error, size, "Your storage changed during deletion. Request a fresh preview.");
        return -1;
    }
    if (jw__writable(job, job->db_path, "primary") != 0 ||
        jw__writable(job, file->path, file->source_id) != 0) {
        if (error != job->error) snprintf(error, size, "%s", job->error);
        return -1;
    }
    return 0;
}

static int jw__cancel_scrapes(jw_delete_job *job) {
    jw_game_entry *games = NULL;
    size_t count = 0;
    if (jw_db_list_indexed_games(job->db_path, &games, &count) != 0) {
        snprintf(job->error, sizeof(job->error), "Could not check artwork work before deletion.");
        return -1;
    }
    int rc = 0;
    for (size_t g = 0; g < count && rc == 0; ++g) {
        for (size_t f = 0; f < job->plan.file_count; ++f) {
            const jw_delete_file *file = &job->plan.files[f];
            if (file->keep != JW_DELETE_REMOVE || strcmp(file->source_id, games[g].source_id) ||
                strcmp(file->rom_relpath, games[g].rom_relpath)) continue;
            jw_scrape_cancel_game(games[g].system, games[g].rom_path);
            for (int attempt = 0; attempt < 200 && jw_scrape_is_pending_game(games[g].system, games[g].rom_path); ++attempt)
                usleep(10000);
            if (jw_scrape_is_pending_game(games[g].system, games[g].rom_path)) {
                snprintf(job->error, sizeof(job->error), "Artwork scraping is stopping. Request a fresh preview when it finishes.");
                rc = -1;
            }
            break;
        }
    }
    free(games);
    return rc;
}

static void *jw__commit(void *context) {
    jw_delete_job *job = context;
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *delay = getenv("JAWAKA_TEST_DELETE_COMMIT_DELAY_MS");
    int milliseconds = delay ? atoi(delay) : 0;
    if (milliseconds > 3000) milliseconds = 3000;
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) usleep(10000);
#endif
    jw_delete_plan fresh = {0};
    sqlite3 *db = NULL;
    int rc = -1;
    bool changed = false, transaction = false;
    jw_db_rom_key *removed = NULL;
    if (jw__cancel_scrapes(job) != 0) goto done;
    if (jw_db_open(job->db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(job->error, sizeof(job->error), "Your library is busy or read-only. Request a fresh preview.");
        if (!jw_storage_path_writable(job->db_path, NULL, 0))
            snprintf(job->readonly_source, sizeof(job->readonly_source), "launcher_sd");
        goto done;
    }
    transaction = true;
    if (jw__build(job, &fresh) != 0) goto done;
    if (!jw_delete_plan_equal(&job->plan, &fresh)) {
        snprintf(job->error, sizeof(job->error), "Files or references changed. Request a fresh deletion preview.");
        goto done;
    }
    for (size_t i = 0; i < fresh.file_count; ++i) {
        if (fresh.files[i].keep != JW_DELETE_REMOVE) continue;
        if (jw_db_relocation_key_reserved(db, fresh.files[i].source_id, fresh.files[i].rom_relpath)) {
            snprintf(job->error, sizeof(job->error), "A file is being relocated. Request a fresh preview when it finishes.");
            goto done;
        }
    }
    removed = calloc(fresh.file_count ? fresh.file_count : 1, sizeof(*removed));
    if (!removed) { snprintf(job->error, sizeof(job->error), "Could not allocate deletion results."); goto done; }
    rc = jw_delete_execute(&fresh, jw__guard, job, &job->result, job->error, sizeof(job->error));
    size_t count = 0;
    for (size_t i = 0; i < fresh.file_count; ++i) {
        if (!job->result.completed || !job->result.completed[i]) continue;
        snprintf(removed[count].source_id, sizeof(removed[count].source_id), "%s", fresh.files[i].source_id);
        snprintf(removed[count++].rom_relpath, sizeof(removed[0].rom_relpath), "%s", fresh.files[i].rom_relpath);
    }
    changed = count > 0;
    if (jw_db_reconcile_removed_roms(db, removed, count) != 0 ||
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        size_t used = strlen(job->error);
        snprintf(job->error + used, sizeof(job->error) - used,
                 "%sLibrary cleanup failed after %zu files were removed. Refresh the library before retrying.",
                 used ? " " : "", job->result.removed_count);
        rc = -1;
    } else transaction = false;
done:
    if (transaction) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    jw_db_close(db);
    free(removed);
    jw_delete_plan_free(&fresh);
    pthread_mutex_lock(&job->mutex);
    job->changed = changed;
    job->phase = rc == 0 ? JW_IPC_DELETE_DONE : JW_IPC_DELETE_ERROR;
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}

static int jw__start_worker(jw_delete_job *job, void *(*worker)(void *)) {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return -1;
    /* Bounded directory/descriptor recursion exceeds macOS's 512 KB default. */
    int rc = pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
    if (rc == 0) rc = pthread_create(&job->thread, &attributes, worker, job);
    pthread_attr_destroy(&attributes);
    return rc;
}

jw_delete_job *jw_delete_job_start(const char *db_path, const char *sdcard_root,
                                  const char *source_id, const char *rom_relpath) {
    jw_delete_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    pthread_mutex_init(&job->mutex, NULL);
    job->phase = JW_IPC_DELETE_PREPARING;
    snprintf(job->db_path, sizeof(job->db_path), "%s", db_path);
    snprintf(job->sdcard_root, sizeof(job->sdcard_root), "%s", sdcard_root);
    snprintf(job->source_id, sizeof(job->source_id), "%s", source_id);
    snprintf(job->rom_relpath, sizeof(job->rom_relpath), "%s", rom_relpath);
    unsigned char random[32];
    FILE *fp = fopen("/dev/urandom", "rb");
    bool random_ok = fp && fread(random, 1, sizeof(random), fp) == sizeof(random);
    if (fp) fclose(fp);
    if (!random_ok) { jw_delete_job_destroy(job); return NULL; }
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(job->token + i * 2, 3, "%02x", random[i]);
    if (jw__start_worker(job, jw__prepare) != 0) {
        jw_delete_job_destroy(job); return NULL;
    }
    job->started = true;
    return job;
}

bool jw_delete_job_busy(jw_delete_job *job) {
    if (!job) return false;
    pthread_mutex_lock(&job->mutex);
    bool busy = job->phase == JW_IPC_DELETE_PREPARING || job->phase == JW_IPC_DELETE_COMMITTING;
    pthread_mutex_unlock(&job->mutex);
    return busy;
}

bool jw_delete_job_committing(jw_delete_job *job) {
    if (!job) return false;
    pthread_mutex_lock(&job->mutex);
    bool committing = job->phase == JW_IPC_DELETE_COMMITTING;
    pthread_mutex_unlock(&job->mutex);
    return committing;
}

bool jw_delete_job_changed(jw_delete_job *job) {
    if (!job) return false;
    pthread_mutex_lock(&job->mutex);
    bool changed = job->changed;
    job->changed = false;
    pthread_mutex_unlock(&job->mutex);
    return changed;
}

void jw_delete_job_cancel(jw_delete_job *job) {
    if (!job) return;
    pthread_mutex_lock(&job->mutex);
    if (job->phase != JW_IPC_DELETE_COMMITTING) {
        job->cancelled = true;
        job->token[0] = '\0';
        if (job->phase == JW_IPC_DELETE_READY) job->phase = JW_IPC_DELETE_CANCELLED;
    }
    pthread_mutex_unlock(&job->mutex);
}

void jw_delete_job_reject(jw_delete_job *job, const char *message, const char *source) {
    if (!job || jw_delete_job_busy(job)) return;
    pthread_mutex_lock(&job->mutex);
    job->token[0] = '\0';
    job->phase = JW_IPC_DELETE_ERROR;
    snprintf(job->error, sizeof(job->error), "%s", message);
    snprintf(job->readonly_source, sizeof(job->readonly_source), "%s", source ? source : "");
    pthread_mutex_unlock(&job->mutex);
}

int jw_delete_job_commit(jw_delete_job *job, const char *token) {
    if (!job) return -1;
    pthread_mutex_lock(&job->mutex);
    if (job->phase != JW_IPC_DELETE_READY || !token || !job->token[0] || strcmp(token, job->token)) {
        pthread_mutex_unlock(&job->mutex);
        return -1;
    }
    job->token[0] = '\0';
    job->phase = JW_IPC_DELETE_COMMITTING;
    job->attempted = true;
    pthread_mutex_unlock(&job->mutex);
    if (job->started) pthread_join(job->thread, NULL);
    job->started = jw__start_worker(job, jw__commit) == 0;
    if (!job->started) {
        snprintf(job->error, sizeof(job->error), "Could not start deletion. Request a fresh preview.");
        jw__finish(job, JW_IPC_DELETE_ERROR);
        return -1;
    }
    return 0;
}

cJSON *jw_delete_job_status(jw_delete_job *job) {
    if (!job) return NULL;
    pthread_mutex_lock(&job->mutex);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "rom-delete-status");
    cJSON_AddNumberToObject(reply, "phase", job->phase);
    cJSON_AddStringToObject(reply, "source_id", job->source_id);
    bool busy = job->phase == JW_IPC_DELETE_PREPARING || job->phase == JW_IPC_DELETE_COMMITTING;
    if (!busy) {
        cJSON_AddStringToObject(reply, "token", job->phase == JW_IPC_DELETE_READY ? job->token : "");
        cJSON_AddStringToObject(reply, "name", job->name);
        cJSON_AddStringToObject(reply, "error", job->error);
        cJSON_AddStringToObject(reply, "readonly_source", job->readonly_source);
        cJSON_AddStringToObject(reply, "missing_sources", job->plan.missing_sources);
        cJSON_AddBoolToObject(reply, "missing_descriptors", job->plan.missing_descriptor);
        cJSON_AddBoolToObject(reply, "writable_progress", job->plan.writable_image);
        cJSON_AddNumberToObject(reply, "disc_count", job->plan.disc_count);
        cJSON_AddNumberToObject(reply, "file_count", job->plan.remove_count);
        cJSON_AddNumberToObject(reply, "keep_count", job->plan.kept_count);
        cJSON_AddNumberToObject(reply, "bytes", job->attempted ? job->result.bytes : job->plan.bytes);
        cJSON_AddNumberToObject(reply, "removed_count", job->result.removed_count);
        cJSON_AddNumberToObject(reply, "absent_count", job->result.absent_count);
        cJSON *files = cJSON_AddArrayToObject(reply, "files");
        size_t shared = 0;
        for (size_t i = 0; i < job->plan.file_count; ++i) {
            const jw_delete_file *file = &job->plan.files[i];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "source_id", file->source_id);
            cJSON_AddStringToObject(item, "rom_relpath", file->rom_relpath);
            cJSON_AddNumberToObject(item, "size", file->size);
            cJSON_AddNumberToObject(item, "keep", file->keep);
            cJSON_AddBoolToObject(item, "missing", file->missing);
            cJSON_AddBoolToObject(item, "removed", job->result.completed && job->result.completed[i] && !file->missing);
            cJSON_AddItemToArray(files, item);
            if (file->keep == JW_DELETE_SHARED) ++shared;
        }
        cJSON_AddNumberToObject(reply, "shared_count", shared);
    }
    pthread_mutex_unlock(&job->mutex);
    return reply;
}

void jw_delete_job_destroy(jw_delete_job *job) {
    if (!job) return;
    jw_delete_job_cancel(job);
    if (job->started) pthread_join(job->thread, NULL);
    jw_delete_plan_free(&job->plan);
    jw_delete_result_free(&job->result);
    pthread_mutex_destroy(&job->mutex);
    free(job);
}
