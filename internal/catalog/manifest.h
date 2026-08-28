#ifndef JW_CONTENT_MANIFEST_H
#define JW_CONTENT_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cJSON cJSON;

#define JW_CONTENT_REASON_MAX 64

typedef enum {
    JW_CONTENT_PLATFORM_LANE = 0,
    JW_CONTENT_SHARED_LANE,
} jw_content_install_lane;

/* A validated CONTENT-1 manifest. The parsed document is retained so the
   compiler can consume the exact provides object that was validated. */
typedef struct {
    cJSON *document;
    cJSON *provides;
    bool redundant_case_variant;
} jw_content_manifest;

/* Validate one already-read pak.json plus its install context. On success,
   `out` owns the parsed JSON until jw_content_manifest_destroy(). */
bool jw_content_manifest_validate(const char *pak_json_text,
                                  const char *pak_abs_path,
                                  jw_content_install_lane lane,
                                  const char *source_id,
                                  jw_content_manifest *out,
                                  char *reason,
                                  size_t reason_size);

void jw_content_manifest_destroy(jw_content_manifest *manifest);

#endif
