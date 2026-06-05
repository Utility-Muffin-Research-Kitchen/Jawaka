#include "internal/settings/settings.h"

#include "internal/db/db.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/device.h"
#include "internal/platform/platform_id.h"
#include "internal/settings/appearance.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Data tables ──────────────────────────────────────────────────────── */

const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT] = {
    "Jawaka-Tabs",
    "Jawaka-Vertical",
    "Jawaka-Horizontal",
    "Jawaka-Coverflow",
};

/* Focus-on-Tabs: only Jawaka-Tabs is an actively-supported layout. The others
   stay in the build but cannot be switched to from the theme picker — the
   cycler skips them and jw__apply_theme refuses them. */
const bool kJawakaThemeEnabled[JW_SETTINGS_THEME_COUNT] = {
    true,   /* Jawaka-Tabs */
    false,  /* Jawaka-Vertical */
    false,  /* Jawaka-Horizontal */
    false,  /* Jawaka-Coverflow */
};

const char *const kPillShapeLabels[JW_SETTINGS_PILL_SHAPE_COUNT] = {
    "Rounded",
    "Soft",
    "Square",
    "Leaf",
};

const char *const kFontSizeLabels[JW_SETTINGS_FONT_SIZE_COUNT] = {
    "Small",
    "Default",
    "Large",
    "Extra Large",
};

/* The value tables backing these labels (pill radius, corner mask, font bump)
   are the canonical ones in appearance.c, shared with the daemon's env export.
   Keep the label row counts locked to them. */
_Static_assert(JW_SETTINGS_PILL_SHAPE_COUNT == JW_APPEARANCE_PILL_SHAPE_COUNT,
               "pill shape label/value table counts out of sync");
_Static_assert(JW_SETTINGS_FONT_SIZE_COUNT == JW_APPEARANCE_FONT_SIZE_COUNT,
               "font size label/value table counts out of sync");

const char *const kClockStyleLabels[JW_SETTINGS_CLOCK_STYLE_COUNT] = {
    "Off",
    "24 Hour",
    "AM/PM",
    "12 Hour",
};

/* Curated color schemes selectable from the Appearance > Color Scheme row. Each
   sets the seven color roles at once. Order: accent (chrome bars), background,
   text, hint, selection (selected-row pill), button label, button glyph bg.
   Selected-row text auto-contrasts against the selection pill at apply time. */
typedef struct {
    const char *name;
    const char *accent, *bg, *text, *hint, *selection, *btn_label, *btn_bg;
} jw__color_scheme;

static const jw__color_scheme kColorSchemes[] = {
    /* name      accent     bg         text       hint       selection  btn_label  btn_bg     */
    { "Leaf",    "#1E331E", "#0F160E", "#E8F1E3", "#7E9579", "#7FB069", "#0F160E", "#7FB069" },
    { "Aurora",  "#173342", "#0E1822", "#E6F0F2", "#6E8898", "#3DDC97", "#0E1822", "#3DDC97" },
    { "Ember",   "#3A2A22", "#1A1413", "#F2EAE2", "#A38A7A", "#FF8A4C", "#1A1413", "#FF8A4C" },
    { "Orchid",  "#2E2240", "#181226", "#ECE4F2", "#8E7CB0", "#C792EA", "#181226", "#C792EA" },
    { "Slate",   "#242A36", "#14171E", "#E4E7ED", "#7C828E", "#7AA2F7", "#14171E", "#7AA2F7" },
    { "Rosé",    "#33222E", "#1C1620", "#F0E6EC", "#A88A98", "#EB6F92", "#1C1620", "#EB6F92" },
};
#define JW_COLOR_SCHEME_COUNT ((int)(sizeof(kColorSchemes) / sizeof(kColorSchemes[0])))
#define JW_COLOR_SCHEME_DEFAULT 0   /* Leaf — the Dweezil/Leaf identity theme */

static void jw__apply_color_scheme(jw_settings_ui *ui, int index, bool *theme_changed);

/* Startup-tab options for Settings > Behavior. Order and index MIRROR the
   launcher's jw_tab enum so the persisted "startup_tab_index" maps 1:1 to the
   tab the launcher opens on boot. */
static const char *kStartupTabLabels[] = {
    "Recents", "Favorites", "Games", "Apps", "Settings",
};
#define JW_STARTUP_TAB_COUNT ((int)(sizeof(kStartupTabLabels) / sizeof(kStartupTabLabels[0])))
#define JW_STARTUP_TAB_DEFAULT 2   /* Games */

/* Auto-sleep options for Settings > Behavior. The label index is persisted as
   "auto_sleep_seconds" (the value, not the index) so the daemon reads seconds
   directly with no shared table. */
static const char *kAutoSleepLabels[]  = { "Off", "15 sec", "30 sec", "45 sec", "1 min", "2 min", "5 min", "10 min" };
static const int   kAutoSleepSeconds[] = {     0,       15,       30,       45,      60,     120,     300,      600 };
#define JW_AUTO_SLEEP_COUNT   ((int)(sizeof(kAutoSleepLabels) / sizeof(kAutoSleepLabels[0])))
#define JW_AUTO_SLEEP_DEFAULT 5   /* 2 min (index into the tables above) */

static const char *kHomeCategoryLabels[] = {
    "Appearance",
    "Display & Sound",
    "Network",
    "Lighting",
    "Library",
    "Accounts",
    "Behavior",
    "About",
};
#define JW_SETTINGS_CATEGORY_COUNT 8

/* Visible rows in the Network page's scanned-network list (scrolls beyond). */
#define JW_WIFI_LIST_ROWS 6
#define JW_NETWORK_ROW_WIFI 0
#define JW_NETWORK_ROW_ADB  1
#define JW_NETWORK_FIXED_ROWS 2
/* Re-trigger a background scan at most this often while the page is open. */
#define JW_WIFI_SCAN_INTERVAL_MS 6000
#define JW_SETTINGS_DISPLAY_COUNT JW_DISPLAY_ROW_COUNT

static const char *kLedModeLabels[JW_LED_MODE_COUNT] = {
    "Static",
    "Breath",
    "Rainbow",
    "Comet",
    "Sweep",
    "Fountain",
    "Hiccup",
};


/* ─── Helpers ──────────────────────────────────────────────────────────── */

static int jw__find_theme_index(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < JW_SETTINGS_THEME_COUNT; i++) {
        if (strcmp(kJawakaThemes[i], name) == 0) return i;
    }
    return 0;
}

static void jw__refresh_brightness(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    int percent = -1;
    if (jw_ipc_platform_brightness(ui->socket_path, &percent) == 0 && percent >= 0)
        ui->brightness_percent = jw_platform_clamp_brightness_percent(percent);
}

/* Pull the live volume from the platform (pactl-backed). Re-querying keeps the
   settings value tied to reality, so OSD/hardware-key changes made outside
   settings aren't stale here and a subsequent step adjusts the true value. */
static void jw__refresh_volume(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    int percent = -1;
    if (jw_ipc_platform_volume(ui->socket_path, &percent) == 0 && percent >= 0) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        ui->volume_percent = percent;
    }
}

/* Pull the current LED config from jawakad's cached state (set once the user has
   configured it; otherwise the defaults stand and stock rainbow keeps running). */
static void jw__refresh_led(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    int enabled = ui->led_enabled ? 1 : 0;
    int r = ui->led_color.r, g = ui->led_color.g, b = ui->led_color.b;
    int brightness = ui->led_brightness, speed = ui->led_speed;
    char mode[16] = "FOREVER";
    if (jw_ipc_get_led(ui->socket_path, &enabled, mode, sizeof(mode),
                       &r, &g, &b, &brightness, &speed) == 0) {
        ui->led_enabled = enabled != 0;
        jw_led_mode m = JW_LED_MODE_STATIC;
        jw_led_mode_parse(mode, &m);
        ui->led_mode = (int)m;
        ui->led_color.r = (unsigned char)r;
        ui->led_color.g = (unsigned char)g;
        ui->led_color.b = (unsigned char)b;
        ui->led_color.a = 255;
        ui->led_brightness = brightness;
        ui->led_speed = speed;
    }
}

static void jw__refresh_secondary_sd_status(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    snprintf(ui->secondary_sd_status, sizeof(ui->secondary_sd_status), "%s", "Unavailable");
    if (!ui->socket_path[0]) {
        return;
    }

    jw_ipc_storage_status_info storage;
    if (jw_ipc_get_storage_status(ui->socket_path, "secondary_sd",
                                  &storage, NULL, 0) != 0) {
        return;
    }
    snprintf(ui->secondary_sd_status, sizeof(ui->secondary_sd_status), "%s",
             storage.busy ? "Busy" : (storage.mounted ? "Mounted" : "Not mounted"));
}

static void jw__refresh_adb(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->adb_enabled = -1;
    ui->adb_intent_enabled = -1;
    if (!ui->socket_path[0]) {
        return;
    }

    int enabled = -1;
    int intent = -1;
    if (jw_ipc_get_adb(ui->socket_path, &enabled, &intent) == 0) {
        ui->adb_enabled = enabled;
        ui->adb_intent_enabled = intent;
    }
}

/* Push the current LED config to jawakad (applies to hardware + persists). */
static void jw__apply_led(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    char status[64];
    int mode = ui->led_mode;
    if (mode < 0 || mode >= JW_LED_MODE_COUNT) mode = 0;
    jw_ipc_set_led(ui->socket_path, ui->led_enabled ? 1 : 0,
                   jw_led_mode_name((jw_led_mode)mode),
                   ui->led_color.r, ui->led_color.g, ui->led_color.b,
                   ui->led_brightness, ui->led_speed, status, sizeof(status));
}

void jw_settings_toggle_led(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    /* Reflect the daemon's current state first so the toggle is correct even if
       it was last changed elsewhere, then flip and apply. */
    jw__refresh_led(ui);
    ui->led_enabled = !ui->led_enabled;
    jw__apply_led(ui);
}

static void jw__persist(const jw_settings_ui *ui, const char *key, const char *val) {
    if (ui->db_path[0])
        jw_db_set_setting(ui->db_path, key, val);
}

static void jw__persist_int(const jw_settings_ui *ui, const char *key, int val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", val);
    jw__persist(ui, key, buf);
}

static void jw__persist_bool(const jw_settings_ui *ui, const char *key, bool val) {
    jw__persist(ui, key, val ? "1" : "0");
}

static void jw__persist_color(const jw_settings_ui *ui, const char *key, ap_color c) {
    char hex[16];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", c.r, c.g, c.b);
    jw__persist(ui, key, hex);
}

