/* realpath(), lstat(), readlink(), and strtok_r() need this under -std=c11. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/catalog/manifest.h"

#include "internal/retroarch/catalog.h"

#include "cJSON.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define JW_CONTENT_PATH_BUF (PATH_MAX + 1)

static void jw_content__reason(char *out, size_t out_size, const char *reason) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", reason);
    }
}

void jw_content_manifest_destroy(jw_content_manifest *manifest) {
    if (!manifest) {
        return;
    }
    cJSON_Delete(manifest->document);
    memset(manifest, 0, sizeof(*manifest));
}

static cJSON *jw_content__item(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static bool jw_content__object_keys(const cJSON *object,
                                    const char *const *known,
                                    size_t known_count) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, object) {
        if (!child->string) {
            return false;
        }
        bool found = false;
        for (size_t i = 0; i < known_count; i++) {
            if (strcmp(child->string, known[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        for (cJSON *other = child->next; other; other = other->next) {
            if (other->string && strcmp(child->string, other->string) == 0) {
                return false;
            }
        }
    }
    return true;
}

/* cJSON represents decoded strings as NUL-terminated buffers. Reject an
   escaped U+0000 before parsing so a field cannot be silently truncated. */
static bool jw_content__escaped_nul(const char *json) {
    bool in_string = false;
    for (size_t i = 0; json && json[i]; i++) {
        if (!in_string) {
            in_string = json[i] == '"';
            continue;
        }
        if (json[i] == '"') {
            in_string = false;
            continue;
        }
        if (json[i] != '\\') {
            continue;
        }
        char escaped = json[++i];
        if (!escaped) {
            break;
        }
        if (escaped == 'u' && json[i + 1] == '0' && json[i + 2] == '0' &&
            json[i + 3] == '0' && json[i + 4] == '0') {
            return true;
        }
        if (escaped == 'u' && json[i + 1] && json[i + 2] &&
            json[i + 3] && json[i + 4]) {
            i += 4;
        }
    }
    return false;
}

static bool jw_content__system_id(const char *s) {
    size_t len = s ? strlen(s) : 0;
    if (len < 2 || len > 32) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool jw_content__core_id(const char *s) {
    size_t len = s ? strlen(s) : 0;
    if (len < 2 || len > 64) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool jw_content__ascii_component(const char *s, size_t max_len,
                                        bool underscore) {
    size_t len = s ? strlen(s) : 0;
    if (len < 1 || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' ||
              (underscore && s[i] == '_'))) {
            return false;
        }
    }
    return true;
}

static bool jw_content__root(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return s && strncmp(s, prefix, n) == 0 &&
           jw_content__ascii_component(s + n, 32, true);
}

static bool jw_content__extension(const char *s, size_t max_len, bool underscore) {
    size_t len = s ? strlen(s) : 0;
    if (len < 1 || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') ||
              (underscore && s[i] == '_'))) {
            return false;
        }
    }
    return true;
}

static bool jw_content__string(const cJSON *item, size_t min_len, size_t max_len) {
    return cJSON_IsString(item) && item->valuestring &&
           strlen(item->valuestring) >= min_len && strlen(item->valuestring) <= max_len;
}

typedef bool (*jw_content_string_rule)(const char *);

static bool jw_content__string_list(const cJSON *array, int min_count, int max_count,
                                    size_t min_len, size_t max_len,
                                    jw_content_string_rule rule) {
    if (!cJSON_IsArray(array)) {
        return false;
    }
    int count = cJSON_GetArraySize(array);
    if (count < min_count || count > max_count) {
        return false;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!jw_content__string(item, min_len, max_len) ||
            (rule && !rule(item->valuestring))) {
            return false;
        }
    }
    return true;
}

static bool jw_content__rom_ext(const char *s) {
    return jw_content__extension(s, 16, true);
}

static bool jw_content__archive_ext(const char *s) {
    return jw_content__extension(s, 8, false);
}

static bool jw_content__path_absolute(const char *path) {
    return path && path[0] == '/';
}

