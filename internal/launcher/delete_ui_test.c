/* Real launcher handlers and rendering; the daemon boundary is a disposable fake. */
static char delete_resume_path[4096];
#define JW_RESUME_PATH delete_resume_path
#define jw_ipc_delete_begin test_delete_begin
#define jw_ipc_delete_disc_begin test_delete_disc_begin
#define jw_ipc_delete_poll test_delete_poll
#define jw_ipc_delete_commit test_delete_commit
#define jw_ipc_delete_cancel test_delete_cancel
#define jw_ipc_delete_close test_delete_close
#define jw_ipc_get_storage_status test_storage_status
#define jw_storage_ui_show_warning test_storage_warning
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>

static int begins, commits, cancels, closes, warnings;
static jw_ipc_delete_phase begin_phase = JW_IPC_DELETE_READY;
static jw_ipc_delete_phase commit_phase = JW_IPC_DELETE_COMMITTING;
static int commit_rc;
static char readonly_source[32];
static char warning_source[32];
static bool disc_request, final_disc, shared_disc;
static int disc_begins;

static void fixture_status(jw_ipc_delete_status *status, jw_ipc_delete_phase phase) {
    jw_ipc_delete_status_free(status);
    status->phase = phase;
    snprintf(status->token, sizeof(status->token), "reviewed-once");
    snprintf(status->name, sizeof(status->name), "Synthetic Adventure");
    snprintf(status->source_id, sizeof(status->source_id), "primary");
    snprintf(status->readonly_source, sizeof(status->readonly_source), "%s", readonly_source);
    if (phase == JW_IPC_DELETE_ERROR) snprintf(status->error, sizeof(status->error), "A fresh preview is required.");
    status->disc_count = 2;
    status->file_count = 2;
    status->bytes = 8192;
    status->shared_count = status->keep_count = 1;
    if (disc_request) {
        status->disc_count = 1;
        status->playlist_edit = !final_disc;
        status->final_disc = final_disc;
        status->playlist_replaced = !final_disc && phase == JW_IPC_DELETE_DONE;
        snprintf(status->disc_name, sizeof(status->disc_name), "%s", final_disc ? "Hidden Disc" : "Opening");
        status->remaining_discs = final_disc ? 0 : 1;
    }
    status->files_count = 3;
    status->files = calloc(status->files_count, sizeof(*status->files));
    assert(status->files);
    for (size_t i = 0; i < status->files_count; ++i) {
        snprintf(status->files[i].source_id, sizeof(status->files[i].source_id), "primary");
        snprintf(status->files[i].rom_relpath, sizeof(status->files[i].rom_relpath),
                 "PS/Adventure/Disc %zu.chd", i + 1);
        status->files[i].keep = i == 2;
        status->files[i].size = 4096;
    }
    if (disc_request && !final_disc) {
        status->files[2].keep = 2;
        snprintf(status->files[2].rom_relpath, sizeof(status->files[2].rom_relpath), "PS/Adventure.m3u");
        status->shared_count = 0;
        if (shared_disc) {
            status->files[0].keep = status->files[1].keep = 1;
            status->bytes = status->file_count = 0;
            status->shared_count = 2;
            status->keep_count = 3;
        }
    }
}

int test_delete_begin(const char *socket, const char *source, const char *relative,
                      jw_ipc_delete_session **session, jw_ipc_delete_status *status) {
    (void)socket;
    assert(!strcmp(source, "primary") && !strcmp(relative, "PS/Adventure.m3u"));
    ++begins;
    disc_request = false;
    *session = (jw_ipc_delete_session *)(uintptr_t)1;
    fixture_status(status, begin_phase);
    return 0;
}

int test_delete_disc_begin(const char *socket, const char *source, const char *relative,
                            const char *disc_source, const char *disc_path, const char *member,
                            jw_ipc_delete_session **session, jw_ipc_delete_status *status) {
    (void)socket;
    assert(!strcmp(source, "primary") && !strcmp(relative, "PS/Adventure.m3u"));
    assert(!strcmp(disc_source, "primary") && !strcmp(disc_path, final_disc ? "PS/disc2.chd" : "PS/disc1.chd"));
    assert(!member[0]);
    ++disc_begins;
    disc_request = true;
    *session = (jw_ipc_delete_session *)(uintptr_t)1;
    fixture_status(status, begin_phase);
    return 0;
}