/* ─── Lifecycle ────────────────────────────────────────────────────────── */

void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path,
                          const char *initial_theme_name,
                          const char *socket_path) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->open   = false;
    ui->screen = JW_SETTINGS_HOME;
    ui->wifi_monitor_fd = -1;
    ui->adb_enabled = -1;
    ui->adb_intent_enabled = -1;
    cat_list_state_init(&ui->home_list,       JW_SETTINGS_CATEGORY_COUNT);
    cat_list_state_init(&ui->appearance_list,  JW_APPEAR_ROW_COUNT);
    cat_list_state_init(&ui->colors_list,      JW_COLOR_ROW_COUNT);
    cat_list_state_init(&ui->layout_list,      JW_LAYOUT_ROW_COUNT);
    cat_list_state_init(&ui->statusbar_list,   JW_STATUSBAR_ROW_COUNT);
    cat_list_state_init(&ui->display_list,     JW_SETTINGS_DISPLAY_COUNT);
    cat_list_state_init(&ui->network_list,     JW_WIFI_LIST_ROWS);
    cat_list_state_init(&ui->lighting_list,    JW_LIGHTING_ROW_COUNT);
    cat_list_state_init(&ui->library_list,     JW_LIBRARY_ROW_COUNT);
    cat_list_state_init(&ui->accounts_list,    JW_ACCOUNTS_ROW_COUNT);
    cat_list_state_init(&ui->behavior_list,    JW_BEHAVIOR_ROW_COUNT);
    cat_list_state_init(&ui->placeholder_list, 1);
    cat_scroll_state_init(&ui->about_scroll);
    ui->theme_index       = jw__find_theme_index(initial_theme_name);
    ui->color_scheme_index = -1;   /* custom until a scheme is loaded below */
    ui->pill_shape_index  = 0;
    ui->font_family_index = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
    ui->font_size_index   = 1;
    ui->show_hints        = true;
    ui->clock_style_index = 1;
    ui->show_battery      = true;
    ui->show_battery_level = false;
    ui->show_wifi         = true;
    ui->show_volume       = true;
    ui->startup_tab_index = JW_STARTUP_TAB_DEFAULT;
    ui->auto_sleep_index  = JW_AUTO_SLEEP_DEFAULT;
    ui->brightness_percent = 50;
    ui->volume_percent     = 50;
    ui->led_enabled    = false;
    ui->led_mode       = 0;   /* static */
    ui->led_color      = cat_hex_to_color("#FFFFFF");
    ui->led_brightness = 5;
    ui->led_speed      = 5;
    snprintf(ui->secondary_sd_status, sizeof(ui->secondary_sd_status), "%s",
             "Unavailable");

    if (db_path && db_path[0])
        snprintf(ui->db_path, sizeof(ui->db_path), "%s", db_path);
    if (socket_path && socket_path[0])
        snprintf(ui->socket_path, sizeof(ui->socket_path), "%s", socket_path);

    /* Restore persisted overrides. The index reads below keep the settings
       UI's own state in sync with the DB; the theme itself (all 7 colors,
       pill shape, font size) is applied by the shared override helper. */
    if (ui->db_path[0]) {
        char val[32];
        if (jw_db_get_setting(db_path, "pill_shape_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT)
                ui->pill_shape_index = idx;
        }
        ui->font_family_index = jw_appearance_font_family_index_from_db(db_path);
        if (jw_db_get_setting(db_path, "font_size_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT)
                ui->font_size_index = idx;
        }
        if (jw_db_get_setting(db_path, "show_hints", val, sizeof(val)) == 0)
            ui->show_hints = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "clock_style_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_CLOCK_STYLE_COUNT)
                ui->clock_style_index = idx;
        }
        if (jw_db_get_setting(db_path, "show_battery", val, sizeof(val)) == 0)
            ui->show_battery = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "show_battery_level", val, sizeof(val)) == 0)
            ui->show_battery_level = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "show_wifi", val, sizeof(val)) == 0)
            ui->show_wifi = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "show_volume", val, sizeof(val)) == 0)
            ui->show_volume = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "color_scheme_index", val, sizeof(val)) == 0 && val[0]) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_COLOR_SCHEME_COUNT)
                ui->color_scheme_index = idx;
        }
        if (jw_db_get_setting(db_path, "startup_tab_index", val, sizeof(val)) == 0 && val[0]) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_STARTUP_TAB_COUNT)
                ui->startup_tab_index = idx;
        }
        /* Stored as seconds (what the daemon reads); map back to the label index. */
        if (jw_db_get_setting(db_path, "auto_sleep_seconds", val, sizeof(val)) == 0 && val[0]) {
            int seconds = atoi(val);
            for (int i = 0; i < JW_AUTO_SLEEP_COUNT; i++) {
                if (kAutoSleepSeconds[i] == seconds) { ui->auto_sleep_index = i; break; }
            }
        }

        jw_settings_apply_persisted_overrides(ui->db_path);

        /* Fresh install (no color ever persisted) → default to the Leaf scheme,
           the project's identity theme. Returning users keep their colors. */
        char probe[32];
        if (jw_db_get_setting(db_path, "accent_color", probe, sizeof(probe)) != 0 || !probe[0])
            jw__apply_color_scheme(ui, JW_COLOR_SCHEME_DEFAULT, NULL);
    }

    jw__refresh_brightness(ui);
    jw__refresh_volume(ui);
    jw__refresh_led(ui);
    jw__refresh_adb(ui);
    jw__refresh_secondary_sd_status(ui);
}

void jw_settings_ui_enter(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = true;
    ui->screen = JW_SETTINGS_HOME;
    jw__refresh_brightness(ui);
    jw__refresh_volume(ui);
    jw__refresh_led(ui);
    jw__refresh_adb(ui);
    jw__refresh_secondary_sd_status(ui);
}

void jw_settings_ui_close(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = false;
    ui->screen = JW_SETTINGS_HOME;
}

bool jw_settings_ui_is_open(const jw_settings_ui *ui) {
    return ui && ui->open;
}

jw_settings_screen jw_settings_ui_screen(const jw_settings_ui *ui) {
    return ui ? ui->screen : JW_SETTINGS_HOME;
}

bool jw_settings_ui_wants_av_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_DISPLAY;
}

void jw_settings_ui_refresh_av(jw_settings_ui *ui) {
    jw__refresh_brightness(ui);
    jw__refresh_volume(ui);
    jw__refresh_led(ui);
}

bool jw_settings_show_hints(const jw_settings_ui *ui) {
    return !ui || ui->show_hints;
}

void jw_settings_status_bar_opts(const jw_settings_ui *ui, cat_status_bar_opts *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ui) {
        out->show_clock = CAT_CLOCK_AUTO;
        out->show_battery = true;
        out->show_wifi = true;
        return;
    }
    if (ui->clock_style_index == 0) {
        out->show_clock = CAT_CLOCK_HIDE;
    } else {
        out->show_clock = CAT_CLOCK_SHOW;
        out->use_24h = (ui->clock_style_index == 1);
        out->no_ampm = (ui->clock_style_index == 3);
    }
    out->show_battery = ui->show_battery;
    out->show_battery_level = ui->show_battery_level;
    out->show_wifi = ui->show_wifi;
    /* We own the radio read (jw_wifi_*), so feed the icon our value rather than
       letting Catastrophe shell out itself — keeps the icon and Network page on
       one source. */
    out->wifi_supplied = true;
    out->wifi_strength = ui->show_wifi ? ui->wifi_strength_cached : 0;
    out->show_volume = ui->show_volume;
    out->volume_percent = ui->show_volume ? ui->volume_percent : -1;
}

bool jw_settings_show_volume(const jw_settings_ui *ui) {
    return ui && ui->show_volume;
}

void jw_settings_ui_refresh_volume(jw_settings_ui *ui) {
    jw__refresh_volume(ui);
}

bool jw_settings_show_wifi(const jw_settings_ui *ui) {
    return ui && ui->show_wifi;
}

void jw_settings_ui_refresh_wifi_strength(jw_settings_ui *ui) {
    if (ui) {
        ui->wifi_strength_cached = jw_wifi_strength_now();
    }
}

void jw_settings_load_status_prefs(const char *db_path,
                                   cat_status_bar_opts *out_opts,
                                   bool *out_show_hints) {
    /* Defaults match jw_settings_ui_init: hints on, 24h clock, battery + wifi on. */
    int  clock_style_index = 1;
    bool show_battery      = true;
    bool show_battery_level = false;
    bool show_wifi         = true;
    bool show_volume       = true;
    int  volume_percent    = -1;
    bool show_hints        = true;

    if (db_path && db_path[0]) {
        char val[32];
        if (jw_db_get_setting(db_path, "clock_style_index", val, sizeof(val)) == 0 && val[0]) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_CLOCK_STYLE_COUNT)
                clock_style_index = idx;
        }
        if (jw_db_get_setting(db_path, "show_battery", val, sizeof(val)) == 0)
            show_battery = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "show_battery_level", val, sizeof(val)) == 0)
            show_battery_level = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "show_wifi", val, sizeof(val)) == 0)
            show_wifi = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "show_volume", val, sizeof(val)) == 0)
            show_volume = (strcmp(val, "0") != 0);
        /* The menu has no live volume poll; use the daemon's last persisted value. */
        if (jw_db_get_setting(db_path, "platform.volume_percent", val, sizeof(val)) == 0 && val[0])
            volume_percent = atoi(val);
        if (jw_db_get_setting(db_path, "show_hints", val, sizeof(val)) == 0)
            show_hints = (strcmp(val, "0") != 0);
    }

    if (out_opts) {
        memset(out_opts, 0, sizeof(*out_opts));
        if (clock_style_index == 0) {
            out_opts->show_clock = CAT_CLOCK_HIDE;
        } else {
            out_opts->show_clock = CAT_CLOCK_SHOW;
            out_opts->use_24h = (clock_style_index == 1);
            out_opts->no_ampm = (clock_style_index == 3);
        }
        out_opts->show_battery = show_battery;
        out_opts->show_battery_level = show_battery_level;
        out_opts->show_wifi = show_wifi;
        out_opts->show_volume = show_volume;
        out_opts->volume_percent = show_volume ? volume_percent : -1;
    }
    if (out_show_hints)
        *out_show_hints = show_hints;
}

/* ─── Render helpers ───────────────────────────────────────────────────── */

static void jw__draw_header(const char *title, int x, int y, int w) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);
    int large_h = TTF_FontHeight(large);
    cat_draw_text_ellipsized(large, title, x + cat_scale(4), y, theme->text, w - cat_scale(8));
    cat_draw_rect(x, y + large_h + cat_scale(4), w, 1, cat_hex_to_color("#ffffff20"));
}

