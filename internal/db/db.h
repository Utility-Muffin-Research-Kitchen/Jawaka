#ifndef JW_DB_H
#define JW_DB_H

#include <stddef.h>
#include <sqlite3.h>

typedef struct {
    int game_count;
    int app_count;
    int system_count;
    char systems_summary[256];
    char sample_summary[256];
} jw_library_summary;

/* Aggregate library + playtime stats for the System menu's Info pages. */
typedef struct {
    char name[128];
    char system[64];
    long long playtime_s;
    long long last_played;
} jw_stat_game;

typedef struct {
    char system[64];
    int  game_count;
    long long playtime_s;
} jw_stat_system;

#define JW_STATS_TOP_MAX     8
#define JW_STATS_SYSTEM_MAX  64

typedef struct {
    long long total_playtime_s;   /* SUM(playtime_s) over all games            */
    int  games_played;       /* games with playtime_s > 0                 */
    int  game_count;
    int  app_count;
    int  favorite_count;     /* favorited games                           */
    int  art_covered;        /* games with a non-empty image_path         */
    long long last_played;        /* MAX(last_played), 0 if never              */
    jw_stat_game   top[JW_STATS_TOP_MAX];        /* most-played, time desc */
    int            top_count;
    jw_stat_system systems[JW_STATS_SYSTEM_MAX]; /* per-system, count desc */
    int            system_count;
} jw_library_stats;

typedef struct {
    char name[64];          /* system id / folder code, e.g. "FC" (used for DB queries) */
    char display_name[64];  /* full name for display, e.g. "Famicom"; filled by the launcher */
    int  game_count;
} jw_system_entry;

typedef struct {
    char name[256];
    char pak_dir[512];
    char icon[256];
} jw_app_entry;

typedef struct {
    char store_id[128];
    char version[64];
    char platform[64];
    char install_path[512];
    char artifact_sha256[80];
    char installed_at[64];
    int  app_present;
    char app_name[256];
    char app_pak_dir[512];
} jw_pakrat_install;

typedef struct {
    int  id;
    char system[64];
    char name[256];
    char rom_path[512];
    char image_path[512];
    int  favorite;   /* 1 if present in favorites, else 0 */
} jw_game_entry;

typedef enum {
    JW_SEARCH_GAME = 0,
    JW_SEARCH_APP
} jw_search_kind;

typedef struct {
    jw_search_kind kind;
    char name[256];
    char system[64];
    char rom_path[512];
    char image_path[512];
    char pak_dir[512];
    char icon[256];
} jw_search_result;

int  jw_db_open(const char *path, sqlite3 **out);
int  jw_db_apply_schema(sqlite3 *db);
void jw_db_close(sqlite3 *db);

int  jw_db_reset_library(sqlite3 *db);
/* Non-destructive rescan helpers. scan_begin sets up per-scan "seen" tracking;
   insert_game/insert_app upsert (preserving id) and record seen; scan_prune
   removes only rows whose ROM/pak vanished plus any orphaned favorites/recents.
   Call inside a transaction: scan_begin -> inserts... -> scan_prune. */
int  jw_db_scan_begin(sqlite3 *db);
int  jw_db_scan_prune(sqlite3 *db);
int  jw_db_insert_game(sqlite3 *db, const char *system, const char *name, const char *rom_path, const char *image_path);
int  jw_db_insert_app(sqlite3 *db, const char *pak_dir, const char *name, const char *icon, const char *platform, const char *pak_version, const char *min_jawaka_version);
int  jw_db_read_summary(const char *db_path, jw_library_summary *out);
int  jw_db_read_stats(const char *db_path, jw_library_stats *out);
int  jw_db_list_systems(const char *db_path, jw_system_entry *out, int max_count, int *out_count);
int  jw_db_list_apps(const char *db_path, jw_app_entry *out, int max_count, int *out_count);
int  jw_db_count_games_for_system(const char *db_path, const char *system, int *out_count);
int  jw_db_list_games_for_system(const char *db_path, const char *system,
                                 jw_game_entry *out, int max_count, int *out_count);
int  jw_db_search_library(const char *db_path, const char *query,
                          jw_search_result *out, int max_count, int *out_count);

