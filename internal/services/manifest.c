#include "internal/services/manifest.h"

#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JW_SVC_RESOLVE_BUF (PATH_MAX + 1)

static void jw__svc_set_reason(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug);
    }
}

/* Matches ^[a-z0-9]+(\.[a-z0-9]+)+$ -- at least two dot-separated segments,
 * each non-empty and restricted to lowercase ascii + digits. Hand-written
 * rather than <regex.h> so this has no platform-dependent regex engine
 * behavior to verify on the MLP1 cross-toolchain. */
static bool jw__svc_is_reverse_dns(const char *s) {
    if (!s || !s[0]) {
        return false;
    }
    if (strlen(s) >= JW_SVC_ID_BUF) {
        return false;
    }
    int dot_count = 0;
    size_t seg_len = 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (seg_len == 0) {
                return false; /* empty segment: leading/trailing/double dot */
            }
            dot_count++;
            seg_len = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            seg_len++;
        } else {
            return false;
        }
    }
    return seg_len > 0 && dot_count >= 1;
}

/* "absolute" if it starts with '/'; "traversal" if any '/'-delimited
 * segment is exactly "..". Neither implies the other; a caller checks
 * absolute first since a rooted path can't also be meaningfully classified
 * by its segments the same way. */
static bool jw__svc_path_is_absolute(const char *p) {
    return p && p[0] == '/';
}

static bool jw__svc_path_has_traversal(const char *p) {
    if (!p) {
        return false;
    }
    const char *cursor = p;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t seg_len = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (seg_len == 2 && cursor[0] == '.' && cursor[1] == '.') {
            return true;
        }
        if (!slash) {
            break;
        }
        cursor = slash + 1;
    }
    return false;
}

static bool jw__svc_path_under_root(const char *candidate_abs, const char *root_abs) {
    size_t root_len = strlen(root_abs);
    if (strncmp(candidate_abs, root_abs, root_len) != 0) {
        return false;
    }
    char next = candidate_abs[root_len];
    return next == '\0' || next == '/';
}

static void jw__svc_dirname_into(const char *rel_path, char *dir_out, size_t dir_out_size,
                                  char *base_out, size_t base_out_size) {
    const char *slash = strrchr(rel_path, '/');
    if (!slash) {
        dir_out[0] = '\0';
        snprintf(base_out, base_out_size, "%s", rel_path);
        return;
    }
    size_t dir_len = (size_t)(slash - rel_path);
    if (dir_len >= dir_out_size) {
        dir_len = dir_out_size - 1;
    }
    memcpy(dir_out, rel_path, dir_len);
    dir_out[dir_len] = '\0';
    snprintf(base_out, base_out_size, "%s", slash + 1);
}

/* Lexically join `base` (an absolute path, no trailing slash except "/"
 * itself) with a single ".."-or-plain path segment, without touching the
 * filesystem. Used only to resolve a symlink's readlink() target relative
 * to the directory that contains it -- by this point the surrounding
 * directory has already been proven real by realpath(), so this only
 * needs to get the ".." arithmetic right, not detect further symlinks. */
static void jw__svc_lexical_join(char *base, size_t base_size, const char *rel) {
    char work[JW_SVC_RESOLVE_BUF];
    snprintf(work, sizeof(work), "%s", rel);
    char *saveptr = NULL;
    char *tok = strtok_r(work, "/", &saveptr);
    while (tok) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            /* no-op */
        } else if (strcmp(tok, "..") == 0) {
            size_t len = strlen(base);
            while (len > 1 && base[len - 1] != '/') {
                len--;
            }
            if (len > 1) {
                len--;
            }
            base[len] = '\0';
        } else {
            size_t len = strlen(base);
            if (len == 0 || base[len - 1] != '/') {
                if (len + 1 < base_size) {
                    base[len++] = '/';
                    base[len] = '\0';
                }
            }
            size_t tok_len = strlen(tok);
            if (len + tok_len < base_size) {
                memcpy(base + len, tok, tok_len + 1);
            }
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }
}

