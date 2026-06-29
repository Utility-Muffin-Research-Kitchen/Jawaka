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
int jw_pakrat_uninstall_app(const jw_pakrat_context *ctx, const char *store_id);

#endif
