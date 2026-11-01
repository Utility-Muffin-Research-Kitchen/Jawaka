/* Show Folders through the real launcher handlers, renderers and resume file,
   with a disposable library and SDL renderer. The folder model has its own
   tests (game_folders_test.c); this one catches the launcher wiring: a row that
   shows one game while an action targets another, a reload that loses your
   place, a resume that cannot find its folder. */
static char folders_resume_path[4096];
#define JW_RESUME_PATH folders_resume_path
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>

static void shot(const char *name) {
    const char *dir = getenv("JW_FOLDERS_SHOT_DIR");
    if (!dir || !dir[0]) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    SDL_Surface *pixels = SDL_CreateRGBSurfaceWithFormat(0, cat_get_screen_width(),
        cat_get_screen_height(), 32, SDL_PIXELFORMAT_RGBA32);
    assert(pixels);
    assert(SDL_RenderReadPixels(cat_get_renderer(), NULL, pixels->format->format,
                               pixels->pixels, pixels->pitch) == 0);
    assert(IMG_SavePNG(pixels, path) == 0);
    SDL_FreeSurface(pixels);
}

static void add_game(sqlite3 *db, const char *name, const char *relpath) {
    char rom_path[600];
    snprintf(rom_path, sizeof(rom_path), "Roms/%s", relpath);
    assert(jw_db_insert_game_stable(db, "SFC", name, "primary", relpath, rom_path,
                                    "", "", "") == 0);
}

static jw_game_entry game_at(const jw_launcher_state *state, const char *relpath) {
    char rom_path[600];
    snprintf(rom_path, sizeof(rom_path), "Roms/%s", relpath);
    jw_game_entry game;
    assert(jw_db_get_game_by_rom_path(state->db_path, rom_path, &game) == 0);
    return game;
}

static void press(jw_launcher_state *state, cat_button button) {
    bool running = true;
    jw__handle_game_browser_input("", state->db_path, state, button, &running);
    assert(running);
}

static void hide(jw_launcher_state *state, const jw_game_entry *game) {
    bool running = true;
    state->action_scope = JW_ACTION_GAME;
    state->action_game = *game;
    state->actions_open = true;
    jw__hide_action_game("", state->db_path, state, &running);
    assert(running && !state->actions_open);
}

static int action_row(const jw_launcher_state *state, jw_action_row_kind kind) {
    for (int i = 0; i < state->action_row_count; i++) {
        if (state->action_rows[i] == kind) return i;
    }
    return -1;
}

/* Game Options, then the pin row, as X and A would do it. */
static void toggle_pin(jw_launcher_state *state, const jw_game_entry *game, bool now_pinned) {
    bool running = true;
    jw__open_game_actions(state->db_path, state, game);
    assert(state->action_pinned == now_pinned);
    int row = action_row(state, JW_ACTION_ROW_PIN);
    assert(row >= 0);
    char title[128], value[128];
    jw__action_row_strings(state, JW_ACTION_ROW_PIN, title, sizeof(title), value, sizeof(value));
    assert(strcmp(title, now_pinned ? "Unpin" : "Pin to Top") == 0);
    cat_list_state_jump(&state->action_list, row, state->action_row_count);
    jw__select_action_row("", state->db_path, state, &running);
    assert(running && !state->actions_open);
}

/* The row whose drawn label is `label` ("Name/" for a folder), or -1. */
static int row_labeled(jw_launcher_state *state, const char *label) {
    for (int row = 0; row < jw__browse_count(state); row++) {
        if (strcmp(jw__gg_name(state, row), label) == 0) return row;
    }
    return -1;
}

static void select_row(jw_launcher_state *state, int row) {
    assert(row >= 0);
    cat_list_state_jump(&state->game_list, row, jw__browse_count(state));
}

/* Every row draws, labels and acts on the same thing: the list drawer, the Grid
   and Coverflow name callbacks, the letter-jump label and the selected game. */