int test_delete_poll(jw_ipc_delete_session *session, jw_ipc_delete_status *status) {
    assert(session);
    fixture_status(status, JW_IPC_DELETE_DONE);
    status->removed_count = 2;
    status->files[0].removed = status->files[1].removed = true;
    return 0;
}

int test_delete_commit(jw_ipc_delete_session *session, const char *token,
                        jw_ipc_delete_status *status) {
    assert(session && !strcmp(token, "reviewed-once"));
    ++commits;
    fixture_status(status, commit_phase);
    return commit_rc;
}

void test_delete_cancel(jw_ipc_delete_session *session) { assert(session); ++cancels; }
void test_delete_close(jw_ipc_delete_session *session) { assert(session); ++closes; }

int test_storage_status(const char *socket, const char *source,
                          jw_ipc_storage_status_info *card, char *message, int size) {
    (void)socket; (void)message; (void)size;
    memset(card, 0, sizeof(*card));
    snprintf(card->source, sizeof(card->source), "%s", source);
    snprintf(card->access, sizeof(card->access), "read-only");
    return 0;
}

bool test_storage_warning(const char *socket, const jw_ipc_storage_status_info *card) {
    (void)socket;
    ++warnings;
    snprintf(warning_source, sizeof(warning_source), "%s", card->source);
    return false;
}

static void screenshot(const char *variable) {
    const char *path = getenv(variable);
    if (!path || !path[0]) return;
    SDL_Surface *pixels = SDL_CreateRGBSurfaceWithFormat(0, cat_get_screen_width(),
        cat_get_screen_height(), 32, SDL_PIXELFORMAT_RGBA32);
    assert(pixels && SDL_RenderReadPixels(cat_get_renderer(), NULL, pixels->format->format,
                                         pixels->pixels, pixels->pitch) == 0);
    assert(IMG_SavePNG(pixels, path) == 0);
    SDL_FreeSurface(pixels);
}

static void write_fixture(const char *root, const char *name, const char *text) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/Roms/PS/%s", root, name);
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void attach_catalog(jw_launcher_state *state) {
    jw_ra_catalog_free(state->system_catalog);
    state->system_catalog = calloc(1, sizeof(*state->system_catalog));
    assert(state->system_catalog);
    state->system_catalog->systems = calloc(1, sizeof(*state->system_catalog->systems));
    assert(state->system_catalog->systems);
    state->system_catalog->system_count = 1;
    state->system_catalog->systems[0].id = strdup("PS");
    state->system_catalog->systems[0].name = strdup("PlayStation");
}

