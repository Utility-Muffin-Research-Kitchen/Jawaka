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

#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/message.h>
static void jw__platform_activate(void) {
    /* SDL_RaiseWindow alone doesn't give keyboard focus when the process was
     * spawned from a background daemon. Promote to a regular foreground app
     * and request activation through the Cocoa layer. */
    typedef id   (*id_fn)(id, SEL);
    typedef void (*void_long_fn)(id, SEL, long);
    typedef void (*void_int_fn)(id, SEL, int);
    id app = ((id_fn)objc_msgSend)((id)objc_getClass("NSApplication"),
                                    sel_registerName("sharedApplication"));
    ((void_long_fn)objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 0L);
    ((void_int_fn)objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), 1);
}
#else
static void jw__platform_activate(void) {}
#endif

#define JW_MAX_SYSTEMS 64
#define JW_TAB_GAMES   1

static const char *kTabs[] = { "Recents", "Games", "Apps", "Settings" };

typedef struct {
    jw_library_summary summary;
    jw_system_entry    systems[JW_MAX_SYSTEMS];
    int                system_count;
    int                cursor;
    int                scroll_offset;
    int                visible_rows;
    char               status[256];
    bool               scan_ready;
} jw_launcher_state;

static int jw__env_flag(const char *name) {
    const char *value = getenv(name);
    return value && strcmp(value, "0") != 0;
}

static uint32_t jw__env_u32(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    if (!value || !value[0]) return fallback;
    long parsed = strtol(value, NULL, 10);
    if (parsed <= 0) return fallback;
    return (uint32_t)parsed;
}

static int jw__request_json(const char *socket_path, cJSON *request, cJSON **out_response) {
    char *request_json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_json) return -1;

    char *response_json = NULL;
    size_t response_len = 0;
    int rc = jw_ipc_request(socket_path, request_json, strlen(request_json), &response_json, &response_len);
    cJSON_free(request_json);
    if (rc != 0) return -1;

    cJSON *response = cJSON_Parse(response_json);
    free(response_json);
    if (!response) return -1;

    *out_response = response;
    return 0;
}

static int jw__send_hello(const char *socket_path) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "type", "hello");
    cJSON_AddStringToObject(request, "role", "launcher");

    cJSON *response = NULL;
    if (jw__request_json(socket_path, request, &response) != 0) return -1;

    cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    int ok = cJSON_IsString(type) && type->valuestring &&
             strcmp(type->valuestring, "hello-ok") == 0;
    cJSON_Delete(response);
    return ok ? 0 : -1;
}

