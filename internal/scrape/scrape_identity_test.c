#define _POSIX_C_SOURCE 200809L

#include "internal/scrape/scrape_identity.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char path[] = "/tmp/jw-scrape-identity-XXXXXX";

static void write_bytes(const void *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(bytes, 1u, size, file) == size);
    assert(fclose(file) == 0);
}

static void expect(const void *bytes, size_t size, const char *rom_name,
                   const char *title, size_t count,
                   const char *first, const char *second, const char *third) {
    write_bytes(bytes, size);
    jw_scrape_identity_candidates candidates;
    jw_scrape_identity_build(path, rom_name, title, "scummvm", &candidates);
    assert(candidates.count == count);
    if (first) assert(strcmp(candidates.names[0], first) == 0);
    if (second) assert(strcmp(candidates.names[1], second) == 0);
    if (third) assert(strcmp(candidates.names[2], third) == 0);
}

static void expect_folder(const char *folder, const char *retry) {
    jw_scrape_identity_candidates candidates;
    jw_scrape_identity_build_folder(folder, &candidates);
    size_t want = retry ? 2u : 1u;
    if (candidates.count != want ||
        (retry && strcmp(candidates.names[1], retry) != 0)) {
        fprintf(stderr, "%s: want retry %s, got %zu candidates%s%s\n", folder,
                retry ? retry : "(none)", candidates.count,
                candidates.count > 1 ? ": " : "",
                candidates.count > 1 ? candidates.names[1] : "");
    }
    assert(candidates.count == want);
    assert(strcmp(candidates.names[0], folder) == 0);
    if (retry) assert(strcmp(candidates.names[1], retry) == 0);
}

/* Folder games: the folder name, then once more without a trailing version.
   The names are ones ScreenScraper missed with the version and found without
   it, or titles whose number must stay. */
static void test_folder_ladder(void) {
    expect_folder("Dragon Destiny v1.2", "Dragon Destiny");
    expect_folder("YumeNikki_v0.10", "YumeNikki");
    expect_folder("space_funeral_v2", "space_funeral");
    expect_folder("Legend of Heroen V1_20", "Legend of Heroen");
    expect_folder("Ara Fell 1.1", "Ara Fell");
    expect_folder("Yume 2kki 0.123c", "Yume 2kki");
    expect_folder("Game - ver1.2", "Game");
    expect_folder("Game version2", "Game");

    expect_folder("Vampires Dawn 2", NULL);
    expect_folder("The Way Episode 1", NULL);
    expect_folder("Final Fantasy VI-2", NULL);
    expect_folder("Final Fantasy 7 2D Remake", NULL);
    expect_folder("Game 2D", NULL);
    expect_folder("Game v", NULL);
    expect_folder("Game v1.", NULL);
    expect_folder("OFF", NULL);
    expect_folder("v1.2", NULL);
    expect_folder("_v1.2", NULL);

    jw_scrape_identity_candidates candidates;
    jw_scrape_identity_build_folder(NULL, &candidates);
    assert(candidates.count == 0);
    jw_scrape_identity_build_folder("", &candidates);
    assert(candidates.count == 0);
}

int main(void) {
    test_folder_ladder();

    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    expect("kq1\n", strlen("kq1\n"), "Kings Quest 1.scummvm", "King's Quest I", 3,
           "kq1.scummvm", "Kings Quest 1.scummvm",
           "King's Quest I.scummvm");
    expect("\t kq1 \t\r\n", strlen("\t kq1 \t\r\n"),
           "Kings Quest 1.svm", "King's Quest I", 3,
           "kq1.scummvm", "Kings Quest 1.svm", "King's Quest I.scummvm");
    expect("kq1", strlen("kq1"), "Kings Quest 1.svm", "King's Quest I", 3,
           "kq1.scummvm", "Kings Quest 1.svm", "King's Quest I.scummvm");

    const char *invalid[] = {
        "", " \t\n", "bad/path\n", "bad\\path\n", "kq1\nsecond\n",
        "kq1\x01\n", "\r", "_kq1\n",
    };
    const size_t invalid_sizes[] = {0, 3, 9, 9, 11, 5, 1, 5};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        expect(invalid[i], invalid_sizes[i], "Kings Quest 1.svm",
               "King's Quest I", 2, "Kings Quest 1.svm",
               "King's Quest I.scummvm", NULL);
    }

    char too_long[257];
    memset(too_long, 'a', sizeof(too_long));
    expect(too_long, sizeof(too_long), "Game.svm", "Game", 2,
           "Game.svm", "Game.scummvm", NULL);
    const unsigned char with_nul[] = {'k', 'q', '1', 0, '\n'};
    expect(with_nul, sizeof(with_nul), "Game.svm", "Game", 2,
           "Game.svm", "Game.scummvm", NULL);

    /* Case-insensitive duplicates spend no extra provider request, and a
       title already carrying the declared suffix is not extended twice. */
    expect("GAME", strlen("GAME"), "game.scummvm", "GAME.SCUMMVM", 1,
           "GAME.scummvm", NULL, NULL);

    unlink(path);
    puts("scrape-identity-test: ok");
    return 0;
}