static bool jw_content__path_traversal(const char *path) {
    const char *cursor = path;
    while (cursor && *cursor) {
        const char *slash = strchr(cursor, '/');
        size_t len = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (len == 2 && cursor[0] == '.' && cursor[1] == '.') {
            return true;
        }
        cursor = slash ? slash + 1 : NULL;
    }
    return false;
}

static bool jw_content__under(const char *candidate, const char *root) {
    size_t len = strlen(root);
    return strncmp(candidate, root, len) == 0 &&
           (candidate[len] == '\0' || candidate[len] == '/');
}

static bool jw_content__dirname(const char *rel, char *dir, size_t dir_size,
                                char *base, size_t base_size) {
    const char *slash = strrchr(rel, '/');
    if (!slash) {
        dir[0] = '\0';
        return snprintf(base, base_size, "%s", rel) < (int)base_size;
    }
    size_t len = (size_t)(slash - rel);
    if (len >= dir_size || snprintf(base, base_size, "%s", slash + 1) >=
                                    (int)base_size) {
        return false;
    }
    memcpy(dir, rel, len);
    dir[len] = '\0';
    return true;
}

static bool jw_content__lexical_join(char *base, size_t base_size, const char *rel) {
    char work[JW_CONTENT_PATH_BUF];
    if (snprintf(work, sizeof(work), "%s", rel) >= (int)sizeof(work)) {
        return false;
    }
    char *save = NULL;
    for (char *part = strtok_r(work, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || !part[0]) {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            size_t len = strlen(base);
            while (len > 1 && base[len - 1] != '/') {
                len--;
            }
            if (len > 1) {
                len--;
            }
            base[len] = '\0';
            continue;
        }
        size_t len = strlen(base);
        int written = snprintf(base + len, base_size - len, "%s%s",
                               (len && base[len - 1] == '/') ? "" : "/", part);
        if (written < 0 || (size_t)written >= base_size - len) {
            return false;
        }
    }
    return true;
}

typedef enum {
    JW_CONTENT_PATH_OK,
    JW_CONTENT_PATH_MISSING,
    JW_CONTENT_PATH_ESCAPES,
    JW_CONTENT_PATH_NON_REGULAR,
    JW_CONTENT_PATH_NOT_EXECUTABLE,
} jw_content_path_result;

/* Resolve every component. The explicit leaf-symlink branch preserves the
   useful `escaping-symlink` diagnosis even when the external target is
   dangling and realpath(3) therefore cannot resolve it. */
