#include "internal/storage/sources.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char temp[] = "/tmp/jw-sources-XXXXXX";
    assert(mkdtemp(temp));
    char card1[JW_STORAGE_PATH_MAX];
    char card2[JW_STORAGE_PATH_MAX];
    char roms[JW_STORAGE_PATH_MAX];
    char game[JW_STORAGE_PATH_MAX];
    snprintf(card1, sizeof(card1), "%s/card1", temp);
    snprintf(card2, sizeof(card2), "%s/card2", temp);
    assert(mkdir(card1, 0700) == 0);
    assert(mkdir(card2, 0700) == 0);
    snprintf(roms, sizeof(roms), "%s/Roms", card2);
    assert(mkdir(roms, 0700) == 0);
    snprintf(game, sizeof(game), "%s/game.zip", roms);
    FILE *file = fopen(game, "wb");
    assert(file);
    assert(fputs("rom", file) >= 0);
    assert(fclose(file) == 0);

    char roots[JW_STORAGE_PATH_MAX * 2];
    char music[JW_STORAGE_PATH_MAX * 2];
    snprintf(roots, sizeof(roots), "%s:%s", card1, card2);
    snprintf(music, sizeof(music), "%s/music1:%s/music2", temp, temp);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("SDCARD_PATH", card1, 1);
    setenv("MUSIC_PATHS", music, 1);

    jw_storage_source_list sources;
    assert(jw_storage_sources_resolve(card1, &sources) == 0);
    assert(sources.count == 2);
    assert(strcmp(sources.sources[0].id, "primary") == 0);
    assert(sources.sources[0].available);
    assert(strcmp(sources.sources[1].id, "secondary_sd") == 0);
    assert(sources.sources[1].available);
    char expected[JW_STORAGE_PATH_MAX];
    snprintf(expected, sizeof(expected), "%s/music2", temp);
    assert(strcmp(sources.sources[1].music_path, expected) == 0);

    char resolved[JW_STORAGE_PATH_MAX];
    assert(jw_storage_resolve_rom(&sources.sources[1], "game.zip", true,
                                  resolved, sizeof(resolved)) == 0);
    char expected_game[JW_STORAGE_PATH_MAX];
    assert(realpath(game, expected_game));
    assert(strcmp(resolved, expected_game) == 0);
    assert(!jw_storage_relative_path_valid("../game.zip"));
    assert(!jw_storage_relative_path_valid("folder//game.zip"));
    assert(jw_storage_relative_path_valid("SNES/game.zip"));

    unsetenv("MUSIC_PATHS");
    setenv("MUSIC_PATH", "/primary-music", 1);
    assert(jw_storage_sources_resolve(card1, &sources) == 0);
    assert(strcmp(sources.sources[0].music_path, "/primary-music") == 0);
    snprintf(expected, sizeof(expected), "%s/Music", card2);
    assert(strcmp(sources.sources[1].music_path, expected) == 0);

    setenv("MUSIC_PATHS", "/only-one", 1);
    assert(jw_storage_sources_resolve(card1, &sources) != 0);

    setenv("MUSIC_PATHS", music, 1);
    setenv("SDCARD_PATH", card2, 1);
    assert(jw_storage_sources_resolve(card1, &sources) != 0);

    snprintf(roots, sizeof(roots), "%s:%s", card1, card1);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("SDCARD_PATH", card1, 1);
    assert(jw_storage_sources_resolve(card1, &sources) != 0);

    unsetenv("SDCARD_PATHS");
    unsetenv("MUSIC_PATHS");
    setenv("SDCARD_PATH", card1, 1);
    setenv("UMRK_SECONDARY_SDCARD_PATH", card2, 1);
    assert(jw_storage_sources_resolve(card1, &sources) == 0);
    assert(sources.count == 2);
    assert(strcmp(sources.sources[1].id, "secondary_sd") == 0);

    unlink(game);
    rmdir(roms);
    rmdir(card2);
    rmdir(card1);
    rmdir(temp);
    return 0;
}
