#include "internal/db/db.h"
#include "internal/discovery/discovery.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Runs real scans against temp directories. A non-MLP1 build treats a directory as
   mounted, and JAWAKA_TEST_CARD_KEY_secondary_sd names the card in the slot. Each case
   gets a new library and new cards. */

#define KEY_ENV "JAWAKA_TEST_CARD_KEY_secondary_sd"
#define KEY_A "fat:aaaa0001"
#define KEY_B "fat:bbbb0002"
#define KEY_C "fat:cccc0003"

static char g_root[256];
static char g_primary[300];
static char g_slot[300];
static char g_db_path[300];
static sqlite3 *g_db;

static void run(const char *fmt, const char *a, const char *b) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), fmt, a, b);
    assert(system(cmd) == 0);
}

static void exec_ok(const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "%s\n", error ? error : sqlite3_errmsg(g_db));
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}

static int scalar(const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, sqlite3_errmsg(g_db));
        abort();
    }
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static void card_path(const char *card, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/cards/%s", g_root, card);
}

/* Adds files first..first+count-1 to a card that stays out of the slot. */
static void add_games(const char *card, int first, int count) {
    char path[400], dir[500];
    card_path(card, path, sizeof(path));
    snprintf(dir, sizeof(dir), "%s/Roms/NES", path);
    run("mkdir -p '%s'%s", dir, "");
    for (int i = first; i < first + count; i++) {
        char file[600];
        snprintf(file, sizeof(file), "%s/g%03d.nes", dir, i);
        FILE *f = fopen(file, "w");
        assert(f);
        fclose(f);
    }
}

static void add_empty_card(const char *card, bool with_roms) {
    char path[400];
    card_path(card, path, sizeof(path));
    run(with_roms ? "mkdir -p '%s/Roms'%s" : "mkdir -p '%s'%s", path, "");
}

static void insert_card(const char *card, const char *key) {
    char path[400];
    card_path(card, path, sizeof(path));
    assert(rename(path, g_slot) == 0);
    if (key) setenv(KEY_ENV, key, 1); else unsetenv(KEY_ENV);
}

static void remove_card(const char *card) {
    char path[400];
    card_path(card, path, sizeof(path));
    assert(rename(g_slot, path) == 0);
    unsetenv(KEY_ENV);
}

static void scan(void) {
    jw_scan_result result;
    assert(jw_scan_library(g_db, g_primary, &result) == 0);
}

static void swap_to(const char *from, const char *to, const char *key) {
    remove_card(from);
    insert_card(to, key);
    scan();
}

static void begin_case(void) {
    if (g_db) jw_db_close(g_db);
    unsetenv(KEY_ENV);
    run("rm -rf '%s/cards' '%s'", g_root, g_slot);
    run("rm -f '%s' '%s-wal'", g_db_path, g_db_path);
    run("mkdir -p '%s/cards' '%s/Roms'", g_root, g_primary);
    assert(jw_db_open(g_db_path, &g_db) == 0);
    assert(jw_db_apply_schema(g_db) == 0);
    /* An idle slot still exists as a mount point. */
    assert(mkdir(g_slot, 0755) == 0);
}

/* Inserting a card replaces the empty mount point. */
static void insert_first(const char *card, const char *key) {
    assert(rmdir(g_slot) == 0);
    insert_card(card, key);
}

static int count_live(void) {
    return scalar("SELECT COUNT(*) FROM games WHERE source_id='secondary_sd'");
}

static int count_parked(void) {
    return scalar("SELECT COUNT(*) FROM games WHERE source_id LIKE 'card-%'");
}

static int count_cards(void) {
    return scalar("SELECT COUNT(*) FROM library_cards");
}

static bool owner_is(const char *key) {
    char sql[128];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM library_cards WHERE is_live=1 AND card_key='%s'", key);
    return scalar(sql) == 1;
}

static int listed_for_nes(void) {
    jw_game_entry games[128];
    int count = 0;
    assert(jw_db_list_games_for_system(g_db_path, "NES", games, 128, &count) == 0);
    return count;
}

static int game_id(const char *source, int number) {
    char sql[160];
    snprintf(sql, sizeof(sql),
             "SELECT COALESCE((SELECT id FROM games WHERE source_id='%s' "
             "AND rom_relpath='NES/g%03d.nes'),0)", source, number);
    return scalar(sql);
}