typedef enum {
    JW_SVC_RUN_PATH_OK,
    JW_SVC_RUN_PATH_MISSING,
    JW_SVC_RUN_PATH_ESCAPES,
    JW_SVC_RUN_PATH_NON_REGULAR,
    JW_SVC_RUN_PATH_NOT_EXECUTABLE,
} jw_svc_run_path_check;

/* Validates that pak_abs_path/rel_run_path resolves (following at most one
 * symlink hop at the leaf -- see manifest.h and the module comment in this
 * file for why a leaf-only hop is the deliberate scope here) to a regular,
 * executable file confined to pak_abs_path.
 *
 * Approach: resolve the CONTAINING DIRECTORY with real realpath(3) (which
 * requires it to exist, and correctly follows every symlink component in
 * it, however deep) and boundary-check that. Then lstat() the leaf
 * component directly: if it is itself a symlink, resolve its target
 * lexically (string-level, no further filesystem calls) against the
 * already-proven-real containing directory and boundary-check the result
 * -- this is what lets an ESCAPING SYMLINK BE DETECTED EVEN WHEN ITS TARGET
 * DOES NOT EXIST, which strict POSIX realpath(3) on the full path cannot
 * do (it fails outright with ENOENT before any boundary check happens).
 */
static jw_svc_run_path_check jw__svc_check_run_path(const char *pak_abs_path,
                                                     const char *rel_run_path) {
    char pak_real[JW_SVC_RESOLVE_BUF];
    if (!realpath(pak_abs_path, pak_real)) {
        return JW_SVC_RUN_PATH_MISSING;
    }

    char rel_dir[JW_SVC_RESOLVE_BUF];
    char base[JW_SVC_RESOLVE_BUF];
    jw__svc_dirname_into(rel_run_path, rel_dir, sizeof(rel_dir), base, sizeof(base));

    char dir_abs[JW_SVC_RESOLVE_BUF];
    if (rel_dir[0]) {
        snprintf(dir_abs, sizeof(dir_abs), "%s/%s", pak_real, rel_dir);
    } else {
        snprintf(dir_abs, sizeof(dir_abs), "%s", pak_real);
    }

    char dir_real[JW_SVC_RESOLVE_BUF];
    if (!realpath(dir_abs, dir_real)) {
        /* Containing directory doesn't exist (or a component along the way
         * is not a directory) -- nothing runnable can live under it. A
         * dangling INTERMEDIATE symlink that also escapes is not
         * distinguished from a plain missing directory here; see the
         * module comment. */
        return JW_SVC_RUN_PATH_MISSING;
    }
    if (!jw__svc_path_under_root(dir_real, pak_real)) {
        return JW_SVC_RUN_PATH_ESCAPES;
    }

    char leaf_abs[JW_SVC_RESOLVE_BUF];
    snprintf(leaf_abs, sizeof(leaf_abs), "%s/%s", dir_real, base);

    struct stat leaf_lst;
    if (lstat(leaf_abs, &leaf_lst) != 0) {
        return JW_SVC_RUN_PATH_MISSING;
    }

    char target_abs[JW_SVC_RESOLVE_BUF];
    if (S_ISLNK(leaf_lst.st_mode)) {
        char link_buf[JW_SVC_RESOLVE_BUF];
        ssize_t n = readlink(leaf_abs, link_buf, sizeof(link_buf) - 1);
        if (n < 0) {
            return JW_SVC_RUN_PATH_MISSING;
        }
        link_buf[n] = '\0';

        if (link_buf[0] == '/') {
            snprintf(target_abs, sizeof(target_abs), "%s", link_buf);
        } else {
            snprintf(target_abs, sizeof(target_abs), "%s", dir_real);
            jw__svc_lexical_join(target_abs, sizeof(target_abs), link_buf);
        }

        if (!jw__svc_path_under_root(target_abs, pak_real)) {
            return JW_SVC_RUN_PATH_ESCAPES;
        }
    } else {
        snprintf(target_abs, sizeof(target_abs), "%s", leaf_abs);
    }

    struct stat target_st;
    if (stat(target_abs, &target_st) != 0) {
        /* Symlink resolved to somewhere inside the pak, but nothing exists
         * there (dangling-but-confined). Still missing, not an escape. */
        return JW_SVC_RUN_PATH_MISSING;
    }
    if (!S_ISREG(target_st.st_mode)) {
        return JW_SVC_RUN_PATH_NON_REGULAR;
    }
    if (access(target_abs, X_OK) != 0) {
        return JW_SVC_RUN_PATH_NOT_EXECUTABLE;
    }
    return JW_SVC_RUN_PATH_OK;
}