static jw_content_path_result jw_content__check_path(const char *pak_root,
                                                     const char *rel,
                                                     bool executable) {
    char root_real[JW_CONTENT_PATH_BUF];
    if (!realpath(pak_root, root_real)) {
        return JW_CONTENT_PATH_MISSING;
    }
    char rel_dir[JW_CONTENT_PATH_BUF], base[JW_CONTENT_PATH_BUF];
    if (!jw_content__dirname(rel, rel_dir, sizeof(rel_dir), base, sizeof(base))) {
        return JW_CONTENT_PATH_MISSING;
    }
    char dir_abs[JW_CONTENT_PATH_BUF];
    int n = snprintf(dir_abs, sizeof(dir_abs), "%s%s%s", root_real,
                     rel_dir[0] ? "/" : "", rel_dir);
    if (n < 0 || (size_t)n >= sizeof(dir_abs)) {
        return JW_CONTENT_PATH_MISSING;
    }
    char dir_real[JW_CONTENT_PATH_BUF];
    if (!realpath(dir_abs, dir_real)) {
        return JW_CONTENT_PATH_MISSING;
    }
    if (!jw_content__under(dir_real, root_real)) {
        return JW_CONTENT_PATH_ESCAPES;
    }
    char leaf[JW_CONTENT_PATH_BUF];
    if (snprintf(leaf, sizeof(leaf), "%s/%s", dir_real, base) >= (int)sizeof(leaf)) {
        return JW_CONTENT_PATH_MISSING;
    }
    struct stat lst;
    if (lstat(leaf, &lst) != 0) {
        return JW_CONTENT_PATH_MISSING;
    }
    char target[JW_CONTENT_PATH_BUF];
    if (S_ISLNK(lst.st_mode)) {
        char link[JW_CONTENT_PATH_BUF];
        ssize_t got = readlink(leaf, link, sizeof(link) - 1);
        if (got < 0 || (size_t)got == sizeof(link) - 1) {
            return JW_CONTENT_PATH_MISSING;
        }
        link[got] = '\0';
        if (link[0] == '/') {
            if (snprintf(target, sizeof(target), "%s", link) >= (int)sizeof(target)) {
                return JW_CONTENT_PATH_MISSING;
            }
        } else {
            if (snprintf(target, sizeof(target), "%s", dir_real) >= (int)sizeof(target) ||
                !jw_content__lexical_join(target, sizeof(target), link)) {
                return JW_CONTENT_PATH_MISSING;
            }
        }
        if (!jw_content__under(target, root_real)) {
            return JW_CONTENT_PATH_ESCAPES;
        }
    } else if (snprintf(target, sizeof(target), "%s", leaf) >= (int)sizeof(target)) {
        return JW_CONTENT_PATH_MISSING;
    }
    char resolved[JW_CONTENT_PATH_BUF];
    if (!realpath(target, resolved)) {
        return JW_CONTENT_PATH_MISSING;
    }
    if (!jw_content__under(resolved, root_real)) {
        return JW_CONTENT_PATH_ESCAPES;
    }
    struct stat st;
    if (stat(resolved, &st) != 0) {
        return JW_CONTENT_PATH_MISSING;
    }
    if (!S_ISREG(st.st_mode)) {
        return JW_CONTENT_PATH_NON_REGULAR;
    }
    if (executable && access(resolved, X_OK) != 0) {
        return JW_CONTENT_PATH_NOT_EXECUTABLE;
    }
    return JW_CONTENT_PATH_OK;
}

static const char *jw_content__path_reason(const cJSON *item, const char *pak_root,
                                           bool executable) {
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return "missing-file";
    }
    const char *path = item->valuestring;
    if (jw_content__path_absolute(path)) {
        return "absolute-path";
    }
    if (jw_content__path_traversal(path)) {
        return "path-traversal";
    }
    switch (jw_content__check_path(pak_root, path, executable)) {
    case JW_CONTENT_PATH_ESCAPES: return "escaping-symlink";
    case JW_CONTENT_PATH_MISSING: return "missing-file";
    case JW_CONTENT_PATH_NON_REGULAR: return "non-regular-file";
    case JW_CONTENT_PATH_NOT_EXECUTABLE: return "not-executable";
    case JW_CONTENT_PATH_OK: return NULL;
    }
    return "missing-file";
}

static const char *jw_content__forbidden(const cJSON *node) {
    if (cJSON_IsObject(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (child->string) {
                if (strcmp(child->string, "requires_direct_drm") == 0) {
                    return "forbidden-requires-direct-drm";
                }
                if (strcmp(child->string, "legacy_flat_core") == 0) {
                    return "forbidden-legacy-flat-core";
                }
                if (strcmp(child->string, "name_map") == 0) {
                    return "forbidden-name-map";
                }
                if (strcmp(child->string, "status") == 0) {
                    return "forbidden-status";
                }
            }
            const char *reason = jw_content__forbidden(child);
            if (reason) {
                return reason;
            }
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            const char *reason = jw_content__forbidden(child);
            if (reason) {
                return reason;
            }
        }
    }
    return NULL;
}

