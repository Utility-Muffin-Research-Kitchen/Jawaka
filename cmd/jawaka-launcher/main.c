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
#include "internal/launcher/game_switcher.h"
#include "internal/platform/cat_services.h"
#include "internal/platform/device.h"
#include "internal/platform/paths.h"
#include "internal/platform/wifi.h"
#include "internal/retroarch/catalog.h"
#include "internal/settings/settings.h"
#include "internal/settings/theme_resolve.h"

#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

#define JW_MAX_SYSTEMS 64
#define JW_MAX_APPS    64
#define JW_MAX_GAMES   512
#define JW_MAX_FAVORITES 256   /* newest-first; a heavier list is truncated */
#define JW_MAX_RECENTS 64      /* most-recently-played first */
#define JW_MAX_SEARCH_RESULTS 128
#define JW_MAX_ACTION_ROWS 10
#define JW_MAX_CORE_CHOICES 24

#define JW_CONTENT_SETTING_CORE_ID "core_id"
#define JW_CONTENT_SETTING_PERFORMANCE_PROFILE "performance_profile"
#define JW_CONTENT_SETTING_DISPLAY_NAME "display_name"

static long long jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/* Button hint text: on device, return NULL so Catastrophe uses the canonical
 * button name (e.g. "L2", "MENU"). On desktop, show the keyboard shortcut.
 * Runtime check via cat_is_device() — no compile-flag branching. */
static inline const char *jw_hint(const char *desktop_key) {
    return CAT_PLATFORM_IS_DEVICE ? NULL : desktop_key;
}
static inline const char *jw_hint_device(const char *desktop_key, const char *device_key) {
    return CAT_PLATFORM_IS_DEVICE ? device_key : desktop_key;
}
#define JW_HINT(dk)            jw_hint(dk)
#define JW_HINT_DEVICE(dk, vk) jw_hint_device(dk, vk)

/* ─── Status bar — see jw__draw_status_bar after jw_launcher_state ──────── */

/* ─── Tabbed mode ─────────────────────────────────────────────────────────── */

typedef enum {
    JW_TAB_RECENTS = 0,
    JW_TAB_FAVORITES,
    JW_TAB_GAMES,
    JW_TAB_APPS,
    JW_TAB_SETTINGS,
    JW_TAB_COUNT
} jw_tab;

static const char *kTabs[JW_TAB_COUNT] = { "Recents", "Favorites", "Games", "Apps", "Settings" };

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

typedef enum {
    JW_ACTION_NONE = 0,
    JW_ACTION_GAME,
    JW_ACTION_SYSTEM
} jw_action_scope;

/* Action rows offer only what the underlying views can't already do directly:
   launching, favoriting, and reaching system defaults all have first-class
   buttons elsewhere, so they don't reappear here. */
typedef enum {
    JW_ACTION_ROW_SEARCH = 0,
    JW_ACTION_ROW_DISPLAY_NAME,
    JW_ACTION_ROW_CORE,
    JW_ACTION_ROW_PERFORMANCE,
    JW_ACTION_ROW_SCRAPE,        /* game: replace art; system: missing only */
    JW_ACTION_ROW_SCRAPE_ALL,    /* system: re-scrape everything */
    JW_ACTION_ROW_SCRAPE_CANCEL, /* swap-in while the target is queued */
    JW_ACTION_ROW_RESET
} jw_action_row_kind;

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
    char               game_system[64];          /* system id (for queries) */
    char               game_system_display[64];  /* full name for the browser title */
    bool               games_open;
    bool               games_are_favorites;  /* browser is showing the Favorites list */
    cat_list_state     game_list;
    /* Favorites tab (tabbed layout): favorited games, reloaded on tab entry */
    jw_game_entry      favorites[JW_MAX_FAVORITES];
    int                favorites_count;
    /* Recents tab: most-recently-played games, reloaded on load + tab entry */
    jw_game_entry      recents[JW_MAX_RECENTS];
    int                recents_count;
    bool               apps_open;
    cat_list_state     app_list;
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
    /* Game switcher: a dedicated recents/resume carousel opened with Select.
       Overlays the current layout; closing restores the prior view as-is. */
    bool               switcher_open;
    jw_game_switcher   switcher;
    /* Contextual system/game actions opened with X. */
    bool               actions_open;
    jw_action_scope    action_scope;
    cat_list_state     action_list;
    jw_action_row_kind action_rows[JW_MAX_ACTION_ROWS];
    int                action_row_count;
    jw_game_entry      action_game;
    char               action_system[64];
    char               action_system_display[64];
    jw_ra_core_choice  action_core_choices[JW_MAX_CORE_CHOICES];
    size_t             action_core_count;
    char               action_core_effective[64];
    char               action_core_game_override[64];
    char               action_core_system_override[64];
    char               action_perf_game_override[64];
    char               action_perf_system_override[64];
    char               action_system_display_override[64];
    bool               action_scrape_pending;   /* target has queued scrape work */
    /* settings (Appearance/Library/Behavior/About) */
    jw_settings_ui     settings;
    /* status line */
    char               sdcard_root[PATH_MAX];
    char               status[256];
    bool               scan_ready;
    int                library_generation;
} jw_launcher_state;

static void jw__draw_app_detail(const jw_launcher_state *state,
                                const jw_app_entry *app,
                                int detail_x, int detail_y,
                                int detail_w, int detail_h);

/* Defined after the image helpers; used by the tabbed renderer above them. */
static void jw__render_favorites(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin);
/* Shared list+art renderer for the Favorites and Recents tabs (uses the image
   helpers, so it's defined late and forward-declared here). */
static void jw__render_game_list_pane(const jw_launcher_state *state,
                                      const jw_game_entry *entries, int count,
                                      int content_y, int content_h, int margin,
                                      const char *empty_msg);

/* Strips trailing region/dump tags — " (USA)", " (E)", " [!]", etc. — from a
   ROM name for display only. The stored name (derived from the filename) is
   left intact so box-art matching and search keep working on the full name. */
static void jw__clean_rom_name(const char *raw, char *out, size_t out_size) {
    if (out_size == 0) return;
    snprintf(out, out_size, "%s", raw ? raw : "");
    int len = (int)strlen(out);
    for (;;) {
        while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) len--;
        if (len == 0) break;
        char close = out[len - 1];
        char open = (close == ')') ? '(' : (close == ']') ? '[' : '\0';
        if (open == '\0') break;              /* no trailing tag group */
        int i = len - 2;
        while (i >= 0 && out[i] != open) i--;
        if (i < 0) break;                     /* unbalanced — leave as-is */
        len = i;                              /* cut at the opening bracket */
    }
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) len--;
    if (len <= 0) { snprintf(out, out_size, "%s", raw ? raw : ""); return; }
    out[len] = '\0';
}

/* Draws a list-row name: the highlighted row scrolls its full name (looping
   marquee) while every other row ellipsizes. Only one row is highlighted at a
   time, so a single shared marquee state suffices; it resets whenever the
   highlighted text changes (cursor move or switching lists). */
static cat_marquee jw__row_marquee;
static char        jw__row_marquee_text[256];
static uint32_t    jw__row_marquee_ms;

static void jw__draw_row_name(TTF_Font *font, const char *text, int x, int y,
                              ap_color color, int max_w, bool selected) {
    if (!selected) {
        cat_draw_text_ellipsized(font, text, x, y, color, max_w);
        return;
    }
    uint32_t now = SDL_GetTicks();
    if (strcmp(text, jw__row_marquee_text) != 0) {
        jw__row_marquee.elapsed_ms = 0;
        snprintf(jw__row_marquee_text, sizeof(jw__row_marquee_text), "%s", text);
        jw__row_marquee_ms = now;
    }
    uint32_t dt = (jw__row_marquee_ms == 0) ? 0u : (now - jw__row_marquee_ms);
    jw__row_marquee_ms = now;
    if (cat_draw_text_marquee(font, text, x, y, color, max_w, &jw__row_marquee, dt))
        cat_request_frame();
}

/* Full console names for the system folder codes stored in the library. The
   metadata catalog still carries terse source labels for some systems, so these
   user-facing names stay here until metadata names are curated. Unknown ids
   fall back to the id itself. */
static const struct { const char *id; const char *name; } kSystemDisplayNames[] = {
    { "FC",      "Nintendo Entertainment System" },
    { "NES",     "Nintendo Entertainment System" },
    { "FDS",     "Famicom Disk System" },
    { "SFC",     "Super Nintendo" },
    { "SNES",    "Super Nintendo" },
    { "SFC_JP",  "Super Famicom" },
    { "N64",     "Nintendo 64" },
    { "GB",      "Game Boy" },
    { "GBC",     "Game Boy Color" },
    { "GBA",     "Game Boy Advance" },
    { "NDS",     "Nintendo DS" },
    { "VB",      "Virtual Boy" },
    { "MD",      "Sega Genesis" },
    { "GENESIS", "Sega Genesis" },
    { "MS",      "Sega Master System" },
    { "GG",      "Game Gear" },
    { "SEGACD",  "Sega CD" },
    { "32X",     "Sega 32X" },
    { "SATURN",  "Sega Saturn" },
    { "DC",      "Dreamcast" },
    { "SG1000",  "SG-1000" },
    { "PCE",     "TurboGrafx-16" },
    { "TG16",    "TurboGrafx-16" },
    { "PCECD",   "TurboGrafx-CD" },
    { "NEOGEO",  "Neo Geo" },
    { "NGP",     "Neo Geo Pocket" },
    { "NGPC",    "Neo Geo Pocket Color" },
    { "WS",      "WonderSwan" },
    { "WSC",     "WonderSwan Color" },
    { "PS",      "PlayStation" },
    { "PSX",     "PlayStation" },
    { "PSP",     "PlayStation Portable" },
    { "ATARI",     "Atari 2600" },
    { "ATARI2600", "Atari 2600" },
    { "A2600",   "Atari 2600" },
    { "A5200",   "Atari 5200" },
    { "A7800",   "Atari 7800" },
    { "SEVENTYEIGHTHUNDRED", "Atari 7800" },
    { "LYNX",    "Atari Lynx" },
    { "JAGUAR",  "Atari Jaguar" },
    { "COLECO",  "ColecoVision" },
    { "INTV",    "Intellivision" },
    { "VECTREX", "Vectrex" },
    { "C64",     "Commodore 64" },
    { "AMIGA",   "Amiga" },
    { "DOS",     "MS-DOS" },
    { "MSX",     "MSX" },
    { "ARCADE",  "Arcade" },
    { "MAME",    "Arcade" },
    { "FBNEO",   "Arcade" },
    { "PORTS",   "Ports" },
};

/* Resolves a system id (folder code, e.g. "FC") to its full display name
   (e.g. "Nintendo Entertainment System"). Falls back to the id when unknown. */
static void jw__system_display_name(const char *db_path, const char *id,
                                    char *out, size_t out_size) {
    if (out_size == 0) return;
    snprintf(out, out_size, "%s", id ? id : "");
    if (!id || !id[0]) return;

    char override[64];
    if (db_path &&
        jw_db_get_system_setting(db_path, id, JW_CONTENT_SETTING_DISPLAY_NAME,
                                 override, sizeof(override)) == 0 &&
        override[0]) {
        snprintf(out, out_size, "%s", override);
        return;
    }

    for (size_t i = 0; i < sizeof(kSystemDisplayNames) / sizeof(kSystemDisplayNames[0]); i++) {
        if (strcasecmp(id, kSystemDisplayNames[i].id) == 0) {
            snprintf(out, out_size, "%s", kSystemDisplayNames[i].name);
            return;
        }
    }
}

/* Sort the systems list alphabetically by display name (the DB returns them
   ordered by folder-code id, e.g. FC/MD/SFC, which isn't the user-facing order). */
static int jw__system_cmp_display(const void *a, const void *b) {
    const jw_system_entry *sa = (const jw_system_entry *)a;
    const jw_system_entry *sb = (const jw_system_entry *)b;
    return strcasecmp(sa->display_name, sb->display_name);
}

/* Fills each listed system's display_name from the catalog after a scan/load,
   then sorts the list alphabetically by that display name. */
static void jw__resolve_system_names(const char *db_path, jw_launcher_state *state) {
    for (int i = 0; i < state->system_count; i++) {
        jw__system_display_name(db_path, state->systems[i].name,
                                state->systems[i].display_name,
                                sizeof(state->systems[i].display_name));
    }
    if (state->system_count > 1) {
        qsort(state->systems, (size_t)state->system_count,
              sizeof(state->systems[0]), jw__system_cmp_display);
    }
}

static int jw__system_index_by_id(const jw_launcher_state *state,
                                  const char *system) {
    if (!state || !system || !system[0]) {
        return -1;
    }
    for (int i = 0; i < state->system_count; i++) {
        if (strcmp(state->systems[i].name, system) == 0) {
            return i;
        }
    }
    return -1;
}

static int jw__flat_cursor_for_system(const jw_launcher_state *state,
                                      const char *system) {
    if (!state || !system || !system[0]) {
        return -1;
    }
    for (int i = 0; i < state->flat_count; i++) {
        const jw_flat_item *it = &state->flat_items[i];
        if (it->kind == JW_FLAT_SYSTEM &&
            it->system_idx >= 0 && it->system_idx < state->system_count &&
            strcmp(state->systems[it->system_idx].name, system) == 0) {
            return i;
        }
    }
    return -1;
}

static void jw__draw_status_bar(const jw_launcher_state *state) {
    cat_status_bar_opts opts = {0};
    jw_settings_status_bar_opts(&state->settings, &opts);
    cat_draw_status_bar(&opts);
}

/* Draws the shared header used by the home tabs, the game browser, and search:
   the section tab bar (current section highlighted) with the status icons inline
   on the right. Returns the tab bar height so callers can place a sub-header /
   content beneath it. */
static int jw__draw_tab_header(const jw_launcher_state *state) {
    int bar_h  = cat_get_tab_bar_height();
    int pill_h = CAT_DS(CAT__PILL_SIZE);
    cat_status_bar_opts sb = {0};
    jw_settings_status_bar_opts(&state->settings, &sb);
    sb.no_pill    = true;
    sb.use_y      = true;
    sb.y_position = (bar_h - pill_h) / 2;
    cat_set_tab_bar_reserved_right(cat_get_status_bar_width(&sb) + CAT_S(12));
    cat_draw_tab_bar(kTabs, JW_TAB_COUNT, (int)state->current_tab);
    cat_draw_status_bar(&sb);
    return bar_h;
}

static int jw__footer_height(const jw_launcher_state *state) {
    return jw_settings_show_hints(&state->settings) ? cat_get_footer_height() : 0;
}

/* ─── Browse-page box model ──────────────────────────────────────────────────
 * The single layout used by every list-left / image-right page (Recents,
 * Favorites, Games, the in-system game browser, search). Built on Catastrophe's
 * cat_box (see Catastrophe plans/BOX_MODEL.md): carve the header off the top and
 * the hint bar off the bottom, inset the remaining content box by one base pad
 * on every side, and split it into a list column and an image column sharing one
 * gutter. The list and image come out as the SAME box height, so their gaps to
 * the chrome match by construction — on every page, at any font size.
 *
 * header_h is the page's full header height (tab bar; plus the sub-header where
 * present). All heights are queried live, so the layout tracks font size and the
 * on/off hint bar automatically. */
#define JW_BROWSE_PAD 12   /* base pad, unscaled (see padding rule in BOX_MODEL.md) */

static int jw__browse_base_item_h(void) {
    return TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + CAT_S(12);
}

static void jw__browse_boxes(const jw_launcher_state *state, int header_h,
                             int item_count, const cat_list_state *ls,
                             SDL_Rect *list, SDL_Rect *image, int *item_h) {
    int pad     = CAT_S(JW_BROWSE_PAD);
    int hints_h = jw__footer_height(state);
    /* The content box owns no bottom padding; the gap above the hint bar is the
       hint box's own top padding, so it disappears with the hints. */
    int hint_pad = (hints_h > 0) ? pad : 0;
    cat_box content = {
        0, header_h, cat_get_screen_width(),
        cat_get_screen_height() - header_h - hints_h - hint_pad,
        pad, pad, 0, pad
    };
    int list_w = cat_box_content(&content).w * 58 / 100;
    cat_box lb, ib;
    cat_box_split_cols(&content, list_w, pad, &lb, &ib);
    SDL_Rect lr = cat_box_content(&lb);
    SDL_Rect ir = cat_box_content(&ib);
    /* Fill the list box with whole rows (shared with the settings pickers), then
       snap the image pane to the same height so their bottoms land on one line. */
    int vis = ls ? ls->visible_rows : 0;
    int ih;
    lr = cat_box_fit_rows(&lb, jw__browse_base_item_h(), item_count, &vis, &ih);
    ir.h = lr.h;
    /* cat_draw_list_pane draws ls->visible_rows rows regardless of the pane
       rect, so the clamped count must land back in the list state — otherwise
       a count cached when the box was taller (hints off, smaller font) keeps
       drawing rows under the hint bar. Same idiom as the settings pickers. */
    if (ls) ((cat_list_state *)ls)->visible_rows = vis;
    /* The row pill is centered in its cell (pill_h = body + CAT_S(6); see
       jw__draw_rom_item), so the first/last pills sit inset from the cell edges
       by (ih - pill_h)/2. Inset the icon box by that same amount so its top and
       bottom line up with the pills, not the bare cell edges. (ih is the filled
       pitch in all cases - fit_rows keeps short lists on the same grid.) */
    int pad_v = (ih - (jw__browse_base_item_h() - CAT_S(6))) / 2;
    if (pad_v > 0) { ir.y += pad_v; ir.h -= pad_v * 2; }
    if (list)   *list   = lr;
    if (image)  *image  = ir;
    if (item_h) *item_h = ih;
}

/* Rows that fit the list column for the given header — used at list-init so the
 * stored visible_rows matches the renderer's box exactly. */
static int jw__browse_visible_rows(const jw_launcher_state *state, int header_h) {
    SDL_Rect list;
    jw__browse_boxes(state, header_h, 0, NULL, &list, NULL, NULL);
    int base = jw__browse_base_item_h();
    int v = (base > 0) ? list.h / base : 1;
    return v > 0 ? v : 1;
}

