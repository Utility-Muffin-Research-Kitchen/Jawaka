#include "internal/store/pakrat_state.h"

#include "cJSON.h"
#include "internal/db/db.h"
#include "internal/store/catalog_source.h"
#include "internal/store/managed_apps.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char *data;
    size_t size;
} jw_mem;

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

static const char *jw__json_string(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static long long jw__json_ll(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (long long)item->valuedouble : 0;
}

static size_t jw__curl_mem_write(void *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
    jw_mem *mem = (jw_mem *)userdata;
    size_t bytes = size * nmemb;
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

static int jw__fetch_url(const char *url, jw_mem *out) {
    if (!url || !out || jw__ensure_curl() != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jw__curl_mem_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jawaka-pakrat/1");
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || (http != 0 && http >= 400)) {
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
    if (jw__fetch_url(storefront_url, out) != 0) {
        fprintf(stderr, "failed to fetch %s\n", storefront_url);
        return -1;
    }
    return 0;
}

static int jw__package_valid(const jw_pakrat_catalog_package *pkg) {
    return pkg && pkg->id[0] && pkg->name[0] && pkg->version[0] &&
           pkg->platform[0] && pkg->install_name[0] && pkg->install_path[0] &&
           pkg->artifact_url[0] && pkg->artifact_name[0] &&
           pkg->artifact_sha256[0] && pkg->artifact_size > 0;
}

static int jw__fill_package(const cJSON *app, const cJSON *pkg,
                            jw_pakrat_catalog_package *out) {
    const cJSON *artifact = cJSON_GetObjectItemCaseSensitive(pkg, "artifact");
    if (!cJSON_IsObject(artifact)) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (jw__copy(out->id, sizeof(out->id), jw__json_string(app, "id")) != 0 ||
        jw__copy(out->name, sizeof(out->name), jw__json_string(app, "name")) != 0 ||
        jw__copy(out->summary, sizeof(out->summary), jw__json_string(app, "summary")) != 0 ||
        jw__copy(out->platform, sizeof(out->platform), jw__json_string(pkg, "platform")) != 0 ||
        jw__copy(out->version, sizeof(out->version), jw__json_string(pkg, "version")) != 0 ||
        jw__copy(out->install_name, sizeof(out->install_name),
                 jw__json_string(pkg, "install_name")) != 0 ||
        jw__copy(out->runtime_manifest_path, sizeof(out->runtime_manifest_path),
                 jw__json_string(pkg, "runtime_manifest_path")) != 0 ||
        jw__copy(out->artifact_url, sizeof(out->artifact_url),
                 jw__json_string(artifact, "url")) != 0 ||
        jw__copy(out->artifact_name, sizeof(out->artifact_name),
                 jw__json_string(artifact, "name")) != 0 ||
        jw__copy(out->artifact_sha256, sizeof(out->artifact_sha256),
                 jw__json_string(artifact, "sha256")) != 0) {
        return -1;
    }
    if (!out->runtime_manifest_path[0] &&
        jw__copy(out->runtime_manifest_path,
                 sizeof(out->runtime_manifest_path), "pak.json") != 0) {
        return -1;
    }
    if (snprintf(out->install_path, sizeof(out->install_path), "%s/%s",
                 out->platform, out->install_name) >=
        (int)sizeof(out->install_path)) {
        return -1;
    }
    out->artifact_size = jw__json_ll(artifact, "size");
    return jw__package_valid(out) ? 0 : -1;
}

static int jw__parse_storefront_packages(const char *json, const char *platform,
                                         jw_pakrat_catalog_package *out,
                                         int max_count, int *out_count) {
    if (!json || !platform || !platform[0] || !out || max_count <= 0 ||
        !out_count) {
        return -1;
    }
    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return -1;
    }
    if (strcmp(jw__json_string(root, "product"), "pak-rat") != 0) {
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (!cJSON_IsArray(apps)) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *app = NULL;
    cJSON_ArrayForEach(app, apps) {
        if (!cJSON_IsObject(app)) {
            cJSON_Delete(root);
            return -1;
        }
        const cJSON *packages = cJSON_GetObjectItemCaseSensitive(app, "packages");
        if (!cJSON_IsArray(packages)) {
            cJSON_Delete(root);
            return -1;
        }
        const cJSON *pkg = NULL;
        cJSON_ArrayForEach(pkg, packages) {
            if (!cJSON_IsObject(pkg) ||
                strcmp(jw__json_string(pkg, "platform"), platform) != 0) {
                continue;
            }
            if (*out_count >= max_count) {
                cJSON_Delete(root);
                return -1;
            }
            if (jw__fill_package(app, pkg, &out[*out_count]) != 0) {
                cJSON_Delete(root);
                return -1;
            }
            (*out_count)++;
        }
    }

    cJSON_Delete(root);
    return 0;
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

    jw_pakrat_catalog_package packages[128];
    int count = 0;
    int parse_rc = jw__parse_storefront_packages(storefront.data, ctx->platform,
                                                 packages,
                                                 (int)(sizeof(packages) /
                                                       sizeof(packages[0])),
                                                 &count);
    free(storefront.data);
    if (parse_rc != 0) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (strcmp(packages[i].id, store_id) == 0) {
            *out = packages[i];
            if (out_is_dev_override) {
                *out_is_dev_override = is_dev;
            }
            return 0;
        }
    }
    return 1;
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
    int fetch_rc = jw__fetch_storefront(ctx, &storefront, NULL);
    if (fetch_rc != 0) {
        return fetch_rc;
    }

    jw_pakrat_catalog_package packages[128];
    int package_count = 0;
    int parse_rc = jw__parse_storefront_packages(storefront.data, ctx->platform,
                                                 packages,
                                                 (int)(sizeof(packages) /
                                                       sizeof(packages[0])),
                                                 &package_count);
    free(storefront.data);
    if (parse_rc != 0) {
        return -1;
    }

    jw_pakrat_managed_apps managed;
    if (jw_pakrat_load_managed_apps(ctx->platform_root, &managed) != 0) {
        return -1;
    }
    int db_available = jw__path_exists(ctx->db_path);

    for (int i = 0; i < package_count && *out_count < max_count; i++) {
        jw_pakrat_app_state *state = &out[*out_count];
        state->package = packages[i];
        state->status = JW_PAKRAT_APP_AVAILABLE;
        state->managed =
            jw_pakrat_managed_app_path_blocked(&managed,
                                               state->package.install_path) > 0;

        jw_pakrat_install install;
        int install_rc = db_available ?
            jw_db_pakrat_get_install(ctx->db_path, state->package.id,
                                     &install) :
            1;
        if (install_rc < 0) {
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

            if (!install.app_present) {
                state->status = JW_PAKRAT_APP_STALE;
            } else if (strcmp(install.version, state->package.version) != 0) {
                state->status = JW_PAKRAT_APP_UPDATE_AVAILABLE;
            } else {
                state->status = JW_PAKRAT_APP_INSTALLED;
            }
        }

        (*out_count)++;
    }

    return package_count <= max_count ? 0 : -1;
}