static int jw__header_h(void) {
    return TTF_FontHeight(cat_get_font(CAT_FONT_LARGE)) + cat_scale(10);
}

static void jw__render_list_row(const cat_list_state *list, int x, int y,
                                int w, int row, const char *label,
                                const char *value, bool cycler) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int iy = y + row * item_h;
    bool selected = (list->cursor == row);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (item_h - pill_h) / 2;
    if (selected)
        cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    cat_draw_text_ellipsized(body, label, x + cat_scale(12), ty, label_c,
                              w / 2 - cat_scale(20));

    if (value) {
        int body_h = TTF_FontHeight(body);
        int vw = cat_measure_text(body, value);
        if (cycler) {
            /* Solid triangles flank the value, matching the tab-switcher
               affordance: ◀ value ▶. Triangles are sized to the text cap and
               vertically centered. */
            int tri_h = body_h / 2;
            int tri_w = tri_h * 3 / 4;
            int gap   = cat_scale(8);
            int total = tri_w + gap + vw + gap + tri_w;
            int rx    = x + w - total - cat_scale(16);
            if (rx < x + w / 2) rx = x + w / 2;
            int tri_y = ty + (body_h - tri_h) / 2;
            cat_draw_triangle(rx, tri_y, tri_w, tri_h, CAT_DIR_LEFT, value_c);
            cat_draw_text(body, value, rx + tri_w + gap, ty, value_c);
            cat_draw_triangle(rx + tri_w + gap + vw + gap, tri_y, tri_w, tri_h,
                              CAT_DIR_RIGHT, value_c);
        } else {
            int vx = x + w - vw - cat_scale(16);
            if (vx < x + w / 2) vx = x + w / 2;
            cat_draw_text(body, value, vx, ty, value_c);
        }
    }
}

static void jw__render_nav_row(const cat_list_state *list, int x, int y,
                               int w, int row, const char *label) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int iy = y + row * item_h;
    bool selected = (list->cursor == row);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (item_h - pill_h) / 2;
    if (selected)
        cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);

    ap_color tc = selected ? theme->highlighted_text : theme->text;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, label, x + cat_scale(12), ty, tc, w - cat_scale(24));
}

static void jw__render_color_swatch(int x, int list_y, int w, int row, ap_color c) {
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int swatch_w = cat_scale(48);
    int swatch_h = cat_scale(14);
    int sx = x + w - cat_scale(20) - swatch_w;
    int sy = list_y + row * item_h + (item_h - swatch_h) / 2;
    cat_draw_pill(sx, sy, swatch_w, swatch_h, c);
}

/* ─── Page renderers ───────────────────────────────────────────────────── */

static void jw__render_home(const jw_settings_ui *ui, int x, int y, int w, int h) {
    /* No "Settings" header — the tab bar above already names this screen, so the
       category list starts at the top and reclaims that space. (Sub-screens keep
       their headers since the tab bar still just says "Settings".) */
    for (int i = 0; i < JW_SETTINGS_CATEGORY_COUNT; i++)
        jw__render_nav_row(&ui->home_list, x, y, w, i, kHomeCategoryLabels[i]);
    (void)h;
}

static void jw__render_appearance(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Appearance", x, y, w);
    int ly = y + jw__header_h();
    /* Layout switching is gated to Tabs, so this row instead cycles the curated
       color schemes (Aurora/Ember/…); "Custom" once colors are hand-edited. */
    const char *scheme_name =
        (ui->color_scheme_index >= 0 && ui->color_scheme_index < JW_COLOR_SCHEME_COUNT)
        ? kColorSchemes[ui->color_scheme_index].name : "Custom";
    jw__render_list_row(&ui->appearance_list, x, ly, w, JW_APPEAR_THEME,
                        "Color Scheme", scheme_name, true);
    jw__render_nav_row(&ui->appearance_list, x, ly, w, JW_APPEAR_COLORS, "Colors");
    jw__render_nav_row(&ui->appearance_list, x, ly, w, JW_APPEAR_LAYOUT, "Layout");
    jw__render_nav_row(&ui->appearance_list, x, ly, w, JW_APPEAR_STATUSBAR, "Status Bar");
    (void)h;
}

static void jw__render_colors(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *t = cat_get_theme();
    jw__draw_header("Colors", x, y, w);
    int ly = y + jw__header_h();

    struct { const char *label; ap_color color; } rows[] = {
        { "Accent",            t->accent },
        { "Background",        t->background },
        { "Text",              t->text },
        { "Selection",         t->highlight },
        { "Secondary Text",    t->hint },
        { "Button Text",       t->button_label },
        { "Button Background", t->button_glyph_bg },
    };

    for (int i = 0; i < JW_COLOR_ROW_COUNT; i++) {
        jw__render_list_row(&ui->colors_list, x, ly, w, i, rows[i].label, NULL, false);
        jw__render_color_swatch(x, ly, w, i, rows[i].color);
    }
    (void)h;
}

static void jw__render_layout(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Layout", x, y, w);
    int ly = y + jw__header_h();
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_PILL_SHAPE,
                        "List Style", kPillShapeLabels[ui->pill_shape_index], true);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_FONT_FAMILY,
                        "Font", kJawakaFontFamilyLabels[ui->font_family_index], true);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_FONT_SIZE,
                        "Font Size", kFontSizeLabels[ui->font_size_index], true);
    (void)h;
}

/* The Status Bar "Battery" row cycles four display modes. They're stored as the
   two persisted bools show_battery (icon) + show_battery_level (number), so the
   mode is just a UI view over that pair. */
enum {
    JW_BATTERY_OFF = 0,   /* neither icon nor number */
    JW_BATTERY_ICON,      /* icon only */
    JW_BATTERY_PERCENT,   /* number only */
    JW_BATTERY_BOTH,      /* icon + number */
    JW_BATTERY_MODE_COUNT
};
static const char *const kBatteryModeLabels[JW_BATTERY_MODE_COUNT] = {
    "Off", "Icon", "Percent", "Both"
};
static int jw__battery_mode(bool icon, bool number) {
    if (icon && number) return JW_BATTERY_BOTH;
    if (number)         return JW_BATTERY_PERCENT;
    if (icon)           return JW_BATTERY_ICON;
    return JW_BATTERY_OFF;
}
static void jw__battery_mode_to_flags(int mode, bool *icon, bool *number) {
    *icon   = (mode == JW_BATTERY_ICON || mode == JW_BATTERY_BOTH);
    *number = (mode == JW_BATTERY_PERCENT || mode == JW_BATTERY_BOTH);
}

static void jw__render_statusbar(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Status Bar", x, y, w);
    int ly = y + jw__header_h();
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_HINTS,
                        "Button Hints", ui->show_hints ? "On" : "Off", true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_CLOCK,
                        "Clock", kClockStyleLabels[ui->clock_style_index], true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_BATTERY,
                        "Battery",
                        kBatteryModeLabels[jw__battery_mode(ui->show_battery, ui->show_battery_level)],
                        true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_WIFI,
                        "Wifi", ui->show_wifi ? "On" : "Off", true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_VOLUME,
                        "Volume", ui->show_volume ? "On" : "Off", true);
    (void)h;
}

/* One labelled slider row (Brightness / Volume) on the Display & Sound page. */
static void jw__draw_slider_row(const jw_settings_ui *ui, int x, int y_base, int w,
                                int row, const char *label, int percent) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(28);
    int iy = y_base + row * item_h;
    bool selected = (ui->display_list.cursor == row);
    int pill_h = item_h - cat_scale(6);
    int pill_y = iy + cat_scale(3);
    if (selected)
        cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + cat_scale(8);

    cat_draw_text_ellipsized(body, label, x + cat_scale(12), ty, label_c,
                              w / 2 - cat_scale(20));

    char value_str[32];
    snprintf(value_str, sizeof(value_str), "%d%%", percent);
    int vw = cat_measure_text(body, value_str);
    cat_draw_text(body, value_str, x + w - vw - cat_scale(16), ty, value_c);

    int track_x = x + cat_scale(12);
    int track_y = pill_y + pill_h - cat_scale(16);
    int track_w = w - cat_scale(32);
    int fill_w = (track_w * percent) / 100;
    cat_draw_rect(track_x, track_y, track_w, cat_scale(4), cat_hex_to_color("#ffffff33"));
    cat_draw_rect(track_x, track_y, fill_w, cat_scale(4), value_c);
}

static void jw__render_display(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Display & Sound", x, y, w);
    int y_base = y + jw__header_h();
    jw__draw_slider_row(ui, x, y_base, w, JW_DISPLAY_BRIGHTNESS, "Brightness",
                        ui->brightness_percent);
    jw__draw_slider_row(ui, x, y_base, w, JW_DISPLAY_VOLUME, "Volume",
                        ui->volume_percent);
    (void)h;
}

static void jw__render_lighting(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Lighting", x, y, w);
    int ly = y + jw__header_h();
    int mode = (ui->led_mode >= 0 && ui->led_mode < JW_LED_MODE_COUNT) ? ui->led_mode : 0;
    char bright[8], speed[8];
    snprintf(bright, sizeof(bright), "%d", ui->led_brightness);
    snprintf(speed,  sizeof(speed),  "%d", ui->led_speed);

    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_ENABLE,
                        "Enable", ui->led_enabled ? "On" : "Off", true);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_MODE,
                        "Mode", kLedModeLabels[mode], true);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_COLOR,
                        "Color", NULL, false);
    jw__render_color_swatch(x, ly, w, JW_LIGHTING_COLOR, ui->led_color);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_BRIGHTNESS,
                        "Brightness", bright, true);
    jw__render_list_row(&ui->lighting_list, x, ly, w, JW_LIGHTING_SPEED,
                        "Speed", speed, true);
    (void)h;
}

static void jw__refresh_wifi(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    if (jw_wifi_status(&ui->wifi) != 0) {
        /* jw_wifi_status already zeroed the struct (valid = false). */
    }
    /* Keep the status-bar icon in sync with the page's own reading. */
    ui->wifi_strength_cached = ui->wifi.connected ? ui->wifi.strength : 0;
}

bool jw_settings_ui_wants_wifi_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_NETWORK;
}