static void jw__draw_footer(const jw_launcher_state *state,
                            cat_footer_item *items, int count) {
    if (jw_settings_show_hints(&state->settings))
        cat_draw_footer(items, count);
}

static void jw__set_launching_status(jw_launcher_state *state,
                                     const char *name,
                                     const char *fallback) {
    if (!state) {
        return;
    }

    const char *display = name && name[0] ? name : fallback;
    if (!display || !display[0]) {
        display = "item";
    }

    size_t max_name_len = 0;
    if (sizeof(state->status) > sizeof("Launching ...")) {
        max_name_len = sizeof(state->status) - sizeof("Launching ...");
    }
    if (max_name_len > (size_t)INT_MAX) {
        max_name_len = (size_t)INT_MAX;
    }

    snprintf(state->status, sizeof(state->status), "Launching %.*s...",
             (int)max_name_len, display);
}

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
        case JW_FLAT_SYSTEM:          return state->systems[it->system_idx].display_name;
        case JW_FLAT_APPS:            return "Apps";
        case JW_FLAT_SETTINGS:        return "Settings";
        case JW_FLAT_TOOLS:           return "Tools";
        default:                      return "";
    }
}

/* ─── Tabbed: list count ──────────────────────────────────────────────────── */

static int jw__tab_list_count(const jw_launcher_state *state) {
    switch (state->current_tab) {
        case JW_TAB_RECENTS:   return state->recents_count;
        case JW_TAB_FAVORITES: return state->favorites_count;
        case JW_TAB_GAMES:     return state->system_count;
        case JW_TAB_APPS:      return state->app_count;
        case JW_TAB_SETTINGS:  return 0;  /* handled by jw_settings_ui */
        default:               return 0;
    }
}

/* ─── Library scan ────────────────────────────────────────────────────────── */

static int jw__reload_library_from_db(const char *db_path, jw_launcher_state *state) {
    if (!db_path || !state) {
        return -1;
    }

    int tab_cursor = state->list.cursor;
    int flat_cursor = state->list.cursor;
    int game_cursor = state->game_list.cursor;
    int app_cursor = state->app_list.cursor;
    char selected_system[64] = "";

    const cat_stylesheet *ss_before = cat_get_stylesheet();
    cat_launcher_layout layout_before = ss_before->launcher.layout;
    if (layout_before == CAT_LAUNCHER_TABBED) {
        if (state->current_tab == JW_TAB_GAMES &&
            state->list.cursor >= 0 &&
            state->list.cursor < state->system_count) {
            snprintf(selected_system, sizeof(selected_system), "%s",
                     state->systems[state->list.cursor].name);
        }
    } else if (state->list.cursor >= 0 && state->list.cursor < state->flat_count) {
        const jw_flat_item *it = &state->flat_items[state->list.cursor];
        if (it->kind == JW_FLAT_SYSTEM &&
            it->system_idx >= 0 && it->system_idx < state->system_count) {
            snprintf(selected_system, sizeof(selected_system), "%s",
                     state->systems[it->system_idx].name);
        }
    }

    if (jw_db_read_summary(db_path, &state->summary) != 0) {
        return -1;
    }

    jw_db_list_systems(db_path, state->systems, JW_MAX_SYSTEMS, &state->system_count);
    jw__resolve_system_names(db_path, state);
    jw_db_list_apps(db_path, state->apps, JW_MAX_APPS, &state->app_count);

    /* Dev-only layout filler: JAWAKA_FAKE_APPS=N appends N synthetic rows to
       the Apps tab to evaluate how a fuller list renders. Empty pak_dir marks
       them unlaunchable (guarded at the launch site). */
    const char *fake_apps_env = getenv("JAWAKA_FAKE_APPS");
    if (fake_apps_env && fake_apps_env[0]) {
        int want = atoi(fake_apps_env);
        for (int i = 0; i < want && state->app_count < JW_MAX_APPS; i++) {
            jw_app_entry *fake = &state->apps[state->app_count++];
            memset(fake, 0, sizeof(*fake));
            snprintf(fake->name, sizeof(fake->name), "Sample App %02d", i + 1);
        }
    }

    if (jw_db_list_recent_games(db_path, state->recents, JW_MAX_RECENTS,
                                &state->recents_count) != 0) {
        state->recents_count = 0;
    }
    if (jw_db_list_favorite_games(db_path, state->favorites, JW_MAX_FAVORITES,
                                  &state->favorites_count) != 0) {
        state->favorites_count = 0;
    }

    if (state->games_open) {
        int rc = -1;
        if (state->games_are_favorites) {
            rc = jw_db_list_favorite_games(db_path, state->games, JW_MAX_GAMES,
                                           &state->game_count);
        } else if (strcmp(state->game_system, "Recently Played") == 0) {
            rc = jw_db_list_recent_games(db_path, state->games, JW_MAX_GAMES,
                                         &state->game_count);
        } else if (state->game_system[0]) {
            rc = jw_db_list_games_for_system(db_path, state->game_system,
                                             state->games, JW_MAX_GAMES,
                                             &state->game_count);
        }
        if (rc != 0 || state->game_count <= 0) {
            state->games_open = false;
            state->games_are_favorites = false;
            state->game_count = 0;
        } else {
            if (game_cursor >= state->game_count) {
                game_cursor = state->game_count - 1;
            }
            cat_list_state_jump(&state->game_list, game_cursor, state->game_count);
        }
    }

    if (state->apps_open) {
        if (state->app_count <= 0) {
            state->apps_open = false;
        } else {
            if (app_cursor >= state->app_count) {
                app_cursor = state->app_count - 1;
            }
            cat_list_state_jump(&state->app_list, app_cursor, state->app_count);
        }
    }

    state->search_open = false;
    state->search_count = 0;

    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;
    if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW) {
        jw__build_carousel_list(state);
        int count = state->flat_count;
        if (selected_system[0]) {
            int selected_cursor = jw__flat_cursor_for_system(state, selected_system);
            if (selected_cursor >= 0) {
                flat_cursor = selected_cursor;
            }
        }
        if (flat_cursor >= count) {
            flat_cursor = count > 0 ? count - 1 : 0;
        }
        cat_list_state_jump(&state->list, flat_cursor, count);
    } else if (layout == CAT_LAUNCHER_VERTICAL) {
        jw__build_flat_list(state);
        int count = state->flat_count;
        if (selected_system[0]) {
            int selected_cursor = jw__flat_cursor_for_system(state, selected_system);
            if (selected_cursor >= 0) {
                flat_cursor = selected_cursor;
            }
        }
        if (flat_cursor >= count) {
            flat_cursor = count > 0 ? count - 1 : 0;
        }
        cat_list_state_jump(&state->list, flat_cursor, count);
    } else {
        int count = jw__tab_list_count(state);
        if (selected_system[0] && state->current_tab == JW_TAB_GAMES) {
            int selected_cursor = jw__system_index_by_id(state, selected_system);
            if (selected_cursor >= 0) {
                tab_cursor = selected_cursor;
            }
        }
        if (tab_cursor >= count) {
            tab_cursor = count > 0 ? count - 1 : 0;
        }
        cat_list_state_jump(&state->list, tab_cursor, count);
    }

    state->scan_ready = true;
    return 0;
}

static int jw__scan_library(const char *socket_path, const char *db_path,
                             jw_launcher_state *state) {
    int rc = jw_ipc_scan_library(socket_path, state->status, sizeof(state->status));
    if (rc != 0) return -1;

    if (jw__reload_library_from_db(db_path, state) != 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "scan complete but summary load failed");
        return -1;
    }

    if (jw_ipc_library_status(socket_path, &state->library_generation) != 0) {
        state->library_generation = -1;
    }
    snprintf(state->status, sizeof(state->status), "%d games, %d systems, %d apps",
        state->summary.game_count, state->system_count, state->summary.app_count);
    return 0;
}

static int jw__load_library_cache(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state) {
    if (jw__reload_library_from_db(db_path, state) != 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "library cache unavailable");
        return -1;
    }

    if (jw_ipc_library_status(socket_path, &state->library_generation) != 0) {
        state->library_generation = -1;
    }
    snprintf(state->status, sizeof(state->status), "%d games, %d systems, %d apps",
        state->summary.game_count, state->system_count, state->summary.app_count);
    return 0;
}

/* ── Background status poller ───────────────────────────────────────────────
   The status-bar volume/Wi-Fi/Bluetooth reads and the library-generation check
   all hit the daemon over IPC (the daemon forks pactl for volume) or shell out.
   Done on the render thread they stalled a frame ~once a second — visible as a
   hitch in any continuous animation (e.g. the long-name marquee).

   Threading contract: the worker never touches jw_settings_ui (the render
   thread owns it and also writes the same cached fields from the settings
   pages). All cross-thread traffic goes through the C11 atomics below — the
   render thread publishes which values to poll (poll_mask) and consumes the
   samples into the settings/status-bar caches once per loop
   (jw__status_poller_sync); the worker only reads the mask and stores samples.
   JW_STATUS_SAMPLE_NONE marks an empty mailbox slot. */
#define JW_STATUS_POLL_VOLUME (1 << 0)
#define JW_STATUS_POLL_WIFI   (1 << 1)
#define JW_STATUS_POLL_BT     (1 << 2)
#define JW_STATUS_SAMPLE_NONE INT_MIN

typedef struct {
    pthread_t   thread;
    bool        started;
    atomic_bool stop;
    char        socket_path[PATH_MAX];
    atomic_int  poll_mask;    /* JW_STATUS_POLL_* bits the render thread wants */
    atomic_int  generation;   /* latest library generation (-1 = unknown yet) */
    atomic_int  volume;       /* sample mailboxes, JW_STATUS_SAMPLE_NONE = empty */
    atomic_int  wifi;         /* 0..3 strength, -1 = wifi unavailable */
    atomic_int  bt;           /* 0=off, 1=on, 2=connected */
    atomic_int  battery;      /* 0..100, -1 = unknown */
    atomic_int  charging;     /* 0/1, -1 = unknown */
    uint32_t    fb_last_fast; /* render-thread fallback throttles (worker never started) */
    uint32_t    fb_last_slow;
    /* Scrape progress snapshot, refreshed at the fast cadence. Strings need
       more than an atomic: the worker copies under scrape_mu and bumps
       scrape_seq; the render thread re-copies when the seq moved. */
    pthread_mutex_t scrape_mu;
    jw_ipc_scrape_status_info scrape;
    atomic_int  scrape_seq;
} jw_status_poller;

static jw_status_poller jw__status_poller;

static void *jw__status_poll_worker(void *arg) {
    jw_status_poller *P = (jw_status_poller *)arg;
    uint32_t last_slow = 0;
    while (!atomic_load(&P->stop)) {
        int mask = atomic_load(&P->poll_mask);
        /* ~1s cadence: volume + library generation (both IPC to the daemon). */
        if (mask & JW_STATUS_POLL_VOLUME) {
            int percent = -1;
            if (jw_ipc_platform_volume(P->socket_path, &percent) == 0 && percent >= 0)
                atomic_store(&P->volume, percent > 100 ? 100 : percent);
        }
        int gen = 0;
        if (jw_ipc_library_status(P->socket_path, &gen) == 0)
            atomic_store(&P->generation, gen);
        /* Scrape progress (same fast cadence; art pops in live while the
           daemon worker downloads, so the status line should track it). */
        {
            jw_ipc_scrape_status_info scrape;
            if (jw_ipc_scrape_status(P->socket_path, &scrape) == 0) {
                pthread_mutex_lock(&P->scrape_mu);
                P->scrape = scrape;
                pthread_mutex_unlock(&P->scrape_mu);
                atomic_fetch_add(&P->scrape_seq, 1);
            }
        }
        /* ~5s cadence: Wi-Fi strength, Bluetooth state, and battery/charging.
           These shell out (wpa_cli/bluetoothctl) or IPC, so the render thread
           never spawns or blocks for status. */
        uint32_t now = SDL_GetTicks();
        if (last_slow == 0 || now - last_slow >= 5000) {
            if (mask & JW_STATUS_POLL_WIFI)
                atomic_store(&P->wifi,
                             jw_wifi_available() ? jw_wifi_strength_now() : -1);
            if (mask & JW_STATUS_POLL_BT)
                atomic_store(&P->bt, jw_settings_bt_state_now());
            int batt = -1, chg = -1;
            if (jw_ipc_platform_power_status(P->socket_path, &batt, &chg) == 0) {
                atomic_store(&P->battery, batt);
                atomic_store(&P->charging, chg);
            }
            last_slow = now;
        }
        /* Sleep ~1s, waking promptly for shutdown. */
        for (int i = 0; i < 10 && !atomic_load(&P->stop); i++) SDL_Delay(100);
    }
    return NULL;
}

/* Fallback when the worker thread could not be created: poll inline on the
   render thread with the worker's cadences — the pre-poller behavior, periodic
   frame hitch included, but status icons and library generation stay live. */
static void jw__status_poller_fallback_poll(jw_settings_ui *s, int mask) {
    jw_status_poller *P = &jw__status_poller;
    uint32_t now = SDL_GetTicks();
    if (P->fb_last_fast == 0 || now - P->fb_last_fast >= 1000) {
        if (mask & JW_STATUS_POLL_VOLUME)
            jw_settings_ui_refresh_volume(s);
        int gen = 0;
        if (jw_ipc_library_status(P->socket_path, &gen) == 0)
            atomic_store(&P->generation, gen);
        P->fb_last_fast = now;
    }
    if (P->fb_last_slow == 0 || now - P->fb_last_slow >= 5000) {
        if (mask & JW_STATUS_POLL_WIFI) {
            jw_settings_ui_refresh_wifi_strength(s);
            jw_cat_services_set_wifi_strength(
                jw_wifi_available() ? s->wifi_strength_cached : -1);
        }
        if (mask & JW_STATUS_POLL_BT)
            jw_settings_ui_refresh_bt_state(s);
        int batt = -1, chg = -1;
        if (jw_ipc_platform_power_status(P->socket_path, &batt, &chg) == 0)
            jw_cat_services_set_power(batt, chg);
        P->fb_last_slow = now;
    }
}

/* Which values the worker should sample: each icon's poll is wanted while the
   icon is shown, except while the matching settings page (A/V, Network,
   Bluetooth) already polls it live on the render thread. */
static int jw__status_poll_mask(const jw_settings_ui *s) {
    int mask = 0;
    if (jw_settings_show_volume(s) && !jw_settings_ui_wants_av_poll(s))
        mask |= JW_STATUS_POLL_VOLUME;
    if (jw_settings_show_wifi(s) && !jw_settings_ui_wants_wifi_poll(s))
        mask |= JW_STATUS_POLL_WIFI;
    if (s->show_bluetooth && !jw_settings_ui_wants_bluetooth_poll(s))
        mask |= JW_STATUS_POLL_BT;
    return mask;
}

/* Render-thread half of the poller, called once per loop iteration: publish
   what the worker should poll (reading jw_settings_ui is render-thread-only)
   and fold any delivered samples into the settings/status-bar caches. A page
   that polls its own values live (A/V, Network, Bluetooth) masks the matching
   poll off; a sample already in flight when a page opens is dropped here, and
   at worst the page's own refresh overwrites it a frame later. */
static void jw__status_poller_sync(jw_settings_ui *s) {
    jw_status_poller *P = &jw__status_poller;
    int mask = jw__status_poll_mask(s);

    if (!P->started) {
        jw__status_poller_fallback_poll(s, mask);
        return;
    }
    atomic_store(&P->poll_mask, mask);

    int v = atomic_exchange(&P->volume, JW_STATUS_SAMPLE_NONE);
    if (v != JW_STATUS_SAMPLE_NONE && (mask & JW_STATUS_POLL_VOLUME))
        s->volume_percent = v;
    v = atomic_exchange(&P->wifi, JW_STATUS_SAMPLE_NONE);
    if (v != JW_STATUS_SAMPLE_NONE && (mask & JW_STATUS_POLL_WIFI)) {
        s->wifi_strength_cached = v > 0 ? v : 0;
        jw_cat_services_set_wifi_strength(v);
    }
    v = atomic_exchange(&P->bt, JW_STATUS_SAMPLE_NONE);
    if (v != JW_STATUS_SAMPLE_NONE && (mask & JW_STATUS_POLL_BT))
        s->bt_state_cached = v;
    int batt = atomic_exchange(&P->battery, JW_STATUS_SAMPLE_NONE);
    int chg  = atomic_exchange(&P->charging, JW_STATUS_SAMPLE_NONE);
    if (batt != JW_STATUS_SAMPLE_NONE || chg != JW_STATUS_SAMPLE_NONE)
        jw_cat_services_set_power(batt == JW_STATUS_SAMPLE_NONE ? -1 : batt,
                                  chg == JW_STATUS_SAMPLE_NONE ? -1 : chg);
}

static void jw__status_poller_start(const char *socket_path, jw_settings_ui *settings) {
    jw_status_poller *P = &jw__status_poller;
    if (P->started) return;
    snprintf(P->socket_path, sizeof(P->socket_path), "%s", socket_path ? socket_path : "");
    atomic_store(&P->generation, -1);
    atomic_store(&P->volume, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->wifi, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->bt, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->battery, JW_STATUS_SAMPLE_NONE);
    atomic_store(&P->charging, JW_STATUS_SAMPLE_NONE);
    /* Seed the mask before the worker's first pass so startup doesn't skip a
       round of samples while waiting for the first sync. */
    atomic_store(&P->poll_mask, jw__status_poll_mask(settings));
    pthread_mutex_init(&P->scrape_mu, NULL);
    atomic_store(&P->scrape_seq, 0);
    atomic_store(&P->stop, false);
    P->started = true;
    if (pthread_create(&P->thread, NULL, jw__status_poll_worker, P) != 0)
        P->started = false;   /* no thread -> jw__status_poller_sync polls inline */
}

static void jw__status_poller_shutdown(void) {
    jw_status_poller *P = &jw__status_poller;
    if (!P->started) return;
    atomic_store(&P->stop, true);
    pthread_join(P->thread, NULL);
    P->started = false;
}