static void test_swap_and_return_keeps_preferences(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("B", 100, 2);
    insert_first("A", KEY_A);
    scan();
    assert(owner_is(KEY_A) && count_live() == 3);

    int kept = game_id("secondary_sd", 1);
    int hidden_game = game_id("secondary_sd", 2);
    char focus[32];
    snprintf(focus, sizeof(focus), "%d", kept);
    assert(jw_db_set_favorite(g_db_path, "game", kept, 1) == 0);
    assert(jw_db_record_play_by_id(g_db_path, kept, 75) == 0);
    assert(jw_db_set_game_setting(g_db_path, kept, "core_id", "fceumm") == 0);
    assert(jw_db_set_game_hidden(g_db_path, hidden_game, 1) == 0);
    assert(jw_db_set_setting(g_db_path, "five_game_ids", focus) == 0);

    swap_to("A", "B", KEY_B);

    /* B's first scan deleted nothing of A's. */
    assert(owner_is(KEY_B) && count_live() == 2 && count_parked() == 3);
    assert(listed_for_nes() == 2);
    jw_game_entry favorites[4];
    int count = -1;
    assert(jw_db_list_favorite_games(g_db_path, favorites, 4, &count) == 0 && count == 0);
    assert(jw_db_list_recent_games(g_db_path, favorites, 4, &count) == 0 && count == 0);
    /* Parked rows can't be reached by path, and nothing shows a raw card-<n>. */
    assert(scalar("SELECT COUNT(*) FROM games WHERE source_id LIKE 'card-%' "
                  "AND rom_path LIKE '/.leaf-parked/card-%' AND image_path IS NULL") == 3);
    assert(jw_db_count_hidden_games(g_db_path, &count) == 0 && count == 0);
    jw_library_stats stats;
    assert(jw_db_read_stats(g_db_path, &stats) == 0);
    assert(stats.game_count == 2 && stats.favorite_count == 0 && stats.games_played == 0);
    assert(stats.total_playtime_s == 75);

    /* A hidden key follows its card rather than hiding B's copy of the path. */
    assert(scalar("SELECT COUNT(*) FROM hidden_roms WHERE source_id='secondary_sd'") == 0);

    swap_to("B", "A", KEY_A);

    assert(owner_is(KEY_A) && count_live() == 3);
    assert(count_parked() == 2);
    assert(game_id("secondary_sd", 1) == kept && game_id("secondary_sd", 2) == hidden_game);
    jw_game_entry game;
    assert(jw_db_get_game_by_id(g_db_path, kept, &game) == 0);
    assert(game.favorite == 1 && game.playtime_s == 75);
    assert(strncmp(game.rom_path, g_slot, strlen(g_slot)) == 0);
    char core[32] = "";
    assert(jw_db_get_game_setting(g_db_path, kept, "core_id", core, sizeof(core)) == 0);
    assert(strcmp(core, "fceumm") == 0);
    assert(jw_db_count_hidden_games(g_db_path, &count) == 0 && count == 1);
    int is_hidden = 0;
    assert(jw_db_is_game_hidden(g_db_path, hidden_game, &is_hidden) == 0 && is_hidden == 1);
    char saved[32] = "";
    assert(jw_db_get_setting(g_db_path, "five_game_ids", saved, sizeof(saved)) == 0);
    assert(strcmp(saved, focus) == 0);
    remove_card("A");
}

static void test_same_path_on_two_cards_stays_separate(void) {
    begin_case();
    /* Only one shared path among different games: below adoption by a wide margin. */
    add_games("A", 0, 20);
    add_games("B", 0, 1);
    add_games("B", 200, 19);
    insert_first("A", KEY_A);
    scan();
    int a_row = game_id("secondary_sd", 0);
    assert(jw_db_record_play_by_id(g_db_path, a_row, 40) == 0);

    swap_to("A", "B", KEY_B);
    int b_row = game_id("secondary_sd", 0);
    assert(b_row != 0 && b_row != a_row);
    jw_game_entry game;
    assert(jw_db_get_game_by_id(g_db_path, b_row, &game) == 0 && game.playtime_s == 0);
    assert(jw_db_get_game_by_id(g_db_path, a_row, &game) == 0 && game.playtime_s == 40);

    char path[400];
    snprintf(path, sizeof(path), "%s/Roms/NES/g000.nes", g_slot);
    assert(jw_db_get_game_by_rom_path(g_db_path, path, &game) == 0 && game.id == b_row);
    remove_card("B");
}

