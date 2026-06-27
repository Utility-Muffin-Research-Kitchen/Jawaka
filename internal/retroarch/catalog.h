#ifndef JW_RETROARCH_CATALOG_H
#define JW_RETROARCH_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char **items;
    size_t count;
} jw_ra_string_list;

typedef struct {
    char *id;
    char *display_name;
    char *type;
    char *libretro_name;
    char *file_name;
    char *config_folder;
    char *info_name;
    char *path;
    bool supports_menu;
    bool supports_savestate;
    bool supports_disk_control;
    bool needs_swap;
    char *status;
} jw_ra_core;

typedef struct {
    char *id;
    char *name;
    jw_ra_string_list patterns;
    jw_ra_string_list extensions;
    jw_ra_string_list archive_extensions;
    jw_ra_string_list archive_inner_extensions;
    char *archive_mode;
    jw_ra_string_list file_names;
    jw_ra_string_list ignore_file_names;
    jw_ra_string_list playlist_extensions;
    char *m3u_generation;
    char *default_core;
    jw_ra_string_list alternate_cores;
    char *rom_root;
    char *image_root;
} jw_ra_system;

typedef struct {
    jw_ra_core *cores;
    size_t core_count;
    jw_ra_system *systems;
    size_t system_count;
} jw_ra_catalog;

typedef struct {
    char id[64];
    char display_name[128];
    char type[32];
    char file_name[256];
    char config_folder[128];
    char path[256];
    bool supports_menu;
    bool supports_savestate;
    bool supports_disk_control;
    bool needs_swap;
    bool is_default;
} jw_ra_core_choice;

jw_ra_catalog *jw_ra_catalog_load(const char *sdcard_root, char *error, size_t error_size);
const jw_ra_catalog *jw_ra_catalog_get(const char *sdcard_root, char *error, size_t error_size);
void jw_ra_catalog_free(jw_ra_catalog *catalog);

const jw_ra_system *jw_ra_catalog_find_system(const jw_ra_catalog *catalog, const char *system_id);
const jw_ra_system *jw_ra_catalog_match_system_folder(const jw_ra_catalog *catalog, const char *folder);
const jw_ra_core *jw_ra_catalog_find_core(const jw_ra_catalog *catalog, const char *core_id);

bool jw_ra_string_list_contains(const jw_ra_string_list *list, const char *value);
bool jw_ra_string_list_contains_casefold(const jw_ra_string_list *list, const char *value);
bool jw_ra_core_is_packaged_retroarch(const jw_ra_core *core);

int jw_ra_catalog_list_system_cores(const jw_ra_catalog *catalog,
                                    const char *system_id,
                                    const char *core_dir,
                                    const char *platform_dir,
                                    jw_ra_core_choice *out,
                                    size_t max_count,
                                    size_t *out_count);

int jw_ra_catalog_resolve_core_file(const jw_ra_catalog *catalog,
                                    const char *system_id,
                                    const char *core_dir,
                                    char *core_file,
                                    size_t core_file_size,
                                    char *core_id,
                                    size_t core_id_size,
                                    char *diagnostic,
                                    size_t diagnostic_size);

int jw_ra_catalog_resolve_core_file_for_choice(const jw_ra_catalog *catalog,
                                               const char *system_id,
                                               const char *preferred_core_id,
                                               const char *core_dir,
                                               char *core_file,
                                               size_t core_file_size,
                                               char *core_id,
                                               size_t core_id_size,
                                               char *diagnostic,
                                               size_t diagnostic_size);

#endif
