#include "internal/retroarch/catalog.h"
#include "internal/platform/platform_id.h"

#include "cJSON.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *jw_ra_platform_id(void) {
    return jw_platform_compiled_id();
}

static void jw_ra_set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", message ? message : "");
    }
}

static char *jw_ra_strdup(const char *value) {
    if (!value) {
        value = "";
    }
    size_t len = strlen(value);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len + 1u);
    return copy;
}

static char *jw_ra_json_string(cJSON *object, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return jw_ra_strdup("");
    }
    return jw_ra_strdup(item->valuestring);
}

static bool jw_ra_json_bool(cJSON *object, const char *key, bool fallback) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

static char *jw_ra_read_text_file(const char *path, size_t max_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || (size_t)size > max_size) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *text = (char *)malloc((size_t)size + 1u);
    if (!text) {
        fclose(fp);
        return NULL;
    }
    size_t read_count = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    if (read_count != (size_t)size) {
        free(text);
        return NULL;
    }
    text[read_count] = '\0';
    return text;
}

static int jw_ra_path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static void jw_ra_string_list_free(jw_ra_string_list *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static int jw_ra_string_list_load(cJSON *object, const char *key, jw_ra_string_list *out) {
    memset(out, 0, sizeof(*out));
    cJSON *array = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!array) {
        return 0;
    }
    if (!cJSON_IsArray(array)) {
        return -1;
    }

    int count = cJSON_GetArraySize(array);
    if (count <= 0) {
        return 0;
    }

    out->items = (char **)calloc((size_t)count, sizeof(char *));
    if (!out->items) {
        return -1;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || !item->valuestring) {
            jw_ra_string_list_free(out);
            return -1;
        }
        out->items[out->count] = jw_ra_strdup(item->valuestring);
        if (!out->items[out->count]) {
            jw_ra_string_list_free(out);
            return -1;
        }
        out->count++;
    }
    return 0;
}

bool jw_ra_string_list_contains(const jw_ra_string_list *list, const char *value) {
    if (!list || !value) {
        return false;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0) {
            return true;
        }
    }
    return false;
}

bool jw_ra_string_list_contains_casefold(const jw_ra_string_list *list, const char *value) {
    if (!list || !value) {
        return false;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcasecmp(list->items[i], value) == 0) {
            return true;
        }
    }
    return false;
}

static void jw_ra_core_free(jw_ra_core *core) {
    if (!core) {
        return;
    }
    free(core->id);
    free(core->display_name);
    free(core->type);
    free(core->libretro_name);
    free(core->file_name);
    free(core->config_folder);
    free(core->info_name);
    free(core->path);
    free(core->status);
}

static void jw_ra_system_free(jw_ra_system *system) {
    if (!system) {
        return;
    }
    free(system->id);
    free(system->name);
    jw_ra_string_list_free(&system->patterns);
    jw_ra_string_list_free(&system->extensions);
    jw_ra_string_list_free(&system->archive_extensions);
    jw_ra_string_list_free(&system->archive_inner_extensions);
    free(system->archive_mode);
    jw_ra_string_list_free(&system->file_names);
    jw_ra_string_list_free(&system->ignore_file_names);
    jw_ra_string_list_free(&system->playlist_extensions);
    free(system->m3u_generation);
    free(system->default_core);
    jw_ra_string_list_free(&system->alternate_cores);
    free(system->rom_root);
    free(system->image_root);
}

void jw_ra_catalog_free(jw_ra_catalog *catalog) {
    if (!catalog) {
        return;
    }
    for (size_t i = 0; i < catalog->core_count; i++) {
        jw_ra_core_free(&catalog->cores[i]);
    }
    for (size_t i = 0; i < catalog->system_count; i++) {
        jw_ra_system_free(&catalog->systems[i]);
    }
    free(catalog->cores);
    free(catalog->systems);
    free(catalog);
}

