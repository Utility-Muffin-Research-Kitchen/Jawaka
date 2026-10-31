#include "internal/db/db.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void expect_synopsis(const char *path, int game_id, const char *want) {
    jw_game_meta meta;
    assert(jw_db_get_game_meta(path, game_id, &meta) == 0);
    if (strcmp(meta.synopsis, want) != 0) {
        fprintf(stderr, "game-meta-test: synopsis \"%s\", want \"%s\"\n", meta.synopsis, want);
        assert(0);
    }
}

static void scrape(const char *path, const char *rom_path, const char *synopsis) {
    jw_game_meta meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.genre, sizeof(meta.genre), "%s", "Platform");
    snprintf(meta.synopsis, sizeof(meta.synopsis), "%s", synopsis);
    assert(jw_db_set_game_meta(path, rom_path, &meta) == 0);
}

int main(void) {
    char path[] = "/tmp/jawaka-game-meta.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);

    const char *rom = "Roms/SFC/Game.sfc";
    sqlite3 *db = NULL;
    assert(jw_db_open(path, &db) == 0 && jw_db_apply_schema(db) == 0);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game(db, "SFC", "Game", rom, NULL) == 0);
    jw_db_close(db);

    jw_game_entry game;
    assert(jw_db_get_game_by_rom_path(path, rom, &game) == 0);

    /* No override: the scraped description, with its other facts. */
    scrape(path, rom, "Scraped");
    expect_synopsis(path, game.id, "Scraped");

    /* An override wins, and leaves the other scraped facts in place. */
    assert(jw_db_set_game_setting(path, game.id, JW_GAME_SETTING_USER_SYNOPSIS, "Mine") == 0);
    expect_synopsis(path, game.id, "Mine");
    jw_game_meta meta;
    assert(jw_db_get_game_meta(path, game.id, &meta) == 0);
    assert(strcmp(meta.genre, "Platform") == 0);

    /* A rescrape updates its own value underneath and never touches the override. */
    scrape(path, rom, "Rescraped");
    expect_synopsis(path, game.id, "Mine");

    /* The info box can't draw a line break. */
    assert(jw_db_set_game_setting(path, game.id, JW_GAME_SETTING_USER_SYNOPSIS,
                                  "First line\n\nSecond line") == 0);
    expect_synopsis(path, game.id, "First line Second line");

    /* Nothing left after cleaning means no override. */
    assert(jw_db_set_game_setting(path, game.id, JW_GAME_SETTING_USER_SYNOPSIS, " \n ") == 0);
    expect_synopsis(path, game.id, "Rescraped");

    /* Reset brings back what the last scrape stored. */
    assert(jw_db_set_game_setting(path, game.id, JW_GAME_SETTING_USER_SYNOPSIS, "Mine") == 0);
    assert(jw_db_delete_game_setting(path, game.id, JW_GAME_SETTING_USER_SYNOPSIS) == 0);
    expect_synopsis(path, game.id, "Rescraped");

    unlink(path);
    printf("game-meta-test: ok\n");
    return 0;
}
