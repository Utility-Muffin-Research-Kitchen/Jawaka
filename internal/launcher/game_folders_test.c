#include "internal/launcher/game_folders.h"

#include "internal/core/title_order.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Input games as a plain relpath array. The marker callback mirrors
   jw_art_is_folder_marker (one file name, never at the system root) and logs
   every call as "subpath|file;". */
typedef struct {
    const char *const *paths;
    int count;
    const char *marker;
    int marker_calls;
    char marker_log[512];
} fixture;

static const char *fixture_relpath(void *userdata, int i) {
    const fixture *fx = userdata;
    return fx->paths[i];
}

static bool fixture_marker(void *userdata, const char *rel_subpath,
                           const char *filename) {
    fixture *fx = userdata;
    fx->marker_calls++;
    size_t used = strlen(fx->marker_log);
    snprintf(fx->marker_log + used, sizeof(fx->marker_log) - used, "%s|%s;",
             rel_subpath, filename);
    return fx->marker && rel_subpath[0] && strcmp(filename, fx->marker) == 0;
}

static jw_game_folder_tree *build(fixture *fx) {
    return jw_game_folder_tree_build(fx->count, fixture_relpath,
                                     fx->marker ? fixture_marker : NULL, fx);
}

static int failures = 0;

static const char *lbl(const char *fmt, ...) {
    static char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static void expect_int(const char *label, int actual, int expected) {
    if (actual != expected) {
        fprintf(stderr, "%s: got %d, expected %d\n", label, actual, expected);
        failures++;
    }
}

static void expect_str(const char *label, const char *actual,
                       const char *expected) {
    bool same = (!actual && !expected) ||
                (actual && expected && strcmp(actual, expected) == 0);
    if (!same) {
        fprintf(stderr, "%s: got \"%s\", expected \"%s\"\n", label,
                actual ? actual : "(null)", expected ? expected : "(null)");
        failures++;
    }
}

static void expect_true(const char *label, bool value) {
    if (!value) {
        fprintf(stderr, "%s: expected true\n", label);
        failures++;
    }
}

#define COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))

static void append(char *buf, size_t size, const char *fmt, ...) {
    size_t used = strlen(buf);
    if (used + 1 >= size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, size - used, fmt, ap);
    va_end(ap);
}

/* A level's rows as text: "[name]" for a folder, "#i" for a game. */
static const char *render_rows(const jw_game_folder_tree *tree, int folder) {
    static char out[4096];
    out[0] = '\0';
    int total = jw_game_folder_rows(tree, folder, NULL, 0);
    if (total < 0) {
        return "(none)";
    }
    jw_game_folder_row *rows = calloc((size_t)(total ? total : 1), sizeof(*rows));
    if (!rows) {
        return "(oom)";
    }
    int again = jw_game_folder_rows(tree, folder, rows, total);
    for (int k = 0; k < again; k++) {
        if (rows[k].kind == JW_GAME_FOLDER_ROW_FOLDER) {
            append(out, sizeof(out), "%s[%s]", k ? " " : "",
                   jw_game_folder_name(tree, rows[k].index));
        } else {
            append(out, sizeof(out), "%s#%d", k ? " " : "", rows[k].index);
        }
    }
    free(rows);
    return out;
}

/* Contract checks that hold for every tree: each game is listed exactly once,
   in the level of_game names; each uncollapsed folder is one row of its
   parent; folder rows come first in name order; games keep input order;
   collapsed folders are one-game leaves listed through their parent. */
static void check_invariants(const char *label, const jw_game_folder_tree *tree,
                             int count) {
    int folders = jw_game_folder_count(tree);
    int *seen_game = calloc((size_t)count + 1, sizeof(int));
    int *seen_folder = calloc((size_t)folders + 1, sizeof(int));
    if (!seen_game || !seen_folder) {
        free(seen_game);
        free(seen_folder);
        fprintf(stderr, "%s: invariants: out of memory\n", label);
        failures++;
        return;
    }
    char who[256];
    snprintf(who, sizeof(who), "%s: invariants", label);
    expect_int(lbl("%s: root parent", who), jw_game_folder_parent(tree, 0), -1);
    expect_int(lbl("%s: root never collapses", who),
               jw_game_folder_collapsed_game(tree, 0), -1);
    expect_int(lbl("%s: root counts every game", who),
               jw_game_folder_game_count(tree, 0), count);
    for (int f = 0; f < folders; f++) {
        int total = jw_game_folder_rows(tree, f, NULL, 0);
        int collapsed = jw_game_folder_collapsed_game(tree, f);
        if (f > 0) {
            expect_true(lbl("%s: folder %d holds a game", who, f),
                        jw_game_folder_game_count(tree, f) >= 1);
            expect_int(lbl("%s: lookup of folder %d's path", who, f),
                       jw_game_folder_lookup(tree, jw_game_folder_path(tree, f)), f);
        }
        if (collapsed >= 0) {
            expect_int(lbl("%s: collapsed folder %d has no rows", who, f), total, -1);
            expect_int(lbl("%s: collapsed folder %d count", who, f),
                       jw_game_folder_game_count(tree, f), 1);
            expect_int(lbl("%s: collapsed folder %d game listed in parent", who, f),
                       jw_game_folder_of_game(tree, collapsed),
                       jw_game_folder_parent(tree, f));
            continue;
        }
        expect_true(lbl("%s: folder %d has rows", who, f), total >= 0);
        if (total <= 0) {
            continue;
        }
        jw_game_folder_row *rows = calloc((size_t)total, sizeof(*rows));
        if (!rows) {
            failures++;
            continue;
        }
        jw_game_folder_rows(tree, f, rows, total);
        bool in_games = false;
        int last_game = -1;
        const char *last_name = NULL;
        for (int k = 0; k < total; k++) {
            int index = rows[k].index;
            if (rows[k].kind == JW_GAME_FOLDER_ROW_FOLDER) {
                expect_true(lbl("%s: folder %d row %d before games", who, f, k),
                            !in_games);
                if (index <= 0 || index >= folders) {
                    expect_true(lbl("%s: folder %d row %d id", who, f, k), false);
                    continue;
                }
                expect_int(lbl("%s: folder %d row %d parent", who, f, k),
                           jw_game_folder_parent(tree, index), f);
                expect_int(lbl("%s: folder %d row %d uncollapsed", who, f, k),
                           jw_game_folder_collapsed_game(tree, index), -1);
                const char *name = jw_game_folder_name(tree, index);
                if (last_name) {
                    expect_true(lbl("%s: folder %d rows sorted at %d", who, f, k),
                                jw_title_compare(last_name, strlen(last_name),
                                                 name, strlen(name)) < 0);
                }
                last_name = name;
                seen_folder[index]++;
            } else {
                in_games = true;
                if (index < 0 || index >= count) {
                    expect_true(lbl("%s: folder %d row %d game", who, f, k), false);
                    continue;
                }
                expect_true(lbl("%s: folder %d games in input order at %d", who, f, k),
                            index > last_game);
                last_game = index;
                expect_int(lbl("%s: folder %d row %d of_game", who, f, k),
                           jw_game_folder_of_game(tree, index), f);
                seen_game[index]++;
            }
        }
        free(rows);
    }
    for (int g = 0; g < count; g++) {
        expect_int(lbl("%s: game %d listed once", who, g), seen_game[g], 1);
    }
    for (int f = 1; f < folders; f++) {
        bool collapsed = jw_game_folder_collapsed_game(tree, f) >= 0;
        expect_int(lbl("%s: folder %d listed", who, f), seen_folder[f],
                   collapsed ? 0 : 1);
    }
    free(seen_game);
    free(seen_folder);
}

/* --- 1. The plan's collapse examples --------------------------------- */

typedef struct {
    const char *label;
    const char *paths[2];
    int count;
    const char *folder;   /* path below the system folder */
    bool collapses;
} collapse_case;

static void check_collapse(const char *label, const char *const *paths,
                           int count, const char *folder, bool collapses) {
    fixture fx = { paths, count, NULL, 0, "" };
    jw_game_folder_tree *tree = build(&fx);
    if (!tree) {
        fprintf(stderr, "%s: build failed\n", label);
        failures++;
        return;
    }
    check_invariants(label, tree, count);
    int f = jw_game_folder_lookup(tree, folder);
    expect_true(lbl("%s: folder %s exists", label, folder), f > 0);
    if (f > 0) {
        int parent = jw_game_folder_parent(tree, f);
        expect_int(lbl("%s: collapsed game", label),
                   jw_game_folder_collapsed_game(tree, f), collapses ? 0 : -1);
        expect_int(lbl("%s: count", label), jw_game_folder_game_count(tree, f),
                   count);
        for (int g = 0; g < count; g++) {
            expect_int(lbl("%s: game %d listed in", label, g),
                       jw_game_folder_of_game(tree, g), collapses ? parent : f);
        }
        expect_int(lbl("%s: rows", label), jw_game_folder_rows(tree, f, NULL, 0),
                   collapses ? -1 : count);
        expect_int(lbl("%s: resolve", label), jw_game_folder_resolve(tree, folder),
                   collapses ? parent : f);
    }
    jw_game_folder_tree_free(tree);
}

static void test_collapse_table(void) {
    static const collapse_case table[] = {
        { "generated playlist",
          { "PS/Final Fantasy VII/Final Fantasy VII (USA).m3u" }, 1,
          "Final Fantasy VII", true },
        { "hand-made playlist", { "PS/Final Fantasy VII/FF7.m3u" }, 1,
          "Final Fantasy VII", false },
        { "organization folder", { "PS/Translated/FF7.m3u" }, 1, "Translated",
          false },
        { "per-game folder inside an organization folder",
          { "PS/Translated/FF7/FF7.m3u" }, 1, "Translated/FF7", true },
        { "long title",
          { "PS/Castlevania - Symphony of the Night/"
            "Castlevania - Symphony of the Night (USA).chd" }, 1,
          "Castlevania - Symphony of the Night", true },
        { "tags on both names", { "PS/Game (USA)/Game (USA) (Rev 1).chd" }, 1,
          "Game (USA)", true },
        { "franchise folder", { "MD/Sonic/Sonic The Hedgehog (USA).zip" }, 1,
          "Sonic", false },
        { "heuristic miss", { "MD/Sonic/Sonic.zip" }, 1, "Sonic", true },
        { "translated GBA", { "GBA/Translated/Mother 3 (English).gba" }, 1,
          "Translated", false },
        { "short folder name", { "PS/FF7/Final Fantasy VII (USA).chd" }, 1, "FF7",
          false },
        { "same game on two cards", { "PS/FF7/FF7.m3u", "PS/FF7/FF7.m3u" }, 2,
          "FF7", false },
        { "match uses the file name", { "SFC/Top Gear/Top Gear (USA).zip" }, 1,
          "Top Gear", true },
    };
    for (int k = 0; k < COUNT(table); k++) {
        check_collapse(table[k].label, table[k].paths, table[k].count,
                       table[k].folder, table[k].collapses);
    }

    {
        const char *paths[] = { "PS/Final Fantasy VII/Final Fantasy VII (USA).m3u" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        expect_str("collapsed playlist root rows", render_rows(tree, 0), "#0");
        jw_game_folder_tree_free(tree);
    }
    {
        /* FF7 promotes into Translated only; Translated had a subfolder when
           it was judged, so it stays. */
        const char *paths[] = { "PS/Translated/FF7/FF7.m3u" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        int translated = jw_game_folder_lookup(tree, "Translated");
        expect_true("Translated exists", translated > 0);
        expect_int("Translated stays a folder",
                   jw_game_folder_collapsed_game(tree, translated), -1);
        expect_int("FF7 listed in Translated", jw_game_folder_of_game(tree, 0),
                   translated);
        expect_str("Translated rows", render_rows(tree, translated), "#0");
        expect_str("root rows with Translated", render_rows(tree, 0),
                   "[Translated]");
        jw_game_folder_tree_free(tree);
    }
    {
        const char *paths[] = { "PS/FF7/FF7.m3u", "PS/FF7/FF7.m3u" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        expect_int("two cards: one folder", jw_game_folder_count(tree), 2);
        expect_str("two cards: FF7 rows",
                   render_rows(tree, jw_game_folder_lookup(tree, "FF7")), "#0 #1");
        expect_str("two cards: root rows", render_rows(tree, 0), "[FF7]");
        jw_game_folder_tree_free(tree);
    }
    {
        const char *paths[] = { "EASYRPG/My Game/RPG_RT.ldb" };
        fixture fx = { paths, COUNT(paths), "RPG_RT.ldb", 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("EasyRPG marker", tree, 1);
        expect_int("marker: listed at root", jw_game_folder_of_game(tree, 0), 0);
        expect_int("marker: folder never exists",
                   jw_game_folder_lookup(tree, "My Game"), -1);
        expect_int("marker: only the root", jw_game_folder_count(tree), 1);
        expect_str("marker: root rows", render_rows(tree, 0), "#0");
        expect_str("marker: callback arguments", fx.marker_log,
                   "My Game|RPG_RT.ldb;");
        jw_game_folder_tree_free(tree);
    }
}

/* --- 2. More collapse and normalization rules ------------------------- */

static void test_collapse_rules(void) {
    {
        const char *paths[] = {
            "SFC/Top Gear/Top Gear (USA).zip",
            "SFC/Top Gear/Extras/Track.zip",
            "SFC/Top Gear/Extras/Demo.zip",
        };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("game plus subfolder", tree, COUNT(paths));
        int top = jw_game_folder_lookup(tree, "Top Gear");
        expect_int("game plus subfolder stays",
                   jw_game_folder_collapsed_game(tree, top), -1);
        expect_str("game plus subfolder rows", render_rows(tree, top),
                   "[Extras] #0");
        jw_game_folder_tree_free(tree);
    }
    {
        /* The inner folder collapses; re-judged afterwards the outer one would
           hold one matching game and no subfolder, but it is never re-judged. */
        const char *paths[] = { "SFC/Top Gear/Top Gear/Top Gear.zip" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("no cascade", tree, COUNT(paths));
        int outer = jw_game_folder_lookup(tree, "Top Gear");
        int inner = jw_game_folder_lookup(tree, "Top Gear/Top Gear");
        expect_int("no cascade: inner collapses",
                   jw_game_folder_collapsed_game(tree, inner), 0);
        expect_int("no cascade: outer stays",
                   jw_game_folder_collapsed_game(tree, outer), -1);
        expect_int("no cascade: game in outer", jw_game_folder_of_game(tree, 0),
                   outer);
        expect_str("no cascade: root rows", render_rows(tree, 0), "[Top Gear]");
        jw_game_folder_tree_free(tree);
    }
    {
        const char *paths[] = { "SFC/Top Gear/Top Gear (USA).zip",
                                "SFC/Top Gear/Top Gear (Europe).zip" };
        check_collapse("two games stay", paths, COUNT(paths), "Top Gear", false);
    }

    static const struct {
        const char *relpath;
        const char *folder;
        bool collapses;
    } names[] = {
        { "GB/Tetris/Tetris [!].gb", "Tetris", true },
        { "SFC/Game/Game (USA) (Rev 1) [!].sfc", "Game", true },
        { "SFC/Super  Mario   World/Super Mario World (USA).sfc",
          "Super  Mario   World", true },
        { "SFC/Super Mario World/Super  Mario World.sfc", "Super Mario World", true },
        { "SFC/ Padded /Padded.sfc", " Padded ", true },
        { "SFC/top gear/TOP GEAR.zip", "top gear", true },
        { "SFC/Game (Europe) [b]/Game (USA).sfc", "Game (Europe) [b]", true },
        { "SFC/Game (a (b))/Game.sfc", "Game (a (b))", true },
        { "SFC/Game [x] (y)/Game (y) [x].sfc", "Game [x] (y)", true },
        { "SFC/Game.v2/Game.v2.zip", "Game.v2", true },
        { "SFC/README/README", "README", true },
        { "SFC/Game/Game (USA.sfc", "Game", false },
        { "SFC/(USA)/(USA).sfc", "(USA)", false },
        { "SFC/Game/Game.tar.gz", "Game", false },
        { "SFC/Game/Game 2.sfc", "Game", false },
        { "SFC/Game/Game (USA) Beta.sfc", "Game", false },
        { "SFC/Game (USA)/(USA).sfc", "Game (USA)", false },
    };
    for (int k = 0; k < COUNT(names); k++) {
        const char *paths[] = { names[k].relpath };
        check_collapse(names[k].relpath, paths, 1, names[k].folder,
                       names[k].collapses);
    }

    {
        /* A marker below an organization folder: listed in that folder, and the
           callback sees the subpath as spelled. Root games are never asked. */
        const char *paths[] = { "EASYRPG/RPGs/My Game/RPG_RT.ldb",
                                "EASYRPG/Loose.zip" };
        fixture fx = { paths, COUNT(paths), "RPG_RT.ldb", 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("nested marker", tree, COUNT(paths));
        int rpgs = jw_game_folder_lookup(tree, "RPGs");
        expect_true("nested marker: RPGs exists", rpgs > 0);
        expect_int("nested marker: listed in RPGs", jw_game_folder_of_game(tree, 0),
                   rpgs);
        expect_int("nested marker: RPGs stays",
                   jw_game_folder_collapsed_game(tree, rpgs), -1);
        expect_int("nested marker: no marker folder",
                   jw_game_folder_lookup(tree, "RPGs/My Game"), -1);
        expect_str("nested marker: callback log", fx.marker_log,
                   "RPGs/My Game|RPG_RT.ldb;");
        jw_game_folder_tree_free(tree);
    }
    {
        /* The marker folder exists when another game sits in it. */
        const char *paths[] = { "EASYRPG/My Game/RPG_RT.ldb",
                                "EASYRPG/My Game/Bonus.zip" };
        fixture fx = { paths, COUNT(paths), "RPG_RT.ldb", 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("marker beside a game", tree, COUNT(paths));
        int folder = jw_game_folder_lookup(tree, "My Game");
        expect_true("marker beside a game: folder exists", folder > 0);
        expect_int("marker beside a game: marker at root",
                   jw_game_folder_of_game(tree, 0), 0);
        expect_int("marker beside a game: other game in folder",
                   jw_game_folder_of_game(tree, 1), folder);
        expect_str("marker beside a game: root rows", render_rows(tree, 0),
                   "[My Game] #0");
        jw_game_folder_tree_free(tree);
    }
    {
        /* A marker game is judged by its marker folder, never by the marker
           file: RPG_RT.ldb must not match a folder that happens to be named
           RPG_RT, which would also move the game a second level. */
        const char *paths[] = { "EASYRPG/RPG_RT/Ib/RPG_RT.ldb" };
        fixture fx = { paths, COUNT(paths), "RPG_RT.ldb", 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("marker file name", tree, COUNT(paths));
        int folder = jw_game_folder_lookup(tree, "RPG_RT");
        expect_true("marker file name: RPG_RT exists", folder > 0);
        expect_int("marker file name: RPG_RT stays",
                   jw_game_folder_collapsed_game(tree, folder), -1);
        expect_int("marker file name: listed in RPG_RT",
                   jw_game_folder_of_game(tree, 0), folder);
        jw_game_folder_tree_free(tree);
    }
    {
        /* Unzipping often doubles the folder: the outer one is the game's own
           and collapses by the marker folder's name, tags and case aside. */
        const char *paths[] = { "EASYRPG/My Game/My Game/RPG_RT.ldb",
                                "EASYRPG/Quest/quest (v1.2)/RPG_RT.ldb" };
        fixture fx = { paths, COUNT(paths), "RPG_RT.ldb", 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("doubled marker folder", tree, COUNT(paths));
        int outer = jw_game_folder_lookup(tree, "My Game");
        int quest = jw_game_folder_lookup(tree, "Quest");
        expect_int("doubled marker folder: My Game collapses",
                   jw_game_folder_collapsed_game(tree, outer), 0);
        expect_int("doubled marker folder: Quest collapses",
                   jw_game_folder_collapsed_game(tree, quest), 1);
        expect_str("doubled marker folder: root rows", render_rows(tree, 0),
                   "#0 #1");
        jw_game_folder_tree_free(tree);
    }
}

/* --- 3. Merge across alias folders and case --------------------------- */

static void test_merge(void) {
    {
        const char *paths[] = { "MD/Hacks/a.zip", "GENESIS/hacks/b.zip" };
        const char *reversed[] = { "GENESIS/hacks/b.zip", "MD/Hacks/a.zip" };
        const char *const *orders[] = { paths, reversed };
        for (int o = 0; o < 2; o++) {
            fixture fx = { orders[o], 2, NULL, 0, "" };
            jw_game_folder_tree *tree = build(&fx);
            check_invariants(lbl("merge order %d", o), tree, 2);
            int f = jw_game_folder_lookup(tree, "HACKS");
            expect_int(lbl("merge order %d: one folder", o),
                       jw_game_folder_count(tree), 2);
            expect_true(lbl("merge order %d: found without case", o), f > 0);
            expect_str(lbl("merge order %d: name", o), jw_game_folder_name(tree, f),
                       "Hacks");
            expect_str(lbl("merge order %d: path", o), jw_game_folder_path(tree, f),
                       "Hacks");
            expect_int(lbl("merge order %d: count", o),
                       jw_game_folder_game_count(tree, f), 2);
            expect_str(lbl("merge order %d: rows", o), render_rows(tree, f), "#0 #1");
            jw_game_folder_tree_free(tree);
        }
    }
    {
        /* Each segment shows its own smallest spelling, even when no single
           input spells the whole path that way. */
        const char *paths[] = { "MD/hacks/Sub/x.zip", "GENESIS/Hacks/sub/y.zip" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        int sub = jw_game_folder_lookup(tree, "hacks/sub");
        expect_str("per-segment spelling: path", jw_game_folder_path(tree, sub),
                   "Hacks/Sub");
        expect_str("per-segment spelling: name", jw_game_folder_name(tree, sub),
                   "Sub");
        expect_str("per-segment spelling: parent path",
                   jw_game_folder_path(tree, jw_game_folder_parent(tree, sub)),
                   "Hacks");
        jw_game_folder_tree_free(tree);
    }
}

/* --- 4. Order independence -------------------------------------------- */

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Everything about the tree that must not depend on input order, one line
   per folder, sorted. Game rows follow input order by contract, so a level's
   games are compared as a sorted list of relpaths. */
static char *describe(const jw_game_folder_tree *tree, const char *const *paths) {
    int folders = jw_game_folder_count(tree);
    char **lines = calloc((size_t)folders, sizeof(*lines));
    size_t total = 1;
    for (int f = 0; f < folders; f++) {
        char line[2048] = "";
        int collapsed = jw_game_folder_collapsed_game(tree, f);
        append(line, sizeof(line), "%s|%s|%d|%s|", jw_game_folder_path(tree, f),
               jw_game_folder_name(tree, f), jw_game_folder_game_count(tree, f),
               collapsed >= 0 ? paths[collapsed] : "-");
        int count = jw_game_folder_rows(tree, f, NULL, 0);
        if (count > 0) {
            jw_game_folder_row *rows = calloc((size_t)count, sizeof(*rows));
            const char **games = calloc((size_t)count, sizeof(*games));
            int game_count = 0;
            jw_game_folder_rows(tree, f, rows, count);
            for (int k = 0; k < count; k++) {
                if (rows[k].kind == JW_GAME_FOLDER_ROW_FOLDER) {
                    append(line, sizeof(line), "[%s]",
                           jw_game_folder_path(tree, rows[k].index));
                } else {
                    games[game_count++] = paths[rows[k].index];
                }
            }
            qsort(games, (size_t)game_count, sizeof(*games), compare_strings);
            for (int k = 0; k < game_count; k++) {
                append(line, sizeof(line), "<%s>", games[k]);
            }
            free(games);
            free(rows);
        }
        lines[f] = malloc(strlen(line) + 1);
        strcpy(lines[f], line);
        total += strlen(line) + 1;
    }
    qsort(lines, (size_t)folders, sizeof(*lines), compare_strings);
    char *out = malloc(total);
    out[0] = '\0';
    for (int f = 0; f < folders; f++) {
        strcat(out, lines[f]);
        strcat(out, "\n");
        free(lines[f]);
    }
    free(lines);
    return out;
}

static void test_order_independence(void) {
    static const char *const paths[] = {
        "MD/Hacks/Sonic 1 Hack.zip",
        "GENESIS/hacks/Sonic 2 Hack.zip",
        "MD/HACKS/Deep/Deep Thing.zip",
        "GENESIS/Hacks/deep/Other.zip",
        "MD/Sonic/Sonic.zip",
        "MD/Streets/Streets of Rage (USA).zip",
        "MD/Streets/Streets of Rage 2 (USA).zip",
        "MD/Streets of Rage 3/Streets of Rage 3 (USA) [!].zip",
        "MD/Top/Top/Top.zip",
        "MD/Root Game.zip",
        "GENESIS/Root Game 2.zip",
        "MD/RPG/Quest/RPG_RT.ldb",
        "MD/rpg/Other Quest/RPG_RT.ldb",
        "MD/FF7/FF7.m3u",
        "MD/FF7/FF7.m3u",
        "GENESIS/alpha/Alpha One.zip",
        "MD/Alpha/Alpha Two.zip",
        "MD/beta/beta.zip",
    };
    enum { N = COUNT(paths) };
    char *baseline = NULL;
    unsigned seed = 12345u;
    for (int round = 0; round < 8; round++) {
        int order[N];
        for (int k = 0; k < N; k++) {
            order[k] = round == 1 ? N - 1 - k : k;
        }
        if (round >= 2) {
            for (int k = N - 1; k > 0; k--) {
                seed = seed * 1103515245u + 12345u;
                int j = (int)((seed >> 16) % (unsigned)(k + 1));
                int swap = order[k];
                order[k] = order[j];
                order[j] = swap;
            }
        }
        const char *permuted[N];
        for (int k = 0; k < N; k++) {
            permuted[k] = paths[order[k]];
        }
        fixture fx = { permuted, N, "RPG_RT.ldb", 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        if (!tree) {
            fprintf(stderr, "order round %d: build failed\n", round);
            failures++;
            continue;
        }
        check_invariants(lbl("order round %d", round), tree, N);
        char *text = describe(tree, permuted);
        if (!baseline) {
            baseline = text;
            /* Spot-check the baseline itself so equal-but-wrong cannot pass. */
            expect_str("order: Hacks/Deep spelling",
                       jw_game_folder_path(tree, jw_game_folder_lookup(tree, "hacks/deep")),
                       "HACKS/Deep");
            expect_str("order: Alpha spelling",
                       jw_game_folder_name(tree, jw_game_folder_lookup(tree, "alpha")),
                       "Alpha");
            expect_str("order: RPG spelling",
                       jw_game_folder_name(tree, jw_game_folder_lookup(tree, "rpg")),
                       "RPG");
        } else {
            if (strcmp(text, baseline) != 0) {
                fprintf(stderr, "order round %d differs:\n%s\nbaseline:\n%s\n",
                        round, text, baseline);
                failures++;
            }
            free(text);
        }
        jw_game_folder_tree_free(tree);
    }
    free(baseline);
}

/* --- 5. Row order ----------------------------------------------------- */

static void test_row_order(void) {
    const char *paths[] = {
        "X/root1.zip",
        "X/gamma/g1.zip",
        "X/Zelda/Zelda (USA).zip",
        "X/alpha/a1.zip",
        "X/root2.zip",
        "X/Beta/b1.zip",
        "X/alpha/a2.zip",
        "X/Beta/b2.zip",
        "X/gamma/g2.zip",
        "X/Mario/Mario.zip",
        "X/root3.zip",
    };
    fixture fx = { paths, COUNT(paths), NULL, 0, "" };
    jw_game_folder_tree *tree = build(&fx);
    check_invariants("row order", tree, COUNT(paths));
    expect_str("row order: root", render_rows(tree, 0),
               "[alpha] [Beta] [gamma] #0 #2 #4 #9 #10");
    expect_str("row order: alpha", render_rows(tree, jw_game_folder_lookup(tree, "alpha")),
               "#3 #6");
    expect_str("row order: Beta", render_rows(tree, jw_game_folder_lookup(tree, "beta")),
               "#5 #7");
    jw_game_folder_tree_free(tree);

    /* Numbers in folder names count as numbers, as they do in game titles. */
    const char *numbered[] = {
        "X/Disc 10/a.zip",
        "X/Disc 2/a.zip",
        "X/disc 1/a.zip",
        "X/Disc 02b/a.zip",
    };
    fixture numbered_fx = { numbered, COUNT(numbered), NULL, 0, "" };
    tree = build(&numbered_fx);
    check_invariants("row order: numbered", tree, COUNT(numbered));
    expect_str("row order: numbered root", render_rows(tree, 0),
               "[disc 1] [Disc 2] [Disc 02b] [Disc 10]");
    jw_game_folder_tree_free(tree);
}

/* --- 6-9. Queries on one nested tree --------------------------------- */

static void test_queries(void) {
    const char *paths[] = {
        "SNES/A/a1.zip",
        "SNES/A/B/b1.zip",
        "SNES/A/B/b2.zip",
        "SNES/A/B/C/C (USA).zip",
        "SNES/A/Quest/RPG_RT.ldb",
        "SNES/root.zip",
    };
    fixture fx = { paths, COUNT(paths), "RPG_RT.ldb", 0, "" };
    jw_game_folder_tree *tree = build(&fx);
    if (!tree) {
        fprintf(stderr, "queries: build failed\n");
        failures++;
        return;
    }
    check_invariants("queries", tree, COUNT(paths));
    int a = jw_game_folder_lookup(tree, "A");
    int b = jw_game_folder_lookup(tree, "A/B");
    int c = jw_game_folder_lookup(tree, "A/B/C");
    expect_int("folder count", jw_game_folder_count(tree), 4);
    expect_true("A, B, C exist", a > 0 && b > 0 && c > 0);

    /* 6. Counts, placement, links, strings. */
    expect_int("count root", jw_game_folder_game_count(tree, 0), 6);
    expect_int("count A", jw_game_folder_game_count(tree, a), 5);
    expect_int("count B", jw_game_folder_game_count(tree, b), 3);
    expect_int("count C", jw_game_folder_game_count(tree, c), 1);
    expect_int("of_game normal", jw_game_folder_of_game(tree, 0), a);
    expect_int("of_game nested", jw_game_folder_of_game(tree, 1), b);
    expect_int("of_game promoted", jw_game_folder_of_game(tree, 3), b);
    expect_int("of_game marker", jw_game_folder_of_game(tree, 4), a);
    expect_int("of_game root", jw_game_folder_of_game(tree, 5), 0);
    expect_int("of_game below range", jw_game_folder_of_game(tree, -1), -1);
    expect_int("of_game above range", jw_game_folder_of_game(tree, 6), -1);
    expect_int("collapsed C", jw_game_folder_collapsed_game(tree, c), 3);
    expect_int("collapsed B", jw_game_folder_collapsed_game(tree, b), -1);
    expect_int("collapsed unknown", jw_game_folder_collapsed_game(tree, 99), -1);
    expect_int("parent root", jw_game_folder_parent(tree, 0), -1);
    expect_int("parent A", jw_game_folder_parent(tree, a), 0);
    expect_int("parent B", jw_game_folder_parent(tree, b), a);
    expect_int("parent C", jw_game_folder_parent(tree, c), b);
    expect_int("parent unknown", jw_game_folder_parent(tree, 99), -1);
    expect_str("name root", jw_game_folder_name(tree, 0), "");
    expect_str("path root", jw_game_folder_path(tree, 0), "");
    expect_str("name B", jw_game_folder_name(tree, b), "B");
    expect_str("path B", jw_game_folder_path(tree, b), "A/B");
    expect_str("path C", jw_game_folder_path(tree, c), "A/B/C");
    expect_str("name unknown", jw_game_folder_name(tree, 99), "");
    expect_int("count unknown", jw_game_folder_game_count(tree, -1), 0);
    expect_str("rows root", render_rows(tree, 0), "[A] #5");
    expect_str("rows A", render_rows(tree, a), "[B] #0 #4");
    expect_str("rows B", render_rows(tree, b), "#1 #2 #3");

    /* Lookup sees collapsed folders, never marker folders. */
    expect_int("lookup root", jw_game_folder_lookup(tree, ""), 0);
    expect_int("lookup collapsed", jw_game_folder_lookup(tree, "a/b/c"), c);
    expect_int("lookup marker folder", jw_game_folder_lookup(tree, "A/Quest"), -1);
    expect_int("lookup unknown", jw_game_folder_lookup(tree, "Nope"), -1);
    expect_int("lookup NULL", jw_game_folder_lookup(tree, NULL), -1);
    expect_int("lookup stray slashes", jw_game_folder_lookup(tree, "/A//B/"), b);

    /* 7. Resolve. */
    expect_int("resolve existing", jw_game_folder_resolve(tree, "A/B"), b);
    expect_int("resolve without case", jw_game_folder_resolve(tree, "a/b"), b);
    expect_int("resolve collapsed", jw_game_folder_resolve(tree, "A/B/C"), b);
    expect_int("resolve unknown deep", jw_game_folder_resolve(tree, "A/B/X/Y"), b);
    expect_int("resolve below collapsed", jw_game_folder_resolve(tree, "A/B/C/D"), b);
    expect_int("resolve marker folder", jw_game_folder_resolve(tree, "A/Quest"), a);
    expect_int("resolve root", jw_game_folder_resolve(tree, ""), 0);
    expect_int("resolve NULL", jw_game_folder_resolve(tree, NULL), 0);
    expect_int("resolve unknown top", jw_game_folder_resolve(tree, "Nope"), 0);

    /* 8. Row for child. */
    expect_int("child folder row", jw_game_folder_row_for_child(tree, a, "A/B"), 0);
    expect_int("child deeper path",
               jw_game_folder_row_for_child(tree, a, "A/B/C/deeper"), 0);
    expect_int("child without case", jw_game_folder_row_for_child(tree, 0, "a/b"), 0);
    expect_int("child collapsed -> game row",
               jw_game_folder_row_for_child(tree, b, "A/B/C"), 2);
    expect_int("child collapsed deeper",
               jw_game_folder_row_for_child(tree, b, "A/B/C/gone"), 2);
    expect_int("child not below", jw_game_folder_row_for_child(tree, a, "Other/x"), -1);
    expect_int("child is the folder", jw_game_folder_row_for_child(tree, a, "A"), -1);
    expect_int("child above", jw_game_folder_row_for_child(tree, b, "A"), -1);
    expect_int("child gone", jw_game_folder_row_for_child(tree, a, "A/Gone"), -1);
    expect_int("child marker folder",
               jw_game_folder_row_for_child(tree, a, "A/Quest"), -1);
    expect_int("child of collapsed",
               jw_game_folder_row_for_child(tree, c, "A/B/C/x"), -1);
    expect_int("child NULL", jw_game_folder_row_for_child(tree, a, NULL), -1);
    expect_int("child unknown folder",
               jw_game_folder_row_for_child(tree, 99, "A"), -1);

    /* 9. Rows arguments. */
    expect_int("rows collapsed", jw_game_folder_rows(tree, c, NULL, 0), -1);
    expect_int("rows negative id", jw_game_folder_rows(tree, -1, NULL, 0), -1);
    expect_int("rows past end",
               jw_game_folder_rows(tree, jw_game_folder_count(tree), NULL, 0), -1);
    expect_int("rows count only", jw_game_folder_rows(tree, a, NULL, 0), 3);
    expect_int("rows NULL with max", jw_game_folder_rows(tree, a, NULL, 5), 3);
    {
        jw_game_folder_row out[3];
        memset(out, 0x5a, sizeof(out));
        expect_int("rows short buffer", jw_game_folder_rows(tree, a, out, 1), 3);
        expect_int("rows short buffer first kind", (int)out[0].kind,
                   JW_GAME_FOLDER_ROW_FOLDER);
        expect_int("rows short buffer first index", out[0].index, b);
        unsigned char untouched[sizeof(out[1])];
        memset(untouched, 0x5a, sizeof(untouched));
        expect_true("rows short buffer stops at max",
                    memcmp(&out[1], untouched, sizeof(untouched)) == 0);
    }
    jw_game_folder_tree_free(tree);
}

/* Reload contract: the same place resolved against rebuilt trees. */
static void test_reload_resolve(void) {
    {
        const char *paths[] = { "SFC/Top Gear/Top Gear (USA).zip",
                                "SFC/Top Gear/Top Gear (Europe).zip",
                                "SFC/Other.zip" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        int top = jw_game_folder_lookup(tree, "Top Gear");
        expect_int("reload: folder survives", jw_game_folder_resolve(tree, "Top Gear"),
                   top);
        expect_int("reload: folder row", jw_game_folder_row_for_child(tree, 0, "Top Gear"),
                   0);
        jw_game_folder_tree_free(tree);
    }
    {
        /* Europe hidden: USA alone matches the name, so the folder collapses. */
        const char *paths[] = { "SFC/Top Gear/Top Gear (USA).zip", "SFC/Other.zip" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        expect_int("reload: now collapses", jw_game_folder_resolve(tree, "Top Gear"), 0);
        expect_str("reload: collapsed root rows", render_rows(tree, 0), "#0 #1");
        expect_int("reload: row for collapsed folder",
                   jw_game_folder_row_for_child(tree, 0, "Top Gear"), 0);
        jw_game_folder_tree_free(tree);
    }
    {
        const char *paths[] = { "SFC/Other.zip" };
        fixture fx = { paths, COUNT(paths), NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        expect_int("reload: games gone", jw_game_folder_resolve(tree, "Top Gear"), 0);
        expect_int("reload: no row for gone folder",
                   jw_game_folder_row_for_child(tree, 0, "Top Gear"), -1);
        jw_game_folder_tree_free(tree);
    }
    {
        jw_game_folder_tree *tree = jw_game_folder_tree_build(0, NULL, NULL, NULL);
        expect_int("reload: system empty", jw_game_folder_resolve(tree, "Top Gear"), 0);
        jw_game_folder_tree_free(tree);
    }
}

/* --- 10. Edge cases --------------------------------------------------- */

static void test_edges(void) {
    {
        jw_game_folder_tree *tree = jw_game_folder_tree_build(0, NULL, NULL, NULL);
        expect_true("empty: built", tree != NULL);
        expect_int("empty: only the root", jw_game_folder_count(tree), 1);
        expect_int("empty: no rows", jw_game_folder_rows(tree, 0, NULL, 0), 0);
        expect_int("empty: no games", jw_game_folder_game_count(tree, 0), 0);
        expect_int("empty: resolve", jw_game_folder_resolve(tree, "x"), 0);
        expect_int("empty: lookup", jw_game_folder_lookup(tree, "x"), -1);
        expect_str("empty: root path", jw_game_folder_path(tree, 0), "");
        check_invariants("empty", tree, 0);
        jw_game_folder_tree_free(tree);
    }
    expect_true("negative count", jw_game_folder_tree_build(-1, fixture_relpath, NULL,
                                                            NULL) == NULL);
    expect_true("missing relpath", jw_game_folder_tree_build(1, NULL, NULL, NULL) == NULL);
    jw_game_folder_tree_free(NULL);
    expect_int("NULL tree count", jw_game_folder_count(NULL), 0);
    expect_int("NULL tree rows", jw_game_folder_rows(NULL, 0, NULL, 0), -1);
    expect_int("NULL tree resolve", jw_game_folder_resolve(NULL, ""), -1);

    {
        const char *paths[] = {
            "SFC/game.zip",
            "game.zip",
            "",
            NULL,
            "SFC//Top Gear///a.zip",
            "/SFC/Top Gear/b.zip",
            "SFC/Dir/",
        };
        fixture fx = { paths, COUNT(paths), "never", 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("odd relpaths", tree, COUNT(paths));
        expect_int("system folder and file: root", jw_game_folder_of_game(tree, 0), 0);
        expect_int("bare file: root", jw_game_folder_of_game(tree, 1), 0);
        expect_int("empty relpath: root", jw_game_folder_of_game(tree, 2), 0);
        expect_int("NULL relpath: root", jw_game_folder_of_game(tree, 3), 0);
        expect_int("trailing slash: root", jw_game_folder_of_game(tree, 6), 0);
        int top = jw_game_folder_lookup(tree, "Top Gear");
        expect_int("empty segments: one folder", jw_game_folder_count(tree), 2);
        expect_int("empty segments: both games", jw_game_folder_game_count(tree, top), 2);
        expect_str("empty segments: path", jw_game_folder_path(tree, top), "Top Gear");
        expect_str("empty segments: marker arguments", fx.marker_log,
                   "Top Gear|a.zip;Top Gear|b.zip;");
        jw_game_folder_tree_free(tree);
    }
    {
        /* 500 bytes: nine 50-byte folders below PS, then a 38-byte file. */
        char rel[512] = "PS";
        for (int k = 0; k < 9; k++) {
            size_t used = strlen(rel);
            rel[used] = '/';
            memset(rel + used + 1, 'A' + k, 50);
            rel[used + 51] = '\0';
        }
        append(rel, sizeof(rel), "/%s", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.chd");
        expect_int("long relpath length", (int)strlen(rel), 500);
        char middle[512];
        size_t middle_len = (size_t)(strrchr(rel, '/') - (rel + 3));
        memcpy(middle, rel + 3, middle_len);
        middle[middle_len] = '\0';
        const char *paths[] = { rel };
        fixture fx = { paths, 1, NULL, 0, "" };
        jw_game_folder_tree *tree = build(&fx);
        check_invariants("long relpath", tree, 1);
        int folder = jw_game_folder_of_game(tree, 0);
        expect_int("long relpath: depth", jw_game_folder_count(tree), 10);
        expect_str("long relpath: path", jw_game_folder_path(tree, folder), middle);
        expect_int("long relpath: lookup", jw_game_folder_lookup(tree, middle), folder);
        expect_int("long relpath: resolve", jw_game_folder_resolve(tree, middle), folder);
        expect_int("long relpath: row from root",
                   jw_game_folder_row_for_child(tree, 0, middle), 0);
        jw_game_folder_tree_free(tree);
    }
}

/* --- 11. A large synthetic system ------------------------------------- */

static void test_large(void) {
    enum { GAMES = 10000, GROUPS = 20, SUBS = 50 };
    char (*storage)[96] = calloc(GAMES, sizeof(*storage));
    const char **paths = calloc(GAMES, sizeof(*paths));
    bool *per_game = calloc(GAMES, sizeof(*per_game));
    if (!storage || !paths || !per_game) {
        fprintf(stderr, "large: out of memory\n");
        failures++;
        free(storage);
        free(paths);
        free(per_game);
        return;
    }
    /* Two in five games sit in their own folder (4,000 collapsing folders);
       the rest spread over 20 groups of 50 subfolders, spelled through two
       alias system folders and two cases. */
    int disc = 0;
    int grouped = 0;
    for (int i = 0; i < GAMES; i++) {
        const char *system = (i & 1) ? "PSX" : "PS";
        if (i % 5 < 2) {
            snprintf(storage[i], sizeof(storage[i]),
                     "%s/Disc Game %04d/Disc Game %04d (USA).chd", system, disc, disc);
            per_game[i] = true;
            disc++;
        } else {
            int group = grouped % GROUPS;
            int sub = (grouped / GROUPS) % SUBS;
            snprintf(storage[i], sizeof(storage[i]), "%s/%s %02d/Sub %03d/Title %05d.chd",
                     system, ((grouped / GROUPS) & 1) ? "group" : "Group", group,
                     sub, i);
            grouped++;
        }
        paths[i] = storage[i];
    }
    fixture fx = { paths, GAMES, "RPG_RT.ldb", 0, "" };
    jw_game_folder_tree *tree = build(&fx);
    if (!tree) {
        fprintf(stderr, "large: build failed\n");
        failures++;
    } else {
        check_invariants("large", tree, GAMES);
        expect_int("large: folders", jw_game_folder_count(tree),
                   1 + disc + GROUPS + GROUPS * SUBS);
        expect_int("large: games", jw_game_folder_game_count(tree, 0), GAMES);
        int collapsed = 0;
        for (int f = 0; f < jw_game_folder_count(tree); f++) {
            collapsed += jw_game_folder_collapsed_game(tree, f) >= 0;
        }
        expect_int("large: collapsed", collapsed, disc);
        expect_int("large: root rows", jw_game_folder_rows(tree, 0, NULL, 0),
                   GROUPS + disc);
        for (int g = 0; g < GROUPS; g++) {
            char lower[32];
            char upper[32];
            char child[48];
            snprintf(lower, sizeof(lower), "group %02d", g);
            snprintf(upper, sizeof(upper), "Group %02d", g);
            snprintf(child, sizeof(child), "Group %02d/Sub 001", g);
            int group = jw_game_folder_lookup(tree, lower);
            expect_str(lbl("large: group %d spelling", g),
                       jw_game_folder_name(tree, group), upper);
            expect_int(lbl("large: group %d count", g),
                       jw_game_folder_game_count(tree, group), (GAMES - disc) / GROUPS);
            expect_int(lbl("large: group %d rows", g),
                       jw_game_folder_rows(tree, group, NULL, 0), SUBS);
            expect_int(lbl("large: group %d row from root", g),
                       jw_game_folder_row_for_child(tree, 0, child), g);
        }
        int mismatched = 0;
        for (int i = 0; i < GAMES; i++) {
            int expected = 0;
            if (!per_game[i]) {
                const char *rest = strchr(paths[i], '/') + 1;
                char folder[96];
                size_t len = (size_t)(strrchr(rest, '/') - rest);
                memcpy(folder, rest, len);
                folder[len] = '\0';
                expected = jw_game_folder_lookup(tree, folder);
            }
            mismatched += jw_game_folder_of_game(tree, i) != expected;
        }
        expect_int("large: every game placed", mismatched, 0);
        {
            int row = jw_game_folder_row_for_child(tree, 0, "Disc Game 0123");
            jw_game_folder_row out[GROUPS + 4000];
            int total = jw_game_folder_rows(tree, 0, out, GROUPS + 4000);
            expect_int("large: root rows fit", total, GROUPS + 4000);
            expect_true("large: collapsed game row",
                        row >= GROUPS && row < total &&
                        out[row].kind == JW_GAME_FOLDER_ROW_GAME &&
                        strstr(paths[out[row].index], "Disc Game 0123 (USA)") != NULL);
            expect_int("large: resolve collapsed",
                       jw_game_folder_resolve(tree, "Disc Game 0123"), 0);
        }
        jw_game_folder_tree_free(tree);
    }
    free(storage);
    free(paths);
    free(per_game);
}

int main(void) {
    test_collapse_table();
    test_collapse_rules();
    test_merge();
    test_order_independence();
    test_row_order();
    test_queries();
    test_reload_resolve();
    test_edges();
    test_large();

    if (failures) {
        fprintf(stderr, "%d game folder check(s) failed\n", failures);
        return 1;
    }
    puts("Game folder checks passed");
    return 0;
}
