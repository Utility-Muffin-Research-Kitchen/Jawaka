#include "internal/store/pakrat_recovery.h"

#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define JW_PAKRAT_STAGE_PREFIX ".pakrat-stage-"
#define JW_PAKRAT_ROLLBACK_PREFIX ".pakrat-rollback-"
#define JW_PAKRAT_ORIGIN_PREFIX ".pakrat-origin-"
#define JW_PAKRAT_ENTRY_POINT "launch.sh"
#define JW_PAKRAT_MANIFEST_MAX_BYTES (1024L * 1024L)

int jw__pakrat_copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s", value);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

int jw__pakrat_join2(char *out, size_t out_size, const char *a, const char *b) {
    if (!out || out_size == 0 || !a || !b) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/%s", a, b);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

int jw__pakrat_join3(char *out, size_t out_size, const char *a, const char *b,
                     const char *c) {
    if (!out || out_size == 0 || !a || !b || !c) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/%s/%s", a, b, c);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

bool jw__pakrat_path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

bool jw__pakrat_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool jw__pakrat_is_regular_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int jw__pakrat_mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        return -1;
    }
    char tmp[PATH_MAX];
    if (jw__pakrat_copy(tmp, sizeof(tmp), path) != 0) {
        return -1;
    }
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 &&
                (errno != EEXIST || !jw__pakrat_is_dir(tmp))) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 &&
        (errno != EEXIST || !jw__pakrat_is_dir(tmp))) {
        return -1;
    }
    return 0;
}

int jw__pakrat_mkdir_parent(const char *path) {
    char parent[PATH_MAX];
    if (jw__pakrat_copy(parent, sizeof(parent), path) != 0) {
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return 0;
    }
    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return jw__pakrat_mkdir_p(parent, 0755);
}

int jw__pakrat_remove_tree(const char *path) {
    struct stat st;
    if (!path || !path[0]) {
        return -1;
    }
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (jw__pakrat_join2(child, sizeof(child), path, entry->d_name) != 0 ||
            jw__pakrat_remove_tree(child) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return rmdir(path) == 0 || errno == ENOENT ? 0 : -1;
}

bool jw__pakrat_safe_name(const char *name) {
    return name && name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
           !strchr(name, '/') && !strchr(name, '\\');
}

bool jw__pakrat_safe_rel_path(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\')) {
        return false;
    }
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return true;
}

void jw__pakrat_log(const char *state_dir, const char *fmt, ...) {
    if (!state_dir || !state_dir[0] || !fmt) {
        return;
    }
    char log_dir[PATH_MAX];
    char log_path[PATH_MAX];
    if (jw__pakrat_join3(log_dir, sizeof(log_dir), state_dir, "store", "logs") != 0 ||
        jw__pakrat_join2(log_path, sizeof(log_path), log_dir, "pakrat.log") != 0 ||
        jw__pakrat_mkdir_p(log_dir, 0755) != 0) {
        return;
    }
    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        return;
    }

    char timestamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &tm_now);
    fprintf(fp, "%s ", timestamp);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fputc('\n', fp);
    fclose(fp);
}

int jw__pakrat_target_path(const char *sdcard_root, const char *install_path,
                           char *out, size_t out_size) {
    if (!sdcard_root || !sdcard_root[0] || !install_path || !out ||
        out_size == 0) {
        return -1;
    }
    const char *path = install_path;
    if (strncmp(path, "Apps/", 5) == 0) {
        path += 5;
    }
    if (!jw__pakrat_safe_rel_path(path)) {
        return -1;
    }
    return jw__pakrat_join3(out, out_size, sdcard_root, "Apps", path);
}

