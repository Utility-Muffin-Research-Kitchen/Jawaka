#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "cJSON.h"
#include "internal/core/autodemo.h"
#include "internal/core/env.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/ipc/ipc_client.h"
#include "internal/launcher/console_colors.h"
#include "internal/platform/paths.h"
#include "internal/settings/settings.h"
#include "internal/settings/theme_resolve.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#define JW_MAX_SYSTEMS 64
#define JW_MAX_APPS    64
#define JW_MAX_GAMES   512
#define JW_MAX_SEARCH_RESULTS 128

/* ─── Tabbed mode ─────────────────────────────────────────────────────────── */

typedef enum {
    JW_TAB_RECENTS = 0,
    JW_TAB_GAMES,
    JW_TAB_APPS,
    JW_TAB_SETTINGS,
    JW_TAB_COUNT
} jw_tab;

static const char *kTabs[JW_TAB_COUNT] = { "Recents", "Games", "Apps", "Settings" };

/* Forward declarations: shared preview helper used by Tabs games-tab and the
 * Vertical preview pane. Defined alongside jw__load_system_icon below. */
static void jw__draw_system_preview(int px, int py, int pw, int ph,
                                     const char *system_code, int game_count);

/* ─── Flat nav list (vertical + horizontal modes) ─────────────────────────── */

typedef enum {
    JW_FLAT_RECENTLY_PLAYED,
    JW_FLAT_FAVORITES,
    JW_FLAT_SYSTEM,
    JW_FLAT_APPS,
    JW_FLAT_SETTINGS,
    JW_FLAT_TOOLS,   /* horizontal: catch-all tile */
} jw_flat_kind;

typedef struct {
    jw_flat_kind kind;
    int          system_idx;
} jw_flat_item;

/* ─── Coverflow animation state ───────────────────────────────────────────── */

typedef struct {
    bool      active;
    float     from_visual;   /* visual cursor position at animation start */
    int       to_cursor;     /* logical target position */
    uint32_t  start_ms;
} jw_coverflow_anim;

/* ─── Launcher state ──────────────────────────────────────────────────────── */

typedef struct {
    /* tabbed mode */
    jw_tab             current_tab;
    /* all modes */
    cat_list_state     list;
    /* data */
    jw_library_summary summary;
    jw_system_entry    systems[JW_MAX_SYSTEMS];
    int                system_count;
    jw_app_entry       apps[JW_MAX_APPS];
    int                app_count;
    jw_game_entry      games[JW_MAX_GAMES];
    int                game_count;
    char               game_system[64];
    bool               games_open;
    cat_list_state     game_list;
    jw_search_result   search_results[JW_MAX_SEARCH_RESULTS];
    int                search_count;
    char               search_query[256];
    bool               search_open;
    cat_list_state     search_list;
    /* flat nav (vertical / horizontal) */
    jw_flat_item       flat_items[JW_MAX_SYSTEMS + 6];
    int                flat_count;
    /* horizontal: tools sub-menu */
    bool               tools_open;
    cat_list_state     tools_list;
    /* coverflow animation */
    jw_coverflow_anim  coverflow_anim;
    /* curated per-console colors (Horizontal carousel; loaded from active theme) */
    jw_console_color_table console_colors;
    /* settings (Appearance/Library/Behavior/About) */
    jw_settings_ui     settings;
    /* status line */
    char               sdcard_root[PATH_MAX];
    char               status[256];
    bool               scan_ready;
} jw_launcher_state;

/* ─── Flat list helpers ───────────────────────────────────────────────────── */

static void jw__build_flat_list(jw_launcher_state *state) {
    int n = 0;
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_RECENTLY_PLAYED, 0 };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_FAVORITES, 0 };
    for (int i = 0; i < state->system_count && n < JW_MAX_SYSTEMS + 4; i++)
        state->flat_items[n++] = (jw_flat_item){ JW_FLAT_SYSTEM, i };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_APPS, 0 };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_SETTINGS, 0 };
    state->flat_count = n;
}

static void jw__build_carousel_list(jw_launcher_state *state) {
    int n = 0;
    for (int i = 0; i < state->system_count && n < JW_MAX_SYSTEMS + 4; i++)
        state->flat_items[n++] = (jw_flat_item){ JW_FLAT_SYSTEM, i };
    state->flat_items[n++] = (jw_flat_item){ JW_FLAT_TOOLS, 0 };
    state->flat_count = n;
}

static const char *jw__flat_label(const jw_launcher_state *state, int idx) {
    if (idx < 0 || idx >= state->flat_count) return "";
    const jw_flat_item *it = &state->flat_items[idx];
    switch (it->kind) {
        case JW_FLAT_RECENTLY_PLAYED: return "Recently Played";
        case JW_FLAT_FAVORITES:       return "Favorites";
        case JW_FLAT_SYSTEM:          return state->systems[it->system_idx].name;
        case JW_FLAT_APPS:            return "Apps";
        case JW_FLAT_SETTINGS:        return "Settings";
        case JW_FLAT_TOOLS:           return "Tools";
        default:                      return "";
    }
}

/* ─── Tabbed: list count ──────────────────────────────────────────────────── */

static int jw__tab_list_count(const jw_launcher_state *state) {
    switch (state->current_tab) {
        case JW_TAB_RECENTS:  return 0;
        case JW_TAB_GAMES:    return state->system_count;
        case JW_TAB_APPS:     return state->app_count;
        case JW_TAB_SETTINGS: return 0;  /* handled by jw_settings_ui */
        default:              return 0;
    }
}

/* ─── Library scan ────────────────────────────────────────────────────────── */

static int jw__scan_library(const char *socket_path, const char *db_path,
                             jw_launcher_state *state) {
    int rc = jw_ipc_scan_library(socket_path, state->status, sizeof(state->status));
    if (rc != 0) return -1;

    if (jw_db_read_summary(db_path, &state->summary) != 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "scan complete but summary load failed");
        return -1;
    }

    jw_db_list_systems(db_path, state->systems, JW_MAX_SYSTEMS, &state->system_count);
    jw_db_list_apps(db_path, state->apps, JW_MAX_APPS, &state->app_count);

    state->scan_ready = true;
    snprintf(state->status, sizeof(state->status), "%d games, %d systems, %d apps",
        state->summary.game_count, state->system_count, state->summary.app_count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TABBED RENDER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void jw__switch_tab(jw_launcher_state *state, int direction) {
    if (!state) return;
    int next = (state->current_tab + direction) % JW_TAB_COUNT;
    if (next < 0) next += JW_TAB_COUNT;
    state->current_tab = (jw_tab)next;
    cat_list_state_jump(&state->list, 0, jw__tab_list_count(state));
    /* In tabbed mode, the Settings tab is owned by jw_settings_ui:
       auto-open on entry, close when navigating away. */
    if (state->current_tab == JW_TAB_SETTINGS)
        jw_settings_ui_enter(&state->settings);
    else
        jw_settings_ui_close(&state->settings);
}

typedef struct { const jw_system_entry *systems; } jw__games_ctx;
typedef struct { const jw_app_entry   *apps;    } jw__apps_ctx;
typedef struct { const jw_game_entry  *games;   } jw__roms_ctx;
typedef struct { const jw_search_result *results; } jw__search_ctx;

static void jw__draw_game_item(int idx, int ix, int iy, int iw, int ih,
                                bool selected, void *user) {
    jw__games_ctx *ctx = (jw__games_ctx *)user;
    ap_theme *theme    = cat_get_theme();
    TTF_Font *body     = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small    = cat_get_font(CAT_FONT_SMALL);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c  = selected ? theme->highlighted_text : theme->text;
    ap_color count_c = selected ? theme->highlighted_text : theme->hint;

    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", ctx->systems[idx].game_count);
    int count_w  = cat_measure_text(small, count_str);
    int name_max = iw - count_w - CAT_S(24);
    int text_y   = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, ctx->systems[idx].name,
        ix + CAT_S(10), text_y, name_c, name_max);
    int count_x = ix + iw - count_w - CAT_S(8);
    int small_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
    cat_draw_text(small, count_str, count_x, small_y, count_c);
}

static void jw__draw_app_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    jw__apps_ctx *ctx = (jw__apps_ctx *)user;
    ap_theme *theme   = cat_get_theme();
    TTF_Font *body    = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, ctx->apps[idx].name,
        ix + CAT_S(10), text_y, name_c, iw - CAT_S(20));
}

