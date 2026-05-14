#define CAT_IMPLEMENTATION
#include "catastrophe.h"

#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/ipc/ipc.h"
#include "internal/platform/paths.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    jw_library_summary summary;
    char status[256];
    bool scan_ready;
} jw_launcher_state;

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
    cJSON_AddStringToObject(request, "role", "launcher");

    cJSON *response = NULL;
    if (jw__request_json(socket_path, request, &response) != 0) {
        return -1;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    int ok = cJSON_IsString(type) && type->valuestring && strcmp(type->valuestring, "hello-ok") == 0;
    cJSON_Delete(response);
    return ok ? 0 : -1;
}

static int jw__scan_library(const char *socket_path, const char *db_path, jw_launcher_state *state) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "scan-library");

    cJSON *response = NULL;
    if (jw__request_json(socket_path, request, &response) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "scan failed: daemon unavailable");
        return -1;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "ok") != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "scan failed: daemon returned error");
        cJSON_Delete(response);
        return -1;
    }
    cJSON_Delete(response);

    if (jw_db_read_summary(db_path, &state->summary) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "scan complete but summary load failed");
        return -1;
    }

    state->scan_ready = true;
    snprintf(state->status, sizeof(state->status), "scan complete: %d games, %d apps",
        state->summary.game_count, state->summary.app_count);
    return 0;
}

static int jw__send_open_menu(const char *socket_path) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "open-menu");

    cJSON *response = NULL;
    int rc = jw__request_json(socket_path, request, &response);
    if (response) cJSON_Delete(response);
    return rc;
}

static int jw__send_shutdown(const char *socket_path) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "shutdown");

    cJSON *response = NULL;
    int rc = jw__request_json(socket_path, request, &response);
    if (response) cJSON_Delete(response);
    return rc;
}

static void jw__render_launcher(const jw_launcher_state *state) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_LARGE);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    cat_clear_screen();
    cat_draw_screen_title("Jawaka Launcher", NULL);

    SDL_Rect content = cat_get_content_rect(true, false, false);
    int x = CAT_S(24);
    int y = content.y + CAT_S(12);
    int max_w = content.w - CAT_S(48);

    cat_draw_text_wrapped(body_font,
        "Games | Apps | Recents | Favorites | Settings",
        x, y, max_w, theme->accent, CAT_ALIGN_LEFT);
    y += CAT_S(34);

    char line[256];
    snprintf(line, sizeof(line), "Games indexed: %d", state->summary.game_count);
    cat_draw_text(body_font, line, x, y, theme->text);
    y += CAT_S(24);

    snprintf(line, sizeof(line), "Apps indexed: %d", state->summary.app_count);
    cat_draw_text(body_font, line, x, y, theme->text);
    y += CAT_S(24);

    snprintf(line, sizeof(line), "Systems: %s", state->summary.systems_summary[0] ? state->summary.systems_summary : "none");
    cat_draw_text_wrapped(small_font, line, x, y, max_w, theme->text, CAT_ALIGN_LEFT);
    y += CAT_S(30);

    snprintf(line, sizeof(line), "Sample: %s", state->summary.sample_summary[0] ? state->summary.sample_summary : "none");
    cat_draw_text_wrapped(small_font, line, x, y, max_w, theme->text, CAT_ALIGN_LEFT);
    y += CAT_S(40);

    cat_draw_text_wrapped(small_font, state->status, x, y, max_w, theme->hint, CAT_ALIGN_LEFT);
    y += CAT_S(36);

    cat_draw_text_wrapped(small_font,
        "M open menu  |  R rescan  |  Q shutdown",
        x, y, max_w, theme->accent, CAT_ALIGN_LEFT);

    cat_present();
}

int main(void) {
    char *socket_path = jw_socket_path();
    char *db_path = jw_db_path();
    if (!socket_path || !db_path) {
        jw_log_error("could not resolve runtime paths");
        free(socket_path);
        free(db_path);
        return 1;
    }

    if (jw__send_hello(socket_path) != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?", socket_path);
        free(socket_path);
        free(db_path);
        return 1;
    }

    cat_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.window_title = "Jawaka Launcher";
    cfg.disable_background = true;

    if (cat_init(&cfg) != CAT_OK) {
        jw_log_error("catastrophe init failed: %s", cat_get_error());
        free(socket_path);
        free(db_path);
        return 1;
    }

    jw_launcher_state state;
    memset(&state, 0, sizeof(state));
    snprintf(state.summary.systems_summary, sizeof(state.summary.systems_summary), "%s", "pending");
    snprintf(state.summary.sample_summary, sizeof(state.summary.sample_summary), "%s", "pending");
    snprintf(state.status, sizeof(state.status), "%s", "scanning library...");

    jw__render_launcher(&state);

    jw__scan_library(socket_path, db_path, &state);

    bool running = true;
    bool auto_demo = jw__env_flag("JAWAKA_AUTODEMO");
    bool auto_action_sent = false;
    uint32_t auto_delay_ms = jw__env_u32("JAWAKA_AUTODEMO_DELAY_MS", 1200);
    uint32_t started_ms = SDL_GetTicks();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                jw__send_shutdown(socket_path);
                running = false;
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                switch (event.key.keysym.sym) {
                    case SDLK_m:
                        if (jw__send_open_menu(socket_path) == 0) {
                            cat_hide_window();
                            running = false;
                        } else {
                            snprintf(state.status, sizeof(state.status), "%s", "open-menu failed");
                        }
                        break;
                    case SDLK_r:
                        jw__scan_library(socket_path, db_path, &state);
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

        if (auto_demo && !auto_action_sent && SDL_GetTicks() - started_ms >= auto_delay_ms) {
            cat_hide_window();
            jw__send_open_menu(socket_path);
            auto_action_sent = true;
            running = false;
        }

        jw__render_launcher(&state);
        SDL_Delay(16);
    }

    cat_quit();
    free(socket_path);
    free(db_path);
    return 0;
}
