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
#include "internal/platform/paths.h"
#include "internal/settings/settings.h"
#include "internal/settings/theme_resolve.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define JW_MAX_SYSTEMS 64
#define JW_MAX_APPS    64

/* ─── Tabbed mode ─────────────────────────────────────────────────────────── */

typedef enum {
    JW_TAB_RECENTS = 0,
    JW_TAB_GAMES,
    JW_TAB_APPS,
    JW_TAB_SETTINGS,
    JW_TAB_COUNT
} jw_tab;

static const char *kTabs[JW_TAB_COUNT] = { "Recents", "Games", "Apps", "Settings" };

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
    /* flat nav (vertical / horizontal) */
    jw_flat_item       flat_items[JW_MAX_SYSTEMS + 6];
    int                flat_count;
    /* horizontal: tools sub-menu */
    bool               tools_open;
    cat_list_state     tools_list;
    /* settings (Appearance/Library/Behavior/About) */
    jw_settings_ui     settings;
    /* status line */
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
    TTF_Font *large   = cat_get_font(CAT_FONT_EXTRA_LARGE);
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    int list_x  = margin;
    int list_w  = sw * 45 / 100;
    int body_h  = TTF_FontHeight(body);
    int item_h  = body_h + CAT_S(12);
    int list_h  = content_h - CAT_S(28);
    int art_x   = list_w + margin * 2;
    int art_y   = content_y + margin;
    int art_w   = sw - art_x - margin;
    int art_h   = content_h - margin * 2;

    cat_draw_rounded_rect(art_x, art_y, art_w, art_h, CAT_S(8),
        cat_hex_to_color("#ffffff18"));

    if (state->system_count > 0 && state->list.cursor < state->system_count) {
        const char *sys = state->systems[state->list.cursor].name;
        int large_h = TTF_FontHeight(large);
        int tw = cat_measure_text(large, sys);
        cat_draw_text_ellipsized(large, sys,
            art_x + (art_w - tw) / 2,
            art_y + (art_h - large_h) / 2,
            theme->hint, art_w - margin * 2);
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

    int status_y = sh - fh - CAT_S(26);
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
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

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

    int status_y = sh - fh - CAT_S(26);
    cat_draw_text_ellipsized(small, state->status, list_x, status_y, theme->hint, list_w);
    (void)margin;
}

static void jw__render_settings(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    int sx = margin;
    int sy = content_y + margin;
    int sw_inner = sw - margin * 2;
    int sh_inner = content_h - CAT_S(28);

    jw_settings_ui_render(&state->settings, sx, sy, sw_inner, sh_inner);

    int status_y = sh - fh - CAT_S(26);
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
            { CAT_BTN_B,  "Back",     false, "B" },
        };
        cat_draw_footer(footer, 3);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_UP,   "Navigate", false, "\xe2\x86\x91\xe2\x86\x93" },
            { CAT_BTN_L2,   "Tab",      false, ";" },
            { CAT_BTN_R2,   "Tab",      false, "t" },
            { CAT_BTN_MENU, "Menu",     false, "H" },
            { CAT_BTN_Y,    "Rescan",   false, "Y" },
            { CAT_BTN_B,    "Shutdown", false, "B" },
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
    ap_theme *theme = cat_get_theme();
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int margin = CAT_S(16);

    cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8), cat_hex_to_color("#ffffff10"));

    if (state->flat_count == 0 || state->list.cursor >= state->flat_count) return;

    const jw_flat_item *it = &state->flat_items[state->list.cursor];
    const char *label = jw__flat_label(state, state->list.cursor);

    int large_h = TTF_FontHeight(large);
    int label_w = cat_measure_text(large, label);
    cat_draw_text_ellipsized(large, label,
        px + (pw - label_w) / 2,
        py + ph / 2 - large_h,
        theme->text, pw - margin * 2);

    if (it->kind == JW_FLAT_SYSTEM) {
        char sub[64];
        snprintf(sub, sizeof(sub), "%d games",
                 state->systems[it->system_idx].game_count);
        int sw2 = cat_measure_text(small, sub);
        cat_draw_text(small, sub,
            px + (pw - sw2) / 2,
            py + ph / 2 + CAT_S(8),
            theme->hint);
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
    int status_y = sh - fh - CAT_S(26);
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
            { CAT_BTN_MENU, "Menu",     false, "H" },
            { CAT_BTN_Y,    "Rescan",   false, "Y" },
            { CAT_BTN_B,    "Shutdown", false, "B" },
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

    /* Derive a muted background color from the label */
    uint32_t h = jw__str_hash(label);
    int hue = (int)(h % 360);
    (void)hue;
    /* Use highlight blended with dark bg for variety */
    SDL_Color bg;
    {
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
    int status_y = sh - fh - CAT_S(26);
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
            { CAT_BTN_MENU,  "Menu",     false, "H" },
            { CAT_BTN_Y,     "Rescan",   false, "Y" },
            { CAT_BTN_B,     "Shutdown", false, "B" },
        };
        cat_draw_footer(footer, 5);
    }
    cat_present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DISPATCH
 * ═══════════════════════════════════════════════════════════════════════════ */

