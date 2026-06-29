#ifndef JW_STORE_PAKRAT_STATE_H
#define JW_STORE_PAKRAT_STATE_H

#include "internal/store/pakrat.h"

typedef struct {
    char id[128];
    char name[256];
    char summary[512];
    char version[64];
    char platform[64];
    char install_name[256];
    char install_path[512];
    char runtime_manifest_path[256];
    char artifact_url[1024];
    char artifact_name[256];
    char artifact_sha256[80];
    long long artifact_size;
} jw_pakrat_catalog_package;

typedef enum {
    JW_PAKRAT_APP_AVAILABLE = 0,
    JW_PAKRAT_APP_INSTALLED,
    JW_PAKRAT_APP_UPDATE_AVAILABLE,
    JW_PAKRAT_APP_STALE,
    JW_PAKRAT_APP_UNMANAGED      /* present on disk but not installed by Pak Rat */
} jw_pakrat_app_status;

typedef struct {
    jw_pakrat_catalog_package package;
    jw_pakrat_app_status status;
    int managed;
    int installed_owned;
    int app_present;
    char installed_version[64];
    char installed_at[64];
    char app_name[256];
    char app_pak_dir[512];
} jw_pakrat_app_state;

const char *jw_pakrat_app_status_name(jw_pakrat_app_status status);

/* Returns 0 on success, 1 when no catalog is configured or no matching package
   exists, and -1 on invalid catalog data or I/O failure. */
int jw_pakrat_find_catalog_package(const jw_pakrat_context *ctx,
                                   const char *store_id,
                                   jw_pakrat_catalog_package *out,
                                   int *out_is_dev_override);

/* Builds the read-only app-store state the UI needs: catalog package data joined
   to Pak Rat ownership rows and scanned app presence. Returns 0 on success, 1
   when no catalog is configured, and -1 on error. */
int jw_pakrat_list_app_states(const jw_pakrat_context *ctx,
                              jw_pakrat_app_state *out,
                              int max_count,
                              int *out_count);

#endif
