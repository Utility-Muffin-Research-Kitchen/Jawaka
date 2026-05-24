#ifndef JW_DB_H
#define JW_DB_H

#include <stddef.h>
#include <sqlite3.h>

typedef struct {
    int game_count;
    int app_count;
    char systems_summary[256];
    char sample_summary[256];
} jw_library_summary;

typedef struct {
    char name[64];
    int  game_count;
} jw_system_entry;

typedef struct {
    char name[256];
    char pak_dir[512];
    char icon[256];
} jw_app_entry;

typedef struct {
    char system[64];
    char name[256];
    char rom_path[512];
    char image_path[512];
} jw_game_entry;

int  jw_db_open(const char *path, sqlite3 **out);
int  jw_db_apply_schema(sqlite3 *db);
void jw_db_close(sqlite3 *db);

int  jw_db_reset_library(sqlite3 *db);
int  jw_db_insert_game(sqlite3 *db, const char *system, const char *name, const char *rom_path, const char *image_path);
int  jw_db_insert_app(sqlite3 *db, const char *pak_dir, const char *name, const char *icon, const char *platform, const char *pak_version, const char *min_jawaka_version);
int  jw_db_read_summary(const char *db_path, jw_library_summary *out);
int  jw_db_list_systems(const char *db_path, jw_system_entry *out, int max_count, int *out_count);
int  jw_db_list_apps(const char *db_path, jw_app_entry *out, int max_count, int *out_count);
int  jw_db_list_games_for_system(const char *db_path, const char *system,
                                 jw_game_entry *out, int max_count, int *out_count);

int  jw_db_get_setting(const char *db_path, const char *key,
                        char *out, size_t out_size);
int  jw_db_set_setting(const char *db_path, const char *key, const char *value);
int  jw_db_get_theme_name(const char *db_path, char *out, size_t out_size);

#endif
