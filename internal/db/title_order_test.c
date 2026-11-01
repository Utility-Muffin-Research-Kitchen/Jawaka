#include "internal/core/title_order.h"
#include "internal/db/db.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int compare(const char *a, const char *b) {
    return jw_title_compare(a, strlen(a), b, strlen(b));
}

/* Every pair of a list in its expected order, so a comparison that isn't a
   consistent total order on these names fails here too. */
static void test_compare_order(void) {
    static const char *const ordered[] = {
        "",
        " Leading space",
        "(Beta)",
        "2 Fast",
        "007",
        "7",
        "10 Yard Fight",
        "99999999999999999999999",
        "100000000000000000000000",
        ":Colon",
        "_Underscore",
        "Aardvark",
        "aardvark",
        "Game 01 a",
        "Game 1 b",
        "Mega Man",
        "Mega Man 2",
        "Mega Man 10",
        "Mega Man X",
        "Zelda",
        "zelda 2",
        "\xC3\x89mile",
        "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3",
    };
    for (int i = 0; i < COUNT(ordered); i++) {
        for (int j = 0; j < COUNT(ordered); j++) {
            int got = compare(ordered[i], ordered[j]);
            int want = i < j ? -1 : (i > j ? 1 : 0);
            if (got != want) {
                fprintf(stderr, "title-order-test: \"%s\" vs \"%s\" = %d, want %d\n",
                        ordered[i], ordered[j], got, want);
                exit(1);
            }
        }
    }
}

/* SQLite hands the collation lengths, not terminated strings. */
static void test_compare_uses_lengths(void) {
    assert(jw_title_compare("Game 12", 6, "Game 1", 6) == 0);
    assert(jw_title_compare("Game 12", 7, "Game 1", 6) > 0);
    assert(jw_title_compare("abc", 0, "abd", 0) == 0);
    assert(jw_title_compare("0", 1, "00", 2) < 0);
}

static void expect_names(const char *label, const jw_game_entry *games, int count,
                         const char *const *want, int want_count) {
    if (count != want_count) {
        fprintf(stderr, "title-order-test: %s: %d rows, want %d\n", label, count, want_count);
        exit(1);
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(games[i].name, want[i]) != 0) {
            fprintf(stderr, "title-order-test: %s: row %d is \"%s\", want \"%s\"\n",
                    label, i, games[i].name, want[i]);
            exit(1);
        }
    }
}

