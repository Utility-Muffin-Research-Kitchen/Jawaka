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

/* The longest Options menu scrolls at a larger accessibility font size. */
static void check_long_actions_menu(jw_launcher_state *state, const jw_game_entry *game) {
    state->menu_open = false;
    jw_settings_ui_close(&state->settings);
    jw_ra_catalog_free(state->system_catalog);
    state->system_catalog = calloc(1, sizeof(*state->system_catalog));
    assert(state->system_catalog);
    state->system_catalog->systems = calloc(1, sizeof(*state->system_catalog->systems));
    assert(state->system_catalog->systems);
    state->system_catalog->system_count = 1;
    state->system_catalog->systems[0].id = strdup("GBA");
    state->actions_open = true;
    state->action_scope = JW_ACTION_GAME;
    state->action_game = *game;
    state->action_core_count = 2;
    state->action_bios_supported = true;
    jw__action_refresh_rows(state);
    assert(state->action_row_count == 8);
    assert(state->action_rows[6] == JW_ACTION_ROW_HIDE && state->action_rows[7] == JW_ACTION_ROW_DELETE);
    cat_list_state_jump(&state->action_list, state->action_row_count - 1, state->action_row_count);
    assert(cat_set_font_bump(5) == CAT_OK);
    jw__render_actions(state);
    assert(state->action_list.visible_rows < state->action_row_count);
    assert(state->action_list.cursor >= state->action_list.scroll_offset);
    assert(state->action_list.cursor < state->action_list.scroll_offset + state->action_list.visible_rows);
    assert(!SDL_RenderIsClipEnabled(cat_get_renderer()));
    save_screenshot("JW_ACTIONS_SCREENSHOT");
    assert(cat_set_font_bump(0) == CAT_OK);
    state->actions_open = false;
    state->action_scope = JW_ACTION_NONE;
}

/* A card swap takes a pick off the locked screen and brings it back, with no launcher
   restart. The stored picks, the mode and the lock don't change. */