static void test_single_shared_game_cards_stay_separate(void) {
    begin_case();
    add_games("A", 0, 1);
    add_games("B", 0, 1);
    insert_first("A", KEY_A);
    scan();
    int a_row = game_id("secondary_sd", 0);
    swap_to("A", "B", KEY_B);
    assert(count_parked() == 1 && count_live() == 1);
    assert(game_id("secondary_sd", 0) != a_row);
    remove_card("B");
}

static void test_nearly_identical_cards_share_rows(void) {
    begin_case();
    add_games("A", 0, 20);
    add_games("B", 0, 19);
    add_games("B", 300, 1);
    insert_first("A", KEY_A);
    scan();
    swap_to("A", "B", KEY_B);
    /* This pins the narrowed promise: one set of rows, by design. */
    assert(count_parked() == 0 && count_cards() == 1 && owner_is(KEY_B));
    assert(count_live() == 20);
    remove_card("B");
}

static void test_library_without_a_registry_is_claimed(void) {
    begin_case();
    add_games("A", 0, 3);
    /* No key: today's slot behavior writes the rows and records nothing. */
    insert_first("A", NULL);
    scan();
    assert(count_cards() == 0 && count_live() == 3);
    exec_ok("CREATE TEMP TABLE before_rows AS SELECT id,source_id,rom_path FROM games;");

    setenv(KEY_ENV, KEY_A, 1);
    jw_cards_outcome outcome = JW_CARDS_UNCHANGED;
    assert(jw_db_cards_reconcile_root(g_db, g_primary, &outcome) == 0);
    assert(outcome == JW_CARDS_CLAIMED && owner_is(KEY_A));
    assert(scalar("SELECT COUNT(*) FROM games g JOIN before_rows b ON g.id=b.id "
                  "AND g.source_id=b.source_id AND g.rom_path=b.rom_path") == 3);
    assert(scalar("PRAGMA user_version") == 6);
    assert(jw_db_cards_reconcile_root(g_db, g_primary, &outcome) == 0);
    assert(outcome == JW_CARDS_UNCHANGED);
    remove_card("A");
    exec_ok("DROP TABLE before_rows;");
}

static void test_no_card_at_first_run_then_a_card_claims(void) {
    begin_case();
    scan();
    assert(count_cards() == 0);
    add_games("A", 0, 2);
    insert_first("A", KEY_A);
    scan();
    assert(owner_is(KEY_A) && count_live() == 2);
    remove_card("A");
}

static void test_reformat_and_bigger_card_are_adopted(void) {
    begin_case();
    add_games("A", 0, 20);
    insert_first("A", KEY_A);
    scan();
    int kept = game_id("secondary_sd", 5);
    assert(jw_db_record_play_by_id(g_db_path, kept, 30) == 0);
    remove_card("A");

    /* The same files under a new serial. */
    insert_card("A", KEY_B);
    scan();
    assert(owner_is(KEY_B) && count_cards() == 1 && count_parked() == 0);
    assert(game_id("secondary_sd", 5) == kept);
    remove_card("A");

    /* A bigger card holding all of them, plus more. */
    add_games("A", 500, 5);
    insert_card("A", KEY_C);
    scan();
    assert(owner_is(KEY_C) && count_cards() == 1 && count_parked() == 0);
    assert(count_live() == 25 && game_id("secondary_sd", 5) == kept);
    remove_card("A");
}

static void test_too_little_overlap_is_a_new_card(void) {
    begin_case();
    /* 17 of 20 is 85%. */
    add_games("A", 0, 20);
    add_games("B", 0, 17);
    add_games("B", 400, 3);
    insert_first("A", KEY_A);
    scan();
    swap_to("A", "B", KEY_B);
    assert(owner_is(KEY_B) && count_cards() == 2 && count_parked() == 20);
    remove_card("B");

    /* All present, but 15 rows is below the floor. */
    begin_case();
    add_games("A", 0, 15);
    add_games("B", 0, 15);
    insert_first("A", KEY_A);
    scan();
    swap_to("A", "B", KEY_B);
    assert(count_cards() == 2 && count_parked() == 15);
    remove_card("B");
}

static void test_unknown_key_can_adopt_a_parked_card(void) {
    begin_case();
    add_games("A", 0, 20);
    add_games("B", 200, 5);
    insert_first("A", KEY_A);
    scan();
    int kept = game_id("secondary_sd", 3);
    swap_to("A", "B", KEY_B);
    assert(owner_is(KEY_B) && game_id("secondary_sd", 3) == 0);
    /* A again, reformatted: its files match the parked set, not the owner. */
    swap_to("B", "A", KEY_C);
    assert(owner_is(KEY_C) && count_cards() == 2);
    assert(game_id("secondary_sd", 3) == kept);
    assert(count_parked() == 5);
    remove_card("A");
}