static void jw__render_launcher(jw_launcher_state *state) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    switch (ss->launcher.layout) {
        case CAT_LAUNCHER_VERTICAL:   jw__render_vertical(state);   break;
        case CAT_LAUNCHER_HORIZONTAL: jw__render_horizontal(state); break;
        default:                      jw__render_tabbed(state);     break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ACTION + INPUT
 * ═══════════════════════════════════════════════════════════════════════════ */

static void jw__activate_tabbed(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state, bool *running) {
    (void)socket_path; (void)db_path; (void)running;
    switch (state->current_tab) {
        case JW_TAB_GAMES:
        case JW_TAB_APPS:
            snprintf(state->status, sizeof(state->status), "%s", "Coming soon");
            break;
        case JW_TAB_SETTINGS:
            /* Settings tab content is owned by jw_settings_ui; A is handled there. */
            break;
        default:
            break;
    }
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
        case JW_FLAT_SYSTEM:
            snprintf(state->status, sizeof(state->status), "%s", "Coming soon");
            break;
        default:
            break;
    }
    (void)socket_path; (void)db_path; (void)running;
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

    if (layout == CAT_LAUNCHER_HORIZONTAL) {
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

static void jw__handle_input(const char *socket_path, const char *db_path,
                              jw_launcher_state *state, cat_button button, bool *running) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;

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
        bool theme_changed = false;
        bool still_open = jw_settings_ui_handle_button(
            &state->settings, button,
            state->status, sizeof(state->status), &theme_changed);
        if (theme_changed)
            jw__rebuild_for_layout(state);
        if (!still_open) {
            /* B at Settings home closed it.
               In tabbed mode, snap back to Games tab so the user isn't
               stranded on an empty Settings tab. In flat/horizontal
               modes, the launcher's main view is what they see. */
            if (layout == CAT_LAUNCHER_TABBED) {
                state->current_tab = JW_TAB_GAMES;
                cat_list_state_jump(&state->list, 0, jw__tab_list_count(state));
            }
        }
        return;
    }

    int count = (layout == CAT_LAUNCHER_TABBED) ? jw__tab_list_count(state)
                                                 : state->flat_count;

    switch (button) {
        case CAT_BTN_UP:
            if (layout == CAT_LAUNCHER_HORIZONTAL)
                break;
            cat_list_state_move(&state->list, -1, count);
            break;
        case CAT_BTN_DOWN:
            if (layout == CAT_LAUNCHER_HORIZONTAL)
                break;
            cat_list_state_move(&state->list, +1, count);
            break;
        case CAT_BTN_LEFT:
            if (layout == CAT_LAUNCHER_HORIZONTAL)
                cat_list_state_move(&state->list, -1, count);
            else
                cat_list_state_page(&state->list, -1, count);
            break;
        case CAT_BTN_RIGHT:
            if (layout == CAT_LAUNCHER_HORIZONTAL)
                cat_list_state_move(&state->list, +1, count);
            else
                cat_list_state_page(&state->list, +1, count);
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
            if (layout == CAT_LAUNCHER_HORIZONTAL)
                jw__build_carousel_list(state);
            else if (layout == CAT_LAUNCHER_VERTICAL)
                jw__build_flat_list(state);
            else
                cat_list_state_jump(&state->list, 0, jw__tab_list_count(state));
            break;
        }
        case CAT_BTN_B:
            jw_ipc_shutdown(socket_path);
            *running = false;
            break;
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
    if (!socket_path || !db_path) {
        jw_log_error("could not resolve runtime paths");
        free(socket_path);
        free(db_path);
        return 1;
    }

    if (jw_ipc_hello(socket_path, "launcher") != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?",
                     socket_path);
        free(socket_path);
        free(db_path);
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
    snprintf(state.status, sizeof(state.status), "%s", "scanning library...");

    /* Scan first so flat lists can be built */
    jw__scan_library(socket_path, db_path, &state);

    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;
    const char *layout_name = (layout == CAT_LAUNCHER_VERTICAL)   ? "vertical"
                            : (layout == CAT_LAUNCHER_HORIZONTAL) ? "horizontal"
                            : "tabbed";
    jw_log_info("launcher layout: %s (theme=%s)", layout_name, theme_name);

    /* Init settings UI with the currently-active theme */
    jw_settings_ui_init(&state.settings, db_path, theme_name);

    jw__rebuild_for_layout(&state);

    jw_autodemo demo;
    jw_autodemo_init(&demo);
    bool running = true;

    jw__render_launcher(&state);

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
    return 0;
}