static int jw_ra_load_core(cJSON *row, jw_ra_core *out) {
    memset(out, 0, sizeof(*out));
    out->id = jw_ra_json_string(row, "id");
    out->display_name = jw_ra_json_string(row, "display_name");
    out->type = jw_ra_json_string(row, "type");
    out->libretro_name = jw_ra_json_string(row, "libretro_name");
    out->file_name = jw_ra_json_string(row, "file_name");
    out->config_folder = jw_ra_json_string(row, "config_folder");
    out->info_name = jw_ra_json_string(row, "info_name");
    out->path = jw_ra_json_string(row, "path");
    out->status = jw_ra_json_string(row, "status");
    out->supports_menu = jw_ra_json_bool(row, "supports_menu", false);
    out->supports_savestate = jw_ra_json_bool(row, "supports_savestate", false);
    out->supports_disk_control = jw_ra_json_bool(row, "supports_disk_control", false);
    out->needs_swap = jw_ra_json_bool(row, "needs_swap", false);

    if (!out->id || !out->type || !out->status || !out->id[0] || !out->type[0] || !out->status[0]) {
        return -1;
    }
    return 0;
}

static int jw_ra_load_system(cJSON *row, jw_ra_system *out) {
    memset(out, 0, sizeof(*out));
    out->id = jw_ra_json_string(row, "id");
    out->name = jw_ra_json_string(row, "name");
    out->archive_mode = jw_ra_json_string(row, "archive_mode");
    out->m3u_generation = jw_ra_json_string(row, "m3u_generation");
    out->default_core = jw_ra_json_string(row, "default_core");
    out->rom_root = jw_ra_json_string(row, "rom_root");
    out->image_root = jw_ra_json_string(row, "image_root");

    if (!out->id || !out->default_core || !out->id[0] || !out->default_core[0]) {
        return -1;
    }
    if (jw_ra_string_list_load(row, "patterns", &out->patterns) != 0 ||
        jw_ra_string_list_load(row, "extensions", &out->extensions) != 0 ||
        jw_ra_string_list_load(row, "archive_extensions", &out->archive_extensions) != 0 ||
        jw_ra_string_list_load(row, "archive_inner_extensions", &out->archive_inner_extensions) != 0 ||
        jw_ra_string_list_load(row, "file_names", &out->file_names) != 0 ||
        jw_ra_string_list_load(row, "ignore_file_names", &out->ignore_file_names) != 0 ||
        jw_ra_string_list_load(row, "playlist_extensions", &out->playlist_extensions) != 0 ||
        jw_ra_string_list_load(row, "alternate_cores", &out->alternate_cores) != 0) {
        return -1;
    }
    return 0;
}

static cJSON *jw_ra_parse_file(const char *path, char **text_out) {
    char *text = jw_ra_read_text_file(path, 512u * 1024u);
    if (!text) {
        return NULL;
    }
    cJSON *json = cJSON_Parse(text);
    if (!json) {
        free(text);
        return NULL;
    }
    *text_out = text;
    return json;
}

static int jw_ra_doc_has_expanded_cores(const char *path) {
    char *text = NULL;
    cJSON *json = jw_ra_parse_file(path, &text);
    if (!json) {
        return 0;
    }
    cJSON *cores = cJSON_GetObjectItemCaseSensitive(json, "cores");
    int expanded = cJSON_IsArray(cores);
    cJSON_Delete(json);
    free(text);
    return expanded;
}

static int jw_ra_validate_catalog(const jw_ra_catalog *catalog, char *error, size_t error_size);

