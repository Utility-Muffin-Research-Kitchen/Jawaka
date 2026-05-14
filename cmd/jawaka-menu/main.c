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

#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/message.h>
static void jw__platform_activate(void) {
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

static const char *kMenuItems[] = {
    "Rescan Library",
    "Return to Launcher",
    "Shutdown",
};
#define JW_MENU_ITEM_COUNT 3

#define JW_MENU_RESCAN   0
#define JW_MENU_RETURN   1
#define JW_MENU_SHUTDOWN 2

typedef struct {
    int  cursor;
    char status[256];
} jw_menu_state;

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
    cJSON_AddStringToObject(request, "role", "menu");

    cJSON *response = NULL;
    if (jw__request_json(socket_path, request, &response) != 0) return -1;

    cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    int ok = cJSON_IsString(type) && type->valuestring &&
             strcmp(type->valuestring, "hello-ok") == 0;
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

    cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "ok") != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "scan failed: daemon error");
        cJSON_Delete(response);
        return -1;
    }

    cJSON *game_count = cJSON_GetObjectItemCaseSensitive(response, "game_count");
    cJSON *app_count  = cJSON_GetObjectItemCaseSensitive(response, "app_count");
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

static int jw__activate_selection(const char *socket_path, jw_menu_state *state, bool *running);

static void jw__render_menu(const jw_menu_state *state) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    cat_status_bar_opts sb;
    memset(&sb, 0, sizeof(sb));
    sb.show_clock   = CAT_CLOCK_AUTO;
    sb.show_battery = true;

    cat_clear_screen();
    cat_draw_screen_title("Menu", &sb);

    SDL_Rect content = cat_get_content_rect(true, true, false);
    int x      = content.x + CAT_S(24);
    int item_w = content.w * 55 / 100;

    int body_h  = TTF_FontHeight(body_font);
    int item_h  = body_h + CAT_S(12);
    int pad     = CAT_S(6);
    int pill_h  = body_h + pad;
    int pill_w  = item_w;

    for (int i = 0; i < JW_MENU_ITEM_COUNT; i++) {
        int iy     = content.y + CAT_S(16) + i * item_h;
        int pill_y = iy + (item_h - pill_h) / 2;

        if (i == state->cursor) {
            cat_draw_pill(x - CAT_S(10), pill_y, pill_w, pill_h, theme->highlight);
        }

        ap_color color = (i == state->cursor) ? theme->highlighted_text : theme->text;
        int text_y = pill_y + (pill_h - body_h) / 2;
        cat_draw_text(body_font, kMenuItems[i], x, text_y, color);
    }

    /* Status line */
    if (state->status[0]) {
        int status_y = content.y + content.h - CAT_S(28);
        cat_draw_text_ellipsized(small_font, state->status,
            x, status_y, theme->hint, item_w);
    }

    /* Footer */
    cat_footer_item footer[] = {
        { CAT_BTN_UP, "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
        { CAT_BTN_A,  "Select",   false, "A" },
        { CAT_BTN_B,  "Back",     false, "B" },
    };
    cat_draw_footer(footer, 3);

    cat_present();
}

static void jw__move_cursor(jw_menu_state *state, int delta) {
    if (!state || delta == 0) return;

    int next = state->cursor + delta;
    if (next < 0) next = 0;
    if (next >= JW_MENU_ITEM_COUNT) next = JW_MENU_ITEM_COUNT - 1;
    state->cursor = next;
}

static int jw__activate_selection(const char *socket_path, jw_menu_state *state, bool *running) {
    switch (state->cursor) {
        case JW_MENU_RESCAN:
            snprintf(state->status, sizeof(state->status), "%s", "scanning...");
            cat_request_frame();
            jw__render_menu(state);
            return jw__send_scan(socket_path, state);
        case JW_MENU_RETURN:
            *running = false;
            return 0;
        case JW_MENU_SHUTDOWN:
            jw__send_shutdown(socket_path);
            *running = false;
            return 0;
        default:
            return 0;
    }
}

static void jw__handle_input(const char *socket_path, jw_menu_state *state,
                              cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            jw__move_cursor(state, -1);
            break;
        case CAT_BTN_DOWN:
            jw__move_cursor(state, 1);
            break;
        case CAT_BTN_A:
        case CAT_BTN_START:
            jw__activate_selection(socket_path, state, running);
            break;
        case CAT_BTN_B:
            *running = false;
            break;
        default:
            break;
    }
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

    /* On macOS, windows spawned from a background process don't get keyboard
     * focus automatically; raise the window and activate through Cocoa. */
    SDL_RaiseWindow(cat_get_window());
    jw__platform_activate();

    jw_menu_state state;
    memset(&state, 0, sizeof(state));
    snprintf(state.status, sizeof(state.status), "%s", "menu ready");

    bool running = true;
    bool auto_demo = jw__env_flag("JAWAKA_AUTODEMO");
    bool auto_shutdown_sent = false;
    uint32_t auto_delay_ms = jw__env_u32("JAWAKA_AUTODEMO_DELAY_MS", 1200);
    uint32_t started_ms = SDL_GetTicks();

    while (running) {
        cat_input_event event;
        while (cat_poll_input(&event)) {
            if (!event.pressed) continue;
            jw__handle_input(socket_path, &state, event.button, &running);
        }

        if (auto_demo && !auto_shutdown_sent) {
            uint32_t elapsed_ms = SDL_GetTicks() - started_ms;
            if (elapsed_ms >= auto_delay_ms) {
                jw__send_shutdown(socket_path);
                auto_shutdown_sent = true;
                running = false;
            } else {
                cat_request_frame_in(auto_delay_ms - elapsed_ms);
            }
        }

        jw__render_menu(&state);
    }

    cat_quit();
    free(socket_path);
    return 0;
}
