/* flock(), kill(), setpgid(), PATH_MAX, clock_gettime() are hidden by glibc
 * under a bare -std=c11 without a broader feature-test macro. Matches the
 * convention used by every other module in this directory. Must precede
 * every #include, including the paired header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/supervisor.h"

#include "cJSON.h"
#include "internal/services/dup_ids.h"
#include "internal/services/launch.h"
#include "internal/services/lease.h"
#include "internal/services/ownership.h"
#include "internal/services/reservation.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long a freshly launched leader must stay alive before "starting"
 * settles into "running". A command that exits immediately is either an
 * ordinary failure or (with live descendants) a foreground-contract
 * violation; both are caught by the leader-exit path, this window just
 * keeps CTL-1 from reporting a one-tick "running" flash for a process
 * that is already on its way out. */
#define JW_SVC_STARTING_SETTLE_MS 1000LL

/* SVC-1: "while desired enablement remains true, retry the non-blocking
 * acquisition once per second." */
#define JW_SVC_LEASE_RETRY_MS 1000LL

struct jw_svc_supervisor {
    char *runtime_dir;
    char *logs_dir;
    char *state_dir;
    char *userdata_root;
    char **apps_scan_roots;      /* NULL-terminated, owned */
    int apps_scan_root_count;
    jw_svc_control_store *store;
    jw_svc_supervised *entries;  /* dynamic array, scan order */
    int count;
    int cap;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static long long jw__mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

/* Persistent CTL-1 timestamps must remain meaningful across daemon and device
 * restarts. CLOCK_MONOTONIC is reserved for in-process deadlines only. */
static long long jw__wall_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000000LL + (long long)(ts.tv_nsec / 1000LL);
}

static long long jw__wall_ms(void) {
    long long now_us = jw__wall_us();
    return now_us < 0 ? now_us : now_us / 1000LL;
}

static void jw__set_reason(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug ? slug : "unknown");
    }
}

