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

static const char *kInGameItems[] = {
    "Continue",
    "Save State",
    "Load State",
    "Reset",
    "RetroArch Settings",
    "Quit",
};
#define JW_INGAME_COUNT     6
#define JW_INGAME_CONTINUE  0
#define JW_INGAME_SAVE      1
#define JW_INGAME_LOAD      2
#define JW_INGAME_RESET     3
#define JW_INGAME_SETTINGS  4
#define JW_INGAME_QUIT      5

typedef struct {
    cat_list_state      list;
    char                status[256];
    cat_status_bar_opts status_bar;
    bool                show_hints;
} jw_menu_state;

typedef struct {
    cat_list_state                  list;
    char                            status[256];
    cat_status_bar_opts             status_bar;
    bool                            show_hints;
    jw_ipc_retroarch_session_info   session;
} jw_ingame_state;

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

static const char *jw__basename_const(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (path ? path : "");
}

static void jw__slot_label(const jw_ingame_state *state, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (!state->session.active || !state->session.command_ok ||
        !state->session.savestate_supported) {
        snprintf(out, out_size, "%s", "No states");
        return;
    }
    if (state->session.state_slot < 0) {
        snprintf(out, out_size, "%s", "Slot Auto");
    } else {
        snprintf(out, out_size, "Slot %d", state->session.state_slot);
    }
}

static void jw__ingame_detail(const jw_ingame_state *state, int item,
                              char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (!state->session.active) {
        return;
    }

    if (item == JW_INGAME_CONTINUE && state->session.disk_count > 1) {
        int slot = state->session.disk_slot >= 0 ? state->session.disk_slot : 0;
        snprintf(out, out_size, "Disk %d/%d", slot + 1,
                 state->session.disk_count);
        return;
    }

    if (item == JW_INGAME_SAVE || item == JW_INGAME_LOAD) {
        jw__slot_label(state, out, out_size);
    }
}

static void jw__render_ingame_menu(const jw_ingame_state *state) {
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small     = cat_get_font(CAT_FONT_SMALL);

    cat_status_bar_opts sb = state->status_bar;

    cat_clear_screen();
    cat_draw_screen_title("In-Game Menu", &sb);

    SDL_Rect content = cat_get_content_rect(true, true, false);
    int pad       = CAT_S(24);
    int x         = content.x + pad;
    int right     = content.x + content.w - pad;
    int list_w    = right - x;
    int body_h    = TTF_FontHeight(body_font);
    int small_h   = TTF_FontHeight(small);
    int line_gap  = CAT_S(5);
    int meta_y    = content.y + CAT_S(12);
    bool show_command = state->session.active && !state->session.command_ok;
    int top_y     = meta_y + small_h + CAT_S(12);
    int status_h  = (state->show_hints && state->status[0])
                  ? small_h + CAT_S(12) : 0;
    int bottom_y  = content.y + content.h - CAT_S(10) - status_h;
    int item_h    = body_h + CAT_S(12);
    int min_item_h = body_h + CAT_S(4);
    int item_w    = list_w * 62 / 100;
    int detail_x  = x + item_w + CAT_S(14);
    int detail_w  = right - detail_x;
    if (show_command) {
        top_y += small_h + line_gap;
    }
    if (item_h * JW_INGAME_COUNT > bottom_y - top_y && bottom_y > top_y) {
        item_h = (bottom_y - top_y) / JW_INGAME_COUNT;
        if (item_h < min_item_h) {
            item_h = min_item_h;
        }
    }
    int pill_h   = item_h - CAT_S(4);
    if (pill_h < body_h + CAT_S(2)) {
        pill_h = body_h + CAT_S(2);
    }
    int pill_w = list_w;

    char session_line[384];
    if (state->session.active) {
        const char *rom = jw__basename_const(state->session.rom_path);
        if (state->session.system[0]) {
            snprintf(session_line, sizeof(session_line), "%s - %s",
                     state->session.system, rom);
        } else {
            snprintf(session_line, sizeof(session_line), "%s", rom);
        }
    } else {
        snprintf(session_line, sizeof(session_line), "%s",
                 "No active RetroArch session");
    }
    cat_draw_text_ellipsized(small, session_line, x, meta_y,
                             theme->hint, list_w);

    if (show_command) {
        char command_line[128];
        snprintf(command_line, sizeof(command_line), "Command: %s",
                 state->session.command_result);
        cat_draw_text_ellipsized(small, command_line, x,
                                 meta_y + small_h + line_gap,
                                 theme->hint, list_w);
    }

    for (int i = 0; i < JW_INGAME_COUNT; i++) {
        int iy     = top_y + i * item_h;
        int pill_y = iy + (item_h - pill_h) / 2;
        bool sel   = (i == state->list.cursor);

        if (sel)
            cat_draw_pill(x - CAT_S(10), pill_y, pill_w, pill_h, theme->highlight);

        ap_color col = sel ? theme->highlighted_text : theme->text;
        int text_y   = pill_y + (pill_h - body_h) / 2;

        char detail[64];
        jw__ingame_detail(state, i, detail, sizeof(detail));
        int label_w = detail[0] && detail_w > CAT_S(40)
                    ? detail_x - x - CAT_S(10) : list_w - CAT_S(8);
        cat_draw_text_ellipsized(body_font, kInGameItems[i], x, text_y,
                                 col, label_w);
        if (detail[0] && detail_w > CAT_S(40)) {
            cat_draw_text_ellipsized(small, detail, detail_x,
                                     text_y + CAT_S(2),
                                     sel ? theme->highlighted_text : theme->hint,
                                     detail_w);
        }
    }

    if (state->show_hints && state->status[0]) {
        int status_y = content.y + content.h - small_h - CAT_S(10);
        cat_draw_text_ellipsized(small, state->status, x, status_y,
                                 theme->hint, list_w);
    }

    if (state->show_hints) {
        cat_footer_item footer[] = {
            { CAT_BTN_UP, "Move", false, JW_HINT_DEVICE("\xe2\x86\x91\xe2\x86\x93", "\xe2\x86\x91\xe2\x86\x93") },
            { CAT_BTN_LEFT, "Adjust", false, JW_HINT_DEVICE("\xe2\x86\x90\xe2\x86\x92", "\xe2\x86\x90\xe2\x86\x92") },
            { CAT_BTN_B,  "Resume", true,  JW_HINT("B") },
            { CAT_BTN_A,  "OK",     true,  JW_HINT("A") },
        };
        cat_draw_footer(footer, 4);
    }
    cat_present();
}