/* "No existing component may be a symlink" for a state.revoke_on_uninstall
 * entry, including state.root itself and the entry's own leaf component.
 * A component that does not exist is never a violation -- only an existing
 * symlink is. userdata_root_abs may be NULL/empty, meaning "not known yet"
 * (e.g. validating a manifest before the package has ever been installed),
 * in which case this check is skipped entirely rather than guessed at. */
static bool jw__svc_revoke_path_has_symlink_component(const char *userdata_root_abs,
                                                       const char *state_root,
                                                       const char *revoke_rel_path) {
    if (!userdata_root_abs || !userdata_root_abs[0]) {
        return false;
    }

    char current[JW_SVC_RESOLVE_BUF];
    snprintf(current, sizeof(current), "%s/%s", userdata_root_abs, state_root);

    struct stat st;
    if (lstat(current, &st) == 0 && S_ISLNK(st.st_mode)) {
        return true;
    }
    if (lstat(current, &st) != 0) {
        return false; /* state.root doesn't exist yet: nothing to check under it */
    }

    char work[JW_SVC_RESOLVE_BUF];
    snprintf(work, sizeof(work), "%s", revoke_rel_path);
    char *saveptr = NULL;
    char *tok = strtok_r(work, "/", &saveptr);
    while (tok) {
        size_t len = strlen(current);
        snprintf(current + len, sizeof(current) - len, "/%s", tok);
        if (lstat(current, &st) != 0) {
            return false; /* rest of the path doesn't exist yet */
        }
        if (S_ISLNK(st.st_mode)) {
            return true;
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return false;
}

bool jw_service_manifest_validate(const char *pak_json_text,
                                   const char *pak_abs_path,
                                   const char *userdata_root_abs_or_null,
                                   jw_service_manifest *out,
                                   char *reason, size_t reason_size) {
    if (!pak_json_text || !pak_abs_path || !out) {
        jw__svc_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(pak_json_text);
    if (!root) {
        jw__svc_set_reason(reason, reason_size, "invalid-json");
        return false;
    }
    bool ok = false;

    cJSON *top_id_item = cJSON_GetObjectItem(root, "id");
    const char *top_id = (cJSON_IsString(top_id_item) && top_id_item->valuestring)
                              ? top_id_item->valuestring
                              : "";

    cJSON *service = cJSON_GetObjectItem(root, "service");
    if (!cJSON_IsObject(service)) {
        jw__svc_set_reason(reason, reason_size, "missing-service");
        goto done;
    }

    cJSON *svc_id_item = cJSON_GetObjectItem(service, "id");
    const char *svc_id = (cJSON_IsString(svc_id_item) && svc_id_item->valuestring)
                              ? svc_id_item->valuestring
                              : "";

    if (!jw__svc_is_reverse_dns(top_id) || !jw__svc_is_reverse_dns(svc_id)) {
        jw__svc_set_reason(reason, reason_size, "malformed-id");
        goto done;
    }
    if (strcmp(top_id, svc_id) != 0) {
        jw__svc_set_reason(reason, reason_size, "id-mismatch");
        goto done;
    }
    snprintf(out->id, sizeof(out->id), "%s", svc_id);

    cJSON *schema_item = cJSON_GetObjectItem(service, "schema");
    /* cJSON tags true/false as cJSON_True/cJSON_False, distinct from
     * cJSON_Number, so this does not have the Python bool-vs-int aliasing
     * problem (True == 1) that a prior draft of this contract's fixture
     * validator had -- cJSON_IsNumber(schema_item) is already false for a
     * JSON boolean. */
    if (!cJSON_IsNumber(schema_item) || schema_item->valuedouble != 1.0) {
        jw__svc_set_reason(reason, reason_size, "unknown-schema");
        goto done;
    }

    cJSON *run = cJSON_GetObjectItem(service, "run");
    cJSON *run_path_item = cJSON_IsObject(run) ? cJSON_GetObjectItem(run, "path") : NULL;
    const char *run_path = (cJSON_IsString(run_path_item) && run_path_item->valuestring)
                                ? run_path_item->valuestring
                                : "";
    if (!run_path[0]) {
        jw__svc_set_reason(reason, reason_size, "run-path-missing");
        goto done;
    }
    if (jw__svc_path_is_absolute(run_path)) {
        jw__svc_set_reason(reason, reason_size, "absolute-run-path");
        goto done;
    }
    if (jw__svc_path_has_traversal(run_path)) {
        jw__svc_set_reason(reason, reason_size, "path-traversal");
        goto done;
    }

    jw_svc_run_path_check path_check = jw__svc_check_run_path(pak_abs_path, run_path);
    switch (path_check) {
        case JW_SVC_RUN_PATH_MISSING:
            jw__svc_set_reason(reason, reason_size, "run-path-missing");
            goto done;
        case JW_SVC_RUN_PATH_ESCAPES:
            jw__svc_set_reason(reason, reason_size, "escaping-symlink");
            goto done;
        case JW_SVC_RUN_PATH_NON_REGULAR:
            jw__svc_set_reason(reason, reason_size, "non-regular-executable");
            goto done;
        case JW_SVC_RUN_PATH_NOT_EXECUTABLE:
            jw__svc_set_reason(reason, reason_size, "run-path-not-executable");
            goto done;
        case JW_SVC_RUN_PATH_OK:
            break;
    }
    snprintf(out->run_path, sizeof(out->run_path), "%s", run_path);

    cJSON *args = cJSON_IsObject(run) ? cJSON_GetObjectItem(run, "args") : NULL;
    if (cJSON_IsArray(args)) {
        int n = cJSON_GetArraySize(args);
        if (n > JW_SVC_MAX_ARGS) {
            jw__svc_set_reason(reason, reason_size, "args-count-over-limit");
            goto done;
        }
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(args, i);
            const char *s = (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
            if (strlen(s) > JW_SVC_MAX_ARG_LEN) { /* UTF-8 byte length: strlen counts bytes */
                jw__svc_set_reason(reason, reason_size, "args-item-too-long");
                goto done;
            }
            snprintf(out->run_args[i], sizeof(out->run_args[i]), "%s", s);
        }
        out->run_args_count = n;
    }

    cJSON *restart_item = cJSON_GetObjectItem(service, "restart");
    const char *restart = (cJSON_IsString(restart_item) && restart_item->valuestring)
                               ? restart_item->valuestring
                               : "";
    if (strcmp(restart, "no") == 0) {
        out->restart_on_failure = false;
    } else if (strcmp(restart, "on-failure") == 0) {
        out->restart_on_failure = true;
    } else {
        jw__svc_set_reason(reason, reason_size, "unknown-restart-policy");
        goto done;
    }

    cJSON *default_enabled_item = cJSON_GetObjectItem(service, "default_enabled");
    if (!cJSON_IsBool(default_enabled_item) || cJSON_IsTrue(default_enabled_item)) {
        jw__svc_set_reason(reason, reason_size, "default-enabled-true");
        goto done;
    }

    cJSON *stop_grace_item = cJSON_GetObjectItem(service, "stop_grace_ms");
    if (stop_grace_item) {
        if (!cJSON_IsNumber(stop_grace_item)) {
            jw__svc_set_reason(reason, reason_size, "stop-grace-ms-non-integer");
            goto done;
        }
        double v = stop_grace_item->valuedouble;
        if (v != (double)(long long)v) {
            jw__svc_set_reason(reason, reason_size, "stop-grace-ms-non-integer");
            goto done;
        }
        if (v < 0) {
            jw__svc_set_reason(reason, reason_size, "stop-grace-ms-negative");
            goto done;
        }
        out->stop_grace_ms = (v > 15000.0) ? 15000 : (int)v;
    } else {
        out->stop_grace_ms = 5000;
    }

    cJSON *lifecycle = cJSON_GetObjectItem(service, "lifecycle");
    out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_IGNORE;
    out->stop_on_storage_change = false;
    out->stop_on_suspend = false;
    if (cJSON_IsObject(lifecycle)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, lifecycle) {
            if (!child->string) {
                continue;
            }
            bool known = strcmp(child->string, "game") == 0 ||
                         strcmp(child->string, "stop_on_storage_change") == 0 ||
                         strcmp(child->string, "stop_on_suspend") == 0;
            if (!known) {
                jw__svc_set_reason(reason, reason_size, "unknown-lifecycle-key");
                goto done;
            }
        }
        cJSON *game_item = cJSON_GetObjectItem(lifecycle, "game");
        if (game_item) {
            const char *g = (cJSON_IsString(game_item) && game_item->valuestring)
                                 ? game_item->valuestring
                                 : "";
            if (strcmp(g, "ignore") == 0) {
                out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_IGNORE;
            } else if (strcmp(g, "stop") == 0) {
                out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_STOP;
            } else if (strcmp(g, "notify") == 0) {
                out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_NOTIFY;
            } else {
                jw__svc_set_reason(reason, reason_size, "unknown-lifecycle-key");
                goto done;
            }
        }
        cJSON *storage_item = cJSON_GetObjectItem(lifecycle, "stop_on_storage_change");
        if (storage_item) {
            out->stop_on_storage_change = cJSON_IsTrue(storage_item);
        }
        cJSON *suspend_item = cJSON_GetObjectItem(lifecycle, "stop_on_suspend");
        if (suspend_item) {
            out->stop_on_suspend = cJSON_IsTrue(suspend_item);
        }
    }

    cJSON *state = cJSON_GetObjectItem(root, "state");
    out->has_state = cJSON_IsObject(state);
    if (out->has_state) {
        cJSON *root_item = cJSON_GetObjectItem(state, "root");
        const char *state_root = (cJSON_IsString(root_item) && root_item->valuestring)
                                      ? root_item->valuestring
                                      : "";
        snprintf(out->state_root, sizeof(out->state_root), "%s", state_root);

        cJSON *revoke = cJSON_GetObjectItem(state, "revoke_on_uninstall");
        if (cJSON_IsArray(revoke)) {
            int n = cJSON_GetArraySize(revoke);
            if (n > JW_SVC_MAX_STATE_LIST) {
                jw__svc_set_reason(reason, reason_size, "revoke-on-uninstall-count-over-limit");
                goto done;
            }
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(revoke, i);
                const char *s = (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
                if (jw__svc_path_is_absolute(s)) {
                    jw__svc_set_reason(reason, reason_size, "revoke-on-uninstall-absolute");
                    goto done;
                }
                if (jw__svc_path_has_traversal(s)) {
                    jw__svc_set_reason(reason, reason_size, "revoke-on-uninstall-traversal");
                    goto done;
                }
                if (jw__svc_revoke_path_has_symlink_component(userdata_root_abs_or_null,
                                                                state_root, s)) {
                    jw__svc_set_reason(reason, reason_size, "revoke-on-uninstall-symlink-component");
                    goto done;
                }
                snprintf(out->state_revoke_on_uninstall[i],
                         sizeof(out->state_revoke_on_uninstall[i]), "%s", s);
            }
            out->state_revoke_count = n;
        }

        cJSON *retained = cJSON_GetObjectItem(state, "retained_roots");
        if (cJSON_IsArray(retained)) {
            int n = cJSON_GetArraySize(retained);
            if (n > JW_SVC_MAX_STATE_LIST) {
                jw__svc_set_reason(reason, reason_size, "retained-roots-count-over-limit");
                goto done;
            }
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(retained, i);
                const char *s = (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
                if (jw__svc_path_is_absolute(s)) {
                    jw__svc_set_reason(reason, reason_size, "retained-roots-absolute");
                    goto done;
                }
                if (jw__svc_path_has_traversal(s)) {
                    jw__svc_set_reason(reason, reason_size, "retained-roots-traversal");
                    goto done;
                }
                snprintf(out->state_retained_roots[i],
                         sizeof(out->state_retained_roots[i]), "%s", s);
            }
            out->state_retained_count = n;
        }
    }

    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}
