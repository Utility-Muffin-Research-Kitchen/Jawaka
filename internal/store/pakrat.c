#include "internal/store/pakrat.h"

#include "cJSON.h"
#include "internal/db/db.h"
#include "internal/discovery/discovery.h"
#include "internal/ipc/ipc.h"
#include "internal/store/catalog_source.h"
#include "internal/store/managed_apps.h"
#include "internal/update/sha256.h"
#include "internal/store/pakrat_state.h"
#include "miniz.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int jw__curl_ready = 0;

#define JW_PAKRAT_CONNECT_TIMEOUT_S 10L
#define JW_PAKRAT_LOW_SPEED_LIMIT_BPS 1024L
#define JW_PAKRAT_LOW_SPEED_TIME_S 30L
#define JW_PAKRAT_MAX_REDIRS 5L
/* Hard ceiling on a downloaded artifact regardless of the catalog's advertised
   size, so a lying catalog cannot fill the (writable) SD and flip it read-only. */
#define JW_PAKRAT_DOWNLOAD_ABS_MAX_BYTES (384LL * 1024LL * 1024LL)
/* Slack added over the catalog's advertised size before aborting the transfer. */
#define JW_PAKRAT_DOWNLOAD_SLACK_BYTES (64LL * 1024LL)

typedef struct {
    FILE *fp;
    curl_off_t written;
    curl_off_t ceiling;
} jw_download_sink;

static size_t jw__download_write(void *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
    jw_download_sink *sink = (jw_download_sink *)userdata;
    size_t bytes = size * nmemb;
    if (size != 0 && bytes / size != nmemb) {
        return 0;
    }
    if ((curl_off_t)bytes > sink->ceiling - sink->written) {
        /* Returning short of nmemb aborts the transfer (CURLE_WRITE_ERROR). */
        return 0;
    }
    size_t got = fwrite(ptr, 1, bytes, sink->fp);
    sink->written += (curl_off_t)got;
    return got;
}

static int jw__ensure_curl(void) {
    if (jw__curl_ready) {
        return 0;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return -1;
    }
    jw__curl_ready = 1;
    return 0;
}

static int jw__copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s", value);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join2(char *out, size_t out_size, const char *a, const char *b) {
    int n = snprintf(out, out_size, "%s/%s", a, b);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join3(char *out, size_t out_size, const char *a, const char *b,
                     const char *c) {
    int n = snprintf(out, out_size, "%s/%s/%s", a, b, c);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static bool jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static void jw__configure_curl_ca(CURL *curl) {
    static const char *ca_files[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/ssl/cert.pem",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/run/libreelec/cacert.pem",
        NULL,
    };
    for (int i = 0; ca_files[i]; i++) {
        if (jw__path_exists(ca_files[i])) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_files[i]);
            return;
        }
    }
    if (jw__path_exists("/etc/ssl/certs")) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, "/etc/ssl/certs");
    }
}

