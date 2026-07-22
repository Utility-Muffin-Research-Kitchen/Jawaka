#include "internal/store/pakrat_catalog.h"

#include "cJSON.h"
#include "internal/platform/leaf_version.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define JW_PAKRAT_MAX_VERSIONS_PER_PACKAGE 16

typedef struct {
    jw_pakrat_catalog_package versions[JW_PAKRAT_MAX_VERSIONS_PER_PACKAGE];
    int count;
} jw__package_history;

static const char *jw__json_string(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static long long jw__json_positive_integer(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0 ||
        item->valuedouble > (double)LLONG_MAX ||
        item->valuedouble != (double)(long long)item->valuedouble) {
        return 0;
    }
    return (long long)item->valuedouble;
}

static int jw__copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int needed = snprintf(out, out_size, "%s", value);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

static int jw__package_valid(const jw_pakrat_catalog_package *pkg) {
    int parsed[3];
    return pkg && pkg->id[0] && pkg->name[0] && pkg->version[0] &&
           jw_pak_version_parse(pkg->version, parsed) == 0 &&
           (!pkg->min_leaf_version[0] ||
            jw_pak_version_parse(pkg->min_leaf_version, parsed) == 0) &&
           pkg->platform[0] && pkg->install_name[0] &&
           pkg->install_path[0] && pkg->runtime_manifest_path[0] &&
           pkg->artifact_url[0] && pkg->artifact_name[0] &&
           strcmp(pkg->artifact_archive, "zip") == 0 &&
           pkg->artifact_sha256[0] && pkg->artifact_size > 0 &&
           pkg->artifact_installed_size > 0;
}