static int jw__format(char *out, size_t out_size, const char *fmt, ...) {
    if (!out || out_size == 0) {
        return -1;
    }
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(out, out_size, fmt, args);
    va_end(args);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

static bool jw__is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *jw__strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1u;
    char *copy = malloc(n);
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

bool jw_svc_supervisor_service_id_is_safe(const char *service_id) {
    return service_id && service_id[0] &&
           strlen(service_id) <= JW_SVC_ID_MAX &&
           strchr(service_id, '/') == NULL &&
           strcmp(service_id, ".") != 0 && strcmp(service_id, "..") != 0;
}

int jw_svc_supervisor_bound_log_tail(int requested) {
    if (requested <= 0) {
        return JW_SVC_LOG_TAIL_DEFAULT;
    }
    return requested > JW_SVC_LOG_TAIL_MAX ? JW_SVC_LOG_TAIL_MAX : requested;
}

bool jw_svc_supervisor_join_scan_root(char *out, size_t out_size,
                                      const char *apps_path,
                                      const char *root_name) {
    if (!out || out_size == 0 || !apps_path || !apps_path[0] ||
        !root_name || !root_name[0]) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/%s", apps_path, root_name);
    return written >= 0 && (size_t)written < out_size;
}

bool jw_svc_supervisor_ctl_op_requires_id(const char *operation) {
    return operation &&
           (strcmp(operation, "status") == 0 ||
            strcmp(operation, "logs") == 0 ||
            strcmp(operation, "export-logs") == 0 ||
            strcmp(operation, "enable") == 0 ||
            strcmp(operation, "disable") == 0 ||
            strcmp(operation, "run") == 0 ||
            strcmp(operation, "stop") == 0 ||
            strcmp(operation, "restart") == 0);
}

/* Bounded string copy that never truncates without a NUL and never triggers
 * gcc's -Wformat-truncation (which rejects snprintf(dst, cap, "%s", src)
 * whenever src's provenance is an unbounded fixed array). */
static void jw__strcpy_bounded(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > dst_size - 1u) {
        n = dst_size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

const char *jw_svc_effective_state_name(jw_svc_effective_state state) {
    switch (state) {
    case JW_SVC_STATE_UNAVAILABLE:      return "unavailable";
    case JW_SVC_STATE_DISABLED:         return "disabled";
    case JW_SVC_STATE_STOPPED:          return "stopped";
    case JW_SVC_STATE_STALE_GENERATION: return "stale-generation";
    case JW_SVC_STATE_STARTING:         return "starting";
    case JW_SVC_STATE_RUNNING:          return "running";
    case JW_SVC_STATE_STOPPING:         return "stopping";
    case JW_SVC_STATE_BACKOFF:          return "backoff";
    case JW_SVC_STATE_FAILED:           return "failed";
    }
    return "unavailable";
}

bool jw_svc_supervisor_status_json(const jw_svc_supervised *e,
                                   struct cJSON *object) {
    cJSON *obj = (cJSON *)object;
    if (!e || !obj) {
        return false;
    }
    bool ok =
        cJSON_AddStringToObject(obj, "id", e->service_id) != NULL &&
        cJSON_AddStringToObject(obj, "state",
                                jw_svc_effective_state_name(e->state)) != NULL &&
        cJSON_AddBoolToObject(obj, "desired_enabled", e->desired_enabled) != NULL &&
        cJSON_AddBoolToObject(obj, "session_run", e->session_run) != NULL &&
        cJSON_AddBoolToObject(obj, "manifest_valid", e->manifest_valid) != NULL &&
        cJSON_AddBoolToObject(obj, "on_secondary_root",
                              e->on_secondary_root) != NULL;
    if (!ok) {
        return false;
    }
    if (!e->manifest_valid && e->reject_reason[0] &&
        !cJSON_AddStringToObject(obj, "reject_reason", e->reject_reason)) {
        return false;
    }

    cJSON *ownership = cJSON_CreateObject();
    if (!ownership) {
        return false;
    }
    if (e->pgid > 0 &&
        (!cJSON_AddNumberToObject(ownership, "pgid", (double)e->pgid) ||
         !cJSON_AddNumberToObject(ownership, "launch_instant_us",
                                  (double)e->launch_instant_us))) {
        cJSON_Delete(ownership);
        return false;
    }
    cJSON_AddItemToObject(obj, "ownership", ownership);

    const char *lease_state = e->lease_fd >= 0 ? "held"
                              : e->state == JW_SVC_STATE_STALE_GENERATION
                                    ? "stale-generation"
                                    : "unheld";
    if (!cJSON_AddStringToObject(obj, "generation_lease_state", lease_state) ||
        !cJSON_AddNumberToObject(obj, "last_transition_at_us",
                                 (double)e->control.last_transition_at_us) ||
        !cJSON_AddStringToObject(obj, "last_transition_reason",
                                 e->control.last_transition_reason) ||
        !cJSON_AddNumberToObject(obj, "restart_count",
                                 (double)e->control.restart_count) ||
        !cJSON_AddBoolToObject(obj, "breaker_open",
                               e->backoff.breaker_open) ||
        !cJSON_AddStringToObject(obj, "installed_package_id",
                                 e->control.installed_package_id) ||
        !cJSON_AddStringToObject(obj, "installed_package_version",
                                 e->installed_package_version)) {
        return false;
    }
    if (e->control.has_last_exit &&
        (!cJSON_AddNumberToObject(obj, "last_exit_code",
                                  (double)e->control.last_exit_code) ||
         !cJSON_AddNumberToObject(obj, "last_exit_at_us",
                                  (double)e->control.last_exit_at_us))) {
        return false;
    }
    if (e->state == JW_SVC_STATE_BACKOFF &&
        !cJSON_AddStringToObject(obj, "backoff_reason", "on-failure")) {
        return false;
    }
    if (e->state == JW_SVC_STATE_FAILED &&
        !cJSON_AddStringToObject(obj, "backoff_reason", "circuit-breaker")) {
        return false;
    }

    bool lifecycle_stop =
        e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_GAME;
    if (!cJSON_AddBoolToObject(obj, "lifecycle_stop_in_force",
                               lifecycle_stop)) {
        return false;
    }
    if (lifecycle_stop &&
        !cJSON_AddStringToObject(obj, "lifecycle_stop_reason", "game")) {
        return false;
    }
    jw_svc_lifecycle_game game_policy =
        e->pgid > 0 ? e->active_lifecycle_game
                    : e->manifest.lifecycle_game;
    const char *coordination =
        game_policy == JW_SVC_LIFECYCLE_GAME_NOTIFY && e->pgid > 0
            ? "unsubscribed" : "n/a";
    return cJSON_AddStringToObject(obj, "coordination", coordination) != NULL;
}

/* ------------------------------------------------------------------ */
/* entry lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void jw__entry_release_runtime(jw_svc_supervised *e) {
    if (!e) {
        return;
    }
    /* A lease acquire may legitimately return descriptor 0 when the daemon
     * was started with stdin closed. Fresh entries are explicitly initialized
     * to -1, so every non-negative value is owned and must be closed. */
    if (e->lease_fd >= 0) {
        close(e->lease_fd);
    }
    e->lease_fd = -1;
    e->pgid = -1;
    e->launch_instant_us = 0;
}

static void jw__entry_initialize(jw_svc_supervised *e) {
    if (!e) {
        return;
    }
    /* `e` is newly obtained from realloc() and contains indeterminate bytes.
     * Never inspect manifest_loaded or lease_fd before initialization. */
    memset(e, 0, sizeof(*e));
    e->pgid = -1;
    e->lease_fd = -1;
}

static jw_svc_supervised *jw__find_mut(jw_svc_supervisor *sup,
                                       const char *service_id) {
    if (!sup || !service_id) {
        return NULL;
    }
    for (int i = 0; i < sup->count; i++) {
        if (strcmp(sup->entries[i].service_id, service_id) == 0) {
            return &sup->entries[i];
        }
    }
    return NULL;
}

const jw_svc_supervised *jw_svc_supervisor_find(const jw_svc_supervisor *sup,
                                                const char *service_id) {
    if (!sup || !service_id) {
        return NULL;
    }
    for (int i = 0; i < sup->count; i++) {
        if (strcmp(sup->entries[i].service_id, service_id) == 0) {
            return &sup->entries[i];
        }
    }
    return NULL;
}

int jw_svc_supervisor_count(const jw_svc_supervisor *sup) {
    return sup ? sup->count : 0;
}

const jw_svc_supervised *jw_svc_supervisor_at(const jw_svc_supervisor *sup,
                                              int index) {
    if (!sup || index < 0 || index >= sup->count) {
        return NULL;
    }
    return &sup->entries[index];
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

void jw_svc_supervisor_close(jw_svc_supervisor *sup) {
    if (!sup) {
        return;
    }
    for (int i = 0; i < sup->count; i++) {
        if (sup->entries[i].manifest_loaded) {
            jw_service_manifest_destroy(&sup->entries[i].manifest);
        }
        if (sup->entries[i].lease_fd >= 0) {
            close(sup->entries[i].lease_fd);
        }
    }
    free(sup->entries);
    for (int i = 0; i < sup->apps_scan_root_count; i++) {
        free(sup->apps_scan_roots[i]);
    }
    free(sup->apps_scan_roots);
    free(sup->runtime_dir);
    free(sup->logs_dir);
    free(sup->state_dir);
    free(sup->userdata_root);
    if (sup->store) {
        jw_svc_control_store_close(sup->store);
    }
    free(sup);
}

jw_svc_supervisor *jw_svc_supervisor_open(const char *runtime_dir,
                                          const char *logs_dir,
                                          const char *state_dir,
                                          const char *const *apps_scan_roots,
                                          const char *userdata_root,
                                          char *reason, size_t reason_size) {
    if (!runtime_dir || !runtime_dir[0] || !state_dir || !state_dir[0] ||
        !apps_scan_roots) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return NULL;
    }

    jw_svc_supervisor *sup = calloc(1, sizeof(*sup));
    if (!sup) {
        jw__set_reason(reason, reason_size, "out-of-memory");
        return NULL;
    }
    sup->runtime_dir = jw__strdup(runtime_dir);
    sup->logs_dir = jw__strdup(logs_dir ? logs_dir : "");
    sup->state_dir = jw__strdup(state_dir);
    sup->userdata_root = jw__strdup(userdata_root ? userdata_root : "");
    if (!sup->runtime_dir || !sup->logs_dir || !sup->state_dir ||
        !sup->userdata_root) {
        jw__set_reason(reason, reason_size, "out-of-memory");
        jw_svc_supervisor_close(sup);
        return NULL;
    }

    int n = 0;
    while (apps_scan_roots[n]) {
        n++;
    }
    sup->apps_scan_roots = calloc((size_t)n + 1u, sizeof(char *));
    if (!sup->apps_scan_roots) {
        jw__set_reason(reason, reason_size, "out-of-memory");
        jw_svc_supervisor_close(sup);
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        sup->apps_scan_roots[i] = jw__strdup(apps_scan_roots[i]);
        if (!sup->apps_scan_roots[i]) {
            jw__set_reason(reason, reason_size, "out-of-memory");
            jw_svc_supervisor_close(sup);
            return NULL;
        }
        sup->apps_scan_root_count++;
    }

    char db_path[PATH_MAX];
    if (jw__format(db_path, sizeof(db_path), "%s/services-control.db",
                   state_dir) != 0) {
        jw__set_reason(reason, reason_size, "path-too-long");
        jw_svc_supervisor_close(sup);
        return NULL;
    }
    char store_reason[JW_SVC_REASON_BUF];
    if (!jw_svc_control_store_open(db_path, &sup->store,
                                   store_reason, sizeof(store_reason))) {
        jw__set_reason(reason, reason_size, "control-store-failed");
        jw_svc_supervisor_close(sup);
        return NULL;
    }
    /* SVC-1: session Run/Stop must not survive a daemon restart, even
     * though it shares a row with the persistent fields. */
    if (!jw_svc_control_store_clear_all_sessions(sup->store,
                                                 store_reason,
                                                 sizeof(store_reason))) {
        jw__set_reason(reason, reason_size, "control-store-failed");
        jw_svc_supervisor_close(sup);
        return NULL;
    }
    return sup;
}

/* ------------------------------------------------------------------ */
/* persistence                                                         */
/* ------------------------------------------------------------------ */

static bool jw__persist(jw_svc_supervisor *sup, jw_svc_supervised *e,
                        const char *transition_reason) {
    e->control.last_transition_at_us = jw__wall_us();
    if (transition_reason) {
        snprintf(e->control.last_transition_reason,
                 sizeof(e->control.last_transition_reason), "%s",
                 transition_reason);
    }
    e->control.breaker_open = e->backoff.breaker_open;
    e->control.backoff_failure_count = e->backoff.count;
    for (int i = 0; i < e->backoff.count && i < JW_SVC_CONTROL_BACKOFF_TRACKED;
         i++) {
        e->control.backoff_failure_times_us[i] =
            e->backoff.failure_times_ms[i] * 1000LL;
    }
    char store_reason[JW_SVC_REASON_BUF];
    if (!jw_svc_control_store_put(sup->store, e->service_id, &e->control,
                                  store_reason, sizeof(store_reason))) {
        return false;
    }
    e->control_loaded = true;
    return true;
}

static void jw__load_control(jw_svc_supervisor *sup, jw_svc_supervised *e) {
    char store_reason[JW_SVC_REASON_BUF];
    if (!jw_svc_control_store_get(sup->store, e->service_id, &e->control,
                                  &e->control_loaded,
                                  store_reason, sizeof(store_reason))) {
        e->control_loaded = false;
        memset(&e->control, 0, sizeof(e->control));
    }
    e->desired_enabled = e->control.start_with_leaf;
    e->session_run = e->control.session_run; /* already cleared at open */
    /* Persistent enablement is consumed once at daemon startup. It is not a
     * perpetual keep-alive bit: CTL-1 Stop and lifecycle stops must win for
     * the rest of this session. */
    e->autostart_pending = e->desired_enabled;
    e->backoff.breaker_open = e->control.breaker_open;
    e->backoff.count = e->control.backoff_failure_count;
    for (int i = 0; i < e->control.backoff_failure_count &&
                i < JW_SVC_BACKOFF_TRACKED; i++) {
        e->backoff.failure_times_ms[i] =
            e->control.backoff_failure_times_us[i] / 1000LL;
    }
}

/* ------------------------------------------------------------------ */
/* scan                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char service_id[JW_SVC_SUPERVISOR_ID_BUF];
    char pak_root[PATH_MAX];
    bool on_secondary_root;
    bool valid;
    char reject_reason[JW_SVC_REASON_BUF];
    char package_version[32];
    jw_service_manifest manifest;
    bool manifest_loaded;
} jw__scan_candidate;

static bool jw__entry_available(const jw_svc_supervised *e) {
    return e && e->pak_present && e->manifest_valid &&
           !e->on_secondary_root;
}

static jw_svc_effective_state jw__entry_idle_state(
    const jw_svc_supervised *e) {
    if (!jw__entry_available(e)) {
        return JW_SVC_STATE_UNAVAILABLE;
    }
    return e->desired_enabled ? JW_SVC_STATE_STOPPED
                              : JW_SVC_STATE_DISABLED;
}

static void jw__load_stale_policy(jw_svc_supervisor *sup,
                                  jw_svc_supervised *e);

static void jw__candidate_free(jw__scan_candidate *c) {
    if (c && c->manifest_loaded) {
        jw_service_manifest_destroy(&c->manifest);
        c->manifest_loaded = false;
    }
}

/* Reads a pak's pak.json into a heap buffer. Returns NULL when the file is
 * absent, unreadable, or larger than 1 MiB (a pak.json has no business
 * being that large; refusing it keeps a hostile or corrupt manifest from
 * making the daemon allocate unbounded memory). */
static char *jw__read_pak_json(const char *pak_root) {
    char path[PATH_MAX];
    if (jw__format(path, sizeof(path), "%s/pak.json", pak_root) != 0) {
        return NULL;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    char *buf = NULL;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size > 0 && size <= 1024L * 1024L &&
            fseek(fp, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)size + 1u);
            if (buf) {
                size_t got = fread(buf, 1, (size_t)size, fp);
                buf[got] = '\0';
                if (got != (size_t)size) {
                    free(buf);
                    buf = NULL;
                }
            }
        }
    }
    fclose(fp);
    return buf;
}

