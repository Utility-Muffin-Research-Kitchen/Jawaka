#ifndef JW_STORAGE_SOURCES_H
#define JW_STORAGE_SOURCES_H

#include <stdbool.h>
#include <stddef.h>

#ifndef PATH_MAX
#define JW_STORAGE_PATH_MAX 4096
#else
#define JW_STORAGE_PATH_MAX PATH_MAX
#endif

#define JW_STORAGE_MAX_SOURCES 8
#define JW_STORAGE_SOURCE_ID_MAX 32

typedef struct {
    char id[JW_STORAGE_SOURCE_ID_MAX];
    char root[JW_STORAGE_PATH_MAX];
    char root_abs[JW_STORAGE_PATH_MAX];
    char roms_path[JW_STORAGE_PATH_MAX];
    char images_path[JW_STORAGE_PATH_MAX];
    char music_path[JW_STORAGE_PATH_MAX];
    char apps_path[JW_STORAGE_PATH_MAX];
    char bios_path[JW_STORAGE_PATH_MAX];
    char saves_path[JW_STORAGE_PATH_MAX];
    char states_path[JW_STORAGE_PATH_MAX];
    char cheats_path[JW_STORAGE_PATH_MAX];
    bool primary;
} jw_storage_source;

typedef struct {
    jw_storage_source sources[JW_STORAGE_MAX_SOURCES];
    int count;
} jw_storage_source_list;

int jw_storage_sources_resolve(const char *primary_root, jw_storage_source_list *out);

const jw_storage_source *jw_storage_sources_primary(const jw_storage_source_list *list);
const jw_storage_source *jw_storage_sources_find_for_path(const jw_storage_source_list *list,
                                                          const char *path);
const jw_storage_source *jw_storage_sources_find_by_id(const jw_storage_source_list *list,
                                                       const char *id);

int jw_storage_resolve_path(const jw_storage_source_list *list, const char *path,
                            char *out, size_t out_size);
int jw_storage_db_path_for_source(const jw_storage_source *source,
                                  const char *relative_path,
                                  const char *absolute_path,
                                  char *out, size_t out_size);

#endif /* JW_STORAGE_SOURCES_H */