/* Re-read the cached scan results into the ui (deduped/sorted by the module). */
static void jw__refresh_wifi_scan(jw_settings_ui *ui) {
    int n = jw_wifi_scan_results(ui->wifi.ssid, ui->wifi_networks,
                                 JW_WIFI_MAX_NETWORKS);
    ui->wifi_network_count = (n > 0) ? n : 0;
    int row_count = JW_NETWORK_FIXED_ROWS +
                    (ui->wifi_radio_on ? ui->wifi_network_count : 0);
    if (row_count < JW_NETWORK_FIXED_ROWS) {
        row_count = JW_NETWORK_FIXED_ROWS;
    }
    if (ui->network_list.cursor >= row_count) {
        ui->network_list.cursor = row_count - 1;
    }
}

/* Set the Network-page feedback toast (timestamped so it auto-expires). */
static void jw__wifi_msg(jw_settings_ui *ui, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->wifi_msg, sizeof(ui->wifi_msg), fmt, ap);
    va_end(ap);
    ui->wifi_msg_ms = SDL_GetTicks();
    if (ui->wifi_msg_ms == 0) ui->wifi_msg_ms = 1;   /* 0 means "none" */
}

/* Begin a join attempt: record it and attach to the wpa event socket so we can
   catch a WRONG_KEY auth failure (the only reliable wrong-password signal). */
static void jw__wifi_attempt_begin(jw_settings_ui *ui, const char *ssid) {
    snprintf(ui->wifi_attempt_ssid, sizeof(ui->wifi_attempt_ssid), "%s", ssid);
    ui->wifi_attempt_ms = SDL_GetTicks();
    if (ui->wifi_monitor_fd >= 0) {
        jw_wifi_monitor_close(ui->wifi_monitor_fd);
    }
    ui->wifi_monitor_fd = jw_wifi_monitor_open();
}

static void jw__wifi_attempt_clear(jw_settings_ui *ui) {
    ui->wifi_attempt_ssid[0] = '\0';
    if (ui->wifi_monitor_fd >= 0) {
        jw_wifi_monitor_close(ui->wifi_monitor_fd);
        ui->wifi_monitor_fd = -1;
    }
}

void jw_settings_ui_refresh_wifi(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    /* Expire the feedback toast so it doesn't linger after the action is over. */
    if (ui->wifi_msg_ms && (int)(SDL_GetTicks() - ui->wifi_msg_ms) > 6000) {
        ui->wifi_msg[0] = '\0';
        ui->wifi_msg_ms = 0;
    }
    /* Drain the event socket every frame (cheap, non-blocking) so a fast auth
       failure isn't missed between the throttled wpa_cli polls below. */
    jw_wifi_evt evt = ui->wifi_attempt_ssid[0] ? jw_wifi_monitor_poll(ui->wifi_monitor_fd)
                                               : JW_WIFI_EVT_NONE;

    /* Self-throttle the wpa_cli polls to ~2s; each forks wpa_cli. */
    unsigned now = SDL_GetTicks();
    if (evt == JW_WIFI_EVT_NONE && ui->wifi_next_poll_ms != 0 &&
        (int)(now - ui->wifi_next_poll_ms) < 0) {
        return;
    }
    jw__refresh_adb(ui);
    ui->wifi_radio_on = jw_wifi_radio_is_on();
    if (!ui->wifi_radio_on) {
        /* Radio off — nothing to poll or scan; clear stale state. */
        memset(&ui->wifi, 0, sizeof(ui->wifi));
        ui->wifi_network_count = 0;
        if (ui->network_list.cursor >= JW_NETWORK_FIXED_ROWS) {
            ui->network_list.cursor = JW_NETWORK_ROW_WIFI;
        }
        ui->wifi_next_poll_ms = now + 2000;
        return;
    }
    jw__refresh_wifi(ui);
    jw__refresh_wifi_scan(ui);

    /* Resolve a pending connect attempt:
       - success: associated (COMPLETED) on the target SSID;
       - WRONG_KEY event: definitive wrong password;
       - auth-fail event (SAE/WPA3 bad key, assoc reject): likely wrong password;
       - timeout (12s): neither.
       On any failure, forget the bad profile and recover the prior network — a
       connect uses select_network, which disabled it. The monitor is closed on
       resolve, so recovery's own churn events are never misread. */
    if (ui->wifi_attempt_ssid[0]) {
        bool connected = ui->wifi.connected &&
                         strcmp(ui->wifi.ssid, ui->wifi_attempt_ssid) == 0;
        bool failed = (evt != JW_WIFI_EVT_NONE) ||
                      (int)(now - ui->wifi_attempt_ms) > 12000;
        if (connected) {
            jw__wifi_msg(ui, "Connected to %s",
                     ui->wifi_attempt_ssid);
            jw__wifi_attempt_clear(ui);
        } else if (failed) {
            if (evt == JW_WIFI_EVT_WRONG_KEY) {
                /* Only a DEFINITIVE wrong key forgets the profile — never a
                   generic/timeout failure, which on this flaky radio can hit a
                   perfectly-good saved network and would otherwise destroy a
                   correct saved password. */
                char bad[64];
                snprintf(bad, sizeof(bad), "%s", ui->wifi_attempt_ssid);
                jw_wifi_forget(bad);
                jw__wifi_msg(ui, "Wrong password");
                jw__refresh_wifi_scan(ui);
            } else {
                jw__wifi_msg(ui, "Couldn't connect — check password");
            }
            jw_wifi_recover();     /* restore the network we were kicked off */
            jw__wifi_attempt_clear(ui);
        }
    }

    /* Trigger a fresh scan on a slower cadence than the result re-read. */
    if (ui->wifi_next_scan_ms == 0 || (int)(now - ui->wifi_next_scan_ms) >= 0) {
        jw_wifi_scan_start();
        ui->wifi_next_scan_ms = now + JW_WIFI_SCAN_INTERVAL_MS;
    }
    ui->wifi_next_poll_ms = now + 2000;
}

/* Fixed rows: Wi-Fi toggle, ADB action; scanned networks follow. */
typedef struct {
    const jw_wifi_network_t *nets;
    bool radio_on;
    int adb_enabled;
    int adb_intent_enabled;
} jw__wifi_list_ctx;