/* Does this pak.json declare a service object at all? A pak without one is
 * an ordinary foreground app, not a service candidate, and must not be
 * flagged for service validation failures. */
static bool jw__pak_declares_service(const char *pak_json_text) {
    cJSON *root = cJSON_Parse(pak_json_text);
    if (!root) {
        return false;
    }
    cJSON *svc = cJSON_GetObjectItemCaseSensitive(root, "service");
    bool declares = svc != NULL;
    cJSON_Delete(root);
    return declares;
}

static void jw__scan_one_root(jw_svc_supervisor *sup, const char *root,
                              bool secondary, jw__scan_candidate **cands,
                              int *count, int *cap) {
    DIR *dir = opendir(root);
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char pak_root[PATH_MAX];
        if (jw__format(pak_root, sizeof(pak_root), "%s/%s", root,
                       de->d_name) != 0 || !jw__is_dir(pak_root)) {
            continue;
        }
        char *text = jw__read_pak_json(pak_root);
        if (!text) {
            continue;
        }
        bool declares = jw__pak_declares_service(text);
        if (!declares) {
            free(text);
            continue; /* ordinary foreground app pak */
        }
        if (*count >= *cap) {
            int new_cap = *cap > 0 ? *cap * 2 : 8;
            jw__scan_candidate *grown =
                realloc(*cands, (size_t)new_cap * sizeof(**cands));
            if (!grown) {
                free(text);
                continue;
            }
            *cands = grown;
            *cap = new_cap;
        }
        jw__scan_candidate *c = &(*cands)[*count];
        memset(c, 0, sizeof(*c));
        c->on_secondary_root = secondary;
        jw__strcpy_bounded(c->pak_root, sizeof(c->pak_root), pak_root);

        char reason[JW_SVC_REASON_BUF] = {0};
        const char *userdata = sup->userdata_root[0]
                                   ? sup->userdata_root : NULL;
        c->valid = jw_service_manifest_validate(text, pak_root, userdata,
                                                &c->manifest, reason,
                                                sizeof(reason));
        if (c->valid) {
            c->manifest_loaded = true;
            jw__strcpy_bounded(c->service_id, sizeof(c->service_id), c->manifest.id);
        } else {
            jw__strcpy_bounded(c->reject_reason, sizeof(c->reject_reason),
                          reason[0] ? reason : "invalid-manifest");
            /* An invalid service is still tracked (CTL-1 reports it as
             * "unavailable", it is not silently dropped), keyed by its
             * declared service.id when that is recoverable from the raw
             * JSON, else by the pak directory name trimmed of .pak as a
             * best-effort identity for logs. */
            const char *id_src = NULL;
            cJSON *raw = cJSON_Parse(text);
            if (raw) {
                cJSON *svc = cJSON_GetObjectItemCaseSensitive(raw, "service");
                cJSON *sid = svc ? cJSON_GetObjectItemCaseSensitive(svc, "id")
                                 : NULL;
                if (sid && cJSON_IsString(sid) && sid->valuestring &&
                    sid->valuestring[0]) {
                    id_src = sid->valuestring;
                }
            }
            if (id_src && jw_svc_supervisor_service_id_is_safe(id_src)) {
                jw__strcpy_bounded(c->service_id, sizeof(c->service_id), id_src);
            } else {
                jw__strcpy_bounded(c->service_id, sizeof(c->service_id), de->d_name);
                size_t len = strlen(c->service_id);
                if (len > 4 && strcmp(c->service_id + len - 4, ".pak") == 0) {
                    c->service_id[len - 4] = '\0';
                }
            }
            if (raw) {
                cJSON_Delete(raw);
            }
        }

        /* Best-effort package version for the installed-package-identity
         * control field. */
        cJSON *root_json = cJSON_Parse(text);
        if (root_json) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(root_json,
                                                        "pak_version");
            if (cJSON_IsString(v) && v->valuestring) {
                jw__strcpy_bounded(c->package_version, sizeof(c->package_version),
                          v->valuestring);
            }
            cJSON_Delete(root_json);
        }
        free(text);
        (*count)++;
    }
    closedir(dir);
    (void)sup;
}