static void jw__poll_library_generation(const char *socket_path,
                                        const char *db_path,
                                        jw_launcher_state *state) {
    (void)socket_path;   /* the library-status IPC now runs on the status poller */
    if (!db_path || !state) {
        return;
    }

    int generation = atomic_load(&jw__status_poller.generation);
    if (generation < 0) {
        return;   /* poller hasn't reported a generation yet */
    }
    if (state->library_generation < 0) {
        /* The startup status query failed, so the cached library may predate a
           scan that completed in the meantime — reload before adopting. */
        if (jw__reload_library_from_db(db_path, state) == 0) {
            state->library_generation = generation;
            cat_request_frame();
        }
        return;
    }
    if (generation == state->library_generation) {
        return;
    }

    if (jw__reload_library_from_db(db_path, state) == 0) {
        state->library_generation = generation;
        /* Drop cached cover textures: a generation bump can mean art was
           *replaced* at an unchanged path (re-scrape), and the texture cache
           is path-keyed. The on-disk thumbnail is mtime-checked on reload, so
           covers rebuild from fresh sources; untouched covers re-decode their
           small thumbnails (~15ms each) at worst. */
        cat_cache_clear();
        snprintf(state->status, sizeof(state->status), "%d games, %d systems, %d apps",
                 state->summary.game_count, state->system_count, state->summary.app_count);
        cat_request_frame();
    }
}

/* Surface scrape progress in the status line: live counts while the daemon
   worker runs and a one-shot summary when the batch finishes. Reads the
   poller's snapshot — no IPC on the render thread. */
static void jw__poll_scrape_status(jw_launcher_state *state) {
    jw_status_poller *P = &jw__status_poller;
    if (!P->started) {
        return;   /* rare fallback path: no scrape progress line */
    }

    static int  last_seq = 0;
    static bool was_active = false;
    int seq = atomic_load(&P->scrape_seq);
    if (seq == last_seq) {
        return;
    }
    last_seq = seq;

    jw_ipc_scrape_status_info s;
    pthread_mutex_lock(&P->scrape_mu);
    s = P->scrape;
    pthread_mutex_unlock(&P->scrape_mu);

    bool active = strcmp(s.state, "running") == 0 ||
                  strcmp(s.state, "paused-quota") == 0;
    if (active) {
        char item[224] = "";
        if (s.current_name[0]) {
            snprintf(item, sizeof(item), "%.*s",
                     (int)sizeof(item) - 1, s.current_name);
            char *dot = strrchr(item, '.');
            if (dot && dot != item) *dot = '\0';
        }
        if (strcmp(s.state, "paused-quota") == 0) {
            snprintf(state->status, sizeof(state->status),
                     "Scraping paused: %.200s",
                     s.message[0] ? s.message : "daily quota exceeded");
        } else if (item[0]) {
            int shown = s.done + 1 > s.total ? s.total : s.done + 1;
            snprintf(state->status, sizeof(state->status),
                     "Scraping %s: %d/%d - %.160s",
                     s.current_system, shown, s.total, item);
        } else {
            snprintf(state->status, sizeof(state->status),
                     "Scraping: %d/%d", s.done, s.total);
        }
        cat_request_frame();
        was_active = true;
    } else if (was_active) {
        was_active = false;
        if (s.total > 0) {
            size_t cap = sizeof(state->status);
            int n = snprintf(state->status, cap,
                             "Scrape finished: %d found", s.found);
            if (s.not_found > 0 && n > 0 && (size_t)n < cap) {
                n += snprintf(state->status + n, cap - (size_t)n,
                              ", %d not found", s.not_found);
            }
            if (s.failed > 0 && n > 0 && (size_t)n < cap) {
                snprintf(state->status + n, cap - (size_t)n,
                         ", %d failed", s.failed);
            }
            cat_request_frame();
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TABBED RENDER
 * ═══════════════════════════════════════════════════════════════════════════ */

static void jw__load_favorites_tab(const char *db_path, jw_launcher_state *state) {
    if (!db_path || jw_db_list_favorite_games(db_path, state->favorites,
                                              JW_MAX_FAVORITES,
                                              &state->favorites_count) != 0) {
        state->favorites_count = 0;
    }
}

static void jw__load_recents_tab(const char *db_path, jw_launcher_state *state) {
    if (!db_path || jw_db_list_recent_games(db_path, state->recents,
                                            JW_MAX_RECENTS,
                                            &state->recents_count) != 0) {
        state->recents_count = 0;
    }
}

static void jw__switch_tab(jw_launcher_state *state, int direction, const char *db_path) {
    if (!state) return;
    /* Cast to int and add COUNT before the modulo: jw_tab is an unsigned enum
       on the device toolchain, so (0u + -1) would wrap to UINT_MAX and
       UINT_MAX % COUNT is 0, not COUNT-1 — breaking the backward wrap at the
       first tab. Adding COUNT keeps the dividend non-negative for dir = +/-1. */
    int next = ((int)state->current_tab + direction + JW_TAB_COUNT) % JW_TAB_COUNT;
    state->current_tab = (jw_tab)next;
    /* Favorites/Recents are reloaded on entry so newly toggled/played items appear. */
    if (state->current_tab == JW_TAB_FAVORITES)
        jw__load_favorites_tab(db_path, state);
    else if (state->current_tab == JW_TAB_RECENTS)
        jw__load_recents_tab(db_path, state);
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

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c  = selected ? theme->highlighted_text : theme->text;
    int name_max = iw - CAT_S(20);
    int text_y   = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    jw__draw_row_name(body, ctx->systems[idx].display_name,
        ix + CAT_S(10), text_y, name_c, name_max, selected);
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
    jw__draw_row_name(body, ctx->apps[idx].name,
        ix + CAT_S(10), text_y, name_c, iw - CAT_S(20), selected);
}

static void jw__draw_rom_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    jw__roms_ctx *ctx = (jw__roms_ctx *)user;
    ap_theme *theme   = cat_get_theme();
    TTF_Font *body    = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + CAT_S(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);

    ap_color name_c = selected ? theme->highlighted_text : theme->text;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    char name[256];
    jw__clean_rom_name(ctx->games[idx].name, name, sizeof(name));
    int name_max = iw - CAT_S(20);

    int name_x = ix + CAT_S(10);
    if (ctx->games[idx].favorite) {
        /* Drawn star (not a font glyph — the body font lacks U+2605). Use the
           bright selection-pill color on a normal row (the accent/chrome tone is
           too dark to read on the row bg), and highlighted_text on a selected
           row so it stays legible against the pill. */
        ap_color star_c = selected ? theme->highlighted_text : theme->highlight;
        int body_h = TTF_FontHeight(body);
        int star_r = body_h * 32 / 100;
        cat_draw_star(name_x + star_r, text_y + body_h / 2, star_r, star_c);
        int advance = star_r * 2 + CAT_S(6);
        name_x += advance;
        name_max -= advance;
    }

    jw__draw_row_name(body, name, name_x, text_y, name_c, name_max, selected);
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
    char name[256];
    if (result->kind == JW_SEARCH_APP)
        snprintf(name, sizeof(name), "%s", result->name);
    else
        jw__clean_rom_name(result->name, name, sizeof(name));
    jw__draw_row_name(body, name, ix + CAT_S(10), text_y, name_c, name_max, selected);

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
    jw__render_game_list_pane(state, state->recents, state->recents_count,
                              content_y, content_h, margin,
                              "No recent games yet — play something and it'll show up here");
}

static void jw__render_games(const jw_launcher_state *state,
                              int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    (void)content_h;

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, content_y, state->system_count,
                     &state->list, &list, &image, &item_h);

    if (state->system_count > 0 && state->list.cursor < state->system_count) {
        const jw_system_entry *sys = &state->systems[state->list.cursor];
        jw__draw_system_preview(image.x, image.y, image.w, image.h,
                                sys->name, sys->game_count);
    } else {
        cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
            cat_hex_to_color("#ffffff18"));
    }

    if (state->system_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? "No games found" : "Scanning library...",
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__games_ctx ctx = { state->systems };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->system_count, &state->list, item_h,
            jw__draw_game_item, &ctx);
    }
}

static void jw__render_apps(const jw_launcher_state *state,
                             int content_y, int content_h, int margin) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    (void)content_h;

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, content_y, state->app_count,
                     &state->list, &list, &image, &item_h);

    if (state->app_count > 0 && state->list.cursor < state->app_count) {
        jw__draw_app_detail(state, &state->apps[state->list.cursor],
                            image.x, image.y, image.w, image.h);
    } else {
        cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
            cat_hex_to_color("#ffffff18"));
    }

    if (state->app_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? "No apps found" : "Scanning library...",
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__apps_ctx ctx = { state->apps };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->app_count, &state->list, item_h,
            jw__draw_app_item, &ctx);
    }
}

static void jw__render_settings(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin) {
    int sw = cat_get_screen_width();

    int sx = margin;
    int sy = content_y + margin;
    int sw_inner = sw - margin * 2;
    /* Same content-box height rule as jw__browse_boxes (top pad here is the
       margin already applied via sy; the gap above the hint bar is the hint
       box's own top padding) - so the Settings list sits on the same row grid
       as the browse tabs instead of a slightly denser one. */
    int hints_h = jw__footer_height(state);
    int sh_inner = content_h - margin - ((hints_h > 0) ? margin : 0);

    jw_settings_ui_render(&state->settings, sx, sy, sw_inner, sh_inner);
}

