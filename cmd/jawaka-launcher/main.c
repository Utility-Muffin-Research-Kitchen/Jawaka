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
#include <ctype.h>

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

#define JW_MAX_SYSTEMS 64
#define JW_MAX_APPS    64

typedef enum {
    JW_TAB_RECENTS = 0,
    JW_TAB_GAMES,
    JW_TAB_APPS,
    JW_TAB_SETTINGS,
    JW_TAB_COUNT
} jw_tab;

static const char *kTabs[JW_TAB_COUNT] = { "Recents", "Games", "Apps", "Settings" };

static const char *kSettingsItems[] = {
    "Rescan Library",
    "Return to Menu",
    "Shutdown",
};
#define JW_SETTINGS_RESCAN   0
#define JW_SETTINGS_RETURN   1
#define JW_SETTINGS_SHUTDOWN 2
#define JW_SETTINGS_COUNT    3

typedef struct {
    jw_tab               current_tab;
    jw_library_summary   summary;
    jw_system_entry      systems[JW_MAX_SYSTEMS];
    int                  system_count;
    jw_app_entry         apps[JW_MAX_APPS];
    int                  app_count;
    int                  cursor;
    int                  scroll_offset;
    int                  visible_rows;
    char                 status[256];
    bool                 scan_ready;
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

static int jw__list_count(const jw_launcher_state *state) {
    switch (state->current_tab) {
        case JW_TAB_RECENTS:  return 0;
        case JW_TAB_GAMES:    return state->system_count;
        case JW_TAB_APPS:     return state->app_count;
        case JW_TAB_SETTINGS: return JW_SETTINGS_COUNT;
        default:              return 0;
    }
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
    jw_db_list_apps(db_path, state->apps, JW_MAX_APPS, &state->app_count);

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

static void jw__switch_tab(jw_launcher_state *state, int direction) {
    if (!state) return;
    int next = (state->current_tab + direction) % JW_TAB_COUNT;
    if (next < 0) next += JW_TAB_COUNT;
    state->current_tab = (jw_tab)next;
    state->cursor = 0;
    state->scroll_offset = 0;
}

static void jw__move_cursor(jw_launcher_state *state, int delta) {
    if (!state || delta == 0) return;
    int count = jw__list_count(state);
    if (count == 0) return;

    int next = state->cursor + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next == state->cursor) return;

    state->cursor = next;
    if (state->cursor < state->scroll_offset) {
        state->scroll_offset = state->cursor;
    } else if (state->cursor >= state->scroll_offset + state->visible_rows) {
        state->scroll_offset = state->cursor - state->visible_rows + 1;
    }
}

static void jw__page_cursor(jw_launcher_state *state, int delta) {
    if (!state || delta == 0) return;
    int count = jw__list_count(state);
    if (count == 0) return;

    int page = state->visible_rows;
    int next = state->cursor + page * delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next == state->cursor) return;

    state->cursor = next;
    if (state->cursor < state->scroll_offset) {
        state->scroll_offset = state->cursor;
    } else if (state->cursor >= state->scroll_offset + state->visible_rows) {
        state->scroll_offset = state->cursor - state->visible_rows + 1;
    }
}

