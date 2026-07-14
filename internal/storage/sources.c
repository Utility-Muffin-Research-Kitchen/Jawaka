#include "internal/storage/sources.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *jw__env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0]) ? value : NULL;
}

static bool jw__format(char *out, size_t out_size, const char *fmt, ...) {
    if (!out || out_size == 0 || !fmt) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(out, out_size, fmt, args);
    va_end(args);
    return needed >= 0 && (size_t)needed < out_size;
}

static void jw__realpath_or_literal(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!path || !path[0]) {
        return;
    }
    char resolved[JW_STORAGE_PATH_MAX];
    if (realpath(path, resolved)) {
        snprintf(out, out_size, "%s", resolved);
    } else {
        snprintf(out, out_size, "%s", path);
    }
}

static bool jw__path_is_within(const char *path, const char *root) {
    if (!path || !root || !path[0] || !root[0]) {
        return false;
    }
    size_t root_len = strlen(root);
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static void jw__source_id_for_root(int index, const char *root,
                                   char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const char *secondary_root = jw__env_value("UMRK_SECONDARY_SDCARD_PATH");
    if (!secondary_root) {
        secondary_root = jw__env_value("SECONDARY_SDCARD_PATH");
    }
    if (index == 0) {
        snprintf(out, out_size, "%s", "primary");
    } else if (root && secondary_root && strcmp(root, secondary_root) == 0) {
        snprintf(out, out_size, "%s", "secondary_sd");
    } else {
        snprintf(out, out_size, "source%d", index + 1);
    }
}

static int jw__split_nth(const char *list, int index, char *out, size_t out_size) {
    if (!list || index < 0 || !out || out_size == 0) {
        return -1;
    }
    const char *start = list;
    int current = 0;
    while (start && *start) {
        const char *end = strchr(start, ':');
        if (current == index) {
            size_t len = end ? (size_t)(end - start) : strlen(start);
            if (len == 0 || len >= out_size) {
                return -1;
            }
            memcpy(out, start, len);
            out[len] = '\0';
            return 0;
        }
        if (!end) {
            break;
        }
        start = end + 1;
        current++;
    }
    return -1;
}

static int jw__source_child_path(const char *plural_env,
                                 const char *singular_env,
                                 const char *root,
                                 const char *child,
                                 int index,
                                 char *out,
                                 size_t out_size) {
    const char *plural = jw__env_value(plural_env);
    if (plural && jw__split_nth(plural, index, out, out_size) == 0) {
        return 0;
    }
    if (index == 0) {
        const char *singular = jw__env_value(singular_env);
        if (singular) {
            return jw__format(out, out_size, "%s", singular) ? 0 : -1;
        }
    }
    return jw__format(out, out_size, "%s/%s", root, child) ? 0 : -1;
}

int jw_storage_sources_resolve(const char *primary_root, jw_storage_source_list *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    const char *root_list = jw__env_value("SDCARD_PATHS");
    char fallback[JW_STORAGE_PATH_MAX];
    if (!root_list) {
        const char *env_primary = jw__env_value("SDCARD_PATH");
        if (!env_primary) {
            env_primary = jw__env_value("JAWAKA_SDCARD_ROOT");
        }
        if (!env_primary) {
            env_primary = primary_root;
        }
        if (!env_primary || !env_primary[0]) {
            env_primary = "./mock-sdcard";
        }
        if (!jw__format(fallback, sizeof(fallback), "%s", env_primary)) {
            return -1;
        }
        root_list = fallback;
    }

    const char *cursor = root_list;
    while (cursor && *cursor && out->count < JW_STORAGE_MAX_SOURCES) {
        const char *end = strchr(cursor, ':');
        size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (len > 0) {
            jw_storage_source *source = &out->sources[out->count];
            if (len >= sizeof(source->root)) {
                return -1;
            }
            memcpy(source->root, cursor, len);
            source->root[len] = '\0';
            source->primary = out->count == 0;
            jw__source_id_for_root(out->count, source->root,
                                   source->id, sizeof(source->id));
            jw__realpath_or_literal(source->root, source->root_abs,
                                    sizeof(source->root_abs));

            int index = out->count;
            if (jw__source_child_path("ROMS_PATHS", "ROMS_PATH", source->root,
                                      "Roms", index, source->roms_path,
                                      sizeof(source->roms_path)) != 0 ||
                jw__source_child_path("IMAGES_PATHS", "IMAGES_PATH", source->root,
                                      "Images", index, source->images_path,
                                      sizeof(source->images_path)) != 0 ||
                jw__source_child_path("MUSIC_PATHS", "MUSIC_PATH", source->root,
                                      "Music", index, source->music_path,
                                      sizeof(source->music_path)) != 0 ||
                jw__source_child_path("APPS_PATHS", "APPS_PATH", source->root,
                                      "Apps", index, source->apps_path,
                                      sizeof(source->apps_path)) != 0 ||
                jw__source_child_path("BIOS_PATHS", "BIOS_PATH", source->root,
                                      "BIOS", index, source->bios_path,
                                      sizeof(source->bios_path)) != 0 ||
                jw__source_child_path("SAVES_PATHS", "SAVES_PATH", source->root,
                                      "Saves", index, source->saves_path,
                                      sizeof(source->saves_path)) != 0 ||
                jw__source_child_path("STATES_PATHS", "STATES_PATH", source->root,
                                      "States", index, source->states_path,
                                      sizeof(source->states_path)) != 0 ||
                jw__source_child_path("CHEATS_PATHS", "CHEATS_PATH", source->root,
                                      "Cheats", index, source->cheats_path,
                                      sizeof(source->cheats_path)) != 0) {
                return -1;
            }
            out->count++;
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }

    return out->count > 0 ? 0 : -1;
}

const jw_storage_source *jw_storage_sources_primary(const jw_storage_source_list *list) {
    return (list && list->count > 0) ? &list->sources[0] : NULL;
}

const jw_storage_source *jw_storage_sources_find_by_id(const jw_storage_source_list *list,
                                                       const char *id) {
    if (!list || !id || !id[0]) {
        return NULL;
    }
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->sources[i].id, id) == 0) {
            return &list->sources[i];
        }
    }
    return NULL;
}