static void jw__render_tabbed(const jw_launcher_state *state) {
    cat_clear_screen();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int header_h = jw__draw_tab_header(state);

    int content_y = header_h;
    int content_h = sh - header_h - fh;
    int margin    = CAT_S(12);

    switch (state->current_tab) {
        case JW_TAB_RECENTS:
            jw__render_recents(state, content_y, content_h, margin);
            break;
        case JW_TAB_FAVORITES:
            jw__render_favorites(state, content_y, content_h, margin);
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
        if (jw_settings_ui_screen(&state->settings) == JW_SETTINGS_NETWORK) {
            cat_footer_item footer[] = {
                { CAT_BTN_X, "Rescan",  false, JW_HINT("X") },
                { CAT_BTN_Y, "Forget",  false, JW_HINT("Y") },
                { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
                { CAT_BTN_A, "Select",  true,  JW_HINT("A") },
            };
            jw__draw_footer(state, footer, 4);
        } else if (jw_settings_ui_screen(&state->settings) == JW_SETTINGS_BLUETOOTH) {
            cat_footer_item footer[] = {
                { CAT_BTN_X, "Scan",    false, JW_HINT("X") },
                { CAT_BTN_Y, "Unpair",  false, JW_HINT("Y") },
                { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
                { CAT_BTN_A, "Select",  true,  JW_HINT("A") },
            };
            jw__draw_footer(state, footer, 4);
        } else if (jw_settings_ui_screen(&state->settings) == JW_SETTINGS_UPDATE) {
            cat_footer_item footer[] = {
                { CAT_BTN_X, "Releases", false, JW_HINT("X") },
                { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
                { CAT_BTN_A, "Select",  true,  JW_HINT("A") },
            };
            jw__draw_footer(state, footer, 3);
        } else if (jw_settings_ui_screen(&state->settings) == JW_SETTINGS_UPDATE_PICKER) {
            cat_footer_item footer[] = {
                { CAT_BTN_X, "Refresh", false, JW_HINT("X") },
                { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
                { CAT_BTN_A, "Pick",    true,  JW_HINT("A") },
            };
            jw__draw_footer(state, footer, 3);
        } else {
            cat_footer_item footer[] = {
                { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
                { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
            };
            jw__draw_footer(state, footer, 2);
        }
    } else if (state->current_tab == JW_TAB_FAVORITES) {
        cat_footer_item footer[] = {
            { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_Y,  "Remove",   false, JW_HINT("Y") },
            { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    } else if (state->current_tab == JW_TAB_RECENTS) {
        cat_footer_item footer[] = {
            { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_B,  "Remove",   false, JW_HINT("B") },
            { CAT_BTN_Y,  "Favorite", false, JW_HINT("Y") },
            { CAT_BTN_A,  "Resume",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 5);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_L1,   "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,    state->current_tab == JW_TAB_GAMES ? "Options" : "Search",
                                            false, JW_HINT("X") },
            { CAT_BTN_A,    "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 3);
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

    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    const char *label = jw__flat_label(state, idx);

    if (it->kind == JW_FLAT_SYSTEM) {
        jw__draw_row_name(body, label, ix + CAT_S(10), text_y, label_c,
                          iw - CAT_S(20), selected);
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
    jw__draw_status_bar(state);

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
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
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, margin, status_y,
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
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 2);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_X,    "Options",  false, JW_HINT("X") },
            { CAT_BTN_MENU, "Menu",     false, JW_HINT("H") },
            { CAT_BTN_Y,    "Rescan",   true,  JW_HINT("Y") },
            { CAT_BTN_A,    "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
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
    jw__draw_status_bar(state);

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);

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
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, CAT_S(12), status_y,
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
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 2);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_X,     "Options",  false, JW_HINT("X") },
            { CAT_BTN_MENU,  "Menu",     false, JW_HINT("H") },
            { CAT_BTN_Y,     "Rescan",   true,  JW_HINT("Y") },
            { CAT_BTN_A,     "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
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

/* Box-art covers are large (~1MB, ~1000px PNGs) and decoding one blocks the UI
   thread ~200ms — and the texture cache only holds a handful of entries, so
   scrolling a big system re-decodes the full image on nearly every step and the
   navigation stutters. Load covers through a persistent downscaled thumbnail cache
   instead: the first view of a cover decodes the full image once and writes a small
   thumbnail under the launcher state dir; every later view decodes that tiny
   thumbnail (~15ms), so the real art shows immediately and scrolling stays snappy. */
#define JW_COVER_THUMB_MAX 384

/* Resolve (once) the cover-thumbnail cache dir: $UMRK_INTERNAL_DATA_PATH/thumbs
   (jw_state_dir creates the state dir). Returns "" if it can't be prepared, in
   which case covers still load downscaled — just without on-disk persistence. */
static const char *jw__cover_thumb_dir(void) {
    static char dir[PATH_MAX] = "";
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        char *state = jw_state_dir();         /* creates the dir; caller frees */
        if (state && state[0]) {
            int n = snprintf(dir, sizeof(dir), "%s/thumbs", state);
            if (n > 0 && n < (int)sizeof(dir)) {
                mkdir(dir, 0775);             /* parent (state dir) already exists */
            } else {
                dir[0] = '\0';
            }
        }
        free(state);
    }
    return dir;
}

static bool jw__cover_thumb_path(const char *cover_abs, char *out, size_t out_size) {
    const char *dir = jw__cover_thumb_dir();
    if (!dir[0]) return false;
    /* FNV-1a hash of the absolute cover path -> stable, collision-resistant name. */
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)cover_abs; *p; ++p) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    int n = snprintf(out, out_size, "%s/%016llx.png", dir, (unsigned long long)h);
    return n > 0 && n < (int)out_size;
}

/* ---- Background cover decoder -----------------------------------------------
   Generating a thumbnail means decoding the full ~1MB source image, which blocks
   the UI thread ~200ms — so the *first* pass through a list (before any thumbnails
   exist) stutters, and the cover under the cursor shows blank until it is built.
   Move that decode to a worker thread with two inputs:
     - a high-priority slot = the cover under the cursor (latest wins), decoded
       first and handed back to the UI as a texture; and
     - a low-priority FIFO queue = the rest of the visible list (pre-warm), decoded
       only when the priority slot is idle. Pre-warm decodes just write the
       thumbnail to disk; the surface is discarded, so when the cursor later lands
       on that cover jw__load_cover hits the fast on-disk thumbnail path.
   Net effect: scroll freely while covers fill in a beat ahead of the cursor, in
   Favorites/Recents/search/systems alike. Covers whose thumbnail already exists
   never reach the worker (the small thumbnail decodes fast enough inline). */
#define JW_COVER_QUEUE_MAX 48          /* pre-warm backlog cap (ring buffer) */

typedef struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            started;
    bool            stop;

    /* High-priority request: the cover under the cursor (latest wins). */
    char            req_path[PATH_MAX];
    char            req_thumb[PATH_MAX];
    bool            has_req;

    /* Low-priority pre-warm queue (FIFO ring of covers to build ahead of time). */
    char            q_path[JW_COVER_QUEUE_MAX][PATH_MAX];
    char            q_thumb[JW_COVER_QUEUE_MAX][PATH_MAX];
    int             q_head;
    int             q_count;

    /* Result: worker -> main (only the priority request is delivered this way). */
    char            done_path[PATH_MAX];
    SDL_Surface    *done_surf;
    bool            has_done;
} jw_cover_loader;

static jw_cover_loader jw__cover_loader = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static void *jw__cover_worker(void *arg) {
    jw_cover_loader *L = (jw_cover_loader *)arg;
    pthread_mutex_lock(&L->lock);
    for (;;) {
        while (!L->stop && !L->has_req && L->q_count == 0) {
            pthread_cond_wait(&L->cond, &L->lock);
        }
        if (L->stop) break;

        char path[PATH_MAX], thumb[PATH_MAX];
        bool was_req;
        if (L->has_req) {                          /* priority: cursor cover first */
            snprintf(path, sizeof(path), "%s", L->req_path);
            snprintf(thumb, sizeof(thumb), "%s", L->req_thumb);
            L->has_req = false;
            was_req = true;
        } else {                                   /* else drain the pre-warm queue */
            snprintf(path, sizeof(path), "%s", L->q_path[L->q_head]);
            snprintf(thumb, sizeof(thumb), "%s", L->q_thumb[L->q_head]);
            L->q_head = (L->q_head + 1) % JW_COVER_QUEUE_MAX;
            L->q_count--;
            was_req = false;
        }
        pthread_mutex_unlock(&L->lock);

        SDL_Surface *surf = cat_decode_thumbnail_surface(path, thumb[0] ? thumb : NULL,
                                                         JW_COVER_THUMB_MAX);

        pthread_mutex_lock(&L->lock);
        if (was_req) {
            if (L->has_done && L->done_surf) {     /* drop a previous undelivered result */
                SDL_FreeSurface(L->done_surf);
                L->done_surf = NULL;
                L->has_done = false;
            }
            if (surf) {
                snprintf(L->done_path, sizeof(L->done_path), "%s", path);
                L->done_surf = surf;
                L->has_done = true;
            }
        } else if (surf) {
            SDL_FreeSurface(surf);                 /* pre-warm: thumbnail is on disk now */
        }
    }
    pthread_mutex_unlock(&L->lock);
    return NULL;
}

static void jw__cover_loader_ensure(void) {
    jw_cover_loader *L = &jw__cover_loader;
    if (L->started) return;            /* called on the main thread only */
    L->started = true;
    if (pthread_create(&L->thread, NULL, jw__cover_worker, L) != 0) {
        L->started = false;            /* no worker -> callers decode synchronously */
    }
}

static void jw__cover_loader_shutdown(void) {
    jw_cover_loader *L = &jw__cover_loader;
    if (!L->started) return;

    pthread_mutex_lock(&L->lock);
    L->stop = true;
    pthread_cond_signal(&L->cond);
    pthread_mutex_unlock(&L->lock);

    pthread_join(L->thread, NULL);

    pthread_mutex_lock(&L->lock);
    if (L->done_surf) {
        SDL_FreeSurface(L->done_surf);
        L->done_surf = NULL;
    }
    L->has_done = false;
    L->has_req = false;
    L->q_head = 0;
    L->q_count = 0;
    L->stop = false;
    L->started = false;
    pthread_mutex_unlock(&L->lock);
}

/* If a decoded surface for `path` is ready, consume it into *out and return true.
   Otherwise set `path` as the priority request (newest wins) and return false. */
static bool jw__cover_async_take(const char *path, const char *thumb, SDL_Surface **out) {
    jw_cover_loader *L = &jw__cover_loader;
    jw__cover_loader_ensure();
    if (!L->started) return false;
    bool got = false;
    pthread_mutex_lock(&L->lock);
    if (L->has_done && strcmp(L->done_path, path) == 0) {
        *out = L->done_surf;
        L->done_surf = NULL;
        L->has_done = false;
        got = true;
    } else {
        if (L->has_done && L->done_surf) {     /* stale result for another cover */
            SDL_FreeSurface(L->done_surf);
            L->done_surf = NULL;
            L->has_done = false;
        }
        if (!L->has_req || strcmp(L->req_path, path) != 0) {
            snprintf(L->req_path, sizeof(L->req_path), "%s", path);
            snprintf(L->req_thumb, sizeof(L->req_thumb), "%s", thumb ? thumb : "");
            L->has_req = true;
            pthread_cond_signal(&L->cond);
        }
    }
    pthread_mutex_unlock(&L->lock);
    return got;
}

/* Append a cover to the low-priority pre-warm queue, skipping duplicates and the
   in-flight priority request. Caller has already confirmed the thumbnail is
   missing. No-op when the queue is full (we just pre-warm fewer covers). */
static void jw__cover_prewarm_enqueue(const char *path, const char *thumb) {
    jw_cover_loader *L = &jw__cover_loader;
    jw__cover_loader_ensure();
    if (!L->started) return;
    pthread_mutex_lock(&L->lock);
    bool dup = (L->has_req && strcmp(L->req_path, path) == 0);
    for (int i = 0; !dup && i < L->q_count; ++i) {
        int idx = (L->q_head + i) % JW_COVER_QUEUE_MAX;
        if (strcmp(L->q_path[idx], path) == 0) dup = true;
    }
    if (!dup && L->q_count < JW_COVER_QUEUE_MAX) {
        int tail = (L->q_head + L->q_count) % JW_COVER_QUEUE_MAX;
        snprintf(L->q_path[tail], PATH_MAX, "%s", path);
        snprintf(L->q_thumb[tail], PATH_MAX, "%s", thumb ? thumb : "");
        L->q_count++;
        pthread_cond_signal(&L->cond);
    }
    pthread_mutex_unlock(&L->lock);
}

/* Load a cover texture. Returns the texture when ready (cached, fast-thumbnail, or
   freshly-decoded by the worker). When the cover still needs the worker to build
   its thumbnail, returns NULL and sets *out_pending=true so the caller can leave
   the panel empty (rather than flashing the system icon) until the art streams in. */
static SDL_Texture *jw__load_cover(const jw_launcher_state *state, const char *cover_abs,
                                   int *out_w, int *out_h, bool *out_pending) {
    if (out_pending) *out_pending = false;
    if (!state || !cover_abs || !cover_abs[0]) return NULL;

    int w = 0, h = 0;
    SDL_Texture *cached = cat_cache_get(cover_abs, &w, &h);
    if (cached) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        return cached;
    }

    char thumb[PATH_MAX];
    const char *thumb_path = jw__cover_thumb_path(cover_abs, thumb, sizeof(thumb))
                                 ? thumb : NULL;

    /* Fast path: a thumbnail already exists -> decode it inline (cheap). */
    if (thumb_path && cat_thumbnail_is_cached(cover_abs, thumb_path)) {
        SDL_Texture *tex = cat_load_image_thumbnail(cover_abs, thumb_path,
                                                    JW_COVER_THUMB_MAX, &w, &h);
        if (tex) {
            cat_cache_put(cover_abs, tex, w, h);
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            return tex;
        }
        /* unreadable thumbnail -> fall through and let the worker rebuild it */
    }

    /* Slow path: building the thumbnail needs a full decode -> do it off-thread. */
    SDL_Surface *surf = NULL;
    if (jw__cover_async_take(cover_abs, thumb_path, &surf)) {
        SDL_Texture *tex = cat_texture_from_surface(surf);
        w = surf->w;
        h = surf->h;
        SDL_FreeSurface(surf);
        if (tex) {
            cat_cache_put(cover_abs, tex, w, h);
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            return tex;
        }
        return NULL;
    }

    if (out_pending) *out_pending = true;
    cat_request_frame_in(40);                  /* re-check until the worker finishes */
    return NULL;
}

/* Pre-warm covers around the cursor so navigating lands on art that is already
   built. Windows the work (a few behind, more ahead) so even huge systems only
   enqueue near-cursor covers, while small lists (Favorites/Recents) fill within a
   couple of cursor moves. Cheap to call every render: bounded by the window and
   skips covers already decoded or already thumbnailed. */
#define JW_PREWARM_BEHIND 2
#define JW_PREWARM_AHEAD  16
static void jw__cover_prewarm(const jw_launcher_state *state,
                              const jw_game_entry *games, int count, int cursor) {
    if (!state || !games || count <= 0) return;
    int from = cursor - JW_PREWARM_BEHIND;
    if (from < 0) from = 0;
    int to = cursor + JW_PREWARM_AHEAD;
    if (to > count - 1) to = count - 1;
    for (int i = from; i <= to; ++i) {
        const char *rel = games[i].image_path;
        if (!rel[0]) continue;
        char abs[PATH_MAX];
        if (jw__resolve_sdcard_path(state, rel, abs, sizeof(abs)) != 0) continue;
        if (cat_cache_get(abs, NULL, NULL)) continue;          /* already decoded */
        char thumb[PATH_MAX];
        const char *tp = jw__cover_thumb_path(abs, thumb, sizeof(thumb)) ? thumb : NULL;
        if (tp && cat_thumbnail_is_cached(abs, tp)) continue;  /* already built */
        jw__cover_prewarm_enqueue(abs, tp);
    }
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

/* Like jw__draw_image_fit, but clips the image to the same rounded-corner shape
   as the list pills (theme pill radius + pill_corner_mask), so cover art matches
   the current List Style setting. Used ONLY for real box art — never icons. */
static void jw__draw_cover_fit(SDL_Texture *tex, int tex_w, int tex_h,
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

    /* Round in proportion to the cover (scaled by the List Style ratio, so a sharp
       style stays square) and keep the style's corner mask. Tied to the image size
       so the curve reads the same on big and small art. */
    const ap_theme *theme = cat_get_theme();
    int smaller = draw_w < draw_h ? draw_w : draw_h;
    int radius = (int)(theme->pill_radius_ratio * smaller * 0.26f + 0.5f);
    unsigned corners = (unsigned)theme->pill_corner_mask;
    if (corners == 0) corners = CAT_CORNER_ALL;

    cat_draw_image_rounded_ex(tex, draw_x, draw_y, draw_w, draw_h, radius, corners);
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
     * The shared icons live next to the active theme root. */
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
    TTF_Font *small   = cat_get_font(CAT_FONT_SMALL);

    cat_draw_rounded_rect(px, py, pw, ph, CAT_S(8),
                          cat_hex_to_color("#ffffff10"));

    /* Icon: up to 88% of pane width or 340px, whichever is smaller, and never
       more than 72% of the pane height (leaves room for the count below). */
    int icon_max = CAT_S(340);
    int icon_box = pw * 88 / 100;
    if (icon_box > icon_max)    icon_box = icon_max;
    if (icon_box > ph * 72 / 100) icon_box = ph * 72 / 100;

    int sub_h = TTF_FontHeight(small);
    int gap   = CAT_S(12);

    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    if (system_code && system_code[0])
        tex = jw__load_system_icon(system_code, &tw, &th);

    /* Vertical stack: icon + count (no name), centered in the pane. */
    int block_h = (tex ? icon_box : 0) + ((game_count >= 0) ? (gap + sub_h) : 0);
    int top_y   = py + (ph - block_h) / 2;

    int count_y = top_y;
    if (tex) {
        jw__draw_image_fit(tex, tw, th,
                           px + (pw - icon_box) / 2, top_y, icon_box, icon_box);
        count_y = top_y + icon_box + gap;
    }

    if (game_count >= 0) {
        char sub[32];
        snprintf(sub, sizeof(sub), "%d games", game_count);
        int subw = cat_measure_text(small, sub);
        cat_draw_text(small, sub, px + (pw - subw) / 2, count_y, theme->hint);
    }
}

static int jw__resolve_app_icon_path(const jw_launcher_state *state,
                                     const jw_app_entry *app,
                                     char *out, size_t out_size) {
    if (!state || !app || !app->icon[0] || !out || out_size == 0) {
        return -1;
    }

    if (app->icon[0] == '/') {
        int needed = snprintf(out, out_size, "%s", app->icon);
        return needed >= 0 && needed < (int)out_size ? 0 : -1;
    }

    char pak_abs[PATH_MAX];
    if (jw__resolve_sdcard_path(state, app->pak_dir, pak_abs, sizeof(pak_abs)) != 0) {
        return -1;
    }

    int needed = snprintf(out, out_size, "%s/%s", pak_abs, app->icon);
    return needed >= 0 && needed < (int)out_size ? 0 : -1;
}

static void jw__draw_app_detail(const jw_launcher_state *state,
                                const jw_app_entry *app,
                                int detail_x, int detail_y,
                                int detail_w, int detail_h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    cat_draw_rounded_rect(detail_x, detail_y, detail_w, detail_h, CAT_S(8),
                          cat_hex_to_color("#ffffff10"));

    if (!app) {
        return;
    }

    /* Icon sizing mirrors jw__draw_system_preview so both panes match. */
    int icon_max = CAT_S(340);
    int icon_box = detail_w * 88 / 100;
    if (icon_box > icon_max)           icon_box = icon_max;
    if (icon_box > detail_h * 72 / 100) icon_box = detail_h * 72 / 100;

    int sub_h = TTF_FontHeight(small);
    int gap   = CAT_S(12);

    SDL_Texture *tex = NULL;
    int icon_w = 0, icon_h = 0;
    char icon_abs[PATH_MAX];
    if (jw__resolve_app_icon_path(state, app, icon_abs, sizeof(icon_abs)) == 0) {
        tex = jw__load_cached_image(icon_abs, &icon_w, &icon_h);
    }
    if (!tex) {
        /* Apps that ship no icon fall back to the Leaf badge. */
        tex = jw__load_system_icon("_apps", &icon_w, &icon_h);
    }

    /* Vertical stack: icon + name, centered in the pane (matches the system
       preview's icon + count layout). */
    int block_h = (tex ? icon_box : 0) + gap + sub_h;
    int top_y   = detail_y + (detail_h - block_h) / 2;

    int label_y = top_y;
    if (tex) {
        jw__draw_image_fit(tex, icon_w, icon_h,
                           detail_x + (detail_w - icon_box) / 2, top_y,
                           icon_box, icon_box);
        label_y = top_y + icon_box + gap;
    }

    int max_w  = detail_w - CAT_S(16) * 2;
    int name_w = cat_measure_text(small, app->name);
    if (name_w > max_w) name_w = max_w;
    cat_draw_text_ellipsized(small, app->name,
                             detail_x + (detail_w - name_w) / 2, label_y,
                             theme->hint, max_w);
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
    jw__draw_status_bar(state);

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme    = cat_get_theme();
    TTF_Font *label_font = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);

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
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 2);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_X,    "Options",  false, JW_HINT("X") },
            { CAT_BTN_MENU, "Menu",     false, JW_HINT("H") },
            { CAT_BTN_Y,    "Rescan",   true,  JW_HINT("Y") },
            { CAT_BTN_A,    "Select",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    }
    cat_present();
}

/* Top header height of the system game browser: the tab bar + system-name
   sub-header in the tabbed layout, otherwise a single title row. Shared by the
   renderer and the visible-row count so the list never reports more rows than
   actually fit (which would run the last selection pill off the bottom). */
static int jw__game_browser_header_h(const jw_launcher_state *state) {
    (void)state;
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
        return cat_get_tab_bar_height() + CAT_S(2) +
               TTF_FontHeight(cat_get_font(CAT_FONT_EXTRA_LARGE));
    return CAT_DS(30);
}

/* Header height for the search page: tab bar + "Search:" sub-header in tabbed
   mode, or the standalone title bar otherwise. Shared by the renderer and the
   visible-row count so the box model and row count never disagree. */
static int jw__search_header_h(const jw_launcher_state *state) {
    (void)state;
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
        return cat_get_tab_bar_height() + CAT_S(2) +
               TTF_FontHeight(cat_get_font(CAT_FONT_EXTRA_LARGE));
    return CAT_DS(34);
}

static void jw__render_game_browser(const jw_launcher_state *state) {
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int margin = CAT_S(12);

    /* In the tabbed layout, show the section tabs across the top (current
       section highlighted) so the user can tab to any section from within a
       system's game list, with the status icons inline in the tab bar — the
       same header as the tabbed home view. The system name drops to a
       sub-header beneath the tabs. Other layouts keep the standalone title +
       full status bar. */
    bool tabbed = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED);
    int header_h;
    int title_y;
    int title_max;
    if (tabbed) {
        int bar_h = cat_get_tab_bar_height();
        int pill_h = CAT_DS(CAT__PILL_SIZE);
        cat_status_bar_opts sb = {0};
        jw_settings_status_bar_opts(&state->settings, &sb);
        sb.no_pill    = true;
        sb.use_y      = true;
        sb.y_position = (bar_h - pill_h) / 2;
        cat_set_tab_bar_reserved_right(cat_get_status_bar_width(&sb) + CAT_S(12));
        cat_draw_tab_bar(kTabs, JW_TAB_COUNT, (int)state->current_tab);
        cat_draw_status_bar(&sb);

        title_y   = bar_h + CAT_S(2);
        header_h  = jw__game_browser_header_h(state);
        title_max = sw - margin * 2;
    } else {
        jw__draw_status_bar(state);
        header_h = jw__game_browser_header_h(state);
        title_y  = CAT_S(6);
        /* Cap the title's visible width so it stops before the status bar
           (top-right). Width adapts when the user hides battery/wifi/clock. */
        cat_status_bar_opts title_sb = {0};
        jw_settings_status_bar_opts(&state->settings, &title_sb);
        title_max = sw - cat_get_status_bar_width(&title_sb) - margin * 3;
        if (title_max < CAT_S(120)) title_max = CAT_S(120);
    }

    int region_y = header_h;
    int region_h = cat_get_screen_height() - header_h - jw__footer_height(state);

    char title[96];
    if (state->games_are_favorites)
        snprintf(title, sizeof(title), "%s", "Favorites");
    else
        snprintf(title, sizeof(title), "%s", state->game_system_display);

    /* A title longer than title_max scrolls (looping marquee) instead of
       truncating, so the full system name is always readable. State is
       function-static (one browser at a time) and resets when the title
       changes. */
    {
        static cat_marquee title_marquee;
        static char        last_title[96] = "";
        static uint32_t    last_ms = 0;
        uint32_t now = SDL_GetTicks();
        if (strcmp(title, last_title) != 0) {
            title_marquee.elapsed_ms = 0;
            snprintf(last_title, sizeof(last_title), "%s", title);
            last_ms = now;
        }
        uint32_t dt = (last_ms == 0) ? 0u : (now - last_ms);
        last_ms = now;
        if (cat_draw_text_marquee(large, title, margin, title_y, theme->text,
                                  title_max, &title_marquee, dt))
            cat_request_frame();
    }

    (void)region_h;
    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, region_y, state->game_count,
                     &state->game_list, &list, &image, &item_h);

    if (state->game_count == 0) {
        cat_draw_text_wrapped(body, "No games found",
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__roms_ctx ctx = { state->games };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->game_count, &state->game_list, item_h,
            jw__draw_rom_item, &ctx);
    }

    cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->game_count > 0 && state->game_list.cursor < state->game_count) {
        const jw_game_entry *game = &state->games[state->game_list.cursor];
        jw__cover_prewarm(state, state->games, state->game_count,
                          state->game_list.cursor);
        /* Show the cover centered/fit in the panel — no name (it's in the list
           on the left). When a game has no cover, fall back to the system icon
           as a placeholder so the panel is never empty. */
        int art_pad = CAT_S(16);
        char image_abs[PATH_MAX];
        int iw = 0, ih = 0;
        SDL_Texture *tex = NULL;
        bool is_cover = false;
        bool pending = false;
        if (jw__resolve_sdcard_path(state, game->image_path, image_abs, sizeof(image_abs)) == 0) {
            tex = jw__load_cover(state, image_abs, &iw, &ih, &pending);
            is_cover = (tex != NULL);
        }
        if (!tex && !pending)            /* genuine no-cover; while pending leave empty */
            tex = jw__load_system_icon(game->system, &iw, &ih);
        if (tex) {
            /* Round real box art to match the list style; leave the icon
               fallback square. */
            if (is_cover)
                jw__draw_cover_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                                   image.w - art_pad * 2, image.h - art_pad * 2);
            else
                jw__draw_image_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                                   image.w - art_pad * 2, image.h - art_pad * 2);
        }
    }

    if (tabbed) {
        cat_footer_item footer[] = {
            { CAT_BTN_L1, "Tab",      false, JW_HINT_DEVICE(";/t", "L1/R1") },
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_Y,  "Favorite", false, JW_HINT("Y") },
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 5);
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_X,  "Options",  false, JW_HINT("X") },
            { CAT_BTN_Y,  "Favorite", false, JW_HINT("Y") },
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
        };
        jw__draw_footer(state, footer, 4);
    }
    cat_present();
}