static void jw__draw_wifi_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    const jw__wifi_list_ctx *ctx = (const jw__wifi_list_ctx *)user;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - cat_scale(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    if (idx == JW_NETWORK_ROW_WIFI) {
        /* The on/off toggle row. */
        cat_draw_text(body, "Wi-Fi", ix + cat_scale(12), ty, label_c);
        /* Action verb (what A will do), not a bare state word — "Turn On" when
           the radio is off, "Turn Off" when it's on, so the button is unambiguous. */
        const char *action = ctx->radio_on ? "Turn Off" : "Turn On";
        int vw = cat_measure_text(body, action);
        cat_draw_text(body, action, ix + iw - vw - cat_scale(16), ty, value_c);
        return;
    }

    if (idx == JW_NETWORK_ROW_ADB) {
        const char *value = "Unavailable";
        if (ctx->adb_enabled == 1) {
            value = "Enabled";
        } else if (ctx->adb_intent_enabled == 1) {
            value = "Repair";
        } else if (ctx->adb_enabled == 0) {
            value = "Enable";
        }

        cat_draw_text(body, "ADB", ix + cat_scale(12), ty, label_c);
        int vw = cat_measure_text(body, value);
        cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
        return;
    }

    const jw_wifi_network_t *net = &ctx->nets[idx - JW_NETWORK_FIXED_ROWS];

    /* Left: SSID, with a leading "* " on the connected network. */
    char label[80];
    snprintf(label, sizeof(label), "%s%s", net->current ? "* " : "", net->ssid);
    cat_draw_text_ellipsized(body, label, ix + cat_scale(12), ty, label_c,
                             iw / 2);

    /* Right: signal word, "Open" prefix for unsecured nets, "saved" suffix for
       networks with a stored profile (other than the connected one). */
    const char *word = (net->strength >= 3) ? "Strong" :
                       (net->strength == 2) ? "Good"   : "Weak";
    const char *open_prefix = net->secured ? "" : "Open  ";
    const char *saved_suffix = (net->saved && !net->current) ? "  saved" : "";
    char value[56];
    snprintf(value, sizeof(value), "%s%s%s", open_prefix, word, saved_suffix);
    int vw = cat_measure_text(body, value);
    cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_network(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    jw__draw_header("Network", x, y, w);
    int ly = y + jw__header_h();
    int item_h = TTF_FontHeight(body) + cat_scale(12);

    const jw_wifi_status_t *wifi = &ui->wifi;

    char status_val[48];
    char signal_val[32];
    if (!ui->wifi_radio_on) {
        snprintf(status_val, sizeof(status_val), "%s", "Off");
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    } else if (!wifi->valid) {
        snprintf(status_val, sizeof(status_val), "%s", "Unavailable");
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    } else if (wifi->connected) {
        snprintf(status_val, sizeof(status_val), "%s", "Connected");
        /* Word + dBm, derived from RSSI with the same thresholds as the status
           icon, so the two always agree. */
        const char *level = (wifi->strength >= 3) ? "Strong" :
                            (wifi->strength == 2) ? "Good"   :
                            (wifi->strength == 1) ? "Weak"   : "—";
        if (wifi->rssi != 0)
            snprintf(signal_val, sizeof(signal_val), "%s (%d dBm)", level, wifi->rssi);
        else
            snprintf(signal_val, sizeof(signal_val), "%s", level);
    } else {
        snprintf(status_val, sizeof(status_val), "%s",
                 wifi->state[0] ? wifi->state : "Disconnected");
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    }

    /* ── Current-connection summary (informational lines) ── */
    int line_h = TTF_FontHeight(body) + cat_scale(8);
    int dy = ly;
    char line[160];

    snprintf(line, sizeof(line), "Status: %s", status_val);
    cat_draw_text(body, line, x + cat_scale(12), dy, theme->text);
    dy += line_h;

    snprintf(line, sizeof(line), "Network: %s",
             (wifi->valid && wifi->ssid[0]) ? wifi->ssid : "—");
    cat_draw_text_ellipsized(body, line, x + cat_scale(12), dy, theme->text, w - cat_scale(24));
    dy += line_h;

    snprintf(line, sizeof(line), "Signal: %s", signal_val);
    cat_draw_text(body, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h;

    snprintf(line, sizeof(line), "IP: %s",
             (wifi->valid && wifi->ip[0]) ? wifi->ip : "—");
    cat_draw_text(body, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h + cat_scale(6);

    /* Action feedback (always visible, regardless of the hint setting). */
    if (ui->wifi_msg[0]) {
        cat_draw_text_ellipsized(body, ui->wifi_msg, x + cat_scale(12), dy,
                                 theme->accent, w - cat_scale(24));
        dy += line_h + cat_scale(6);
    }

    /* ── List: fixed controls first, then scanned networks ── */
    int count = JW_NETWORK_FIXED_ROWS +
                (ui->wifi_radio_on ? ui->wifi_network_count : 0);
    jw__wifi_list_ctx ctx = {
        ui->wifi_networks,
        ui->wifi_radio_on,
        ui->adb_enabled,
        ui->adb_intent_enabled,
    };
    int list_h = JW_WIFI_LIST_ROWS * item_h;
    cat_draw_list_pane(x, dy, w, list_h, count, &ui->network_list, item_h,
                       jw__draw_wifi_item, &ctx);
    if (ui->wifi_radio_on && ui->wifi_network_count == 0) {
        cat_draw_text(body, "Scanning…", x + cat_scale(12),
                      dy + item_h * JW_NETWORK_FIXED_ROWS, theme->hint);
    }

    (void)h;
}

static void jw__render_library(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Library", x, y, w);
    int ly = y + jw__header_h();
    jw__render_list_row(&ui->library_list, x, ly, w, JW_LIBRARY_RESET_RETROARCH,
                        "Reset RetroArch Config", "Defaults", true);
    jw__render_list_row(&ui->library_list, x, ly, w, JW_LIBRARY_UNMOUNT_SECONDARY,
                        "Unmount Secondary SD",
                        ui->secondary_sd_status[0] ? ui->secondary_sd_status : "Unavailable",
                        true);
    (void)h;
}

static void jw__render_accounts(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Accounts", x, y, w);
    int ly = y + jw__header_h();
    /* Placeholders — sign-in is not wired up yet, so both rows report the
       not-signed-in state and do nothing on press. */
    jw__render_list_row(&ui->accounts_list, x, ly, w, JW_ACCOUNTS_SCREENSCRAPER,
                        "ScreenScraper.fr", "Not signed in", false);
    jw__render_list_row(&ui->accounts_list, x, ly, w, JW_ACCOUNTS_RETROACHIEVEMENTS,
                        "RetroAchievements", "Not signed in", false);
    (void)h;
}

/* ─── About ────────────────────────────────────────────────────────────── */

/* Bump alongside meaningful launcher releases. */
#define JW_ABOUT_VERSION "0.0.1"

typedef struct { const char *name; const char *license; } jw__about_credit;

/* Third-party components shipped with / used by the CFW and their licenses.
   Short names only — the full license texts ship with each component and live
   in the respective repos. */
static const jw__about_credit kAboutCredits[] = {
    { "Jawaka + Catastrophe",                   "MIT" },
    { "RetroArch",                              "GPLv3" },
    { "Libretro cores",                         "GPL / per-core" },
    { "SDL2 / SDL2_image / SDL2_ttf",           "Zlib" },
    { "FreeType",                               "FreeType License" },
    { "HarfBuzz",                               "MIT" },
    { "libpng",                                 "libpng License" },
    { "zlib",                                   "Zlib License" },
    { "SQLite",                                 "Public Domain" },
    { "cJSON",                                  "MIT" },
    { "System icons (libretro Systematic)",     "CC BY-SA 4.0" },
    { "Fonts (Space Grotesk, Inter, Source Han Sans)", "SIL OFL 1.1" },
    { "Dropbear SSH",                           "MIT-style" },
};
#define JW_ABOUT_CREDIT_COUNT ((int)(sizeof(kAboutCredits) / sizeof(kAboutCredits[0])))

/* One row of the About content. The whole thing — identity, a System block, a
   Library block, and the open-source components — is a flat list of these,
   rendered inside one scroll view. */
typedef enum {
    JW_ABOUT_PLAIN,     /* single left label (identity)            */
    JW_ABOUT_HEADING,   /* section heading (accent)                */
    JW_ABOUT_FIELD,     /* field name (left, dim) : value (right)  */
    JW_ABOUT_CREDIT     /* component (left) : license (right, dim) */
} jw__about_kind;

typedef struct {
    jw__about_kind kind;
    char label[48];
    char value[72];
} jw__about_row;

#define JW_ABOUT_MAX_ROWS 64

typedef struct {
    const jw__about_row *rows;
    int                  count;
    TTF_Font            *font;
    int                  row_h;
} jw__about_ctx;

/* cat_scroll_view content callback: lay out every row at its natural position.
   Long labels/values marquee instead of truncating; the scroll view applies the
   offset and clips. */
static void jw__draw_about_rows(int x, int y, int w, void *user) {
    const jw__about_ctx *ctx = (const jw__about_ctx *)user;
    ap_theme *theme = cat_get_theme();
    /* Per-row marquee state (only one About screen is live at a time). */
    static cat_marquee label_mq[JW_ABOUT_MAX_ROWS];
    static cat_marquee value_mq[JW_ABOUT_MAX_ROWS];
    static uint32_t last_ms = 0;
    uint32_t now = SDL_GetTicks();
    uint32_t dt  = (last_ms == 0) ? 0u : (now - last_ms);
    last_ms = now;

    bool animating = false;
    for (int i = 0; i < ctx->count && i < JW_ABOUT_MAX_ROWS; i++) {
        const jw__about_row *r = &ctx->rows[i];
        int row_y = y + i * ctx->row_h;
        if (r->kind == JW_ABOUT_HEADING) {
            if (cat_draw_text_marquee(ctx->font, r->label, x, row_y, theme->accent, w, &label_mq[i], dt))
                animating = true;
            continue;
        }
        if (r->kind == JW_ABOUT_PLAIN) {
            if (cat_draw_text_marquee(ctx->font, r->label, x, row_y, theme->text, w, &label_mq[i], dt))
                animating = true;
            continue;
        }
        /* FIELD: dim label + bright value. CREDIT: bright name + dim license. */
        int label_w, value_x;
        ap_color label_col, value_col;
        if (r->kind == JW_ABOUT_FIELD) {
            label_w   = w * 40 / 100;
            value_x   = x + w * 42 / 100;
            label_col = theme->hint;
            value_col = theme->text;
        } else {
            label_w   = w * 56 / 100;
            value_x   = x + w * 60 / 100;
            label_col = theme->text;
            value_col = theme->hint;
        }
        int value_w = (x + w) - value_x;
        /* Components ping-pong (they barely overflow, so a bounce reads better);
           device-info values use the continuous loop. */
        cat_marquee_mode mode = (r->kind == JW_ABOUT_CREDIT) ? CAT_MARQUEE_PINGPONG
                                                             : CAT_MARQUEE_LOOP;
        label_mq[i].mode = mode;
        value_mq[i].mode = mode;
        if (cat_draw_text_marquee(ctx->font, r->label, x, row_y, label_col, label_w, &label_mq[i], dt))
            animating = true;
        if (cat_draw_text_marquee(ctx->font, r->value, value_x, row_y, value_col, value_w, &value_mq[i], dt))
            animating = true;
    }
    if (animating) cat_request_frame();
}

/* Append a row (no-op once full). */
static void jw__about_push(jw__about_row *rows, int *n, jw__about_kind kind,
                           const char *label, const char *value) {
    if (*n >= JW_ABOUT_MAX_ROWS) return;
    rows[*n].kind = kind;
    snprintf(rows[*n].label, sizeof(rows[*n].label), "%s", label ? label : "");
    snprintf(rows[*n].value, sizeof(rows[*n].value), "%s", value ? value : "");
    (*n)++;
}

static void jw__render_about(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("About", x, y, w);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(6);
    int top = y + jw__header_h() + pad;

    /* Live system facts + library counts, refreshed about once a second while
       the page is open (cheap reads; only one About screen is live at a time). */
    static jw_system_info    info;
    static jw_library_summary summary;
    static uint32_t          last_refresh = 0;
    static bool              have = false;
    uint32_t now = SDL_GetTicks();
    if (!have || now - last_refresh > 1000) {
        jw_platform_system_info(ui->db_path, &info);
        if (jw_db_read_summary(ui->db_path, &summary) != 0) memset(&summary, 0, sizeof(summary));
        last_refresh = now;
        have = true;
    }

    jw__about_row rows[JW_ABOUT_MAX_ROWS];
    int n = 0;
    char buf[72];

    jw__about_push(rows, &n, JW_ABOUT_PLAIN, "Jawaka  v" JW_ABOUT_VERSION, "");

    jw__about_push(rows, &n, JW_ABOUT_HEADING, "System", "");
    snprintf(buf, sizeof(buf), "LoongOS %s", info.os_version[0] ? info.os_version : "?");
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Stock OS", buf);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Kernel", info.kernel[0] ? info.kernel : "—");
    /* Hardware: prefer the parsed labeled lines (SoC / Power / Board / RAM);
       fall back to the raw device-tree model when nothing parsed. */
    if (info.soc[0] || info.pmic[0] || info.board[0]) {
        if (info.soc[0])
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "SoC", info.soc);
        if (info.pmic[0]) {
            snprintf(buf, sizeof(buf), "%s PMIC", info.pmic);
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "Power", buf);
        }
        if (info.board[0])
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "Board", info.board);
        if (info.ram_type[0] && info.mem_total_kb > 0) {
            long ram_gb = (info.mem_total_kb + 524288) / 1048576;   /* round kB → GB */
            if (ram_gb < 1) ram_gb = 1;
            snprintf(buf, sizeof(buf), "%ld GB %s", ram_gb, info.ram_type);
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "RAM", buf);
        } else if (info.ram_type[0]) {
            jw__about_push(rows, &n, JW_ABOUT_FIELD, "RAM", info.ram_type);
        }
    } else {
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Device", info.device[0] ? info.device : "—");
    }
    if (info.mem_total_kb > 0) {
        snprintf(buf, sizeof(buf), "%ld / %ld MB", info.mem_avail_kb / 1024, info.mem_total_kb / 1024);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Memory free", buf);
    }
    if (info.sd_total_mb > 0) {
        snprintf(buf, sizeof(buf), "%ld / %ld GB", info.sd_free_mb / 1024, info.sd_total_mb / 1024);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Storage free", buf);
    }
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "IP", info.ip[0] ? info.ip : "—");
    if (info.battery_percent >= 0) {
        snprintf(buf, sizeof(buf), "%d%%%s", info.battery_percent, info.charging ? " (charging)" : "");
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Battery", buf);
    }
    if (info.cpu_temp_c > 0) {
        snprintf(buf, sizeof(buf), "%d \xc2\xb0""C", info.cpu_temp_c);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "CPU temp", buf);
    }
    if (info.uptime_s > 0) {
        snprintf(buf, sizeof(buf), "%ldh %ldm", info.uptime_s / 3600, (info.uptime_s % 3600) / 60);
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Uptime", buf);
    }

    jw__about_push(rows, &n, JW_ABOUT_HEADING, "Library", "");
    snprintf(buf, sizeof(buf), "%d", summary.game_count);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Games", buf);
    snprintf(buf, sizeof(buf), "%d", summary.system_count);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Systems", buf);
    snprintf(buf, sizeof(buf), "%d", summary.app_count);
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "Apps", buf);

    jw__about_push(rows, &n, JW_ABOUT_HEADING, "Open-source components", "");
    for (int i = 0; i < JW_ABOUT_CREDIT_COUNT; i++)
        jw__about_push(rows, &n, JW_ABOUT_CREDIT, kAboutCredits[i].name, kAboutCredits[i].license);

    /* Everything scrolls together in one (non-selectable) scroll view. Up/down
       scroll; a scrollbar appears when the content outgrows the pane. The scroll
       offset lives in ui->about_scroll — the settings render path is const by
       convention, but the scroll view must persist its clamped offset across
       frames, so this one field is the deliberate exception. */
    int row_h     = TTF_FontHeight(small) + cat_scale(8);
    int avail_h   = (y + h) - top;
    int rows_fit  = avail_h / row_h;
    if (rows_fit < 1) rows_fit = 1;
    int view_h    = rows_fit * row_h;        /* row-aligned: never a partial row */
    int content_h = n * row_h;
    jw__about_ctx ctx = { rows, n, small, row_h };
    cat_draw_scroll_view(x + pad, top, w - pad * 2, view_h, content_h,
                         (cat_scroll_state *)&ui->about_scroll,
                         jw__draw_about_rows, &ctx);
}

