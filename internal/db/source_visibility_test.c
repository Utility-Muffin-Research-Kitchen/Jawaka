#include "internal/db/db.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Non-MLP1 builds treat a source root as available when its directory exists.
   The MLP1 mountinfo check is covered on the device. */
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

static bool has_game(const jw_game_entry *games, int count, int id) {
    for (int i = 0; i < count; i++) {
        if (games[i].id == id) return true;
    }
    return false;
}

static int system_games(const char *path, const char *name) {
    jw_system_entry systems[8];
    int count = 0;
    assert(jw_db_list_systems(path, systems, 8, &count) == 0);
    for (int i = 0; i < count; i++) {
        if (strcmp(systems[i].name, name) == 0) return systems[i].game_count;
    }
    return 0;
}

/* The second card's games are ids 2 (NES) and 3 (PS, its only game). */
static void expect_second_card_listed(const char *path, bool listed) {
    jw_game_entry games[8];
    jw_search_result search[8];
    jw_library_summary summary;
    int count = 0;

    assert(system_games(path, "NES") == (listed ? 3 : 2));
    assert(system_games(path, "PS") == (listed ? 1 : 0));

    assert(jw_db_count_games_for_system(path, "NES", &count) == 0);
    assert(count == (listed ? 3 : 2));
    assert(jw_db_list_games_for_system(path, "NES", games, 8, &count) == 0);
    assert(count == (listed ? 3 : 2));
    assert(has_game(games, count, 1) && has_game(games, count, 4));
    assert(has_game(games, count, 2) == listed);
    assert(jw_db_count_games_for_system(path, "PS", &count) == 0);
    assert(count == (listed ? 1 : 0));

    assert(jw_db_list_favorite_games(path, games, 8, &count) == 0);
    assert(has_game(games, count, 1));
    assert(has_game(games, count, 2) == listed);
    assert(jw_db_list_recent_games(path, games, 8, &count) == 0);
    assert(has_game(games, count, 1));
    assert(has_game(games, count, 2) == listed);

    assert(jw_db_search_library(path, "Mario", search, 8, &count) == 0);
    assert(count == (listed ? 2 : 1));
    assert(search[0].id == 1 || listed);

    assert(jw_db_read_summary(path, &summary) == 0);
    assert(summary.game_count == (listed ? 4 : 2));
    assert(summary.system_count == (listed ? 2 : 1));
}

int main(void) {
    /* URI read-only mode exercises a real SQLite readonly connection even
       when a test runner can bypass filesystem permission bits. */
    assert(sqlite3_config(SQLITE_CONFIG_URI, 1) == SQLITE_OK);

    char root[] = "/tmp/jawaka-source-visibility.XXXXXX";
    assert(mkdtemp(root));
    char primary[128], card[128], card_roms[128], path[128], readonly[192], roots[256];
    snprintf(primary, sizeof(primary), "%s/primary", root);
    snprintf(card, sizeof(card), "%s/card", root);
    snprintf(card_roms, sizeof(card_roms), "%s/Roms", card);
    snprintf(path, sizeof(path), "%s/library.db", root);
    snprintf(readonly, sizeof(readonly), "file:%s?mode=ro", path);
    snprintf(roots, sizeof(roots), "%s:%s", primary, card);
    assert(mkdir(primary, 0755) == 0);
    setenv("SDCARD_PATHS", roots, 1);

    sqlite3 *db = NULL;
    assert(jw_db_open(path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    exec_ok(db,
        "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path) VALUES"
        "(1,'NES','Mario','primary','NES/mario.nes','Roms/NES/mario.nes'),"
        "(2,'NES','Mario','secondary_sd','NES/mario.nes','/card/Roms/NES/mario.nes'),"
        "(3,'PS','Only on card','secondary_sd','PS/only.cue','/card/Roms/PS/only.cue'),"
        "(4,'NES','Unknown source','source5','NES/unknown.nes','/other/NES/unknown.nes'),"
        "(5,'NES','Hidden on card','secondary_sd','NES/hidden.nes','/card/Roms/NES/hidden.nes');");
    assert(jw_db_set_game_hidden(path, 5, 1) == 0);
    assert(jw_db_set_favorite(path, "game", 1, 1) == 0);
    assert(jw_db_set_favorite(path, "game", 2, 1) == 0);
    assert(jw_db_record_play_by_id(path, 1, 10) == 0);
    assert(jw_db_record_play_by_id(path, 2, 20) == 0);
    assert(jw_db_set_game_setting(path, 2, "display_name", "Mario") == 0);

    /* Missing card: its rows leave every list, and an id no source claims stays. */
    expect_second_card_listed(path, false);

    /* The rows and their preferences survive being left out. */
    assert(scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='secondary_sd'") == 3);
    assert(scalar(db, "SELECT playtime_s FROM games WHERE id=2") == 20);
    assert(scalar(db, "SELECT COUNT(*) FROM game_settings WHERE game_id=2") == 1);

    /* A preference question and an identity lookup don't depend on the card. */
    int hidden = -1;
    assert(jw_db_is_game_hidden(path, 2, &hidden) == 0 && hidden == 0);
    assert(jw_db_is_game_hidden(path, 5, &hidden) == 0 && hidden == 1);
    jw_game_entry game;
    assert(jw_db_get_game_by_id(path, 2, &game) == 0);
    assert(strcmp(game.source_id, "secondary_sd") == 0);

    /* Hidden Games still lists a hidden game whose card is out. */
    jw_game_entry games[8];
    int count = 0;
    assert(jw_db_count_hidden_games(path, &count) == 0 && count == 1);
    assert(jw_db_list_hidden_games(path, games, 8, &count) == 0);
    assert(count == 1 && games[0].id == 5);

    /* LIMIT applies after the filter, so a full page of missing-card recents
       can't push a playable game off the list. */
    jw_game_entry one;
    exec_ok(db, "UPDATE recents SET last_opened=100 WHERE target_id=1;"
                "UPDATE recents SET last_opened=200 WHERE target_id=2;");
    assert(jw_db_list_recent_games(path, &one, 1, &count) == 0);
    assert(count == 1 && one.id == 1);

    /* A read-only library card can still answer. */
    assert(system_games(readonly, "PS") == 0);
    assert(system_games(readonly, "NES") == 2);

    /* A mounted card with no Roms folder has nothing that can launch. */
    assert(mkdir(card, 0755) == 0);
    expect_second_card_listed(path, false);

    /* The card coming back brings the same entries, without reopening anything. */
    assert(mkdir(card_roms, 0755) == 0);
    expect_second_card_listed(path, true);
    assert(jw_db_list_recent_games(path, &one, 1, &count) == 0);
    assert(count == 1 && one.id == 2);
    assert(scalar(db, "SELECT playtime_s FROM games WHERE id=2") == 20);

    /* A resolver failure must never hide a working library. */
    assert(rmdir(card_roms) == 0);
    assert(rmdir(card) == 0);
    expect_second_card_listed(path, false);
    setenv("ROMS_PATHS", "/only/one/entry", 1);
    expect_second_card_listed(path, true);
    unsetenv("ROMS_PATHS");

    /* Without a declared second source there is nothing to hide. */
    unsetenv("SDCARD_PATHS");
    setenv("SDCARD_PATH", primary, 1);
    expect_second_card_listed(path, true);

    jw_db_close(db);
    unlink(path);
    rmdir(primary);
    rmdir(root);
    puts("source visibility tests passed");
    return 0;
}
