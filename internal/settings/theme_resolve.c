#include "internal/settings/theme_resolve.h"

#include "internal/db/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int jw_resolve_theme_name(const char *db_path, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';

    /* 1. JAWAKA_THEME — explicit dev/test override, always wins */
    const char *env = getenv("JAWAKA_THEME");
    if (env && env[0]) {
        snprintf(out, out_size, "%s", env);
        return 0;
    }

    /* 2. persisted DB setting */
    if (db_path) {
        char buf[256];
        if (jw_db_get_theme_name(db_path, buf, sizeof(buf)) == 0 && buf[0]) {
            snprintf(out, out_size, "%s", buf);
            return 0;
        }
    }

    /* 3. hard default */
    snprintf(out, out_size, "%s", "Jawaka-Tabs");
    return 0;
}