int jw_svc_supervisor_scan(jw_svc_supervisor *sup) {
    if (!sup) {
        return -1;
    }

    jw__scan_candidate *cands = NULL;
    int cand_count = 0, cand_cap = 0;
    for (int i = 0; i < sup->apps_scan_root_count; i++) {
        /* The first configured root is the Primary; any later root is a
         * Secondary (SVC-1: a service on a Secondary root is reported but
         * not startable on MLP1). */
        jw__scan_one_root(sup, sup->apps_scan_roots[i], i > 0,
                          &cands, &cand_count, &cand_cap);
    }

    /* Cross-manifest duplicate service.id: "Duplicates make both
     * unavailable." Only validated manifests participate (an invalid one
     * already carries its own rejection). */
    if (cand_count > 1) {
        const char **ids = calloc((size_t)cand_count, sizeof(char *));
        bool *dup = calloc((size_t)cand_count, sizeof(bool));
        if (ids && dup) {
            for (int i = 0; i < cand_count; i++) {
                ids[i] = cands[i].valid ? cands[i].service_id : NULL;
            }
            if (jw_svc_find_duplicate_ids(ids, (size_t)cand_count, dup)) {
                for (int i = 0; i < cand_count; i++) {
                    if (dup[i] && cands[i].valid) {
                        jw_service_manifest_destroy(&cands[i].manifest);
                        cands[i].manifest_loaded = false;
                        cands[i].valid = false;
                        snprintf(cands[i].reject_reason,
                                 sizeof(cands[i].reject_reason), "%s",
                                 "duplicate-service-id");
                    }
                }
            }
        }
        free(ids);
        free(dup);
    }

    /* Reconcile against the current set: a running service whose pak
     * vanished or went invalid keeps its owned group but becomes
     * unavailable for new actions. */
    for (int i = 0; i < sup->count; i++) {
        sup->entries[i].pak_present = false;
    }
    for (int i = 0; i < cand_count; i++) {
        jw__scan_candidate *c = &cands[i];
        jw_svc_supervised *e = jw__find_mut(sup, c->service_id);
        /* A malformed manifest may still contain the exact id of a valid
         * service. It must not overwrite that valid entry merely because its
         * directory happened to sort later in readdir() order. */
        if (e && !c->valid && e->manifest_valid) {
            jw__candidate_free(c);
            continue;
        }
        if (!e) {
            if (sup->count >= sup->cap) {
                int new_cap = sup->cap > 0 ? sup->cap * 2 : 8;
                jw_svc_supervised *grown =
                    realloc(sup->entries,
                            (size_t)new_cap * sizeof(*sup->entries));
                if (!grown) {
                    jw__candidate_free(c);
                    continue;
                }
                sup->entries = grown;
                sup->cap = new_cap;
            }
            e = &sup->entries[sup->count++];
            jw__entry_initialize(e);
            jw__strcpy_bounded(e->service_id, sizeof(e->service_id), c->service_id);
            e->state = JW_SVC_STATE_DISABLED;
            jw__load_control(sup, e);
        } else if (e->manifest_loaded) {
            jw_service_manifest_destroy(&e->manifest);
            e->manifest_loaded = false;
        }
        e->pak_present = true;
        e->manifest_valid = c->valid;
        e->on_secondary_root = c->on_secondary_root;
        jw__strcpy_bounded(e->pak_root, sizeof(e->pak_root), c->pak_root);
        jw__strcpy_bounded(e->reject_reason, sizeof(e->reject_reason), c->reject_reason);
        jw__strcpy_bounded(e->installed_package_version,
                           sizeof(e->installed_package_version),
                           c->package_version);
        /* Bounded copies: a service.id is at most JW_SVC_ID_MAX (128) but
         * the control field caps at JW_SVC_CONTROL_PACKAGE_ID_MAX (127), so
         * copy with an explicit length bound instead of snprintf("%s") --
         * which gcc's -Wformat-truncation rejects as potentially lossy. */
        jw__strcpy_bounded(e->control.installed_package_id,
                           sizeof(e->control.installed_package_id),
                           c->service_id);
        jw__strcpy_bounded(e->control.installed_package_version,
                           sizeof(e->control.installed_package_version),
                           c->package_version);
        if (c->valid) {
            e->manifest = c->manifest;
            e->manifest_loaded = true;
            c->manifest_loaded = false; /* ownership moved */
        }
        if (!jw__entry_available(e)) {
            e->state = JW_SVC_STATE_UNAVAILABLE;
        } else if (e->state == JW_SVC_STATE_UNAVAILABLE) {
            if (e->pgid > 0) {
                e->state = (e->reap_pending || e->stop_kill_sent)
                               ? JW_SVC_STATE_STOPPING
                               : JW_SVC_STATE_RUNNING;
            } else {
                e->state = jw__entry_idle_state(e);
            }
        }
        jw__candidate_free(c);
    }
    for (int i = 0; i < sup->count; i++) {
        if (!sup->entries[i].pak_present) {
            sup->entries[i].manifest_valid = false;
            jw__strcpy_bounded(sup->entries[i].reject_reason,
                          sizeof(sup->entries[i].reject_reason),
                          "package-missing");
            sup->entries[i].state = JW_SVC_STATE_UNAVAILABLE;
        }
    }
    /* Detect survivors even when they were launched only for the old daemon's
     * session and persistent enablement is false. Without this probe a daemon
     * restart would clear session_run and silently forget a still-live writer
     * until somebody explicitly tried to Run it again. */
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (e->pgid > 0 || !jw__entry_available(e)) {
            continue;
        }
        char lease_reason[JW_SVC_REASON_BUF];
        int lease_fd = jw_svc_lease_acquire(sup->runtime_dir, e->service_id,
                                            lease_reason,
                                            sizeof(lease_reason));
        if (lease_fd >= 0) {
            close(lease_fd);
        } else if (strcmp(lease_reason, "stale-generation") == 0) {
            jw__load_stale_policy(sup, e);
            e->state = JW_SVC_STATE_STALE_GENERATION;
            e->lease_retry_next_ms =
                jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
        }
    }
    free(cands);
    return sup->count;
}