static void test_lists(void) {
    char path[] = "/tmp/jawaka-title-order.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);

    sqlite3 *db = NULL;
    assert(jw_db_open(path, &db) == 0 && jw_db_apply_schema(db) == 0);
    assert(jw_db_scan_begin(db) == 0);
    static const char *const scanned[] = {
        "zelda", "Mega Man 10", "aardvark", "Mega Man 2", "Alpha", "Renamed",
    };
    for (int i = 0; i < COUNT(scanned); i++) {
        char rom[128];
        snprintf(rom, sizeof(rom), "Roms/SFC/%s.sfc", scanned[i]);
        assert(jw_db_insert_game(db, "SFC", scanned[i], rom, NULL) == 0);
    }
    jw_db_close(db);

    jw_game_entry renamed;
    assert(jw_db_get_game_by_rom_path(path, "Roms/SFC/Renamed.sfc", &renamed) == 0);
    assert(jw_db_set_game_setting(path, renamed.id, "display_name", "Mega Man 3") == 0);

    /* Lowercase titles sit among the others, and an override sorts as its title. */
    jw_game_entry games[16];
    int count = 0;
    static const char *const listed[] = {
        "aardvark", "Alpha", "Mega Man 2", "Mega Man 3", "Mega Man 10", "zelda",
    };
    assert(jw_db_list_games_for_system(path, "SFC", games, COUNT(games), &count) == 0);
    expect_names("system list", games, count, listed, COUNT(listed));

    /* Pinned games come first by rank, and only this list reads the ranks. */
    assert(jw_db_set_game_setting(path, games[5].id, JW_GAME_SETTING_SORT_RANK, "2") == 0);
    assert(jw_db_set_game_setting(path, games[3].id, JW_GAME_SETTING_SORT_RANK, "1") == 0);
    jw_game_entry pinned[16];
    int pinned_count = 0;
    static const char *const pinned_order[] = {
        "Mega Man 3", "zelda", "aardvark", "Alpha", "Mega Man 2", "Mega Man 10",
    };
    assert(jw_db_list_games_for_system(path, "SFC", pinned, COUNT(pinned), &pinned_count) == 0);
    expect_names("pinned system list", pinned, pinned_count, pinned_order, COUNT(pinned_order));
    assert(pinned[0].sort_rank == 1 && pinned[1].sort_rank == 2 && pinned[2].sort_rank == 0);

    /* Junk in the setting isn't a pin. */
    assert(jw_db_set_game_setting(path, games[5].id, JW_GAME_SETTING_SORT_RANK, "soon") == 0);
    assert(jw_db_set_game_setting(path, games[3].id, JW_GAME_SETTING_SORT_RANK, "-4") == 0);
    assert(jw_db_list_games_for_system(path, "SFC", pinned, COUNT(pinned), &pinned_count) == 0);
    expect_names("junk ranks", pinned, pinned_count, listed, COUNT(listed));

    /* Folder ranks: exact keys only, ignoring ASCII case in the path. */
    assert(jw_db_set_system_setting(path, "SFC", JW_SYSTEM_SETTING_FOLDER_RANK_PREFIX "hacks/translated", "3") == 0);
    assert(jw_db_set_system_setting(path, "SFC", JW_SYSTEM_SETTING_FOLDER_RANK_PREFIX "junk", "x") == 0);
    assert(jw_db_set_system_setting(path, "GBA", JW_SYSTEM_SETTING_FOLDER_RANK_PREFIX "other", "1") == 0);
    jw_folder_rank *ranks = NULL;
    int rank_count = 0;
    jw_db_list_folder_ranks(path, "SFC", &ranks, &rank_count);
    assert(rank_count == 1);
    assert(jw_db_folder_rank(ranks, rank_count, "Hacks/Translated") == 3);
    assert(jw_db_folder_rank(ranks, rank_count, "Hacks") == 0);
    assert(jw_db_folder_rank(ranks, rank_count, "Hacks/Translated/More") == 0);
    jw_db_free_folder_ranks(ranks, rank_count);

    /* Favorites use the same order, and so does the Hidden Games list. */
    for (int i = 0; i < count; i++) {
        assert(jw_db_set_favorite(path, "game", games[i].id, 1) == 0);
    }
    int favorite_count = 0;
    jw_game_entry favorites[16];
    assert(jw_db_list_favorite_games(path, favorites, COUNT(favorites), &favorite_count) == 0);
    expect_names("favorites", favorites, favorite_count, listed, COUNT(listed));

    for (int i = 0; i < count; i++) {
        assert(jw_db_set_game_hidden(path, games[i].id, 1) == 0);
    }
    int hidden_count = 0;
    jw_game_entry hidden[16];
    assert(jw_db_list_hidden_games(path, hidden, COUNT(hidden), &hidden_count) == 0);
    expect_names("hidden games", hidden, hidden_count, listed, COUNT(listed));

    /* The collation exists only on jw_db_open connections. Nothing stored may name
       it, or a connection without it, such as Central Scrutinizer or an older
       release, could not use the library. */
    sqlite3 *plain = NULL;
    sqlite3_stmt *st = NULL;
    assert(sqlite3_open(path, &plain) == SQLITE_OK);
    assert(sqlite3_prepare_v2(plain,
                              "SELECT COUNT(*) FROM sqlite_master WHERE sql LIKE '%JW_TITLE%';",
                              -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(plain, "SELECT name FROM games ORDER BY name;", -1, &st,
                              NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    sqlite3_close(plain);

    unlink(path);
}

int main(void) {
    test_compare_order();
    test_compare_uses_lengths();
    test_lists();
    printf("title-order-test: ok\n");
    return 0;
}