static void check_disc_deletion(jw_launcher_state *state, const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/Roms", root);
    assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/Roms/PS", root);
    assert(mkdir(path, 0755) == 0);
    write_fixture(root, "Adventure.m3u", "disc1.chd|Opening\r\ndisc2.chd|Hidden Disc\r\n#SAVEDISK:Save Disk\r\n");
    write_fixture(root, "disc1.chd", "synthetic first disc");
    write_fixture(root, "disc2.chd", "synthetic second disc");
    snprintf(path, sizeof(path), "%s/Roms/PS/Adventure.m3u", root);
    sqlite3 *db = NULL;
    assert(jw_db_open(state->db_path, &db) == 0 && jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game_stable(db, "PS", "Synthetic Adventure", "primary", "PS/Adventure.m3u",
                                   path, "", "", "") == 0);
    jw_db_close(db);
    jw_game_entry parent;
    assert(jw_db_get_game_by_source_relpath(state->db_path, "primary", "PS/Adventure.m3u", &parent) == 0);
    assert(jw_db_set_rom_hidden(state->db_path, "primary", "PS/disc2.chd", "", 1) == 0);
    assert(jw_db_set_favorite(state->db_path, "game", parent.id, 1) == 0);
    attach_catalog(state);
    jw__open_game_actions(state->db_path, state, &parent);
    state->discs_open = true;
    assert(state->action_content.disc_count == 2 && state->disc_visible_count == 1);
    assert(jw__can_delete_disc(state));
    state->system_catalog->systems[0].provider = strdup("mlp1/Custom.pak");
    assert(!jw__can_delete_disc(state));
    bool running = true;
    jw__handle_discs_input("", state->db_path, state, CAT_BTN_X, &running);
    assert(!state->delete_open);
    free(state->system_catalog->systems[0].provider);
    state->system_catalog->systems[0].provider = NULL;
    begin_phase = JW_IPC_DELETE_READY;
    commit_phase = JW_IPC_DELETE_COMMITTING;
    final_disc = false;
    shared_disc = true;
    jw__render_discs(state);
    screenshot("JW_DELETE_DISCS_SCREENSHOT");
    jw__handle_discs_input("", state->db_path, state, CAT_BTN_X, &running);
    assert(disc_begins == 1 && !state->delete_confirm && state->delete_status.playlist_edit);
    assert(!strcmp(jw__delete_title(state), "Delete Disc") && !state->delete_status.bytes);
    assert(state->delete_status.remaining_discs == 1 && !state->delete_status.final_disc);
    jw__render_launcher(state);
    screenshot("JW_DELETE_DISC_SCREENSHOT");
    int before = commits;
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == before && state->discs_open && state->action_content.disc_count == 2);
    shared_disc = false;
    /* The daemon may still be committing after its response is lost. */
    commit_phase = JW_IPC_DELETE_ERROR;
    commit_rc = -1;
    jw__handle_discs_input("", state->db_path, state, CAT_BTN_X, &running);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == before + 1 && !state->delete_result_known);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(state->delete_reconciled && !state->delete_open);
    assert(!state->actions_open && !state->discs_open && !state->action_content.disc_count);
    commit_rc = 0;
    commit_phase = JW_IPC_DELETE_COMMITTING;
    attach_catalog(state);
    jw__open_game_actions(state->db_path, state, &parent);
    state->discs_open = true;
    before = commits;
    jw__handle_discs_input("", state->db_path, state, CAT_BTN_X, &running);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == before + 1);
    /* The fake daemon now publishes a completed edit to the disposable playlist. */
    write_fixture(root, "Adventure.m3u", "disc2.chd|Hidden Disc\r\n#SAVEDISK:Save Disk\r\n");
    snprintf(path, sizeof(path), "%s/Roms/PS/disc1.chd", root);
    assert(unlink(path) == 0);
    state->delete_next_poll = 0;
    jw__delete_tick(state, &running);
    assert(state->delete_status.playlist_replaced && state->delete_reconciled);
    assert(state->actions_open && state->discs_open && state->action_content.disc_count == 1);
    assert(state->disc_visible_count == 0); /* the surviving hidden disc still belongs to the game */
    jw_game_entry current;
    assert(jw_db_get_game_by_id(state->db_path, parent.id, &current) == 0 && current.favorite);
    jw__handle_delete_input(state, CAT_BTN_B);
    attach_catalog(state);
    jw__handle_discs_input("", state->db_path, state, CAT_BTN_B, &running);
    jw__action_refresh_rows(state);
    assert(state->action_rows[state->action_row_count - 2] == JW_ACTION_ROW_DISCS);
    cat_list_state_jump(&state->action_list, state->action_row_count - 2, state->action_row_count);
    jw__handle_actions_input("", state->db_path, state, CAT_BTN_A, &running);
    assert(state->discs_open && state->action_content.disc_count == 1);
    jw__handle_discs_input("", state->db_path, state, CAT_BTN_Y, &running);
    assert(state->disc_visible_count == 1 && state->disc_hidden[0]);
    final_disc = true;
    jw__handle_discs_input("", state->db_path, state, CAT_BTN_X, &running);
    assert(state->delete_status.final_disc && !state->delete_status.playlist_edit && !state->delete_confirm);
    assert(!strcmp(jw__delete_title(state), "Delete Game"));
    jw__render_launcher(state);
    screenshot("JW_DELETE_FINAL_DISC_SCREENSHOT");
    assert(access(path, F_OK) == -1); /* only the earlier first-disc edit has happened */
    snprintf(path, sizeof(path), "%s/Roms/PS/Adventure.m3u", root);
    assert(access(path, F_OK) == 0);
    before = commits;
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == before + 1); /* one explicit confirmation covers whole-game scope */
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/Roms/PS/disc2.chd", root);
    assert(unlink(path) == 0);
    assert(jw_db_open(state->db_path, &db) == 0);
    jw_db_rom_key removed[] = {{"primary", "PS/Adventure.m3u"}, {"primary", "PS/disc2.chd"}};
    assert(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    assert(jw_db_reconcile_removed_roms(db, removed, 2) == 0);
    assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    jw_db_close(db);
    assert(test_delete_poll(state->delete_session, &state->delete_status) == 0);
    assert(state->delete_status.phase == JW_IPC_DELETE_DONE);
    /* Back can arrive before the regular frame tick after a quick result. */
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(!state->actions_open && !state->discs_open && !state->delete_open);
    jw_ra_catalog_free(state->system_catalog);
    state->system_catalog = NULL;
    snprintf(path, sizeof(path), "%s/Roms/PS", root);
    assert(rmdir(path) == 0);
    snprintf(path, sizeof(path), "%s/Roms", root);
    assert(rmdir(path) == 0);
}

