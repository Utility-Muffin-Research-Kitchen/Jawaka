#ifndef JW_DISCOVERY_DELETE_H
#define JW_DISCOVERY_DELETE_H

#include "internal/discovery/content.h"

typedef struct {
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    char rom_relpath[512];
    char name[256];
    char system_id[64];
} jw_delete_owner;

typedef enum {
    JW_DELETE_REMOVE = 0,
    JW_DELETE_SHARED,
    JW_DELETE_PRESERVED,
} jw_delete_keep;

typedef struct {
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    char rom_relpath[512];
    char path[JW_STORAGE_PATH_MAX];
    uint64_t size;
    bool missing;
    jw_delete_keep keep;
} jw_delete_file;

typedef struct {
    jw_delete_file *files;
    size_t file_count;
    size_t launch_file;
    size_t disc_count;
    size_t remove_count;
    size_t kept_count;
    size_t missing_count;
    uint64_t bytes;
    char missing_sources[512];
    bool missing_descriptor;
    bool writable_image;
    void *snapshot;
} jw_delete_plan;

typedef bool (*jw_delete_cancelled)(void *context);
/* Invoked before every removal, including an already absent file. Returning -1
   stops immediately; the daemon can recheck storage and tests can inject I/O errors. */
typedef int (*jw_delete_guard)(void *context, const jw_delete_file *file,
                              char *error, size_t error_size);

typedef struct {
    /* One bit per plan file: removed now or confirmed absent, safe to reconcile. */
    bool *completed;
    size_t removed_count;
    size_t absent_count;
    uint64_t bytes;
    size_t failed_index;
} jw_delete_result;

bool jw_delete_supported(const jw_ra_system *system);
int jw_delete_plan_build(const jw_storage_source_list *sources,
                         const jw_ra_catalog *catalog,
                         const char *source_id, const char *rom_relpath,
                         const char *system_id,
                         const jw_delete_owner *owners, size_t owner_count,
                         const char *const *protected_paths, size_t protected_count,
                         jw_delete_cancelled cancelled, void *context,
                         jw_delete_plan *out, char *error, size_t error_size);
/* Compare a fresh build to the reviewed plan before consuming its one-use token. */
bool jw_delete_plan_equal(const jw_delete_plan *a, const jw_delete_plan *b);
bool jw_delete_plan_sources_match(const jw_delete_plan *plan,
                                   const jw_storage_source_list *current);
int jw_delete_execute(const jw_delete_plan *plan, jw_delete_guard guard, void *context,
                      jw_delete_result *result, char *error, size_t error_size);
void jw_delete_plan_free(jw_delete_plan *plan);
void jw_delete_result_free(jw_delete_result *result);

#endif