static void test_empty_and_missing_roms(void) {
    begin_case();
    add_games("A", 0, 3);
    add_empty_card("empty", true);
    add_empty_card("bare", false);
    insert_first("A", KEY_A);
    scan();
    int kept = game_id("secondary_sd", 0);

    /* An empty Roms folder is a new card: it parks, and nothing is deleted. */
    swap_to("A", "empty", KEY_B);
    assert(owner_is(KEY_B) && count_live() == 0 && count_parked() == 3);

    /* No Roms folder isn't taking the slot. */
    swap_to("empty", "bare", KEY_C);
    assert(owner_is(KEY_B) && count_parked() == 3);

    swap_to("bare", "A", KEY_A);
    assert(owner_is(KEY_A) && count_live() == 3 && game_id("secondary_sd", 0) == kept);
    /* The empty card held nothing, so it left the registry. */
    assert(count_cards() == 1);
    remove_card("A");
}

static void test_no_roms_folder_on_a_new_card_keeps_the_owner(void) {
    begin_case();
    add_games("A", 0, 3);
    add_empty_card("bare", false);
    insert_first("A", KEY_A);
    scan();
    swap_to("A", "bare", KEY_B);
    assert(owner_is(KEY_A) && count_live() == 3 && count_parked() == 0);
    /* Hidden, because the mounted card has nothing that can launch. */
    assert(listed_for_nes() == 0);
    remove_card("bare");
}

static void test_unreadable_key_changes_nothing(void) {
    begin_case();
    add_games("A", 0, 3);
    insert_first("A", KEY_A);
    scan();
    int before = game_id("secondary_sd", 0);
    remove_card("A");
    add_games("A", 50, 1);

    insert_card("A", "unreadable");
    scan();
    /* The slot wasn't scanned: no prune, and the new file gets no row. */
    assert(owner_is(KEY_A) && count_live() == 3 && count_parked() == 0);
    assert(game_id("secondary_sd", 0) == before && game_id("secondary_sd", 50) == 0);
    /* Nothing hidden either: a game shown but refused is the worse failure. */
    assert(listed_for_nes() == 3);
    jw_game_entry game;
    assert(jw_db_get_game_by_id(g_db_path, before, &game) == 0);
    remove_card("A");
}

static void test_no_key_keeps_slot_behavior(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("B", 100, 2);
    insert_first("A", NULL);
    scan();
    assert(count_live() == 3);
    swap_to("A", "B", NULL);
    assert(count_cards() == 0 && count_parked() == 0 && count_live() == 2);
    assert(game_id("secondary_sd", 0) == 0);
    remove_card("B");
}

/* Fires during a scan's writes, the way a card pulled at that moment would. */
static int g_change_after;
static const char *g_change_table;
static const char *g_change_key;
static int g_rows_written;
static void change_key_hook(void *data, int op, const char *db, const char *table,
                            sqlite3_int64 rowid) {
    (void)data; (void)db; (void)rowid;
    if (op != SQLITE_INSERT || strcmp(table, g_change_table) != 0) return;
    g_rows_written++;
    if (g_rows_written == g_change_after) setenv(KEY_ENV, g_change_key, 1);
}

static void test_card_changes_mid_scan(void) {
    begin_case();
    add_games("A", 0, 3);
    insert_first("A", KEY_A);
    scan();
    exec_ok("CREATE TEMP TABLE before_rows AS SELECT id,source_id,rom_relpath,rom_path FROM games;");
    /* Past a 64-write batch, with rows that aren't on the owner. */
    remove_card("A");
    add_games("A", 100, 80);
    insert_card("A", KEY_A);

    g_change_after = 70;
    g_change_table = "_scan_staged_games";
    g_change_key = KEY_B;
    g_rows_written = 0;
    sqlite3_update_hook(g_db, change_key_hook, NULL);
    scan();
    sqlite3_update_hook(g_db, NULL, NULL);
    assert(g_rows_written == 83);

    /* Existing rows byte-identical, no row from the other card, nothing pruned. */
    assert(count_live() == 3);
    assert(scalar("SELECT COUNT(*) FROM games g JOIN before_rows b ON g.id=b.id "
                  "AND g.source_id=b.source_id AND g.rom_relpath=b.rom_relpath "
                  "AND g.rom_path=b.rom_path") == 3);
    exec_ok("DROP TABLE before_rows;");
    remove_card("A");
}

