#ifndef JW_STORE_PAKRAT_H
#define JW_STORE_PAKRAT_H

#include <limits.h>

typedef struct {
    char platform[64];
    char sdcard_root[PATH_MAX];
    char state_dir[PATH_MAX];
    char db_path[PATH_MAX];
    char platform_root[PATH_MAX];
    char socket_path[PATH_MAX];
} jw_pakrat_context;

int jw_pakrat_rescan(const jw_pakrat_context *ctx);
/* Install (or update) a catalog pak. allow_adopt != 0 permits replacing a pak
   already on disk that Pak Rat does not own (a manual install); the caller is
   responsible for getting the user's consent first. */
int jw_pakrat_install_app(const jw_pakrat_context *ctx, const char *store_id,
                          int allow_adopt);
/* Install the exact version selected by a prior catalog read. The installer
   re-runs normal compatibility selection and refuses if it no longer resolves
   to expected_version. */
int jw_pakrat_install_app_target(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 const char *expected_version,
                                 int allow_adopt);
/* Repair an owned install from its exact immutable historical version. The
   gate exception applies only when version equals the ownership record. */
int jw_pakrat_repair_app_version(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 const char *version);
int jw_pakrat_uninstall_app(const jw_pakrat_context *ctx, const char *store_id);

#endif