/* Shared tabbed-tab content for game lists (Favorites, Recents): the list with
   star markers on the left and box art for the selected game on the right. */
static void jw__render_game_list_pane(const jw_launcher_state *state,
                                      const jw_game_entry *entries, int count,
                                      int content_y, int content_h, int margin,
                                      const char *empty_msg) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    (void)content_h;

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, content_y, count, &state->list,
                     &list, &image, &item_h);

    if (count == 0) {
        cat_draw_text_wrapped(body, empty_msg,
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
        return;
    }

    jw__roms_ctx ctx = { entries };
    cat_draw_list_pane(list.x, list.y, list.w, list.h,
        count, &state->list, item_h, jw__draw_rom_item, &ctx);

    cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->list.cursor >= count) return;
    const jw_game_entry *game = &entries[state->list.cursor];
    jw__cover_prewarm(state, entries, count, state->list.cursor);

    /* Cover centered/fit in the panel — no name (it's in the list). When a game
       has no cover, fall back to its system icon so the panel is never empty. */
    int art_pad = CAT_S(16);
    char image_abs[PATH_MAX];
    int iw = 0, ih = 0;
    SDL_Texture *tex = NULL;
    bool is_cover = false;
    bool pending = false;
    if (jw__resolve_sdcard_path(state, game->image_path, image_abs, sizeof(image_abs)) == 0) {
        tex = jw__load_cover(state, image_abs, &iw, &ih, &pending);
        is_cover = (tex != NULL);
    }
    if (!tex && !pending)            /* genuine no-cover; while pending leave empty */
        tex = jw__load_system_icon(game->system, &iw, &ih);
    if (tex) {
        /* Round real box art to match the list style; icon fallback stays square. */
        if (is_cover)
            jw__draw_cover_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                               image.w - art_pad * 2, image.h - art_pad * 2);
        else
            jw__draw_image_fit(tex, iw, ih, image.x + art_pad, image.y + art_pad,
                               image.w - art_pad * 2, image.h - art_pad * 2);
    }
}

static void jw__render_favorites(const jw_launcher_state *state,
                                 int content_y, int content_h, int margin) {
    jw__render_game_list_pane(state, state->favorites, state->favorites_count,
                              content_y, content_h, margin,
                              "No favorites yet — open a game and press Y to add one");
}

static void jw__render_app_browser(const jw_launcher_state *state) {
    cat_clear_screen();
    jw__draw_status_bar(state);

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int margin = CAT_S(12);
    int header_h = CAT_DS(30);
    int content_y = header_h + margin;
    int content_h = sh - content_y - fh - margin;

    cat_draw_text_ellipsized(large, "Apps", margin, CAT_S(6),
                             theme->text, sw - margin * 2);

    int list_x = margin;
    int list_w = sw * 58 / 100;
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int detail_x = list_x + list_w + margin;
    int detail_w = sw - detail_x - margin;

    if (state->app_count == 0) {
        cat_draw_text_wrapped(body,
            state->scan_ready ? "No apps found" : "Scanning library...",
            list_x + CAT_S(8), content_y + CAT_S(8),
            list_w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__apps_ctx ctx = { state->apps };
        cat_draw_list_pane(list_x, content_y, list_w, content_h,
            state->app_count, &state->app_list, item_h,
            jw__draw_app_item, &ctx);
    }

    if (state->app_count > 0 && state->app_list.cursor < state->app_count) {
        jw__draw_app_detail(state, &state->apps[state->app_list.cursor],
                            detail_x, content_y, detail_w, content_h);
    } else {
        cat_draw_rounded_rect(detail_x, content_y, detail_w, content_h, CAT_S(8),
            cat_hex_to_color("#ffffff10"));
    }

    int status_y = content_y + content_h - TTF_FontHeight(small);
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, sw - margin * 2);

    cat_footer_item footer[] = {
        { CAT_BTN_X,  "Search",   false, JW_HINT("X") },
        { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
        { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
    };
    cat_draw_footer(footer, 3);
    cat_present();
}

static void jw__render_search(const jw_launcher_state *state) {
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    /* TTF_Font *small = cat_get_font(CAT_FONT_SMALL); — for the commented-out results line below */
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int margin = CAT_S(12);

    /* Same header as the home tabs / game browser: in tabbed layout, the section
       tab bar with status icons inline, and "Search: <query>" as a sub-header
       beneath it (so L1/R1 can tab away). Other layouts keep the standalone
       status pill + title. */
    bool tabbed = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED);
    int title_y;
    if (tabbed) {
        title_y = jw__draw_tab_header(state) + CAT_S(2);
    } else {
        jw__draw_status_bar(state);
        title_y = CAT_S(6);
    }
    int region_y = jw__search_header_h(state);   /* shared with jw__search_visible_rows */
    (void)fh; (void)sh;

    char title[320];
    if (state->search_query[0])
        snprintf(title, sizeof(title), "Search: %s  (%d %s)", state->search_query,
                 state->search_count, state->search_count == 1 ? "result" : "results");
    else
        snprintf(title, sizeof(title), "%s", "Search: (empty)");
    cat_draw_text_ellipsized(large, title, margin, title_y, theme->text, sw - margin * 2);

    SDL_Rect list, image;
    int item_h;
    jw__browse_boxes(state, region_y, state->search_count,
                     &state->search_list, &list, &image, &item_h);

    if (state->search_count == 0) {
        cat_draw_text_wrapped(body, "No results",
            list.x + CAT_S(8), list.y + CAT_S(8),
            list.w - margin * 2, theme->hint, CAT_ALIGN_LEFT);
    } else {
        jw__search_ctx ctx = { state->search_results };
        cat_draw_list_pane(list.x, list.y, list.w, list.h,
            state->search_count, &state->search_list, item_h,
            jw__draw_search_item, &ctx);
    }

    cat_draw_rounded_rect(image.x, image.y, image.w, image.h, CAT_S(8),
        cat_hex_to_color("#ffffff10"));

    if (state->search_count > 0 && state->search_list.cursor < state->search_count) {
        const jw_search_result *result = &state->search_results[state->search_list.cursor];

        /* Cover art only — the name is already in the list rows. Game cover (its
           system icon as a fallback) or an app's icon, centered/fit in the pane,
           exactly like the Recents/Favorites detail panes. */
        int art_pad = CAT_S(16);
        int art_x   = image.x + art_pad;
        int art_y   = image.y + art_pad;
        int art_w   = image.w - art_pad * 2;
        int art_h   = image.h - art_pad * 2;

        char img_abs[PATH_MAX];
        int iw = 0, ih = 0;
        SDL_Texture *tex = NULL;
        bool is_cover = false;
        bool pending = false;
        if (result->kind == JW_SEARCH_GAME) {
            if (result->image_path[0] &&
                jw__resolve_sdcard_path(state, result->image_path, img_abs, sizeof(img_abs)) == 0) {
                tex = jw__load_cover(state, img_abs, &iw, &ih, &pending);
                is_cover = (tex != NULL);
            }
            if (!tex && !pending)        /* genuine no-cover; while pending leave empty */
                tex = jw__load_system_icon(result->system, &iw, &ih);
        } else {
            jw_app_entry app;
            memset(&app, 0, sizeof(app));
            snprintf(app.pak_dir, sizeof(app.pak_dir), "%s", result->pak_dir);
            snprintf(app.icon, sizeof(app.icon), "%s", result->icon);
            if (jw__resolve_app_icon_path(state, &app, img_abs, sizeof(img_abs)) == 0)
                tex = jw__load_cached_image(img_abs, &iw, &ih);
        }
        if (tex) {
            /* Round real box art to match the list style; icons stay square. */
            if (is_cover)
                jw__draw_cover_fit(tex, iw, ih, art_x, art_y, art_w, art_h);
            else
                jw__draw_image_fit(tex, iw, ih, art_x, art_y, art_w, art_h);
        }
    }

    /* Results-count ("N results") line at the bottom — commented out for now per request.
    int status_y = content_y + content_h - TTF_FontHeight(small);
    if (jw_settings_show_hints(&state->settings)) cat_draw_text_ellipsized(small, state->status, margin, status_y,
                             theme->hint, sw - margin * 2);
    */

    cat_footer_item footer[] = {
        { CAT_BTN_X,  "Search",   false, JW_HINT("X") },
        { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
        { CAT_BTN_A,  "Launch",   true,  JW_HINT("A") },
    };
    jw__draw_footer(state, footer, 3);
    cat_present();
}

typedef struct { const jw_launcher_state *state; } jw__actions_ctx;

static const char *jw__action_core_label(const jw_launcher_state *state,
                                         const char *core_id);
static const char *jw__action_perf_label(const char *value);

static void jw__action_row_strings(const jw_launcher_state *state,
                                   jw_action_row_kind row,
                                   char *title, size_t title_size,
                                   char *value, size_t value_size) {
    if (title && title_size > 0) title[0] = '\0';
    if (value && value_size > 0) value[0] = '\0';
    switch (row) {
        case JW_ACTION_ROW_SEARCH:
            snprintf(title, title_size, "%s", "Search This System");
            snprintf(value, value_size, "%s", "Open");
            break;
        case JW_ACTION_ROW_DISPLAY_NAME: {
            snprintf(title, title_size, "%s", "Display Name");
            if (state->action_scope == JW_ACTION_SYSTEM) {
                const char *label = state->action_system_display[0]
                    ? state->action_system_display
                    : "Default";
                if (state->action_system_display_override[0]) {
                    snprintf(value, value_size, "%s (custom)", label);
                } else {
                    snprintf(value, value_size, "%s", label);
                }
            } else {
                char name[256];
                jw__clean_rom_name(state->action_game.name, name, sizeof(name));
                snprintf(value, value_size, "%s", name[0] ? name : "Scanned");
            }
            break;
        }
        case JW_ACTION_ROW_CORE:
            snprintf(title, title_size, "%s", "Core");
            if (state->action_scope == JW_ACTION_GAME &&
                state->action_core_game_override[0]) {
                snprintf(value, value_size, "%s (game)",
                         jw__action_core_label(state, state->action_core_game_override));
            } else if (state->action_core_system_override[0]) {
                snprintf(value, value_size, "%s (system)",
                         jw__action_core_label(state, state->action_core_system_override));
            } else if (state->action_core_effective[0]) {
                snprintf(value, value_size, "%s (default)",
                         jw__action_core_label(state, state->action_core_effective));
            } else {
                snprintf(value, value_size, "%s", "Unavailable");
            }
            break;
        case JW_ACTION_ROW_PERFORMANCE:
            snprintf(title, title_size, "%s", "Performance");
            if (state->action_scope == JW_ACTION_GAME &&
                state->action_perf_game_override[0]) {
                snprintf(value, value_size, "%s (game)",
                         jw__action_perf_label(state->action_perf_game_override));
            } else if (state->action_perf_system_override[0]) {
                snprintf(value, value_size, "%s (system)",
                         jw__action_perf_label(state->action_perf_system_override));
            } else {
                snprintf(value, value_size, "%s", "Auto");
            }
            break;
        case JW_ACTION_ROW_SCRAPE:
            if (state->action_scope == JW_ACTION_GAME) {
                snprintf(title, title_size, "%s", "Scrape Artwork");
                snprintf(value, value_size, "%s", "Replace");
            } else {
                snprintf(title, title_size, "%s", "Scrape Missing Artwork");
                snprintf(value, value_size, "%s", "Start");
            }
            break;
        case JW_ACTION_ROW_SCRAPE_ALL:
            snprintf(title, title_size, "%s", "Re-scrape All Artwork");
            snprintf(value, value_size, "%s", "Replace all");
            break;
        case JW_ACTION_ROW_SCRAPE_CANCEL:
            snprintf(title, title_size, "%s", "Cancel Scraping");
            snprintf(value, value_size, "%s", "Stop");
            break;
        case JW_ACTION_ROW_RESET:
            snprintf(title, title_size, "%s",
                     state->action_scope == JW_ACTION_GAME
                         ? "Reset Game Overrides"
                         : "Reset System Overrides");
            snprintf(value, value_size, "%s", "Clear");
            break;
        default:
            break;
    }
}

static void jw__draw_action_item(int idx, int ix, int iy, int iw, int ih,
                                 bool selected, void *user) {
    jw__actions_ctx *ctx = (jw__actions_ctx *)user;
    const jw_launcher_state *state = ctx ? ctx->state : NULL;
    if (!state || idx < 0 || idx >= state->action_row_count) {
        return;
    }

    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);

    int pill_h = TTF_FontHeight(body) + CAT_S(8);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected) {
        cat_draw_pill(ix, pill_y, iw - CAT_S(4), pill_h, theme->highlight);
    }

    char title[96];
    char value[160];
    jw__action_row_strings(state, state->action_rows[idx],
                           title, sizeof(title), value, sizeof(value));

    ap_color title_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int text_y = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    int value_w = iw * 42 / 100;
    int title_w = iw - value_w - CAT_S(28);

    cat_draw_text_ellipsized(body, title, ix + CAT_S(10), text_y,
                             title_c, title_w);
    if (value[0]) {
        int value_y = pill_y + (pill_h - TTF_FontHeight(small)) / 2;
        cat_draw_text_ellipsized(small, value,
                                 ix + iw - value_w - CAT_S(12),
                                 value_y, value_c, value_w);
    }
}

/* Header height for the actions page: tab bar + name sub-header in tabbed
   mode, or the standalone title bar otherwise - same shape as search, shared
   by the renderer and the box model so they never disagree. */
static int jw__actions_header_h(void) {
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED)
        return cat_get_tab_bar_height() + CAT_S(2) +
               TTF_FontHeight(cat_get_font(CAT_FONT_EXTRA_LARGE));
    return CAT_DS(34);
}

static void jw__render_actions(const jw_launcher_state *state) {
    cat_clear_screen();

    ap_theme *theme = cat_get_theme();
    TTF_Font *large = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = jw__footer_height(state);
    int margin = CAT_S(12);

    /* Same header as the home tabs / game browser / search: in tabbed layout
       the section tab bar with the status icons inline, and the game or
       system name as a sub-header beneath it. The status bar never floats
       free in the tabbed world. Other layouts keep the standalone pill. */
    bool tabbed = (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED);
    int title_y;
    if (tabbed) {
        title_y = jw__draw_tab_header(state) + CAT_S(2);
    } else {
        jw__draw_status_bar(state);
        title_y = CAT_S(6);
    }

    int title_max = sw - margin * 2;
    if (!tabbed) {
        cat_status_bar_opts title_sb = {0};
        jw_settings_status_bar_opts(&state->settings, &title_sb);
        title_max = sw - cat_get_status_bar_width(&title_sb) - margin * 3;
        if (title_max < CAT_S(120)) title_max = CAT_S(120);
    }

    char title[320];
    if (state->action_scope == JW_ACTION_GAME) {
        char name[256];
        jw__clean_rom_name(state->action_game.name, name, sizeof(name));
        snprintf(title, sizeof(title), "%s", name);
    } else {
        snprintf(title, sizeof(title), "System Options: %s",
                 state->action_system_display);
    }

    /* A title longer than title_max scrolls (looping marquee) instead of
       truncating, so the full game name is always readable - same treatment
       as the game browser's system sub-header. */
    {
        static cat_marquee title_marquee;
        static char        last_title[320] = "";
        static uint32_t    last_ms = 0;
        uint32_t now = SDL_GetTicks();
        if (strcmp(title, last_title) != 0) {
            title_marquee.elapsed_ms = 0;
            snprintf(last_title, sizeof(last_title), "%s", title);
            last_ms = now;
        }
        uint32_t dt = (last_ms == 0) ? 0u : (now - last_ms);
        last_ms = now;
        if (cat_draw_text_marquee(large, title, margin, title_y, theme->text,
                                  title_max, &title_marquee, dt))
            cat_request_frame();
    }

    /* Box model below the header: the rows fill the content box on the
       canonical grid - same pitch and filled geometry as every tab. */
    int header_h = jw__actions_header_h();
    int hint_pad = (fh > 0) ? margin : 0;
    cat_box page = { 0, header_h, sw, sh - header_h - fh - hint_pad,
                     margin, margin, 0, margin };

    /* No status toast in this view: a selection's feedback is the row value
       itself changing, and the name already lives in the sub-header. */
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&page, item_h, state->action_row_count,
                                   &vis, &item_h);
    ((cat_list_state *)&state->action_list)->visible_rows = vis;
    jw__actions_ctx ctx = { state };
    if (state->action_row_count > 0) {
        cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h,
                           state->action_row_count, &state->action_list,
                           item_h, jw__draw_action_item, &ctx);
    }

    /* No Left/Right hint: cycling a value row is discoverable and A cycles it
       too. The Tab hint only applies where the tab bar is shown (tabbed). */
    cat_footer_item footer[] = {
        { CAT_BTN_L1,   "Tab",    false, JW_HINT_DEVICE(";/t", "L1/R1") },
        { CAT_BTN_B,    "Back",   true,  JW_HINT("B") },
        { CAT_BTN_A,    "Select", true,  JW_HINT("A") },
    };
    cat_footer_item *footer_items = tabbed ? footer : footer + 1;
    jw__draw_footer(state, footer_items, tabbed ? 3 : 2);
    cat_present();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DISPATCH
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Dedicated Select-opened switcher view: a focused recents carousel with its
   own title/footer, drawn over whatever home layout was active. */