static void test_card_changes_during_apply(void) {
    begin_case();
    add_games("A", 0, 3);
    insert_first("A", KEY_A);
    scan();
    remove_card("A");
    /* g000 vanishes, so a completed scan would prune it. */
    char gone[400];
    card_path("A", gone, sizeof(gone));
    run("rm -f '%s/Roms/NES/g000.nes'%s", gone, "");
    add_games("A", 100, 4);
    insert_card("A", KEY_A);

    g_change_after = 2;
    g_change_table = "games";
    g_change_key = KEY_B;
    g_rows_written = 0;
    sqlite3_update_hook(g_db, change_key_hook, NULL);
    scan();
    sqlite3_update_hook(g_db, NULL, NULL);

    /* What was applied is the owner's; the card left, so nothing is pruned. */
    assert(game_id("secondary_sd", 0) != 0);
    assert(count_live() == 3 + 4);
    remove_card("A");
}

static int g_staged_rows;
static void count_staged_hook(void *data, int op, const char *db, const char *table,
                              sqlite3_int64 rowid) {
    (void)data; (void)op; (void)db; (void)rowid;
    if (strcmp(table, "_scan_staged_games") == 0) g_staged_rows++;
}

static void test_only_a_keyed_second_card_is_staged(void) {
    begin_case();
    char roms[400];
    snprintf(roms, sizeof(roms), "%s/Roms/NES", g_primary);
    run("mkdir -p '%s'%s", roms, "");
    run("touch '%s/primary.nes'%s", roms, "");
    add_games("A", 0, 2);
    insert_first("A", NULL);

    g_staged_rows = 0;
    sqlite3_update_hook(g_db, count_staged_hook, NULL);
    scan();
    sqlite3_update_hook(g_db, NULL, NULL);
    assert(g_staged_rows == 0 && count_live() == 2);
    assert(scalar("SELECT COUNT(*) FROM games WHERE source_id='primary'") == 1);
    run("rm -f '%s/primary.nes'%s", roms, "");
    remove_card("A");
}

static void test_open_relocation_defers(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("B", 100, 2);
    insert_first("A", KEY_A);
    scan();
    int game = game_id("secondary_sd", 0);
    exec_ok("INSERT INTO library_relocation_ops(operation_id,state,expected_generation,"
            "mapping_generation,scan_ticket_generation,item_count,request_fingerprint,updated_at)"
            " VALUES('op','prepared',1,0,0,1,'',1);");
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO library_relocation_items VALUES('op',0,%d,"
             "'secondary_sd','NES/g000.nes','','','x','','primary','NES/g000.nes','','','y','','s','t');",
             game);
    exec_ok(sql);

    swap_to("A", "B", KEY_B);
    /* Rows stay, hidden, and the mismatched slot isn't scanned or pruned. */
    assert(owner_is(KEY_A) && count_parked() == 0 && count_live() == 3);
    assert(listed_for_nes() == 0);

    exec_ok("DELETE FROM library_relocation_ops WHERE operation_id='op';");
    scan();
    assert(owner_is(KEY_B) && count_parked() == 3 && count_live() == 2);
    remove_card("B");
}

static void test_oldest_card_is_evicted_past_the_cap(void) {
    begin_case();
    char name[16], key[32], previous[16] = "";
    int favorite_row = 0;
    for (int i = 0; i < JW_CARDS_MAX_PARKED + 2; i++) {
        snprintf(name, sizeof(name), "c%d", i);
        snprintf(key, sizeof(key), "fat:0000%04x", i + 1);
        add_games(name, i * 10, 1);
        if (i == 0) {
            insert_first(name, key);
            scan();
            favorite_row = game_id("secondary_sd", 0);
            assert(jw_db_set_favorite(g_db_path, "game", favorite_row, 1) == 0);
        } else {
            swap_to(previous, name, key);
        }
        /* Ordering relies on last_seen, so make the seconds differ. */
        exec_ok("UPDATE library_cards SET last_seen=last_seen-1000 WHERE is_live=0;");
        snprintf(previous, sizeof(previous), "%s", name);
    }
    assert(count_cards() == JW_CARDS_MAX_PARKED + 1);
    assert(scalar("SELECT COUNT(*) FROM library_cards WHERE is_live=0") == JW_CARDS_MAX_PARKED);
    assert(count_parked() == JW_CARDS_MAX_PARKED);
    /* The first card and its orphans are gone. The others are untouched. */
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM games WHERE id=%d", favorite_row);
    assert(scalar(sql) == 0);
    assert(scalar("SELECT COUNT(*) FROM favorites WHERE kind='game'") == 0);
    assert(scalar("SELECT COUNT(*) FROM library_cards WHERE card_key='fat:00000001'") == 0);
    remove_card(previous);
}