static void jw__draw_rom_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    jw__roms_ctx *ctx = (jw__roms_ctx *)user;
    ap_theme *theme   = cat_get_theme();
    TTF_Font *body    = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small   = cat_get_font(CAT_FONT_SMALL);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    ap_color path_c = selected ? theme->highlighted_text : theme->hint;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    const char *name = ctx->games[idx].name;
    const char *path = ctx->games[idx].rom_path;
    int path_w = iw / 3;
    int name_max = iw - path_w - CAT_S(32);
    if (name_max < CAT_S(80)) {
        path_w = 0;
        name_max = iw - CAT_S(20);
    }

    cat_draw_text_ellipsized(body, name, ix + CAT_S(10), text_y, name_c, name_max);

    if (path_w > 0 && path && path[0]) {
        int small_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
        cat_draw_text_ellipsized(small, path,
            ix + iw - CAT_S(10) - path_w, small_y,
            path_c, path_w);
    }
}

static void jw__draw_search_item(int idx, int ix, int iy, int iw, int ih,
                                  bool selected, void *user) {
    jw__search_ctx *ctx = (jw__search_ctx *)user;
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body      = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small     = cat_get_font(CAT_FONT_SMALL);

    const jw_search_result *result = &ctx->results[idx];
    const char *kind = result->kind == JW_SEARCH_APP ? "App" : "Game";
    const char *meta = result->kind == JW_SEARCH_APP ? result->pak_dir : result->system;

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    ap_color meta_c = selected ? theme->highlighted_text : theme->hint;

    int kind_w = cat_measure_text(small, kind);
    int meta_w = iw / 4;
    int name_max = iw - kind_w - meta_w - CAT_S(42);
    if (name_max < CAT_S(96)) {
        meta_w = 0;
        name_max = iw - kind_w - CAT_S(32);
    }

    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, result->name,
        ix + CAT_S(10), text_y, name_c, name_max);

    int small_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
    if (meta_w > 0 && meta && meta[0]) {
        cat_draw_text_ellipsized(small, meta,
            ix + iw - kind_w - meta_w - CAT_S(24), small_y,
            meta_c, meta_w);
    }
    cat_draw_text(small, kind, ix + iw - kind_w - CAT_S(10), small_y, meta_c);
}

static void jw__render_recents(const jw_launcher_state *state,
                                int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *large_font = cat_get_font(CAT_FONT_EXTRA_LARGE);
    int sw = cat_get_screen_width();
    const char *msg = "No recent games";
    int large_h = TTF_FontHeight(large_font);
    int tw = cat_measure_text(large_font, msg);
    cat_draw_text(large_font, msg,
        (sw - tw) / 2,
        content_y + (content_h - large_h) / 2,
        theme->hint);
    (void)margin;
    (void)state;
}

static void jw__render_games(const jw_launcher_state *state,
                              int content_y, int content_h, int margin) {
    ap_theme *theme   = cat_get_theme();
    TTF_Font *body    = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small   = cat_get_font(CAT_FONT_SMALL);
    int sw = cat_get_screen_width();

    int list_x  = margin;
    int list_w  = sw * 45 / 100;
    int body_h  = TTF_FontHeight(body);
    int item_h  = body_h + CAT_S(12);
    int list_h  = content_h - CAT_S(28);
    int art_x   = list_w + margin * 2;
    int art_y   = content_y + margin;
    int art_w   = sw - art_x - margin;
    int art_h   = content_h - margin * 2;

    if (state->system_count > 0 && state->list.cursor < state->system_count) {
        const jw_system_entry *sys = &state->systems[state->list.cursor];
        jw__draw_system_preview(art_x, art_y, art_w, art_h,
                                sys->name, sys->game_count);
    } else {
        cat_draw_rounded_rect(art_x, art_y, art_w, art_h, CAT_S(8),
            cat_hex_to_color("#ffffff18"));
    }

    if (state->system_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? "No games found" : "Scanning library...",
            list_x + CAT_S(8), content_y + margin + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__games_ctx ctx = { state->systems };
        cat_draw_list_pane(list_x, content_y + margin, list_w, list_h,
            state->system_count, &state->list, item_h,
            jw__draw_game_item, &ctx);
    }

    int status_y = content_y + content_h - TTF_FontHeight(small);
    cat_draw_text_ellipsized(small, state->status, list_x, status_y, theme->hint, list_w);
    (void)margin;
}

static void jw__render_apps(const jw_launcher_state *state,
                             int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);
    int sw = cat_get_screen_width();

    int list_x = margin;
    int list_w = sw * 45 / 100;
    int body_h = TTF_FontHeight(body);
    int item_h = body_h + CAT_S(12);
    int list_h = content_h - CAT_S(28);
    int art_x  = list_w + margin * 2;
    int art_y  = content_y + margin;
    int art_w  = sw - art_x - margin;
    int art_h  = content_h - margin * 2;

    cat_draw_rounded_rect(art_x, art_y, art_w, art_h, CAT_S(8),
        cat_hex_to_color("#ffffff18"));

    if (state->app_count > 0 && state->list.cursor < state->app_count) {
        const char *name = state->apps[state->list.cursor].name;
        int large_h = TTF_FontHeight(large);
        int tw = cat_measure_text(large, name);
        cat_draw_text_ellipsized(large, name,
            art_x + (art_w - tw) / 2,
            art_y + (art_h - large_h) / 2,
            theme->hint, art_w - margin * 2);
    }

    if (state->app_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? "No apps found" : "Scanning library...",
            list_x + CAT_S(8), content_y + margin + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__apps_ctx ctx = { state->apps };
        cat_draw_list_pane(list_x, content_y + margin, list_w, list_h,
            state->app_count, &state->list, item_h,
            jw__draw_app_item, &ctx);
    }

    int status_y = content_y + content_h - TTF_FontHeight(small);
    cat_draw_text_ellipsized(small, state->status, list_x, status_y, theme->hint, list_w);
    (void)margin;
}

static void jw__render_settings(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int sw = cat_get_screen_width();

    int sx = margin;
    int sy = content_y + margin;
    int sw_inner = sw - margin * 2;
    int sh_inner = content_h - CAT_S(28);

    jw_settings_ui_render(&state->settings, sx, sy, sw_inner, sh_inner);

    int status_y = content_y + content_h - TTF_FontHeight(small);
    cat_draw_text_ellipsized(small, state->status, sx, status_y, theme->hint, sw_inner);
}

static void jw__render_tabbed(const jw_launcher_state *state) {
    cat_clear_screen();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();
    int header_h = cat_get_tab_bar_height();

    cat_draw_tab_bar(kTabs, JW_TAB_COUNT, (int)state->current_tab);

    int content_y = header_h;
    int content_h = sh - header_h - fh;
    int margin    = CAT_S(12);

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

    if (jw_settings_ui_is_open(&state->settings)) {
        cat_footer_item footer[] = {
            { CAT_BTN_UP, "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
            { CAT_BTN_A,  "Select",   false, "A" },
            { CAT_BTN_L2, "Tab",      false, ";" },
            { CAT_BTN_R2, "Tab",      false, "t" },
        };
        cat_draw_footer(footer, 4);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_UP,   "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
            { CAT_BTN_L2,   "Tab",      false, ";" },
            { CAT_BTN_R2,   "Tab",      false, "t" },
            { CAT_BTN_X,    "Search",   false, "X" },
            { CAT_BTN_MENU, "Menu",     false, "H" },
            { CAT_BTN_Y,    "Rescan",   false, "Y" },
        };
        cat_draw_footer(footer, 6);
    }
    cat_present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VERTICAL RENDER (NextUI-style)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const jw_launcher_state *state;
} jw__vert_ctx;

static void jw__draw_vert_item(int idx, int ix, int iy, int iw, int ih,
                                bool selected, void *user) {
    jw__vert_ctx *ctx = (jw__vert_ctx *)user;
    const jw_launcher_state *state = ctx->state;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    const jw_flat_item *it = &state->flat_items[idx];
    bool is_section = (it->kind == JW_FLAT_RECENTLY_PLAYED ||
                       it->kind == JW_FLAT_FAVORITES ||
                       it->kind == JW_FLAT_APPS ||
                       it->kind == JW_FLAT_SETTINGS);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text
                                : (is_section ? theme->text : theme->text);
    ap_color count_c = selected ? theme->highlighted_text : theme->hint;

    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    const char *label = jw__flat_label(state, idx);

    if (it->kind == JW_FLAT_SYSTEM) {
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d",
                 state->systems[it->system_idx].game_count);
        int count_w  = cat_measure_text(small, count_str);
        int name_max = iw - count_w - CAT_S(24);
        cat_draw_text_ellipsized(body, label, ix + CAT_S(10), text_y, label_c, name_max);
        int count_x = ix + iw - count_w - CAT_S(8);
        int small_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
        cat_draw_text(small, count_str, count_x, small_y, count_c);
    } else {
        /* section header: slightly muted when not selected */
        if (!selected)
            label_c = theme->hint;
        cat_draw_text_ellipsized(body, label, ix + CAT_S(10), text_y, label_c,
                                 iw - CAT_S(20));
    }
}