/* Favorites. kind is "game" or "app"; target_id is the games/apps id.
   set_favorite adds (on != 0) or removes (on == 0); it is idempotent.
   list_favorite_games returns favorited games newest-first (by added_at). */
int  jw_db_set_favorite(const char *db_path, const char *kind, int target_id, int on);
int  jw_db_list_favorite_games(const char *db_path, jw_game_entry *out,
                               int max_count, int *out_count);

/* Recents + playtime. record_play (called when a game session ends) bumps the
   game's cumulative playtime_s + last_played and upserts its recents row.
   list_recent_games returns games most-recently-opened first. */
int  jw_db_record_play(const char *db_path, const char *rom_path, int duration_s);
int  jw_db_list_recent_games(const char *db_path, jw_game_entry *out,
                             int max_count, int *out_count);
/* Drop a single play-history row (kind 'game'/'app'). Does not touch the
   game/app itself or its favorite; the game just leaves the Recents list.
   Idempotent — removing an absent row succeeds. */
int  jw_db_remove_recent(const char *db_path, const char *kind, int target_id);

/* Scoped content actions/settings. game settings are keyed by stable games.id;
   system settings are keyed by canonical Jawaka system id. Empty values should
   normally be deleted by callers rather than stored. */
int  jw_db_get_game_by_rom_path(const char *db_path, const char *rom_path,
                                jw_game_entry *out);
int  jw_db_get_game_setting(const char *db_path, int game_id,
                            const char *key, char *out, size_t out_size);
int  jw_db_set_game_setting(const char *db_path, int game_id,
                            const char *key, const char *value);
int  jw_db_delete_game_setting(const char *db_path, int game_id,
                               const char *key);
int  jw_db_get_system_setting(const char *db_path, const char *system,
                              const char *key, char *out, size_t out_size);
int  jw_db_set_system_setting(const char *db_path, const char *system,
                              const char *key, const char *value);
int  jw_db_delete_system_setting(const char *db_path, const char *system,
                                 const char *key);

typedef struct {
    const char *key;
    char       *out;
    size_t      out_size;
    int         found;
} jw_db_setting_query;

/* Set a game's image_path by rom_path (scrape worker: art landed outside a
   scan). Returns 0 on success, 1 when no game row matched, -1 on error. */
int  jw_db_set_game_image(const char *db_path, const char *rom_path,
                          const char *image_path);

/* Atomically increment an integer-valued setting, creating it at 1 when
   absent. Safe against concurrent bumps from the daemon main loop and the
   scrape worker (single UPSERT statement). */
int  jw_db_increment_setting(const char *db_path, const char *key);

int  jw_db_get_setting(const char *db_path, const char *key,
                        char *out, size_t out_size);
int  jw_db_get_settings(const char *db_path, jw_db_setting_query *queries,
                        int count);
int  jw_db_set_setting(const char *db_path, const char *key, const char *value);
/* Write multiple key/value settings in a single open + transaction. Far cheaper
   than N jw_db_set_setting() calls, each of which re-opens the DB and re-applies
   the schema. keys[i]/values[i] are paired; count is the number of pairs. */
int  jw_db_set_settings(const char *db_path, const char *const *keys,
                        const char *const *values, int count);
int  jw_db_get_theme_name(const char *db_path, char *out, size_t out_size);

/* Pak Rat store ownership. apps remains scan truth; pakrat_installs records
   packages installed/updated through the store and intentionally survives
   library rescans. install_path is the Apps-namespace path, e.g.
   "mlp1/SDLReader.pak" or "shared/RetroArch.pak". */
int  jw_db_pakrat_upsert_install(const char *db_path, const char *store_id,
                                 const char *version, const char *platform,
                                 const char *install_path,
                                 const char *artifact_sha256,
                                 const char *installed_at);
int  jw_db_pakrat_remove_install(const char *db_path, const char *store_id);
/* Returns 0 when found, 1 when no matching store_id exists, -1 on error. */
int  jw_db_pakrat_get_install(const char *db_path, const char *store_id,
                              jw_pakrat_install *out);
int  jw_db_pakrat_list_installs(const char *db_path, jw_pakrat_install *out,
                                int max_count, int *out_count);

#endif