static void test_failure_rolls_back(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("B", 100, 2);
    insert_first("A", KEY_A);
    scan();
    exec_ok("CREATE TEMP TRIGGER fail_card BEFORE INSERT ON main.library_cards "
            "BEGIN SELECT RAISE(ABORT,'simulated'); END;");
    remove_card("A");
    insert_card("B", KEY_B);
    jw_cards_outcome outcome = JW_CARDS_UNCHANGED;
    assert(jw_db_cards_reconcile_root(g_db, g_primary, &outcome) != 0);
    /* Back to the previous owner and rows. */
    assert(owner_is(KEY_A) && count_cards() == 1 && count_live() == 3 && count_parked() == 0);
    assert(scalar("SELECT COUNT(*) FROM games WHERE rom_path LIKE '/.leaf-parked/%'") == 0);
    assert(sqlite3_get_autocommit(g_db) != 0);
    /* The scan then retains the slot. */
    scan();
    assert(count_live() == 3 && game_id("secondary_sd", 100) == 0);
    exec_ok("DROP TRIGGER fail_card;");
    scan();
    assert(owner_is(KEY_B) && count_parked() == 3 && count_live() == 2);
    remove_card("B");
}

static void test_reconcile_inside_a_callers_transaction(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("B", 100, 2);
    insert_first("A", KEY_A);
    scan();

    /* Pak Rat scans inside its own BEGIN IMMEDIATE. */
    remove_card("A");
    insert_card("B", KEY_B);
    exec_ok("BEGIN IMMEDIATE;");
    scan();
    assert(sqlite3_get_autocommit(g_db) == 0);
    assert(owner_is(KEY_B) && count_parked() == 3);
    exec_ok("ROLLBACK;");
    assert(owner_is(KEY_A) && count_live() == 3 && count_parked() == 0);

    exec_ok("BEGIN IMMEDIATE;");
    scan();
    exec_ok("COMMIT;");
    assert(owner_is(KEY_B) && count_parked() == 3 && count_live() == 2);
    remove_card("B");
}

/* What an older build does when it scans a card under the slot: it prunes whatever
   isn't there and writes that card's rows, ignoring library_cards. */
static void older_build_scan(int first, int count, int playtime) {
    exec_ok("DELETE FROM games WHERE source_id='secondary_sd';");
    for (int i = first; i < first + count; i++) {
        char sql[512];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO games(system,name,source_id,rom_relpath,rom_path,playtime_s) "
                 "VALUES('NES','g%03d','secondary_sd','NES/g%03d.nes','/x/g%03d.nes',%d);",
                 i, i, i, playtime);
        exec_ok(sql);
    }
}

static void test_older_build_wrote_a_parked_cards_rows(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("B", 100, 2);
    insert_first("A", KEY_A);
    scan();
    swap_to("A", "B", KEY_B);
    int old_b = game_id("secondary_sd", 100);
    assert(jw_db_record_play_by_id(g_db_path, old_b, 10) == 0);
    assert(jw_db_set_favorite(g_db_path, "game", old_b, 1) == 0);
    assert(jw_db_set_game_setting(g_db_path, old_b, "core_id", "fceumm") == 0);
    /* Back to A, so B is parked with one extra row only it has. */
    swap_to("B", "A", KEY_A);
    assert(owner_is(KEY_A));
    char parked_extra[256];
    int b_card = scalar("SELECT id FROM library_cards WHERE card_key='" KEY_B "'");
    snprintf(parked_extra, sizeof(parked_extra),
             "INSERT INTO games(system,name,source_id,rom_relpath,rom_path) VALUES"
             "('NES','g199','card-%d','NES/g199.nes','/.leaf-parked/x');", b_card);
    exec_ok(parked_extra);

    /* An older build then scans B and plays it. */
    older_build_scan(100, 2, 500);

    remove_card("A");
    add_games("B", 199, 1);
    insert_card("B", KEY_B);
    jw_cards_outcome outcome = JW_CARDS_UNCHANGED;
    assert(jw_db_cards_reconcile_root(g_db, g_primary, &outcome) == 0);
    assert(outcome == JW_CARDS_RECOVERED);

    /* B's newer progress stays live and takes the parked row's preferences. Nothing
       was parked under A, whose rows the older build had already pruned. */
    assert(owner_is(KEY_B) && count_parked() == 0 && count_cards() == 1);
    int live_b = game_id("secondary_sd", 100);
    jw_game_entry game;
    assert(jw_db_get_game_by_id(g_db_path, live_b, &game) == 0);
    assert(game.playtime_s == 510 && game.favorite == 1);
    char core[32] = "";
    assert(jw_db_get_game_setting(g_db_path, live_b, "core_id", core, sizeof(core)) == 0);
    assert(strcmp(core, "fceumm") == 0);
    assert(game_id("secondary_sd", 199) != 0);
    remove_card("B");
}