static void jw__render_vertical_preview(const jw_launcher_state *state,
                                         int px, int py, int pw, int ph) {
    if (state->flat_count == 0 || state->list.cursor >= state->flat_count) {
        cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8),
                              cat_hex_to_color("#ffffff10"));
        return;
    }

    const jw_flat_item *it = &state->flat_items[state->list.cursor];

    if (it->kind == JW_FLAT_SYSTEM) {
        const jw_system_entry *sys = &state->systems[it->system_idx];
        jw__draw_system_preview(px, py, pw, ph, sys->name, sys->game_count);
    } else {
        /* Non-system entries (Recents, Favorites, Apps, Settings): text only,
         * matches pre-icon behaviour. */
        ap_theme *theme  = cat_get_theme();
        TTF_Font *large  = cat_get_font(CAT_FONT_EXTRA_LARGE);
        const char *label = jw__flat_label(state, state->list.cursor);
        int large_h = TTF_FontHeight(large);
        int label_w = cat_measure_text(large, label);
        int margin  = CAT_S(16);
        cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8),
                              cat_hex_to_color("#ffffff10"));
        cat_draw_text_ellipsized(large, label,
                                  px + (pw - label_w) / 2,
                                  py + (ph - large_h) / 2,
                                  theme->text, pw - margin * 2);
    }
}

static void jw__render_vertical(const jw_launcher_state *state) {
    cat_clear_screen();

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();
    int sb_h = CAT_DS(20);
    int margin = CAT_S(10);

    float split = ss->launcher.list_split;
    int list_w = (int)(sw * split);
    int list_x = 0;
    int prev_x = list_w + margin;
    int prev_w = sw - prev_x;
    int content_y = sb_h + margin;
    int content_h = sh - content_y - fh - margin;

    /* Subtle divider between list and preview */
    cat_draw_rect(list_w, content_y, 1, content_h, cat_hex_to_color("#ffffff20"));

    /* Left: nav list */
    int body_h = TTF_FontHeight(body);
    int item_h = body_h + CAT_S(12);
    jw__vert_ctx ctx = { state };
    cat_draw_list_pane(list_x, content_y, list_w, content_h,
        state->flat_count, &state->list, item_h,
        jw__draw_vert_item, &ctx);

    /* Right: preview panel */
    jw__render_vertical_preview(state, prev_x, content_y, prev_w - margin, content_h);

    /* Status line at bottom-left */
    int status_y = content_y + content_h - TTF_FontHeight(small);
    cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, list_w - margin);

    /* Settings overlay (dims background + draws panel) */
    if (jw_settings_ui_is_open(&state->settings)) {
        SDL_Renderer *ren = cat_get_renderer();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_Rect full = { 0, 0, sw, sh };
        SDL_RenderFillRect(ren, &full);

        int ox = sw / 6;
        int ow = sw - ox * 2;
        int oy = content_y;
        int oh = content_h;
        cat_draw_rounded_rect(ox, oy, ow, oh, CAT_S(8), theme->background);
        jw_settings_ui_render(&state->settings,
                               ox + CAT_S(12), oy + CAT_S(8),
                               ow - CAT_S(24), oh - CAT_S(16));

        cat_footer_item footer[] = {
            { CAT_BTN_UP, "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
            { CAT_BTN_A,  "Select",   false, "A" },
            { CAT_BTN_B,  "Back",     false, "B" },
        };
        cat_draw_footer(footer, 3);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_UP,   "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
            { CAT_BTN_X,    "Search",   false, "X" },
            { CAT_BTN_MENU, "Menu",     false, "H" },
            { CAT_BTN_Y,    "Rescan",   false, "Y" },
        };
        cat_draw_footer(footer, 4);
    }
    cat_present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HORIZONTAL RENDER (kUI-style carousel)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Hash a string to a deterministic hue [0..360) for placeholder tile colors */
static uint32_t jw__str_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* Draw a filled parallelogram quad using SDL_RenderGeometry.
   quad[0]=TL, quad[1]=TR, quad[2]=BR, quad[3]=BL (clockwise). */
static void jw__fill_quad(const SDL_FPoint quad[4], SDL_Color c) {
    SDL_Renderer *ren = cat_get_renderer();
    SDL_Vertex verts[4] = {
        { { quad[0].x, quad[0].y }, c, { 0, 0 } },
        { { quad[1].x, quad[1].y }, c, { 1, 0 } },
        { { quad[2].x, quad[2].y }, c, { 1, 1 } },
        { { quad[3].x, quad[3].y }, c, { 0, 1 } },
    };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(ren, NULL, verts, 4, idx, 6);
}

/* Draw a single carousel tile centered at cx, cy. */
static void jw__draw_carousel_tile(const jw_launcher_state *state, int tile_idx,
                                    int cx, int cy, int tw, int th, int skew,
                                    uint8_t alpha) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    const jw_flat_item *it = &state->flat_items[tile_idx];
    const char *label = jw__flat_label(state, tile_idx);

    /* Background color: curated per-console palette if available, otherwise
     * fall back to a muted hash-derived color so unmapped systems still get
     * deterministic-but-distinct tiles. */
    SDL_Color bg;
    bool curated = false;
    if (it->kind == JW_FLAT_SYSTEM) {
        curated = jw_console_colors_lookup(
            &state->console_colors,
            state->systems[it->system_idx].name, &bg);
    }
    if (curated) {
        bg.a = alpha;
    } else {
        uint32_t h = jw__str_hash(label);
        SDL_Color hl = theme->highlight;
        uint8_t mix = (uint8_t)((h & 0xFF) / 3);
        bg.r = (uint8_t)((hl.r * mix + 20 * (255 - mix)) / 255);
        bg.g = (uint8_t)((hl.g * mix + 10 * (255 - mix)) / 255);
        bg.b = (uint8_t)((hl.b * mix + 30 * (255 - mix)) / 255);
        bg.a = alpha;
    }

    int tx = cx - tw / 2;
    int ty = cy - th / 2;

    /* Parallelogram: top edge shifted left by skew relative to bottom */
    SDL_FPoint quad[4] = {
        { (float)(tx + skew),      (float)ty        }, /* TL */
        { (float)(tx + tw + skew), (float)ty        }, /* TR */
        { (float)(tx + tw),        (float)(ty + th) }, /* BR */
        { (float)tx,               (float)(ty + th) }, /* BL */
    };
    jw__fill_quad(quad, bg);

    /* Highlight border for active tile */
    if (alpha == 255) {
        SDL_Color border = theme->highlight;
        border.a = 200;
        SDL_Renderer *ren = cat_get_renderer();
        SDL_SetRenderDrawColor(ren, border.r, border.g, border.b, border.a);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_FPoint border_pts[5] = {
            quad[0], quad[1], quad[2], quad[3], quad[0]
        };
        SDL_RenderDrawLinesF(ren, border_pts, 5);
    }

    /* Clip text rendering to tile bounding box */
    SDL_Rect clip = {
        (int)quad[3].x, ty,
        tw + skew, th
    };
    SDL_Renderer *ren = cat_get_renderer();
    SDL_RenderSetClipRect(ren, &clip);

    ap_color text_c;
    text_c.r = 255; text_c.g = 255; text_c.b = 255; text_c.a = alpha;

    int body_h = TTF_FontHeight(body);
    int small_h = TTF_FontHeight(small);

    /* System label centered in tile */
    int lw = cat_measure_text(body, label);
    int lx = cx - lw / 2 + skew / 2;
    int ly = cy - body_h / 2;
    if (it->kind == JW_FLAT_SYSTEM) ly -= small_h / 2 + CAT_S(4);
    cat_draw_text_ellipsized(body, label, lx, ly, text_c, tw - CAT_S(16));

    /* Game count below for system tiles */
    if (it->kind == JW_FLAT_SYSTEM) {
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%d games",
                 state->systems[it->system_idx].game_count);
        int cw = cat_measure_text(small, cnt);
        ap_color hint_c = theme->hint;
        hint_c.a = alpha;
        cat_draw_text(small, cnt, cx - cw / 2 + skew / 2,
                      ly + body_h + CAT_S(4), hint_c);
    }

    SDL_RenderSetClipRect(ren, NULL);
}

