/* clock_gettime()/CLOCK_MONOTONIC and fsync() need broader-than-bare-C11
 * visibility on glibc; see the matching comment in internal/services/
 * lease.c. Must precede every #include. */
#define _GNU_SOURCE

#include "internal/services/reservation.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void jw__res_set_reason(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug);
    }
}

/* service_id is used as a single path component. Independently checked
 * here rather than trusted from a caller, matching the same posture
 * internal/services/lease.c takes for the same reason. */
static bool jw__res_service_id_is_safe(const char *service_id) {
    if (!service_id || !service_id[0]) {
        return false;
    }
    if (strchr(service_id, '/') != NULL) {
        return false;
    }
    return strcmp(service_id, ".") != 0 && strcmp(service_id, "..") != 0;
}

static const char *jw__res_game_policy_to_str(jw_svc_reservation_game_policy policy) {
    switch (policy) {
        case JW_SVC_RESERVATION_GAME_STOP:
            return "stop";
        case JW_SVC_RESERVATION_GAME_NOTIFY:
            return "notify";
        case JW_SVC_RESERVATION_GAME_IGNORE:
        default:
            return "ignore";
    }
}

static bool jw__res_game_policy_from_str(const char *s, jw_svc_reservation_game_policy *out) {
    if (!s) {
        return false;
    }
    if (strcmp(s, "ignore") == 0) {
        *out = JW_SVC_RESERVATION_GAME_IGNORE;
        return true;
    }
    if (strcmp(s, "stop") == 0) {
        *out = JW_SVC_RESERVATION_GAME_STOP;
        return true;
    }
    if (strcmp(s, "notify") == 0) {
        *out = JW_SVC_RESERVATION_GAME_NOTIFY;
        return true;
    }
    return false;
}

long long jw_svc_reservation_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static bool jw__res_paths(const char *runtime_dir, const char *service_id,
                          char *dir_out, size_t dir_out_size,
                          char *final_out, size_t final_out_size,
                          char *temp_out, size_t temp_out_size) {
    int n = snprintf(dir_out, dir_out_size, "%s/services/%s", runtime_dir, service_id);
    if (n < 0 || (size_t)n >= dir_out_size) {
        return false;
    }
    n = snprintf(final_out, final_out_size, "%s/reservation", dir_out);
    if (n < 0 || (size_t)n >= final_out_size) {
        return false;
    }
    n = snprintf(temp_out, temp_out_size, "%s/.reservation.tmp", dir_out);
    if (n < 0 || (size_t)n >= temp_out_size) {
        return false;
    }
    return true;
}

/* The atomic-write recipe contracts.md's LIFE-1 section specifies for its
 * own active-launch record, reused verbatim: same-directory temporary,
 * complete write, file fsync, rename, then parent-directory fsync. */
static bool jw__res_write_atomic(const char *temp_path, const char *final_path,
                                 const char *dir_path, const char *content,
                                 char *reason, size_t reason_size) {
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    size_t len = strlen(content);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, content + written, len - written);
        if (n < 0) {
            close(fd);
            jw__res_set_reason(reason, reason_size, "write-failed");
            return false;
        }
        written += (size_t)n;
    }

    int fsync_rc = fsync(fd);
    int close_rc = close(fd);
    if (fsync_rc != 0 || close_rc != 0) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    if (rename(temp_path, final_path) != 0) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd < 0) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    int dir_fsync_rc = fsync(dir_fd);
    int dir_close_rc = close(dir_fd);
    if (dir_fsync_rc != 0 || dir_close_rc != 0) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    return true;
}

