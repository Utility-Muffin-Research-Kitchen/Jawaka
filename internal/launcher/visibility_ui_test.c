/* Use the real launcher handlers with a disposable library and SDL renderer. */
static char visibility_resume_path[4096];
#define JW_RESUME_PATH visibility_resume_path
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>

static void save_screenshot(const char *variable) {
    const char *path = getenv(variable);
    if (!path || !path[0]) return;
    SDL_Surface *pixels = SDL_CreateRGBSurfaceWithFormat(0, cat_get_screen_width(),
        cat_get_screen_height(), 32, SDL_PIXELFORMAT_RGBA32);
    assert(pixels);
    assert(SDL_RenderReadPixels(cat_get_renderer(), NULL, pixels->format->format,
                               pixels->pixels, pixels->pitch) == 0);
    assert(IMG_SavePNG(pixels, path) == 0);
    SDL_FreeSurface(pixels);
}

static void check_discs(jw_launcher_state *state, const char *root) {
    char path[512];
    snprintf(path, sizeof(path), "%s/Roms", root);
    assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/Roms/PS", root);
    assert(mkdir(path, 0755) == 0);
    struct { const char *name, *text; } files[] = {
        { "Adventure.m3u", "disc1.chd|Disc One\r\ndisc2.chd|Disc Two\r\n"
          "disc1.chd|Encore\r\n#SAVEDISK:Save Disk\r\n" },
        { "Single.m3u", "disc2.chd|Only Disc\n" },
        { "disc1.chd", "synthetic disc one" },
        { "disc2.chd", "synthetic disc two" },
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/Roms/PS/%s", root, files[i].name);
        FILE *fp = fopen(path, "wb");
        assert(fp && fputs(files[i].text, fp) >= 0);
        assert(fclose(fp) == 0);
    }
    sqlite3 *db = NULL;
    assert(jw_db_open(state->db_path, &db) == 0);
    assert(jw_db_scan_begin(db) == 0);
    for (int i = 0; i < 3; ++i) {
        snprintf(path, sizeof(path), "%s/Roms/PS/%s", root, files[i].name);
        char relative[64];
        snprintf(relative, sizeof(relative), "PS/%s", files[i].name);
        assert(jw_db_insert_game_stable(db, "PS", files[i].name, "primary", relative,
                                       path, "", "", "") == 0);
    }
    jw_db_close(db);
    jw_game_entry parent, single, child, lookup;
    assert(jw_db_get_game_by_source_relpath(state->db_path, "primary", "PS/Adventure.m3u", &parent) == 0);
    assert(jw_db_get_game_by_source_relpath(state->db_path, "primary", "PS/Single.m3u", &single) == 0);
    assert(jw_db_get_game_by_source_relpath(state->db_path, "primary", "PS/disc1.chd", &child) == 0);
    assert(jw_db_set_favorite(state->db_path, "game", child.id, 1) == 0);
    assert(jw_db_record_play_by_id(state->db_path, child.id, 30) == 0);
    state->menu_open = false;
    jw_settings_ui_close(&state->settings);
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(jw__open_system_games(state->db_path, "PS", state) == 0 && state->game_count == 3);
    jw__open_game_actions(state->db_path, state, &parent);
    assert(state->action_content.disc_count == 3 && state->disc_visible_count == 3);
    assert(state->action_rows[state->action_row_count - 2] == JW_ACTION_ROW_HIDE);
    assert(state->action_rows[state->action_row_count - 1] == JW_ACTION_ROW_DISCS);

    /* Exercise the longest Options menu at a larger accessibility font size. */
    state->action_core_count = 2;
    state->action_bios_supported = true;
    jw__action_refresh_rows(state);
    assert(state->action_row_count == 8);
    cat_list_state_jump(&state->action_list, state->action_row_count - 1, state->action_row_count);
    assert(cat_set_font_bump(5) == CAT_OK);
    jw__render_actions(state);
    assert(state->action_list.visible_rows < state->action_row_count);
    assert(state->action_list.cursor >= state->action_list.scroll_offset);
    assert(state->action_list.cursor < state->action_list.scroll_offset + state->action_list.visible_rows);
    assert(!SDL_RenderIsClipEnabled(cat_get_renderer()));
    save_screenshot("JW_DISCS_ACTIONS_SCREENSHOT");
    assert(cat_set_font_bump(0) == CAT_OK);
    bool running = true;
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(state->discs_open && state->disc_visible_count == 3);

    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(running && state->discs_open && state->action_content.disc_count == 3);
    assert(state->disc_visible_count == 1 && state->disc_indices[0] == 1);
    assert(state->disc_hidden[0] && state->disc_hidden[2]);
    assert(state->game_count == 2 && state->favorites_count == 0 && state->recents_count == 0);
    assert(jw_db_get_game_by_id(state->db_path, child.id, &lookup) == 0);
    assert(lookup.favorite && lookup.playtime_s == 30);
    assert(state->library_generation == 23);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_Y, &running);
    assert(state->discs_show_hidden && state->disc_visible_count == 3);
    for (int i = 0; i < 3; ++i) assert(state->disc_indices[i] == i);
    jw__render_actions(state);
    save_screenshot("JW_DISCS_SCREENSHOT");
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(!state->disc_hidden[0] && !state->disc_hidden[2] && state->game_count == 3);
    assert(state->favorites_count == 1 && state->recents_count == 1);

    /* Every disc can be hidden without hiding or rewriting either playlist. */
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_Y, &running);
    assert(!state->discs_show_hidden && state->disc_visible_count == 1);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(state->disc_visible_count == 0 && state->game_count == 2);
    int hidden = 1;
    assert(jw_db_is_game_hidden(state->db_path, parent.id, &hidden) == 0 && !hidden);
    assert(jw_db_get_game_by_id(state->db_path, parent.id, &lookup) == 0);
    assert(access(lookup.rom_path, R_OK) == 0);
    jw__render_actions(state);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_Y, &running);
    assert(state->disc_visible_count == 3);
    for (int i = 0; i < 3; ++i) assert(state->disc_indices[i] == i && state->disc_hidden[i]);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_B, &running);
    assert(!state->discs_open && state->actions_open);

    jw__open_game_actions(state->db_path, state, &single);
    assert(state->action_content.disc_count == 1 && state->disc_visible_count == 0);
    assert(state->action_rows[state->action_row_count - 1] == JW_ACTION_ROW_DISCS);
    cat_list_state_jump(&state->action_list, state->action_row_count - 1, state->action_row_count);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(state->discs_open);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_Y, &running);
    assert(state->disc_visible_count == 1 && state->disc_hidden[0]);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(!state->disc_hidden[0]);
    jw__refresh_discs(root, state); /* A directory is an unreadable visibility DB. */
    assert(state->disc_error[0] && state->disc_visible_count == 0);
    jw__render_actions(state);

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/Roms/PS/%s", root, files[i].name);
        FILE *fp = fopen(path, "rb");
        char bytes[256];
        assert(fp);
        size_t size = fread(bytes, 1, sizeof(bytes), fp);
        assert(fclose(fp) == 0 && size == strlen(files[i].text));
        assert(memcmp(bytes, files[i].text, size) == 0);
        assert(unlink(path) == 0);
    }
    jw__open_game_actions(state->db_path, state, &parent);
    assert(state->disc_error[0] && state->action_content.disc_count == 0);
    assert(state->action_rows[state->action_row_count - 1] == JW_ACTION_ROW_DISCS);
    cat_list_state_jump(&state->action_list, state->action_row_count - 1, state->action_row_count);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(state->discs_open && state->disc_error[0] && state->disc_visible_count == 0);
    jw__render_actions(state);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_B, &running);
    assert(state->actions_open && !state->discs_open);
    jw__clear_action_content(state);
    snprintf(path, sizeof(path), "%s/Roms/PS", root);
    assert(rmdir(path) == 0);
    snprintf(path, sizeof(path), "%s/Roms", root);
    assert(rmdir(path) == 0);
}