/* Tools sub-menu drawn as a centered overlay list */
static void jw__draw_tools_menu(jw_launcher_state *state) {
    static const char *kTools[] = {
        "Recently Played", "Favorites", "Apps", "Settings"
    };
    static const int kToolsCount = 4;

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();

    int body_h = TTF_FontHeight(body);
    int item_h = body_h + CAT_S(12);
    int menu_w = sw * 40 / 100;
    int menu_h = item_h * kToolsCount + CAT_S(16);
    int mx = (sw - menu_w) / 2;
    int my = (sh - menu_h) / 2;

    /* Dim background */
    SDL_Renderer *ren = cat_get_renderer();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect full = { 0, 0, sw, sh };
    SDL_RenderFillRect(ren, &full);

    cat_draw_rounded_rect(mx, my, menu_w, menu_h, CAT_S(8),
                          theme->background);

    for (int i = 0; i < kToolsCount; i++) {
        int iy = my + CAT_S(8) + i * item_h;
        bool sel = (state->tools_list.cursor == i);
        int pill_h = body_h + CAT_S(6);
        int pill_y = iy + (item_h - pill_h) / 2;
        if (sel)
            cat_draw_pill(mx + CAT_S(4), pill_y, menu_w - CAT_S(8), pill_h,
                          theme->highlight);
        ap_color tc = sel ? theme->highlighted_text : theme->text;
        int ty = pill_y + (pill_h - body_h) / 2;
        cat_draw_text_ellipsized(body, kTools[i], mx + CAT_S(12), ty, tc,
                                 menu_w - CAT_S(24));
    }
}

static void jw__render_horizontal(jw_launcher_state *state) {
    cat_clear_screen();

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    int skew = ss->launcher.carousel_skew;

    /* Carousel geometry */
    int tile_w   = CAT_S(180);
    int tile_h   = sh * 55 / 100;
    int spacing  = tile_w + CAT_S(20);
    int center_y = sh / 2 - fh / 2;
    int active   = state->list.cursor;
    int count    = state->flat_count;

    /* Draw tiles relative to active (active is centered) */
    int center_x = sw / 2;
    for (int i = 0; i < count; i++) {
        int offset = i - active;
        if (offset < -3 || offset > 3) continue;
        int cx = center_x + offset * spacing;
        uint8_t alpha;
        int w = tile_w, h = tile_h;
        if (offset == 0) {
            alpha = 255;
            w = tile_w * 12 / 10;
            h = tile_h * 12 / 10;
        } else if (abs(offset) == 1) {
            alpha = 160;
        } else {
            alpha = 80;
        }
        jw__draw_carousel_tile(state, i, cx, center_y, w, h, skew, alpha);
    }

    /* Status bar area: selected item name at top */
    int sb_h = CAT_DS(20);
    if (count > 0 && active < count) {
        TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
        const char *label = jw__flat_label(state, active);
        int lw = cat_measure_text(body, label);
        cat_draw_text(body, label, (sw - lw) / 2, sb_h / 2 - TTF_FontHeight(body) / 2,
                      theme->text);
    }

    /* Status line */
    int status_y = sh - fh - TTF_FontHeight(small);
    cat_draw_text_ellipsized(small, state->status, CAT_S(12), status_y,
                             theme->hint, sw / 2);

    /* Tools overlay */
    if (state->tools_open)
        jw__draw_tools_menu(state);

    /* Settings overlay */
    if (jw_settings_ui_is_open(&state->settings)) {
        SDL_Renderer *ren = cat_get_renderer();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_Rect full = { 0, 0, sw, sh };
        SDL_RenderFillRect(ren, &full);

        int ox = sw / 6;
        int ow = sw - ox * 2;
        int oy = sb_h + CAT_S(8);
        int oh = sh - oy - fh - CAT_S(8);
        cat_draw_rounded_rect(ox, oy, ow, oh, CAT_S(8), theme->background);
        jw_settings_ui_render(&state->settings,
                               ox + CAT_S(12), oy + CAT_S(8),
                               ow - CAT_S(24), oh - CAT_S(16));

        cat_footer_item footer[] = {
            { CAT_BTN_UP, "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
            { CAT_BTN_A,  "Select",   false, "A" },
            { CAT_BTN_B,  "Back",     false, "B" },
        };
        cat_draw_footer(footer, 3);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_LEFT,  "Navigate", false, "\xe2\x86\x90\xe2\x86\x92" },
            { CAT_BTN_A,     "Select",   false, "A" },
            { CAT_BTN_X,     "Search",   false, "X" },
            { CAT_BTN_MENU,  "Menu",     false, "H" },
            { CAT_BTN_Y,     "Rescan",   false, "Y" },
        };
        cat_draw_footer(footer, 5);
    }
    cat_present();
}

static int jw__resolve_sdcard_path(const jw_launcher_state *state, const char *path,
                                    char *out, size_t out_size) {
    if (!state || !path || !path[0] || !out || out_size == 0) {
        return -1;
    }

    int needed = 0;
    if (path[0] == '/') {
        needed = snprintf(out, out_size, "%s", path);
    } else {
        needed = snprintf(out, out_size, "%s/%s", state->sdcard_root, path);
    }

    return needed >= 0 && needed < (int)out_size ? 0 : -1;
}

static SDL_Texture *jw__load_cached_image(const char *path, int *out_w, int *out_h) {
    if (!path || !path[0]) {
        return NULL;
    }

    int tex_w = 0;
    int tex_h = 0;
    SDL_Texture *tex = cat_cache_get(path, &tex_w, &tex_h);
    if (tex) {
        if (out_w) *out_w = tex_w;
        if (out_h) *out_h = tex_h;
        return tex;
    }

    tex = cat_load_image(path);
    if (!tex) {
        return NULL;
    }

    if (SDL_QueryTexture(tex, NULL, NULL, &tex_w, &tex_h) != 0 || tex_w <= 0 || tex_h <= 0) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    cat_cache_put(path, tex, tex_w, tex_h);
    if (out_w) *out_w = tex_w;
    if (out_h) *out_h = tex_h;
    return tex;
}

static void jw__draw_image_fit(SDL_Texture *tex, int tex_w, int tex_h,
                                int x, int y, int w, int h) {
    if (!tex || tex_w <= 0 || tex_h <= 0 || w <= 0 || h <= 0) {
        return;
    }

    int draw_w = w;
    int draw_h = (tex_h * draw_w) / tex_w;
    if (draw_h > h) {
        draw_h = h;
        draw_w = (tex_w * draw_h) / tex_h;
    }

    int draw_x = x + (w - draw_w) / 2;
    int draw_y = y + (h - draw_h) / 2;
    cat_draw_image(tex, draw_x, draw_y, draw_w, draw_h);
}

/* ─── System icon loader (shared across themes) ──────────────────────────── */

/* Loader order:
 *   1. <sdcard_root>/Roms/<SYSTEM>/icon.png       (user override; skipped for codes starting with '_')
 *   2. <theme_dir>/<theme>/<icon_dir>/<SYSTEM>.png (theme-bundled override, if any)
 *   3. <themes_dir_parent>/system_icons/<SYSTEM>.png (shared baseline)
 *   4. <themes_dir_parent>/system_icons/_default.png (final fallback)
 * Returns NULL only if all four fail.
 * Pass "_tools" as system_code for the Tools tile.
 */
static SDL_Texture *jw__load_system_icon(const char *system_code,
                                         int *out_w, int *out_h) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    const char *theme_dir    = cat_get_active_theme_dir();
    const char *theme_name   = cat_get_active_theme_name();
    char path[1024];

    /* (1) user override on the sdcard */
    if (system_code[0] != '_') {
        char *sdcard_root = jw_sdcard_root();
        if (sdcard_root) {
            snprintf(path, sizeof(path), "%s/Roms/%s/icon.png",
                     sdcard_root, system_code);
            SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
            free(sdcard_root);
            if (t) return t;
        }
    }

    /* (2) theme-bundled override, if the theme ships its own system_icons/ */
    if (theme_dir[0] && theme_name[0]) {
        snprintf(path, sizeof(path), "%s/%s/%s/%s.png",
                 theme_dir, theme_name,
                 ss->launcher.coverflow_icon_dir, system_code);
        SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
        if (t) return t;
    }

    /* (3) shared baseline at <themes_dir_parent>/system_icons/<SYSTEM>.png.
     * theme_dir is e.g. "./res/themes" or "/mnt/SDCARD/Themes"; the shared
     * icons live next to it. */
    if (theme_dir[0]) {
        snprintf(path, sizeof(path), "%s/../system_icons/%s.png",
                 theme_dir, system_code);
        SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
        if (t) return t;
    }

    /* (4) shared _default.png */
    if (theme_dir[0]) {
        snprintf(path, sizeof(path), "%s/../system_icons/_default.png",
                 theme_dir);
        SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
        if (t) return t;
    }

    return NULL;
}

