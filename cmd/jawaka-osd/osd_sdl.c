#include "cmd/jawaka-osd/osd_backend.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define JW_OSD_HIDE_AFTER_MS 1200u

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static int s_percent = 50;
static uint64_t s_hide_at;
static bool s_visible;

static void jw__draw_rect(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_Rect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(s_renderer, r, g, b, a);
    SDL_RenderFillRect(s_renderer, &rect);
}

static void jw__draw(void) {
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 0);
    SDL_RenderClear(s_renderer);

    jw__draw_rect(0, 0, 480, 96, 18, 20, 24, 230);
    jw__draw_rect(24, 43, 432, 10, 72, 76, 84, 255);

    int fill = (432 * s_percent) / 100;
    jw__draw_rect(24, 43, fill, 10, 250, 210, 92, 255);
    jw__draw_rect(20 + fill, 35, 18, 26, 255, 240, 150, 255);

    SDL_RenderPresent(s_renderer);
}

int jw_osd_backend_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        return -1;
    }

    Uint32 flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;
#ifdef SDL_WINDOW_ALWAYS_ON_TOP
    flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#endif
    s_window = SDL_CreateWindow("Jawaka OSD",
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                480, 96, flags);
    if (!s_window) {
        SDL_Quit();
        return -1;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
        SDL_Quit();
        return -1;
    }
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
    return 0;
}

void jw_osd_backend_show_brightness(int percent, uint64_t now_ms) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_percent = percent;
    s_hide_at = now_ms + JW_OSD_HIDE_AFTER_MS;
    s_visible = true;
    SDL_ShowWindow(s_window);
    SDL_RaiseWindow(s_window);
    jw__draw();
}

void jw_osd_backend_show_volume(int percent, uint64_t now_ms) {
    /* SDL backend reuses the same toast for now */
    jw_osd_backend_show_brightness(percent, now_ms);
}

void jw_osd_backend_tick(uint64_t now_ms) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { }

    if (s_visible && now_ms >= s_hide_at) {
        SDL_HideWindow(s_window);
        s_visible = false;
    }
}

void jw_osd_backend_shutdown(void) {
    if (s_renderer) {
        SDL_DestroyRenderer(s_renderer);
        s_renderer = NULL;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    SDL_Quit();
}