static bool jw__is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int jw__mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        return -1;
    }
    char tmp[PATH_MAX];
    if (jw__copy(tmp, sizeof(tmp), path) != 0) {
        return -1;
    }
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int jw__mkdir_parent(const char *path) {
    char parent[PATH_MAX];
    if (jw__copy(parent, sizeof(parent), path) != 0) {
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
    return jw__mkdir_p(parent, 0755);
}

static void jw__pakrat_log(const jw_pakrat_context *ctx, const char *fmt, ...) {
    if (!ctx || !ctx->state_dir[0] || !fmt) {
        return;
    }
    char log_dir[PATH_MAX];
    char log_path[PATH_MAX];
    if (jw__join3(log_dir, sizeof(log_dir), ctx->state_dir, "store", "logs") != 0 ||
        jw__join2(log_path, sizeof(log_path), log_dir, "pakrat.log") != 0 ||
        jw__mkdir_p(log_dir, 0755) != 0) {
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

static int jw__remove_tree(const char *path) {
    struct stat st;
    if (!path || !path[0] || lstat(path, &st) != 0) {
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
        if (jw__join2(child, sizeof(child), path, entry->d_name) != 0 ||
            jw__remove_tree(child) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return rmdir(path) == 0 || errno == ENOENT ? 0 : -1;
}

static bool jw__safe_name(const char *name) {
    return name && name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
           !strchr(name, '/') && !strchr(name, '\\');
}

static bool jw__safe_rel_path(const char *path) {
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

static bool jw__has_suffix(const char *value, const char *suffix) {
    if (!value || !suffix) {
        return false;
    }
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           strcmp(value + value_len - suffix_len, suffix) == 0;
}

static int jw__validate_context(const jw_pakrat_context *ctx) {
    if (!ctx || !ctx->platform[0] || !ctx->sdcard_root[0] || !ctx->state_dir[0] ||
        !ctx->db_path[0] || !ctx->platform_root[0]) {
        return -1;
    }
    if (jw__mkdir_p(ctx->state_dir, 0755) != 0) {
        fprintf(stderr, "could not create state dir: %s\n", ctx->state_dir);
        return -1;
    }
    if (!jw__is_dir(ctx->platform_root)) {
        fprintf(stderr, "platform root missing: %s\n", ctx->platform_root);
        return -1;
    }
    return 0;
}

static const char *jw__json_string(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static int jw__download_file(const char *url, const char *path,
                             long long expected_size, int is_dev) {
    if (jw__ensure_curl() != 0) {
        return -1;
    }
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.partial", path) >= (int)sizeof(tmp) ||
        jw__mkdir_parent(path) != 0) {
        return -1;
    }
    /* Bound the transfer to the catalog's advertised size (plus a little slack),
       clamped to an absolute ceiling. If the size is unknown (<= 0), fall back to
       the absolute ceiling so the download is still bounded. */
    curl_off_t ceiling = JW_PAKRAT_DOWNLOAD_ABS_MAX_BYTES;
    if (expected_size > 0 &&
        expected_size <=
            JW_PAKRAT_DOWNLOAD_ABS_MAX_BYTES - JW_PAKRAT_DOWNLOAD_SLACK_BYTES) {
        ceiling = (curl_off_t)(expected_size + JW_PAKRAT_DOWNLOAD_SLACK_BYTES);
    }
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        return -1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        unlink(tmp);
        return -1;
    }
    jw_download_sink sink = {fp, 0, ceiling};
    char error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    jw__configure_curl_ca(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, JW_PAKRAT_MAX_REDIRS);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, is_dev ? "http,https" : "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                     is_dev ? "http,https" : "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     is_dev ? (CURLPROTO_HTTPS | CURLPROTO_HTTP) : CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     is_dev ? (CURLPROTO_HTTPS | CURLPROTO_HTTP) : CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, ceiling);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, JW_PAKRAT_CONNECT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, JW_PAKRAT_LOW_SPEED_LIMIT_BPS);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, JW_PAKRAT_LOW_SPEED_TIME_S);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jw__download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jawaka-pakrat/1");
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    int close_rc = fclose(fp);
    if (rc != CURLE_OK || close_rc != 0 || (http != 0 && http >= 400)) {
        fprintf(stderr, "Pak Rat download failed: url=%s curl=%d http=%ld%s%s\n",
                url, (int)rc, http, error[0] ? " error=" : "",
                error[0] ? error : "");
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int jw__extract_zip_single_pak(const char *zip_path, const char *stage_dir,
                                      const char *install_name,
                                      char *out_pak_dir, size_t out_size) {
    if (jw__remove_tree(stage_dir) != 0 || jw__mkdir_p(stage_dir, 0755) != 0) {
        return -1;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        return -1;
    }

    char top[256] = "";
    int files = 0;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || !st.m_is_supported ||
            !jw__safe_rel_path(st.m_filename)) {
            mz_zip_reader_end(&zip);
            return -1;
        }

        char entry_top[256];
        const char *slash = strchr(st.m_filename, '/');
        size_t top_len = slash ? (size_t)(slash - st.m_filename) : strlen(st.m_filename);
        if (top_len == 0 || top_len >= sizeof(entry_top)) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        memcpy(entry_top, st.m_filename, top_len);
        entry_top[top_len] = '\0';
        if (!jw__has_suffix(entry_top, ".pak")) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (!top[0]) {
            jw__copy(top, sizeof(top), entry_top);
        } else if (strcmp(top, entry_top) != 0) {
            mz_zip_reader_end(&zip);
            return -1;
        }

        char dest[PATH_MAX];
        if (jw__join2(dest, sizeof(dest), stage_dir, st.m_filename) != 0) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (st.m_is_directory) {
            if (jw__mkdir_p(dest, 0755) != 0) {
                mz_zip_reader_end(&zip);
                return -1;
            }
            continue;
        }
        if (jw__mkdir_parent(dest) != 0 ||
            !mz_zip_reader_extract_to_file(&zip, i, dest, 0)) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        mode_t mode = (mode_t)((st.m_external_attr >> 16) & 0777u);
        chmod(dest, mode ? mode : 0644);
        files++;
    }

    mz_zip_reader_end(&zip);
    if (!top[0] || files <= 0 || strcmp(top, install_name) != 0) {
        return -1;
    }
    return jw__join2(out_pak_dir, out_size, stage_dir, top);
}

static int jw__validate_runtime_manifest(const jw_pakrat_catalog_package *pkg,
                                         const char *pak_dir) {
    if (!jw__safe_rel_path(pkg->runtime_manifest_path)) {
        return -1;
    }
    char path[PATH_MAX];
    if (jw__join2(path, sizeof(path), pak_dir, pkg->runtime_manifest_path) != 0) {
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
    if (size <= 0 || size > 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    char *json = (char *)malloc((size_t)size + 1u);
    if (!json) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(json, 1, (size_t)size, fp);
    fclose(fp);
    json[got] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        return -1;
    }
    int ok = strcmp(jw__json_string(root, "platform"), pkg->platform) == 0 &&
             strcmp(jw__json_string(root, "pak_version"), pkg->version) == 0;
    cJSON_Delete(root);
    return ok ? 0 : -1;
}

static int jw__notify_daemon_scan(const jw_pakrat_context *ctx) {
    if (!ctx || !ctx->socket_path[0] || !jw__path_exists(ctx->socket_path)) {
        return 1;
    }

    const char *request = "{\"type\":\"scan-library\"}";
    char *response = NULL;
    size_t response_len = 0;
    if (jw_ipc_request(ctx->socket_path, request, strlen(request),
                       &response, &response_len) != 0) {
        free(response);
        return -1;
    }
    printf("daemon-scan: ");
    fwrite(response, 1, response_len, stdout);
    fputc('\n', stdout);
    free(response);
    return 0;
}

static int jw__target_path_for_install(const jw_pakrat_context *ctx,
                                       const char *install_path,
                                       char *out, size_t out_size) {
    const char *path = install_path;
    if (strncmp(path, "Apps/", 5) == 0) {
        path += 5;
    }
    if (!jw__safe_rel_path(path)) {
        return -1;
    }
    return jw__join3(out, out_size, ctx->sdcard_root, "Apps", path);
}

int jw_pakrat_rescan(const jw_pakrat_context *ctx) {
    if (jw__validate_context(ctx) != 0 || jw__mkdir_parent(ctx->db_path) != 0) {
        return -1;
    }
    /* jw_scan_library reads getenv("PLATFORM"); install/uninstall run this on a
       worker thread while the render thread is live, and POSIX setenv is not
       thread-safe against a concurrent getenv. ctx->platform is the compiled
       platform id (process-wide constant), so only write when the value is
       actually missing or wrong -- the steady-state rescan then performs no env
       mutation and races nothing. */
    const char *cur_platform = getenv("PLATFORM");
    if (!cur_platform || strcmp(cur_platform, ctx->platform) != 0) {
        setenv("PLATFORM", ctx->platform, 1);
    }
    sqlite3 *db = NULL;
    if (jw_db_open(ctx->db_path, &db) != 0 || !db) {
        return -1;
    }
    jw_scan_result result;
    int rc = jw_db_apply_schema(db) == 0 &&
             jw_scan_library(db, ctx->sdcard_root, &result) == 0 ? 0 : -1;
    jw_db_close(db);
    if (rc == 0) {
        printf("rescan: games=%d systems=%d apps=%d\n",
               result.game_count, result.system_count, result.app_count);
    }
    return rc;
}

int jw_pakrat_install_app(const jw_pakrat_context *ctx, const char *store_id,
                          int allow_adopt) {
    if (jw__validate_context(ctx) != 0 || !store_id || !store_id[0]) {
        return -1;
    }

    int rc = -1;
    char artifact_path[PATH_MAX] = "";
    char app_stage_dir[PATH_MAX] = "";
    char target_tmp[PATH_MAX] = "";
    char catalog_url[1200] = "";
    char catalog_base[1024] = "";
    int catalog_is_dev = 0;
    if (jw_pakrat_catalog_base_url(ctx->state_dir, catalog_base,
                                   sizeof(catalog_base),
                                   &catalog_is_dev) == 0 &&
        catalog_base[0]) {
        snprintf(catalog_url, sizeof(catalog_url), "%sstorefront.json",
                 catalog_base);
    }
    jw__pakrat_log(ctx, "install-start store_id=%s platform=%s catalog_mode=%s catalog_url=%s",
                   store_id, ctx->platform,
                   catalog_is_dev ? "dev" : "official",
                   catalog_url[0] ? catalog_url : "-");

    jw_pakrat_catalog_package pkg;
    int is_dev = 0;
    int parse_rc = jw_pakrat_find_catalog_package(ctx, store_id, &pkg, &is_dev);
    if (parse_rc != 0) {
        fprintf(stderr, parse_rc > 0 ?
                "Pak Rat catalog URL is not configured or store id/platform not found\n" :
                "invalid storefront\n");
        goto cleanup;
    }

    if (!jw__safe_name(pkg.install_name) || !jw__has_suffix(pkg.install_name, ".pak") ||
        !jw__safe_name(pkg.artifact_name) ||
        !jw_pakrat_catalog_url_allowed(pkg.artifact_url, is_dev)) {
        fprintf(stderr, "catalog package failed safety checks\n");
        goto cleanup;
    }

    int blocked = jw_pakrat_managed_app_path_blocked_from_platform(
        ctx->platform_root, pkg.install_path);
    if (blocked != 0) {
        fprintf(stderr, blocked > 0 ? "target path is release-managed\n" :
                "could not read managed app policy\n");
        goto cleanup;
    }

    jw_pakrat_install existing;
    int install_row = jw_db_pakrat_get_install(ctx->db_path, store_id, &existing);
    if (install_row < 0) {
        goto cleanup;
    }

    char target[PATH_MAX];
    if (jw__target_path_for_install(ctx, pkg.install_path, target, sizeof(target)) != 0 ||
        snprintf(target_tmp, sizeof(target_tmp), "%s.partial", target) >=
            (int)sizeof(target_tmp)) {
        goto cleanup;
    }
    if (jw__path_exists(target) && install_row != 0 && !allow_adopt) {
        fprintf(stderr, "target exists without Pak Rat ownership; adoption requires consent\n");
        goto cleanup;
    }
    if (install_row == 0 && strcmp(existing.install_path, pkg.install_path) != 0) {
        fprintf(stderr, "installed record uses another path; move is deferred\n");
        goto cleanup;
    }

    char downloads_dir[PATH_MAX];
    char staging_dir[PATH_MAX];
    if (jw__join3(downloads_dir, sizeof(downloads_dir), ctx->state_dir,
                  "store", "downloads") != 0 ||
        jw__join3(staging_dir, sizeof(staging_dir), ctx->state_dir,
                  "store", "staging") != 0 ||
        jw__mkdir_p(downloads_dir, 0755) != 0 ||
        jw__join2(artifact_path, sizeof(artifact_path), downloads_dir,
                  pkg.artifact_name) != 0) {
        goto cleanup;
    }

    jw__pakrat_log(ctx,
                   "download-start store_id=%s version=%s target=Apps/%s artifact_url=%s artifact_sha256=%s artifact_size=%lld",
                   store_id, pkg.version, pkg.install_path,
                   pkg.artifact_url, pkg.artifact_sha256, pkg.artifact_size);
    printf("download: %s\n", pkg.artifact_url);
    if (jw__download_file(pkg.artifact_url, artifact_path, pkg.artifact_size,
                          is_dev) != 0) {
        fprintf(stderr, "download failed\n");
        goto cleanup;
    }
    struct stat artifact_st;
    if (stat(artifact_path, &artifact_st) != 0 ||
        (long long)artifact_st.st_size != pkg.artifact_size) {
        fprintf(stderr, "artifact size mismatch\n");
        goto cleanup;
    }
    char sha[65] = "";
    char sha_err[256];
    if (jw_sha256_file_hex(artifact_path, sha, sha_err, sizeof(sha_err)) != 0 ||
        strcmp(sha, pkg.artifact_sha256) != 0) {
        fprintf(stderr, "artifact SHA-256 mismatch\n");
        goto cleanup;
    }

    char extracted_pak[PATH_MAX];
    if (jw__join2(app_stage_dir, sizeof(app_stage_dir), staging_dir, store_id) != 0 ||
        jw__extract_zip_single_pak(artifact_path, app_stage_dir,
                                   pkg.install_name, extracted_pak,
                                   sizeof(extracted_pak)) != 0 ||
        jw__validate_runtime_manifest(&pkg, extracted_pak) != 0) {
        fprintf(stderr, "artifact extraction/validation failed\n");
        goto cleanup;
    }

    if (jw__mkdir_parent(target) != 0 ||
        jw__remove_tree(target_tmp) != 0 ||
        rename(extracted_pak, target_tmp) != 0 ||
        jw__remove_tree(target) != 0 ||
        rename(target_tmp, target) != 0) {
        fprintf(stderr, "install target replacement failed\n");
        goto cleanup;
    }

    if (jw_db_pakrat_upsert_install(ctx->db_path, store_id, pkg.version,
                                    pkg.platform, pkg.install_path, sha, NULL) != 0 ||
        jw_pakrat_rescan(ctx) != 0) {
        goto cleanup;
    }
    (void)jw__notify_daemon_scan(ctx);

    printf("installed: %s %s -> Apps/%s\n", store_id, pkg.version,
           pkg.install_path);
    jw__pakrat_log(ctx, "install-complete store_id=%s version=%s target=Apps/%s",
                   store_id, pkg.version, pkg.install_path);
    rc = 0;

cleanup:
    if (rc != 0) {
        jw__pakrat_log(ctx, "install-failed store_id=%s", store_id);
    }
    if (artifact_path[0]) {
        (void)jw__remove_tree(artifact_path);
    }
    if (app_stage_dir[0]) {
        (void)jw__remove_tree(app_stage_dir);
    }
    if (rc != 0 && target_tmp[0]) {
        (void)jw__remove_tree(target_tmp);
    }
    return rc;
}

int jw_pakrat_uninstall_app(const jw_pakrat_context *ctx, const char *store_id) {
    if (jw__validate_context(ctx) != 0 || !store_id || !store_id[0]) {
        return -1;
    }

    jw_pakrat_install row;
    int rc = jw_db_pakrat_get_install(ctx->db_path, store_id, &row);
    if (rc > 0) {
        printf("not installed: %s\n", store_id);
        return 0;
    }
    if (rc < 0) {
        return -1;
    }

    int blocked = jw_pakrat_managed_app_path_blocked_from_platform(
        ctx->platform_root, row.install_path);
    if (blocked != 0) {
        fprintf(stderr, blocked > 0 ? "target path is release-managed\n" :
                "could not read managed app policy\n");
        return -1;
    }

    char target[PATH_MAX];
    if (jw__target_path_for_install(ctx, row.install_path, target, sizeof(target)) != 0) {
        return -1;
    }
    if (jw__remove_tree(target) != 0 ||
        jw_db_pakrat_remove_install(ctx->db_path, store_id) != 0 ||
        jw_pakrat_rescan(ctx) != 0) {
        return -1;
    }
    (void)jw__notify_daemon_scan(ctx);

    printf("uninstalled: %s (userdata preserved)\n", store_id);
    return 0;
}
