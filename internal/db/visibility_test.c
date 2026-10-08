#include "internal/db/db.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void exec_ok(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "%s\n", error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}

static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static void expect_hidden(const char *path, int id, int expected) {
    int hidden = -1;
    assert(jw_db_is_game_hidden(path, id, &hidden) == 0);
    assert(hidden == expected);
}

int main(void) {
    /* URI read-only mode exercises a real SQLite readonly connection even
       when a test runner can bypass filesystem permission bits. */
    assert(sqlite3_config(SQLITE_CONFIG_URI, 1) == SQLITE_OK);
    char path[] = "/tmp/jawaka-visibility.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    sqlite3 *db = NULL;
    assert(jw_db_open(path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    exec_ok(db,
        "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path) VALUES"
        "(1,'NES','Alpha','primary','NES/alpha.nes','Roms/NES/alpha.nes'),"
        "(2,'NES','Zulu','primary','NES/zulu.nes','Roms/NES/zulu.nes'),"
        "(3,'PS','Only game','primary','PS/only.cue','Roms/PS/only.cue'),"
        "(4,'NES','Alpha copy','secondary_sd','NES/alpha.nes','/card/Roms/NES/alpha.nes'),"
        "(5,'PS','超级马里奥','primary','PS/chinese.cue','Roms/PS/chinese.cue');"
        "INSERT INTO hidden_roms VALUES('primary','NES/alpha.nes','member');"
        "INSERT INTO settings VALUES('five_game_ids','1,2'),('language','zh_CN');");
    assert(jw_db_set_favorite(path, "game", 1, 1) == 0);
    assert(jw_db_set_favorite(path, "game", 2, 1) == 0);
    assert(jw_db_record_play_by_id(path, 1, 1000) == 0);
    assert(jw_db_record_play_by_id(path, 2, 10) == 0);
    exec_ok(db, "UPDATE recents SET last_opened=100 WHERE target_id=2;"
                "UPDATE recents SET last_opened=200 WHERE target_id=1;");
    assert(jw_db_set_game_setting(path, 1, "display_name", "Alpha HiddenSuffix") == 0);
    assert(jw_db_set_game_setting(path, 2, "display_name", "Zulu VisibleSuffix") == 0);
    expect_hidden(path, 1, 0); /* An archive member never hides the whole game. */
    assert(jw_db_set_game_hidden(path, 1, 1) == 0);
    assert(jw_db_set_game_hidden(path, 1, 1) == 0);
    assert(jw_db_set_game_hidden(path, 3, 1) == 0);
    assert(jw_db_set_game_hidden(path, 5, 1) == 0);
    assert(jw_db_set_game_hidden(path, 999, 1) == JW_DB_RC_NO_ROW);
    expect_hidden(path, 1, 1);
    expect_hidden(path, 4, 0); /* Same path on the other card stays independent. */

    jw_system_entry systems[4];
    jw_game_entry games[8], game;
    jw_search_result search[8];
    int count = 0;
    assert(jw_db_list_systems(path, systems, 4, &count) == 0 && count == 1);
    assert(strcmp(systems[0].name, "NES") == 0 && systems[0].game_count == 2);
    assert(jw_db_count_games_for_system(path, "NES", &count) == 0 && count == 2);
    assert(jw_db_count_games_for_system(path, "PS", &count) == 0 && count == 0);
    assert(jw_db_list_games_for_system(path, "NES", games, 1, &count) == 0);
    assert(count == 1 && games[0].id == 4);
    assert(jw_db_list_favorite_games(path, games, 1, &count) == 0);
    assert(count == 1 && games[0].id == 2);
    assert(jw_db_list_recent_games(path, games, 1, &count) == 0);
    assert(count == 1 && games[0].id == 2);
    assert(jw_db_search_library(path, "alpha", search, 1, &count) == 0);
    assert(count == 1 && search[0].id == 4); /* FTS filtering precedes LIMIT. */
    assert(jw_db_search_library(path, "suffix", search, 1, &count) == 0);
    assert(count == 1 && search[0].id == 2); /* Override LIKE fallback. */
    assert(jw_db_search_library(path, "CJMLA", search, 8, &count) == 0 && count == 0);
    assert(jw_db_search_library(path, "超级", search, 8, &count) == 0 && count == 0);

    jw_library_stats stats;
    jw_library_summary summary;
    assert(jw_db_read_stats(path, &stats) == 0);
    assert(stats.game_count == 5 && stats.total_playtime_s == 1010);
    assert(stats.favorite_count == 2 && stats.top_count == 1);
    assert(strcmp(stats.top[0].name, "Zulu VisibleSuffix") == 0);
    assert(jw_db_read_summary(path, &summary) == 0);
    assert(summary.game_count == 2 && summary.system_count == 1);
    assert(strstr(summary.sample_summary, "HiddenSuffix") == NULL);

    /* Focus, boot resume and a running game can still resolve hidden IDs/paths. */
    assert(jw_db_get_game_by_id(path, 1, &game) == 0 && game.favorite);
    assert(game.playtime_s == 1000);
    assert(jw_db_get_game_by_rom_path(path, "Roms/NES/alpha.nes", &game) == 0);
    assert(jw_db_get_game_by_source_relpath(path, "primary", "NES/alpha.nes", &game) == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM settings WHERE key='five_game_ids' AND value='1,2'") == 1);
    assert(jw_db_count_hidden_games(path, &count) == 0 && count == 3);
    assert(jw_db_list_hidden_games(path, games, 8, &count) == 0 && count == 3);
    assert(games[0].id == 1 && games[0].favorite && games[0].playtime_s == 1000);

    char readonly[256];
    snprintf(readonly, sizeof(readonly), "file:%s?mode=ro", path);
    assert(jw_db_set_game_hidden(readonly, 2, 1) == JW_DB_RC_READONLY);
    assert(jw_db_set_game_hidden(readonly, 1, 0) == JW_DB_RC_READONLY);
    assert(jw_db_clear_hidden_game(readonly, "primary", "NES/alpha.nes") == JW_DB_RC_READONLY);
    expect_hidden(path, 1, 1);
    expect_hidden(path, 2, 0);

    /* Resetting overrides and scanning the same identity keep Hide and history. */
    assert(jw_db_delete_game_setting(path, 1, "display_name") == 0);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game_stable(db, "NES", "Alpha rescanned", "primary",
        "NES/alpha.nes", "Roms/NES/alpha.nes", NULL, NULL, NULL) == 0);
    assert(jw_db_scan_prune(db) == 0); /* No completed sources: retain every row. */
    expect_hidden(path, 1, 1);
    assert(jw_db_get_game_by_id(path, 1, &game) == 0 && game.playtime_s == 1000);
    assert(jw_db_clear_hidden_game(path, "primary", "NES/alpha.nes") == 0);
    expect_hidden(path, 1, 0);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms WHERE member='member'") == 1);
    assert(jw_db_list_favorite_games(path, games, 8, &count) == 0 && count == 2);
    assert(jw_db_list_recent_games(path, games, 1, &count) == 0 && games[0].id == 1);

    assert(jw_db_set_game_hidden(path, 1, 1) == 0);
    assert(jw_db_set_game_hidden(path, 2, 1) == 0);
    assert(jw_db_set_game_hidden(path, 4, 1) == 0);
    assert(jw_db_list_systems(path, systems, 4, &count) == 0 && count == 0);
    assert(jw_db_read_summary(path, &summary) == 0 && summary.game_count == 0);
    assert(jw_db_read_stats(path, &stats) == 0 && stats.game_count == 5 && stats.top_count == 0);
    assert(jw_db_count_hidden_games(path, &count) == 0 && count == 5);

    /* Pruning missing files preserves clearable preferences, and a returning
       source/path inherits Hide without an artificial game row. */
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_scan_source_complete(db, "primary") == 0);
    assert(jw_db_scan_prune(db) == 0);
    assert(jw_db_list_hidden_games(path, games, 8, &count) == 0 && count == 5);
    int orphans = 0;
    for (int i = 0; i < count; ++i) {
        if (games[i].id == 0) {
            assert(strcmp(games[i].name, games[i].rom_relpath) == 0);
            assert(strcmp(games[i].source_id, "primary") == 0);
            orphans++;
        }
    }
    assert(orphans == 4);
    assert(jw_db_clear_hidden_game(path, "primary", "PS/only.cue") == 0);
    assert(jw_db_count_hidden_games(path, &count) == 0 && count == 4);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game_stable(db, "NES", "Returned", "primary",
        "NES/alpha.nes", "Roms/NES/alpha.nes", NULL, NULL, NULL) == 0);
    assert(jw_db_get_game_by_source_relpath(path, "primary", "NES/alpha.nes", &game) == 0);
    expect_hidden(path, game.id, 1);
    assert(jw_db_set_game_hidden(path, game.id, 0) == 0);
    assert(jw_db_set_game_hidden(path, game.id, 0) == 0);
    expect_hidden(path, game.id, 0);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms WHERE member='member'") == 1);

    jw_db_close(db);
    unlink(path);
    puts("visibility tests passed");
    return 0;
}