static void jw__jump_letter(jw_launcher_state *state, int direction) {
    if (!state || state->current_tab != JW_TAB_GAMES || state->system_count == 0) return;
    if (direction == 0) return;

    int cur = state->cursor;
    if (cur < 0 || cur >= state->system_count) cur = 0;

    char cur_first = (char)toupper(state->systems[cur].name[0]);
    int count = state->system_count;
    int best = -1;

    if (direction > 0) {
        for (int i = cur + 1; i < count; i++) {
            char c = (char)toupper(state->systems[i].name[0]);
            if (c != cur_first) { best = i; break; }
        }
        if (best < 0) {
            for (int i = 0; i < cur; i++) {
                char c = (char)toupper(state->systems[i].name[0]);
                if (c != cur_first) { best = i; break; }
            }
        }
    } else {
        for (int i = cur - 1; i >= 0; i--) {
            char c = (char)toupper(state->systems[i].name[0]);
            if (c != cur_first) { best = i; break; }
        }
        if (best < 0) {
            for (int i = count - 1; i > cur; i--) {
                char c = (char)toupper(state->systems[i].name[0]);
                if (c != cur_first) { best = i; break; }
            }
        }
    }

    if (best >= 0 && best != state->cursor) {
        state->cursor = best;
        if (state->cursor < state->scroll_offset) {
            state->scroll_offset = state->cursor;
        } else if (state->cursor >= state->scroll_offset + state->visible_rows) {
            state->scroll_offset = state->cursor - state->visible_rows + 1;
        }
    }
}

static void jw__activate_item(const char *socket_path, const char *db_path,
                               jw_launcher_state *state, bool *running) {
    switch (state->current_tab) {
        case JW_TAB_GAMES:
        case JW_TAB_APPS:
            snprintf(state->status, sizeof(state->status), "%s", "Coming soon");
            break;
        case JW_TAB_SETTINGS: {
            int idx = state->cursor;
            if (idx == JW_SETTINGS_RESCAN) {
                snprintf(state->status, sizeof(state->status), "%s", "rescanning...");
                cat_request_frame();
                jw__scan_library(socket_path, db_path, state);
            } else if (idx == JW_SETTINGS_RETURN) {
                if (jw__send_open_menu(socket_path) == 0) {
                    cat_hide_window();
                    *running = false;
                } else {
                    snprintf(state->status, sizeof(state->status), "%s", "open-menu failed");
                }
            } else if (idx == JW_SETTINGS_SHUTDOWN) {
                jw__send_shutdown(socket_path);
                *running = false;
            }
            break;
        }
        default:
            break;
    }
}

/* ─── Render helpers ──────────────────────────────────────── */

static void jw__draw_tab_bar(jw_tab active_tab) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int header_h = CAT_DS(26);
    cat_draw_rect(0, 0, sw, header_h, theme->accent);

    int tab_font_h = TTF_FontHeight(small_font);
    int tab_y = (header_h - tab_font_h) / 2;
    int tab_x = CAT_S(16);
    for (int i = 0; i < JW_TAB_COUNT; i++) {
        bool active = (i == (int)active_tab);
        ap_color color = active ? theme->text : theme->hint;
        int tw = cat_draw_text(small_font, kTabs[i], tab_x, tab_y, color);
        if (active) {
            cat_draw_rect(tab_x, header_h - CAT_S(3), tw, CAT_S(3), theme->text);
        }
        tab_x += tw + CAT_S(20);
    }
}

static void jw__draw_list_items(int x, int y, int w, int item_h, int body_h,
                                 int count, int scroll_offset, int visible, int cursor,
                                 void (*draw_item)(int idx, int ix, int iy, int iw, int ibody, bool selected, void *user),
                                 void *user) {
    ap_theme *theme = cat_get_theme();
    int pill_h = body_h + CAT_S(6);

    for (int i = scroll_offset; i < count && i < scroll_offset + visible; i++) {
        int iy = y + (i - scroll_offset) * item_h;
        int pill_y = iy + (item_h - pill_h) / 2;
        int pill_w = w - CAT_S(4);
        bool selected = (i == cursor);

        if (selected) {
            cat_draw_pill(x, pill_y, pill_w, pill_h, theme->highlight);
        }

        draw_item(i, x, pill_y, pill_w, pill_h, selected, user);
    }

    if (count > visible) {
        int list_h = y + visible * item_h - y;
        cat_draw_scrollbar(x + w - CAT_S(4), y, list_h - CAT_S(4),
            visible, count, scroll_offset);
    }
}

/* ─── Render: Recents ─────────────────────────────────────── */