static const char *jw_content__validate_system(const cJSON *system,
                                               const char *pak_root,
                                               bool *warning) {
    static const char *const keys[] = {
        "id", "name", "patterns", "extensions", "archive_extensions",
        "archive_inner_extensions", "archive_mode", "file_names",
        "ignore_file_names", "playlist_extensions", "m3u_generation",
        "default_core", "alternate_cores", "rom_root", "image_root",
        "bios_notes", "icon_flat", "icon_photographic",
        "screenscraper_platform_ids", "group", "bios_directory",
        "requires_direct_drm", "legacy_flat_core", "name_map", "status",
    };
    if (!jw_content__object_keys(system, keys, sizeof(keys) / sizeof(keys[0]))) {
        return "unknown-field";
    }
    cJSON *id = jw_content__item(system, "id");
    if (!cJSON_IsString(id) || !jw_content__system_id(id->valuestring)) {
        return "malformed-system-id";
    }
    if (!jw_content__string(jw_content__item(system, "name"), 1, 64)) {
        return "malformed-system-name";
    }
    cJSON *patterns = jw_content__item(system, "patterns");
    if (!jw_content__string_list(patterns, 1, 32, 1, 64, NULL)) {
        return "malformed-patterns";
    }
    for (cJSON *a = patterns->child; a; a = a->next) {
        for (cJSON *b = a->next; b; b = b->next) {
            if (a->valuestring && b->valuestring &&
                strcasecmp(a->valuestring, b->valuestring) == 0) {
                *warning = true;
            }
        }
    }
    if (!jw_content__string_list(jw_content__item(system, "extensions"),
                                 1, 64, 1, 16, jw_content__rom_ext)) {
        return "malformed-extensions";
    }
    cJSON *item = jw_content__item(system, "archive_extensions");
    if (item && !jw_content__string_list(item, 0, 8, 1, 8, jw_content__archive_ext)) {
        return "malformed-archive-extensions";
    }
    item = jw_content__item(system, "archive_inner_extensions");
    if (item && !jw_content__string_list(item, 0, 64, 1, 16, jw_content__rom_ext)) {
        return "malformed-archive-inner-extensions";
    }
    item = jw_content__item(system, "playlist_extensions");
    if (item && !jw_content__string_list(item, 0, 8, 1, 8, jw_content__archive_ext)) {
        return "malformed-playlist-extensions";
    }
    item = jw_content__item(system, "file_names");
    if (item && !jw_content__string_list(item, 0, 32, 1, 128, NULL)) {
        return "malformed-file-names";
    }
    item = jw_content__item(system, "ignore_file_names");
    if (item && !jw_content__string_list(item, 0, 32, 1, 128, NULL)) {
        return "malformed-ignore-file-names";
    }
    item = jw_content__item(system, "bios_notes");
    if (item && !jw_content__string_list(item, 0, 8, 1, 256, NULL)) {
        return "malformed-bios-notes";
    }
    item = jw_content__item(system, "archive_mode");
    if (item && (!cJSON_IsString(item) ||
        (strcmp(item->valuestring, "pass_through") != 0 &&
         strcmp(item->valuestring, "extract") != 0 &&
         strcmp(item->valuestring, "none") != 0))) {
        return "unknown-archive-mode";
    }
    item = jw_content__item(system, "m3u_generation");
    if (item && (!cJSON_IsString(item) ||
        (strcmp(item->valuestring, "none") != 0 &&
         strcmp(item->valuestring, "auto") != 0 &&
         strcmp(item->valuestring, "manual") != 0))) {
        return "unknown-m3u-generation";
    }
    item = jw_content__item(system, "rom_root");
    if (!cJSON_IsString(item) || !jw_content__root(item->valuestring, "Roms/")) {
        return "malformed-rom-root";
    }
    item = jw_content__item(system, "image_root");
    if (!cJSON_IsString(item) || !jw_content__root(item->valuestring, "Images/")) {
        return "malformed-image-root";
    }
    item = jw_content__item(system, "default_core");
    if (!cJSON_IsString(item) || !jw_content__core_id(item->valuestring)) {
        return "malformed-core-id";
    }
    item = jw_content__item(system, "alternate_cores");
    if (item && !jw_content__string_list(item, 0, 16, 2, 64, jw_content__core_id)) {
        return "malformed-core-id";
    }
    const char *reason = jw_content__path_reason(
        jw_content__item(system, "icon_flat"), pak_root, false);
    if (reason) {
        return reason;
    }
    item = jw_content__item(system, "icon_photographic");
    if (item && !cJSON_IsNull(item)) {
        reason = jw_content__path_reason(item, pak_root, false);
        if (reason) {
            return reason;
        }
    }
    item = jw_content__item(system, "screenscraper_platform_ids");
    if (item) {
        if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) > 8) {
            return "malformed-screenscraper-platform-ids";
        }
        cJSON *value = NULL;
        cJSON_ArrayForEach(value, item) {
            if (!cJSON_IsNumber(value) || value->valuedouble != (double)value->valueint ||
                value->valueint < 1 || value->valueint > 99999) {
                return "malformed-screenscraper-platform-ids";
            }
        }
    }
    item = jw_content__item(system, "group");
    if (item && !cJSON_IsNull(item) && !jw_content__string(item, 1, 32)) {
        return "malformed-group";
    }
    item = jw_content__item(system, "bios_directory");
    if (item && !cJSON_IsNull(item) &&
        (!cJSON_IsString(item) || !jw_content__ascii_component(item->valuestring, 32, true))) {
        return "malformed-bios-directory";
    }
    return NULL;
}

