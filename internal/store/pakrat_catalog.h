#ifndef JW_STORE_PAKRAT_CATALOG_H
#define JW_STORE_PAKRAT_CATALOG_H

#define JW_PAKRAT_CATALOG_SCHEMA_MAX 1
#define JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF (-2)

typedef struct {
    char id[128];
    char name[256];
    char summary[512];
    char version[64];
    char min_leaf_version[64];
    char platform[64];
    char install_name[256];
    char install_path[512];
    char runtime_manifest_path[256];
    char artifact_url[1024];
    char artifact_name[256];
    char artifact_archive[16];
    char artifact_sha256[80];
    long long artifact_size;
    long long artifact_installed_size;
} jw_pakrat_catalog_package;

/* STORE-CONTENT-1. A package that declares `provides` lives in `content[]`,
   never in `apps[]`, because it is gated on the content-pak contract by
   construction and `apps[]` is the gate-unaware lane. A client that predates
   this contract parses `apps[]`, ignores the unknown `content` key, and never
   learns a content pak exists -- which is the whole reason the lane is a new
   key rather than a field on an existing one.

   The lane is metadata for what the store SHOWS. It never decides whether
   Pak Rat offers "Open": that comes from the installed pak's launch.sh, so
   the answer stays right for a hybrid, for a pure content pak, and for a
   sideloaded pak that is in no lane at all. */
typedef enum {
    JW_PAKRAT_LANE_APPS = 0,
    JW_PAKRAT_LANE_CONTENT = 1
} jw_pakrat_catalog_lane;

typedef struct {
    jw_pakrat_catalog_package package;
    char gated_version[64];
    char gated_min_leaf_version[64];
    jw_pakrat_catalog_lane lane;
} jw_pakrat_catalog_selection;

/* Parse one storefront and select the newest compatible version of every
   package for `platform`. device_leaf_version may be empty/unknown; that fails
   gated versions closed unless is_dev_override is nonzero.

   Returns 0 on success, JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF when the
   storefront schema is newer than this client, and -1 for malformed or
   internally inconsistent catalog data. */
int jw_pakrat_catalog_parse_and_select(
    const char *json,
    const char *platform,
    const char *device_leaf_version,
    int is_dev_override,
    jw_pakrat_catalog_selection *out,
    int max_count,
    int *out_count);

/* Resolve one exact immutable historical version. Gates are intentionally not
   applied here; callers must restrict this to repair of an owned install whose
   recorded version exactly matches requested_version.

   Returns 0 on success, 1 when the app/platform/version is not present,
   JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF for a newer schema, and -1 for
   malformed or internally inconsistent data. */
int jw_pakrat_catalog_find_exact(
    const char *json,
    const char *platform,
    const char *store_id,
    const char *requested_version,
    jw_pakrat_catalog_package *out);

#endif