jw_ra_catalog *jw_ra_catalog_load(const char *sdcard_root, char *error, size_t error_size) {
    jw_ra_set_error(error, error_size, "");
    if (!sdcard_root || !sdcard_root[0]) {
        jw_ra_set_error(error, error_size, "missing SD-card root");
        return NULL;
    }

    char defaults_dir[PATH_MAX];
    if (snprintf(defaults_dir, sizeof(defaults_dir), "%s/UMRK/%s/defaults",
                 sdcard_root, jw_ra_platform_id()) >= (int)sizeof(defaults_dir)) {
        jw_ra_set_error(error, error_size, "defaults path too long");
        return NULL;
    }

    char cores_path[PATH_MAX];
    char cores_v2_path[PATH_MAX];
    char systems_path[PATH_MAX];
    if (snprintf(cores_path, sizeof(cores_path), "%s/cores.json", defaults_dir) >=
            (int)sizeof(cores_path) ||
        snprintf(cores_v2_path, sizeof(cores_v2_path), "%s/cores.v2.json", defaults_dir) >=
            (int)sizeof(cores_v2_path) ||
        snprintf(systems_path, sizeof(systems_path), "%s/systems.json", defaults_dir) >=
            (int)sizeof(systems_path)) {
        jw_ra_set_error(error, error_size, "metadata path too long");
        return NULL;
    }

    const char *selected_cores_path = cores_path;
    if (!jw_ra_doc_has_expanded_cores(cores_path) && jw_ra_doc_has_expanded_cores(cores_v2_path)) {
        selected_cores_path = cores_v2_path;
    }

    char *cores_text = NULL;
    char *systems_text = NULL;
    cJSON *cores_doc = jw_ra_parse_file(selected_cores_path, &cores_text);
    cJSON *systems_doc = jw_ra_parse_file(systems_path, &systems_text);
    if (!cores_doc || !systems_doc) {
        cJSON_Delete(cores_doc);
        cJSON_Delete(systems_doc);
        free(cores_text);
        free(systems_text);
        jw_ra_set_error(error, error_size, "expanded RetroArch metadata missing or invalid");
        return NULL;
    }

    cJSON *cores = cJSON_GetObjectItemCaseSensitive(cores_doc, "cores");
    cJSON *systems = cJSON_GetObjectItemCaseSensitive(systems_doc, "systems");
    if (!cJSON_IsArray(cores) || !cJSON_IsArray(systems)) {
        cJSON_Delete(cores_doc);
        cJSON_Delete(systems_doc);
        free(cores_text);
        free(systems_text);
        jw_ra_set_error(error, error_size, "expanded RetroArch metadata has wrong shape");
        return NULL;
    }

    cJSON *cores_platform = cJSON_GetObjectItemCaseSensitive(cores_doc, "platform");
    cJSON *systems_platform = cJSON_GetObjectItemCaseSensitive(systems_doc, "platform");
    if (!cJSON_IsString(cores_platform) || !cores_platform->valuestring ||
        !cJSON_IsString(systems_platform) || !systems_platform->valuestring ||
        strcmp(cores_platform->valuestring, jw_ra_platform_id()) != 0 ||
        strcmp(systems_platform->valuestring, jw_ra_platform_id()) != 0) {
        cJSON_Delete(cores_doc);
        cJSON_Delete(systems_doc);
        free(cores_text);
        free(systems_text);
        jw_ra_set_error(error, error_size, "expanded RetroArch metadata platform mismatch");
        return NULL;
    }

    jw_ra_catalog *catalog = (jw_ra_catalog *)calloc(1, sizeof(jw_ra_catalog));
    if (!catalog) {
        cJSON_Delete(cores_doc);
        cJSON_Delete(systems_doc);
        free(cores_text);
        free(systems_text);
        jw_ra_set_error(error, error_size, "out of memory");
        return NULL;
    }

    int core_count = cJSON_GetArraySize(cores);
    int system_count = cJSON_GetArraySize(systems);
    catalog->cores = (jw_ra_core *)calloc((size_t)core_count, sizeof(jw_ra_core));
    catalog->systems = (jw_ra_system *)calloc((size_t)system_count, sizeof(jw_ra_system));
    if ((core_count > 0 && !catalog->cores) || (system_count > 0 && !catalog->systems)) {
        jw_ra_catalog_free(catalog);
        cJSON_Delete(cores_doc);
        cJSON_Delete(systems_doc);
        free(cores_text);
        free(systems_text);
        jw_ra_set_error(error, error_size, "out of memory");
        return NULL;
    }

    cJSON *row = NULL;
    cJSON_ArrayForEach(row, cores) {
        if (!cJSON_IsObject(row) || jw_ra_load_core(row, &catalog->cores[catalog->core_count]) != 0) {
            jw_ra_catalog_free(catalog);
            cJSON_Delete(cores_doc);
            cJSON_Delete(systems_doc);
            free(cores_text);
            free(systems_text);
            jw_ra_set_error(error, error_size, "invalid core row in RetroArch metadata");
            return NULL;
        }
        catalog->core_count++;
    }

    cJSON_ArrayForEach(row, systems) {
        if (!cJSON_IsObject(row) || jw_ra_load_system(row, &catalog->systems[catalog->system_count]) != 0) {
            jw_ra_catalog_free(catalog);
            cJSON_Delete(cores_doc);
            cJSON_Delete(systems_doc);
            free(cores_text);
            free(systems_text);
            jw_ra_set_error(error, error_size, "invalid system row in RetroArch metadata");
            return NULL;
        }
        catalog->system_count++;
    }

    cJSON_Delete(cores_doc);
    cJSON_Delete(systems_doc);
    free(cores_text);
    free(systems_text);
    if (jw_ra_validate_catalog(catalog, error, error_size) != 0) {
        jw_ra_catalog_free(catalog);
        return NULL;
    }
    return catalog;
}