static void test_older_build_wrote_a_new_cards_rows(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("C", 300, 2);
    insert_first("A", KEY_A);
    scan();
    older_build_scan(300, 2, 90);
    remove_card("A");
    insert_card("C", KEY_C);
    jw_cards_outcome outcome = JW_CARDS_UNCHANGED;
    assert(jw_db_cards_reconcile_root(g_db, g_primary, &outcome) == 0);
    assert(outcome == JW_CARDS_RECOVERED);
    assert(owner_is(KEY_C) && count_cards() == 1 && count_parked() == 0 && count_live() == 2);
    remove_card("C");
}

static void test_older_build_rows_not_on_the_mounted_card(void) {
    begin_case();
    add_games("A", 0, 3);
    add_games("B", 100, 2);
    add_games("C", 300, 2);
    insert_first("A", KEY_A);
    scan();
    swap_to("A", "B", KEY_B);
    swap_to("B", "A", KEY_A);
    older_build_scan(300, 2, 90);

    /* The rows are C's, but B is mounted: the normal branch runs. */
    remove_card("A");
    insert_card("B", KEY_B);
    jw_cards_outcome outcome = JW_CARDS_UNCHANGED;
    assert(jw_db_cards_reconcile_root(g_db, g_primary, &outcome) == 0);
    assert(outcome == JW_CARDS_SWAPPED);
    assert(owner_is(KEY_B) && count_live() == 2 && game_id("secondary_sd", 100) != 0);
    remove_card("B");
}

static void test_normal_swap_never_merges(void) {
    begin_case();
    /* Overlapping paths and a current digest: separate rows, no merge. */
    add_games("A", 0, 5);
    add_games("B", 0, 5);
    insert_first("A", KEY_A);
    scan();
    int a_row = game_id("secondary_sd", 0);
    assert(jw_db_record_play_by_id(g_db_path, a_row, 60) == 0);
    swap_to("A", "B", KEY_B);
    int b_row = game_id("secondary_sd", 0);
    assert(jw_db_record_play_by_id(g_db_path, b_row, 5) == 0);
    swap_to("B", "A", KEY_A);
    assert(game_id("secondary_sd", 0) == a_row);
    jw_game_entry game;
    assert(jw_db_get_game_by_id(g_db_path, a_row, &game) == 0 && game.playtime_s == 60);
    assert(jw_db_get_game_by_id(g_db_path, b_row, &game) == 0 && game.playtime_s == 5);
    remove_card("A");
}