static void jw__render_behavior(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Behavior", x, y, w);
    int ly = y + jw__header_h();
    int tab = (ui->startup_tab_index >= 0 && ui->startup_tab_index < JW_STARTUP_TAB_COUNT)
              ? ui->startup_tab_index : JW_STARTUP_TAB_DEFAULT;
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_STARTUP_TAB,
                        "Startup Tab", kStartupTabLabels[tab], true);
    int sleep_idx = (ui->auto_sleep_index >= 0 && ui->auto_sleep_index < JW_AUTO_SLEEP_COUNT)
                    ? ui->auto_sleep_index : JW_AUTO_SLEEP_DEFAULT;
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_AUTO_SLEEP,
                        "Auto Sleep", kAutoSleepLabels[sleep_idx], true);
    (void)h;
}

/* ─── Main render dispatch ─────────────────────────────────────────────── */

void jw_settings_ui_render(const jw_settings_ui *ui,
                            int x, int y, int w, int h) {
    if (!ui || !ui->open) return;
    switch (ui->screen) {
        case JW_SETTINGS_HOME:       jw__render_home(ui, x, y, w, h);       break;
        case JW_SETTINGS_APPEARANCE: jw__render_appearance(ui, x, y, w, h); break;
        case JW_SETTINGS_COLORS:     jw__render_colors(ui, x, y, w, h);     break;
        case JW_SETTINGS_LAYOUT:     jw__render_layout(ui, x, y, w, h);     break;
        case JW_SETTINGS_STATUS_BAR: jw__render_statusbar(ui, x, y, w, h);  break;
        case JW_SETTINGS_DISPLAY:    jw__render_display(ui, x, y, w, h);    break;
        case JW_SETTINGS_NETWORK:    jw__render_network(ui, x, y, w, h);    break;
        case JW_SETTINGS_LIGHTING:   jw__render_lighting(ui, x, y, w, h);   break;
        case JW_SETTINGS_LIBRARY:    jw__render_library(ui, x, y, w, h);                 break;
        case JW_SETTINGS_ACCOUNTS:   jw__render_accounts(ui, x, y, w, h);                break;
        case JW_SETTINGS_BEHAVIOR:   jw__render_behavior(ui, x, y, w, h);                 break;
        case JW_SETTINGS_ABOUT:      jw__render_about(ui, x, y, w, h);                   break;
    }
}

void jw_settings_apply_persisted_overrides(const char *db_path) {
    if (!db_path || !db_path[0]) return;

    ap_theme *t = cat_get_theme();
    char val[32];

    if (jw_db_get_setting(db_path, "pill_shape_index", val, sizeof(val)) == 0 && val[0]) {
        int idx = atoi(val);
        if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT) {
            t->pill_radius_ratio = kJawakaPillRadiusValues[idx];
            t->pill_corner_mask  = kJawakaPillCornerMasks[idx];
        }
    }
    /* Font size and family both reload the glyph atlas, so resolve them together
       and reload at most once. cat_set_font_bump() reloads using the theme's
       font_path, so point that at the persisted family first; if the bump is
       unchanged it short-circuits and we issue the single reload ourselves.
       Env-launched processes already had both applied by cat_init (via
       CAT_FONT_PATH/CAT_FONT_BUMP), so there we only sync the bump. */
    {
        int bump = cat_get_font_bump();
        if (jw_db_get_setting(db_path, "font_size_index", val, sizeof(val)) == 0 && val[0]) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT)
                bump = kJawakaFontSizeValues[idx];
        }
        if (getenv("CAT_FONT_PATH")) {
            cat_set_font_bump(bump);
        } else {
            int fidx = jw_appearance_font_family_index_from_db(db_path);
            snprintf(t->font_path, sizeof(t->font_path), "%s",
                     jw_appearance_font_path_for_index(fidx));
            if (bump != cat_get_font_bump())
                cat_set_font_bump(bump);          /* one reload, at the new family path */
            else
                cat_reload_fonts(t->font_path);   /* bump unchanged: one reload for the family */
        }
    }
    if (jw_db_get_setting(db_path, "accent_color", val, sizeof(val)) == 0 && val[0])
        t->accent = cat_hex_to_color(val);
    if (jw_db_get_setting(db_path, "text_color", val, sizeof(val)) == 0 && val[0])
        t->text = cat_hex_to_color(val);
    if (jw_db_get_setting(db_path, "hint_color", val, sizeof(val)) == 0 && val[0])
        t->hint = cat_hex_to_color(val);
    if (jw_db_get_setting(db_path, "highlight_color", val, sizeof(val)) == 0 && val[0])
        t->highlight = cat_hex_to_color(val);
    if (jw_db_get_setting(db_path, "bg_color", val, sizeof(val)) == 0 && val[0])
        t->background = cat_hex_to_color(val);
    if (jw_db_get_setting(db_path, "button_label_color", val, sizeof(val)) == 0 && val[0])
        t->button_label = cat_hex_to_color(val);
    if (jw_db_get_setting(db_path, "button_glyph_bg_color", val, sizeof(val)) == 0 && val[0])
        t->button_glyph_bg = cat_hex_to_color(val);

    cat_finalize_theme_colors(t);
}

/* Apply a curated color scheme. The seven color roles are written straight into
   the live theme in memory (instant — no per-key DB read-back), then all eight
   keys persist in a single transaction. Cycling schemes used to cost ~18 DB
   re-opens per press (8 writes + 10 read-backs); this is one open. */
static void jw__apply_color_scheme(jw_settings_ui *ui, int index, bool *theme_changed) {
    if (index < 0 || index >= JW_COLOR_SCHEME_COUNT) return;
    const jw__color_scheme *s = &kColorSchemes[index];

    ap_theme *t = cat_get_theme();
    t->accent          = cat_hex_to_color(s->accent);
    t->background      = cat_hex_to_color(s->bg);
    t->text            = cat_hex_to_color(s->text);
    t->hint            = cat_hex_to_color(s->hint);
    t->highlight       = cat_hex_to_color(s->selection);
    t->button_label    = cat_hex_to_color(s->btn_label);
    t->button_glyph_bg = cat_hex_to_color(s->btn_bg);
    cat_finalize_theme_colors(t);

    ui->color_scheme_index = index;
    if (theme_changed) *theme_changed = true;

    char idx_buf[16];
    snprintf(idx_buf, sizeof(idx_buf), "%d", index);
    const char *keys[] = {
        "accent_color", "bg_color", "text_color", "hint_color",
        "highlight_color", "button_label_color", "button_glyph_bg_color",
        "color_scheme_index",
    };
    const char *vals[] = {
        s->accent, s->bg, s->text, s->hint, s->selection,
        s->btn_label, s->btn_bg, idx_buf,
    };
    if (ui->db_path[0])
        jw_db_set_settings(ui->db_path, keys, vals,
                           (int)(sizeof(keys) / sizeof(keys[0])));
}

static void jw__cycle_color_scheme(jw_settings_ui *ui, int direction, bool *theme_changed) {
    int n = JW_COLOR_SCHEME_COUNT;
    int cur = ui->color_scheme_index;
    int next = (cur < 0) ? (direction > 0 ? 0 : n - 1)
                         : ((cur + direction + n) % n);
    jw__apply_color_scheme(ui, next, theme_changed);
}

static void jw__change_brightness(jw_settings_ui *ui, int delta,
                                  char *status_buf, size_t status_size) {
    int next = jw_platform_clamp_brightness_percent(ui->brightness_percent + delta);
    int resolved = next;
    if (ui->socket_path[0] &&
        jw_ipc_set_brightness(ui->socket_path, next, &resolved, status_buf,
                              (int)status_size) == 0) {
        ui->brightness_percent = jw_platform_clamp_brightness_percent(resolved);
        return;
    }
    if (status_buf && status_size > 0)
        snprintf(status_buf, status_size, "%s", "brightness failed");
}

static void jw__change_volume(jw_settings_ui *ui, int delta,
                              char *status_buf, size_t status_size) {
    int next = ui->volume_percent + delta;
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    int resolved = next;
    if (ui->socket_path[0] &&
        jw_ipc_set_volume(ui->socket_path, next, &resolved, status_buf,
                          (int)status_size) == 0) {
        if (resolved < 0) resolved = 0;
        if (resolved > 100) resolved = 100;
        ui->volume_percent = resolved;
        return;
    }
    if (status_buf && status_size > 0)
        snprintf(status_buf, status_size, "%s", "volume failed");
}

/* ─── Color picker helper ──────────────────────────────────────────────── */

static bool jw__pick_color(jw_settings_ui *ui, ap_color *target,
                           const char *db_key, int active_role) {
    ap_theme *theme = cat_get_theme();
    cat_color_picker_context context = {
        .roles = {
            { "Accent",     theme->accent },
            { "Background", theme->background },
            { "Text",       theme->text },
            { "Selection",  theme->highlight },
            { "Secondary",  theme->hint },
            { "Btn Text",   theme->button_label },
            { "Btn Bg",     theme->button_glyph_bg },
        },
        .role_count = JW_COLOR_ROW_COUNT,
        .active_role = active_role,
    };

    ap_color picked;
    if (cat_color_picker_ctx(*target, &picked, &context) == CAT_OK) {
        *target = picked;
        jw__persist_color(ui, db_key, picked);
        return true;
    }
    return false;
}

