#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "internal/core/autodemo.h"
#include "internal/core/log.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/paths.h"
#include "internal/settings/settings.h"
#include "internal/settings/theme_resolve.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *jw_hint(const char *desktop_key) {
    return CAT_PLATFORM_IS_DEVICE ? NULL : desktop_key;
}
static inline const char *jw_hint_device(const char *desktop_key, const char *device_key) {
    return CAT_PLATFORM_IS_DEVICE ? device_key : desktop_key;
}
#define JW_HINT(dk)            jw_hint(dk)
#define JW_HINT_DEVICE(dk, vk) jw_hint_device(dk, vk)

static const char *kMenuItems[] = {
    "Rescan Library",
    "Return to Launcher",
    "Exit to Stock",
    "Reboot",
    "Power Off",
};
#define JW_MENU_COUNT       5
#define JW_MENU_RESCAN      0
#define JW_MENU_RETURN      1
#define JW_MENU_EXIT_STOCK  2
#define JW_MENU_REBOOT      3
#define JW_MENU_POWEROFF    4

typedef struct {
    cat_list_state      list;
    char                status[256];
    cat_status_bar_opts status_bar;
    bool                show_hints;
} jw_menu_state;

static void jw__render_menu(const jw_menu_state *state) {
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small     = cat_get_font(CAT_FONT_SMALL);

    cat_status_bar_opts sb = state->status_bar;

    cat_clear_screen();
    cat_draw_screen_title("Menu", &sb);

    SDL_Rect content = cat_get_content_rect(true, true, false);
    int x      = content.x + CAT_S(24);
    int item_w = content.w * 55 / 100;
    int body_h = TTF_FontHeight(body_font);
    int item_h = body_h + CAT_S(12);
    int pill_h = body_h + CAT_S(6);
    int pill_w = item_w;

    for (int i = 0; i < JW_MENU_COUNT; i++) {
        int iy     = content.y + CAT_S(16) + i * item_h;
        int pill_y = iy + (item_h - pill_h) / 2;
        bool sel   = (i == state->list.cursor);

        if (sel)
            cat_draw_pill(x - CAT_S(10), pill_y, pill_w, pill_h, theme->highlight);

        ap_color col = sel ? theme->highlighted_text : theme->text;
        int text_y   = pill_y + (pill_h - body_h) / 2;
        cat_draw_text(body_font, kMenuItems[i], x, text_y, col);
    }

    /* Status line follows the same hints toggle as the footer: hidden when
       hints are off, matching the launcher. Only carries real feedback now
       (e.g. rescan results), so it stays empty until something sets it. */
    if (state->show_hints && state->status[0]) {
        int status_y = content.y + content.h - CAT_S(28);
        cat_draw_text_ellipsized(small, state->status, x, status_y,
                                 theme->hint, item_w);
    }

    if (state->show_hints) {
        cat_footer_item footer[] = {
            { CAT_BTN_UP, "Navigate", false, JW_HINT_DEVICE("\xe2\x86\x91\xe2\x86\x93", "\xe2\x86\x91\xe2\x86\x93") },
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        cat_draw_footer(footer, 3);
    }
    cat_present();
}

static int jw__activate(const char *socket_path, jw_menu_state *state, bool *running) {
    switch (state->list.cursor) {
        case JW_MENU_RESCAN:
            snprintf(state->status, sizeof(state->status), "%s", "scanning...");
            cat_request_frame();
            jw__render_menu(state);
            return jw_ipc_scan_library(socket_path, state->status, sizeof(state->status));
        case JW_MENU_RETURN:
            *running = false;
            return 0;
        case JW_MENU_EXIT_STOCK:
            /* EXIT-TO-STOCK: temporary dev/test feature. Sends shutdown IPC
               which writes /tmp/umrk-exit-to-stock sentinel. See jawakad
               shutdown handler and loong_pangu.wrapper sentinel check. */
            jw_ipc_shutdown(socket_path);
            *running = false;
            return 0;
        case JW_MENU_REBOOT:
            jw_ipc_platform_action(socket_path, "reboot", 0);
            *running = false;
            return 0;
        case JW_MENU_POWEROFF:
            jw_ipc_platform_action(socket_path, "poweroff", 0);
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
            cat_list_state_move(&state->list, -1, JW_MENU_COUNT);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->list, +1, JW_MENU_COUNT);
            break;
        case CAT_BTN_A:
        case CAT_BTN_START:
            jw__activate(socket_path, state, running);
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
    char *db_path     = jw_db_path();
    if (!socket_path) {
        jw_log_error("could not resolve socket path");
        free(db_path);
        return 1;
    }

    if (jw_ipc_hello(socket_path, "menu") != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?",
                     socket_path);
        free(socket_path);
        free(db_path);
        return 1;
    }

    cat_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.window_title       = "Jawaka Menu";
    cfg.disable_background = true;

    if (cat_init(&cfg) != CAT_OK) {
        jw_log_error("catastrophe init failed: %s", cat_get_error());
        free(socket_path);
        free(db_path);
        return 1;
    }

    /* Resolve theme: env > DB > default Jawaka-Tabs.
       Matches launcher precedence so menu inherits whatever was last picked. */
    {
        char theme_name[256];
        jw_resolve_theme_name(db_path, theme_name, sizeof(theme_name));
        cat_stylesheet ss;
        if (cat_stylesheet_load_theme(&ss, theme_name) == CAT_OK)
            cat_stylesheet_apply(&ss);

        /* Apply the user's persisted color/layout overrides on top of the
           theme so the menu matches the launcher's customized appearance. */
        jw_settings_apply_persisted_overrides(db_path);
    }

    cat_activate_window();

    jw_menu_state state;
    memset(&state, 0, sizeof(state));
    cat_list_state_init(&state.list, JW_MENU_COUNT);
    /* status stays empty (memset above); the line only shows real feedback. */

    /* Inherit the launcher's status-bar and button-hint preferences. */
    jw_settings_load_status_prefs(db_path, &state.status_bar, &state.show_hints);

    jw_autodemo demo;
    jw_autodemo_init(&demo);
    bool running = true;

    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            jw__handle_input(socket_path, &state, ev.button, &running);
        }

        if (demo.enabled && !demo.fired) {
            uint32_t rem = jw_autodemo_remaining_ms(&demo);
            if (jw_autodemo_should_fire(&demo)) {
                jw_ipc_shutdown(socket_path);
                running = false;
            } else {
                cat_request_frame_in(rem);
            }
        }

        jw__render_menu(&state);
    }

    cat_quit();
    free(socket_path);
    free(db_path);
    return 0;
}