static void jw__render_switcher(jw_launcher_state *state) {
    cat_clear_screen();

    cat_status_bar_opts sb = {0};
    jw_settings_status_bar_opts(&state->settings, &sb);
    cat_draw_screen_title("Switcher", &sb);

    bool hints = jw_settings_show_hints(&state->settings);
    SDL_Rect content = cat_get_content_rect(true, hints, false);
    int margin = CAT_S(12);
    jw_game_switcher_render(&state->switcher,
                            content.x + margin, content.y,
                            content.w - margin * 2, content.h);

    if (state->status[0]) {
        TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
        cat_draw_text_ellipsized(small, state->status,
                                 content.x + margin,
                                 content.y + content.h - TTF_FontHeight(small) - CAT_S(4),
                                 cat_get_theme()->hint, content.w - margin * 2);
    }

    cat_footer_item footer[] = {
        { CAT_BTN_Y, "Remove", false, JW_HINT("Y") },
        { CAT_BTN_B, "Back",   true,  JW_HINT("B") },
        { CAT_BTN_A, "Resume", true,  JW_HINT("A") },
    };
    jw__draw_footer(state, footer, 3);
    cat_present();
}

static void jw__render_launcher(jw_launcher_state *state) {
    if (state->switcher_open) {
        jw__render_switcher(state);
        return;
    }

    if (state->search_open) {
        jw__render_search(state);
        return;
    }

    if (state->actions_open) {
        jw__render_actions(state);
        return;
    }

    if (state->games_open) {
        jw__render_game_browser(state);
        return;
    }

    if (state->apps_open) {
        jw__render_app_browser(state);
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

static int jw__game_browser_visible_rows(const jw_launcher_state *state) {
    return jw__browse_visible_rows(state, jw__game_browser_header_h(state));
}

static int jw__app_browser_visible_rows(const jw_launcher_state *state) {
    int fh = jw__footer_height(state);
    int content_h = cat_get_screen_height() - CAT_DS(30) - CAT_S(24) - fh;
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + CAT_S(12);
    int visible = content_h / item_h;
    return visible > 0 ? visible : 1;
}

static int jw__search_visible_rows(const jw_launcher_state *state) {
    return jw__browse_visible_rows(state, jw__search_header_h(state));
}

static const jw_platform_perf_profile kActionPerfProfiles[] = {
    JW_PLATFORM_PERF_PROFILE_AUTO,
    JW_PLATFORM_PERF_PROFILE_BALANCED,
    JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
    JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
};
#define JW_ACTION_PERF_COUNT ((int)(sizeof(kActionPerfProfiles) / sizeof(kActionPerfProfiles[0])))

static bool jw__runtime_cores_dir(const jw_launcher_state *state,
                                  char *out, size_t out_size) {
    const char *env = getenv("CORES_PATH");
    if (!env || !env[0]) {
        env = getenv("JAWAKA_RETROARCH_CORES_DIR");
    }
    if (env && env[0]) {
        return snprintf(out, out_size, "%s", env) < (int)out_size;
    }

    const char *system_path = getenv("SYSTEM_PATH");
    if (system_path && system_path[0]) {
        return snprintf(out, out_size, "%s/cores", system_path) < (int)out_size;
    }

    (void)state;
    return false;
}

static int jw__action_find_core(const jw_launcher_state *state, const char *core_id) {
    if (!state || !core_id || !core_id[0]) {
        return -1;
    }
    for (size_t i = 0; i < state->action_core_count; i++) {
        if (strcmp(state->action_core_choices[i].id, core_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const char *jw__action_core_label(const jw_launcher_state *state,
                                         const char *core_id) {
    int idx = jw__action_find_core(state, core_id);
    if (idx >= 0) {
        return state->action_core_choices[idx].display_name;
    }
    return core_id && core_id[0] ? core_id : "Default";
}

static int jw__action_perf_index(const char *value) {
    jw_platform_perf_profile profile;
    if (value && value[0] && jw_platform_parse_perf_profile(value, &profile)) {
        for (int i = 0; i < JW_ACTION_PERF_COUNT; i++) {
            if (kActionPerfProfiles[i] == profile) {
                return i;
            }
        }
    }
    return 0;
}

static const char *jw__action_perf_label(const char *value) {
    jw_platform_perf_profile profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    if (value && value[0]) {
        (void)jw_platform_parse_perf_profile(value, &profile);
    }
    return jw_platform_perf_profile_label(profile);
}

static void jw__action_add_row(jw_launcher_state *state, jw_action_row_kind row) {
    if (!state || state->action_row_count >= JW_MAX_ACTION_ROWS) {
        return;
    }
    state->action_rows[state->action_row_count++] = row;
}

static void jw__action_refresh_rows(jw_launcher_state *state) {
    if (!state) {
        return;
    }
    int old_cursor = state->action_list.cursor;
    state->action_row_count = 0;
    if (state->action_scope == JW_ACTION_SYSTEM) {
        jw__action_add_row(state, JW_ACTION_ROW_SEARCH);
        jw__action_add_row(state, JW_ACTION_ROW_DISPLAY_NAME);
        if (state->action_core_count > 1 || state->action_core_system_override[0]) {
            jw__action_add_row(state, JW_ACTION_ROW_CORE);
        }
        jw__action_add_row(state, JW_ACTION_ROW_PERFORMANCE);
        if (state->action_scrape_pending) {
            jw__action_add_row(state, JW_ACTION_ROW_SCRAPE_CANCEL);
        } else {
            jw__action_add_row(state, JW_ACTION_ROW_SCRAPE);
            jw__action_add_row(state, JW_ACTION_ROW_SCRAPE_ALL);
        }
        jw__action_add_row(state, JW_ACTION_ROW_RESET);
    } else if (state->action_scope == JW_ACTION_GAME) {
        jw__action_add_row(state, JW_ACTION_ROW_DISPLAY_NAME);
        if (state->action_core_count > 1 ||
            state->action_core_game_override[0] ||
            state->action_core_system_override[0]) {
            jw__action_add_row(state, JW_ACTION_ROW_CORE);
        }
        jw__action_add_row(state, JW_ACTION_ROW_PERFORMANCE);
        jw__action_add_row(state, state->action_scrape_pending
                                      ? JW_ACTION_ROW_SCRAPE_CANCEL
                                      : JW_ACTION_ROW_SCRAPE);
        jw__action_add_row(state, JW_ACTION_ROW_RESET);
    }
    cat_list_state_init(&state->action_list, 7);
    cat_list_state_jump(&state->action_list, old_cursor, state->action_row_count);
}

static void jw__action_refresh_core_choices(const char *db_path,
                                            jw_launcher_state *state) {
    (void)db_path;
    if (!state) {
        return;
    }
    state->action_core_count = 0;
    state->action_core_effective[0] = '\0';

    char cores_dir[PATH_MAX];
    if (!jw__runtime_cores_dir(state, cores_dir, sizeof(cores_dir))) {
        return;
    }

    char error[256];
    const jw_ra_catalog *catalog =
        jw_ra_catalog_get(state->sdcard_root, error, sizeof(error));
    if (!catalog) {
        snprintf(state->status, sizeof(state->status), "Core metadata unavailable: %.180s",
                 error[0] ? error : "unknown");
        return;
    }

    const char *system = state->action_scope == JW_ACTION_GAME
        ? state->action_game.system
        : state->action_system;
    (void)jw_ra_catalog_list_system_cores(catalog, system, cores_dir,
                                          state->action_core_choices,
                                          JW_MAX_CORE_CHOICES,
                                          &state->action_core_count);

    const char *preferred = state->action_core_game_override[0]
        ? state->action_core_game_override
        : state->action_core_system_override;
    char diagnostic[256];
    char core_file[PATH_MAX];
    if (jw_ra_catalog_resolve_core_file_for_choice(catalog, system,
                                                   preferred && preferred[0] ? preferred : NULL,
                                                   cores_dir,
                                                   core_file, sizeof(core_file),
                                                   state->action_core_effective,
                                                   sizeof(state->action_core_effective),
                                                   diagnostic, sizeof(diagnostic)) != 0 &&
        state->action_core_count > 0) {
        snprintf(state->action_core_effective,
                 sizeof(state->action_core_effective), "%s",
                 state->action_core_choices[0].id);
    }
}

static void jw__action_refresh_overrides(const char *db_path,
                                         jw_launcher_state *state) {
    if (!state || !db_path) {
        return;
    }
    state->action_core_game_override[0] = '\0';
    state->action_core_system_override[0] = '\0';
    state->action_perf_game_override[0] = '\0';
    state->action_perf_system_override[0] = '\0';
    state->action_system_display_override[0] = '\0';

    const char *system = state->action_scope == JW_ACTION_GAME
        ? state->action_game.system
        : state->action_system;

    if (state->action_scope == JW_ACTION_GAME && state->action_game.id > 0) {
        (void)jw_db_get_game_setting(db_path, state->action_game.id,
                                     JW_CONTENT_SETTING_CORE_ID,
                                     state->action_core_game_override,
                                     sizeof(state->action_core_game_override));
        (void)jw_db_get_game_setting(db_path, state->action_game.id,
                                     JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                     state->action_perf_game_override,
                                     sizeof(state->action_perf_game_override));
    }
    if (system && system[0]) {
        (void)jw_db_get_system_setting(db_path, system,
                                       JW_CONTENT_SETTING_CORE_ID,
                                       state->action_core_system_override,
                                       sizeof(state->action_core_system_override));
        (void)jw_db_get_system_setting(db_path, system,
                                       JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                       state->action_perf_system_override,
                                       sizeof(state->action_perf_system_override));
        (void)jw_db_get_system_setting(db_path, system,
                                       JW_CONTENT_SETTING_DISPLAY_NAME,
                                       state->action_system_display_override,
                                       sizeof(state->action_system_display_override));
    }
}

static void jw__action_refresh_scrape_pending(jw_launcher_state *state) {
    state->action_scrape_pending = false;
    const char *socket_path = state->settings.socket_path;
    if (!socket_path[0]) {
        return;
    }
    bool pending = false;
    const char *system = state->action_scope == JW_ACTION_GAME
        ? state->action_game.system
        : state->action_system;
    const char *rom_path = state->action_scope == JW_ACTION_GAME
        ? state->action_game.rom_path
        : NULL;
    if (jw_ipc_scrape_pending(socket_path, system, rom_path, &pending) == 0) {
        state->action_scrape_pending = pending;
    }
}

static void jw__action_refresh(const char *db_path, jw_launcher_state *state) {
    jw__action_refresh_overrides(db_path, state);
    jw__action_refresh_core_choices(db_path, state);
    jw__action_refresh_scrape_pending(state);
    jw__action_refresh_rows(state);
}

static void jw__open_system_actions(const char *db_path, jw_launcher_state *state,
                                    const char *system, const char *display_name) {
    if (!state || !system || !system[0]) {
        return;
    }
    state->actions_open = true;
    state->action_scope = JW_ACTION_SYSTEM;
    memset(&state->action_game, 0, sizeof(state->action_game));
    snprintf(state->action_system, sizeof(state->action_system), "%s", system);
    jw__system_display_name(db_path, system, state->action_system_display,
                            sizeof(state->action_system_display));
    if (!state->action_system_display[0] && display_name && display_name[0]) {
        snprintf(state->action_system_display, sizeof(state->action_system_display),
                 "%s", display_name);
    }
    jw__action_refresh(db_path, state);
    /* No "Actions: ..." status echo - the name is already the sub-header. */
    state->status[0] = '\0';
}

static void jw__open_game_actions(const char *db_path, jw_launcher_state *state,
                                  const jw_game_entry *game) {
    if (!state || !game || game->id <= 0) {
        return;
    }
    state->actions_open = true;
    state->action_scope = JW_ACTION_GAME;
    state->action_game = *game;
    snprintf(state->action_system, sizeof(state->action_system), "%s", game->system);
    jw__system_display_name(db_path, game->system, state->action_system_display,
                            sizeof(state->action_system_display));
    jw__action_refresh(db_path, state);
    /* No "Actions: ..." status echo - the name is already the sub-header. */
    state->status[0] = '\0';
}

static bool jw__selected_home_game(const jw_launcher_state *state,
                                   const jw_game_entry **out) {
    if (!state || !out) {
        return false;
    }
    *out = NULL;
    if (cat_get_stylesheet()->launcher.layout != CAT_LAUNCHER_TABBED) {
        return false;
    }
    if (state->current_tab == JW_TAB_RECENTS &&
        state->recents_count > 0 &&
        state->list.cursor < state->recents_count) {
        *out = &state->recents[state->list.cursor];
        return true;
    }
    if (state->current_tab == JW_TAB_FAVORITES &&
        state->favorites_count > 0 &&
        state->list.cursor < state->favorites_count) {
        *out = &state->favorites[state->list.cursor];
        return true;
    }
    return false;
}

static bool jw__selected_home_system(const jw_launcher_state *state,
                                     const jw_system_entry **out) {
    if (!state || !out) {
        return false;
    }
    *out = NULL;
    cat_launcher_layout layout = cat_get_stylesheet()->launcher.layout;
    if (layout == CAT_LAUNCHER_TABBED) {
        if (state->current_tab == JW_TAB_GAMES &&
            state->system_count > 0 &&
            state->list.cursor < state->system_count) {
            *out = &state->systems[state->list.cursor];
            return true;
        }
        return false;
    }
    if (state->list.cursor >= state->flat_count) {
        return false;
    }
    const jw_flat_item *it = &state->flat_items[state->list.cursor];
    if (it->kind == JW_FLAT_SYSTEM &&
        it->system_idx >= 0 && it->system_idx < state->system_count) {
        *out = &state->systems[it->system_idx];
        return true;
    }
    return false;
}

static bool jw__open_context_actions(const char *db_path, jw_launcher_state *state) {
    if (!state) {
        return false;
    }
    if (state->games_open &&
        state->game_count > 0 &&
        state->game_list.cursor < state->game_count) {
        jw__open_game_actions(db_path, state, &state->games[state->game_list.cursor]);
        return true;
    }

    const jw_game_entry *game = NULL;
    if (jw__selected_home_game(state, &game) && game) {
        jw__open_game_actions(db_path, state, game);
        return true;
    }

    const jw_system_entry *system = NULL;
    if (jw__selected_home_system(state, &system) && system) {
        jw__open_system_actions(db_path, state, system->name, system->display_name);
        return true;
    }

    return false;
}

static int jw__perform_search(const char *db_path, jw_launcher_state *state,
                              const char *query) {
    snprintf(state->search_query, sizeof(state->search_query), "%s", query ? query : "");
    state->search_count = 0;

    if (jw_db_search_library(db_path, state->search_query, state->search_results,
                             JW_MAX_SEARCH_RESULTS, &state->search_count) != 0) {
        state->search_open = true;
        cat_list_state_init(&state->search_list, jw__search_visible_rows(state));
        snprintf(state->status, sizeof(state->status), "%s", "search failed");
        return -1;
    }

    state->search_open = true;
    cat_list_state_init(&state->search_list, jw__search_visible_rows(state));
    cat_list_state_jump(&state->search_list, 0, state->search_count);
    snprintf(state->status, sizeof(state->status), "%d results", state->search_count);
    return 0;
}

static void jw__open_search(const char *db_path, jw_launcher_state *state) {
    cat_keyboard_result result;
    int rc = cat_keyboard(state->search_query,
                          "Search library\nStart: Confirm\nY: Cancel",
                          CAT_KB_GENERAL, &result);
    if (rc == CAT_OK) {
        jw__perform_search(db_path, state, result.text);
    } else if (rc == CAT_ERROR) {
        snprintf(state->status, sizeof(state->status), "%s", "search keyboard failed");
    }
}

static int jw__open_system_games(const char *db_path, const char *system,
                                 jw_launcher_state *state) {
    char display_name[64];
    jw__system_display_name(db_path, system, display_name, sizeof(display_name));

    if (jw_db_list_games_for_system(db_path, system, state->games, JW_MAX_GAMES,
                                    &state->game_count) != 0 ||
        state->game_count == 0) {
        snprintf(state->status, sizeof(state->status), "No launchable games for %s",
                 display_name[0] ? display_name : system);
        return -1;
    }

    snprintf(state->game_system, sizeof(state->game_system), "%s", system);
    snprintf(state->game_system_display, sizeof(state->game_system_display), "%s",
             display_name[0] ? display_name : system);
    state->games_are_favorites = false;
    state->games_open = true;
    cat_list_state_init(&state->game_list, jw__game_browser_visible_rows(state));
    cat_list_state_jump(&state->game_list, 0, state->game_count);
    snprintf(state->status, sizeof(state->status), "%d %s games",
             state->game_count, state->game_system_display);
    return 0;
}

static int jw__open_favorites(const char *db_path, jw_launcher_state *state) {
    if (jw_db_list_favorite_games(db_path, state->games, JW_MAX_GAMES,
                                  &state->game_count) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Could not load favorites");
        return -1;
    }

    snprintf(state->game_system, sizeof(state->game_system), "%s", "Favorites");
    state->games_are_favorites = true;
    state->games_open = true;
    cat_list_state_init(&state->game_list, jw__game_browser_visible_rows(state));
    cat_list_state_jump(&state->game_list, 0, state->game_count);
    if (state->game_count == 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "No favorites yet — press Y on a game to add one");
    } else {
        snprintf(state->status, sizeof(state->status), "%d favorites", state->game_count);
    }
    return 0;
}

static int jw__open_recents(const char *db_path, jw_launcher_state *state) {
    if (jw_db_list_recent_games(db_path, state->games, JW_MAX_GAMES,
                                &state->game_count) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Could not load recents");
        return -1;
    }

    snprintf(state->game_system, sizeof(state->game_system), "%s", "Recently Played");
    snprintf(state->game_system_display, sizeof(state->game_system_display),
             "%s", "Recently Played");
    state->games_are_favorites = false;
    state->games_open = true;
    cat_list_state_init(&state->game_list, jw__game_browser_visible_rows(state));
    cat_list_state_jump(&state->game_list, 0, state->game_count);
    if (state->game_count == 0) {
        snprintf(state->status, sizeof(state->status), "%s", "No recent games yet");
    } else {
        snprintf(state->status, sizeof(state->status), "%d recent", state->game_count);
    }
    return 0;
}

static void jw__open_apps(jw_launcher_state *state) {
    state->apps_open = true;
    cat_list_state_init(&state->app_list, jw__app_browser_visible_rows(state));
    cat_list_state_jump(&state->app_list, 0, state->app_count);
    if (state->app_count > 0) {
        snprintf(state->status, sizeof(state->status), "%d apps", state->app_count);
    } else {
        snprintf(state->status, sizeof(state->status), "%s",
                 state->scan_ready ? "No apps found" : "Scanning library...");
    }
}

static int jw__launch_app_request(const char *socket_path, const char *name,
                                  const char *pak_dir, jw_launcher_state *state,
                                  bool *running) {
    if (!pak_dir || !pak_dir[0]) {
        snprintf(state->status, sizeof(state->status), "%s", "No app selected");
        return -1;
    }

    jw__set_launching_status(state, name, "app");
    cat_request_frame();
    jw__render_launcher(state);

    if (jw_ipc_launch_app(socket_path, pak_dir, state->status, sizeof(state->status)) != 0) {
        return -1;
    }

    cat_hide_window();
    *running = false;
    return 0;
}

static int jw__launch_app_at(const char *socket_path, jw_launcher_state *state,
                             int cursor, bool *running) {
    if (state->app_count <= 0 || cursor < 0 || cursor >= state->app_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No app selected");
        return -1;
    }

    const jw_app_entry *app = &state->apps[cursor];
    if (!app->pak_dir[0]) {
        snprintf(state->status, sizeof(state->status),
                 "%.200s is a layout sample, not a real app", app->name);
        return -1;
    }
    return jw__launch_app_request(socket_path, app->name, app->pak_dir, state, running);
}

static int jw__launch_game_entry_with_mode(const char *socket_path,
                                           jw_launcher_state *state,
                                           const jw_game_entry *game,
                                           bool switcher_resume,
                                           bool *running) {
    if (!game) {
        snprintf(state->status, sizeof(state->status), "%s", "No game selected");
        return -1;
    }

    jw__set_launching_status(state, game->name, "game");
    cat_request_frame();
    jw__render_launcher(state);

    int rc = switcher_resume
        ? jw_ipc_launch_game_switcher(socket_path, game->system, game->rom_path,
                                      state->status, sizeof(state->status))
        : jw_ipc_launch_game(socket_path, game->system, game->rom_path,
                             state->status, sizeof(state->status));
    if (rc != 0) {
        return -1;
    }

    cat_hide_window();
    *running = false;
    return 0;
}

static int jw__launch_game_entry(const char *socket_path, jw_launcher_state *state,
                                 const jw_game_entry *game, bool *running) {
    return jw__launch_game_entry_with_mode(socket_path, state, game, false, running);
}

static int jw__launch_selected_game(const char *socket_path, jw_launcher_state *state,
                                    bool *running) {
    if (state->game_count <= 0 || state->game_list.cursor >= state->game_count) {
        snprintf(state->status, sizeof(state->status), "%s", "No game selected");
        return -1;
    }
    return jw__launch_game_entry(socket_path, state,
                                 &state->games[state->game_list.cursor], running);
}

static int jw__launch_selected_app(const char *socket_path, jw_launcher_state *state,
                                   bool *running) {
    return jw__launch_app_at(socket_path, state, state->list.cursor, running);
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

    jw__set_launching_status(state, result->name, "game");
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

static void jw__refresh_action_game_from_db(const char *db_path,
                                            jw_launcher_state *state) {
    if (!state || state->action_scope != JW_ACTION_GAME ||
        state->action_game.id <= 0 || !db_path) {
        return;
    }
    jw_game_entry updated;
    if (jw_db_get_game_by_rom_path(db_path, state->action_game.rom_path,
                                   &updated) == 0) {
        state->action_game = updated;
    }
}

static void jw__refresh_action_system_display(const char *db_path,
                                              jw_launcher_state *state) {
    if (!state || !db_path || !state->action_system[0]) {
        return;
    }
    jw__system_display_name(db_path, state->action_system,
                            state->action_system_display,
                            sizeof(state->action_system_display));
    if (state->games_open && strcmp(state->game_system, state->action_system) == 0) {
        snprintf(state->game_system_display, sizeof(state->game_system_display), "%s",
                 state->action_system_display[0]
                     ? state->action_system_display
                     : state->action_system);
    }
}

static void jw__refresh_after_action_write(const char *db_path,
                                           jw_launcher_state *state) {
    if (!state) {
        return;
    }
    (void)jw__reload_library_from_db(db_path, state);
    jw__refresh_action_game_from_db(db_path, state);
    jw__refresh_action_system_display(db_path, state);
    jw__action_refresh(db_path, state);
}

static void jw__cycle_action_core(const char *db_path, jw_launcher_state *state,
                                  int direction) {
    if (!state || state->action_core_count == 0) {
        return;
    }
    int idx = jw__action_find_core(state, state->action_core_effective);
    if (idx < 0) idx = 0;
    int next = (idx + direction + (int)state->action_core_count) %
               (int)state->action_core_count;
    const char *next_id = state->action_core_choices[next].id;

    int rc;
    if (state->action_scope == JW_ACTION_GAME) {
        rc = jw_db_set_game_setting(db_path, state->action_game.id,
                                    JW_CONTENT_SETTING_CORE_ID, next_id);
    } else {
        rc = jw_db_set_system_setting(db_path, state->action_system,
                                      JW_CONTENT_SETTING_CORE_ID, next_id);
    }
    if (rc == 0) {
        snprintf(state->status, sizeof(state->status), "Core: %.160s",
                 jw__action_core_label(state, next_id));
        jw__refresh_after_action_write(db_path, state);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", "Core update failed");
    }
}

static void jw__cycle_action_performance(const char *db_path,
                                         jw_launcher_state *state,
                                         int direction) {
    if (!state) {
        return;
    }
    const char *current = "";
    if (state->action_scope == JW_ACTION_GAME &&
        state->action_perf_game_override[0]) {
        current = state->action_perf_game_override;
    } else if (state->action_perf_system_override[0]) {
        current = state->action_perf_system_override;
    }
    int idx = jw__action_perf_index(current);
    int next = (idx + direction + JW_ACTION_PERF_COUNT) % JW_ACTION_PERF_COUNT;
    const char *next_name = jw_platform_perf_profile_name(kActionPerfProfiles[next]);

    int rc;
    if (state->action_scope == JW_ACTION_GAME) {
        rc = jw_db_set_game_setting(db_path, state->action_game.id,
                                    JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                    next_name);
    } else {
        rc = jw_db_set_system_setting(db_path, state->action_system,
                                      JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                      next_name);
    }
    if (rc == 0) {
        snprintf(state->status, sizeof(state->status), "Performance: %.160s",
                 jw_platform_perf_profile_label(kActionPerfProfiles[next]));
        jw__refresh_after_action_write(db_path, state);
    } else {
        snprintf(state->status, sizeof(state->status), "%s",
                 "Performance update failed");
    }
}

static void jw__edit_action_display_name(const char *db_path,
                                         jw_launcher_state *state) {
    if (!state ||
        (state->action_scope == JW_ACTION_GAME && state->action_game.id <= 0) ||
        (state->action_scope == JW_ACTION_SYSTEM && !state->action_system[0]) ||
        state->action_scope == JW_ACTION_NONE) {
        return;
    }

    const char *current = state->action_scope == JW_ACTION_SYSTEM
        ? state->action_system_display
        : state->action_game.name;
    cat_keyboard_result result;
    int rc = cat_keyboard(current,
                          "Start: Confirm\nY: Cancel\nLeave empty to reset",
                          CAT_KB_GENERAL, &result);
    if (rc == CAT_OK) {
        if (result.text[0]) {
            int write_rc = state->action_scope == JW_ACTION_SYSTEM
                ? jw_db_set_system_setting(db_path, state->action_system,
                                           JW_CONTENT_SETTING_DISPLAY_NAME,
                                           result.text)
                : jw_db_set_game_setting(db_path, state->action_game.id,
                                         JW_CONTENT_SETTING_DISPLAY_NAME,
                                         result.text);
            if (write_rc != 0) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Display name update failed");
                return;
            }
            snprintf(state->status, sizeof(state->status), "Display Name: %.160s",
                     result.text);
        } else {
            int delete_rc = state->action_scope == JW_ACTION_SYSTEM
                ? jw_db_delete_system_setting(db_path, state->action_system,
                                              JW_CONTENT_SETTING_DISPLAY_NAME)
                : jw_db_delete_game_setting(db_path, state->action_game.id,
                                            JW_CONTENT_SETTING_DISPLAY_NAME);
            if (delete_rc != 0) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Display name reset failed");
                return;
            }
            snprintf(state->status, sizeof(state->status), "%s",
                     "Display name reset");
        }
        jw__refresh_after_action_write(db_path, state);
    } else if (rc == CAT_ERROR) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "display name keyboard failed");
    }
}

static void jw__reset_action_overrides(const char *db_path,
                                       jw_launcher_state *state) {
    if (!state) {
        return;
    }
    int rc = 0;
    if (state->action_scope == JW_ACTION_GAME) {
        rc |= jw_db_delete_game_setting(db_path, state->action_game.id,
                                        JW_CONTENT_SETTING_CORE_ID);
        rc |= jw_db_delete_game_setting(db_path, state->action_game.id,
                                        JW_CONTENT_SETTING_PERFORMANCE_PROFILE);
        rc |= jw_db_delete_game_setting(db_path, state->action_game.id,
                                        JW_CONTENT_SETTING_DISPLAY_NAME);
    } else if (state->action_scope == JW_ACTION_SYSTEM) {
        rc |= jw_db_delete_system_setting(db_path, state->action_system,
                                          JW_CONTENT_SETTING_CORE_ID);
        rc |= jw_db_delete_system_setting(db_path, state->action_system,
                                          JW_CONTENT_SETTING_PERFORMANCE_PROFILE);
        rc |= jw_db_delete_system_setting(db_path, state->action_system,
                                          JW_CONTENT_SETTING_DISPLAY_NAME);
    }
    if (rc == 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Overrides reset");
        jw__refresh_after_action_write(db_path, state);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", "Reset failed");
    }
}

static void jw__start_action_scrape(const char *socket_path, const char *db_path,
                                    jw_launcher_state *state, bool missing_only) {
    bool is_game = state->action_scope == JW_ACTION_GAME;
    int enqueued = 0;
    char status[256] = "";
    int rc = jw_ipc_scrape_start(socket_path,
                                 is_game ? "game" : "system",
                                 is_game ? state->action_game.system
                                         : state->action_system,
                                 is_game ? state->action_game.rom_path : NULL,
                                 missing_only, &enqueued,
                                 status, sizeof(status));
    if (rc != 0) {
        snprintf(state->status, sizeof(state->status), "Scrape failed: %.180s",
                 status[0] ? status : "daemon unavailable");
        return;
    }
    if (is_game) {
        char name[256];
        jw__clean_rom_name(state->action_game.name, name, sizeof(name));
        snprintf(state->status, sizeof(state->status),
                 "Scraping artwork: %.180s", name);
    } else if (enqueued == 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 missing_only ? "Nothing to scrape - all games have artwork"
                              : "Nothing to scrape - no games in this system");
    } else {
        snprintf(state->status, sizeof(state->status),
                 "Scraping %d game%s in %.140s", enqueued,
                 enqueued == 1 ? "" : "s", state->action_system_display);
    }
    jw__action_refresh(db_path, state);
}

static void jw__cancel_action_scrape(const char *socket_path, const char *db_path,
                                     jw_launcher_state *state) {
    bool is_game = state->action_scope == JW_ACTION_GAME;
    int removed = 0;
    if (jw_ipc_scrape_cancel(socket_path,
                             is_game ? "game" : "system",
                             is_game ? state->action_game.system
                                     : state->action_system,
                             is_game ? state->action_game.rom_path : NULL,
                             &removed) != 0) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "Cancel failed: daemon unavailable");
        return;
    }
    snprintf(state->status, sizeof(state->status),
             "Scraping cancelled (%d item%s dropped)", removed,
             removed == 1 ? "" : "s");
    jw__action_refresh(db_path, state);
}

