#ifndef JW_DISCOVERY_CONTENT_H
#define JW_DISCOVERY_CONTENT_H

#include "internal/storage/sources.h"
#include "internal/retroarch/catalog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    /* Heap strings owned by the content; jw_content_free releases them. */
    char *rom_relpath;
    char *path;
    uint64_t size;
    bool missing;
    /* Exact descriptor bytes, including labels, directives and line endings. */
    char *descriptor;
    size_t descriptor_size;
} jw_content_file;

typedef struct {
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    char rom_relpath[512];
    char member[512];
    char label[256];
    size_t file_index;
    /* Exact root-playlist line span, including its original line ending. */
    size_t line_start;
    size_t line_end;
} jw_content_disc;

typedef struct {
    size_t parent;
    size_t child;
} jw_content_reference;

typedef struct {
    const char *source_id;
    const char *rom_relpath;
    const jw_ra_system *system;
    size_t file_index;
} jw_content_root;

typedef struct {
    bool is_playlist;
    jw_content_file *files;
    size_t file_count;
    jw_content_disc *discs;
    size_t disc_count;
    /* files[launch_file] is the selected launch file; files are deduplicated. */
    size_t launch_file;
    jw_content_reference *references;
    size_t reference_count;
} jw_content;

/* A cheap extension check; inspection determines whether it contains discs. */
bool jw_content_is_playlist_path(const char *path);
/* Read-only inspection. Missing referenced files retain their normalized keys;
   unreadable/malformed descriptors and unsafe paths fail with a concrete error.
   The catalog system is needed for CMD file-argument recognition. */
int jw_content_inspect(const jw_storage_source_list *sources,
                       const char *source_id, const char *rom_relpath,
                       const jw_ra_system *system, jw_content *out,
                       char *error, size_t error_size);
/* One inspection graph for a deletion preview. Each descriptor is read once;
   discs describe only roots[0], while references include every root. */
int jw_content_inspect_many(const jw_storage_source_list *sources,
                            jw_content_root *roots, size_t root_count,
                            bool allow_unavailable_references,
                            bool (*cancelled)(void *), void *context,
                            jw_content *out, char *error, size_t error_size);
bool jw_content_is_descriptor_path(const char *path);
void jw_content_free(jw_content *content);

#endif