static void check_rows_agree(jw_launcher_state *state) {
    int saved = state->game_list.cursor;
    for (int row = 0; row < jw__browse_count(state); row++) {
        const jw_game_entry *game = jw__browse_game(state, row);
        int folder = jw__browse_folder_at(state, row);
        assert((game != NULL) != (folder >= 0));
        char want[300];
        if (game) jw__clean_rom_name(game->name, want, sizeof(want));
        else      snprintf(want, sizeof(want), "%s/",
                           jw_game_folder_name(state->folder_tree, folder));
        assert(strcmp(jw__gg_name(state, row), want) == 0);
        const char *label = game ? game->name
                                 : jw_game_folder_name(state->folder_tree, folder);
        assert(strcmp(jw__games_label_cb(row, state), label) == 0);
        select_row(state, row);
        assert(jw__browse_selected_game(state) == game);
    }
    select_row(state, saved);
}

static void render_all_layouts(jw_launcher_state *state, const char *tag) {
    cat_stylesheet *style = (cat_stylesheet *)cat_get_stylesheet();
    char name[64];
    style->launcher.layout = CAT_LAUNCHER_TABBED;
    jw__render_game_browser(state);
    snprintf(name, sizeof(name), "%s-list", tag);
    shot(name);
    style->launcher.layout = CAT_LAUNCHER_COVERFLOW;
    jw__render_coverflow_games(state);
    snprintf(name, sizeof(name), "%s-coverflow", tag);
    shot(name);
    style->launcher.layout = CAT_LAUNCHER_GRID;
    jw__render_grid_games(state);
    snprintf(name, sizeof(name), "%s-grid", tag);
    shot(name);
    style->launcher.layout = CAT_LAUNCHER_TABBED;
}