/* ------------------------------------------------------------------ */
/* launch / stop                                                       */
/* ------------------------------------------------------------------ */

/* Builds the small service-specific layer of the child environment as the
 * JSON object launch.c expects. The inherited daemon environment (Leaf
 * runtime paths) is snapshotted by launch.c itself; here we only add what
 * SVC-1 supervision step 3 names explicitly: the service runtime dir. */
static char *jw__build_env_json(jw_svc_supervisor *sup,
                                const jw_svc_supervised *e) {
    cJSON *env = cJSON_CreateObject();
    if (!env) {
        return NULL;
    }
    char svc_dir[PATH_MAX];
    if (jw__format(svc_dir, sizeof(svc_dir), "%s/services/%s",
                   sup->runtime_dir, e->service_id) != 0 ||
        !cJSON_AddStringToObject(env, "UMRK_SERVICE_RUNTIME_DIR", svc_dir) ||
        !cJSON_AddStringToObject(env, "UMRK_SERVICE_ID", e->service_id)) {
        cJSON_Delete(env);
        return NULL;
    }
    char *text = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    return text;
}

/* Called only after jw_svc_group_absent() proved that no non-zombie group
 * member remains. Blocking here cannot wait on a live leader: the only child
 * left to collect is the deliberately held zombie reservation. */