static void jw__select_action_row(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state, bool *running) {
    if (!state || state->action_list.cursor < 0 ||
        state->action_list.cursor >= state->action_row_count) {
        return;
    }
    jw_action_row_kind row = state->action_rows[state->action_list.cursor];
    switch (row) {
        case JW_ACTION_ROW_SEARCH:
            state->actions_open = false;
            jw__open_search(db_path, state);
            break;
        case JW_ACTION_ROW_DISPLAY_NAME:
            jw__edit_action_display_name(db_path, state);
            break;
        case JW_ACTION_ROW_CORE:
            jw__cycle_action_core(db_path, state, +1);
            break;
        case JW_ACTION_ROW_PERFORMANCE:
            jw__cycle_action_performance(db_path, state, +1);
            break;
        case JW_ACTION_ROW_SCRAPE:
            jw__start_action_scrape(socket_path, db_path, state,
                                    state->action_scope == JW_ACTION_SYSTEM);
            break;
        case JW_ACTION_ROW_SCRAPE_ALL:
            jw__start_action_scrape(socket_path, db_path, state, false);
            break;
        case JW_ACTION_ROW_SCRAPE_CANCEL:
            jw__cancel_action_scrape(socket_path, db_path, state);
            break;
        case JW_ACTION_ROW_RESET:
            jw__reset_action_overrides(db_path, state);
            break;
        default:
            break;
    }
}

static void jw__handle_actions_input(const char *socket_path, const char *db_path,
                                     jw_launcher_state *state,
                                     cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->action_list, -1, state->action_row_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->action_list, +1, state->action_row_count);
            break;
        case CAT_BTN_LEFT:
        case CAT_BTN_RIGHT:
            if (state->action_list.cursor >= 0 &&
                state->action_list.cursor < state->action_row_count) {
                jw_action_row_kind row = state->action_rows[state->action_list.cursor];
                int dir = button == CAT_BTN_LEFT ? -1 : +1;
                if (row == JW_ACTION_ROW_CORE) {
                    jw__cycle_action_core(db_path, state, dir);
                } else if (row == JW_ACTION_ROW_PERFORMANCE) {
                    jw__cycle_action_performance(db_path, state, dir);
                }
            }
            break;
        case CAT_BTN_A:
            jw__select_action_row(socket_path, db_path, state, running);
            break;
        case CAT_BTN_B:
            state->actions_open = false;
            state->action_scope = JW_ACTION_NONE;
            state->status[0] = '\0';
            break;
        case CAT_BTN_L1:
        case CAT_BTN_R1:
            /* The actions header shows the section tabs, so L1/R1 tabs away —
               closing the actions view and landing on the adjacent section,
               like the game browser and search. Tabbed layout only. */
            if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED) {
                state->actions_open = false;
                state->action_scope = JW_ACTION_NONE;
                state->status[0] = '\0';
                jw__switch_tab(state, button == CAT_BTN_L1 ? -1 : +1, db_path);
            }
            break;
        case CAT_BTN_MENU:
            if (jw_ipc_open_menu(socket_path) == 0) {
                cat_hide_window();
                *running = false;
            } else {
                snprintf(state->status, sizeof(state->status), "%s",
                         "open-menu failed");
            }
            break;
        default:
            break;
    }
}

static void jw__activate_tabbed(const char *socket_path, const char *db_path,
                                  jw_launcher_state *state, bool *running) {
    switch (state->current_tab) {
        case JW_TAB_RECENTS:
            if (state->recents_count > 0 && state->list.cursor < state->recents_count) {
                /* Recents resumes: the daemon loads the newest state (preferring
                   the game-switcher slot) when one exists and cold-launches
                   otherwise, so no per-entry state probe is needed here. */
                jw__launch_game_entry_with_mode(socket_path, state,
                                                &state->recents[state->list.cursor],
                                                true, running);
            }
            break;
        case JW_TAB_FAVORITES:
            if (state->favorites_count > 0 && state->list.cursor < state->favorites_count) {
                jw__launch_game_entry(socket_path, state,
                                      &state->favorites[state->list.cursor], running);
            }
            break;
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
        case JW_FLAT_FAVORITES:
            jw__open_favorites(db_path, state);
            break;
        case JW_FLAT_RECENTLY_PLAYED:
            jw__open_recents(db_path, state);
            break;
        case JW_FLAT_APPS:
            jw__open_apps(state);
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

/* Rebuild layout-dependent state. Call after the active stylesheet's
 * launcher.layout may have changed (theme switch) or at first startup. */
static void jw__rebuild_for_layout(jw_launcher_state *state) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;

    state->tools_open = false;
    state->apps_open = false;
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

    int fh         = jw__footer_height(state);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h     = TTF_FontHeight(body) + CAT_S(12);
    int visible;
    if (layout == CAT_LAUNCHER_TABBED) {
        /* Tab pages use the shared box model with a tab-bar-only header (no
           sub-header), so the row count matches the renderer exactly. */
        visible = jw__browse_visible_rows(state, cat_get_tab_bar_height());
    } else {
        int sb_h      = CAT_DS(20);
        int margin    = CAT_S(10);
        int content_h = cat_get_screen_height() - sb_h - margin - fh - margin;
        visible = content_h / item_h;
    }
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
        case CAT_BTN_L1:
        case CAT_BTN_R1:
            /* The search header shows the section tabs, so L1/R1 tabs away —
               closing search and landing on the adjacent section, like the game
               browser. Tabbed layout only. */
            if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED) {
                state->search_open = false;
                state->status[0] = '\0';
                jw__switch_tab(state, button == CAT_BTN_L1 ? -1 : +1, db_path);
            }
            break;
        default:
            break;
    }
}

static void jw__toggle_favorite_selected(const char *db_path, jw_launcher_state *state) {
    if (state->game_count <= 0 || state->game_list.cursor >= state->game_count) {
        return;
    }
    jw_game_entry *game = &state->games[state->game_list.cursor];
    int want_on = !game->favorite;

    if (jw_db_set_favorite(db_path, "game", game->id, want_on) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Favorite update failed");
        return;
    }
    game->favorite = want_on;

    /* When viewing the Favorites list, an unfavorite must drop the row, so
       reload the list and keep the cursor near its prior position. */
    if (state->games_are_favorites && !want_on) {
        int prev_cursor = state->game_list.cursor;
        jw__open_favorites(db_path, state);
        if (state->game_count > 0) {
            int c = prev_cursor >= state->game_count ? state->game_count - 1 : prev_cursor;
            cat_list_state_jump(&state->game_list, c, state->game_count);
        }
        return;
    }

    /* Bound the name so the prefix + name always fit the status buffer; the
       line is ellipsized on screen anyway. */
    snprintf(state->status, sizeof(state->status), "%s %.200s",
             want_on ? "Favorited" : "Unfavorited", game->name);
}