static void jw__ingame_refresh(const char *socket_path, jw_ingame_state *state) {
    if (jw_ipc_get_retroarch_session(socket_path, &state->session,
                                     state->status,
                                     sizeof(state->status)) != 0) {
        memset(&state->session, 0, sizeof(state->session));
        if (!state->status[0]) {
            snprintf(state->status, sizeof(state->status), "%s",
                     "RetroArch session unavailable");
        }
    } else if (state->session.active && state->session.command_ok) {
        state->status[0] = '\0';
    }
}

static void jw__ingame_continue(const char *socket_path, jw_ingame_state *state,
                                bool *running) {
    if (!state->session.active) {
        *running = false;
        return;
    }

    if (jw_ipc_retroarch_action(socket_path, "continue", 0,
                                state->status, sizeof(state->status)) == 0) {
        *running = false;
    }
}

static int jw__ingame_activate(const char *socket_path, jw_ingame_state *state,
                               bool *running) {
    if (!state->session.active) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "No active RetroArch session");
        *running = false;
        return -1;
    }

    const char *action = NULL;
    int value = 0;

    switch (state->list.cursor) {
        case JW_INGAME_CONTINUE:
            jw__ingame_continue(socket_path, state, running);
            return 0;
        case JW_INGAME_SAVE:
            if (!state->session.savestate_supported) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Savestates are not available");
                return -1;
            }
            action = "save-state";
            value = state->session.state_slot;
            break;
        case JW_INGAME_LOAD:
            if (!state->session.savestate_supported) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Savestates are not available");
                return -1;
            }
            action = "load-state";
            value = state->session.state_slot;
            break;
        case JW_INGAME_RESET:
            action = "reset";
            break;
        case JW_INGAME_SETTINGS:
            action = "settings";
            break;
        case JW_INGAME_QUIT:
            action = "quit";
            break;
        default:
            return 0;
    }

    if (jw_ipc_retroarch_action(socket_path, action, value,
                                state->status, sizeof(state->status)) == 0) {
        *running = false;
    }
    return 0;
}

static void jw__ingame_adjust(const char *socket_path, jw_ingame_state *state,
                              int delta) {
    const char *action = NULL;
    if (state->list.cursor == JW_INGAME_CONTINUE &&
        state->session.disk_count > 1) {
        action = delta < 0 ? "disk-prev" : "disk-next";
    } else if ((state->list.cursor == JW_INGAME_SAVE ||
                state->list.cursor == JW_INGAME_LOAD) &&
               state->session.savestate_supported) {
        action = delta < 0 ? "state-slot-prev" : "state-slot-next";
    }

    if (!action) {
        return;
    }

    if (jw_ipc_retroarch_action(socket_path, action, 0,
                                state->status, sizeof(state->status)) == 0) {
        jw__ingame_refresh(socket_path, state);
    }
}

static void jw__handle_ingame_input(const char *socket_path,
                                    jw_ingame_state *state,
                                    cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->list, -1, JW_INGAME_COUNT);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->list, +1, JW_INGAME_COUNT);
            break;
        case CAT_BTN_LEFT:
            jw__ingame_adjust(socket_path, state, -1);
            break;
        case CAT_BTN_RIGHT:
            jw__ingame_adjust(socket_path, state, +1);
            break;
        case CAT_BTN_A:
        case CAT_BTN_START:
            jw__ingame_activate(socket_path, state, running);
            break;
        case CAT_BTN_B:
            jw__ingame_continue(socket_path, state, running);
            break;
        default:
            break;
    }
}

static int jw__run_ingame_menu(const char *socket_path, const char *db_path) {
    jw_ingame_state state;
    memset(&state, 0, sizeof(state));
    cat_list_state_init(&state.list, JW_INGAME_COUNT);
    jw_settings_load_status_prefs(db_path, &state.status_bar, &state.show_hints);
    jw__ingame_refresh(socket_path, &state);

    bool running = true;
    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            jw__handle_ingame_input(socket_path, &state, ev.button, &running);
        }

        jw__render_ingame_menu(&state);
    }

    return 0;
}

int main(int argc, char **argv) {
    bool in_game = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--in-game") == 0) {
            in_game = true;
            continue;
        }
        fprintf(stderr, "Usage: jawaka-menu [--in-game]\n");
        return 2;
    }

    char *socket_path = jw_socket_path();
    char *db_path     = jw_db_path();
    if (!socket_path) {
        jw_log_error("could not resolve socket path");
        free(db_path);
        return 1;
    }

    if (jw_ipc_hello(socket_path, in_game ? "ingame-menu" : "menu") != 0) {
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

    if (in_game) {
        int rc = jw__run_ingame_menu(socket_path, db_path);
        cat_quit();
        free(socket_path);
        free(db_path);
        return rc;
    }

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