int main(void) {
    char root[] = "/tmp/jw-folders-ui-XXXXXX";
    assert(mkdtemp(root));
    snprintf(folders_resume_path, sizeof(folders_resume_path), "%s/resume", root);
    setenv("SDCARD_PATH", root, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("CAT_THEMES_DIR", "res/themes", 1);
    setenv("CAT_THEME_NAME", "Jawaka-Tabs", 1);
    char font[PATH_MAX];
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = { .start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font };
    assert(cat_init(&config) == CAT_OK);
    /* Draw without presenting: an idle cat_present() sleeps to the next minute. */
    g_defer_present = true;

    jw_launcher_state *state = calloc(1, sizeof(*state));
    assert(state);
    snprintf(state->db_path, sizeof(state->db_path), "%s/library.db", root);
    snprintf(state->sdcard_root, sizeof(state->sdcard_root), "%s", root);
    state->current_tab = JW_TAB_GAMES;
    state->visible_tab_count = JW_TAB_COUNT;
    for (int i = 0; i < JW_TAB_COUNT; ++i) state->visible_tabs[i] = (jw_tab)i;
    state->haptics_muted = true;
    cat_list_state_init(&state->list, 7);

    /* A deep folder chain whose game's rom_relpath is 490 bytes: longer than the
       resume reader's old 160-byte lines. */
    char deep[600] = "SFC/Deep";
    while (strlen(deep) < 440) strcat(deep, "/A folder name long enough to add up");
    char deep_game[600];
    int pad = 490 - (int)strlen(deep) - (int)strlen("/Long Game.zip");
    assert(pad > 0);
    snprintf(deep_game, sizeof(deep_game), "%s/Long Game%.*s.zip", deep, pad,
             "--------------------------------------------------------------");
    assert(strlen(deep_game) == 490);

    sqlite3 *db = NULL;
    assert(jw_db_open(state->db_path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    assert(jw_db_scan_begin(db) == 0);
    add_game(db, "Alpha", "SFC/Alpha.zip");
    add_game(db, "Top Gear (USA)", "SFC/Top Gear/Top Gear (USA).zip");
    add_game(db, "Top Gear 2 (USA)", "SFC/Top Gear/Top Gear 2 (USA).zip");
    add_game(db, "Kaizo World", "SFC/Hacks/Kaizo World.zip");
    add_game(db, "Seiken Densetsu 3", "SFC/Hacks/Translated/Seiken Densetsu 3.zip");
    add_game(db, "Zelda (USA)", "SFC/Zelda/Zelda (USA).zip");
    add_game(db, "Long Game", deep_game);
    jw_db_close(db);
    const int total = 7;

    jw_settings_ui_init(&state->settings, state->db_path, "Jawaka-Tabs", "");
    assert(!state->settings.show_folders);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);

    /* Off: today's flat list, row i is game i, no tree. */
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);
    assert(!state->folder_tree && jw__browse_count(state) == total);
    for (int row = 0; row < total; row++) assert(jw__browse_game(state, row) == &state->games[row]);
    check_rows_agree(state);
    render_all_layouts(state, "off");

    /* Pinned, flat: first by rank, whichever folder the game sits in. */
    jw_game_entry flat_zelda = game_at(state, "SFC/Zelda/Zelda (USA).zip");
    assert(jw_db_set_game_setting(state->db_path, flat_zelda.id, JW_GAME_SETTING_SORT_RANK, "1") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(jw__browse_game(state, 0)->id == flat_zelda.id);
    assert(jw_db_delete_game_setting(state->db_path, flat_zelda.id, JW_GAME_SETTING_SORT_RANK) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);

    /* Game Options pins a game: it goes to the top, the selection goes with it, and the
       row turns into Unpin. A second pin goes after the first. */
    jw_game_entry opt_zelda = game_at(state, "SFC/Zelda/Zelda (USA).zip");
    jw_game_entry opt_alpha = game_at(state, "SFC/Alpha.zip");
    select_row(state, jw__browse_find_game(state, opt_zelda.source_id, opt_zelda.rom_relpath));
    toggle_pin(state, &opt_zelda, false);
    assert(state->game_list.cursor == 0 && jw__browse_selected_game(state)->id == opt_zelda.id);
    assert(strcmp(state->status, "Pinned to the top of this list.") == 0);
    select_row(state, jw__browse_find_game(state, opt_alpha.source_id, opt_alpha.rom_relpath));
    toggle_pin(state, &opt_alpha, false);
    assert(state->game_list.cursor == 1 && jw__browse_selected_game(state)->id == opt_alpha.id);
    assert(jw__browse_game(state, 0)->id == opt_zelda.id);
    check_rows_agree(state);
    render_all_layouts(state, "pinned-from-options");

    /* Unpinning puts it back in title order, still selected. */
    toggle_pin(state, &opt_alpha, true);
    assert(jw__browse_game(state, 0)->id == opt_zelda.id);
    assert(jw__browse_selected_game(state)->id == opt_alpha.id && state->game_list.cursor == 1);
    assert(strcmp(state->status, "Unpinned.") == 0);
    toggle_pin(state, &opt_zelda, true);
    char opt_rank[16] = "";
    assert(jw_db_get_game_setting(state->db_path, opt_zelda.id, JW_GAME_SETTING_SORT_RANK,
                                  opt_rank, sizeof(opt_rank)) != 0 || !opt_rank[0]);

    /* A pin goes after a folder's rank too, which a game can't see from its own level. */
    assert(jw_db_set_system_setting(state->db_path, "SFC", JW_SYSTEM_SETTING_FOLDER_RANK_PREFIX "hacks", "7") == 0);
    assert(jw_db_set_game_pinned(state->db_path, opt_alpha.id, 1) == 0);
    assert(jw_db_get_game_setting(state->db_path, opt_alpha.id, JW_GAME_SETTING_SORT_RANK,
                                  opt_rank, sizeof(opt_rank)) == 0 && strcmp(opt_rank, "8") == 0);
    assert(jw_db_set_game_pinned(state->db_path, opt_alpha.id, 0) == 0);
    assert(jw_db_delete_system_setting(state->db_path, "SFC", JW_SYSTEM_SETTING_FOLDER_RANK_PREFIX "hacks") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);

    /* Reload, flat: a title edit moves rows, and the selection stays on its game. */
    jw_game_entry alpha = game_at(state, "SFC/Alpha.zip");
    jw_game_entry top_gear = game_at(state, "SFC/Top Gear/Top Gear (USA).zip");
    select_row(state, row_labeled(state, "Top Gear"));
    int top_gear_row = state->game_list.cursor;
    assert(jw_db_set_game_setting(state->db_path, alpha.id, "display_name", "Zz Last") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == top_gear_row - 1);
    assert(jw__browse_selected_game(state)->id == top_gear.id);

    /* Renaming the selected game itself carries the selection along. */
    assert(jw_db_set_game_setting(state->db_path, top_gear.id, "display_name", "Aa First") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == 0 && jw__browse_selected_game(state)->id == top_gear.id);
    assert(jw_db_delete_game_setting(state->db_path, alpha.id, "display_name") == 0);
    assert(jw_db_delete_game_setting(state->db_path, top_gear.id, "display_name") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == top_gear_row && jw__browse_selected_game(state)->id == top_gear.id);

    /* A game that's gone leaves the cursor on its old row, and the last row at worst. */
    select_row(state, 2);
    jw_game_entry third = *jw__browse_selected_game(state);
    assert(jw_db_set_game_hidden(state->db_path, third.id, 1) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == 2 && jw__browse_selected_game(state)->id != third.id);
    assert(jw_db_set_game_hidden(state->db_path, third.id, 0) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    select_row(state, total - 2);
    jw_game_entry second_last = state->games[total - 2];
    jw_game_entry very_last = state->games[total - 1];
    assert(jw_db_set_game_hidden(state->db_path, second_last.id, 1) == 0);
    assert(jw_db_set_game_hidden(state->db_path, very_last.id, 1) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == total - 3);
    assert(jw_db_set_game_hidden(state->db_path, second_last.id, 0) == 0);
    assert(jw_db_set_game_hidden(state->db_path, very_last.id, 0) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    check_rows_agree(state);

    /* Favorites is flat too, and follows its game through the same reload. */
    assert(jw_db_set_favorite(state->db_path, "game", alpha.id, 1) == 0);
    assert(jw_db_set_favorite(state->db_path, "game", top_gear.id, 1) == 0);
    jw__close_game_browser(state);
    assert(jw__open_favorites(state->db_path, state) == 0);
    select_row(state, 1);
    assert(jw__browse_selected_game(state)->id == top_gear.id);
    /* Ranks don't order Favorites, so Game Options offers no pin there. */
    jw__open_game_actions(state->db_path, state, jw__browse_selected_game(state));
    assert(action_row(state, JW_ACTION_ROW_PIN) < 0);
    state->actions_open = false;
    state->action_scope = JW_ACTION_NONE;
    assert(jw_db_set_favorite(state->db_path, "game", alpha.id, 0) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == 0 && jw__browse_selected_game(state)->id == top_gear.id);
    assert(jw_db_set_favorite(state->db_path, "game", top_gear.id, 0) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(!state->games_open);
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);

    /* Turning it on in Settings > Games rebuilds the open list at its root. */
    jw_settings_ui_open(&state->settings, JW_SETTINGS_GAMES);
    cat_list_state_jump(&state->settings.games_list, JW_GAMES_SHOW_FOLDERS, JW_GAMES_ROW_COUNT);
    bool theme_changed = false;
    jw__system_settings_input(state, &state->settings, CAT_BTN_A, &theme_changed);
    jw_settings_ui_close(&state->settings);
    assert(state->settings.show_folders && state->folder_tree && state->browse_folder == 0);
    char value[16] = "";
    assert(jw_db_get_setting(state->db_path, JW_SHOW_FOLDERS_SETTING_KEY, value, sizeof(value)) == 0);
    assert(strcmp(value, "1") == 0);

    /* Root: folders first by name, then the root's games in title order, Zelda
       promoted out of its own one-game folder. */
    assert(jw__browse_count(state) == 5);
    assert(strcmp(jw__gg_name(state, 0), "Deep/") == 0);
    assert(strcmp(jw__gg_name(state, 1), "Hacks/") == 0);
    assert(strcmp(jw__gg_name(state, 2), "Top Gear/") == 0);
    assert(strcmp(jw__gg_name(state, 3), "Alpha") == 0);
    assert(strcmp(jw__gg_name(state, 4), "Zelda") == 0);
    check_rows_agree(state);

    /* Folder rows: no favorite, no actions, A opens, B comes back onto them. */
    select_row(state, row_labeled(state, "Top Gear/"));
    press(state, CAT_BTN_Y);
    jw_game_entry favorites[4];
    int favorite_count = -1;
    assert(jw_db_list_favorite_games(state->db_path, favorites, 4, &favorite_count) == 0);
    assert(favorite_count == 0);
    /* X goes through the whole dispatcher: a folder row must neither open
       actions nor fall through to Search the way an empty list does. */
    bool running = true;
    jw__handle_input("", state->db_path, state, CAT_BTN_X, &running);
    assert(running && !state->actions_open && !state->search_open);
    assert(state->game_list.cursor == row_labeled(state, "Top Gear/"));

    /* Coverflow's Up/Down letter jump lands on folders too. */
    cat_stylesheet *style = (cat_stylesheet *)cat_get_stylesheet();
    style->launcher.layout = CAT_LAUNCHER_COVERFLOW;
    select_row(state, row_labeled(state, "Deep/"));
    press(state, CAT_BTN_DOWN);
    assert(state->game_list.cursor == row_labeled(state, "Hacks/"));
    press(state, CAT_BTN_UP);
    assert(state->game_list.cursor == row_labeled(state, "Deep/"));
    style->launcher.layout = CAT_LAUNCHER_TABBED;
    select_row(state, row_labeled(state, "Top Gear/"));
    render_all_layouts(state, "root");
    press(state, CAT_BTN_A);
    assert(state->browse_folder > 0 && jw__browse_count(state) == 2);
    char title[160];
    jw__browse_title(state, true, title, sizeof(title));
    assert(strlen(title) > 11 && strcmp(title + strlen(title) - 11, " / Top Gear") == 0);
    jw__browse_title(state, false, title, sizeof(title));
    assert(strcmp(title, "Top Gear") == 0);              /* the Grid's slot */
    check_rows_agree(state);
    render_all_layouts(state, "topgear");
    press(state, CAT_BTN_B);
    assert(state->browse_folder == 0 && state->game_list.cursor == row_labeled(state, "Top Gear/"));

    /* Two levels down and back up, each B landing on the folder it left. */
    select_row(state, row_labeled(state, "Hacks/"));
    press(state, CAT_BTN_A);
    select_row(state, row_labeled(state, "Translated/"));
    press(state, CAT_BTN_A);
    assert(jw__browse_count(state) == 1);
    press(state, CAT_BTN_B);
    assert(state->game_list.cursor == row_labeled(state, "Translated/"));
    assert(strcmp(state->status, "2 games") == 0);   /* the level you land on */
    press(state, CAT_BTN_B);
    assert(state->browse_folder == 0 && state->game_list.cursor == row_labeled(state, "Hacks/"));
    assert(strncmp(state->status, "7 ", 2) == 0);     /* the whole system at its root */

    /* A title cut inside a UTF-8 character drops the broken character. */
    char cut[] = "Pok\xC3\xA9mon / Caf\xC3";
    jw__utf8_trim_tail(cut);
    assert(strcmp(cut, "Pok\xC3\xA9mon / Caf") == 0);
    char whole[] = "Caf\xC3\xA9";
    jw__utf8_trim_tail(whole);
    assert(strcmp(whole, "Caf\xC3\xA9") == 0);
    char euro[] = "Price \xE2\x82";
    jw__utf8_trim_tail(euro);
    assert(strcmp(euro, "Price ") == 0);

    /* B at the root still closes the list. */
    press(state, CAT_BTN_B);
    assert(!state->games_open && !state->folder_tree);
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);

    /* Reload: hiding one of Top Gear's two games leaves a folder named after its
       last game, which collapses; you land at the root on that game. */
    select_row(state, row_labeled(state, "Top Gear/"));
    press(state, CAT_BTN_A);
    jw_game_entry tg2 = game_at(state, "SFC/Top Gear/Top Gear 2 (USA).zip");
    select_row(state, row_labeled(state, "Top Gear 2"));
    hide(state, &tg2);
    assert(state->games_open && state->browse_folder == 0);
    const jw_game_entry *selected = jw__browse_selected_game(state);
    assert(selected && strcmp(selected->rom_relpath, "SFC/Top Gear/Top Gear (USA).zip") == 0);
    assert(row_labeled(state, "Top Gear/") < 0);
    check_rows_agree(state);

    /* Reload: hiding Translated's only game takes the folder away; you land in
       Hacks, its nearest surviving ancestor. */
    select_row(state, row_labeled(state, "Hacks/"));
    press(state, CAT_BTN_A);
    select_row(state, row_labeled(state, "Translated/"));
    press(state, CAT_BTN_A);
    jw_game_entry seiken = game_at(state, "SFC/Hacks/Translated/Seiken Densetsu 3.zip");
    hide(state, &seiken);
    assert(state->games_open);
    assert(strcmp(jw_game_folder_path(state->folder_tree, state->browse_folder), "Hacks") == 0);
    assert(jw__browse_count(state) == 1 && jw__browse_selected_game(state));

    /* Reload: a title edit moves rows at this level, and the selection follows. */
    while (jw__browse_leave_folder(state)) {}
    jw_game_entry zelda = game_at(state, "SFC/Zelda/Zelda (USA).zip");
    select_row(state, row_labeled(state, "Zelda"));
    int zelda_row = state->game_list.cursor;
    jw_game_entry root_alpha = game_at(state, "SFC/Alpha.zip");
    assert(jw_db_set_game_setting(state->db_path, root_alpha.id, "display_name", "Zz Last") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == zelda_row - 1 && jw__browse_selected_game(state)->id == zelda.id);
    assert(jw_db_delete_game_setting(state->db_path, root_alpha.id, "display_name") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->game_list.cursor == zelda_row && jw__browse_selected_game(state)->id == zelda.id);

    /* Resume: from deep inside the chain, onto the long game, through a
       restart; then B lands on the folder the game sat in. */
    while (jw__browse_leave_folder(state)) {}
    int deep_folder = jw_game_folder_lookup(state->folder_tree, deep + 4);
    assert(deep_folder > 0 && jw__browse_set_level(state, deep_folder));
    jw_game_entry long_game = game_at(state, deep_game);
    select_row(state, jw__browse_find_game(state, long_game.source_id, long_game.rom_relpath));
    jw__save_resume(state);
    jw_resume resume;
    assert(jw__load_resume(&resume));
    assert(strcmp(resume.browse.game_relpath, deep_game) == 0);
    assert(strcmp(resume.browse.folder, deep + 4) == 0);
    jw__close_game_browser(state);
    jw__apply_resume(state->db_path, state, &resume);
    assert(state->games_open && state->browse_folder == deep_folder);
    selected = jw__browse_selected_game(state);
    assert(selected && strcmp(selected->rom_relpath, deep_game) == 0);
    int parent = jw_game_folder_parent(state->folder_tree, deep_folder);
    char deep_name[300];
    snprintf(deep_name, sizeof(deep_name), "%s/", jw_game_folder_name(state->folder_tree, deep_folder));
    press(state, CAT_BTN_B);
    assert(state->browse_folder == parent && state->game_list.cursor == row_labeled(state, deep_name));

    /* A line longer than the reader's buffer is skipped whole, never parsed in
       pieces: its tail here would otherwise read as "tab=6". */
    FILE *fp = fopen(folders_resume_path, "w");
    assert(fp);
    for (size_t i = 0; i < JW_BROWSE_PATH_MAX + 31; i++) fputc('x', fp);
    fputs("tab=6\ntab=2\ngames_open=0\n", fp);
    fclose(fp);
    assert(jw__load_resume(&resume) && resume.tab == 2);

    /* The 5-Game picker stays flat with Show Folders on, and A picks the game
       its row shows. */
    jw__close_game_browser(state);
    jw__focus_setup_begin(state);
    assert(state->focus_pick_active);
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);
    assert(!state->folder_tree && jw__browse_count(state) == state->game_count);
    select_row(state, 1);
    int picked = jw__browse_selected_game(state)->id;
    jw__handle_pick_input(state->db_path, state, CAT_BTN_A);
    bool found = false;
    for (int i = 0; i < state->focus_setup_count; i++) found |= state->focus_setup_ids[i] == picked;
    assert(found);
    jw__pick_cancel(state);
    assert(!state->focus_pick_active);

    /* Pinning from inside a folder leads that level and leaves the root alone. */
    jw_game_entry kaizo = game_at(state, "SFC/Hacks/Kaizo World.zip");
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);
    int root_rows = jw__browse_count(state);
    select_row(state, row_labeled(state, "Hacks/"));
    press(state, CAT_BTN_A);
    select_row(state, jw__browse_find_game(state, kaizo.source_id, kaizo.rom_relpath));
    toggle_pin(state, &kaizo, false);
    assert(state->browse_folder > 0 && jw__browse_selected_game(state)->id == kaizo.id);
    assert(state->game_list.cursor == 0);
    press(state, CAT_BTN_B);
    assert(jw__browse_count(state) == root_rows && row_labeled(state, "Hacks/") >= 0);
    assert(jw_db_set_game_pinned(state->db_path, kaizo.id, 0) == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);

    /* The Favorites tab draws its own plain list through the shared row drawer. */
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);
    select_row(state, row_labeled(state, "Alpha"));
    press(state, CAT_BTN_Y);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(state->favorites_count == 1);
    jw__close_game_browser(state);
    state->current_tab = JW_TAB_FAVORITES;
    jw__render_tabbed(state);
    shot("favorites-tab");
    state->current_tab = JW_TAB_GAMES;

    /* A pinned folder and game lead the level, and survive going in and out. */
    jw__close_game_browser(state);
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);
    jw_game_entry pinned_alpha = game_at(state, "SFC/Alpha.zip");
    assert(jw_db_set_game_setting(state->db_path, pinned_alpha.id, JW_GAME_SETTING_SORT_RANK, "1") == 0);
    assert(jw_db_set_system_setting(state->db_path, "SFC", JW_SYSTEM_SETTING_FOLDER_RANK_PREFIX "hacks", "2") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(strcmp(jw__gg_name(state, 0), "Alpha") == 0);
    assert(strcmp(jw__gg_name(state, 1), "Hacks/") == 0);
    check_rows_agree(state);
    render_all_layouts(state, "pinned");
    select_row(state, 1);
    press(state, CAT_BTN_A);
    press(state, CAT_BTN_B);
    assert(state->game_list.cursor == 1);
    assert(jw_db_delete_game_setting(state->db_path, pinned_alpha.id, JW_GAME_SETTING_SORT_RANK) == 0);
    assert(jw_db_delete_system_setting(state->db_path, "SFC", JW_SYSTEM_SETTING_FOLDER_RANK_PREFIX "hacks") == 0);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    /* The selection stayed on Hacks through both reloads. */
    assert(jw__browse_folder_at(state, state->game_list.cursor) >= 0);
    assert(strcmp(jw__gg_name(state, state->game_list.cursor), "Hacks/") == 0);
    jw__close_game_browser(state);

    /* Turning it off rebuilds an open list flat again. */
    assert(jw__open_system_games(state->db_path, "SFC", state) == 0);
    assert(state->folder_tree);
    jw_settings_ui_open(&state->settings, JW_SETTINGS_GAMES);
    cat_list_state_jump(&state->settings.games_list, JW_GAMES_SHOW_FOLDERS, JW_GAMES_ROW_COUNT);
    jw__system_settings_input(state, &state->settings, CAT_BTN_A, &theme_changed);
    jw_settings_ui_close(&state->settings);
    assert(!state->settings.show_folders && !state->folder_tree);
    assert(jw__browse_count(state) == state->game_count);
    jw__system_settings_input(state, &state->settings, CAT_BTN_B, &theme_changed);

    /* Hiding the system's last games closes the list, as it does flat. */
    jw_settings_ui_open(&state->settings, JW_SETTINGS_GAMES);
    cat_list_state_jump(&state->settings.games_list, JW_GAMES_SHOW_FOLDERS, JW_GAMES_ROW_COUNT);
    jw__system_settings_input(state, &state->settings, CAT_BTN_A, &theme_changed);
    jw_settings_ui_close(&state->settings);
    assert(state->settings.show_folders && state->folder_tree);
    while (state->games_open && state->game_count > 0) {
        jw_game_entry last = state->games[0];
        hide(state, &last);
    }
    assert(!state->games_open);

    free(state);
    cat_quit();
    puts("folders-ui-test: off parity, setting rebuild, rows agree, folder input, "
         "reload contract, long resume, picker flat, favorites tab passed");
    return 0;
}