static void test_owner_check_and_listing(void) {
    begin_case();
    add_games("A", 0, 2);
    add_games("B", 100, 2);
    insert_first("A", KEY_A);
    scan();
    int a_row = game_id("secondary_sd", 0);
    jw_storage_source_list *sources = malloc(sizeof(*sources));
    assert(sources);
    const jw_storage_source *second;

    assert(jw_storage_sources_resolve(g_primary, sources) == 0);
    second = jw_storage_sources_find_by_id(sources, "secondary_sd");
    assert(jw_db_card_owner_check_at(g_db_path, second) == JW_CARD_OWNER_MATCH);
    assert(jw_db_card_owner_check_at(g_db_path, &sources->sources[0]) == JW_CARD_OWNER_UNKNOWN);
    bool listed = false;
    assert(jw_db_game_listed(g_db_path, a_row, &listed) == 0 && listed);

    /* Mismatch with the rows still under the slot, as while a reconcile is deferred
       or the library card is read-only: lists hide them, launch and delete refuse. */
    remove_card("A");
    insert_card("B", KEY_B);
    assert(jw_storage_sources_resolve(g_primary, sources) == 0);
    second = jw_storage_sources_find_by_id(sources, "secondary_sd");
    assert(jw_db_card_owner_check_at(g_db_path, second) == JW_CARD_OWNER_MISMATCH);
    assert(jw_db_game_listed(g_db_path, a_row, &listed) == 0 && !listed);
    assert(listed_for_nes() == 0);
    jw_search_result search[8];
    int count = -1;
    assert(jw_db_search_library(g_db_path, "g000", search, 8, &count) == 0 && count == 0);
    /* A preference question and an identity lookup don't depend on the card. */
    int hidden = -1;
    assert(jw_db_is_game_hidden(g_db_path, a_row, &hidden) == 0 && hidden == 0);
    jw_game_entry game;
    assert(jw_db_get_game_by_id(g_db_path, a_row, &game) == 0);

    /* Unreadable and no key both allow: a game shown but refused is the worse bug. */
    setenv(KEY_ENV, "unreadable", 1);
    assert(jw_storage_sources_resolve(g_primary, sources) == 0);
    second = jw_storage_sources_find_by_id(sources, "secondary_sd");
    assert(jw_db_card_owner_check_at(g_db_path, second) == JW_CARD_OWNER_UNKNOWN);
    assert(jw_db_game_listed(g_db_path, a_row, &listed) == 0 && listed);
    unsetenv(KEY_ENV);
    assert(jw_storage_sources_resolve(g_primary, sources) == 0);
    second = jw_storage_sources_find_by_id(sources, "secondary_sd");
    assert(jw_db_card_owner_check_at(g_db_path, second) == JW_CARD_OWNER_UNKNOWN);

    /* No registry row at all: nothing to compare against. */
    setenv(KEY_ENV, KEY_B, 1);
    exec_ok("DELETE FROM library_cards;");
    assert(jw_storage_sources_resolve(g_primary, sources) == 0);
    second = jw_storage_sources_find_by_id(sources, "secondary_sd");
    assert(jw_db_card_owner_check_at(g_db_path, second) == JW_CARD_OWNER_UNKNOWN);
    assert(jw_db_game_listed(g_db_path, a_row, &listed) == 0 && listed);
    /* A parked row hides without any of that. */
    exec_ok("UPDATE games SET source_id='card-9' WHERE source_id='secondary_sd';");
    assert(jw_db_game_listed(g_db_path, a_row, &listed) == 0 && !listed);
    free(sources);
    remove_card("B");
}

int main(void) {
    char temp[] = "/tmp/jw-card-identity-XXXXXX";
    assert(mkdtemp(temp));
    char resolved[PATH_MAX];
    assert(realpath(temp, resolved) && strlen(resolved) < sizeof(g_root));
    snprintf(g_root, sizeof(g_root), "%s", resolved);
    snprintf(g_primary, sizeof(g_primary), "%s/primary", g_root);
    snprintf(g_slot, sizeof(g_slot), "%s/slot", g_root);
    snprintf(g_db_path, sizeof(g_db_path), "%s/library.db", g_root);
    char roots[700];
    snprintf(roots, sizeof(roots), "%s:%s", g_primary, g_slot);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("JAWAKA_DISABLE_RETROARCH_V2", "1", 1);

    test_swap_and_return_keeps_preferences();
    test_same_path_on_two_cards_stays_separate();
    test_single_shared_game_cards_stay_separate();
    test_nearly_identical_cards_share_rows();
    test_library_without_a_registry_is_claimed();
    test_no_card_at_first_run_then_a_card_claims();
    test_reformat_and_bigger_card_are_adopted();
    test_too_little_overlap_is_a_new_card();
    test_unknown_key_can_adopt_a_parked_card();
    test_empty_and_missing_roms();
    test_no_roms_folder_on_a_new_card_keeps_the_owner();
    test_unreadable_key_changes_nothing();
    test_no_key_keeps_slot_behavior();
    test_card_changes_mid_scan();
    test_card_changes_during_apply();
    test_only_a_keyed_second_card_is_staged();
    test_open_relocation_defers();
    test_oldest_card_is_evicted_past_the_cap();
    test_failure_rolls_back();
    test_reconcile_inside_a_callers_transaction();
    test_older_build_wrote_a_parked_cards_rows();
    test_older_build_wrote_a_new_cards_rows();
    test_older_build_rows_not_on_the_mounted_card();
    test_normal_swap_never_merges();
    test_owner_check_and_listing();

    jw_db_close(g_db);
    run("rm -rf '%s'%s", g_root, "");
    puts("card identity tests passed");
    return 0;
}