/* Shared preview-pane renderer: rounded backdrop + centered icon + label
 * + game-count subtitle. Used by Tabs games-tab right pane and the Vertical
 * preview pane so they stay visually consistent.
 *
 * Pass game_count < 0 to suppress the subtitle (e.g. non-system entries). */
static void jw__draw_system_preview(int px, int py, int pw, int ph,
                                     const char *system_code, int game_count) {
    ap_theme *theme   = cat_get_theme();
    TTF_Font *large   = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *small   = cat_get_font(CAT_FONT_SMALL);
    int margin        = CAT_S(16);

    cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8),
                          cat_hex_to_color("#ffffff10"));

    /* Icon: up to 40% of pane width or 192px, whichever is smaller */
    int icon_max = CAT_S(192);
    int icon_box = pw * 40 / 100;
    if (icon_box > icon_max) icon_box = icon_max;
    if (icon_box > ph / 2)   icon_box = ph / 2;

    int label_h = TTF_FontHeight(large);
    int sub_h   = TTF_FontHeight(small);
    int gap     = CAT_S(12);
    int sub_gap = CAT_S(4);

    /* Vertical stack: icon + name + (count). Center the block in the pane. */
    int block_h = icon_box + gap + label_h
                + ((game_count >= 0) ? (sub_gap + sub_h) : 0);
    int top_y   = py + (ph - block_h) / 2;

    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    if (system_code && system_code[0])
        tex = jw__load_system_icon(system_code, &tw, &th);

    if (tex) {
        jw__draw_image_fit(tex, tw, th,
                           px + (pw - icon_box) / 2, top_y,
                           icon_box, icon_box);
    } else {
        /* No icon → collapse the icon slot so text sits centered */
        top_y += icon_box / 2;
    }

    /* System name */
    int label_y = top_y + (tex ? (icon_box + gap) : 0);
    int label_w = cat_measure_text(large, system_code ? system_code : "");
    cat_draw_text_ellipsized(large, system_code ? system_code : "",
                              px + (pw - label_w) / 2, label_y,
                              theme->text, pw - margin * 2);

    /* Game count */
    if (game_count >= 0) {
        char sub[32];
        snprintf(sub, sizeof(sub), "%d games", game_count);
        int subw = cat_measure_text(small, sub);
        cat_draw_text(small, sub,
                      px + (pw - subw) / 2,
                      label_y + label_h + sub_gap,
                      theme->hint);
    }
}

/* ─── Coverflow: animation helpers ───────────────────────────────────────── */

static float jw__ease_out_cubic(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static float jw__coverflow_visual_cursor(const jw_launcher_state *state) {
    const jw_coverflow_anim *a = &state->coverflow_anim;
    if (!a->active) return (float)state->list.cursor;
    const cat_stylesheet *ss = cat_get_stylesheet();
    uint32_t elapsed = SDL_GetTicks() - a->start_ms;
    if (elapsed >= ss->launcher.coverflow_anim_ms) return (float)a->to_cursor;
    float t = (float)elapsed / (float)ss->launcher.coverflow_anim_ms;
    float eased = jw__ease_out_cubic(t);
    return a->from_visual + ((float)a->to_cursor - a->from_visual) * eased;
}

static void jw__coverflow_start_anim(jw_launcher_state *state, int new_cursor) {
    jw_coverflow_anim *a = &state->coverflow_anim;
    a->from_visual = jw__coverflow_visual_cursor(state);
    a->to_cursor   = new_cursor;
    a->start_ms    = SDL_GetTicks();
    a->active      = true;
    state->list.cursor = new_cursor;
}

/* ─── Coverflow: render ───────────────────────────────────────────────────── */

static void jw__render_coverflow(jw_launcher_state *state) {
    cat_clear_screen();

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme    = cat_get_theme();
    TTF_Font *label_font = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    int icon_c  = CAT_S(ss->launcher.coverflow_icon_size);
    int icon_s  = CAT_S(ss->launcher.coverflow_side_size);
    int spacing = CAT_S(ss->launcher.coverflow_spacing);
    int cx0     = sw / 2;
    int cy      = sh / 2 - fh / 2 - CAT_S(20);
    int count   = state->flat_count;

    /* Retire animation when finished */
    jw_coverflow_anim *a = &state->coverflow_anim;
    if (a->active && SDL_GetTicks() - a->start_ms >= ss->launcher.coverflow_anim_ms) {
        a->active = false;
    }

    float v_cursor = jw__coverflow_visual_cursor(state);

    /* Request another frame while animation is in flight */
    if (a->active) cat_request_frame();

    int lo = (int)floorf(v_cursor) - 1;
    int hi = (int)floorf(v_cursor) + 2;
    if (lo < 0)           lo = 0;
    if (hi > count - 1)   hi = count - 1;

    /* Two-pass draw: sides first so center overlaps them */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = lo; i <= hi; i++) {
            float dist  = (float)i - v_cursor;
            float adist = fabsf(dist);
            if (adist > 2.0f) continue;

            bool is_center_pass = adist < 0.5f;
            if (pass == 0 && is_center_pass)  continue;
            if (pass == 1 && !is_center_pass) continue;

            /* c = 1 at center, 0 at side */
            float c = 1.0f - fminf(adist, 1.0f);
            if (c < 0.0f) c = 0.0f;

            int size_px = (int)((1.0f - c) * (float)icon_s + c * (float)icon_c);
            uint8_t alpha = (uint8_t)((1.0f - c) * (float)ss->launcher.coverflow_side_alpha
                                      + c * 255.0f);
            int cx = cx0 + (int)(dist * (float)spacing);

            const jw_flat_item *it = &state->flat_items[i];
            const char *code;
            if (it->kind == JW_FLAT_SYSTEM)     code = state->systems[it->system_idx].name;
            else if (it->kind == JW_FLAT_TOOLS) code = "_tools";
            else                                continue;

            int tw, th;
            SDL_Texture *tex = jw__load_system_icon(code, &tw, &th);
            if (!tex) continue;

            SDL_SetTextureAlphaMod(tex, alpha);
            jw__draw_image_fit(tex, tw, th,
                               cx - size_px / 2, cy - size_px / 2,
                               size_px, size_px);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }

    /* Label + game count for the logical (target) cursor item */
    if (count > 0 && state->list.cursor < count) {
        const jw_flat_item *it = &state->flat_items[state->list.cursor];
        const char *label = jw__flat_label(state, state->list.cursor);
        int lw = cat_measure_text(label_font, label);
        int ly = cy + icon_c / 2 + CAT_S(20);
        cat_draw_text(label_font, label, (sw - lw) / 2, ly, theme->text);

        if (it->kind == JW_FLAT_SYSTEM) {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "%d games",
                     state->systems[it->system_idx].game_count);
            int cw = cat_measure_text(small_font, cnt);
            cat_draw_text(small_font, cnt, (sw - cw) / 2,
                          ly + TTF_FontHeight(label_font) + CAT_S(6),
                          theme->hint);
        }
    }

    /* Tools overlay */
    if (state->tools_open)
        jw__draw_tools_menu(state);

    /* Settings overlay */
    if (jw_settings_ui_is_open(&state->settings)) {
        SDL_Renderer *ren = cat_get_renderer();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_Rect full = { 0, 0, sw, sh };
        SDL_RenderFillRect(ren, &full);

        int sb_h = CAT_DS(20);
        int ox = sw / 6;
        int ow = sw - ox * 2;
        int oy = sb_h + CAT_S(8);
        int oh = sh - oy - fh - CAT_S(8);
        cat_draw_rounded_rect(ox, oy, ow, oh, CAT_S(8), theme->background);
        jw_settings_ui_render(&state->settings,
                               ox + CAT_S(12), oy + CAT_S(8),
                               ow - CAT_S(24), oh - CAT_S(16));

        cat_footer_item footer[] = {
            { CAT_BTN_UP, "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
            { CAT_BTN_A,  "Select",   false, "A" },
            { CAT_BTN_B,  "Back",     false, "B" },
        };
        cat_draw_footer(footer, 3);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_LEFT, "Navigate", false, "\xe2\x86\x90\xe2\x86\x92" },
            { CAT_BTN_A,    "Select",   false, "A" },
            { CAT_BTN_X,    "Search",   false, "X" },
            { CAT_BTN_MENU, "Menu",     false, "H" },
            { CAT_BTN_Y,    "Rescan",   false, "Y" },
        };
        cat_draw_footer(footer, 5);
    }
    cat_present();
}

