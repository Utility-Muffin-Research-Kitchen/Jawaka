#include "internal/store/pakrat_state.h"

#include "internal/db/db.h"
#include "internal/platform/leaf_version.h"
#include "internal/store/catalog_source.h"
#include "internal/store/managed_apps.h"
#include "internal/store/pakrat_state_logic.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char *data;
    size_t size;
} jw_mem;

#define JW_PAKRAT_CATALOG_MAX_BYTES (8u * 1024u * 1024u)
#define JW_PAKRAT_CONNECT_TIMEOUT_S 10L
#define JW_PAKRAT_TRANSFER_TIMEOUT_S 30L
#define JW_PAKRAT_MAX_REDIRS 5L

static int jw__curl_ready = 0;

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

static int jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

/* Resolve a catalog install_path ("[Apps/]<rel>") to the on-disk pak path, so a
   manually-present (unmanaged) pak can be told apart from a merely-available
   one. Mirrors jw__target_path_for_install in pakrat.c. */
static int jw__install_target_path(const jw_pakrat_context *ctx,
                                   const char *install_path,
                                   char *out, size_t out_size) {
    const char *p = install_path;
    if (strncmp(p, "Apps/", 5) == 0) {
        p += 5;
    }
    int n = snprintf(out, out_size, "%s/Apps/%s", ctx->sdcard_root, p);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
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

static size_t jw__curl_mem_write(void *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
    jw_mem *mem = (jw_mem *)userdata;
    size_t bytes = size * nmemb;
    if (size != 0 && bytes / size != nmemb) {
        return 0;
    }
    if (mem->size > JW_PAKRAT_CATALOG_MAX_BYTES ||
        bytes > JW_PAKRAT_CATALOG_MAX_BYTES - mem->size) {
        return 0;
    }
    char *next = (char *)realloc(mem->data, mem->size + bytes + 1u);
    if (!next) {
        return 0;
    }
    mem->data = next;
    memcpy(mem->data + mem->size, ptr, bytes);
    mem->size += bytes;
    mem->data[mem->size] = '\0';
    return bytes;
}

static int jw__fetch_url(const char *url, int is_dev, jw_mem *out) {
    if (!url || !out || jw__ensure_curl() != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, JW_PAKRAT_CONNECT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, JW_PAKRAT_TRANSFER_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jw__curl_mem_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jawaka-pakrat/1");
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || (http != 0 && http >= 400)) {
        fprintf(stderr, "Pak Rat fetch failed: url=%s curl=%d http=%ld%s%s\n",
                url, (int)rc, http, error[0] ? " error=" : "",
                error[0] ? error : "");
        free(out->data);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

static int jw__fetch_storefront(const jw_pakrat_context *ctx, jw_mem *out,
                                int *out_is_dev_override) {
    if (!ctx || !ctx->state_dir[0] || !out) {
        return -1;
    }

    char base_url[1024];
    int is_dev = 0;
    if (jw_pakrat_catalog_base_url(ctx->state_dir, base_url, sizeof(base_url),
                                   &is_dev) != 0) {
        return -1;
    }
    if (!base_url[0]) {
        return 1;
    }
    if (out_is_dev_override) {
        *out_is_dev_override = is_dev;
    }

    char storefront_url[1200];
    if (snprintf(storefront_url, sizeof(storefront_url), "%sstorefront.json",
                 base_url) >= (int)sizeof(storefront_url)) {
        return -1;
    }
    if (jw__fetch_url(storefront_url, is_dev, out) != 0) {
        fprintf(stderr, "failed to fetch %s\n", storefront_url);
        return -1;
    }
    return 0;
}

static const char *jw__installed_leaf_version(const jw_pakrat_context *ctx,
                                               jw_installed_release *release) {
    memset(release, 0, sizeof(*release));
    return jw_installed_release_read(ctx->state_dir, release) == 0
               ? release->version
               : "";
}

static int jw__version_cmp_text(const char *left, const char *right,
                                int *out_cmp) {
    int left_version[3];
    int right_version[3];
    if (!left || !right || !out_cmp ||
        jw_pak_version_parse(left, left_version) != 0 ||
        jw_pak_version_parse(right, right_version) != 0) {
        return -1;
    }
    *out_cmp = jw_version_cmp(left_version, right_version);
    return 0;
}

static void jw__hide_obsolete_gate(jw_pakrat_app_state *state) {
    if (!state->gated_version[0] || !state->installed_owned) {
        return;
    }
    int cmp = 0;
    if (jw__version_cmp_text(state->gated_version,
                             state->installed_version, &cmp) != 0 ||
        cmp <= 0) {
        state->gated_version[0] = '\0';
        state->gated_min_leaf_version[0] = '\0';
    }
}

const char *jw_pakrat_app_status_name(jw_pakrat_app_status status) {
    switch (status) {
    case JW_PAKRAT_APP_AVAILABLE:
        return "available";
    case JW_PAKRAT_APP_INSTALLED:
        return "installed";
    case JW_PAKRAT_APP_UPDATE_AVAILABLE:
        return "update_available";
    case JW_PAKRAT_APP_STALE:
        return "stale";
    case JW_PAKRAT_APP_UNMANAGED:
        return "unmanaged";
    }
    return "unknown";
}

int jw_pakrat_find_catalog_package(const jw_pakrat_context *ctx,
                                   const char *store_id,
                                   jw_pakrat_catalog_package *out,
                                   int *out_is_dev_override) {
    if (!ctx || !ctx->platform[0] || !store_id || !store_id[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    jw_mem storefront;
    int is_dev = 0;
    int fetch_rc = jw__fetch_storefront(ctx, &storefront, &is_dev);
    if (fetch_rc != 0) {
        return fetch_rc;
    }

    jw_installed_release release;
    jw_pakrat_catalog_selection selections[128];
    int count = 0;
    int parse_rc = jw_pakrat_catalog_parse_and_select(
        storefront.data, ctx->platform,
        jw__installed_leaf_version(ctx, &release), is_dev,
        selections, (int)(sizeof(selections) / sizeof(selections[0])), &count);
    free(storefront.data);
    if (parse_rc != 0) {
        return parse_rc;
    }

    for (int i = 0; i < count; i++) {
        if (strcmp(selections[i].package.id, store_id) == 0) {
            *out = selections[i].package;
            if (out_is_dev_override) {
                *out_is_dev_override = is_dev;
            }
            return 0;
        }
    }
    return 1;
}

int jw_pakrat_find_catalog_package_version(
    const jw_pakrat_context *ctx,
    const char *store_id,
    const char *version,
    jw_pakrat_catalog_package *out,
    int *out_is_dev_override) {
    if (!ctx || !ctx->platform[0] || !store_id || !store_id[0] ||
        !version || !version[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    jw_mem storefront;
    int is_dev = 0;
    int fetch_rc = jw__fetch_storefront(ctx, &storefront, &is_dev);
    if (fetch_rc != 0) {
        return fetch_rc;
    }
    int parse_rc = jw_pakrat_catalog_find_exact(
        storefront.data, ctx->platform, store_id, version, out);
    free(storefront.data);
    if (parse_rc == 0 && out_is_dev_override) {
        *out_is_dev_override = is_dev;
    }
    return parse_rc;
}

int jw_pakrat_list_app_states(const jw_pakrat_context *ctx,
                              jw_pakrat_app_state *out,
                              int max_count,
                              int *out_count) {
    if (!ctx || !ctx->platform[0] || !ctx->state_dir[0] || !ctx->db_path[0] ||
        !ctx->platform_root[0] || !out || max_count <= 0 || !out_count) {
        return -1;
    }
    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    jw_mem storefront;
    int is_dev = 0;
    int fetch_rc = jw__fetch_storefront(ctx, &storefront, &is_dev);
    if (fetch_rc != 0) {
        return fetch_rc;
    }

    jw_installed_release release;
    jw_pakrat_catalog_selection selections[128];
    int package_count = 0;
    int parse_rc = jw_pakrat_catalog_parse_and_select(
        storefront.data, ctx->platform,
        jw__installed_leaf_version(ctx, &release), is_dev,
        selections, (int)(sizeof(selections) / sizeof(selections[0])),
        &package_count);
    if (parse_rc != 0) {
        free(storefront.data);
        return parse_rc;
    }

    jw_pakrat_managed_apps managed;
    if (jw_pakrat_load_managed_apps(ctx->platform_root, &managed) != 0) {
        free(storefront.data);
        return -1;
    }
    int db_available = jw__path_exists(ctx->db_path);

    for (int i = 0; i < package_count && *out_count < max_count; i++) {
        jw_pakrat_app_state *state = &out[*out_count];
        state->package = selections[i].package;
        state->status = JW_PAKRAT_APP_AVAILABLE;
        state->primary_action_allowed = 1;
        jw__copy(state->action_version, sizeof(state->action_version),
                 state->package.version);
        jw__copy(state->gated_version, sizeof(state->gated_version),
                 selections[i].gated_version);
        jw__copy(state->gated_min_leaf_version,
                 sizeof(state->gated_min_leaf_version),
                 selections[i].gated_min_leaf_version);
        state->managed =
            jw_pakrat_managed_app_path_blocked(&managed,
                                               state->package.install_path) > 0;

        jw_pakrat_install install;
        int install_rc = db_available ?
            jw_db_pakrat_get_install(ctx->db_path, state->package.id,
                                     &install) :
            1;
        if (install_rc < 0) {
            free(storefront.data);
            return -1;
        }
        if (install_rc == 0) {
            state->installed_owned = 1;
            state->app_present = install.app_present;
            jw__copy(state->installed_version, sizeof(state->installed_version),
                     install.version);
            jw__copy(state->installed_at, sizeof(state->installed_at),
                     install.installed_at);
            jw__copy(state->app_name, sizeof(state->app_name), install.app_name);
            jw__copy(state->app_pak_dir, sizeof(state->app_pak_dir),
                     install.app_pak_dir);

            state->status = jw_pakrat_resolve_owned_state(
                state->package.version, install.version, install.app_present,
                &state->primary_action_allowed);

            int needs_exact =
                state->status == JW_PAKRAT_APP_STALE ||
                state->status == JW_PAKRAT_APP_INSTALLED;
            if (needs_exact &&
                strcmp(state->package.version, install.version) == 0) {
                state->primary_action_allowed = 1;
                jw__copy(state->action_version,
                         sizeof(state->action_version), install.version);
                state->action_uses_history =
                    state->status == JW_PAKRAT_APP_STALE;
            } else if (needs_exact) {
                int installed_parsed[3];
                jw_pakrat_catalog_package exact;
                int exact_rc =
                    jw_pak_version_parse(install.version,
                                         installed_parsed) == 0
                        ? jw_pakrat_catalog_find_exact(
                              storefront.data, ctx->platform,
                              state->package.id, install.version, &exact)
                        : 1;
                if (exact_rc < 0) {
                    free(storefront.data);
                    return exact_rc;
                }
                if (exact_rc == 0) {
                    state->primary_action_allowed = 1;
                    state->action_uses_history = 1;
                    jw__copy(state->action_version,
                             sizeof(state->action_version), install.version);
                } else {
                    state->primary_action_allowed = 0;
                    state->action_version[0] = '\0';
                    state->installed_version_missing_from_history = 1;
                }
            }
            jw__hide_obsolete_gate(state);
        } else {
            /* No ownership row: if the pak is already on disk it was installed
               manually (or by an older tool). Surface it as unmanaged so the UI
               can offer to adopt it rather than wrongly calling it "available". */
            char target[PATH_MAX];
            if (jw__install_target_path(ctx, state->package.install_path,
                                        target, sizeof(target)) == 0 &&
                jw__path_exists(target)) {
                state->status = JW_PAKRAT_APP_UNMANAGED;
                state->app_present = 1;
            }
        }

        (*out_count)++;
    }

    free(storefront.data);
    return package_count <= max_count ? 0 : -1;
}