static void check_focus_follows_the_card(jw_launcher_state *state, const char *root, int primary_id) {
    char slot[PATH_MAX], roms[PATH_MAX], sources[PATH_MAX * 2], ids[64];
    const char *key = "JAWAKA_TEST_CARD_KEY_secondary_sd";
    snprintf(slot, sizeof(slot), "%.4000s/slot", root);
    snprintf(roms, sizeof(roms), "%.4000s/Roms", slot);
    snprintf(sources, sizeof(sources), "%.4000s:%.4000s", root, slot);
    assert(mkdir(slot, 0755) == 0 && mkdir(roms, 0755) == 0);
    setenv("SDCARD_PATHS", sources, 1);
    setenv(key, "fat:aaaa0001", 1);

    sqlite3 *db = NULL;
    assert(jw_db_open(state->db_path, &db) == 0);
    assert(sqlite3_exec(db,
        "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path) VALUES"
        "(900,'GBA','Card pick','secondary_sd','GBA/Pick.gba','/slot/Roms/GBA/Pick.gba');"
        "INSERT INTO library_cards(card_key,is_live,last_seen) VALUES('fat:aaaa0001',1,1);",
        NULL, NULL, NULL) == SQLITE_OK);
    snprintf(ids, sizeof(ids), "%d,900", primary_id);
    assert(jw_db_set_setting(state->db_path, JW_FOCUS_KEY_IDS, ids) == 0);
    assert(jw_db_set_setting(state->db_path, JW_FOCUS_KEY_ACTIVE, "1") == 0);

    state->focus_active = true;
    state->focus_lock = JW_FOCUS_LOCK_PIN;
    state->focus_count = 0;
    jw__focus_refresh(state);
    assert(state->focus_count == 2 && state->focus_games[1].id == 900);
    state->focus_cursor = 1;

    /* A different card: the pick leaves on the next generation change, and the cursor
       stays in range. */
    setenv(key, "fat:bbbb0002", 1);
    int generation = state->library_generation;
    atomic_store(&jw__status_poller.generation, generation + 1);
    jw__poll_library_generation("", state->db_path, state);
    assert(state->focus_count == 1 && state->focus_games[0].id == primary_id);
    atomic_store(&jw__status_poller.generation, generation);
    state->library_generation = generation;
    assert(state->focus_cursor == 0 && state->focus_active && state->focus_lock == JW_FOCUS_LOCK_PIN);
    char stored[64] = "";
    assert(jw_db_get_setting(state->db_path, JW_FOCUS_KEY_IDS, stored, sizeof(stored)) == 0);
    assert(strcmp(stored, ids) == 0);

    /* Setup only offers picks it could also let you remove. */
    state->focus_active = false;
    jw__focus_setup_begin(state);
    assert(state->focus_setup_count == 1 && state->focus_setup_ids[0] == primary_id);
    jw__pick_cancel(state);
    state->focus_active = true;
    /* Saving setup can't drop what it couldn't show: the pick returns with its card. */
    int saved[JW_FOCUS_MAX_GAMES] = { primary_id };
    assert(jw__focus_keep_absent_picks(state->db_path, saved, 1) == 2 && saved[1] == 900);
    /* A pick you can see and removed stays removed. */
    int none[JW_FOCUS_MAX_GAMES] = { 0 };
    assert(jw__focus_keep_absent_picks(state->db_path, none, 0) == 1 && none[0] == 900);
    /* With every place taken, your new picks win. */
    int full[JW_FOCUS_MAX_GAMES] = { 1, 2, 3, 4, 5 };
    assert(jw__focus_keep_absent_picks(state->db_path, full, JW_FOCUS_MAX_GAMES) == JW_FOCUS_MAX_GAMES);
    assert(full[4] == 5);

    /* Its rows parked, as once the scan has swapped the cards. */
    assert(sqlite3_exec(db, "UPDATE games SET source_id='card-1' WHERE id=900;",
                        NULL, NULL, NULL) == SQLITE_OK);
    jw__focus_refresh(state);
    assert(state->focus_count == 1);

    /* Every pick gone keeps the locked screen. */
    snprintf(stored, sizeof(stored), "900");
    assert(jw_db_set_setting(state->db_path, JW_FOCUS_KEY_IDS, stored) == 0);
    jw__focus_refresh(state);
    assert(state->focus_count == 0 && state->focus_active && state->focus_cursor == 0);

    /* Back with its card. */
    assert(sqlite3_exec(db, "UPDATE games SET source_id='secondary_sd' WHERE id=900;",
                        NULL, NULL, NULL) == SQLITE_OK);
    setenv(key, "fat:aaaa0001", 1);
    assert(jw_db_set_setting(state->db_path, JW_FOCUS_KEY_IDS, ids) == 0);
    jw__focus_refresh(state);
    assert(state->focus_count == 2);

    /* A config that can't be read keeps the current tiles. */
    assert(jw_db_set_setting(state->db_path, JW_FOCUS_KEY_ACTIVE, "0") == 0);
    setenv(key, "fat:bbbb0002", 1);
    jw__focus_refresh(state);
    assert(state->focus_count == 2);

    assert(sqlite3_exec(db, "DELETE FROM library_cards; DELETE FROM games WHERE id=900;",
                        NULL, NULL, NULL) == SQLITE_OK);
    jw_db_close(db);
    unsetenv(key);
    unsetenv("SDCARD_PATHS");
    rmdir(roms);
    rmdir(slot);
    state->focus_active = false;
    state->focus_lock = JW_FOCUS_LOCK_NONE;
    state->focus_count = 0;
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
    check_focus_follows_the_card(state, root, bravo.id);
    assert(jw_db_set_setting(state->db_path, JW_FOCUS_KEY_IDS, ids) == 0);

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

    check_long_actions_menu(state, &alpha);

    jw_settings_ui_close(&state->settings);
    jw_ra_catalog_free(state->system_catalog);
    jw__close_game_browser(state);
    jw_cover_loader_shutdown(jw__covers());
    unlink(state->db_path);
    unlink(visibility_resume_path);
    free(state);
    cat_quit();
    rmdir(root);
    puts("visibility-ui-test: hide, last-game refresh, Focus, Settings restore and long Options menu passed");
    return 0;
}