const jw_storage_source *jw_storage_sources_find_for_path(const jw_storage_source_list *list,
                                                          const char *path) {
    if (!list || list->count <= 0 || !path || !path[0]) {
        return NULL;
    }
    if (path[0] != '/') {
        return jw_storage_sources_primary(list);
    }

    char resolved[JW_STORAGE_PATH_MAX];
    jw__realpath_or_literal(path, resolved, sizeof(resolved));

    const jw_storage_source *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < list->count; i++) {
        const jw_storage_source *source = &list->sources[i];
        const char *root = source->root_abs[0] ? source->root_abs : source->root;
        size_t len = strlen(root);
        if (len > best_len && jw__path_is_within(resolved, root)) {
            best = source;
            best_len = len;
        }
    }
    return best ? best : jw_storage_sources_primary(list);
}

int jw_storage_resolve_path(const jw_storage_source_list *list, const char *path,
                            char *out, size_t out_size) {
    if (!list || list->count <= 0 || !path || !path[0] || !out || out_size == 0) {
        return -1;
    }
    int needed = 0;
    if (path[0] == '/') {
        needed = snprintf(out, out_size, "%s", path);
    } else {
        const jw_storage_source *primary = jw_storage_sources_primary(list);
        needed = snprintf(out, out_size, "%s/%s", primary->root, path);
    }
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

int jw_storage_db_path_for_source(const jw_storage_source *source,
                                  const char *relative_path,
                                  const char *absolute_path,
                                  char *out, size_t out_size) {
    if (!source || !out || out_size == 0) {
        return -1;
    }
    const char *value = source->primary ? relative_path : absolute_path;
    if (!value || !value[0]) {
        return -1;
    }
    char temp[JW_STORAGE_PATH_MAX];
    int needed = snprintf(temp, sizeof(temp), "%s", value);
    if (needed < 0 || (size_t)needed >= sizeof(temp)) {
        return -1;
    }
    needed = snprintf(out, out_size, "%s", temp);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}