static void jw__render_recents(const jw_launcher_state *state,
                                int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large_font = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();

    /* Center a simple empty-state message */
    const char *msg = "No recent games";
    int large_h = TTF_FontHeight(large_font);
    int tw = cat_measure_text(large_font, msg);
    cat_draw_text(large_font, msg,
        (sw - tw) / 2,
        content_y + (content_h - large_h) / 2,
        theme->hint);

    (void)body_font;
    (void)margin;
}

/* ─── Render: Games ───────────────────────────────────────── */

typedef struct {
    const jw_system_entry *systems;
} jw__games_draw_ctx;

static void jw__draw_game_item(int idx, int ix, int iy, int iw, int ih,
                                bool selected, void *user) {
    jw__games_draw_ctx *ctx = (jw__games_draw_ctx *)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    ap_color name_col  = selected ? theme->highlighted_text : theme->text;
    ap_color count_col = selected ? theme->highlighted_text : theme->hint;

    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", ctx->systems[idx].game_count);
    int count_w = cat_measure_text(small_font, count_str);
    int name_max_w = iw - count_w - CAT_S(24);

    int text_y = iy + (ih - TTF_FontHeight(body_font)) / 2;
    cat_draw_text_ellipsized(body_font, ctx->systems[idx].name,
        ix + CAT_S(10), text_y, name_col, name_max_w);

    int count_x = ix + iw - count_w - CAT_S(8);
    int small_y = iy + (ih - TTF_FontHeight(small_font)) / 2;
    cat_draw_text(small_font, count_str, count_x, small_y, count_col);
}

static void jw__render_games(const jw_launcher_state *state,
                              int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large_font = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    /* Left panel: systems list (45% of screen width) */
    int list_x   = margin;
    int list_w   = sw * 45 / 100;
    int body_h   = TTF_FontHeight(body_font);
    int item_h   = body_h + CAT_S(12);
    int list_h   = content_h - CAT_S(28);
    int visible  = (list_h - margin) / item_h;
    if (visible < 1) visible = 1;

    /* Right panel: art placeholder */
    int art_x = list_w + margin * 2;
    int art_y = content_y + margin;
    int art_w = sw - art_x - margin;
    int art_h = content_h - margin * 2;

    cat_draw_rounded_rect(art_x, art_y, art_w, art_h, CAT_S(8),
        cat_hex_to_color("#ffffff18"));

    if (state->system_count > 0 && state->cursor < state->system_count) {
        const char *sys = state->systems[state->cursor].name;
        int large_h = TTF_FontHeight(large_font);
        int tw = cat_measure_text(large_font, sys);
        cat_draw_text_ellipsized(large_font, sys,
            art_x + (art_w - tw) / 2,
            art_y + (art_h - large_h) / 2,
            theme->hint, art_w - margin * 2);
    }

    /* Systems list */
    if (state->system_count == 0) {
        cat_draw_text_wrapped(body_font,
            state->scan_ready ? "No games found" : "Scanning library...",
            list_x + CAT_S(8), content_y + margin + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__games_draw_ctx ctx;
        ctx.systems = state->systems;
        jw__draw_list_items(list_x, content_y + margin, list_w,
            item_h, body_h,
            state->system_count, state->scroll_offset, visible, state->cursor,
            jw__draw_game_item, &ctx);
    }

    /* Status */
    int status_y = sh - fh - CAT_S(26);
    cat_draw_text_ellipsized(small_font, state->status,
        list_x, status_y, theme->hint, list_w);

    (void)margin;
}

/* ─── Render: Apps ────────────────────────────────────────── */

typedef struct {
    const jw_app_entry *apps;
} jw__apps_draw_ctx;

static void jw__draw_app_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    jw__apps_draw_ctx *ctx = (jw__apps_draw_ctx *)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);

    ap_color name_col = selected ? theme->highlighted_text : theme->text;

    int text_y = iy + (ih - TTF_FontHeight(body_font)) / 2;
    cat_draw_text_ellipsized(body_font, ctx->apps[idx].name,
        ix + CAT_S(10), text_y, name_col, iw - CAT_S(20));
}