static bool jw__confirm_retroarch_reset(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Reset", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Reset shared RetroArch config to packaged defaults?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__reset_retroarch_config(jw_settings_ui *ui,
                                       char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!jw__confirm_retroarch_reset()) {
        snprintf(status_buf, status_size, "%s", "RetroArch reset canceled");
        return;
    }

    jw_ipc_reset_retroarch_config(ui->socket_path, status_buf, (int)status_size);
}

static bool jw__confirm_secondary_unmount(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Unmount", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Safely unmount the secondary SD card?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_adb_enable(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Enable", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Enable USB ADB and restore it at boot?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_adb_disable(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Disable", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Disable USB ADB and remove Leaf's boot restore marker?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__set_adb(jw_settings_ui *ui, bool enabled,
                        char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (enabled) {
        if (!jw__confirm_adb_enable()) {
            snprintf(status_buf, status_size, "%s", "ADB enable canceled");
            return;
        }
    } else if (!jw__confirm_adb_disable()) {
        snprintf(status_buf, status_size, "%s", "ADB disable canceled");
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_set_adb(ui->socket_path, enabled ? 1 : 0,
                       status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s",
                 enabled ? "ADB enable failed" : "ADB disable failed");
    }
    jw__refresh_adb(ui);
}

static void jw__safe_unmount_secondary_sd(jw_settings_ui *ui,
                                          char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!jw__confirm_secondary_unmount()) {
        snprintf(status_buf, status_size, "%s", "Unmount canceled");
        jw__refresh_secondary_sd_status(ui);
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_safe_unmount_storage(ui->socket_path, "secondary_sd",
                                    status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s", "Unmount failed");
    }
    jw__refresh_secondary_sd_status(ui);
}

/* ─── Input dispatch ───────────────────────────────────────────────────── */

bool jw_settings_ui_handle_button(jw_settings_ui *ui, cat_button button,
                                    char *status_buf, size_t status_size,
                                    bool *theme_changed) {
    if (!ui || !ui->open) return false;
    if (theme_changed) *theme_changed = false;

    switch (ui->screen) {

    /* ── Home ────────────────────────────────────────────────────────── */
    case JW_SETTINGS_HOME:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->home_list, -1, JW_SETTINGS_CATEGORY_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->home_list, +1, JW_SETTINGS_CATEGORY_COUNT); break;
            case CAT_BTN_A: {
                int idx = ui->home_list.cursor;
                if (idx == 0) ui->screen = JW_SETTINGS_APPEARANCE;
                else if (idx == 1) {
                    ui->screen = JW_SETTINGS_DISPLAY;
                    /* Re-sync to live values so an OSD/hardware change made
                       outside settings isn't stale before we adjust. */
                    jw__refresh_brightness(ui);
                    jw__refresh_volume(ui);
                }
                else if (idx == 2) {
                    ui->screen = JW_SETTINGS_NETWORK;
                    ui->network_list.cursor = 0;
                    ui->network_list.scroll_offset = 0;
                    ui->wifi_msg[0] = '\0';
                    jw__wifi_attempt_clear(ui);
                    jw__refresh_adb(ui);
                    jw__refresh_wifi(ui);          /* show status immediately */
                    jw_wifi_scan_start();          /* kick a scan */
                    jw__refresh_wifi_scan(ui);     /* show any cached results now */
                    unsigned now = SDL_GetTicks();
                    ui->wifi_next_poll_ms = now + 2000;            /* then live every ~2s */
                    ui->wifi_next_scan_ms = now + JW_WIFI_SCAN_INTERVAL_MS;
                }
                else if (idx == 3) {
                    ui->screen = JW_SETTINGS_LIGHTING;
                    jw__refresh_led(ui);
                }
                else if (idx == 4) {
                    ui->screen = JW_SETTINGS_LIBRARY;
                    jw__refresh_secondary_sd_status(ui);
                }
                else if (idx == 5) ui->screen = JW_SETTINGS_ACCOUNTS;
                else if (idx == 6) ui->screen = JW_SETTINGS_BEHAVIOR;
                else if (idx == 7) {
                    ui->screen = JW_SETTINGS_ABOUT;
                    cat_scroll_state_init(&ui->about_scroll);   /* start at top */
                }
                break;
            }
            case CAT_BTN_B:
                jw_settings_ui_close(ui);
                return false;
            default: break;
        }
        break;

    /* ── Appearance sub-menu ─────────────────────────────────────────── */
    case JW_SETTINGS_APPEARANCE:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->appearance_list, -1, JW_APPEAR_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->appearance_list, +1, JW_APPEAR_ROW_COUNT); break;
            case CAT_BTN_LEFT:
                if (ui->appearance_list.cursor == JW_APPEAR_THEME)
                    jw__cycle_color_scheme(ui, -1, theme_changed);
                break;
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int row = ui->appearance_list.cursor;
                if (row == JW_APPEAR_THEME)
                    jw__cycle_color_scheme(ui, +1, theme_changed);
                else if (row == JW_APPEAR_COLORS)   ui->screen = JW_SETTINGS_COLORS;
                else if (row == JW_APPEAR_LAYOUT)   ui->screen = JW_SETTINGS_LAYOUT;
                else if (row == JW_APPEAR_STATUSBAR) ui->screen = JW_SETTINGS_STATUS_BAR;
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Colors ──────────────────────────────────────────────────────── */
    case JW_SETTINGS_COLORS:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->colors_list, -1, JW_COLOR_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->colors_list, +1, JW_COLOR_ROW_COUNT); break;
            case CAT_BTN_A: {
                ap_theme *t = cat_get_theme();
                int row = ui->colors_list.cursor;
                bool changed = false;
                if (row == JW_COLOR_ACCENT)
                    changed = jw__pick_color(ui, &t->accent, "accent_color", row);
                else if (row == JW_COLOR_TEXT)
                    changed = jw__pick_color(ui, &t->text, "text_color", row);
                else if (row == JW_COLOR_HINT)
                    changed = jw__pick_color(ui, &t->hint, "hint_color", row);
                else if (row == JW_COLOR_HIGHLIGHT)
                    changed = jw__pick_color(ui, &t->highlight, "highlight_color", row);
                else if (row == JW_COLOR_BACKGROUND)
                    changed = jw__pick_color(ui, &t->background, "bg_color", row);
                else if (row == JW_COLOR_BTN_TEXT)
                    changed = jw__pick_color(ui, &t->button_label, "button_label_color", row);
                else if (row == JW_COLOR_BTN_BG)
                    changed = jw__pick_color(ui, &t->button_glyph_bg, "button_glyph_bg_color", row);
                if (changed) {
                    if (row == JW_COLOR_ACCENT)
                        cat_set_theme_color(NULL);
                    cat_finalize_theme_colors(t);
                    /* Hand-editing a color diverges from any preset → "Custom". */
                    if (ui->color_scheme_index != -1) {
                        ui->color_scheme_index = -1;
                        jw__persist_int(ui, "color_scheme_index", -1);
                    }
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_APPEARANCE;
                break;
            default: break;
        }
        break;

    /* ── Layout ──────────────────────────────────────────────────────── */
    case JW_SETTINGS_LAYOUT:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->layout_list, -1, JW_LAYOUT_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->layout_list, +1, JW_LAYOUT_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                int row = ui->layout_list.cursor;
                if (row == JW_LAYOUT_PILL_SHAPE) {
                    int next = (ui->pill_shape_index + dir + JW_SETTINGS_PILL_SHAPE_COUNT) % JW_SETTINGS_PILL_SHAPE_COUNT;
                    ui->pill_shape_index = next;
                    cat_get_theme()->pill_radius_ratio = kJawakaPillRadiusValues[next];
                    cat_get_theme()->pill_corner_mask  = kJawakaPillCornerMasks[next];
                    jw__persist_int(ui, "pill_shape_index", next);
                } else if (row == JW_LAYOUT_FONT_FAMILY) {
                    int next = (ui->font_family_index + dir + JW_APPEARANCE_FONT_FAMILY_COUNT) %
                               JW_APPEARANCE_FONT_FAMILY_COUNT;
                    const char *path = jw_appearance_font_path_for_index(next);
                    if (cat_reload_fonts(path) == CAT_OK) {
                        ui->font_family_index = next;
                        jw__persist_int(ui, "font_family_index", next);
                    } else if (status_buf && status_size > 0) {
                        snprintf(status_buf, status_size, "%s", "font load failed");
                    }
                } else if (row == JW_LAYOUT_FONT_SIZE) {
                    int next = (ui->font_size_index + dir + JW_SETTINGS_FONT_SIZE_COUNT) % JW_SETTINGS_FONT_SIZE_COUNT;
                    ui->font_size_index = next;
                    cat_set_font_bump(kJawakaFontSizeValues[next]);
                    jw__persist_int(ui, "font_size_index", next);
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_APPEARANCE;
                break;
            default: break;
        }
        break;

    /* ── Status Bar ──────────────────────────────────────────────────── */
    case JW_SETTINGS_STATUS_BAR:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->statusbar_list, -1, JW_STATUSBAR_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->statusbar_list, +1, JW_STATUSBAR_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                int row = ui->statusbar_list.cursor;
                if (row == JW_STATUSBAR_HINTS) {
                    ui->show_hints = !ui->show_hints;
                    jw__persist_bool(ui, "show_hints", ui->show_hints);
                } else if (row == JW_STATUSBAR_CLOCK) {
                    int next = (ui->clock_style_index + dir + JW_SETTINGS_CLOCK_STYLE_COUNT) % JW_SETTINGS_CLOCK_STYLE_COUNT;
                    ui->clock_style_index = next;
                    jw__persist_int(ui, "clock_style_index", next);
                } else if (row == JW_STATUSBAR_BATTERY) {
                    int mode = jw__battery_mode(ui->show_battery, ui->show_battery_level);
                    mode = (mode + dir + JW_BATTERY_MODE_COUNT) % JW_BATTERY_MODE_COUNT;
                    jw__battery_mode_to_flags(mode, &ui->show_battery, &ui->show_battery_level);
                    jw__persist_bool(ui, "show_battery", ui->show_battery);
                    jw__persist_bool(ui, "show_battery_level", ui->show_battery_level);
                } else if (row == JW_STATUSBAR_WIFI) {
                    ui->show_wifi = !ui->show_wifi;
                    jw__persist_bool(ui, "show_wifi", ui->show_wifi);
                } else if (row == JW_STATUSBAR_VOLUME) {
                    ui->show_volume = !ui->show_volume;
                    jw__persist_bool(ui, "show_volume", ui->show_volume);
                    if (ui->show_volume) jw__refresh_volume(ui);
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_APPEARANCE;
                break;
            default: break;
        }
        break;

    /* ── Display & Sound (brightness + volume) ───────────────────────── */
    case JW_SETTINGS_DISPLAY:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->display_list, -1, JW_DISPLAY_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->display_list, +1, JW_DISPLAY_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                if (ui->display_list.cursor == JW_DISPLAY_BRIGHTNESS)
                    jw__change_brightness(ui, dir * JW_PLATFORM_BRIGHTNESS_STEP_PERCENT,
                                          status_buf, status_size);
                else if (ui->display_list.cursor == JW_DISPLAY_VOLUME)
                    jw__change_volume(ui, dir * JW_PLATFORM_VOLUME_STEP_PERCENT,
                                      status_buf, status_size);
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Network (Wi-Fi scan/connect + ADB access) ───────────────────── */
    case JW_SETTINGS_NETWORK: {
        int row_count = JW_NETWORK_FIXED_ROWS +
                        (ui->wifi_radio_on ? ui->wifi_network_count : 0);
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->network_list, -1, row_count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->network_list, +1, row_count);
                break;
            case CAT_BTN_A: {
                /* Row 0: toggle the radio. (Blocks briefly; turning OFF will drop
                   an ADB-over-Wi-Fi session — expected.) */
                if (ui->network_list.cursor == JW_NETWORK_ROW_WIFI) {
                    bool turning_on = !ui->wifi_radio_on;
                    jw__wifi_msg(ui, turning_on ? "Turning Wi-Fi on…"
                                                : "Turning Wi-Fi off…");
                    snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                    jw__wifi_attempt_clear(ui);
                    jw_wifi_set_radio(turning_on);
                    ui->wifi_radio_on = jw_wifi_radio_is_on();
                    ui->network_list.cursor = 0;
                    ui->wifi_next_poll_ms = SDL_GetTicks();
                    break;
                }

                if (ui->network_list.cursor == JW_NETWORK_ROW_ADB) {
                    bool enable = !(ui->adb_enabled == 1);
                    jw__set_adb(ui, enable, status_buf, status_size);
                    jw__wifi_msg(ui, "%s", status_buf && status_buf[0]
                                           ? status_buf : (enable ? "ADB enabled" : "ADB disabled"));
                    break;
                }

                /* Later rows: connect the selected network (open/saved direct; a
                   secured network with no saved profile prompts for the key). */
                int ni = ui->network_list.cursor - JW_NETWORK_FIXED_ROWS;
                if (!ui->wifi_radio_on || ni < 0 || ni >= ui->wifi_network_count) {
                    break;
                }
                const jw_wifi_network_t *net = &ui->wifi_networks[ni];

                if (net->current && ui->wifi.connected) {
                    /* A on the connected network disconnects it. */
                    if (jw_wifi_disconnect() == 0)
                        jw__wifi_msg(ui,
                                 "Disconnected from %s", net->ssid);
                    else
                        jw__wifi_msg(ui,
                                 "Could not disconnect");
                    jw__wifi_attempt_clear(ui);
                    snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                    ui->wifi_next_poll_ms = SDL_GetTicks();
                    break;
                }

                jw_wifi_connect_result r = jw_wifi_connect(net->ssid, net->secured);
                if (r == JW_WIFI_CONNECT_NEED_PASSWORD) {
                    cat_keyboard_result kb;
                    char prompt[96];
                    snprintf(prompt, sizeof(prompt), "Password for %s | B: Cancel",
                             net->ssid);
                    if (cat_keyboard("", prompt, CAT_KB_GENERAL, &kb) == CAT_OK &&
                        kb.text[0]) {
                        r = jw_wifi_connect_psk(net->ssid, kb.text);
                    } else {
                        jw__wifi_msg(ui, "Cancelled");
                        snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                        break;
                    }
                }

                if (r == JW_WIFI_CONNECT_OK) {
                    jw__wifi_msg(ui,
                             "Connecting to %s…", net->ssid);
                    /* Track the attempt + attach the event monitor so the poll can
                       confirm success or catch a WRONG_KEY auth failure. */
                    jw__wifi_attempt_begin(ui, net->ssid);
                } else {
                    jw__wifi_msg(ui,
                             "Could not connect to %s", net->ssid);
                }
                snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                ui->wifi_next_poll_ms = SDL_GetTicks();   /* poll right away */
                break;
            }
            case CAT_BTN_Y: {
                /* Forget the selected network's saved profile (scan rows only). */
                int ni = ui->network_list.cursor - JW_NETWORK_FIXED_ROWS;
                if (ui->wifi_radio_on && ni >= 0 && ni < ui->wifi_network_count) {
                    const jw_wifi_network_t *net = &ui->wifi_networks[ni];
                    if (net->saved) {
                        if (jw_wifi_forget(net->ssid) == 0)
                            jw__wifi_msg(ui,
                                     "Forgot %s", net->ssid);
                        else
                            jw__wifi_msg(ui,
                                     "Could not forget %s", net->ssid);
                        jw_wifi_scan_start();
                        jw__refresh_wifi(ui);
                        jw__refresh_wifi_scan(ui);
                    } else {
                        jw__wifi_msg(ui,
                                 "%s isn't saved", net->ssid);
                    }
                    snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                }
                break;
            }
            case CAT_BTN_X:
                if (!ui->wifi_radio_on) {
                    break;   /* nothing to scan with the radio off */
                }
                jw_wifi_scan_start();
                jw__refresh_wifi(ui);
                jw__refresh_wifi_scan(ui);
                ui->wifi_next_scan_ms = SDL_GetTicks() + JW_WIFI_SCAN_INTERVAL_MS;
                jw__wifi_msg(ui, "Scanning Wi-Fi…");
                snprintf(status_buf, status_size, "Scanning Wi-Fi…");
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;
    }

    /* ── Lighting (LED ring) ─────────────────────────────────────────── */
    case JW_SETTINGS_LIGHTING:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->lighting_list, -1, JW_LIGHTING_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->lighting_list, +1, JW_LIGHTING_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                int row = ui->lighting_list.cursor;
                bool changed = true;
                if (row == JW_LIGHTING_ENABLE) {
                    ui->led_enabled = !ui->led_enabled;
                } else if (row == JW_LIGHTING_MODE) {
                    ui->led_mode = (ui->led_mode + dir + JW_LED_MODE_COUNT) % JW_LED_MODE_COUNT;
                } else if (row == JW_LIGHTING_COLOR) {
                    if (button == CAT_BTN_A)
                        changed = jw__pick_color(ui, &ui->led_color, "led_color", -1);
                    else
                        changed = false;
                } else if (row == JW_LIGHTING_BRIGHTNESS) {
                    int next = ui->led_brightness + dir;
                    if (next < 0) next = 0;
                    if (next > JW_LED_BRIGHTNESS_MAX) next = JW_LED_BRIGHTNESS_MAX;
                    changed = (next != ui->led_brightness);
                    ui->led_brightness = next;
                } else if (row == JW_LIGHTING_SPEED) {
                    int next = ui->led_speed + dir;
                    if (next < 0) next = 0;
                    if (next > JW_LED_SPEED_MAX) next = JW_LED_SPEED_MAX;
                    changed = (next != ui->led_speed);
                    ui->led_speed = next;
                }
                if (changed) jw__apply_led(ui);
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Library ─────────────────────────────────────────────────────── */
    case JW_SETTINGS_LIBRARY:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->library_list, -1, JW_LIBRARY_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->library_list, +1, JW_LIBRARY_ROW_COUNT);
                break;
            case CAT_BTN_A:
                if (ui->library_list.cursor == JW_LIBRARY_RESET_RETROARCH) {
                    jw__reset_retroarch_config(ui, status_buf, status_size);
                } else if (ui->library_list.cursor == JW_LIBRARY_UNMOUNT_SECONDARY) {
                    jw__safe_unmount_secondary_sd(ui, status_buf, status_size);
                }
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── Accounts (placeholder) ──────────────────────────────────────── */
    case JW_SETTINGS_ACCOUNTS:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->accounts_list, -1, JW_ACCOUNTS_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->accounts_list, +1, JW_ACCOUNTS_ROW_COUNT);
                break;
            case CAT_BTN_A:
                /* Sign-in not implemented yet — report it instead of pretending. */
                snprintf(status_buf, status_size, "Sign-in coming soon");
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── About ───────────────────────────────────────────────────────── */
    case JW_SETTINGS_ABOUT: {
        int line_h = TTF_FontHeight(cat_get_font(CAT_FONT_SMALL)) + cat_scale(8);
        switch (button) {
            case CAT_BTN_UP:   cat_scroll_state_move(&ui->about_scroll, -line_h); break;
            case CAT_BTN_DOWN: cat_scroll_state_move(&ui->about_scroll, +line_h); break;
            case CAT_BTN_B:    ui->screen = JW_SETTINGS_HOME; break;
            default: break;
        }
        break;
    }

    /* ── Behavior ────────────────────────────────────────────────────── */
    case JW_SETTINGS_BEHAVIOR:
        switch (button) {
            case CAT_BTN_UP:   cat_list_state_move(&ui->behavior_list, -1, JW_BEHAVIOR_ROW_COUNT); break;
            case CAT_BTN_DOWN: cat_list_state_move(&ui->behavior_list, +1, JW_BEHAVIOR_ROW_COUNT); break;
            case CAT_BTN_LEFT:
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                if (ui->behavior_list.cursor == JW_BEHAVIOR_STARTUP_TAB) {
                    int next = (ui->startup_tab_index + dir + JW_STARTUP_TAB_COUNT)
                               % JW_STARTUP_TAB_COUNT;
                    ui->startup_tab_index = next;
                    jw__persist_int(ui, "startup_tab_index", next);
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_AUTO_SLEEP) {
                    int next = (ui->auto_sleep_index + dir + JW_AUTO_SLEEP_COUNT)
                               % JW_AUTO_SLEEP_COUNT;
                    ui->auto_sleep_index = next;
                    /* Persist the seconds value (the daemon reads it directly). */
                    jw__persist_int(ui, "auto_sleep_seconds", kAutoSleepSeconds[next]);
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;
    }

    return ui->open;
}
