#ifndef JW_DISCOVERY_H
#define JW_DISCOVERY_H

#include <sqlite3.h>

typedef struct {
    int game_count;
    int app_count;
    int system_count;
} jw_scan_result;

int jw_scan_library(sqlite3 *db, const char *sdcard_root, jw_scan_result *out);

#endif