static bool jw__reap_verified_leader(pid_t leader, int *out_status) {
    int status = 0;
    pid_t reaped;
    do {
        reaped = waitpid(leader, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    if (reaped != leader) {
        return false;
    }
    if (out_status) {
        *out_status = status;
    }
    return true;
}

/* A stale generation is report-only and must never be signalled by this
 * daemon. Its last atomic policy snapshot still governs lifecycle decisions;
 * if the snapshot is missing/corrupt, SVC-1 requires the fail-safe "stop"
 * behavior. */
static void jw__load_stale_policy(jw_svc_supervisor *sup,
                                  jw_svc_supervised *e) {
    e->active_lifecycle_game = JW_SVC_LIFECYCLE_GAME_STOP;
    jw_svc_reservation res;
    char reason[JW_SVC_REASON_BUF];
    if (!jw_svc_reservation_read(sup->runtime_dir, e->service_id, &res,
                                 reason, sizeof(reason))) {
        return;
    }
    if (res.game_policy == JW_SVC_RESERVATION_GAME_IGNORE) {
        e->active_lifecycle_game = JW_SVC_LIFECYCLE_GAME_IGNORE;
    } else if (res.game_policy == JW_SVC_RESERVATION_GAME_NOTIFY) {
        e->active_lifecycle_game = JW_SVC_LIFECYCLE_GAME_NOTIFY;
    }
}

/* One supervised launch attempt: acquire the generation lease, open the
 * rotating log, fork/exec, record the ownership reservation. On success the
 * entry owns a live pgid and the daemon-held lease fd. */
static bool jw__start_generation(jw_svc_supervisor *sup,
                                 jw_svc_supervised *e,
                                 char *reason, size_t reason_size) {
    char slug[JW_SVC_REASON_BUF];

    /* The lease is the no-overlap gate. Never start without it. */
    int lease_fd = jw_svc_lease_acquire(sup->runtime_dir, e->service_id,
                                        slug, sizeof(slug));
    if (lease_fd < 0) {
        if (strcmp(slug, "stale-generation") == 0) {
            jw__load_stale_policy(sup, e);
        }
        jw__set_reason(reason, reason_size,
                       strcmp(slug, "stale-generation") == 0
                           ? "stale-generation" : "lease-failed");
        return false;
    }

    int log_fd = -1;
    if (sup->logs_dir[0]) {
        log_fd = jw_svc_launch_open_log(sup->logs_dir, e->service_id,
                                        slug, sizeof(slug));
        /* A failed log open is not fatal: launch.c logs to /dev/null
         * instead rather than leaving the service on Jawaka's stdout. */
    }

    char run_abs[PATH_MAX];
    if (jw__format(run_abs, sizeof(run_abs), "%s/%s", e->pak_root,
                   e->manifest.run_path) != 0) {
        if (log_fd >= 0) close(log_fd);
        close(lease_fd);
        jw__set_reason(reason, reason_size, "path-too-long");
        return false;
    }

    const char *args[JW_SVC_MAX_ARGS];
    for (int i = 0; i < e->manifest.run_args_count; i++) {
        args[i] = e->manifest.run_args[i];
    }

    char *env_json = jw__build_env_json(sup, e);
    if (!env_json) {
        if (log_fd >= 0) close(log_fd);
        close(lease_fd);
        jw__set_reason(reason, reason_size, "launch-failed");
        return false;
    }
    jw_svc_launch_request req = {
        .run_path_abs = run_abs,
        .args = args,
        .args_count = e->manifest.run_args_count,
        .env_json = env_json,
        .lease_fd = lease_fd,
        .log_fd = log_fd,
    };
    char launch_reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, launch_reason, sizeof(launch_reason));
    free(env_json);
    if (log_fd >= 0) {
        close(log_fd); /* the child holds its own duplicate */
    }
    if (pid < 0) {
        close(lease_fd);
        jw__set_reason(reason, reason_size, "launch-failed");
        return false;
    }

    /* Ownership reservation: leader pid == pgid, monotonic launch instant,
     * normalized lifecycle-policy snapshot. Written before the entry is
     * marked running so a crash between fork and here still leaves no
     * running group without a record. */
    long long launch_instant_us = jw_svc_reservation_now_us();
    jw_svc_reservation res = {
        .pgid = pid,
        .launch_instant_us = launch_instant_us,
        .game_policy =
            e->manifest.lifecycle_game == JW_SVC_LIFECYCLE_GAME_STOP
                ? JW_SVC_RESERVATION_GAME_STOP
                : e->manifest.lifecycle_game == JW_SVC_LIFECYCLE_GAME_NOTIFY
                      ? JW_SVC_RESERVATION_GAME_NOTIFY
                      : JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = e->manifest.stop_on_storage_change,
        .stop_on_suspend = e->manifest.stop_on_suspend,
    };
    if (!jw_svc_reservation_write(sup->runtime_dir, e->service_id, &res,
                                  slug, sizeof(slug))) {
        /* The group is live but unrecorded: stop it now rather than run
         * an untracked service, per SVC-1's "never an unreserved group". */
        jw_svc_stop_result stopped =
            jw_svc_stop_group(pid, e->manifest.stop_grace_ms,
                              jw_svc_group_absent);
        if (stopped.verified_absent &&
            jw__reap_verified_leader(pid, NULL)) {
            close(lease_fd);
        } else {
            /* Keep the exact child identity and lease in memory until later
             * ticks prove absence. Closing either here would lose the only
             * safe handle on a still-live writer after the disk write failed. */
            e->lease_fd = lease_fd;
            e->pgid = pid;
            e->launch_instant_us = launch_instant_us;
            e->active_stop_grace_ms = e->manifest.stop_grace_ms;
            e->active_restart_on_failure = false;
            e->active_lifecycle_game = e->manifest.lifecycle_game;
            e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
            e->state = JW_SVC_STATE_STOPPING;
            e->stop_kill_sent = stopped.escalated_to_kill;
            e->stopping_since_ms = jw__mono_ms();
            e->reap_pending = stopped.verified_absent;
        }
        jw__set_reason(reason, reason_size, "reservation-failed");
        return false;
    }

    e->lease_fd = lease_fd;
    e->pgid = pid;
    e->launch_instant_us = launch_instant_us;
    e->active_stop_grace_ms = e->manifest.stop_grace_ms;
    e->active_restart_on_failure = e->manifest.restart_on_failure;
    e->active_lifecycle_game = e->manifest.lifecycle_game;
    e->autostart_pending = false;
    e->reap_pending = false;
    e->stop_kill_sent = false;
    /* No stop has been requested for the fresh generation: if its leader
     * exits now that is a genuine crash, eligible for on-failure restart. */
    e->pending_stop_reason = JW_SVC_STOP_NONE;
    e->state = JW_SVC_STATE_STARTING;
    e->stopping_since_ms = jw__mono_ms();
    return true;
}

/* Runs the stop sequence against the entry's owned group, then reaps the
 * leader and releases the lease and reservation once absence is verified.
 * SVC-1's order is reservation -> verify -> reap, never reap -> verify. */
static bool jw__stop_and_reap(jw_svc_supervisor *sup, jw_svc_supervised *e,
                              char *reason, size_t reason_size) {
    if (e->pgid <= 0) {
        jw__set_reason(reason, reason_size, "not-running");
        return false;
    }
    jw_svc_stop_result res =
        jw_svc_stop_group(e->pgid, e->active_stop_grace_ms,
                          jw_svc_group_absent);
    if (!res.verified_absent) {
        e->state = JW_SVC_STATE_STOPPING;
        e->stopping_since_ms = jw__mono_ms();
        e->stop_kill_sent = true;
        jw__set_reason(reason, reason_size, "stop-failed");
        return false;
    }
    /* Verified writer-free: now, and only now, reap the leader and release
     * the reservation. Reaping first would free the pgid for recycling. */
    int status = 0;
    if (!jw__reap_verified_leader(e->pgid, &status)) {
        e->state = JW_SVC_STATE_STOPPING;
        e->reap_pending = true;
        jw__set_reason(reason, reason_size, "stop-failed");
        return false;
    }
    e->reap_pending = false;
    jw__entry_release_runtime(e);
    e->state = jw__entry_idle_state(e);
    (void)sup;
    return true;
}

/* ------------------------------------------------------------------ */
/* CTL-1 operations                                                    */
/* ------------------------------------------------------------------ */

static bool jw__require_available(const jw_svc_supervised *e,
                                  char *reason, size_t reason_size) {
    if (!e) {
        jw__set_reason(reason, reason_size, "unknown-service");
        return false;
    }
    if (!e->manifest_valid || e->on_secondary_root || !e->pak_present) {
        jw__set_reason(reason, reason_size, "unavailable");
        return false;
    }
    return true;
}

bool jw_svc_supervisor_enable(jw_svc_supervisor *sup, const char *service_id,
                              char *reason, size_t reason_size) {
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_available(e, reason, reason_size)) {
        return false;
    }
    bool old_desired = e->desired_enabled;
    bool old_control_desired = e->control.start_with_leaf;
    e->desired_enabled = true;
    e->control.start_with_leaf = true;
    if (!jw__persist(sup, e, "enable")) {
        e->desired_enabled = old_desired;
        e->control.start_with_leaf = old_control_desired;
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    return true;
}

bool jw_svc_supervisor_disable(jw_svc_supervisor *sup, const char *service_id,
                               char *reason, size_t reason_size) {
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!e) {
        jw__set_reason(reason, reason_size, "unknown-service");
        return false;
    }
    bool old_desired = e->desired_enabled;
    bool old_control_desired = e->control.start_with_leaf;
    bool old_session_run = e->session_run;
    bool old_control_session_run = e->control.session_run;
    bool old_autostart_pending = e->autostart_pending;
    e->desired_enabled = false;
    e->control.start_with_leaf = false;
    e->session_run = false;
    e->control.session_run = false;
    e->autostart_pending = false;
    /* Persist the disabled intent before attempting a potentially unverified
     * stop. Otherwise a stuck service would come back as desired after the
     * next daemon restart and could be relaunched when its stale lease clears. */
    if (!jw__persist(sup, e, "disable")) {
        e->desired_enabled = old_desired;
        e->control.start_with_leaf = old_control_desired;
        e->session_run = old_session_run;
        e->control.session_run = old_control_session_run;
        e->autostart_pending = old_autostart_pending;
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    if (e->pgid > 0) {
        e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
        char stop_reason[JW_SVC_REASON_BUF];
        if (!jw__stop_and_reap(sup, e, stop_reason, sizeof(stop_reason))) {
            jw__set_reason(reason, reason_size, "stop-failed");
            return false;
        }
    }
    if (e->state != JW_SVC_STATE_STOPPING) {
        e->state = jw__entry_idle_state(e);
    }
    return true;
}

bool jw_svc_supervisor_run(jw_svc_supervisor *sup, const char *service_id,
                           char *reason, size_t reason_size) {
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_available(e, reason, reason_size)) {
        return false;
    }
    if (e->pgid > 0 || e->state == JW_SVC_STATE_STARTING) {
        jw__set_reason(reason, reason_size, "already-running");
        return false;
    }
    /* A user-initiated Run clears the breaker (SVC-1 restart policy) and
     * triggers an immediate lease attempt even past the 1 s retry. */
    jw_svc_backoff_reset(&e->backoff);
    char start_reason[JW_SVC_REASON_BUF];
    if (!jw__start_generation(sup, e, start_reason, sizeof(start_reason))) {
        if (strcmp(start_reason, "stale-generation") == 0) {
            e->session_run = true;
            e->control.session_run = true;
            e->state = JW_SVC_STATE_STALE_GENERATION;
            e->lease_retry_next_ms = jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
            if (!jw__persist(sup, e, "run-stale-generation")) {
                e->session_run = false;
                e->control.session_run = false;
                snprintf(start_reason, sizeof(start_reason), "%s",
                         "store-failed");
            }
        }
        jw__set_reason(reason, reason_size, start_reason);
        return false;
    }
    e->session_run = true;
    e->control.session_run = true;
    if (!jw__persist(sup, e, "run")) {
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    return true;
}

bool jw_svc_supervisor_stop(jw_svc_supervisor *sup, const char *service_id,
                            char *reason, size_t reason_size) {
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!e) {
        jw__set_reason(reason, reason_size, "unknown-service");
        return false;
    }
    if (e->pgid <= 0) {
        jw__set_reason(reason, reason_size, "not-running");
        return false;
    }
    e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
    e->autostart_pending = false;
    if (!jw__stop_and_reap(sup, e, reason, reason_size)) {
        return false;
    }
    e->session_run = false;
    e->control.session_run = false;
    if (!jw__persist(sup, e, "stop")) {
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    if (e->state != JW_SVC_STATE_STOPPING) {
        e->state = jw__entry_idle_state(e);
    }
    return true;
}

bool jw_svc_supervisor_restart(jw_svc_supervisor *sup, const char *service_id,
                               char *reason, size_t reason_size) {
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_available(e, reason, reason_size)) {
        return false;
    }
    jw_svc_backoff_reset(&e->backoff);
    if (e->pgid > 0) {
        e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
        if (!jw__stop_and_reap(sup, e, reason, reason_size)) {
            return false;
        }
    }
    char start_reason[JW_SVC_REASON_BUF];
    if (!jw__start_generation(sup, e, start_reason, sizeof(start_reason))) {
        if (strcmp(start_reason, "stale-generation") == 0) {
            e->session_run = true;
            e->control.session_run = true;
            e->state = JW_SVC_STATE_STALE_GENERATION;
            e->lease_retry_next_ms = jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
            if (!jw__persist(sup, e, "restart-stale-generation")) {
                e->session_run = false;
                e->control.session_run = false;
                snprintf(start_reason, sizeof(start_reason), "%s",
                         "store-failed");
            }
        }
        jw__set_reason(reason, reason_size, start_reason);
        return false;
    }
    e->session_run = true;
    e->control.session_run = true;
    if (!jw__persist(sup, e, "restart")) {
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* tick                                                                */
/* ------------------------------------------------------------------ */

/* Handles a leader that has exited: records the exit, decides whether the
 * failure is restartable (on-failure policy + backoff), and either starts
 * the stop sequence against the still-reserved group (live descendants) or
 * finalizes an already-absent group. */
static void jw__handle_leader_exit(jw_svc_supervisor *sup,
                                   jw_svc_supervised *e, int exit_code,
                                   bool failed) {
    e->control.has_last_exit = true;
    e->control.last_exit_code = exit_code;
    e->control.last_exit_at_us = jw__wall_us();

    /* Session Run is consumed by the exit: it is one-shot session control,
     * not a keep-alive. Only persistent "Start with Leaf"
     * (desired_enabled) or the on-failure restart policy may bring the
     * service back without a fresh user action. */
    e->session_run = false;
    e->control.session_run = false;

    /* The leader is now a zombie holding the pgid reservation. If live
     * descendants remain the group is NOT absent and must be stopped and
     * verified before any restart -- never interpret leader exit as service
     * absence. */
    bool absent = jw_svc_group_absent(e->pgid);

    bool intentional = (e->pending_stop_reason == JW_SVC_STOP_INTENTIONAL) ||
                       (e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_GAME);
    if (!intentional && failed && e->active_restart_on_failure &&
        jw__entry_available(e)) {
        jw_svc_backoff_decision d =
            jw_svc_backoff_record_failure(&e->backoff, jw__wall_ms());
        if (d.breaker_open) {
            e->state = JW_SVC_STATE_FAILED;
            jw__persist(sup, e, "circuit-breaker");
        } else {
            e->state = JW_SVC_STATE_BACKOFF;
            e->backoff_retry_at_ms = jw__mono_ms() + d.delay_ms;
            jw__persist(sup, e, "failure");
        }
    } else {
        e->state = JW_SVC_STATE_STOPPING;
        jw__persist(sup, e, "exited");
    }

    if (absent) {
        /* Whole group is gone-or-zombie already: reap the leader and
         * release. The failure/backoff state decided above still stands. */
        if (!jw__reap_verified_leader(e->pgid, NULL)) {
            /* Keep the lease and ownership identity rather than freeing a
             * pgid whose leader was not successfully collected. */
            e->state = JW_SVC_STATE_STOPPING;
            e->reap_pending = true;
            return;
        }
        jw__entry_release_runtime(e);
        if (e->state != JW_SVC_STATE_BACKOFF &&
            e->state != JW_SVC_STATE_FAILED) {
            e->state = jw__entry_idle_state(e);
        }
    } else {
        /* Live descendants remain: begin the asynchronous stop sequence.
         * SIGTERM now, SIGKILL after the grace window, verified absence
         * before any restart. */
        kill(-e->pgid, SIGTERM);
        e->stopping_since_ms = jw__mono_ms();
        e->stop_kill_sent = false;
        e->reap_pending = true;
        if (!intentional) {
            e->pending_stop_reason = JW_SVC_STOP_LEADER_EXITED;
        }
        if (e->state != JW_SVC_STATE_BACKOFF &&
            e->state != JW_SVC_STATE_FAILED) {
            e->state = JW_SVC_STATE_STOPPING;
        }
    }
}

/* Observe child termination without consuming the zombie reservation.
 * waitpid(WNOHANG) is not a poll: once a child has exited it reaps it. WNOWAIT
 * is therefore mandatory until jw_svc_group_absent() has proved the group
 * writer-free. */
static bool jw__observe_leader_exit(pid_t leader, int *out_exit_code,
                                    bool *out_failed) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int rc;
    do {
        rc = waitid(P_PID, (id_t)leader, &info,
                    WEXITED | WNOHANG | WNOWAIT);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0 || info.si_pid != leader) {
        return false;
    }
    if (info.si_code == CLD_EXITED) {
        *out_exit_code = info.si_status;
        *out_failed = info.si_status != 0;
    } else {
        *out_exit_code = 128 + info.si_status;
        *out_failed = true;
    }
    return true;
}

int jw_svc_supervisor_tick(jw_svc_supervisor *sup) {
    if (!sup) {
        return -1;
    }
    int changes = 0;
    long long now = jw__mono_ms();

    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];

        /* 1. Poll the leader without reaping it early. */
        if (e->pgid > 0 && !e->reap_pending) {
            int exit_code = -1;
            bool failed = true;
            if (jw__observe_leader_exit(e->pgid, &exit_code, &failed)) {
                jw__handle_leader_exit(sup, e, exit_code, failed);
                changes++;
                continue;
            }
            /* Settle "starting" into "running" after the leader has
             * survived its first window. */
            if (e->state == JW_SVC_STATE_STARTING &&
                now - e->stopping_since_ms >= JW_SVC_STARTING_SETTLE_MS) {
                e->state = JW_SVC_STATE_RUNNING;
                changes++;
            }
        }

        /* 2. Asynchronous stop sequence for a group whose leader already
         *    exited: escalate to SIGKILL once the grace window elapses,
         *    then verify absence. */
        if (e->pgid > 0 && e->reap_pending) {
            long long grace = e->active_stop_grace_ms;
            if (!e->stop_kill_sent &&
                now - e->stopping_since_ms >= grace) {
                kill(-e->pgid, SIGKILL);
                e->stop_kill_sent = true;
                e->stopping_since_ms = now;
            }
            /* Reap as soon as the group is verified absent -- whether that
             * is inside the TERM grace window or after the KILL window. */
            bool wait_window_done =
                e->stop_kill_sent &&
                now - e->stopping_since_ms >= JW_SVC_STOP_KILL_WAIT_MS;
            if (jw_svc_group_absent(e->pgid)) {
                if (!jw__reap_verified_leader(e->pgid, NULL)) {
                    continue;
                }
                jw__entry_release_runtime(e);
                e->reap_pending = false;
                if (e->state != JW_SVC_STATE_BACKOFF &&
                    e->state != JW_SVC_STATE_FAILED) {
                    e->state = jw__entry_idle_state(e);
                }
                changes++;
            } else if (wait_window_done) {
                /* TERM+KILL+2s and the group is STILL present: a known
                 * live writer. Leave it in "stopping", keep the lease, and
                 * do not reap -- SVC-1's unverified-stop table governs the
                 * surrounding operation. Keep polling each tick so a later
                 * disappearance is still observed. */
            }
        }

        /* 3. Backoff: retry a failed on-failure service once its delay
         *    elapses, only if the previous generation's group is gone. */
        bool started_this_tick = false;
        if (e->state == JW_SVC_STATE_BACKOFF && e->pgid <= 0 &&
            now >= e->backoff_retry_at_ms) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                e->control.restart_count++;
                jw__persist(sup, e, "backoff-retry");
                started_this_tick = true;
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->state = JW_SVC_STATE_STALE_GENERATION;
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
                changes++;
            } else {
                /* A failed retry is another failure for backoff purposes. */
                jw_svc_backoff_decision d =
                    jw_svc_backoff_record_failure(&e->backoff,
                                                  jw__wall_ms());
                if (d.breaker_open) {
                    e->state = JW_SVC_STATE_FAILED;
                } else {
                    e->backoff_retry_at_ms = now + d.delay_ms;
                }
                jw__persist(sup, e, "failure");
                changes++;
            }
        }

        /* 4. Stale generation: an old daemon generation still holds the
         *    lease. Retry the non-blocking acquisition once per second
         *    while desired enablement remains true. */
        if (!started_this_tick &&
            e->state == JW_SVC_STATE_STALE_GENERATION && e->pgid <= 0 &&
            (e->desired_enabled || e->session_run) &&
            now >= e->lease_retry_next_ms) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                jw__persist(sup, e, "lease-acquired");
                started_this_tick = true;
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
            } else {
                e->session_run = false;
                e->control.session_run = false;
                e->autostart_pending = false;
                if (e->pgid <= 0) {
                    e->state = jw__entry_idle_state(e);
                }
                jw__persist(sup, e, "stale-retry-failed");
                changes++;
            }
        }

        /* A report-only stale generation with no current desired intent still
         * needs passive liveness refresh. Acquire-and-close proves the old
         * holder is gone without launching a replacement. */
        if (!started_this_tick &&
            e->state == JW_SVC_STATE_STALE_GENERATION && e->pgid <= 0 &&
            !e->desired_enabled && !e->session_run &&
            now >= e->lease_retry_next_ms) {
            char lease_reason[JW_SVC_REASON_BUF];
            int lease_fd = jw_svc_lease_acquire(
                sup->runtime_dir, e->service_id, lease_reason,
                sizeof(lease_reason));
            if (lease_fd >= 0) {
                close(lease_fd);
                e->state = jw__entry_idle_state(e);
                jw__persist(sup, e, "stale-generation-cleared");
                changes++;
            } else {
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
            }
        }

        /* 5. Autostart: desired enablement with no running, no backoff,
         *    and no stale lease starts exactly one new generation. */
        bool idle = e->pgid <= 0 &&
                    e->state != JW_SVC_STATE_BACKOFF &&
                    e->state != JW_SVC_STATE_FAILED &&
                    e->state != JW_SVC_STATE_STOPPING &&
                    e->state != JW_SVC_STATE_STALE_GENERATION &&
                    e->state != JW_SVC_STATE_STARTING;
        if (idle && !started_this_tick && e->autostart_pending &&
            e->manifest_valid && e->pak_present && !e->on_secondary_root) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                jw__persist(sup, e, "autostart");
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->state = JW_SVC_STATE_STALE_GENERATION;
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
                changes++;
            } else if (e->pgid <= 0) {
                /* Consume this startup attempt. Retrying a fork/exec or
                 * reservation failure at the daemon's ~20 Hz tick rate is a
                 * process-spawn loop, not an autostart policy. A user Run can
                 * explicitly try again. */
                e->autostart_pending = false;
                e->state = jw__entry_idle_state(e);
                jw__persist(sup, e, "autostart-failed");
                changes++;
            }
        }
    }
    return changes;
}