static const char *jw_content__validate_core(const cJSON *core, const char *pak_root) {
    static const char *const keys[] = {
        "id", "display_name", "type", "libretro_name", "file_name",
        "info_name", "config_folder", "path", "supports_menu",
        "supports_savestate", "supports_disk_control", "needs_swap",
        "requires_direct_drm", "legacy_flat_core", "name_map", "status",
    };
    if (!jw_content__object_keys(core, keys, sizeof(keys) / sizeof(keys[0]))) {
        return "unknown-field";
    }
    cJSON *item = jw_content__item(core, "id");
    if (!cJSON_IsString(item) || !jw_content__core_id(item->valuestring)) {
        return "malformed-core-id";
    }
    if (!jw_content__string(jw_content__item(core, "display_name"), 1, 64)) {
        return "malformed-core-display-name";
    }
    static const char *const bools[] = {
        "supports_menu", "supports_savestate", "supports_disk_control", "needs_swap",
    };
    for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
        item = jw_content__item(core, bools[i]);
        if (item && !cJSON_IsBool(item)) {
            return "malformed-core-flag";
        }
    }
    cJSON *type = jw_content__item(core, "type");
    if (!cJSON_IsString(type) ||
        (strcmp(type->valuestring, "retroarch") != 0 && strcmp(type->valuestring, "path") != 0)) {
        return "unknown-core-type";
    }
    if (strcmp(type->valuestring, "retroarch") == 0) {
        if (jw_content__item(core, "path")) {
            return "core-type-field-mismatch";
        }
        item = jw_content__item(core, "libretro_name");
        if (!cJSON_IsString(item) || !jw_content__core_id(item->valuestring)) {
            return "malformed-libretro-name";
        }
        item = jw_content__item(core, "file_name");
        if (!item) {
            return "missing-core-file-name";
        }
        const char *reason = jw_content__path_reason(item, pak_root, false);
        if (reason) {
            return reason;
        }
        item = jw_content__item(core, "info_name");
        if (!item) {
            return "missing-core-info-name";
        }
        reason = jw_content__path_reason(item, pak_root, false);
        if (reason) {
            return reason;
        }
        item = jw_content__item(core, "config_folder");
        if (!item) {
            return "missing-config-folder";
        }
        if (!cJSON_IsString(item) || !jw_ra_core_folder_is_safe(item->valuestring)) {
            return "unsafe-config-folder";
        }
    } else {
        static const char *const retro_only[] = {
            "libretro_name", "file_name", "info_name", "config_folder",
        };
        for (size_t i = 0; i < sizeof(retro_only) / sizeof(retro_only[0]); i++) {
            if (jw_content__item(core, retro_only[i])) {
                return "core-type-field-mismatch";
            }
        }
        item = jw_content__item(core, "path");
        if (!item) {
            return "missing-core-path";
        }
        const char *reason = jw_content__path_reason(item, pak_root, true);
        if (reason) {
            return reason;
        }
    }
    return NULL;
}