/* Drop the selected game from the Recents tab's play-history and reload the list,
   keeping the cursor near its prior spot. The game and any favorite are untouched
   — it just leaves Recents. */
static void jw__remove_selected_recent(const char *db_path, jw_launcher_state *state) {
    if (state->recents_count <= 0 || state->list.cursor >= state->recents_count) {
        return;
    }
    const jw_game_entry *rec = &state->recents[state->list.cursor];
    int   rec_id = rec->id;
    char  removed_name[256];
    snprintf(removed_name, sizeof(removed_name), "%.200s", rec->name);

    if (jw_db_remove_recent(db_path, "game", rec_id) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Remove failed");
        return;
    }

    int prev_cursor = state->list.cursor;
    jw__load_recents_tab(db_path, state);
    int c = prev_cursor >= state->recents_count ? state->recents_count - 1 : prev_cursor;
    if (c < 0) c = 0;
    cat_list_state_jump(&state->list, c, jw__tab_list_count(state));
    snprintf(state->status, sizeof(state->status), "Removed %.200s", removed_name);
}

static void jw__handle_game_browser_input(const char *socket_path, const char *db_path,
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
        case CAT_BTN_Y:
            jw__toggle_favorite_selected(db_path, state);
            break;
        case CAT_BTN_B:
            state->games_open = false;
            state->games_are_favorites = false;
            state->game_count = 0;
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

static void jw__handle_app_browser_input(const char *socket_path,
                                         jw_launcher_state *state,
                                         cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->app_list, -1, state->app_count);
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->app_list, +1, state->app_count);
            break;
        case CAT_BTN_LEFT:
            cat_list_state_page(&state->app_list, -1, state->app_count);
            break;
        case CAT_BTN_RIGHT:
            cat_list_state_page(&state->app_list, +1, state->app_count);
            break;
        case CAT_BTN_A:
            jw__launch_app_at(socket_path, state, state->app_list.cursor, running);
            break;
        case CAT_BTN_B:
            state->apps_open = false;
            state->status[0] = '\0';
            break;
        default:
            break;
    }
}

/* Open the Select switcher: a fresh load of Recents in carousel form. The
   underlying layout state is untouched, so closing just clears the flag. */
static void jw__open_switcher(const char *db_path, jw_launcher_state *state) {
    /* RetroArch States/ root for savestate thumbnails: prefer the explicit
       STATES_PATH from env.sh, else the default <sdcard>/States layout. */
    const char *states = getenv("STATES_PATH");
    char states_buf[PATH_MAX + 16];
    if (!states || !states[0]) {
        snprintf(states_buf, sizeof(states_buf), "%s/States", state->sdcard_root);
        states = states_buf;
    }
    jw_game_switcher_reset(&state->switcher, false, state->sdcard_root, states);
    jw_game_switcher_load(&state->switcher, db_path);
    jw_game_switcher_resolve_thumbnails(&state->switcher);
    state->switcher_open = true;
    state->status[0] = '\0';
}

/* Y in the switcher: drop the selected game from Recents only (id, artwork,
   favorite, and the game itself are untouched), then refresh both the carousel
   and the Recents tab list so they stay consistent. */
static void jw__switcher_remove_selected(const char *db_path, jw_launcher_state *state) {
    const jw_game_entry *sel = jw_game_switcher_selected(&state->switcher);
    if (!sel || sel->id < 0) {
        return;
    }
    char removed_name[256];
    snprintf(removed_name, sizeof(removed_name), "%.200s", sel->name);

    if (jw_db_remove_recent(db_path, "game", sel->id) != 0) {
        snprintf(state->status, sizeof(state->status), "%s", "Remove failed");
        return;
    }

    jw_game_switcher_remove_selected(&state->switcher);

    /* Keep the Recents tab in sync (it also reloads on tab entry). */
    jw__load_recents_tab(db_path, state);
    if (state->current_tab == JW_TAB_RECENTS) {
        int c = state->list.cursor;
        if (c >= state->recents_count) {
            c = state->recents_count > 0 ? state->recents_count - 1 : 0;
        }
        cat_list_state_jump(&state->list, c, jw__tab_list_count(state));
    }

    snprintf(state->status, sizeof(state->status), "Removed %.200s", removed_name);
}

static void jw__handle_switcher_input(const char *socket_path, const char *db_path,
                                      jw_launcher_state *state,
                                      cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_LEFT:
        case CAT_BTN_UP:        /* alias Up/Down to the carousel for desktop ease */
            jw_game_switcher_move(&state->switcher, -1);
            break;
        case CAT_BTN_RIGHT:
        case CAT_BTN_DOWN:
            jw_game_switcher_move(&state->switcher, +1);
            break;
        case CAT_BTN_A: {
            const jw_game_entry *sel = jw_game_switcher_selected(&state->switcher);
            if (sel) {
                state->switcher_open = false;
                jw__launch_game_entry_with_mode(socket_path, state, sel, true, running);
            }
            break;
        }
        case CAT_BTN_Y:
            jw__switcher_remove_selected(db_path, state);
            break;
        case CAT_BTN_B:
            state->switcher_open = false;
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

    /* Analog-stick click is a global shortcut: toggle the LED ring on/off. */
    if (button == CAT_BTN_STICK) {
        jw_settings_toggle_led(&state->settings);
        return;
    }

    /* The switcher overlay captures all input while open. */
    if (state->switcher_open) {
        jw__handle_switcher_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->actions_open) {
        jw__handle_actions_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->search_open) {
        jw__handle_search_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->games_open) {
        /* In the tabbed layout the section tabs sit above the game list, so
           L1/R1 tabs away from the system — closing the browser and landing on
           the adjacent section, exactly as on the tabbed home view. */
        if (layout == CAT_LAUNCHER_TABBED &&
            (button == CAT_BTN_L1 || button == CAT_BTN_R1)) {
            bool back = (button == CAT_BTN_L1);
            state->games_open = false;
            state->games_are_favorites = false;
            state->game_count = 0;
            state->status[0] = '\0';
            jw__switch_tab(state, back ? -1 : +1, db_path);
            return;
        }
        if (button == CAT_BTN_X) {
            if (!jw__open_context_actions(db_path, state)) {
                jw__open_search(db_path, state);
            }
            return;
        }
        jw__handle_game_browser_input(socket_path, db_path, state, button, running);
        return;
    }

    if (state->apps_open) {
        if (button == CAT_BTN_X) {
            jw__open_search(db_path, state);
            return;
        }
        jw__handle_app_browser_input(socket_path, state, button, running);
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
                if (state->tools_list.cursor == 2) {
                    jw__open_apps(state);
                } else if (state->tools_list.cursor == 3) {
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
        /* MENU always opens the main menu, from any settings sub-screen and
           any layout — it is a global action, not consumed by settings. */
        if (button == CAT_BTN_MENU) {
            if (jw_ipc_open_menu(socket_path) == 0) {
                cat_hide_window();
                *running = false;
            } else {
                snprintf(state->status, sizeof(state->status), "%s", "open-menu failed");
            }
            return;
        }
        /* Tabbed mode: Settings is a tab, not an app. Triggers must escape
           it cleanly from any sub-screen, and B at Settings home is a no-op
           (the user leaves via L1/R1). jw__switch_tab closes Settings as a
           side effect when moving off the tab. */
        if (layout == CAT_LAUNCHER_TABBED) {
            if (button == CAT_BTN_L1) {
                jw__switch_tab(state, -1, db_path);
                return;
            }
            if (button == CAT_BTN_R1) {
                jw__switch_tab(state, +1, db_path);
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
        if (!jw__open_context_actions(db_path, state)) {
            jw__open_search(db_path, state);
        }
        return;
    }

    /* Select opens the dedicated switcher from any home layout/tab (desktop:
       Space). It is launcher-local; the daemon stays out of it. */
    if (button == CAT_BTN_SELECT) {
        jw__open_switcher(db_path, state);
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
        case CAT_BTN_L1:   /* L1/R1 tab between sections (L2/R2 reserved for future use) */
            if (layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab(state, -1, db_path);
            break;
        case CAT_BTN_R1:
            if (layout == CAT_LAUNCHER_TABBED)
                jw__switch_tab(state, +1, db_path);
            break;
        case CAT_BTN_A:
            if (layout == CAT_LAUNCHER_TABBED)
                jw__activate_tabbed(socket_path, db_path, state, running);
            else
                jw__activate_flat(socket_path, db_path, state, running);
            break;
        case CAT_BTN_B:
            /* B is unused on the home tabs (no "back" at the top level). On the
               Recents tab it removes the selected entry. NOTE: this is a TEMPORARY
               mapping — B is normally back/cancel, so the remove-from-recents
               button is flagged for team review (see umrk-workspace plans). */
            if (layout == CAT_LAUNCHER_TABBED && state->current_tab == JW_TAB_RECENTS)
                jw__remove_selected_recent(db_path, state);
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
            /* On the Recents tab, Y toggles the favorite of the selected game
               (the row's star updates in place); on the Favorites tab, Y removes
               the selected favorite and reloads the list; everywhere else Y
               rescans the library. */
            if (layout == CAT_LAUNCHER_TABBED && state->current_tab == JW_TAB_RECENTS) {
                if (state->recents_count > 0 && state->list.cursor < state->recents_count) {
                    jw_game_entry *rec = &state->recents[state->list.cursor];
                    int want_on = !rec->favorite;
                    if (jw_db_set_favorite(db_path, "game", rec->id, want_on) == 0) {
                        rec->favorite = want_on;
                        snprintf(state->status, sizeof(state->status), "%s %.200s",
                                 want_on ? "Favorited" : "Unfavorited", rec->name);
                    } else {
                        snprintf(state->status, sizeof(state->status), "%s",
                                 "Favorite update failed");
                    }
                }
                break;
            }
            if (layout == CAT_LAUNCHER_TABBED && state->current_tab == JW_TAB_FAVORITES) {
                if (state->favorites_count > 0 && state->list.cursor < state->favorites_count) {
                    const jw_game_entry *fav = &state->favorites[state->list.cursor];
                    if (jw_db_set_favorite(db_path, "game", fav->id, 0) == 0) {
                        int prev_cursor = state->list.cursor;
                        jw__load_favorites_tab(db_path, state);
                        int c = prev_cursor >= state->favorites_count
                                    ? state->favorites_count - 1 : prev_cursor;
                        if (c < 0) c = 0;
                        cat_list_state_jump(&state->list, c, jw__tab_list_count(state));
                        snprintf(state->status, sizeof(state->status), "%s",
                                 "Removed from favorites");
                    } else {
                        snprintf(state->status, sizeof(state->status), "%s",
                                 "Favorite update failed");
                    }
                }
                break;
            }
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
    long long process_start_ms = jw__monotonic_ms();
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

    long long hello_start_ms = jw__monotonic_ms();
    if (jw_ipc_hello(socket_path, "launcher") != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?",
                     socket_path);
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }
    long long hello_done_ms = jw__monotonic_ms();

    cat_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.window_title       = "Jawaka Launcher";
    cfg.disable_background = true;
    /* The joystick scan costs ~200ms on MLP1; run it after the first frame is
       on screen (below, right after frontend-ready) instead of before it. */
    cfg.defer_input_init   = true;

    long long cat_start_ms = jw__monotonic_ms();
    if (cat_init(&cfg) != CAT_OK) {
        jw_log_error("catastrophe init failed: %s", cat_get_error());
        free(socket_path);
        free(db_path);
        free(sdcard_root);
        return 1;
    }
    jw_cat_services_install(socket_path);
    long long cat_done_ms = jw__monotonic_ms();

    /* Resolve theme: env > DB > default Jawaka-Tabs */
    long long theme_start_ms = jw__monotonic_ms();
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
    long long theme_done_ms = jw__monotonic_ms();

    cat_activate_window();

    jw_launcher_state state;
    memset(&state, 0, sizeof(state));
    state.library_generation = -1;
    /* Honor the persisted startup tab (Settings > Behavior > Startup Tab);
       default Games. Index mirrors the jw_tab enum. */
    state.current_tab = JW_TAB_GAMES;
    {
        char startup_buf[16];
        if (jw_db_get_setting(db_path, "startup_tab_index", startup_buf,
                              sizeof(startup_buf)) == 0 && startup_buf[0]) {
            int idx = atoi(startup_buf);
            if (idx >= 0 && idx < JW_TAB_COUNT)
                state.current_tab = (jw_tab)idx;
        }
    }
    snprintf(state.sdcard_root, sizeof(state.sdcard_root), "%s", sdcard_root);
    snprintf(state.status, sizeof(state.status), "%s", "loading library...");

    long long cache_start_ms = jw__monotonic_ms();
    if (jw__load_library_cache(socket_path, db_path, &state) != 0) {
        snprintf(state.status, sizeof(state.status), "%s", "scanning library...");
        jw__scan_library(socket_path, db_path, &state);
    }
    long long cache_done_ms = jw__monotonic_ms();

    const cat_stylesheet *ss = cat_get_stylesheet();
    cat_launcher_layout layout = ss->launcher.layout;
    const char *layout_name = (layout == CAT_LAUNCHER_VERTICAL)   ? "vertical"
                            : (layout == CAT_LAUNCHER_HORIZONTAL) ? "horizontal"
                            : (layout == CAT_LAUNCHER_COVERFLOW)  ? "coverflow"
                            : "tabbed";
    jw_log_info("launcher layout: %s (theme=%s)", layout_name, theme_name);

    /* Init settings UI with the currently-active theme */
    long long settings_start_ms = jw__monotonic_ms();
    jw_settings_ui_init(&state.settings, db_path, theme_name, socket_path);
    long long settings_done_ms = jw__monotonic_ms();

    /* Prime the startup tab's lazily-loaded contents so the first frame is
       correct (Favorites/Recents are normally loaded on tab entry, and the
       Settings tab is owned by jw_settings_ui). Tabbed layout only — other
       layouts use flat/carousel lists, not per-tab state. */
    if (cat_get_stylesheet()->launcher.layout == CAT_LAUNCHER_TABBED) {
        if (state.current_tab == JW_TAB_FAVORITES)
            jw__load_favorites_tab(db_path, &state);
        else if (state.current_tab == JW_TAB_RECENTS)
            jw__load_recents_tab(db_path, &state);
        else if (state.current_tab == JW_TAB_SETTINGS)
            jw_settings_ui_enter(&state.settings);
    }

    jw__rebuild_for_layout(&state);

    jw_autodemo demo;
    jw_autodemo_init(&demo);
    bool running = true;

    long long first_frame_start_ms = jw__monotonic_ms();
    cat_request_frame();
    jw__render_launcher(&state);
    long long first_frame_done_ms = jw__monotonic_ms();

    /* First frame is on screen; jawakad owns any platform-specific readiness
     * side effects such as dismissing the MLP1 stock boot transition. */
    long long ready_start_ms = jw__monotonic_ms();
    if (jw_ipc_frontend_ready(socket_path, "launcher") != 0) {
        jw_log_warn("frontend-ready notification failed");
    }
    long long ready_done_ms = jw__monotonic_ms();
    cat_init_input();
    jw_log_info("launcher startup timings: hello_ms=%lld cat_ms=%lld theme_ms=%lld cache_ms=%lld settings_ms=%lld first_frame_ms=%lld ready_ms=%lld total_ms=%lld",
                hello_done_ms - hello_start_ms,
                cat_done_ms - cat_start_ms,
                theme_done_ms - theme_start_ms,
                cache_done_ms - cache_start_ms,
                settings_done_ms - settings_start_ms,
                first_frame_done_ms - first_frame_start_ms,
                ready_done_ms - ready_start_ms,
                ready_done_ms - process_start_ms);

    /* Move the status-bar/library polls off the render thread (see jw__status_poller). */
    jw__status_poller_start(socket_path, &state.settings);

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

        /* While the Display & Sound page is open, poll live brightness/volume so
           the sliders follow the hardware volume keys (jawakad's input proxy
           consumes those events, so the UI can only observe the result). */
        if (jw_settings_ui_wants_av_poll(&state.settings)) {
            jw_settings_ui_refresh_av(&state.settings);
            cat_request_frame_in(300);
        }

        /* While the Network page is open, keep the Wi-Fi status live (the refresh
           self-throttles so platform Wi-Fi is not polled every frame). */
        if (jw_settings_ui_wants_wifi_poll(&state.settings)) {
            jw_settings_ui_refresh_wifi(&state.settings);
            cat_request_frame_in(2000);
        }

        if (jw_settings_ui_wants_bluetooth_poll(&state.settings)) {
            jw_settings_ui_refresh_bluetooth(&state.settings);
            cat_request_frame_in(250);
        }

        if (jw_settings_ui_wants_update_poll(&state.settings)) {
            jw_settings_ui_refresh_update(&state.settings);
            cat_request_frame_in(500);
        }

        /* The library-generation IPC and the status-bar volume/Wi-Fi/Bluetooth
           reads all run on the background status poller now (jw__status_poller);
           the sync publishes what to poll and folds delivered samples into the
           settings/status-bar caches, and the generation poll adopts the latest
           generation. Keeping the polls off the render thread is what removes
           the ~1s hitch in continuous animations. */
        jw__status_poller_sync(&state.settings);
        jw__poll_library_generation(socket_path, db_path, &state);
        jw__poll_scrape_status(&state);

        /* Keep the status bar live while idle: the launcher only renders on
           input or a requested frame, so without this a wifi connect / charger
           plug-in / clock tick wouldn't show until the next button press. ~1s
           is responsive and cheap (the earliest pending frame request wins, so
           shorter ticks above still fire sooner). */
        cat_request_frame_in(1000);

        jw__render_launcher(&state);
    }

    jw__status_poller_shutdown();
    jw__cover_loader_shutdown();
    cat_quit();
    free(socket_path);
    free(db_path);
    free(sdcard_root);
    return 0;
}