int main(void) {
    char root[] = "/tmp/jw-visibility-ui-XXXXXX";
    assert(mkdtemp(root));
    snprintf(visibility_resume_path, sizeof(visibility_resume_path), "%s/resume", root);
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

    jw_launcher_state *state = calloc(1, sizeof(*state));
    assert(state);
    snprintf(state->db_path, sizeof(state->db_path), "%s/library.db", root);
    snprintf(state->sdcard_root, sizeof(state->sdcard_root), "%s", root);
    state->current_tab = JW_TAB_GAMES;
    state->visible_tab_count = JW_TAB_COUNT;
    for (int i = 0; i < JW_TAB_COUNT; ++i) state->visible_tabs[i] = (jw_tab)i;
    state->haptics_muted = true;
    state->library_generation = 23;
    cat_list_state_init(&state->list, 7);

    sqlite3 *db = NULL;
    assert(jw_db_open(state->db_path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game_stable(db, "GBA", "Alpha", "primary", "GBA/Alpha.gba",
        "Roms/GBA/Alpha.gba", "", "", "") == 0);
    assert(jw_db_insert_game_stable(db, "GBA", "Bravo", "primary", "GBA/Bravo.gba",
        "Roms/GBA/Bravo.gba", "", "", "") == 0);
    jw_db_close(db);
    jw_game_entry alpha, bravo;
    assert(jw_db_get_game_by_rom_path(state->db_path, "Roms/GBA/Alpha.gba", &alpha) == 0);
    assert(jw_db_get_game_by_rom_path(state->db_path, "Roms/GBA/Bravo.gba", &bravo) == 0);
    assert(jw_db_set_favorite(state->db_path, "game", bravo.id, 1) == 0);
    assert(jw_db_record_play_by_id(state->db_path, bravo.id, 12) == 0);
    char ids[32];
    snprintf(ids, sizeof(ids), "%d", bravo.id);
    assert(jw_db_set_setting(state->db_path, JW_FOCUS_KEY_IDS, ids) == 0);
    jw_settings_ui_init(&state->settings, state->db_path, "Jawaka-Tabs", "");
    assert(jw__reload_library_from_db(state->db_path, state) == 0);
    assert(jw__open_system_games(state->db_path, "GBA", state) == 0);
    cat_list_state_jump(&state->game_list, 1, state->game_count);

    state->action_scope = JW_ACTION_GAME;
    state->action_game = bravo;
    state->actions_open = true;
    jw__action_refresh_rows(state);
    assert(state->action_rows[state->action_row_count - 1] == JW_ACTION_ROW_HIDE);
    bool running = true;
    jw__hide_action_game("", state->db_path, state, &running);
    assert(running && !state->actions_open && state->games_open);
    assert(state->game_count == 1 && state->game_list.cursor == 0);
    assert(state->games[0].id == alpha.id);
    assert(state->favorites_count == 0 && state->recents_count == 0);
    assert(state->library_generation == 23);

    state->action_scope = JW_ACTION_GAME;
    state->action_game = alpha;
    jw__hide_action_game("", state->db_path, state, &running);
    assert(!state->games_open && state->system_count == 0);
    assert(state->list.cursor == 0 && state->library_generation == 23);
    jw_resume resume;
    assert(jw__load_resume(&resume) && !resume.games_open && resume.list_cursor == 0);

    /* Pick uses the filtered browser; locked Focus and Arrange retain IDs. */
    setenv("JAWAKA_FOCUS_MODE", "1", 1);
    setenv("JAWAKA_FOCUS_IDS", ids, 1);
    jw__focus_init(state);
    assert(state->focus_active && state->focus_count == 1);
    assert(state->focus_games[0].id == bravo.id);
    state->focus_active = false;
    jw__focus_setup_begin(state);
    assert(state->focus_pick_active && state->system_count == 0);
    assert(state->focus_setup_count == 1 && state->focus_setup_ids[0] == bravo.id);
    jw__pick_done(state);
    assert(state->focus_setup_step == JW_FSETUP_ARRANGE);
    jw_game_entry arranged;
    assert(jw_db_get_game_by_id(state->db_path, state->focus_setup_ids[0], &arranged) == 0);
    jw__focus_pick_reenter(state);
    jw__pick_clear(state);
    assert(state->focus_setup_count == 0);
    jw__pick_cancel(state);

    /* Settings stays reachable after hiding the entire library. */
    jw_settings_ui_open(&state->settings, JW_SETTINGS_GAMES);
    state->settings.games_list.cursor = JW_GAMES_HIDDEN_GAMES;
    bool changed = false;
    assert(jw__system_settings_input(state, &state->settings, CAT_BTN_A, &changed));
    assert(state->settings.screen == JW_SETTINGS_HIDDEN_GAMES);
    assert(state->settings.hidden_games_count == 2);
    state->menu_open = true;
    state->menu_tab = JW_SMTAB_SETTINGS;
    jw__render_launcher(state);
    save_screenshot("JW_VISIBILITY_SCREENSHOT");
    assert(jw__system_settings_input(state, &state->settings, CAT_BTN_A, &changed));
    assert(state->settings.hidden_games_count == 1 && !state->settings.visibility_changed);
    assert(state->system_count == 1 && state->systems[0].game_count == 1);
    assert(state->library_generation == 23);
    assert(jw__system_settings_input(state, &state->settings, CAT_BTN_B, &changed));
    assert(state->settings.screen == JW_SETTINGS_GAMES);

    /* Failed refreshes discard cached browse rows after a successful write. */
    assert(jw__open_system_games(state->db_path, "GBA", state) == 0);
    state->search_open = true;
    state->search_count = 1;
    assert(jw__refresh_after_visibility_write(root, state) != 0); /* directory, not a DB */
    assert(!state->games_open && !state->search_open && state->search_count == 0);
    assert(state->system_count == 0 && state->recents_count == 0 && state->favorites_count == 0);

    check_discs(state, root);

    jw_settings_ui_close(&state->settings);
    jw_ra_catalog_free(state->system_catalog);
    jw__close_game_browser(state);
    jw_cover_loader_shutdown(jw__covers());
    unlink(state->db_path);
    unlink(visibility_resume_path);
    free(state);
    cat_quit();
    rmdir(root);
    puts("visibility-ui-test: game/disc visibility, refresh, Focus and Settings restore passed");
    return 0;
}