static const char *jw_content__validate_extension(const cJSON *extension) {
    static const char *const keys[] = {"system_id", "add_alternate_cores"};
    if (!jw_content__object_keys(extension, keys, sizeof(keys) / sizeof(keys[0]))) {
        return "unknown-extension-field";
    }
    cJSON *item = jw_content__item(extension, "system_id");
    if (!cJSON_IsString(item) || !jw_content__system_id(item->valuestring)) {
        return "malformed-system-id";
    }
    item = jw_content__item(extension, "add_alternate_cores");
    if (cJSON_IsArray(item) && cJSON_GetArraySize(item) == 0) {
        return "empty-add-alternate-cores";
    }
    if (!jw_content__string_list(item, 1, 16, 2, 64, jw_content__core_id)) {
        return "malformed-core-id";
    }
    return NULL;
}

bool jw_content_manifest_validate(const char *pak_json_text,
                                  const char *pak_abs_path,
                                  jw_content_install_lane lane,
                                  const char *source_id,
                                  jw_content_manifest *out,
                                  char *reason,
                                  size_t reason_size) {
    static const char *const provides_keys[] = {
        "schema", "systems", "system_extensions", "cores",
    };
    if (!pak_json_text || !pak_abs_path || !source_id || !out) {
        jw_content__reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (lane == JW_CONTENT_SHARED_LANE) {
        jw_content__reason(reason, reason_size, "shared-content-unsupported");
        return false;
    }
    if (strcmp(source_id, "primary") != 0) {
        jw_content__reason(reason, reason_size, "secondary-source-unsupported");
        return false;
    }
    if (jw_content__escaped_nul(pak_json_text)) {
        jw_content__reason(reason, reason_size, "invalid-json");
        return false;
    }
    cJSON *document = cJSON_ParseWithOpts(pak_json_text, NULL, true);
    if (!cJSON_IsObject(document)) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, "invalid-json");
        return false;
    }
    cJSON *provides = jw_content__item(document, "provides");
    if (!jw_content__object_keys(provides, provides_keys,
                                 sizeof(provides_keys) / sizeof(provides_keys[0]))) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, "unknown-field");
        return false;
    }
    cJSON *schema = jw_content__item(provides, "schema");
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1.0) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, "unknown-schema");
        return false;
    }
    const char *failure = jw_content__forbidden(provides);
    if (failure) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, failure);
        return false;
    }
    cJSON *systems = jw_content__item(provides, "systems");
    cJSON *extensions = jw_content__item(provides, "system_extensions");
    cJSON *cores = jw_content__item(provides, "cores");
    if (systems && !cJSON_IsArray(systems)) {
        failure = "unknown-field";
    } else if (systems && cJSON_GetArraySize(systems) > 32) {
        failure = "too-many-systems";
    } else if (extensions && !cJSON_IsArray(extensions)) {
        failure = "unknown-field";
    } else if (extensions && cJSON_GetArraySize(extensions) > 32) {
        failure = "too-many-system-extensions";
    } else if (cores && !cJSON_IsArray(cores)) {
        failure = "unknown-field";
    } else if (cores && cJSON_GetArraySize(cores) > 32) {
        failure = "too-many-cores";
    } else if ((!systems || cJSON_GetArraySize(systems) == 0) &&
               (!extensions || cJSON_GetArraySize(extensions) == 0) &&
               (!cores || cJSON_GetArraySize(cores) == 0)) {
        failure = "empty-provides";
    }
    bool warning = false;
    if (!failure && systems) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, systems) {
            failure = jw_content__validate_system(item, pak_abs_path, &warning);
            if (failure) break;
        }
    }
    if (!failure && cores) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, cores) {
            failure = jw_content__validate_core(item, pak_abs_path);
            if (failure) break;
        }
    }
    if (!failure && extensions) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, extensions) {
            failure = jw_content__validate_extension(item);
            if (failure) break;
        }
    }
    if (failure) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, failure);
        return false;
    }
    out->document = document;
    out->provides = provides;
    out->redundant_case_variant = warning;
    return true;
}