static void jw__render_apps(const jw_launcher_state *state,
                             int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large_font = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    /* Left panel: app list (45% of screen width) */
    int list_x   = margin;
    int list_w   = sw * 45 / 100;
    int body_h   = TTF_FontHeight(body_font);
    int item_h   = body_h + CAT_S(12);
    int list_h   = content_h - CAT_S(28);
    int visible  = (list_h - margin) / item_h;
    if (visible < 1) visible = 1;

    /* Right panel: placeholder */
    int art_x = list_w + margin * 2;
    int art_y = content_y + margin;
    int art_w = sw - art_x - margin;
    int art_h = content_h - margin * 2;

    cat_draw_rounded_rect(art_x, art_y, art_w, art_h, CAT_S(8),
        cat_hex_to_color("#ffffff18"));

    if (state->app_count > 0 && state->cursor < state->app_count) {
        const char *app_name = state->apps[state->cursor].name;
        int large_h = TTF_FontHeight(large_font);
        int tw = cat_measure_text(large_font, app_name);
        cat_draw_text_ellipsized(large_font, app_name,
            art_x + (art_w - tw) / 2,
            art_y + (art_h - large_h) / 2,
            theme->hint, art_w - margin * 2);
    }

    /* App list */
    if (state->app_count == 0) {
        cat_draw_text_wrapped(body_font,
            state->scan_ready ? "No apps found" : "Scanning library...",
            list_x + CAT_S(8), content_y + margin + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__apps_draw_ctx ctx;
        ctx.apps = state->apps;
        jw__draw_list_items(list_x, content_y + margin, list_w,
            item_h, body_h,
            state->app_count, state->scroll_offset, visible, state->cursor,
            jw__draw_app_item, &ctx);
    }

    /* Status */
    int status_y = sh - fh - CAT_S(26);
    cat_draw_text_ellipsized(small_font, state->status,
        list_x, status_y, theme->hint, list_w);

    (void)margin;
}

/* ─── Render: Settings ────────────────────────────────────── */

typedef struct {
    int dummy;
} jw__settings_draw_ctx;

static void jw__draw_settings_item(int idx, int ix, int iy, int iw, int ih,
                                    bool selected, void *user) {
    (void)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);

    ap_color name_col = selected ? theme->highlighted_text : theme->text;

    int text_y = iy + (ih - TTF_FontHeight(body_font)) / 2;
    cat_draw_text_ellipsized(body_font, kSettingsItems[idx],
        ix + CAT_S(10), text_y, name_col, iw - CAT_S(20));
}

static void jw__render_settings(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body_font  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    /* Full-width list — no art panel for settings */
    int list_x = margin;
    int list_w = sw - margin * 2;
    int body_h = TTF_FontHeight(body_font);
    int item_h = body_h + CAT_S(12);
    int list_h = content_h - CAT_S(28);
    int visible = (list_h - margin) / item_h;
    if (visible < 1) visible = 1;

    jw__settings_draw_ctx ctx;
    jw__draw_list_items(list_x, content_y + margin, list_w,
        item_h, body_h,
        JW_SETTINGS_COUNT, state->scroll_offset, visible, state->cursor,
        jw__draw_settings_item, &ctx);

    int status_y = sh - fh - CAT_S(26);
    cat_draw_text_ellipsized(small_font, state->status,
        list_x, status_y, theme->hint, list_w);

    (void)content_h;
    (void)margin;
}

/* ─── Main render dispatch ────────────────────────────────── */