bool jw_svc_reservation_write(const char *runtime_dir, const char *service_id,
                              const jw_svc_reservation *reservation,
                              char *reason, size_t reason_size) {
    if (!runtime_dir || !runtime_dir[0] || !jw__res_service_id_is_safe(service_id) ||
        !reservation || reservation->pgid <= 0) {
        jw__res_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    char dir[PATH_MAX], final_path[PATH_MAX], temp_path[PATH_MAX];
    if (!jw__res_paths(runtime_dir, service_id, dir, sizeof(dir),
                       final_path, sizeof(final_path), temp_path, sizeof(temp_path))) {
        jw__res_set_reason(reason, reason_size, "path-too-long");
        return false;
    }

    struct stat dir_st;
    if (lstat(dir, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) {
        /* This module does not create or harden the service directory --
         * that is jw_svc_lease_acquire()'s job, which SVC-1's ordering
         * runs before this record is ever written. */
        jw__res_set_reason(reason, reason_size, "service-dir-unavailable");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        jw__res_set_reason(reason, reason_size, "out-of-memory");
        return false;
    }
    /* cJSON stores every number as a double. A microsecond
     * CLOCK_MONOTONIC value fits exactly (no precision loss) as long as
     * it stays under 2^53 microseconds -- about 285 years of uptime,
     * which is not a real constraint on any device this runs on. */
    cJSON_AddNumberToObject(root, "pgid", (double)reservation->pgid);
    cJSON_AddNumberToObject(root, "launch_instant_us", (double)reservation->launch_instant_us);
    cJSON_AddStringToObject(root, "game", jw__res_game_policy_to_str(reservation->game_policy));
    cJSON_AddBoolToObject(root, "stop_on_storage_change", reservation->stop_on_storage_change);
    cJSON_AddBoolToObject(root, "stop_on_suspend", reservation->stop_on_suspend);

    char *json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_text) {
        jw__res_set_reason(reason, reason_size, "out-of-memory");
        return false;
    }

    bool ok = jw__res_write_atomic(temp_path, final_path, dir, json_text, reason, reason_size);
    free(json_text);
    return ok;
}

bool jw_svc_reservation_read(const char *runtime_dir, const char *service_id,
                             jw_svc_reservation *out,
                             char *reason, size_t reason_size) {
    if (!runtime_dir || !runtime_dir[0] || !jw__res_service_id_is_safe(service_id) || !out) {
        jw__res_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    memset(out, 0, sizeof(*out));

    char dir[PATH_MAX], final_path[PATH_MAX], temp_path[PATH_MAX];
    if (!jw__res_paths(runtime_dir, service_id, dir, sizeof(dir),
                       final_path, sizeof(final_path), temp_path, sizeof(temp_path))) {
        jw__res_set_reason(reason, reason_size, "path-too-long");
        return false;
    }

    errno = 0;
    FILE *fp = fopen(final_path, "rb");
    if (!fp) {
        jw__res_set_reason(reason, reason_size,
                           errno == ENOENT ? "reservation-missing" : "reservation-corrupt");
        return false;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    bool truncated = false;
    if (n == sizeof(buf) - 1) {
        int extra = fgetc(fp);
        truncated = extra != EOF;
    }
    bool read_failed = ferror(fp);
    fclose(fp);
    if (read_failed || truncated || n == 0) {
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }

    cJSON *pgid_item = cJSON_GetObjectItemCaseSensitive(root, "pgid");
    cJSON *instant_item = cJSON_GetObjectItemCaseSensitive(root, "launch_instant_us");
    cJSON *game_item = cJSON_GetObjectItemCaseSensitive(root, "game");
    cJSON *storage_item = cJSON_GetObjectItemCaseSensitive(root, "stop_on_storage_change");
    cJSON *suspend_item = cJSON_GetObjectItemCaseSensitive(root, "stop_on_suspend");

    bool shape_ok = cJSON_IsNumber(pgid_item) && pgid_item->valuedouble > 0 &&
                    cJSON_IsNumber(instant_item) &&
                    cJSON_IsString(game_item) && game_item->valuestring &&
                    cJSON_IsBool(storage_item) && cJSON_IsBool(suspend_item);

    jw_svc_reservation_game_policy policy = JW_SVC_RESERVATION_GAME_IGNORE;
    if (shape_ok && !jw__res_game_policy_from_str(game_item->valuestring, &policy)) {
        shape_ok = false;
    }

    if (!shape_ok) {
        cJSON_Delete(root);
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }

    out->pgid = (pid_t)pgid_item->valuedouble;
    out->launch_instant_us = (long long)instant_item->valuedouble;
    out->game_policy = policy;
    out->stop_on_storage_change = cJSON_IsTrue(storage_item);
    out->stop_on_suspend = cJSON_IsTrue(suspend_item);
    cJSON_Delete(root);
    return true;
}
