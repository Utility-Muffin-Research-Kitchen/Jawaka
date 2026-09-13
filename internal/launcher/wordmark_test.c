/* Exercise the actual launcher resolver and async decoder with SDL's dummy
   renderer. Keeping the test in the same TU avoids a second artwork loader. */
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>
#include <utime.h>

static void wm_dirs(const char *path) {
    char copy[PATH_MAX]; snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(copy, 0700); *p = '/'; }
    mkdir(copy, 0700);
}
static void wm_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); assert(f); fputs(text, f); fclose(f);
}
static void wm_png(const char *path, int width) {
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, 16, 32, SDL_PIXELFORMAT_RGBA32);
    assert(surface); SDL_FillRect(surface, NULL, 0xffffffff);
    assert(!IMG_SavePNG(surface, path)); SDL_FreeSurface(surface);
}
static void wm_expect(jw_launcher_state *state, int width) {
    int w = 0, h = 0; SDL_Texture *texture = NULL;
    for (int i = 0; i < 150; i++) {
        texture = jw__gg_wordmark(state, &w, &h);
        if (texture) break;
        SDL_Delay(10);
    }
    if ((width && (!texture || w != width)) || (!width && texture)) {
        fprintf(stderr, "wordmark: wanted width %d, got %d (texture %p)\n", width, w, (void *)texture);
        abort();
    }
}
int main(void) {
    char root[] = "/tmp/jw-wordmark-XXXXXX"; assert(mkdtemp(root));
    char path[PATH_MAX], font[PATH_MAX];
    snprintf(path, sizeof(path), "%s/res/themes/Test", root); wm_dirs(path);
    snprintf(path, sizeof(path), "%s/res/themes/Test/stylesheet.json", root); wm_file(path, "{}");
    snprintf(path, sizeof(path), "%s/res/themes", root); setenv("CAT_THEMES_DIR", path, 1);
    setenv("CAT_THEME_NAME", "Test", 1);
    snprintf(path, sizeof(path), "%s/state", root); wm_dirs(path); setenv("UMRK_INTERNAL_DATA_PATH", path, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = {.start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font};
    assert(cat_init(&config) == CAT_OK);
    jw_launcher_state *state = calloc(1, sizeof(*state)); assert(state);
    snprintf(state->sdcard_root, sizeof(state->sdcard_root), "%s", root);
    snprintf(state->game_system, sizeof(state->game_system), "TEST");
    state->settings.user_theme_index = 0;
    state->settings.user_themes.count = 2;
    snprintf(state->settings.user_theme_dir, sizeof(state->settings.user_theme_dir), "A");
    snprintf(state->settings.user_themes.root, PATH_MAX, "%s/Themes", root);
    snprintf(state->settings.user_themes.items[0].dir, 128, "A");
    snprintf(state->settings.user_themes.items[1].dir, 128, "B");
    const char *dirs[] = {"Roms/TEST", "Themes/A/grid/wordmarks", "Themes/B/grid/wordmarks", "Apps/mlp1/Test.pak/art", "res/grid_wordmarks"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]); wm_dirs(path);
    }
    char rom[PATH_MAX], theme[PATH_MAX], provider[PATH_MAX], bundled[PATH_MAX];
    snprintf(rom, sizeof(rom), "%s/Roms/TEST/wordmark.png", root);
    snprintf(theme, sizeof(theme), "%s/Themes/A/grid/wordmarks/TEST.png", root);
    snprintf(provider, sizeof(provider), "%s/Apps/mlp1/Test.pak/art/mark.png", root);
    snprintf(bundled, sizeof(bundled), "%s/res/grid_wordmarks/TEST.png", root);
    wm_png(rom, 32); wm_png(theme, 64); wm_png(provider, 128); wm_png(bundled, 256);
    snprintf(path, sizeof(path), "%s/Apps", root); setenv("APPS_PATH", path, 1);
    jw_ra_system system = {.id = "TEST", .name = "Test system", .wordmark = "art/mark.png", .wordmark_provider = "mlp1/Test.pak"};
    jw_ra_catalog catalog = {.sdcard_root = root, .systems = &system, .system_count = 1, .info_dir = "gen-one/info"};
    state->system_catalog = &catalog;
    /* A cold higher-priority candidate stays pending even with lower art ready. */
    int w, h; assert(!jw__gg_wordmark(state, &w, &h));
    wm_expect(state, 32);
    unlink(rom); state->wordmark_revision++; wm_expect(state, 64);
    /* Same index, different folder: theme identity must invalidate the memo. */
    snprintf(path, sizeof(path), "%s/Themes/B/grid/wordmarks/TEST.png", root); wm_png(path, 96);
    snprintf(state->settings.user_themes.items[0].dir, 128, "B");
    snprintf(state->settings.user_theme_dir, 128, "B"); wm_expect(state, 96);
    /* A malformed highest-priority PNG must advance after an async failure. */
    snprintf(state->settings.user_themes.items[0].dir, 128, "A");
    snprintf(state->settings.user_theme_dir, 128, "A");
    wm_file(theme, "bad PNG"); catalog.info_dir = "gen-two/info";
    state->wordmark_revision++; wm_expect(state, 128);
    assert(system.provider == NULL); /* artwork doesn't assign system ownership */
    /* Same-path, same-mtime provider replacement changes only catalog identity. */
    struct stat st; assert(!stat(provider, &st));
    wm_png(provider, 192); struct utimbuf times = {.actime = st.st_atime, .modtime = st.st_mtime};
    assert(!utime(provider, &times)); catalog.info_dir = "gen-three/info";
    state->wordmark_revision++; wm_expect(state, 192);
    /* Removed provider, then undecodable bundle: the text branch receives NULL. */
    system.wordmark_provider = NULL; state->wordmark_revision++; wm_expect(state, 256);
    wm_file(bundled, "bad PNG"); catalog.info_dir = "gen-four/info";
    state->wordmark_revision++; wm_expect(state, 0);
    /* Reinstall after failures and a 1024px mark: decode cap remains 512px. */
    system.wordmark_provider = "mlp1/Test.pak"; wm_png(provider, 1024);
    catalog.info_dir = "gen-five/info"; state->wordmark_revision++; wm_expect(state, 512);
    jw_cover_loader_shutdown(jw__covers());
    cat_cache_clear();
    /* A full/unwritable thumbnail directory still returns the decoded surface. */
    snprintf(path, sizeof(path), "%s/missing/thumb.png", root);
    SDL_Texture *texture = NULL; bool failed = false;
    for (int i = 0; i < 150 && !texture && !failed; i++) {
        texture = jw__load_page_image_status(provider, JW_WORDMARK_MAX, &w, &h, path, &failed);
        SDL_Delay(10);
    }
    assert(texture && !failed && w == 512);
    jw_cover_loader_shutdown(jw__covers());
    state->system_catalog = NULL; free(state); cat_quit();
    puts("PASS wordmark-test: precedence, pending/failure, theme switch, replacement, removal, decode cap");
    return 0;
}
