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

static jw_scrape_identity_candidates g_names;

/* The manual name appended after a ladder, or NULL when it adds nothing. */
static const char *append_manual(const char *title, const char *extension) {
    size_t before = g_names.count;
    if (!jw_scrape_identity_add_manual_name(&g_names, title, extension)) {
        assert(g_names.count == before);
        return NULL;
    }
    assert(g_names.count == before + 1u);
    return g_names.names[before];
}

static const char *manual_after_file(const char *rom_name, const char *title,
                                     const char *extension) {
    jw_scrape_identity_build_filename(rom_name, &g_names);
    assert(g_names.count == 1u && strcmp(g_names.names[0], rom_name) == 0);
    return append_manual(title, extension);
}

static const char *manual_after_folder(const char *folder, const char *title) {
    jw_scrape_identity_build_folder(folder, &g_names);
    return append_manual(title, NULL);
}

static void expect_name(const char *got, const char *want) {
    if (!want) {
        assert(got == NULL);
        return;
    }
    assert(got && strcmp(got, want) == 0);
}

/* The manual Display Name lookup outside descriptor mode: a ROM's own
   extension goes on, a folder game's name goes alone, and a name equal to
   any one already sent, ignoring case, costs no second request. */
static void test_manual_name(void) {
    expect_name(manual_after_file("smb.nes", "Super Mario Bros", "nes"),
                "Super Mario Bros.nes");
    expect_name(manual_after_file("smb.nes", "  Super Mario Bros\t", "nes"),
                "Super Mario Bros.nes");
    expect_name(manual_after_file("smb.nes", "Super Mario Bros.NES", "nes"),
                "Super Mario Bros.NES");
    expect_name(manual_after_file("Sonic.md.zip", "Sonic the Hedgehog", "zip"),
                "Sonic the Hedgehog.zip");
    expect_name(manual_after_file("README", "Read Me", NULL), "Read Me");
    expect_name(manual_after_file("README", "Read Me", ""), "Read Me");

    expect_name(manual_after_file("smb.nes", "SMB", "nes"), NULL);
    expect_name(manual_after_file("smb.nes", "smb.NES", "nes"), NULL);
    expect_name(manual_after_file("smb.nes", "", "nes"), NULL);
    expect_name(manual_after_file("smb.nes", " \t ", "nes"), NULL);
    expect_name(manual_after_file("smb.nes", NULL, "nes"), NULL);

    /* A folder game's ladder may hold two names; the manual one is checked
       against both. */
    expect_name(manual_after_folder("Folder Game", "Renamed RPG"), "Renamed RPG");
    expect_name(manual_after_folder("Folder Game", "FOLDER GAME"), NULL);
    expect_name(manual_after_folder("Dragon Destiny v1.2", "dragon destiny"), NULL);
    expect_name(manual_after_folder("Dragon Destiny v1.2", "Dragon Destiny 2"),
                "Dragon Destiny 2");
    assert(g_names.count == 3u);

    /* A name too long for a lookup is skipped, not cut short. */
    char long_title[JW_SCRAPE_IDENTITY_NAME_MAX + 8];
    memset(long_title, 'a', JW_SCRAPE_IDENTITY_NAME_MAX);
    long_title[JW_SCRAPE_IDENTITY_NAME_MAX] = '\0';
    expect_name(manual_after_folder("Folder Game", long_title), NULL);
    long_title[JW_SCRAPE_IDENTITY_NAME_MAX - 4] = '\0';
    expect_name(manual_after_file("smb.nes", long_title, "nes"), NULL);
    expect_name(manual_after_folder("Folder Game", long_title), long_title);

    jw_scrape_identity_build_filename(NULL, &g_names);
    assert(g_names.count == 0u);
    assert(!jw_scrape_identity_add_manual_name(NULL, "Title", "nes"));
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

    test_manual_name();

    puts("scrape-identity-test: ok");
    return 0;
}