int jw__pakrat_target_sibling_path(const char *target, const char *store_id,
                                   const char *kind, char *out,
                                   size_t out_size) {
    if (!target || !target[0] || !store_id || !store_id[0] ||
        !kind || !kind[0] || !out || out_size == 0 ||
        !jw__pakrat_safe_name(store_id) || !jw__pakrat_safe_name(kind)) {
        return -1;
    }
    char parent[PATH_MAX];
    if (jw__pakrat_copy(parent, sizeof(parent), target) != 0) {
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return -1;
    }
    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    int n = snprintf(out, out_size, "%s/.pakrat-%s-%s", parent, kind, store_id);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

/* ── Rollback origin marker ───────────────────────────────────────────────
   A rollback sibling is named for its store id, not its install name, so the
   target path cannot be reconstructed from the sibling alone. Recovery reaches
   a recorded install through jw_db_pakrat_list_installs, but an adopted install
   that crashes before its first record is written has no row — and deleting its
   rollback would destroy the only copy of the app that was moved aside. The
   marker records install_path before move-aside so that case can be restored
   instead of swept without leaving an unmarked crash window. */

int jw__pakrat_write_origin_marker(const char *target, const char *store_id,
                                   const char *install_path) {
    char marker[PATH_MAX];
    if (!jw__pakrat_safe_rel_path(install_path) ||
        jw__pakrat_target_sibling_path(target, store_id, "origin",
                                       marker, sizeof(marker)) != 0) {
        return -1;
    }
    FILE *fp = fopen(marker, "wb");
    if (!fp) {
        return -1;
    }
    int ok = fprintf(fp, "%s\n", install_path) > 0;
    if (fflush(fp) != 0) {
        ok = 0;
    }
    if (fclose(fp) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

int jw__pakrat_read_origin_marker(const char *apps_dir, const char *store_id,
                                  char *out, size_t out_size) {
    char marker[PATH_MAX];
    if (!apps_dir || !out || out_size == 0 || !jw__pakrat_safe_name(store_id)) {
        return -1;
    }
    int marker_n = snprintf(marker, sizeof(marker), "%s/%s%s", apps_dir,
                            JW_PAKRAT_ORIGIN_PREFIX, store_id);
    if (marker_n < 0 || (size_t)marker_n >= sizeof(marker)) {
        return -1;
    }
    FILE *fp = fopen(marker, "rb");
    if (!fp) {
        return -1;
    }
    char line[PATH_MAX];
    char *got = fgets(line, (int)sizeof(line), fp);
    int complete = got && (strchr(line, '\n') != NULL || feof(fp));
    int read_failed = ferror(fp);
    int close_failed = fclose(fp) != 0;
    if (!got || !complete || read_failed || close_failed) {
        return -1;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (!len || !jw__pakrat_safe_rel_path(line)) {
        return -1;
    }
    return jw__pakrat_copy(out, out_size, line);
}

void jw__pakrat_clear_origin_marker(const char *target, const char *store_id) {
    char marker[PATH_MAX];
    if (jw__pakrat_target_sibling_path(target, store_id, "origin",
                                       marker, sizeof(marker)) == 0) {
        (void)unlink(marker);
    }
}

static int jw__pakrat_manifest_string(const cJSON *obj, const char *key,
                                      char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        int n = snprintf(out, out_size, "%s", item->valuestring);
        if (n < 0 || (size_t)n >= out_size) {
            return -1;
        }
    }
    return 0;
}

int jw__pakrat_read_manifest(const char *pak_dir, const char *manifest_rel,
                             jw__pakrat_manifest *out) {
    if (!pak_dir || !out || !jw__pakrat_safe_rel_path(manifest_rel)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char path[PATH_MAX];
    if (jw__pakrat_join2(path, sizeof(path), pak_dir, manifest_rel) != 0) {
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long size = ftell(fp);
    if (size <= 0 || size > JW_PAKRAT_MANIFEST_MAX_BYTES ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    char *json = (char *)malloc((size_t)size + 1u);
    if (!json) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(json, 1, (size_t)size, fp);
    int read_failed = ferror(fp);
    int close_failed = fclose(fp) != 0;
    if (got != (size_t)size || read_failed || close_failed) {
        free(json);
        return -1;
    }
    json[got] = '\0';

    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    free(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return -1;
    }
    int fields_ok =
        jw__pakrat_manifest_string(root, "platform", out->platform,
                                   sizeof(out->platform)) == 0 &&
        jw__pakrat_manifest_string(root, "pak_version", out->pak_version,
                                   sizeof(out->pak_version)) == 0 &&
        jw__pakrat_manifest_string(root, "min_leaf_version",
                                   out->min_leaf_version,
                                   sizeof(out->min_leaf_version)) == 0;
    cJSON_Delete(root);
    return fields_ok ? 0 : -1;
}

/* The promoted tree at |target| counts as committed only when its identity is
   verifiable and matches the install record: pak.json parses, pak_version
   equals the recorded version, and the declared launch.sh entry point exists.
   Returns a static reason string for the rollback log when it does not. */
static const char *jw__uncommitted_promote_reason(const char *target,
                                                  const jw_pakrat_install *install) {
    jw__pakrat_manifest manifest;
    if (jw__pakrat_read_manifest(target, "pak.json", &manifest) != 0 ||
        !manifest.pak_version[0]) {
        return "manifest-unreadable";
    }
    const char *record_version = install ? install->version : "";
    if (!record_version[0] || strcmp(manifest.pak_version, record_version) != 0) {
        return install ? "version-mismatch" : "no-record";
    }
    if (!manifest.platform[0] || !install->platform[0] ||
        strcmp(manifest.platform, install->platform) != 0) {
        return "platform-mismatch";
    }
    char entry_point[PATH_MAX];
    if (jw__pakrat_join2(entry_point, sizeof(entry_point), target,
                         JW_PAKRAT_ENTRY_POINT) != 0 ||
        !jw__pakrat_is_regular_file(entry_point)) {
        return "entry-point-missing";
    }
    return NULL;
}

/* After a rollback sibling is restored to |target|, make sure the install
   record describes the tree that is actually on disk. In the designed crash
   windows the row was never updated, so it already matches; this only rewrites
   when the restored tree is identifiable and disagrees. */
static int jw__reconcile_record_to_tree(const jw_pakrat_recovery_context *ctx,
                                        const jw_pakrat_install *install,
                                        const char *target) {
    if (!install) {
        return 0;
    }
    jw__pakrat_manifest manifest;
    if (jw__pakrat_read_manifest(target, "pak.json", &manifest) != 0 ||
        !manifest.pak_version[0] ||
        strcmp(manifest.pak_version, install->version) == 0) {
        return 0;
    }
    if (!manifest.platform[0] ||
        strcmp(manifest.platform, install->platform) != 0) {
        return -1;
    }
    if (jw_db_pakrat_upsert_install(ctx->db_path, install->store_id,
                                    manifest.pak_version, install->platform,
                                    install->install_path,
                                    install->artifact_sha256,
                                    install->installed_at) != 0) {
        fprintf(stderr, "could not reconcile Pak Rat install record: %s\n",
                install->store_id);
        return -1;
    }
    jw__pakrat_log(ctx->state_dir,
                   "install-recover reconciled record store_id=%s version=%s->%s",
                   install->store_id, install->version, manifest.pak_version);
    return 0;
}

int jw__pakrat_reconcile_transition(const jw_pakrat_recovery_context *ctx,
                                    const char *store_id,
                                    const char *install_path,
                                    const jw_pakrat_install *install) {
    if (!ctx) {
        return -1;
    }
    char target[PATH_MAX];
    char target_stage[PATH_MAX];
    char target_rollback[PATH_MAX];
    char legacy_partial[PATH_MAX];
    if (jw__pakrat_target_path(ctx->sdcard_root, install_path,
                               target, sizeof(target)) != 0 ||
        jw__pakrat_target_sibling_path(target, store_id, "stage",
                                       target_stage, sizeof(target_stage)) != 0 ||
        jw__pakrat_target_sibling_path(target, store_id, "rollback",
                                       target_rollback, sizeof(target_rollback)) != 0) {
        return -1;
    }
    int legacy_n = snprintf(legacy_partial, sizeof(legacy_partial), "%s.partial",
                            target);
    if (legacy_n < 0 || (size_t)legacy_n >= sizeof(legacy_partial)) {
        return -1;
    }

    if (jw__pakrat_path_exists(target_stage) &&
        jw__pakrat_remove_tree(target_stage) != 0) {
        fprintf(stderr, "could not remove stale Pak Rat stage path: %s\n", target_stage);
        return -1;
    }
    if (jw__pakrat_path_exists(legacy_partial) &&
        jw__pakrat_remove_tree(legacy_partial) != 0) {
        fprintf(stderr, "could not remove stale Pak Rat legacy stage path: %s\n",
                legacy_partial);
        return -1;
    }

    int target_exists = jw__pakrat_path_exists(target);
    int rollback_exists = jw__pakrat_path_exists(target_rollback);
    if (!target_exists && rollback_exists) {
        if (rename(target_rollback, target) != 0) {
            fprintf(stderr, "could not restore Pak Rat rollback path: %s\n", target);
            return -1;
        }
        jw__pakrat_clear_origin_marker(target, store_id);
        jw__pakrat_log(ctx->state_dir,
                       "install-recover restored store_id=%s target=Apps/%s",
                       store_id, install_path);
        if (jw__reconcile_record_to_tree(ctx, install, target) != 0) {
            return -1;
        }
    } else if (target_exists && rollback_exists) {
        const char *rollback_reason =
            jw__uncommitted_promote_reason(target, install);
        if (!rollback_reason) {
            if (jw__pakrat_remove_tree(target_rollback) != 0) {
                fprintf(stderr, "could not remove stale Pak Rat rollback path: %s\n",
                        target_rollback);
                return -1;
            }
            jw__pakrat_clear_origin_marker(target, store_id);
            jw__pakrat_log(ctx->state_dir,
                           "install-recover cleaned rollback store_id=%s target=Apps/%s",
                           store_id, install_path);
        } else {
            /* The install-record update is the commit point: this promote
               never committed, so prefer the tree that was already running. */
            if (jw__pakrat_remove_tree(target) != 0 ||
                rename(target_rollback, target) != 0) {
                fprintf(stderr,
                        "could not roll back uncommitted Pak Rat promote: %s\n",
                        target);
                return -1;
            }
            jw__pakrat_clear_origin_marker(target, store_id);
            jw__pakrat_log(ctx->state_dir,
                           "install-recover rolled back uncommitted promote store_id=%s target=Apps/%s reason=%s",
                           store_id, install_path, rollback_reason);
            if (jw__reconcile_record_to_tree(ctx, install, target) != 0) {
                return -1;
            }
        }
    } else if (!rollback_exists) {
        /* A write-ahead origin marker can survive a crash just before the
           move-aside rename. With no rollback tree it has no recovery role. */
        jw__pakrat_clear_origin_marker(target, store_id);
    }
    return 0;
}

/* Sweep one Apps dir for transition siblings whose store id has no install
   row. Recovery iterates install rows, so an interrupted first install (no
   row yet) is only reachable here. Conservative: exact prefixes only. */
static int jw__sweep_orphan_transition_dirs(const jw_pakrat_recovery_context *ctx,
                                            const jw_pakrat_install *installs,
                                            int install_count,
                                            const char *platform_dir) {
    char apps_dir[PATH_MAX];
    if (!jw__pakrat_safe_name(platform_dir) ||
        jw__pakrat_join3(apps_dir, sizeof(apps_dir), ctx->sdcard_root, "Apps",
                         platform_dir) != 0) {
        return -1;
    }
    DIR *dir = opendir(apps_dir);
    if (!dir) {
        return errno == ENOENT ? 0 : -1;
    }
    int rc = 0;
    struct dirent *entry = NULL;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                rc = -1;
            }
            break;
        }
        const char *store_id = NULL;
        bool is_rollback = false;
        bool is_origin = false;
        if (strncmp(entry->d_name, JW_PAKRAT_STAGE_PREFIX,
                    sizeof(JW_PAKRAT_STAGE_PREFIX) - 1) == 0) {
            store_id = entry->d_name + sizeof(JW_PAKRAT_STAGE_PREFIX) - 1;
        } else if (strncmp(entry->d_name, JW_PAKRAT_ROLLBACK_PREFIX,
                           sizeof(JW_PAKRAT_ROLLBACK_PREFIX) - 1) == 0) {
            store_id = entry->d_name + sizeof(JW_PAKRAT_ROLLBACK_PREFIX) - 1;
            is_rollback = true;
        } else if (strncmp(entry->d_name, JW_PAKRAT_ORIGIN_PREFIX,
                           sizeof(JW_PAKRAT_ORIGIN_PREFIX) - 1) == 0) {
            store_id = entry->d_name + sizeof(JW_PAKRAT_ORIGIN_PREFIX) - 1;
            is_origin = true;
        }
        if (!store_id || !jw__pakrat_safe_name(store_id)) {
            continue;
        }
        char orphan[PATH_MAX];
        if (jw__pakrat_join2(orphan, sizeof(orphan), apps_dir, entry->d_name) != 0) {
            rc = -1;
            break;
        }

        bool owned = false;
        for (int i = 0; i < install_count; i++) {
            if (strcmp(installs[i].store_id, store_id) != 0) {
                continue;
            }
            const char *kind =
                is_rollback ? "rollback" : (is_origin ? "origin" : "stage");
            char expected_target[PATH_MAX];
            char expected_sibling[PATH_MAX];
            if (jw__pakrat_target_path(ctx->sdcard_root,
                                       installs[i].install_path,
                                       expected_target,
                                       sizeof(expected_target)) != 0 ||
                jw__pakrat_target_sibling_path(expected_target, store_id, kind,
                                               expected_sibling,
                                               sizeof(expected_sibling)) != 0) {
                /* A malformed ownership row must never make recovery delete a
                   possibly-related tree. The row-level reconcile reports it. */
                owned = true;
                break;
            }
            if (strcmp(expected_sibling, orphan) == 0) {
                owned = true;
                break;
            }
        }
        if (owned) {
            continue;
        }

        if (is_origin) {
            char rollback[PATH_MAX];
            int rollback_n =
                snprintf(rollback, sizeof(rollback), "%s/%s%s", apps_dir,
                         JW_PAKRAT_ROLLBACK_PREFIX, store_id);
            if (rollback_n < 0 || (size_t)rollback_n >= sizeof(rollback)) {
                rc = -1;
                break;
            }
            if (jw__pakrat_path_exists(rollback)) {
                continue;
            }
            if (unlink(orphan) != 0 && errno != ENOENT) {
                fprintf(stderr,
                        "could not remove orphaned Pak Rat origin marker: %s\n",
                        orphan);
                rc = -1;
                break;
            }
            jw__pakrat_log(ctx->state_dir,
                           "install-recover swept orphan origin store_id=%s path=Apps/%s/%s",
                           store_id, platform_dir, entry->d_name);
            continue;
        }

        /* A rollback tree may be the only surviving copy of an app: an adopted
           install that crashed between the move-aside and the promote has no
           install row, so it reaches this sweep rather than reconcile.
           Restore it when its target is missing; never delete it blind. */
        if (is_rollback) {
            char install_path[PATH_MAX];
            char target[PATH_MAX];
            if (jw__pakrat_read_origin_marker(apps_dir, store_id, install_path,
                                              sizeof(install_path)) == 0 &&
                jw__pakrat_target_path(ctx->sdcard_root, install_path,
                                       target, sizeof(target)) == 0) {
                if (!jw__pakrat_path_exists(target)) {
                    if (rename(orphan, target) != 0) {
                        fprintf(stderr,
                                "could not restore orphaned Pak Rat rollback: %s\n",
                                target);
                        rc = -1;
                        break;
                    }
                    jw__pakrat_clear_origin_marker(target, store_id);
                    jw__pakrat_log(ctx->state_dir,
                                   "install-recover restored unrecorded rollback store_id=%s target=Apps/%s",
                                   store_id, install_path);
                    continue;
                }
                jw__pakrat_clear_origin_marker(target, store_id);
            } else {
                /* No marker: the target name is not reconstructable from the
                   sibling, so deleting it could destroy an app. Leave it and
                   surface it instead. */
                jw__pakrat_log(ctx->state_dir,
                               "install-recover retained unidentifiable rollback store_id=%s path=Apps/%s/%s",
                               store_id, platform_dir, entry->d_name);
                continue;
            }
        }

        if (jw__pakrat_remove_tree(orphan) != 0) {
            fprintf(stderr, "could not remove orphaned Pak Rat path: %s/%s\n",
                    apps_dir, entry->d_name);
            rc = -1;
            break;
        }
        jw__pakrat_log(ctx->state_dir,
                       "install-recover swept orphan store_id=%s path=Apps/%s/%s",
                       store_id, platform_dir, entry->d_name);
    }
    closedir(dir);
    return rc;
}

int jw_pakrat_recover_installs(const jw_pakrat_recovery_context *ctx) {
    enum { JW_PAKRAT_MAX_RECOVERY_INSTALLS = 1024 };
    int install_count = 0;
    if (!ctx || !ctx->sdcard_root[0] || !ctx->db_path[0] ||
        jw__pakrat_mkdir_parent(ctx->db_path) != 0) {
        return -1;
    }
    jw_pakrat_install *installs =
        (jw_pakrat_install *)calloc((size_t)JW_PAKRAT_MAX_RECOVERY_INSTALLS,
                                    sizeof(*installs));
    if (!installs) {
        return -1;
    }
    int rc = -1;
    if (jw_db_pakrat_list_installs(ctx->db_path, installs,
                                   JW_PAKRAT_MAX_RECOVERY_INSTALLS,
                                   &install_count) != 0) {
        goto done;
    }
    for (int i = 0; i < install_count; i++) {
        if (jw__pakrat_reconcile_transition(ctx, installs[i].store_id,
                                            installs[i].install_path,
                                            &installs[i]) != 0) {
            goto done;
        }
    }
    if (ctx->platform[0] &&
        jw__sweep_orphan_transition_dirs(ctx, installs, install_count,
                                         ctx->platform) != 0) {
        goto done;
    }
    if (ctx->platform[0] && strcmp(ctx->platform, "shared") != 0 &&
        jw__sweep_orphan_transition_dirs(ctx, installs, install_count,
                                         "shared") != 0) {
        goto done;
    }
    rc = 0;
done:
    free(installs);
    return rc;
}