static void jw__render_game_browser(const jw_launcher_state *state) {
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();
    int margin = CAT_S(12);
    int header_h = CAT_DS(30);
    int content_y = header_h + margin;
    int content_h = sh - content_y - fh - margin;

    char title[96];
    snprintf(title, sizeof(title), "%s Games", state->game_system);
    cat_draw_text_ellipsized(large, title, margin, CAT_S(6), theme->text, sw - margin * 2);

    int list_x = margin;
    int list_w = sw * 58 / 100;
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int detail_x = list_x + list_w + margin;
    int detail_w = sw - detail_x - margin;

    if (state->game_count == 0) {
        cat_draw_text_wrapped(body, "No games found",
            list_x + CAT_S(8), content_y + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__roms_ctx ctx = { state->games };
        cat_draw_list_pane(list_x, content_y, list_w, content_h,
            state->game_count, &state->game_list, item_h,
            jw__draw_rom_item, &ctx);
    }

    cat_draw_rounded_rect(detail_x, content_y, detail_w, content_h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->game_count > 0 && state->game_list.cursor < state->game_count) {
        const jw_game_entry *game = &state->games[state->game_list.cursor];
        bool drew_art = false;
        char image_abs[PATH_MAX];
        int image_w = 0;
        int image_h = 0;
        if (jw__resolve_sdcard_path(state, game->image_path, image_abs, sizeof(image_abs)) == 0) {
            SDL_Texture *tex = jw__load_cached_image(image_abs, &image_w, &image_h);
            if (tex) {
                int art_pad = CAT_S(16);
                int art_x = detail_x + art_pad;
                int art_y = content_y + art_pad;
                int art_w = detail_w - art_pad * 2;
                int art_h = content_h * 68 / 100;
                jw__draw_image_fit(tex, image_w, image_h, art_x, art_y, art_w, art_h);

                int text_y = art_y + art_h + CAT_S(12);
                cat_draw_text_ellipsized(large, game->name,
                    detail_x + art_pad, text_y,
                    theme->text, detail_w - art_pad * 2);
                cat_draw_text_ellipsized(small, game->rom_path,
                    detail_x + art_pad, text_y + TTF_FontHeight(large) + CAT_S(8),
                    theme->hint, detail_w - art_pad * 2);
                drew_art = true;
            }
        }

        if (!drew_art) {
            int large_h = TTF_FontHeight(large);
            int max_w   = detail_w - margin * 2;

            int name_w = cat_measure_text(large, game->name);
            if (name_w > max_w) name_w = max_w;
            cat_draw_text_ellipsized(large, game->name,
                detail_x + (detail_w - name_w) / 2,
                content_y + content_h / 2 - large_h,
                theme->text, max_w);

            int path_w = cat_measure_text(small, game->rom_path);
            if (path_w > max_w) path_w = max_w;
            cat_draw_text_ellipsized(small, game->rom_path,
                detail_x + (detail_w - path_w) / 2,
                content_y + content_h / 2 + CAT_S(8),
                theme->hint, max_w);
        }
    }

    int status_y = content_y + content_h - TTF_FontHeight(small);
    cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, sw - margin * 2);

    cat_footer_item footer[] = {
        { CAT_BTN_UP, "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
        { CAT_BTN_A,  "Launch",   false, "A" },
        { CAT_BTN_X,  "Search",   false, "X" },
        { CAT_BTN_B,  "Back",     false, "B" },
    };
    cat_draw_footer(footer, 4);
    cat_present();
}

static void jw__render_search(const jw_launcher_state *state) {
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();
    int margin = CAT_S(12);
    int header_h = CAT_DS(34);
    int content_y = header_h + margin;
    int content_h = sh - content_y - fh - margin;

    char title[320];
    snprintf(title, sizeof(title), "Search: %s", state->search_query[0] ? state->search_query : "(empty)");
    cat_draw_text_ellipsized(large, title, margin, CAT_S(6), theme->text, sw - margin * 2);

    int list_x = margin;
    int list_w = sw * 58 / 100;
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int detail_x = list_x + list_w + margin;
    int detail_w = sw - detail_x - margin;

    if (state->search_count == 0) {
        cat_draw_text_wrapped(body, "No results",
            list_x + CAT_S(8), content_y + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__search_ctx ctx = { state->search_results };
        cat_draw_list_pane(list_x, content_y, list_w, content_h,
            state->search_count, &state->search_list, item_h,
            jw__draw_search_item, &ctx);
    }

    cat_draw_rounded_rect(detail_x, content_y, detail_w, content_h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->search_count > 0 && state->search_list.cursor < state->search_count) {
        const jw_search_result *result = &state->search_results[state->search_list.cursor];
        const char *kind = result->kind == JW_SEARCH_APP ? "App" : "Game";
        const char *line_one = result->kind == JW_SEARCH_APP ? result->pak_dir : result->system;
        const char *line_two = result->kind == JW_SEARCH_APP ? "launch.sh" : result->rom_path;

        int large_h = TTF_FontHeight(large);
        int max_w   = detail_w - margin * 2;

        int kind_w = cat_measure_text(small, kind);
        if (kind_w > max_w) kind_w = max_w;
        cat_draw_text_ellipsized(small, kind,
            detail_x + (detail_w - kind_w) / 2,
            content_y + content_h / 2 - large_h - CAT_S(28),
            theme->hint, max_w);

        int name_w = cat_measure_text(large, result->name);
        if (name_w > max_w) name_w = max_w;
        cat_draw_text_ellipsized(large, result->name,
            detail_x + (detail_w - name_w) / 2,
            content_y + content_h / 2 - large_h,
            theme->text, max_w);
        cat_draw_text_ellipsized(small, line_one,
            detail_x + margin,
            content_y + content_h / 2 + CAT_S(10),
            theme->hint, detail_w - margin * 2);
        cat_draw_text_ellipsized(small, line_two,
            detail_x + margin,
            content_y + content_h / 2 + CAT_S(10) + TTF_FontHeight(small) + CAT_S(4),
            theme->hint, detail_w - margin * 2);
    }

    int status_y = content_y + content_h - TTF_FontHeight(small);
    cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, sw - margin * 2);

    cat_footer_item footer[] = {
        { CAT_BTN_UP, "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
        { CAT_BTN_A,  "Launch",   false, "A" },
        { CAT_BTN_X,  "Search",   false, "X" },
        { CAT_BTN_B,  "Back",     false, "B" },
    };
    cat_draw_footer(footer, 4);
    cat_present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DISPATCH
 * ═══════════════════════════════════════════════════════════════════════════ */

static void jw__render_launcher(jw_launcher_state *state) {
    if (state->search_open) {
        jw__render_search(state);
        return;
    }

    if (state->games_open) {
        jw__render_game_browser(state);
        return;
    }

    const cat_stylesheet *ss = cat_get_stylesheet();
    switch (ss->launcher.layout) {
        case CAT_LAUNCHER_VERTICAL:   jw__render_vertical(state);   break;
        case CAT_LAUNCHER_HORIZONTAL: jw__render_horizontal(state); break;
        case CAT_LAUNCHER_COVERFLOW:  jw__render_coverflow(state);  break;
        default:                      jw__render_tabbed(state);     break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ACTION + INPUT
 * ═══════════════════════════════════════════════════════════════════════════ */

static int jw__game_browser_visible_rows(void) {
    int fh = cat_get_footer_height();
    int content_h = cat_get_screen_height() - CAT_DS(30) - CAT_S(24) - fh;
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int visible = content_h / item_h;
    return visible > 0 ? visible : 1;
}

static int jw__search_visible_rows(void) {
    int fh = cat_get_footer_height();
    int content_h = cat_get_screen_height() - CAT_DS(34) - CAT_S(24) - fh;
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int visible = content_h / item_h;
    return visible > 0 ? visible : 1;
}

static int jw__perform_search(const char *db_path, jw_launcher_state *state,
                              const char *query) {
    snprintf(state->search_query, sizeof(state->search_query), "%s", query ? query : "");
    state->search_count = 0;

    if (jw_db_search_library(db_path, state->search_query, state->search_results,
                             JW_MAX_SEARCH_RESULTS, &state->search_count) != 0) {
        state->search_open = true;
        cat_list_state_init(&state->search_list, jw__search_visible_rows());
        snprintf(state->status, sizeof(state->status), "%s", "search failed");
        return -1;
    }

    state->search_open = true;
    cat_list_state_init(&state->search_list, jw__search_visible_rows());
    cat_list_state_jump(&state->search_list, 0, state->search_count);
    snprintf(state->status, sizeof(state->status), "%d results", state->search_count);
    return 0;
}

static void jw__open_search(const char *db_path, jw_launcher_state *state) {
    cat_keyboard_result result;
    int rc = cat_keyboard(state->search_query, "Start: Search | Y: Cancel",
                          CAT_KB_GENERAL, &result);
    if (rc == CAT_OK) {
        jw__perform_search(db_path, state, result.text);
    } else if (rc == CAT_ERROR) {
        snprintf(state->status, sizeof(state->status), "%s", "search keyboard failed");
    }
}

static int jw__open_system_games(const char *db_path, const char *system,
                                 jw_launcher_state *state) {
    if (jw_db_list_games_for_system(db_path, system, state->games, JW_MAX_GAMES,
                                    &state->game_count) != 0 ||
        state->game_count == 0) {
        snprintf(state->status, sizeof(state->status), "No launchable games for %s", system);
        return -1;
    }

    snprintf(state->game_system, sizeof(state->game_system), "%s", system);
    state->games_open = true;
    cat_list_state_init(&state->game_list, jw__game_browser_visible_rows());
    cat_list_state_jump(&state->game_list, 0, state->game_count);
    snprintf(state->status, sizeof(state->status), "%d %s games",
             state->game_count, system);
    return 0;
}

static int jw__launch_app_request(const char *socket_path, const char *name,
                                  const char *pak_dir, jw_launcher_state *state,
                                  bool *running) {
    if (!pak_dir || !pak_dir[0]) {
        snprintf(state->status, sizeof(state->status), "%s", "No app selected");
        return -1;
    }

    snprintf(state->status, sizeof(state->status), "Launching %s...", name && name[0] ? name : "app");
    cat_request_frame();
    jw__render_launcher(state);

    if (jw_ipc_launch_app(socket_path, pak_dir, state->status, sizeof(state->status)) != 0) {
        return -1;
    }

    cat_hide_window();
    *running = false;
    return 0;
}

static int jw__launch_selected_game(const char *socket_path, jw_launcher_state *state,
                                    bool *running) {
    if (state->game_count <= 0 || state->game_list.cursor >= state->game_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No game selected");
        return -1;
    }

    const jw_game_entry *game = &state->games[state->game_list.cursor];
    snprintf(state->status, sizeof(state->status), "Launching %s...", game->name);
    cat_request_frame();
    jw__render_launcher(state);

    if (jw_ipc_launch_game(socket_path, game->system, game->rom_path,
                           state->status, sizeof(state->status)) != 0) {
        return -1;
    }

    cat_hide_window();
    *running = false;
    return 0;
}

static int jw__launch_selected_app(const char *socket_path, jw_launcher_state *state,
                                   bool *running) {
    if (state->app_count <= 0 || state->list.cursor >= state->app_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No app selected");
        return -1;
    }

    const jw_app_entry *app = &state->apps[state->list.cursor];
    return jw__launch_app_request(socket_path, app->name, app->pak_dir, state, running);
}

static int jw__launch_selected_search_result(const char *socket_path,
                                             jw_launcher_state *state,
                                             bool *running) {
    if (state->search_count <= 0 || state->search_list.cursor >= state->search_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No result selected");
        return -1;
    }

    const jw_search_result *result = &state->search_results[state->search_list.cursor];
    if (result->kind == JW_SEARCH_APP) {
        return jw__launch_app_request(socket_path, result->name, result->pak_dir, state, running);
    }

    snprintf(state->status, sizeof(state->status), "Launching %s...", result->name);
    cat_request_frame();
    jw__render_launcher(state);

    if (jw_ipc_launch_game(socket_path, result->system, result->rom_path,
                           state->status, sizeof(state->status)) != 0) {
        return -1;
    }

    cat_hide_window();
    *running = false;
    return 0;
}

static void jw__activate_tabbed(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state, bool *running) {
    switch (state->current_tab) {
        case JW_TAB_GAMES:
            if (state->system_count > 0 && state->list.cursor < state->system_count) {
                jw__open_system_games(db_path, state->systems[state->list.cursor].name, state);
            }
            break;
        case JW_TAB_APPS:
            jw__launch_selected_app(socket_path, state, running);
            break;
        case JW_TAB_SETTINGS:
            /* Settings tab content is owned by jw_settings_ui; A is handled there. */
            break;
        default:
            break;
    }
    (void)socket_path;
    (void)running;
}

static void jw__activate_flat(const char *socket_path, const char *db_path,
                               jw_launcher_state *state, bool *running) {
    if (state->list.cursor >= state->flat_count) return;
    const jw_flat_item *it = &state->flat_items[state->list.cursor];
    switch (it->kind) {
        case JW_FLAT_SETTINGS:
            jw_settings_ui_enter(&state->settings);
            break;
        case JW_FLAT_TOOLS:
            state->tools_open = true;
            cat_list_state_init(&state->tools_list, 4);
            break;
        case JW_FLAT_RECENTLY_PLAYED:
        case JW_FLAT_FAVORITES:
        case JW_FLAT_APPS:
            snprintf(state->status, sizeof(state->status), "%s", "Coming soon");
            break;
        case JW_FLAT_SYSTEM:
            jw__open_system_games(db_path, state->systems[it->system_idx].name, state);
            break;
        default:
            break;
    }
    (void)socket_path;
    (void)running;
}

static const char *jw__system_label_cb(int idx, void *user) {
    const jw_system_entry *systems = (const jw_system_entry *)user;
    return systems[idx].name;
}

/* Rebuild layout-dependent state. Call after the active stylesheet's
 * launcher.layout may have changed (theme switch) or at first startup. */
static void jw__rebuild_for_layout(jw_launcher_state *state) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;

    state->tools_open = false;
    memset(&state->coverflow_anim, 0, sizeof(state->coverflow_anim));

    /* Refresh per-console color palette from the active theme stylesheet.
     * Empty / missing maps degrade to hash-derived colors in the carousel. */
    jw_console_colors_load(&state->console_colors,
                           cat_get_active_theme_dir(),
                           cat_get_active_theme_name());

    if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW) {
        jw__build_carousel_list(state);
    } else if (layout == CAT_LAUNCHER_VERTICAL) {
        jw__build_flat_list(state);
    } else {
        state->flat_count = 0;
    }

    int fh        = cat_get_footer_height();
    int sb_h      = CAT_DS(20);
    int margin    = CAT_S(10);
    int content_h = cat_get_screen_height() - sb_h - margin - fh - margin;
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h    = TTF_FontHeight(body) + CAT_S(12);
    int visible   = content_h / item_h;
    if (visible < 1) visible = 1;

    int count = (layout == CAT_LAUNCHER_TABBED) ? jw__tab_list_count(state)
                                                : state->flat_count;
    cat_list_state_init(&state->list, visible);
    cat_list_state_jump(&state->list, 0, count);
}

static void jw__handle_search_input(const char *socket_path, const char *db_path,
                                    jw_launcher_state *state,
                                    cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->search_list, -1, state->search_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->search_list, +1, state->search_count);
            break;
        case CAT_BTN_LEFT:
            cat_list_state_page(&state->search_list, -1, state->search_count);
            break;
        case CAT_BTN_RIGHT:
            cat_list_state_page(&state->search_list, +1, state->search_count);
            break;
        case CAT_BTN_A:
            jw__launch_selected_search_result(socket_path, state, running);
            break;
        case CAT_BTN_B:
            state->search_open = false;
            state->status[0] = '\0';
            break;
        case CAT_BTN_X:
            jw__open_search(db_path, state);
            break;
        default:
            break;
    }
}

static void jw__handle_game_browser_input(const char *socket_path,
                                          jw_launcher_state *state,
                                          cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->game_list, -1, state->game_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->game_list, +1, state->game_count);
            break;
        case CAT_BTN_LEFT:
            cat_list_state_page(&state->game_list, -1, state->game_count);
            break;
        case CAT_BTN_RIGHT:
            cat_list_state_page(&state->game_list, +1, state->game_count);
            break;
        case CAT_BTN_A:
            jw__launch_selected_game(socket_path, state, running);
            break;
        case CAT_BTN_B:
            state->games_open = false;
            state->game_count = 0;
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

static void jw__handle_input(const char *socket_path, const char *db_path,
                              jw_launcher_state *state, cat_button button, bool *running) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;

    /* Desktop-only: Q exercises the IPC shutdown path. Catastrophe only
       emits CAT_BTN_QUIT off-device, so this is inert on real hardware. */
    if (button == CAT_BTN_QUIT) {
        jw_ipc_shutdown(socket_path);
        *running = false;
        return;
    }

    if (state->search_open) {
        jw__handle_search_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->games_open) {
        if (button == CAT_BTN_X) {
            jw__open_search(db_path, state);
            return;
        }
        jw__handle_game_browser_input(socket_path, state, button, running);
        (void)db_path;
        return;
    }

    /* Tools overlay captures all input first.
       Tools entries: 0=Recently Played, 1=Favorites, 2=Apps, 3=Settings */
    if (state->tools_open) {
        static const int kToolsCount = 4;
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&state->tools_list, -1, kToolsCount);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&state->tools_list, +1, kToolsCount);
                break;
            case CAT_BTN_A:
                state->tools_open = false;
                if (state->tools_list.cursor == 3) {
                    jw_settings_ui_enter(&state->settings);
                } else {
                    snprintf(state->status, sizeof(state->status), "%s", "Coming soon");
                }
                break;
            case CAT_BTN_B:
            case CAT_BTN_MENU:
                state->tools_open = false;
                break;
            default:
                break;
        }
        return;
    }

    /* Settings UI captures input when open. */
    if (jw_settings_ui_is_open(&state->settings)) {
        /* Tabbed mode: Settings is a tab, not an app. Triggers must escape
           it cleanly from any sub-screen, and B at Settings home is a no-op
           (the user leaves via L2/R2). jw__switch_tab closes Settings as a
           side effect when moving off the tab. */
        if (layout == CAT_LAUNCHER_TABBED) {
            if (button == CAT_BTN_L2) {
                jw__switch_tab(state, -1);
                return;
            }
            if (button == CAT_BTN_R2) {
                jw__switch_tab(state, +1);
                return;
            }
        }
        bool theme_changed = false;
        bool still_open = jw_settings_ui_handle_button(
            &state->settings, button,
            state->status, sizeof(state->status), &theme_changed);
        if (theme_changed)
            jw__rebuild_for_layout(state);
        if (!still_open && layout == CAT_LAUNCHER_TABBED) {
            /* B at Settings home in tabbed mode is a no-op: re-open so the
               user stays in Settings until they use the triggers to leave. */
            jw_settings_ui_enter(&state->settings);
        }
        return;
    }

    if (button == CAT_BTN_X) {
        jw__open_search(db_path, state);
        return;
    }

    int count = (layout == CAT_LAUNCHER_TABBED) ? jw__tab_list_count(state)
                                                 : state->flat_count;

    switch (button) {
        case CAT_BTN_UP:
            if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW)
                break;
            cat_list_state_move(&state->list, -1, count);
            break;
        case CAT_BTN_DOWN:
            if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW)
                break;
            cat_list_state_move(&state->list, +1, count);
            break;
        case CAT_BTN_LEFT:
            if (layout == CAT_LAUNCHER_COVERFLOW) {
                int nc = state->list.cursor > 0 ? state->list.cursor - 1 : 0;
                if (nc != state->list.cursor) jw__coverflow_start_anim(state, nc);
            } else if (layout == CAT_LAUNCHER_HORIZONTAL) {
                cat_list_state_move(&state->list, -1, count);
            } else {
                cat_list_state_page(&state->list, -1, count);
            }
            break;
        case CAT_BTN_RIGHT:
            if (layout == CAT_LAUNCHER_COVERFLOW) {
                int nc = state->list.cursor < count - 1 ? state->list.cursor + 1 : count - 1;
                if (nc != state->list.cursor) jw__coverflow_start_anim(state, nc);
            } else if (layout == CAT_LAUNCHER_HORIZONTAL) {
                cat_list_state_move(&state->list, +1, count);
            } else {
                cat_list_state_page(&state->list, +1, count);
            }
            break;
        case CAT_BTN_L1:
            if (layout == CAT_LAUNCHER_TABBED && state->current_tab == JW_TAB_GAMES)
                cat_list_state_jump_letter(&state->list, jw__system_label_cb,
                                           state->systems, state->system_count, -1);
            break;
        case CAT_BTN_R1:
            if (layout == CAT_LAUNCHER_TABBED && state->current_tab == JW_TAB_GAMES)
                cat_list_state_jump_letter(&state->list, jw__system_label_cb,
                                           state->systems, state->system_count, +1);
            break;
        case CAT_BTN_L2:
            if (layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab(state, -1);
            break;
        case CAT_BTN_R2:
            if (layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab(state, +1);
            break;
        case CAT_BTN_A:
            if (layout == CAT_LAUNCHER_TABBED)
                jw__activate_tabbed(socket_path, db_path, state, running);
            else
                jw__activate_flat(socket_path, db_path, state, running);
            break;
        case CAT_BTN_MENU:
            if (jw_ipc_open_menu(socket_path) == 0) {
                cat_hide_window();
                *running = false;
            } else {
                snprintf(state->status, sizeof(state->status), "%s", "open-menu failed");
            }
            break;
        case CAT_BTN_Y: {
            snprintf(state->status, sizeof(state->status), "%s", "rescanning...");
            cat_request_frame();
            jw__render_launcher(state);
            jw__scan_library(socket_path, db_path, state);
            if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW)
                jw__build_carousel_list(state);
            else if (layout == CAT_LAUNCHER_VERTICAL)
                jw__build_flat_list(state);
            else
                cat_list_state_jump(&state->list, 0, jw__tab_list_count(state));
            break;
        }
        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    char *socket_path = jw_socket_path();
    char *db_path     = jw_db_path();
    char *sdcard_root = jw_sdcard_root();
    if (!socket_path || !db_path || !sdcard_root) {
        jw_log_error("could not resolve runtime paths");
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }

    if (jw_ipc_hello(socket_path, "launcher") != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?",
                     socket_path);
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }

    cat_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.window_title       = "Jawaka Launcher";
    cfg.disable_background = true;

    if (cat_init(&cfg) != CAT_OK) {
        jw_log_error("catastrophe init failed: %s", cat_get_error());
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }

    /* Resolve theme: env > DB > default Jawaka-Tabs */
    char theme_name_buf[256];
    jw_resolve_theme_name(db_path, theme_name_buf, sizeof(theme_name_buf));
    const char *theme_name = theme_name_buf;
    {
        cat_stylesheet ss;
        if (cat_stylesheet_load_theme(&ss, theme_name) == CAT_OK)
            cat_stylesheet_apply(&ss);
        else
            jw_log_error("theme '%s' not found, using defaults", theme_name);
    }

    cat_activate_window();

    jw_launcher_state state;
    memset(&state, 0, sizeof(state));
    state.current_tab = JW_TAB_GAMES;
    snprintf(state.sdcard_root, sizeof(state.sdcard_root), "%s", sdcard_root);
    snprintf(state.status, sizeof(state.status), "%s", "scanning library...");

    /* Scan first so flat lists can be built */
    jw__scan_library(socket_path, db_path, &state);

    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;
    const char *layout_name = (layout == CAT_LAUNCHER_VERTICAL)   ? "vertical"
                            : (layout == CAT_LAUNCHER_HORIZONTAL) ? "horizontal"
                            : (layout == CAT_LAUNCHER_COVERFLOW)  ? "coverflow"
                            : "tabbed";
    jw_log_info("launcher layout: %s (theme=%s)", layout_name, theme_name);

    /* Init settings UI with the currently-active theme */
    jw_settings_ui_init(&state.settings, db_path, theme_name);

    jw__rebuild_for_layout(&state);

    jw_autodemo demo;
    jw_autodemo_init(&demo);
    bool running = true;

    jw__render_launcher(&state);

    /* First frame is on screen; jawakad owns any platform-specific readiness
     * side effects such as dismissing the MLP1 stock boot transition. */
    if (jw_ipc_frontend_ready(socket_path, "launcher") != 0) {
        jw_log_warn("frontend-ready notification failed");
    }

    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            jw__handle_input(socket_path, db_path, &state, ev.button, &running);
        }

        if (demo.enabled && !demo.fired) {
            uint32_t rem = jw_autodemo_remaining_ms(&demo);
            if (jw_autodemo_should_fire(&demo)) {
                cat_hide_window();
                jw_ipc_open_menu(socket_path);
                running = false;
            } else {
                cat_request_frame_in(rem);
            }
        }

        jw__render_launcher(&state);
    }

    cat_quit();
    free(socket_path);
    free(db_path);
    free(sdcard_root);
    return 0;
}
