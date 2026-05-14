#define CAT_IMPLEMENTATION
#include "catastrophe.h"

#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/ipc/ipc.h"
#include "internal/platform/paths.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char status[256];
} jw_menu_state;

static int jw__env_flag(const char *name) {
    const char *value = getenv(name);
    return value && strcmp(value, "0") != 0;
}

static uint32_t jw__env_u32(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    long parsed = strtol(value, NULL, 10);
    if (parsed <= 0) {
        return fallback;
    }
    return (uint32_t)parsed;
}

static int jw__request_json(const char *socket_path, cJSON *request, cJSON **out_response) {
    char *request_json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_json) {
        return -1;
    }

    char *response_json = NULL;
    size_t response_len = 0;
    int rc = jw_ipc_request(socket_path, request_json, strlen(request_json), &response_json, &response_len);
    cJSON_free(request_json);
    if (rc != 0) {
        return -1;
    }

    cJSON *response = cJSON_Parse(response_json);
    free(response_json);
    if (!response) {
        return -1;
    }

    *out_response = response;
    return 0;
}

static int jw__send_hello(const char *socket_path) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "hello");
    cJSON_AddStringToObject(request, "role", "menu");

    cJSON *response = NULL;
    if (jw__request_json(socket_path, request, &response) != 0) {
        return -1;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    int ok = cJSON_IsString(type) && type->valuestring && strcmp(type->valuestring, "hello-ok") == 0;
    cJSON_Delete(response);
    return ok ? 0 : -1;
}

static int jw__send_scan(const char *socket_path, jw_menu_state *state) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "scan-library");

    cJSON *response = NULL;
    if (jw__request_json(socket_path, request, &response) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "scan failed");
        return -1;
    }

    cJSON *game_count = cJSON_GetObjectItemCaseSensitive(response, "game_count");
    cJSON *app_count = cJSON_GetObjectItemCaseSensitive(response, "app_count");
    if (cJSON_IsNumber(game_count) && cJSON_IsNumber(app_count)) {
        snprintf(state->status, sizeof(state->status), "scan complete: %d games, %d apps",
            game_count->valueint, app_count->valueint);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", "scan complete");
    }

    cJSON_Delete(response);
    return 0;
}

static int jw__send_shutdown(const char *socket_path) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "shutdown");

    cJSON *response = NULL;
    int rc = jw__request_json(socket_path, request, &response);
    if (response) cJSON_Delete(response);
    return rc;
}

static void jw__render_menu(const jw_menu_state *state) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_LARGE);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    cat_clear_screen();
    cat_draw_screen_title("Jawaka Menu", NULL);

    SDL_Rect content = cat_get_content_rect(true, false, false);
    int x = CAT_S(24);
    int y = content.y + CAT_S(16);
    int max_w = content.w - CAT_S(48);

    cat_draw_text_wrapped(body_font, "Daemon-owned contextual shell", x, y, max_w, theme->accent, CAT_ALIGN_LEFT);
    y += CAT_S(34);

    cat_draw_text_wrapped(small_font, "R rescan library", x, y, max_w, theme->text, CAT_ALIGN_LEFT);
    y += CAT_S(20);
    cat_draw_text_wrapped(small_font, "Esc return to launcher", x, y, max_w, theme->text, CAT_ALIGN_LEFT);
    y += CAT_S(20);
    cat_draw_text_wrapped(small_font, "Q shutdown", x, y, max_w, theme->text, CAT_ALIGN_LEFT);
    y += CAT_S(34);

    cat_draw_text_wrapped(small_font, state->status, x, y, max_w, theme->hint, CAT_ALIGN_LEFT);
    cat_present();
}

int main(void) {
    char *socket_path = jw_socket_path();
    if (!socket_path) {
        jw_log_error("could not resolve socket path");
        return 1;
    }

    if (jw__send_hello(socket_path) != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?", socket_path);
        free(socket_path);
        return 1;
    }

    cat_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.window_title = "Jawaka Menu";
    cfg.disable_background = true;

    if (cat_init(&cfg) != CAT_OK) {
        jw_log_error("catastrophe init failed: %s", cat_get_error());
        free(socket_path);
        return 1;
    }

    jw_menu_state state;
    memset(&state, 0, sizeof(state));
    snprintf(state.status, sizeof(state.status), "%s", "menu ready");

    bool running = true;
    bool auto_demo = jw__env_flag("JAWAKA_AUTODEMO");
    bool auto_shutdown_sent = false;
    uint32_t auto_delay_ms = jw__env_u32("JAWAKA_AUTODEMO_DELAY_MS", 1200);
    uint32_t started_ms = SDL_GetTicks();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                switch (event.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        running = false;
                        break;
                    case SDLK_r:
                        jw__send_scan(socket_path, &state);
                        break;
                    case SDLK_q:
                        jw__send_shutdown(socket_path);
                        running = false;
                        break;
                    default:
                        break;
                }
            }
        }

        if (auto_demo && !auto_shutdown_sent && SDL_GetTicks() - started_ms >= auto_delay_ms) {
            jw__send_shutdown(socket_path);
            auto_shutdown_sent = true;
            running = false;
        }

        jw__render_menu(&state);
        SDL_Delay(16);
    }

    cat_quit();
    free(socket_path);
    return 0;
}