/* ------------------------------------------------------------------ */
/* shutdown / lifecycle                                                */
/* ------------------------------------------------------------------ */

int jw_svc_supervisor_stop_all(jw_svc_supervisor *sup) {
    if (!sup) {
        return 0;
    }
    int unverified = 0;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (e->state == JW_SVC_STATE_STALE_GENERATION) {
            /* The old generation is deliberately not signallable, but its
             * locked lease is positive evidence that shutdown cannot verify
             * it stopped. Shutdown continues and reports the warning. */
            e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
            jw__persist(sup, e, "shutdown-unverified-stale");
            unverified++;
            continue;
        }
        if (e->pgid <= 0) {
            continue;
        }
        e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
        char reason[JW_SVC_REASON_BUF];
        if (!jw__stop_and_reap(sup, e, reason, sizeof(reason))) {
            /* SVC-1: shutdown continues with a warning; the stuck service
             * is recorded, not allowed to wedge the platform transition. */
            unverified++;
            continue;
        }
        e->session_run = false;
        e->control.session_run = false;
        jw__persist(sup, e, "shutdown");
    }
    return unverified;
}

int jw_svc_supervisor_game_launch_begin(jw_svc_supervisor *sup,
                                        char *out_stuck_id,
                                        size_t stuck_id_size) {
    if (out_stuck_id && stuck_id_size > 0) {
        out_stuck_id[0] = '\0';
    }
    if (!sup) {
        return 0;
    }
    int stopped = 0;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (e->state == JW_SVC_STATE_STALE_GENERATION &&
            e->active_lifecycle_game == JW_SVC_LIFECYCLE_GAME_STOP) {
            if (out_stuck_id && stuck_id_size > 0 &&
                out_stuck_id[0] == '\0') {
                snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
            }
            continue;
        }
        if (e->pgid <= 0) {
            continue;
        }
        if (e->active_lifecycle_game != JW_SVC_LIFECYCLE_GAME_STOP) {
            continue; /* notify/ignore are LIFE-1/A3b, not this phase */
        }
        e->pending_stop_reason = JW_SVC_STOP_LIFECYCLE_GAME;
        e->autostart_pending = false;
        char reason[JW_SVC_REASON_BUF];
        if (!jw__stop_and_reap(sup, e, reason, sizeof(reason))) {
            if (out_stuck_id && stuck_id_size > 0 &&
                out_stuck_id[0] == '\0') {
                snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
            }
            continue;
        }
        jw__persist(sup, e, "game-launch");
        stopped++;
    }
    return stopped;
}
