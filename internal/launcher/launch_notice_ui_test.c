/* Exercise the actual presentation wrapper and tab snapshots with SDL's dummy
   renderer, following wordmark_test.c's single-TU launcher test pattern. */
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>

static SDL_Surface *notice_pixels(SDL_Texture *target) {
    SDL_Renderer *renderer = cat_get_renderer();
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    assert(SDL_SetRenderTarget(renderer, target) == 0);
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0,
        cat_get_screen_width(), cat_get_screen_height(), 32, SDL_PIXELFORMAT_RGBA32);
    assert(surface);
    assert(SDL_RenderReadPixels(renderer, NULL, surface->format->format,
                               surface->pixels, surface->pitch) == 0);
    assert(SDL_SetRenderTarget(renderer, previous) == 0);
    return surface;
}

static bool notice_pixels_equal(const SDL_Surface *a, const SDL_Surface *b) {
    assert(a->w == b->w && a->h == b->h);
    for (int y = 0; y < a->h; ++y) {
        if (memcmp((const char *)a->pixels + y * a->pitch,
                   (const char *)b->pixels + y * b->pitch, a->w * 4) != 0)
            return false;
    }
    return true;
}

int main(void) {
    char root[] = "/tmp/jw-launch-notice-XXXXXX";
    assert(mkdtemp(root));
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
    state->current_tab = JW_TAB_APPS;
    state->visible_tab_count = JW_TAB_COUNT;
    for (int i = 0; i < JW_TAB_COUNT; ++i) state->visible_tabs[i] = (jw_tab)i;
    state->settings.show_hints = true;
    state->scan_ready = true;
    cat_list_state_init(&state->list, 10);
    g_present_state = state;

    SDL_Texture *baseline = jw__capture_view(state);
    assert(baseline);
    SDL_Surface *empty = notice_pixels(baseline);
    g_defer_present = true;
    jw__render_launcher(state);
    SDL_Surface *live_empty = notice_pixels(NULL);
    jw__launch_notice(state, "launch failed: standalone emulator missing");
    cat__g.next_redraw_ms = 0;
    SDL_Texture *snapshot = jw__capture_view(state);
    assert(snapshot);
    SDL_Surface *captured = notice_pixels(snapshot);
    assert(notice_pixels_equal(empty, captured)); /* no pill baked into the slide */
    assert(cat__g.next_redraw_ms == 0);           /* snapshots do not pace */

    /* The screenshot-flash path holds presentation on the live backbuffer:
       it must still include the pill and schedule its expiry. */
    jw__render_launcher(state);
    SDL_Surface *live = notice_pixels(NULL);
    assert(!notice_pixels_equal(live_empty, live));
    assert(cat__g.next_redraw_ms != 0);

    /* A hosted page never reaches main's tick. Once an earlier wake (e.g. the
       status poll) is consumed, its next presentation must re-arm expiry. */
    cat__g.next_redraw_ms = 0;
    jw__render_launcher(state);
    uint32_t deadline = state->launch_notice.started_ms + JW_SYSTEM_NOTICE_MS;
    assert(cat__g.next_redraw_ms >= deadline && cat__g.next_redraw_ms - deadline < 100);

    state->launch_notice.started_ms = SDL_GetTicks() - JW_SYSTEM_NOTICE_MS;
    cat__g.next_redraw_ms = 0;
    jw__render_launcher(state);
    SDL_Surface *expired = notice_pixels(NULL);
    assert(state->launch_notice.text[0] == '\0');
    assert(notice_pixels_equal(live_empty, expired));

    SDL_FreeSurface(empty);
    SDL_FreeSurface(live_empty);
    SDL_FreeSurface(captured);
    SDL_FreeSurface(live);
    SDL_FreeSurface(expired);
    SDL_DestroyTexture(baseline);
    SDL_DestroyTexture(snapshot);
    g_present_state = NULL;
    free(state);
    cat_quit();
    rmdir(root);
    puts("launch-notice-ui-test: snapshots, deferred live draw, expiry wake and removal passed");
    return 0;
}