static int jw__fill_package(const cJSON *app, const cJSON *package,
                            const cJSON *version,
                            jw_pakrat_catalog_package *out) {
    const cJSON *artifact =
        cJSON_GetObjectItemCaseSensitive(version, "artifact");
    if (!cJSON_IsObject(artifact)) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (jw__copy(out->id, sizeof(out->id),
                 jw__json_string(app, "id")) != 0 ||
        jw__copy(out->name, sizeof(out->name),
                 jw__json_string(app, "name")) != 0 ||
        jw__copy(out->summary, sizeof(out->summary),
                 jw__json_string(app, "summary")) != 0 ||
        jw__copy(out->platform, sizeof(out->platform),
                 jw__json_string(package, "platform")) != 0 ||
        jw__copy(out->version, sizeof(out->version),
                 jw__json_string(version, "version")) != 0 ||
        jw__copy(out->min_leaf_version, sizeof(out->min_leaf_version),
                 jw__json_string(version, "min_leaf_version")) != 0 ||
        jw__copy(out->install_name, sizeof(out->install_name),
                 jw__json_string(package, "install_name")) != 0 ||
        jw__copy(out->runtime_manifest_path,
                 sizeof(out->runtime_manifest_path),
                 jw__json_string(package, "runtime_manifest_path")) != 0 ||
        jw__copy(out->artifact_url, sizeof(out->artifact_url),
                 jw__json_string(artifact, "url")) != 0 ||
        jw__copy(out->artifact_name, sizeof(out->artifact_name),
                 jw__json_string(artifact, "name")) != 0 ||
        jw__copy(out->artifact_archive, sizeof(out->artifact_archive),
                 jw__json_string(artifact, "archive")) != 0 ||
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
    out->artifact_size = jw__json_positive_integer(artifact, "size");
    out->artifact_installed_size =
        jw__json_positive_integer(artifact, "installed_size");
    return jw__package_valid(out) ? 0 : -1;
}

static int jw__artifact_equal(const jw_pakrat_catalog_package *left,
                              const jw_pakrat_catalog_package *right) {
    return left && right &&
           strcmp(left->artifact_url, right->artifact_url) == 0 &&
           strcmp(left->artifact_name, right->artifact_name) == 0 &&
           strcmp(left->artifact_archive, right->artifact_archive) == 0 &&
           strcmp(left->artifact_sha256, right->artifact_sha256) == 0 &&
           left->artifact_size == right->artifact_size &&
           left->artifact_installed_size == right->artifact_installed_size;
}

static int jw__version_cmp_text(const char *left, const char *right) {
    int left_version[3];
    int right_version[3];
    if (jw_pak_version_parse(left, left_version) != 0 ||
        jw_pak_version_parse(right, right_version) != 0) {
        return 0;
    }
    return jw_version_cmp(left_version, right_version);
}

static int jw__gate_passes(const jw_pakrat_catalog_package *version,
                           const int device_version[3],
                           int device_known,
                           int is_dev_override) {
    if (!version->min_leaf_version[0]) {
        return 1;
    }
    if (!device_known) {
        return is_dev_override != 0;
    }
    int minimum[3];
    return jw_pak_version_parse(version->min_leaf_version, minimum) == 0 &&
           jw_version_cmp(device_version, minimum) >= 0;
}

static int jw__parse_package_history(const cJSON *app, const cJSON *package,
                                     jw__package_history *out) {
    jw_pakrat_catalog_package legacy;
    if (!out ||
        jw__fill_package(app, package, package, &legacy) != 0 ||
        legacy.min_leaf_version[0] ||
        strcmp(jw__json_string(app, "version"), legacy.version) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    const cJSON *versions =
        cJSON_GetObjectItemCaseSensitive(package, "versions");
    if (!versions) {
        out->versions[0] = legacy;
        out->count = 1;
        return 0;
    }
    if (!cJSON_IsArray(versions)) {
        return -1;
    }

    int version_count = cJSON_GetArraySize(versions);
    if (version_count <= 0 ||
        version_count > JW_PAKRAT_MAX_VERSIONS_PER_PACKAGE) {
        return -1;
    }

    int legacy_index = -1;
    int safe_floor_index = -1;
    for (int i = 0; i < version_count; i++) {
        const cJSON *entry = cJSON_GetArrayItem(versions, i);
        if (!cJSON_IsObject(entry) ||
            jw__fill_package(app, package, entry, &out->versions[i]) != 0) {
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(out->versions[i].version,
                       out->versions[j].version) == 0) {
                return -1;
            }
        }
        if (!out->versions[i].min_leaf_version[0] &&
            (safe_floor_index < 0 ||
             jw__version_cmp_text(out->versions[i].version,
                                  out->versions[safe_floor_index].version) > 0)) {
            safe_floor_index = i;
        }
        if (strcmp(out->versions[i].version, legacy.version) == 0) {
            legacy_index = i;
        }
    }

    if (safe_floor_index < 0 || legacy_index < 0 ||
        legacy_index != safe_floor_index ||
        out->versions[legacy_index].min_leaf_version[0] ||
        !jw__artifact_equal(&legacy, &out->versions[legacy_index])) {
        return -1;
    }
    out->count = version_count;
    return 0;
}

static int jw__select_package(const cJSON *app, const cJSON *package,
                              const int device_version[3],
                              int device_known, int is_dev_override,
                              jw_pakrat_catalog_selection *out) {
    jw__package_history history;
    if (jw__parse_package_history(app, package, &history) != 0) {
        return -1;
    }
    int selected_index = -1;
    int gated_index = -1;
    for (int i = 0; i < history.count; i++) {
        if (jw__gate_passes(&history.versions[i], device_version, device_known,
                            is_dev_override)) {
            if (selected_index < 0 ||
                jw__version_cmp_text(history.versions[i].version,
                                     history.versions[selected_index].version) > 0) {
                selected_index = i;
            }
        } else if (gated_index < 0 ||
                   jw__version_cmp_text(history.versions[i].version,
                                        history.versions[gated_index].version) > 0) {
            gated_index = i;
        }
    }
    if (selected_index < 0) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->package = history.versions[selected_index];
    if (gated_index >= 0 &&
        jw__version_cmp_text(history.versions[gated_index].version,
                             history.versions[selected_index].version) > 0) {
        if (jw__copy(out->gated_version, sizeof(out->gated_version),
                     history.versions[gated_index].version) != 0 ||
            jw__copy(out->gated_min_leaf_version,
                     sizeof(out->gated_min_leaf_version),
                     history.versions[gated_index].min_leaf_version) != 0) {
            return -1;
        }
    }
    return 0;
}

static int jw__catalog_root_valid(const cJSON *root) {
    if (!cJSON_IsObject(root)) {
        return -1;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (schema) {
        if (!cJSON_IsNumber(schema) ||
            schema->valuedouble != (double)schema->valueint ||
            schema->valueint < 1) {
            return -1;
        }
        if (schema->valueint > JW_PAKRAT_CATALOG_SCHEMA_MAX) {
            return JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF;
        }
    }
    return strcmp(jw__json_string(root, "product"), "pak-rat") == 0
               ? 0
               : -1;
}

int jw_pakrat_catalog_parse_and_select(
    const char *json,
    const char *platform,
    const char *device_leaf_version,
    int is_dev_override,
    jw_pakrat_catalog_selection *out,
    int max_count,
    int *out_count) {
    if (!json || !platform || !platform[0] || !out || max_count <= 0 ||
        !out_count) {
        return -1;
    }
    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    cJSON *root = cJSON_Parse(json);
    int root_rc = jw__catalog_root_valid(root);
    if (root_rc != 0) {
        cJSON_Delete(root);
        return root_rc;
    }

    int device_version[3] = {0, 0, 0};
    int device_known =
        jw_leaf_release_version_parse(device_leaf_version,
                                      device_version) == 0;
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
        const cJSON *packages =
            cJSON_GetObjectItemCaseSensitive(app, "packages");
        if (!cJSON_IsArray(packages)) {
            cJSON_Delete(root);
            return -1;
        }
        const cJSON *package = NULL;
        cJSON_ArrayForEach(package, packages) {
            if (!cJSON_IsObject(package) ||
                strcmp(jw__json_string(package, "platform"), platform) != 0) {
                continue;
            }
            if (*out_count >= max_count ||
                jw__select_package(app, package, device_version, device_known,
                                   is_dev_override, &out[*out_count]) != 0) {
                cJSON_Delete(root);
                return -1;
            }
            (*out_count)++;
        }
    }

    cJSON_Delete(root);
    return 0;
}

int jw_pakrat_catalog_find_exact(
    const char *json,
    const char *platform,
    const char *store_id,
    const char *requested_version,
    jw_pakrat_catalog_package *out) {
    int parsed_version[3];
    if (!json || !platform || !platform[0] || !store_id || !store_id[0] ||
        !requested_version || jw_pak_version_parse(requested_version,
                                                   parsed_version) != 0 ||
        !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    int root_rc = jw__catalog_root_valid(root);
    if (root_rc != 0) {
        cJSON_Delete(root);
        return root_rc;
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
        if (strcmp(jw__json_string(app, "id"), store_id) != 0) {
            continue;
        }
        const cJSON *packages =
            cJSON_GetObjectItemCaseSensitive(app, "packages");
        if (!cJSON_IsArray(packages)) {
            cJSON_Delete(root);
            return -1;
        }
        const cJSON *package = NULL;
        cJSON_ArrayForEach(package, packages) {
            if (!cJSON_IsObject(package) ||
                strcmp(jw__json_string(package, "platform"), platform) != 0) {
                continue;
            }
            jw__package_history history;
            if (jw__parse_package_history(app, package, &history) != 0) {
                cJSON_Delete(root);
                return -1;
            }
            for (int i = 0; i < history.count; i++) {
                if (strcmp(history.versions[i].version,
                           requested_version) == 0) {
                    *out = history.versions[i];
                    cJSON_Delete(root);
                    return 0;
                }
            }
        }
    }

    cJSON_Delete(root);
    return 1;
}