int main(void) {
    char root[] = "/tmp/jw-delete-ui-XXXXXX";
    assert(mkdtemp(root));
    snprintf(delete_resume_path, sizeof(delete_resume_path), "%s/resume", root);
    setenv("SDCARD_PATH", root, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("CAT_THEMES_DIR", "res/themes", 1);
    setenv("CAT_THEME_NAME", "Jawaka-Tabs", 1);
    char font[PATH_MAX];
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = {.start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font};
    assert(cat_init(&config) == CAT_OK);
    jw_launcher_state *state = calloc(1, sizeof(*state));
    assert(state);
    g_present_state = state;
    state->settings.show_hints = true;
    state->haptics_muted = true;
    state->scan_ready = true;
    snprintf(state->db_path, sizeof(state->db_path), "%s/library.db", root);
    snprintf(state->settings.db_path, sizeof(state->settings.db_path), "%s", state->db_path);
    snprintf(state->sdcard_root, sizeof(state->sdcard_root), "%s", root);
    sqlite3 *db = NULL;
    assert(jw_db_open(state->db_path, &db) == 0 && jw_db_apply_schema(db) == 0);
    jw_db_close(db);
    jw_ra_system system = {.id = "PS"};
    jw_ra_catalog catalog = {.systems = &system, .system_count = 1};
    state->system_catalog = &catalog;
    jw_game_entry game = {.id = 42};
    snprintf(game.name, sizeof(game.name), "Synthetic Adventure");
    snprintf(game.source_id, sizeof(game.source_id), "primary");
    snprintf(game.rom_relpath, sizeof(game.rom_relpath), "PS/Adventure.m3u");
    state->action_scope = JW_ACTION_GAME;
    state->action_game = game;
    const struct { const char *id; bool eligible; } cases[] = {
        {"PS", true}, {"SEGACD", true}, {"GBA", true}, {"PORTS", false},
        {"DOS", false}, {"AMIGA", false}, {"ARCADE", false}, {"DC", false},
        {"SCUMMVM", false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        system.id = (char *)cases[i].id;
        snprintf(state->action_game.system, sizeof(state->action_game.system), "%s", system.id);
        jw__action_refresh_rows(state);
        assert(state->action_rows[state->action_row_count - 1] ==
            (cases[i].eligible ? JW_ACTION_ROW_DELETE : JW_ACTION_ROW_HIDE));
    }
    system.id = "PS";
    system.provider = "mlp1/Custom.pak";
    snprintf(state->action_game.system, sizeof(state->action_game.system), "PS");
    jw__action_refresh_rows(state);
    assert(state->action_rows[state->action_row_count - 1] == JW_ACTION_ROW_HIDE);
    system.provider = NULL;
    snprintf(game.system, sizeof(game.system), "PS");

    state->action_is_playlist = true;
    state->action_content.disc_count = 1;
    state->action_core_count = 2;
    state->action_bios_supported = true;
    jw__action_refresh_rows(state);
    assert(state->action_row_count == 9);
    assert(state->action_rows[6] == JW_ACTION_ROW_HIDE &&
           state->action_rows[7] == JW_ACTION_ROW_DISCS &&
           state->action_rows[8] == JW_ACTION_ROW_DELETE);
    cat_list_state_jump(&state->action_list, 8, 9);
    jw__render_actions(state);
    assert(state->action_list.scroll_offset > 0 &&
           state->action_list.cursor < state->action_list.scroll_offset + state->action_list.visible_rows);
    state->action_content.disc_count = 0;
    state->action_is_playlist = false;

    jw__delete_begin(state, &game);
    assert(state->delete_open && !state->delete_confirm && begins == 1);
    jw__render_launcher(state);
    screenshot("JW_DELETE_SCREENSHOT");
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(!state->delete_open && !commits && cancels == 1);

    jw__delete_begin(state, &game);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_X);
    assert(state->delete_files_open && !state->delete_confirm);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(!commits);
    assert(cat_set_font_bump(5) == CAT_OK);
    jw__render_launcher(state);
    assert(!SDL_RenderIsClipEnabled(cat_get_renderer()));
    assert(cat_set_font_bump(0) == CAT_OK);
    state->delete_status.missing_descriptors = true;
    state->delete_status.writable_progress = true;
    snprintf(state->delete_status.missing_sources, sizeof(state->delete_status.missing_sources), "secondary_sd");
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(state->delete_open && !state->delete_files_open && !state->delete_confirm);
    jw__render_launcher(state);
    int height = jw__delete_body(state, 0, 0, cat_get_screen_width() - CAT_S(36), false);
    assert(height > cat_get_screen_height() / 2); /* warnings remain inspectable by scrolling */
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 1 && state->delete_status.phase == JW_IPC_DELETE_COMMITTING);
    jw__handle_delete_input(state, CAT_BTN_A);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(commits == 1 && state->delete_open);
    state->delete_reconciled = true; /* cache reconciliation has its own disposable DB test */
    state->delete_next_poll = 0;
    bool running = true;
    jw__delete_tick(state, &running);
    assert(state->delete_status.phase == JW_IPC_DELETE_DONE);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 1);
    jw__handle_delete_input(state, CAT_BTN_B);

    commit_phase = JW_IPC_DELETE_ERROR;
    jw__delete_begin(state, &game);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 2 && state->delete_status.phase == JW_IPC_DELETE_ERROR);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 2); /* a failed or lost commit never replays */
    state->delete_reconciled = true;
    jw__handle_delete_input(state, CAT_BTN_B);

    commit_rc = -1;
    jw__delete_begin(state, &game);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 3 && !state->delete_result_known);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 3);
    state->delete_reconciled = true;
    jw__handle_delete_input(state, CAT_BTN_B);
    commit_rc = 0;

    begin_phase = JW_IPC_DELETE_PREPARING;
    jw__delete_begin(state, &game);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(!state->delete_open && cancels == 2);
    begin_phase = JW_IPC_DELETE_ERROR;
    snprintf(readonly_source, sizeof(readonly_source), "secondary_sd");
    jw__delete_begin(state, &game);
    jw__delete_tick(state, &running);
    assert(!state->delete_open && warnings == 1 && !strcmp(warning_source, "secondary_sd"));
    readonly_source[0] = '\0';
    begin_phase = JW_IPC_DELETE_READY;

    state->settings.open = true;
    state->settings.screen = JW_SETTINGS_HIDDEN_GAMES;
    state->settings.hidden_games_count = 1;
    state->settings.hidden_games = calloc(1, sizeof(*state->settings.hidden_games));
    assert(state->settings.hidden_games);
    state->settings.hidden_games[0].rom.game = game;
    state->settings.hidden_games[0].delete_eligible = true;
    bool theme_changed = false;
    int before = begins;
    assert(jw__system_settings_input(state, &state->settings, CAT_BTN_X, &theme_changed));
    assert(begins == before); /* the in-game Settings host cannot open Delete */
    state->settings.allow_delete_game = true;
    assert(jw__system_settings_input(state, &state->settings, CAT_BTN_X, &theme_changed));
    assert(begins == before + 1 && state->delete_open && !state->settings.delete_game_requested);
    assert(state->settings.hidden_games_count == 1);
    jw__delete_close(state);
    jw_settings_ui_close(&state->settings);
    state->system_catalog = NULL;
    check_disc_deletion(state, root);
    /* A lone CUE or cartridge has no playlist discs; never show "Discs: 0". */
    jw_ipc_delete_status summary = {.file_count = 1, .bytes = 4096};
    char text[256];
    jw__delete_summary(&summary, text, sizeof(text));
    assert(!strstr(text, "Discs") && !strncmp(text, "Files: 1. ", 10));
    summary.disc_count = 2;
    jw__delete_summary(&summary, text, sizeof(text));
    assert(!strncmp(text, "Discs: 2. Files: 1. ", 20));
    g_present_state = NULL;
    unlink(state->db_path);
    free(state);
    cat_quit();
    unlink(delete_resume_path); /* the refresh path saves a resume breadcrumb */
    rmdir(root);
    puts("delete-ui-test: game/disc scope, hidden final disc, refreshed playlist, no replay and Cancel default passed");
    return 0;
}