const jw_ra_catalog *jw_ra_catalog_get(const char *sdcard_root, char *error, size_t error_size) {
    static jw_ra_catalog *cached_catalog = NULL;
    static char *cached_root = NULL;

    jw_ra_set_error(error, error_size, "");
    if (!sdcard_root || !sdcard_root[0]) {
        jw_ra_set_error(error, error_size, "missing SD-card root");
        return NULL;
    }

    if (cached_catalog && cached_root && strcmp(cached_root, sdcard_root) == 0) {
        return cached_catalog;
    }

    jw_ra_catalog *loaded = jw_ra_catalog_load(sdcard_root, error, error_size);
    if (!loaded) {
        return NULL;
    }

    char *root_copy = jw_ra_strdup(sdcard_root);
    if (!root_copy) {
        jw_ra_catalog_free(loaded);
        jw_ra_set_error(error, error_size, "out of memory");
        return NULL;
    }

    jw_ra_catalog_free(cached_catalog);
    free(cached_root);
    cached_catalog = loaded;
    cached_root = root_copy;
    return cached_catalog;
}

const jw_ra_system *jw_ra_catalog_find_system(const jw_ra_catalog *catalog, const char *system_id) {
    if (!catalog || !system_id || !system_id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < catalog->system_count; i++) {
        if (strcmp(catalog->systems[i].id, system_id) == 0) {
            return &catalog->systems[i];
        }
    }
    return NULL;
}

const jw_ra_system *jw_ra_catalog_match_system_folder(const jw_ra_catalog *catalog, const char *folder) {
    const jw_ra_system *system = jw_ra_catalog_find_system(catalog, folder);
    if (system) {
        return system;
    }
    if (!catalog || !folder || !folder[0]) {
        return NULL;
    }
    for (size_t i = 0; i < catalog->system_count; i++) {
        if (jw_ra_string_list_contains_casefold(&catalog->systems[i].patterns, folder)) {
            return &catalog->systems[i];
        }
    }
    return NULL;
}

static int jw_ra_validate_unique_cores(const jw_ra_catalog *catalog, char *error, size_t error_size) {
    for (size_t i = 0; i < catalog->core_count; i++) {
        for (size_t j = i + 1u; j < catalog->core_count; j++) {
            if (strcmp(catalog->cores[i].id, catalog->cores[j].id) == 0) {
                char message[256];
                snprintf(message, sizeof(message), "duplicate core id in RetroArch metadata: %s",
                         catalog->cores[i].id);
                jw_ra_set_error(error, error_size, message);
                return -1;
            }
        }
    }
    return 0;
}

static int jw_ra_validate_unique_systems(const jw_ra_catalog *catalog, char *error, size_t error_size) {
    for (size_t i = 0; i < catalog->system_count; i++) {
        for (size_t j = i + 1u; j < catalog->system_count; j++) {
            if (strcmp(catalog->systems[i].id, catalog->systems[j].id) == 0) {
                char message[256];
                snprintf(message, sizeof(message), "duplicate system id in RetroArch metadata: %s",
                         catalog->systems[i].id);
                jw_ra_set_error(error, error_size, message);
                return -1;
            }
        }
    }
    return 0;
}

static int jw_ra_validate_system_defaults(const jw_ra_catalog *catalog, char *error, size_t error_size) {
    for (size_t i = 0; i < catalog->system_count; i++) {
        const jw_ra_system *system = &catalog->systems[i];
        const jw_ra_core *core = jw_ra_catalog_find_core(catalog, system->default_core);
        if (!core) {
            char message[256];
            snprintf(message, sizeof(message), "%s default core missing from RetroArch metadata: %s",
                     system->id, system->default_core);
            jw_ra_set_error(error, error_size, message);
            return -1;
        }
        for (size_t j = 0; j < system->alternate_cores.count; j++) {
            const char *alternate_core = system->alternate_cores.items[j];
            if (!jw_ra_catalog_find_core(catalog, alternate_core)) {
                char message[256];
                snprintf(message, sizeof(message), "%s alternate core missing from RetroArch metadata: %s",
                         system->id, alternate_core);
                jw_ra_set_error(error, error_size, message);
                return -1;
            }
        }
    }
    return 0;
}