static void jw__render_launcher(const jw_launcher_state *state) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    cat_clear_screen();

    int sw = cat_get_screen_width();
    (void)sw;
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    /* Tab bar */
    jw__draw_tab_bar(state->current_tab);

    /* Content area */
    int header_h = CAT_DS(26);
    int content_y = header_h;
    int content_h = sh - header_h - fh;
    int margin = CAT_S(12);

    /* Dispatch per-tab render */
    switch (state->current_tab) {
        case JW_TAB_RECENTS:
            jw__render_recents(state, content_y, content_h, margin);
            break;
        case JW_TAB_GAMES:
            jw__render_games(state, content_y, content_h, margin);
            break;
        case JW_TAB_APPS:
            jw__render_apps(state, content_y, content_h, margin);
            break;
        case JW_TAB_SETTINGS:
            jw__render_settings(state, content_y, content_h, margin);
            break;
        default:
            break;
    }

    /* Footer */
    cat_footer_item footer[] = {
        { CAT_BTN_UP,   "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
        { CAT_BTN_L2,   "Tab",      false, ";" },
        { CAT_BTN_R2,   "Tab",      false, "t" },
        { CAT_BTN_MENU, "Menu",     false, "H" },
        { CAT_BTN_Y,    "Rescan",   false, "Y" },
        { CAT_BTN_B,    "Shutdown", false, "B" },
    };
    cat_draw_footer(footer, 6);

    (void)small_font;
    (void)theme;

    cat_present();
}

static void jw__rescan(const char *socket_path, const char *db_path, jw_launcher_state *state) {
    snprintf(state->status, sizeof(state->status), "%s", "rescanning...");
    cat_request_frame();
    jw__render_launcher(state);
    jw__scan_library(socket_path, db_path, state);
}

static void jw__handle_input(const char *socket_path, const char *db_path,
                              jw_launcher_state *state, cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            jw__move_cursor(state, -1);
            break;
        case CAT_BTN_DOWN:
            jw__move_cursor(state, 1);
            break;
        case CAT_BTN_LEFT:
            jw__page_cursor(state, -1);
            break;
        case CAT_BTN_RIGHT:
            jw__page_cursor(state, 1);
            break;
        case CAT_BTN_L1:
            jw__jump_letter(state, -1);
            break;
        case CAT_BTN_R1:
            jw__jump_letter(state, 1);
            break;
        case CAT_BTN_L2:
            jw__switch_tab(state, -1);
            break;
        case CAT_BTN_R2:
            jw__switch_tab(state, 1);
            break;
        case CAT_BTN_A:
            jw__activate_item(socket_path, db_path, state, running);
            break;
        case CAT_BTN_MENU:
            if (jw__send_open_menu(socket_path) == 0) {
                cat_hide_window();
                *running = false;
            } else {
                snprintf(state->status, sizeof(state->status), "%s", "open-menu failed");
            }
            break;
        case CAT_BTN_Y:
            jw__rescan(socket_path, db_path, state);
            break;
        case CAT_BTN_B:
            jw__send_shutdown(socket_path);
            *running = false;
            break;
        default:
            break;
    }
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

    SDL_RaiseWindow(cat_get_window());
    jw__platform_activate();

    jw_launcher_state state;
    memset(&state, 0, sizeof(state));
    state.current_tab = JW_TAB_GAMES;
    snprintf(state.status, sizeof(state.status), "%s", "scanning library...");

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
        cat_input_event event;
        while (cat_poll_input(&event)) {
            if (!event.pressed) continue;
            jw__handle_input(socket_path, db_path, &state, event.button, &running);
        }

        if (auto_demo && !auto_action_sent) {
            uint32_t elapsed_ms = SDL_GetTicks() - started_ms;
            if (elapsed_ms >= auto_delay_ms) {
                cat_hide_window();
                jw__send_open_menu(socket_path);
                auto_action_sent = true;
                running = false;
            } else {
                cat_request_frame_in(auto_delay_ms - elapsed_ms);
            }
        }

        jw__render_launcher(&state);
    }

    cat_quit();
    free(socket_path);
    free(db_path);
    return 0;
}
