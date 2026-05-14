#ifndef JW_DB_H
#define JW_DB_H

#include <sqlite3.h>

typedef struct {
    int game_count;
    int app_count;
    char systems_summary[256];
    char sample_summary[256];
} jw_library_summary;

int  jw_db_open(const char *path, sqlite3 **out);
int  jw_db_apply_schema(sqlite3 *db);
void jw_db_close(sqlite3 *db);

int  jw_db_reset_library(sqlite3 *db);
int  jw_db_insert_game(sqlite3 *db, const char *system, const char *name, const char *rom_path, const char *image_path);
int  jw_db_insert_app(sqlite3 *db, const char *pak_dir, const char *name, const char *icon, const char *platform, const char *pak_version, const char *min_jawaka_version);
int  jw_db_read_summary(const char *db_path, jw_library_summary *out);

#endif