static int jw_ra_validate_catalog(const jw_ra_catalog *catalog, char *error, size_t error_size) {
    if (!catalog || catalog->core_count == 0 || catalog->system_count == 0) {
        jw_ra_set_error(error, error_size, "RetroArch metadata catalog is empty");
        return -1;
    }
    if (jw_ra_validate_unique_cores(catalog, error, error_size) != 0 ||
        jw_ra_validate_unique_systems(catalog, error, error_size) != 0 ||
        jw_ra_validate_system_defaults(catalog, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

const jw_ra_core *jw_ra_catalog_find_core(const jw_ra_catalog *catalog, const char *core_id) {
    if (!catalog || !core_id || !core_id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < catalog->core_count; i++) {
        if (strcmp(catalog->cores[i].id, core_id) == 0) {
            return &catalog->cores[i];
        }
    }
    return NULL;
}

bool jw_ra_core_is_packaged_retroarch(const jw_ra_core *core) {
    return core &&
           strcmp(core->type, "retroarch") == 0 &&
           strcmp(core->status, "packaged") == 0 &&
           core->file_name &&
           core->file_name[0];
}

static bool jw_ra_core_file_exists(const char *core_dir, const char *file_name) {
    char path[PATH_MAX];
    if (!core_dir || !file_name || !file_name[0]) {
        return false;
    }
    if (snprintf(path, sizeof(path), "%s/%s", core_dir, file_name) >= (int)sizeof(path)) {
        return false;
    }
    return jw_ra_path_exists(path) != 0;
}

static int jw_ra_copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    return snprintf(out, out_size, "%s", value) < (int)out_size ? 0 : -1;
}

int jw_ra_catalog_resolve_core_file(const jw_ra_catalog *catalog,
                                    const char *system_id,
                                    const char *core_dir,
                                    char *core_file,
                                    size_t core_file_size,
                                    char *core_id,
                                    size_t core_id_size,
                                    char *diagnostic,
                                    size_t diagnostic_size) {
    jw_ra_set_error(diagnostic, diagnostic_size, "");
    const jw_ra_system *system = jw_ra_catalog_find_system(catalog, system_id);
    if (!system) {
        system = jw_ra_catalog_match_system_folder(catalog, system_id);
    }
    if (!system) {
        jw_ra_set_error(diagnostic, diagnostic_size, "system missing from metadata");
        return -1;
    }

    const jw_ra_core *core = jw_ra_catalog_find_core(catalog, system->default_core);
    bool default_unavailable = true;
    if (jw_ra_core_is_packaged_retroarch(core) &&
        jw_ra_core_file_exists(core_dir, core->file_name)) {
        if (jw_ra_copy(core_file, core_file_size, core->file_name) != 0 ||
            jw_ra_copy(core_id, core_id_size, core->id) != 0) {
            jw_ra_set_error(diagnostic, diagnostic_size, "resolved core path too long");
            return -1;
        }
        return 0;
    }

    if (core && jw_ra_core_is_packaged_retroarch(core)) {
        jw_ra_set_error(diagnostic, diagnostic_size, "default core is packaged but missing on disk");
    } else if (core) {
        jw_ra_set_error(diagnostic, diagnostic_size, "default core is not a packaged RetroArch core");
    } else {
        jw_ra_set_error(diagnostic, diagnostic_size, "default core is missing from metadata");
    }

    for (size_t i = 0; i < system->alternate_cores.count; i++) {
        const jw_ra_core *alternate = jw_ra_catalog_find_core(catalog, system->alternate_cores.items[i]);
        if (!jw_ra_core_is_packaged_retroarch(alternate) ||
            !jw_ra_core_file_exists(core_dir, alternate->file_name)) {
            continue;
        }
        if (jw_ra_copy(core_file, core_file_size, alternate->file_name) != 0 ||
            jw_ra_copy(core_id, core_id_size, alternate->id) != 0) {
            jw_ra_set_error(diagnostic, diagnostic_size, "resolved alternate core path too long");
            return -1;
        }
        if (default_unavailable) {
            char message[256];
            snprintf(message, sizeof(message), "default core %s unavailable; using alternate %s",
                     system->default_core, alternate->id);
            jw_ra_set_error(diagnostic, diagnostic_size, message);
        }
        return 0;
    }

    return -1;
}