static int jw__scan_library(const char *socket_path, const char *db_path,
                             jw_launcher_state *state) {
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

    jw_db_list_systems(db_path, state->systems, JW_MAX_SYSTEMS, &state->system_count);

    state->scan_ready = true;
    state->cursor = 0;
    state->scroll_offset = 0;
    snprintf(state->status, sizeof(state->status), "%d games, %d systems, %d apps",
        state->summary.game_count, state->system_count, state->summary.app_count);
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
    TTF_Font *body_font  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large_font = cat_get_font(CAT_FONT_EXTRA_LARGE);

    cat_clear_screen();

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    /* Tab bar header */
    int header_h = CAT_DS(26);
    cat_draw_rect(0, 0, sw, header_h, theme->accent);

    int tab_font_h = TTF_FontHeight(small_font);
    int tab_y = (header_h - tab_font_h) / 2;
    int tab_x = CAT_S(16);
    for (int i = 0; i < 4; i++) {
        bool active = (i == JW_TAB_GAMES);
        ap_color color = active ? theme->text : theme->hint;
        int tw = cat_draw_text(small_font, kTabs[i], tab_x, tab_y, color);
        if (active) {
            cat_draw_rect(tab_x, header_h - CAT_S(3), tw, CAT_S(3), theme->text);
        }
        tab_x += tw + CAT_S(20);
    }

    /* Content geometry */
    int margin    = CAT_S(12);
    int content_y = header_h;
    int content_h = sh - header_h - fh;

    /* Left panel: systems list (45% of screen width) */
    int list_x   = margin;
    int list_w   = sw * 45 / 100;
    int body_h   = TTF_FontHeight(body_font);
    int item_h   = body_h + CAT_S(12);    /* sized to the actual rendered font */
    int list_h   = content_h - CAT_S(28); /* reserve bottom row for status */
    int visible  = (list_h - margin) / item_h;
    if (visible < 1) visible = 1;

    /* Right panel: art placeholder */
    int art_x = list_w + margin * 2;
    int art_y = content_y + margin;
    int art_w = sw - art_x - margin;
    int art_h = content_h - margin * 2;

    cat_draw_rounded_rect(art_x, art_y, art_w, art_h, CAT_S(8),
        cat_hex_to_color("#ffffff18"));

    /* System name preview inside art panel */
    if (state->system_count > 0 && state->cursor < state->system_count) {
        const char *sys = state->systems[state->cursor].name;
        int large_h = TTF_FontHeight(large_font);
        int tw = cat_measure_text(large_font, sys);
        cat_draw_text_ellipsized(large_font, sys,
            art_x + (art_w - tw) / 2,
            art_y + (art_h - large_h) / 2,
            theme->hint, art_w - margin * 2);
    }

    /* Systems list items */
    if (state->system_count == 0) {
        cat_draw_text_wrapped(body_font,
            state->scan_ready ? "No games found" : "Scanning library...",
            list_x + CAT_S(8), content_y + margin + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        for (int i = state->scroll_offset;
             i < state->system_count && i < state->scroll_offset + visible; i++) {
            int iy = content_y + margin + (i - state->scroll_offset) * item_h;

            int pad    = CAT_S(6);
            int pill_h = body_h + pad;
            int pill_y = iy + (item_h - pill_h) / 2;
            int pill_w = list_w - margin - CAT_S(4);

            if (i == state->cursor) {
                cat_draw_pill(list_x, pill_y, pill_w, pill_h, theme->highlight);
            }

            ap_color name_col  = (i == state->cursor) ? theme->highlighted_text : theme->text;
            ap_color count_col = (i == state->cursor) ? theme->highlighted_text : theme->hint;

            char count_str[16];
            snprintf(count_str, sizeof(count_str), "%d", state->systems[i].game_count);
            int count_w = cat_measure_text(small_font, count_str);
            int name_max_w = pill_w - count_w - CAT_S(24);

            int text_y  = pill_y + (pill_h - body_h) / 2;
            cat_draw_text_ellipsized(body_font, state->systems[i].name,
                list_x + CAT_S(10), text_y, name_col, name_max_w);

            int count_x = list_x + pill_w - count_w - CAT_S(8);
            int small_y = pill_y + (pill_h - TTF_FontHeight(small_font)) / 2;
            cat_draw_text(small_font, count_str, count_x, small_y, count_col);
        }

        if (state->system_count > visible) {
            cat_draw_scrollbar(list_x + list_w - CAT_S(4),
                content_y + margin, list_h - margin,
                visible, state->system_count, state->scroll_offset);
        }
    }

    /* Status line */
    int status_y = sh - fh - CAT_S(26);
    cat_draw_text_ellipsized(small_font, state->status,
        list_x, status_y, theme->hint, list_w);

    /* Footer */
    cat_footer_item footer[] = {
        { CAT_BTN_UP,   "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
        { CAT_BTN_A,    "Select",   false, NULL },
        { CAT_BTN_MENU, "Menu",     false, "M"  },
        { CAT_BTN_Y,    "Rescan",   false, "R"  },
    };
    cat_draw_footer(footer, 4);

    /* This UI consumes raw SDL events directly, so keep Catastrophe out of its
     * idle wait path; otherwise cat_present() can swallow the next wake event. */
    cat_request_frame();
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

    /* On macOS, windows spawned from a background process don't get keyboard
     * focus automatically; raise the window and activate through Cocoa. */
    SDL_RaiseWindow(cat_get_window());
    jw__platform_activate();

    jw_launcher_state state;
    memset(&state, 0, sizeof(state));
    snprintf(state.status, sizeof(state.status), "%s", "scanning library...");

    /* Compute visible_rows — must match the item_h formula in jw__render_launcher */
    {
        int fh = cat_get_footer_height();
        int header_h = CAT_DS(26);
        int content_h = cat_get_screen_height() - header_h - fh;
        int list_h = content_h - CAT_S(28);
        int body_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM));
        int item_h = body_h + CAT_S(12);
        state.visible_rows = (list_h - CAT_S(12)) / item_h;
        if (state.visible_rows < 1) state.visible_rows = 1;
    }

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
                    case SDLK_UP:
                        if (state.cursor > 0) {
                            state.cursor--;
                            if (state.cursor < state.scroll_offset) {
                                state.scroll_offset = state.cursor;
                            }
                        }
                        break;
                    case SDLK_DOWN:
                        if (state.cursor < state.system_count - 1) {
                            state.cursor++;
                            if (state.cursor >= state.scroll_offset + state.visible_rows) {
                                state.scroll_offset = state.cursor - state.visible_rows + 1;
                            }
                        }
                        break;
                    case SDLK_m:
                        if (jw__send_open_menu(socket_path) == 0) {
                            cat_hide_window();
                            running = false;
                        } else {
                            snprintf(state.status, sizeof(state.status), "%s", "open-menu failed");
                        }
                        break;
                    case SDLK_r:
                        snprintf(state.status, sizeof(state.status), "%s", "rescanning...");
                        jw__render_launcher(&state);
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
