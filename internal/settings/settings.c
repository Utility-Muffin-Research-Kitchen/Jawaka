#include "internal/settings/settings.h"

#include "internal/db/db.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/device.h"
#include "internal/platform/platform_id.h"
#include "internal/scrape/scrape_catalog.h"
#include "internal/settings/appearance.h"
#include "cJSON.h"

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
    "Hidden",
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

/* Two ROYGBIV runs, Leaf leading: seven dark schemes (Leaf, then the spectrum in
   R-O-Y-B-I-V order), then the seven light schemes in the same order so each light
   sits a hue-twin of the dark above it. The selected-row text auto-contrast in
   cat_finalize_theme_colors picks the readable one of {text,bg}, so dark text lands
   on the bright selection pill on both dark and light pages. */
static const jw__color_scheme kColorSchemes[] = {
    /* name        accent     bg         text       hint       selection  btn_label  btn_bg     */
    /* Dark */
    { "Leaf",      "#1E331E", "#0F160E", "#E8F1E3", "#7E9579", "#7FB069", "#0F160E", "#7FB069" },
    { "Rose",      "#33222E", "#1C1620", "#F0E6EC", "#A88A98", "#EB6F92", "#1C1620", "#EB6F92" },
    { "Ember",     "#3A2A22", "#1A1413", "#F2EAE2", "#A38A7A", "#FF8A4C", "#1A1413", "#FF8A4C" },
    { "Goldenrod", "#332C16", "#15120A", "#F2EEDD", "#A89A6E", "#E8C24A", "#15120A", "#E8C24A" },
    { "Tide",      "#173042", "#0D1620", "#E2EEF4", "#6E8A9A", "#4FA3E0", "#0D1620", "#4FA3E0" },
    { "Indigo",    "#232544", "#131426", "#E6E7F4", "#7C7EA0", "#818CF5", "#131426", "#818CF5" },
    { "Orchid",    "#2E2240", "#181226", "#ECE4F2", "#8E7CB0", "#C792EA", "#181226", "#C792EA" },
    /* Light */
    { "Meadow",    "#9CCB85", "#D1D0A6", "#1B2E1B", "#5E7654", "#7FB069", "#1B2E1B", "#7FB069" },
    { "Petal",     "#F2BFCE", "#DEC9D0", "#3A2630", "#8A6E78", "#E886A4", "#3A2630", "#E886A4" },
    { "Apricot",   "#EAC99C", "#E6D2B0", "#332518", "#8A7050", "#EE9F54", "#332518", "#EE9F54" },
    { "Wheat",     "#E6D38A", "#ECE6C2", "#332C12", "#897F50", "#CDB23E", "#332C12", "#CDB23E" },
    { "Sky",       "#9CC3E0", "#BFCED7", "#1B2A33", "#5A7280", "#6FA8DC", "#1B2A33", "#6FA8DC" },
    { "Periwinkle","#BFC0E6", "#CFCFE2", "#22243A", "#6E708E", "#8088E6", "#22243A", "#8088E6" },
    { "Lavender",  "#D0BCE4", "#D2C9DC", "#2A2238", "#786E8A", "#BE8FE2", "#2A2238", "#BE8FE2" },
};
#define JW_COLOR_SCHEME_COUNT ((int)(sizeof(kColorSchemes) / sizeof(kColorSchemes[0])))
#define JW_COLOR_SCHEME_DEFAULT 0   /* Leaf — the Dweezil/Leaf identity theme */

static void jw__apply_color_scheme(jw_settings_ui *ui, int index, bool *theme_changed);

#define JW_SETTINGS_VALUE_MAX 64
typedef enum {
    JW_SETTING_PILL_SHAPE_INDEX = 0,
    JW_SETTING_FONT_FAMILY_INDEX,
    JW_SETTING_FONT_SIZE_INDEX,
    JW_SETTING_SHOW_HINTS,
    JW_SETTING_CLOCK_STYLE_INDEX,
    JW_SETTING_SHOW_BATTERY,
    JW_SETTING_SHOW_BATTERY_LEVEL,
    JW_SETTING_SHOW_WIFI,
    JW_SETTING_SHOW_BLUETOOTH,
    JW_SETTING_SHOW_VOLUME,
    JW_SETTING_COLOR_SCHEME_INDEX,
    JW_SETTING_STARTUP_TAB_INDEX,
    JW_SETTING_AUTO_SLEEP_SECONDS,
    JW_SETTING_BOOT_SPLASH_ENABLED,
    JW_SETTING_GAME_PERFORMANCE_PROFILE,
    JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT,
    JW_SETTING_PLATFORM_VOLUME_PERCENT,
    JW_SETTING_ACCENT_COLOR,
    JW_SETTING_BG_COLOR,
    JW_SETTING_TEXT_COLOR,
    JW_SETTING_HINT_COLOR,
    JW_SETTING_HIGHLIGHT_COLOR,
    JW_SETTING_BUTTON_LABEL_COLOR,
    JW_SETTING_BUTTON_GLYPH_BG_COLOR,
    JW_SETTING_TIMEZONE,
    JW_SETTING_SS_USER,
    JW_SETTING_SS_PASS,
    JW_SETTING_SS_VERIFIED,
    JW_SETTING_SS_MAXTHREADS,
    JW_SETTING_SS_REQUESTS_TODAY,
    JW_SETTING_SS_MAX_REQUESTS,
    JW_SETTING_SCRAPE_ARTWORK_PRIO,
    JW_SETTING_SCRAPE_REGION_PRIO,
    JW_SETTING_RA_USER,
    JW_SETTING_RA_PASS,
    JW_SETTING_TAB_GLIDE,
    JW_SETTING_REFRESH_RATE_HZ,
    JW_SETTING_COUNT,
} jw__setting_key;

static const char *const kSettingKeys[JW_SETTING_COUNT] = {
    [JW_SETTING_PILL_SHAPE_INDEX] = "pill_shape_index",
    [JW_SETTING_FONT_FAMILY_INDEX] = "font_family_index",
    [JW_SETTING_FONT_SIZE_INDEX] = "font_size_index",
    [JW_SETTING_SHOW_HINTS] = "show_hints",
    [JW_SETTING_CLOCK_STYLE_INDEX] = "clock_style_index",
    [JW_SETTING_SHOW_BATTERY] = "show_battery",
    [JW_SETTING_SHOW_BATTERY_LEVEL] = "show_battery_level",
    [JW_SETTING_SHOW_WIFI] = "show_wifi",
    [JW_SETTING_SHOW_BLUETOOTH] = "show_bluetooth",
    [JW_SETTING_SHOW_VOLUME] = "show_volume",
    [JW_SETTING_COLOR_SCHEME_INDEX] = "color_scheme_index",
    [JW_SETTING_STARTUP_TAB_INDEX] = "startup_tab_index",
    [JW_SETTING_AUTO_SLEEP_SECONDS] = "auto_sleep_seconds",
    [JW_SETTING_BOOT_SPLASH_ENABLED] = "boot_splash_enabled",
    [JW_SETTING_GAME_PERFORMANCE_PROFILE] = "platform.performance.game_profile",
    [JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT] = "platform.brightness_percent",
    [JW_SETTING_PLATFORM_VOLUME_PERCENT] = "platform.volume_percent",
    [JW_SETTING_ACCENT_COLOR] = "accent_color",
    [JW_SETTING_BG_COLOR] = "bg_color",
    [JW_SETTING_TEXT_COLOR] = "text_color",
    [JW_SETTING_HINT_COLOR] = "hint_color",
    [JW_SETTING_HIGHLIGHT_COLOR] = "highlight_color",
    [JW_SETTING_BUTTON_LABEL_COLOR] = "button_label_color",
    [JW_SETTING_BUTTON_GLYPH_BG_COLOR] = "button_glyph_bg_color",
    [JW_SETTING_TIMEZONE] = "timezone",
    [JW_SETTING_SS_USER] = "screenscraper_user",
    [JW_SETTING_SS_PASS] = "screenscraper_pass",
    [JW_SETTING_SS_VERIFIED] = "screenscraper_verified",
    [JW_SETTING_SS_MAXTHREADS] = "screenscraper_maxthreads",
    [JW_SETTING_SS_REQUESTS_TODAY] = "screenscraper_requests_today",
    [JW_SETTING_SS_MAX_REQUESTS] = "screenscraper_max_requests",
    [JW_SETTING_SCRAPE_ARTWORK_PRIO] = "scrape.artwork_priority",
    [JW_SETTING_SCRAPE_REGION_PRIO] = "scrape.region_priority",
    [JW_SETTING_RA_USER] = "retroachievements_user",
    [JW_SETTING_RA_PASS] = "retroachievements_pass",
    [JW_SETTING_TAB_GLIDE] = "tab_glide",
    [JW_SETTING_REFRESH_RATE_HZ] = "refresh_rate_hz",
};

static const char *const kTabSwitchLabels[] = { "Snap", "Glide" };
#define JW_TAB_SWITCH_COUNT 2

/* Curated time-zone list for Settings > Behavior > Time Zone. Each entry maps a
   friendly label to an IANA zone id, exported as the TZ environment variable. The
   clock uses localtime(), which honors TZ, so picking a zone corrects the clock
   instantly and flows to launched apps. zoneinfo for every entry ships in the
   rootfs (/usr/share/zoneinfo), so no data needs bundling. ASCII labels only
   (the launcher font subset has no extended-Latin glyphs). */
/* Ordered by UTC (standard-time) offset, the convention OS time-zone pickers use.
   `off` is the displayed base offset (DST shifts it at runtime); ASCII only. */
typedef struct { const char *label; const char *tz; const char *off; } jw__timezone_entry;
static const jw__timezone_entry kTimeZones[] = {
    { "US Hawaii",          "Pacific/Honolulu",    "UTC-10"   },
    { "US Alaska",          "America/Anchorage",   "UTC-9"    },
    { "US Pacific",         "America/Los_Angeles", "UTC-8"    },
    { "US Mountain",        "America/Denver",      "UTC-7"    },
    { "US Arizona",         "America/Phoenix",     "UTC-7"    },
    { "US Central",         "America/Chicago",     "UTC-6"    },
    { "US Eastern",         "America/New_York",    "UTC-5"    },
    { "Brazil (East)",      "America/Sao_Paulo",   "UTC-3"    },
    { "UTC",                "UTC",                 "UTC"      },
    { "UK / Ireland",       "Europe/London",       "UTC+0"    },
    { "Central Europe",     "Europe/Paris",        "UTC+1"    },
    { "Eastern Europe",     "Europe/Athens",       "UTC+2"    },
    { "India",              "Asia/Kolkata",        "UTC+5:30" },
    { "China",              "Asia/Shanghai",       "UTC+8"    },
    { "Japan / Korea",      "Asia/Tokyo",          "UTC+9"    },
    { "Sydney",             "Australia/Sydney",    "UTC+10"   },
};
#define JW_TIMEZONE_COUNT ((int)(sizeof(kTimeZones) / sizeof(kTimeZones[0])))
/* Rows visible at once in the picker pane (the list scrolls past this). Matches
   the System Update picker, which uses the same two-line item height + pane. */
#define JW_TIMEZONE_VISIBLE_ROWS 7

static const char *jw__timezone_label(const char *tz) {
    if (!tz || !tz[0]) return "System default";
    for (int i = 0; i < JW_TIMEZONE_COUNT; ++i)
        if (strcmp(kTimeZones[i].tz, tz) == 0) return kTimeZones[i].label;
    return tz;   /* unknown id: show the raw zone */
}

static int jw__timezone_index_of(const char *tz) {
    if (tz && tz[0])
        for (int i = 0; i < JW_TIMEZONE_COUNT; ++i)
            if (strcmp(kTimeZones[i].tz, tz) == 0) return i;
    return 0;
}

/* Set TZ and refresh libc's timezone state so the very next localtime() (the
   status-bar clock) reflects the new zone without a restart. Empty tz leaves the
   system default in place. */
static void jw__apply_timezone(const char *tz) {
    if (tz && tz[0]) setenv("TZ", tz, 1);
    tzset();
}

static bool jw__setting_has(char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX],
                            const unsigned char found[JW_SETTING_COUNT],
                            jw__setting_key key) {
    return found[key] && values[key][0];
}

static int jw__load_setting_values(const char *db_path,
                                   char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX],
                                   unsigned char found[JW_SETTING_COUNT]) {
    memset(values, 0, sizeof(char) * JW_SETTING_COUNT * JW_SETTINGS_VALUE_MAX);
    memset(found, 0, sizeof(unsigned char) * JW_SETTING_COUNT);
    if (!db_path || !db_path[0]) {
        return -1;
    }

    jw_db_setting_query queries[JW_SETTING_COUNT];
    for (int i = 0; i < JW_SETTING_COUNT; i++) {
        queries[i].key = kSettingKeys[i];
        queries[i].out = values[i];
        queries[i].out_size = JW_SETTINGS_VALUE_MAX;
        queries[i].found = 0;
    }
    if (jw_db_get_settings(db_path, queries, JW_SETTING_COUNT) != 0) {
        return -1;
    }
    for (int i = 0; i < JW_SETTING_COUNT; i++) {
        found[i] = queries[i].found ? 1 : 0;
    }
    return 0;
}

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
#define JW_AUTO_SLEEP_DEFAULT 0   /* Off by default (index into the tables above).
                                     Deep-suspend wake is not yet reliable, so
                                     auto-sleep stays opt-in until that is solid. */

static const jw_platform_perf_profile kGamePerfProfiles[] = {
    JW_PLATFORM_PERF_PROFILE_AUTO,
    JW_PLATFORM_PERF_PROFILE_BALANCED,
    JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
    JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
};
#define JW_GAME_PERF_PROFILE_COUNT \
    ((int)(sizeof(kGamePerfProfiles) / sizeof(kGamePerfProfiles[0])))
#define JW_GAME_PERF_PROFILE_DEFAULT 0

static int jw__game_perf_index_for_profile(jw_platform_perf_profile profile) {
    for (int i = 0; i < JW_GAME_PERF_PROFILE_COUNT; i++) {
        if (kGamePerfProfiles[i] == profile) {
            return i;
        }
    }
    return JW_GAME_PERF_PROFILE_DEFAULT;
}

/* Top-level Settings categories, grouped by theme: look & feel, connectivity,
   games & content, system. The A handler maps each row to its screen by row
   index (a positional `idx == N` chain), so this label order and those checks
   must stay in lockstep — reordering here means renumbering there. */
static const char *kHomeCategoryLabels[] = {
    "Appearance",
    "Display & Sound",
    "Lighting",
    "Network",
    "Bluetooth",
    "Game Art",
    "Accounts",
    "General",
};
/* System Update and About are not listed here — they live in the System menu
   (the Menu-button popup), hosted there via jw_settings_ui_open(). */
#define JW_SETTINGS_CATEGORY_COUNT 8

/* Visible rows in the Network page's scanned-network list (scrolls beyond). */
#define JW_WIFI_LIST_ROWS 6
#define JW_NETWORK_ROW_WIFI 0
#define JW_NETWORK_ROW_ADB  1
#define JW_NETWORK_FIXED_ROWS 2
/* Re-trigger a background scan at most this often while the page is open. */
#define JW_WIFI_SCAN_INTERVAL_MS 6000
#define JW_BT_POLL_INTERVAL_MS 2000
#define JW_BT_SCAN_INTERVAL_MS 12000
#define JW_BT_ENTRY_DEFER_MS 250
#define JW_SETTINGS_DISPLAY_COUNT JW_DISPLAY_ROW_COUNT
#define JW_UPDATE_PICKER_VISIBLE_ROWS 7

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

static int jw__scrape_csv_to_order(const char *csv,
                                   const jw_ss_option *catalog, int catalog_count,
                                   const char *const *fallback, int fallback_count,
                                   int *order);

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

static void jw__refresh_audio_status(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) return;
    jw_ipc_audio_status status;
    if (jw_ipc_platform_audio_status(ui->socket_path, &status) != 0) {
        return;
    }

    ui->audio_output = status.output;
    ui->audio_available_outputs = status.available_outputs;
    ui->test_sound_playing = (status.test_playing != 0);
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        ui->audio_volumes[i] = status.volume_percent[i];
    }
    if (ui->audio_output >= 0 &&
        ui->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT &&
        ui->audio_volumes[ui->audio_output] >= 0) {
        ui->volume_percent = ui->audio_volumes[ui->audio_output];
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
    ui->adb_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    int enabled = -1;
    int intent = -1;
    bool supported = false;
    if (jw_ipc_get_adb(ui->socket_path, &enabled, &intent, &supported) == 0) {
        ui->adb_supported = supported;
        ui->adb_enabled = enabled;
        ui->adb_intent_enabled = intent;
    }
}

static void jw__refresh_boot_splash(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->boot_splash_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    int enabled = -1;
    bool supported = false;
    if (jw_ipc_get_boot_splash(ui->socket_path, &enabled, &supported) == 0) {
        ui->boot_splash_supported = supported;
        if (enabled >= 0) {
            ui->boot_splash_enabled = enabled != 0;
        }
    }
}

static void jw__refresh_refresh_rate(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->refresh_rate_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    int hz = -1;
    bool supported = false;
    if (jw_ipc_get_refresh_rate(ui->socket_path, &hz, &supported) == 0) {
        ui->refresh_rate_supported = supported;
        /* Reflect the panel's actual current rate when the daemon reports it
           (truth over the persisted mirror, e.g. after moving the card). */
        if (hz == 60 || hz == 90 || hz == 120) {
            ui->refresh_rate_hz = hz;
        }
    }
}

static void jw__refresh_performance(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    ui->performance_supported = false;
    if (!ui->socket_path[0]) {
        return;
    }

    jw_ipc_performance_status_info status;
    if (jw_ipc_get_performance_status(ui->socket_path, &status, NULL, 0) != 0) {
        return;
    }
    ui->performance_supported = status.supported;
    jw_platform_perf_profile profile;
    if (jw_platform_parse_perf_profile(status.global_profile, &profile)) {
        ui->game_perf_profile = jw__game_perf_index_for_profile(profile);
    }
}

static void jw__update_msg(jw_settings_ui *ui, const char *fmt, ...) {
    if (!ui) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->update_msg, sizeof(ui->update_msg), fmt ? fmt : "", ap);
    va_end(ap);
    ui->update_msg_ms = SDL_GetTicks();
    if (ui->update_msg_ms == 0) {
        ui->update_msg_ms = 1;
    }
}

static void jw__refresh_update_status(jw_settings_ui *ui, bool quiet) {
    if (!ui) {
        return;
    }
    if (!ui->socket_path[0]) {
        ui->update_have_status = false;
        if (!quiet) {
            jw__update_msg(ui, "Update service unavailable");
        }
        return;
    }

    char status[192] = { 0 };
    if (jw_ipc_update_status(ui->socket_path, &ui->update,
                             status, sizeof(status)) == 0) {
        ui->update_have_status = true;
        if (!quiet && status[0]) {
            jw__update_msg(ui, "%s", status);
        }
    } else {
        ui->update_have_status = false;
        if (!quiet) {
            jw__update_msg(ui, "%s", status[0] ? status : "Update status unavailable");
        }
    }
}

bool jw_settings_ui_wants_update_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_UPDATE &&
           (ui->update.download_active || ui->update.install_active);
}

void jw_settings_ui_refresh_update(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }

    if (ui->update_msg_ms && (int)(SDL_GetTicks() - ui->update_msg_ms) > 8000) {
        ui->update_msg[0] = '\0';
        ui->update_msg_ms = 0;
    }

    unsigned now = SDL_GetTicks();
    if (ui->update_next_poll_ms != 0 &&
        (int)(now - ui->update_next_poll_ms) < 0) {
        return;
    }

    jw__refresh_update_status(ui, true);
    ui->update_next_poll_ms =
        now + (jw_settings_ui_wants_update_poll(ui) ? 500u : 3000u);
    if (jw_settings_ui_wants_update_poll(ui)) {
        cat_request_frame_in(250);
    }
}

static int jw__bt_row_count(const jw_settings_ui *ui) {
    if (!ui || !ui->bt_radio_on) {
        return JW_BLUETOOTH_FIXED_ROWS;
    }
    /* Power + name + Paired header + paired devices + Nearby header + nearby devices. */
    return JW_BLUETOOTH_FIXED_ROWS + 2 + ui->bt_paired_count + ui->bt_nearby_count;
}

static void jw__bt_msg(jw_settings_ui *ui, const char *fmt, ...) {
    if (!ui) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->bt_msg, sizeof(ui->bt_msg), fmt, ap);
    va_end(ap);
    ui->bt_msg_ms = SDL_GetTicks();
    if (ui->bt_msg_ms == 0) {
        ui->bt_msg_ms = 1;
    }
}

static const jw_bt_device_t *jw__bt_cached_device_in_lists(
        const jw_bt_device_t *paired, int paired_count,
        const jw_bt_device_t *nearby, int nearby_count,
        const char *mac) {
    if (!mac || !jw_bt_mac_valid(mac)) {
        return NULL;
    }
    for (int i = 0; paired && i < paired_count; i++) {
        if (strcmp(paired[i].mac, mac) == 0) {
            return &paired[i];
        }
    }
    for (int i = 0; nearby && i < nearby_count; i++) {
        if (strcmp(nearby[i].mac, mac) == 0) {
            return &nearby[i];
        }
    }
    return NULL;
}

static bool jw__bt_name_is_mac(const char *name) {
    return name && jw_bt_mac_valid(name);
}

static void jw__bt_merge_summary_device(const jw_bt_device_t *old_paired,
                                        int old_paired_count,
                                        const jw_bt_device_t *old_nearby,
                                        int old_nearby_count,
                                        const jw_bt_device_t *summary,
                                        jw_bt_device_t *out) {
    if (!summary || !out) {
        return;
    }

    const jw_bt_device_t *cached = jw__bt_cached_device_in_lists(
        old_paired, old_paired_count, old_nearby, old_nearby_count, summary->mac);
    if (cached) {
        *out = *cached;
    } else {
        *out = *summary;
    }

    if (summary->name[0] &&
        (!out->name[0] || jw__bt_name_is_mac(out->name) ||
         !jw__bt_name_is_mac(summary->name))) {
        snprintf(out->name, sizeof(out->name), "%s", summary->name);
    }
    if (summary->alias[0] &&
        (!out->alias[0] || jw__bt_name_is_mac(out->alias) ||
         !jw__bt_name_is_mac(summary->alias))) {
        snprintf(out->alias, sizeof(out->alias), "%s", summary->alias);
    }
    if ((out->kind == JW_BT_DEVICE_UNKNOWN || out->kind == JW_BT_DEVICE_OTHER) &&
        summary->kind != JW_BT_DEVICE_UNKNOWN &&
        summary->kind != JW_BT_DEVICE_OTHER) {
        out->kind = summary->kind;
    }

    snprintf(out->mac, sizeof(out->mac), "%s", summary->mac);
    out->paired = summary->paired;
    out->connected = summary->connected;
}

static void jw__bt_merge_summary_list(const jw_bt_device_t *old_paired,
                                      int old_paired_count,
                                      const jw_bt_device_t *old_nearby,
                                      int old_nearby_count,
                                      jw_bt_device_t *dst,
                                      int *dst_count,
                                      const jw_bt_device_t *summary,
                                      int summary_count) {
    if (!dst || !dst_count || !summary || summary_count < 0) {
        return;
    }
    int count = summary_count > JW_BT_MAX_DEVICES ? JW_BT_MAX_DEVICES : summary_count;
    jw_bt_device_t merged[JW_BT_MAX_DEVICES];
    for (int i = 0; i < count; i++) {
        jw__bt_merge_summary_device(old_paired, old_paired_count,
                                    old_nearby, old_nearby_count,
                                    &summary[i], &merged[i]);
    }
    memcpy(dst, merged, sizeof(jw_bt_device_t) * (size_t)count);
    *dst_count = count;
}

static void jw__refresh_bluetooth_lists(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }

    if (!jw_bt_available()) {
        memset(&ui->bt_status, 0, sizeof(ui->bt_status));
        ui->bt_radio_on = false;
        ui->bt_paired_count = 0;
        ui->bt_nearby_count = 0;
        ui->bluetooth_list.cursor = 0;
        ui->bluetooth_list.scroll_offset = 0;
        return;
    }

    int rows = 0;
    jw_bt_status_t status;
    if (jw_bt_status(&status) != 0) {
        if (!ui->bt_status.available &&
            ui->bt_paired_count == 0 &&
            ui->bt_nearby_count == 0) {
            ui->bt_radio_on = false;
        }
        goto clamp_cursor;
    }

    ui->bt_status = status;
    ui->bt_radio_on = ui->bt_status.powered || jw_bt_radio_is_on();
    if (!ui->bt_radio_on) {
        ui->bt_paired_count = 0;
        ui->bt_nearby_count = 0;
        goto clamp_cursor;
    }

    jw_bt_device_t paired[JW_BT_MAX_DEVICES];
    jw_bt_device_t nearby[JW_BT_MAX_DEVICES];
    int paired_count = 0;
    int nearby_count = 0;
    if (jw_bt_list_summaries(paired, JW_BT_MAX_DEVICES, &paired_count,
                             nearby, JW_BT_MAX_DEVICES, &nearby_count) == 0) {
        jw_bt_device_t old_paired[JW_BT_MAX_DEVICES];
        jw_bt_device_t old_nearby[JW_BT_MAX_DEVICES];
        int old_paired_count = ui->bt_paired_count;
        int old_nearby_count = ui->bt_nearby_count;
        memcpy(old_paired, ui->bt_paired, sizeof(old_paired));
        memcpy(old_nearby, ui->bt_nearby, sizeof(old_nearby));
        jw__bt_merge_summary_list(old_paired, old_paired_count,
                                  old_nearby, old_nearby_count,
                                  ui->bt_paired, &ui->bt_paired_count,
                                  paired, paired_count);
        jw__bt_merge_summary_list(old_paired, old_paired_count,
                                  old_nearby, old_nearby_count,
                                  ui->bt_nearby, &ui->bt_nearby_count,
                                  nearby, nearby_count);
    }

clamp_cursor:
    rows = jw__bt_row_count(ui);
    if (rows < 1) {
        rows = 1;
    }
    if (ui->bluetooth_list.cursor >= rows) {
        ui->bluetooth_list.cursor = rows - 1;
    }
}

static bool jw__bt_scan_start(jw_settings_ui *ui, bool manual) {
    if (!ui || ui->bt_op != JW_BT_OP_NONE) {
        return false;
    }
    if (!jw_bt_available()) {
        if (manual) {
            jw__bt_msg(ui, "Bluetooth unavailable");
        }
        return false;
    }
    if (!ui->bt_radio_on) {
        if (manual) {
            jw__bt_msg(ui, "Bluetooth is off");
        }
        return false;
    }
    if (jw_bt_scan_start() != 0) {
        if (manual) {
            jw__bt_msg(ui, "Could not start Bluetooth scan");
        }
        return false;
    }
    ui->bt_op = JW_BT_OP_SCAN;
    ui->bt_op_manual = manual;
    ui->bt_next_scan_ms = SDL_GetTicks() + JW_BT_SCAN_INTERVAL_MS;
    if (manual) {
        jw__bt_msg(ui, "Scanning Bluetooth...");
    }
    cat_request_frame_in(250);
    return true;
}

bool jw_settings_ui_wants_bluetooth_poll(const jw_settings_ui *ui) {
    return ui && ui->open && ui->screen == JW_SETTINGS_BLUETOOTH;
}

static void jw__set_audio_output(jw_settings_ui *ui, jw_platform_audio_output output,
                                 char *status_buf, size_t status_size);

void jw_settings_ui_refresh_bluetooth(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }

    if (!jw_bt_available()) {
        ui->bt_op = JW_BT_OP_NONE;
        ui->bt_op_manual = false;
        jw__refresh_bluetooth_lists(ui);
        return;
    }

    if (ui->bt_msg_ms && (int)(SDL_GetTicks() - ui->bt_msg_ms) > 6000) {
        ui->bt_msg[0] = '\0';
        ui->bt_msg_ms = 0;
    }

    if (ui->bt_op != JW_BT_OP_NONE) {
        char message[128] = { 0 };
        jw_bt_operation_status st =
            (ui->bt_op == JW_BT_OP_SCAN)
            ? jw_bt_scan_poll(message, sizeof(message))
            : jw_bt_connect_poll(message, sizeof(message));
        if (st == JW_BT_OP_RUNNING) {
            cat_request_frame_in(250);
            return;
        }
        if (st == JW_BT_OP_OK || st == JW_BT_OP_FAILED || st == JW_BT_OP_TIMEOUT) {
            bool was_connect = (ui->bt_op != JW_BT_OP_SCAN);
            bool quiet_auto_scan = (ui->bt_op == JW_BT_OP_SCAN && !ui->bt_op_manual);
            if (!quiet_auto_scan) {
                jw__bt_msg(ui, "%s", message[0] ? message :
                           (st == JW_BT_OP_OK ? "Bluetooth done" : "Bluetooth failed"));
            }
            ui->bt_op = JW_BT_OP_NONE;
            ui->bt_op_manual = false;
            jw__refresh_bluetooth_lists(ui);
            ui->bt_next_poll_ms = SDL_GetTicks() + JW_BT_POLL_INTERVAL_MS;
            /* A successful connect should make the headset the audio output on its
               own — otherwise you connect headphones and still hear the speaker
               until you dig into Sound settings. The route is guarded daemon-side
               (BLUETOOTH output is only "available" when an audio sink is actually
               connected), so connecting a non-audio device here is a no-op. */
            if (was_connect && st == JW_BT_OP_OK) {
                char route_status[128];
                jw__set_audio_output(ui, JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH,
                                     route_status, sizeof(route_status));
            }
            cat_request_frame();
            return;
        }
    }

    unsigned now = SDL_GetTicks();
    if (ui->bt_next_poll_ms == 0 || (int)(now - ui->bt_next_poll_ms) >= 0) {
        jw__refresh_bluetooth_lists(ui);
        ui->bt_next_poll_ms = now + JW_BT_POLL_INTERVAL_MS;
    }

    /* No background auto-scan. It re-scanned every ~12s, and each scan pinned the
       page on "Bluetooth is busy" for ~8s and churned the BT radio (which starves
       WiFi on this combo chip). Discovery is manual only now: the Paired list is
       always shown, and the user presses Scan (X) when they want to find a new
       device to pair. */
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

static void jw__apply_persisted_overrides_from_values(
        char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX],
        const unsigned char found[JW_SETTING_COUNT]) {
    ap_theme *t = cat_get_theme();

    {
        int idx = jw__setting_has(values, found, JW_SETTING_PILL_SHAPE_INDEX)
                  ? atoi(values[JW_SETTING_PILL_SHAPE_INDEX])
                  : JW_SETTINGS_PILL_SHAPE_DEFAULT;   /* fresh install → Leaf */
        if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT) {
            t->pill_radius_ratio = kJawakaPillRadiusValues[idx];
            t->pill_corner_mask  = kJawakaPillCornerMasks[idx];
        }
    }

    /* Font size and family can reload the glyph atlas; keep the same behavior
       as jw_settings_apply_persisted_overrides(), but use the already-loaded
       setting values instead of re-opening SQLite for each key. */
    {
        int bump = cat_get_font_bump();
        if (jw__setting_has(values, found, JW_SETTING_FONT_SIZE_INDEX)) {
            int idx = atoi(values[JW_SETTING_FONT_SIZE_INDEX]);
            if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT)
                bump = kJawakaFontSizeValues[idx];
        }
        /* Always resolve and apply the selected family explicitly. We used to
           short-circuit when CAT_FONT_PATH was inherited from the daemon and only
           set the bump — but cat_set_font_bump reloads via theme.font_path, which
           can be empty here, so a font-size change fell back through the candidate
           list to res/font.ttf and clobbered the family to the rounded default.
           Setting the path first keeps both family and bump correct on every apply. */
        int fidx = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
        if (jw__setting_has(values, found, JW_SETTING_FONT_FAMILY_INDEX)) {
            int idx = atoi(values[JW_SETTING_FONT_FAMILY_INDEX]);
            if (idx >= 0 && idx < JW_APPEARANCE_FONT_FAMILY_COUNT)
                fidx = idx;
        }
        snprintf(t->font_path, sizeof(t->font_path), "%s",
                 jw_appearance_font_path_for_index(fidx));
        if (bump != cat_get_font_bump())
            cat_set_font_bump(bump);
        else
            cat_reload_fonts(t->font_path);
    }

    if (jw__setting_has(values, found, JW_SETTING_ACCENT_COLOR))
        t->accent = cat_hex_to_color(values[JW_SETTING_ACCENT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_TEXT_COLOR))
        t->text = cat_hex_to_color(values[JW_SETTING_TEXT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_HINT_COLOR))
        t->hint = cat_hex_to_color(values[JW_SETTING_HINT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_HIGHLIGHT_COLOR))
        t->highlight = cat_hex_to_color(values[JW_SETTING_HIGHLIGHT_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_BG_COLOR))
        t->background = cat_hex_to_color(values[JW_SETTING_BG_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_BUTTON_LABEL_COLOR))
        t->button_label = cat_hex_to_color(values[JW_SETTING_BUTTON_LABEL_COLOR]);
    if (jw__setting_has(values, found, JW_SETTING_BUTTON_GLYPH_BG_COLOR))
        t->button_glyph_bg = cat_hex_to_color(values[JW_SETTING_BUTTON_GLYPH_BG_COLOR]);

    cat_finalize_theme_colors(t);

    /* Apply the persisted time zone so the clock is correct from launch. */
    if (jw__setting_has(values, found, JW_SETTING_TIMEZONE) &&
        values[JW_SETTING_TIMEZONE][0])
        jw__apply_timezone(values[JW_SETTING_TIMEZONE]);
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
    ui->update_have_status = false;
    ui->update_next_poll_ms = 0;
    ui->bt_op = JW_BT_OP_NONE;
    ui->bt_op_manual = false;
    cat_list_state_init(&ui->home_list,       JW_SETTINGS_CATEGORY_COUNT);
    cat_list_state_init(&ui->appearance_list,  JW_APPEAR_ROW_COUNT);
    cat_list_state_init(&ui->colors_list,      JW_COLOR_ROW_COUNT);
    cat_list_state_init(&ui->layout_list,      JW_LAYOUT_ROW_COUNT);
    cat_list_state_init(&ui->statusbar_list,   JW_STATUSBAR_ROW_COUNT);
    cat_list_state_init(&ui->display_list,     JW_SETTINGS_DISPLAY_COUNT);
    cat_list_state_init(&ui->network_list,     JW_WIFI_LIST_ROWS);
    cat_list_state_init(&ui->bluetooth_list,   JW_BLUETOOTH_LIST_ROWS);
    cat_list_state_init(&ui->lighting_list,    JW_LIGHTING_ROW_COUNT);
    cat_list_state_init(&ui->accounts_list,    JW_ACCOUNTS_ROW_COUNT);
    cat_list_state_init(&ui->scraping_list,    JW_SCRAPING_ROW_COUNT);
    cat_list_state_init(&ui->scrape_edit_list, 8);
    cat_list_state_init(&ui->behavior_list,    JW_BEHAVIOR_ROW_COUNT);
    cat_list_state_init(&ui->update_list,      JW_UPDATE_ROW_COUNT);
    cat_list_state_init(&ui->update_picker_list, JW_UPDATE_PICKER_VISIBLE_ROWS);
    cat_list_state_init(&ui->timezone_picker_list, JW_TIMEZONE_VISIBLE_ROWS);
    cat_list_state_init(&ui->placeholder_list, 1);
    cat_scroll_state_init(&ui->about_scroll);
    ui->theme_index       = jw__find_theme_index(initial_theme_name);
    ui->color_scheme_index = -1;   /* custom until a scheme is loaded below */
    ui->pill_shape_index  = JW_SETTINGS_PILL_SHAPE_DEFAULT;
    ui->font_family_index = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
    ui->timezone[0]       = '\0';  /* "" = follow system tz until the user picks */
    ui->font_size_index   = 1;
    ui->tab_glide         = 1;     /* Glide by default — matches Leaf's soft feel; Snap is opt-out */
    ui->show_hints        = true;
    ui->clock_style_index = 1;
    ui->show_battery      = true;
    ui->show_battery_level = false;
    ui->show_wifi         = true;
    ui->show_bluetooth    = true;
    ui->show_volume       = true;
    ui->startup_tab_index = JW_STARTUP_TAB_DEFAULT;
    ui->auto_sleep_index  = JW_AUTO_SLEEP_DEFAULT;
    ui->boot_splash_enabled = true;
    ui->boot_splash_supported = false;
    ui->refresh_rate_hz   = 60;
    ui->refresh_rate_supported = false;
    ui->game_perf_profile = JW_GAME_PERF_PROFILE_DEFAULT;
    ui->performance_supported = false;
    ui->brightness_percent = 50;
    ui->volume_percent     = 50;
    ui->audio_output       = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    ui->audio_available_outputs = JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_SPEAKER);
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        ui->audio_volumes[i] = -1;
    }
    ui->audio_volumes[JW_PLATFORM_AUDIO_OUTPUT_SPEAKER] = ui->volume_percent;
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
        char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX];
        unsigned char found[JW_SETTING_COUNT];
        if (jw__load_setting_values(db_path, values, found) == 0) {
            if (jw__setting_has(values, found, JW_SETTING_PILL_SHAPE_INDEX)) {
                int idx = atoi(values[JW_SETTING_PILL_SHAPE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT)
                    ui->pill_shape_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_FONT_FAMILY_INDEX)) {
                int idx = atoi(values[JW_SETTING_FONT_FAMILY_INDEX]);
                if (idx >= 0 && idx < JW_APPEARANCE_FONT_FAMILY_COUNT)
                    ui->font_family_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_FONT_SIZE_INDEX)) {
                int idx = atoi(values[JW_SETTING_FONT_SIZE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT)
                    ui->font_size_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_TAB_GLIDE))
                ui->tab_glide = (strcmp(values[JW_SETTING_TAB_GLIDE], "0") != 0) ? 1 : 0;
            if (jw__setting_has(values, found, JW_SETTING_SHOW_HINTS))
                ui->show_hints = (strcmp(values[JW_SETTING_SHOW_HINTS], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_CLOCK_STYLE_INDEX)) {
                int idx = atoi(values[JW_SETTING_CLOCK_STYLE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_CLOCK_STYLE_COUNT)
                    ui->clock_style_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_TIMEZONE))
                snprintf(ui->timezone, sizeof(ui->timezone), "%s",
                         values[JW_SETTING_TIMEZONE]);
            if (jw__setting_has(values, found, JW_SETTING_SS_USER))
                snprintf(ui->ss_username, sizeof(ui->ss_username), "%s",
                         values[JW_SETTING_SS_USER]);
            if (jw__setting_has(values, found, JW_SETTING_SS_VERIFIED))
                ui->ss_verified = (strcmp(values[JW_SETTING_SS_VERIFIED], "1") == 0);
            if (jw__setting_has(values, found, JW_SETTING_SS_MAXTHREADS))
                ui->ss_max_threads = atoi(values[JW_SETTING_SS_MAXTHREADS]);
            if (jw__setting_has(values, found, JW_SETTING_SS_REQUESTS_TODAY))
                ui->ss_requests_today = atoi(values[JW_SETTING_SS_REQUESTS_TODAY]);
            if (jw__setting_has(values, found, JW_SETTING_SS_MAX_REQUESTS))
                ui->ss_max_requests = atoi(values[JW_SETTING_SS_MAX_REQUESTS]);
            ui->scrape_artwork_included = jw__scrape_csv_to_order(
                jw__setting_has(values, found, JW_SETTING_SCRAPE_ARTWORK_PRIO)
                    ? values[JW_SETTING_SCRAPE_ARTWORK_PRIO] : NULL,
                jw_ss_media_types, jw_ss_media_types_count,
                jw_ss_default_artwork_priority,
                jw_ss_default_artwork_priority_count,
                ui->scrape_artwork_order);
            ui->scrape_region_included = jw__scrape_csv_to_order(
                jw__setting_has(values, found, JW_SETTING_SCRAPE_REGION_PRIO)
                    ? values[JW_SETTING_SCRAPE_REGION_PRIO] : NULL,
                jw_ss_regions, jw_ss_regions_count,
                jw_ss_default_region_priority,
                jw_ss_default_region_priority_count,
                ui->scrape_region_order);
            if (jw__setting_has(values, found, JW_SETTING_RA_USER))
                snprintf(ui->ra_username, sizeof(ui->ra_username), "%s",
                         values[JW_SETTING_RA_USER]);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY))
                ui->show_battery = (strcmp(values[JW_SETTING_SHOW_BATTERY], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY_LEVEL))
                ui->show_battery_level = (strcmp(values[JW_SETTING_SHOW_BATTERY_LEVEL], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_WIFI))
                ui->show_wifi = (strcmp(values[JW_SETTING_SHOW_WIFI], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BLUETOOTH))
                ui->show_bluetooth = (strcmp(values[JW_SETTING_SHOW_BLUETOOTH], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_VOLUME))
                ui->show_volume = (strcmp(values[JW_SETTING_SHOW_VOLUME], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_COLOR_SCHEME_INDEX)) {
                int idx = atoi(values[JW_SETTING_COLOR_SCHEME_INDEX]);
                if (idx >= 0 && idx < JW_COLOR_SCHEME_COUNT)
                    ui->color_scheme_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_STARTUP_TAB_INDEX)) {
                int idx = atoi(values[JW_SETTING_STARTUP_TAB_INDEX]);
                if (idx >= 0 && idx < JW_STARTUP_TAB_COUNT)
                    ui->startup_tab_index = idx;
            }
            /* Stored as seconds (what the daemon reads); map back to the label index. */
            if (jw__setting_has(values, found, JW_SETTING_AUTO_SLEEP_SECONDS)) {
                int seconds = atoi(values[JW_SETTING_AUTO_SLEEP_SECONDS]);
                for (int i = 0; i < JW_AUTO_SLEEP_COUNT; i++) {
                    if (kAutoSleepSeconds[i] == seconds) { ui->auto_sleep_index = i; break; }
                }
            }
            if (jw__setting_has(values, found, JW_SETTING_BOOT_SPLASH_ENABLED))
                ui->boot_splash_enabled = (strcmp(values[JW_SETTING_BOOT_SPLASH_ENABLED], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_REFRESH_RATE_HZ)) {
                int hz = atoi(values[JW_SETTING_REFRESH_RATE_HZ]);
                if (hz == 60 || hz == 90 || hz == 120) ui->refresh_rate_hz = hz;
            }
            if (jw__setting_has(values, found, JW_SETTING_GAME_PERFORMANCE_PROFILE)) {
                jw_platform_perf_profile profile;
                if (jw_platform_parse_perf_profile(
                        values[JW_SETTING_GAME_PERFORMANCE_PROFILE], &profile)) {
                    ui->game_perf_profile = jw__game_perf_index_for_profile(profile);
                }
            }
            if (jw__setting_has(values, found, JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT))
                ui->brightness_percent = jw_platform_clamp_brightness_percent(
                    atoi(values[JW_SETTING_PLATFORM_BRIGHTNESS_PERCENT]));
            if (jw__setting_has(values, found, JW_SETTING_PLATFORM_VOLUME_PERCENT)) {
                int volume = atoi(values[JW_SETTING_PLATFORM_VOLUME_PERCENT]);
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                ui->volume_percent = volume;
                ui->audio_volumes[JW_PLATFORM_AUDIO_OUTPUT_SPEAKER] = volume;
            }

            jw__apply_persisted_overrides_from_values(values, found);

            /* Fresh install (no color ever persisted) → default to the Leaf scheme,
               the project's identity theme. Returning users keep their colors. */
            if (!jw__setting_has(values, found, JW_SETTING_ACCENT_COLOR))
                jw__apply_color_scheme(ui, JW_COLOR_SCHEME_DEFAULT, NULL);
        }
    }
}

void jw_settings_ui_enter(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = true;
    ui->screen = JW_SETTINGS_HOME;
}

void jw_settings_ui_close(jw_settings_ui *ui) {
    if (!ui) return;
    if (ui->bt_op != JW_BT_OP_NONE) {
        jw_bt_cancel_operation();
        ui->bt_op = JW_BT_OP_NONE;
        ui->bt_op_manual = false;
    }
    ui->open = false;
    ui->screen = JW_SETTINGS_HOME;
}

/* Open the UI directly on a specific screen and prime its state, so a host that
   doesn't go through the Settings home (e.g. jawaka-menu's System popup) lands
   the same as picking the category would. Currently used for About and System
   Update; mirrors the per-screen entry priming the home A-handler does. */
void jw_settings_ui_open(jw_settings_ui *ui, jw_settings_screen screen) {
    if (!ui) return;
    ui->open = true;
    ui->screen = screen;
    if (screen == JW_SETTINGS_UPDATE) {
        ui->update_list.cursor = 0;
        ui->update_list.scroll_offset = 0;
        ui->update_msg[0] = '\0';
        ui->update_msg_ms = 0;
        jw__refresh_update_status(ui, false);
        ui->update_next_poll_ms = SDL_GetTicks() + 1000;
    } else if (screen == JW_SETTINGS_ABOUT) {
        cat_scroll_state_init(&ui->about_scroll);   /* start at top */
    }
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
    jw__refresh_audio_status(ui);
    jw__refresh_led(ui);
}

bool jw_settings_show_hints(const jw_settings_ui *ui) {
    return !ui || ui->show_hints;
}

bool jw_settings_tab_glide(const jw_settings_ui *ui) {
    return ui && ui->tab_glide != 0;
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
    out->show_bluetooth = ui->show_bluetooth;
    out->bt_state = ui->show_bluetooth ? ui->bt_state_cached : 0;
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

int jw_settings_bt_state_now(void) {
    if (!jw_bt_radio_is_on()) return 0;
    return (jw_bt_any_connected() == 1) ? 2 : 1;
}

void jw_settings_ui_refresh_bt_state(jw_settings_ui *ui) {
    if (ui) {
        ui->bt_state_cached = jw_settings_bt_state_now();
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
    bool show_bluetooth    = true;
    bool show_volume       = true;
    int  volume_percent    = -1;
    bool show_hints        = true;

    if (db_path && db_path[0]) {
        char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX];
        unsigned char found[JW_SETTING_COUNT];
        if (jw__load_setting_values(db_path, values, found) == 0) {
            if (jw__setting_has(values, found, JW_SETTING_CLOCK_STYLE_INDEX)) {
                int idx = atoi(values[JW_SETTING_CLOCK_STYLE_INDEX]);
                if (idx >= 0 && idx < JW_SETTINGS_CLOCK_STYLE_COUNT)
                    clock_style_index = idx;
            }
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY))
                show_battery = (strcmp(values[JW_SETTING_SHOW_BATTERY], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BATTERY_LEVEL))
                show_battery_level = (strcmp(values[JW_SETTING_SHOW_BATTERY_LEVEL], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_WIFI))
                show_wifi = (strcmp(values[JW_SETTING_SHOW_WIFI], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_BLUETOOTH))
                show_bluetooth = (strcmp(values[JW_SETTING_SHOW_BLUETOOTH], "0") != 0);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_VOLUME))
                show_volume = (strcmp(values[JW_SETTING_SHOW_VOLUME], "0") != 0);
            /* The menu has no live volume poll; use the daemon's last persisted value. */
            if (jw__setting_has(values, found, JW_SETTING_PLATFORM_VOLUME_PERCENT))
                volume_percent = atoi(values[JW_SETTING_PLATFORM_VOLUME_PERCENT]);
            if (jw__setting_has(values, found, JW_SETTING_SHOW_HINTS))
                show_hints = (strcmp(values[JW_SETTING_SHOW_HINTS], "0") != 0);
        }
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
        out_opts->show_bluetooth = show_bluetooth;
        out_opts->bt_state = show_bluetooth ? jw_settings_bt_state_now() : 0;
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

/* Canonical vertical advance for one sub-header caption line — the single source
   of truth for sub-header line spacing across every settings page. */
static int jw__subheader_line_h(TTF_Font *f) {
    return TTF_FontHeight(f) + cat_scale(8);
}

/* One settings page = a single full-width column: the title header strip, an
   optional sub-header strip, then the content box (mirrors jw__browse_boxes with
   the cover column collapsed to zero). cat_box_carve_top with height 0 is a no-op,
   so a header-less / sub-header-less page costs nothing. Pure geometry, no drawing:
   the title still draws via jw__draw_header (its geometry equals the carved header
   strip); info lines draw into *sub; rows/list fill the returned content rect.
   *header and *sub are nullable. */
static SDL_Rect jw__settings_boxes(int x, int y, int w, int h,
                                   bool show_header, int sub_h,
                                   SDL_Rect *header, SDL_Rect *sub) {
    cat_box page = { x, y, w, h, 0, 0, 0, 0 };
    cat_box hdr = cat_box_carve_top(&page, show_header ? jw__header_h() : 0);
    cat_box sb  = cat_box_carve_top(&page, sub_h);
    if (header) *header = cat_box_content(&hdr);
    if (sub)    *sub    = cat_box_content(&sb);
    return cat_box_content(&page);
}

static void jw__render_list_row_h(const cat_list_state *list, int x, int y,
                                  int w, int row, const char *label,
                                  const char *value, bool cycler, int item_h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
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

/* The canonical settings list row: medium font + cat_scale(12) padding. */
static void jw__render_list_row(const cat_list_state *list, int x, int y,
                                int w, int row, const char *label,
                                const char *value, bool cycler) {
    int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
    jw__render_list_row_h(list, x, y, w, row, label, value, cycler, item_h);
}

/* Row pitch for the Display & Sound page. The Brightness/Volume sliders need the
   taller slot for their track, so the Audio Output row (a plain list row between
   them) must share this same pitch — otherwise the three rows, each positioned by
   row*own_height, overlap and gap. */
static int jw__display_row_h(void) {
    return TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(28);
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

/* List-pane row for the Settings home categories — mirrors jw__render_nav_row but
   positioned by the scrolling list pane, so the list fills the page and scrolls
   instead of overflowing under the footer. */
static void jw__draw_home_item(int idx, int ix, int iy, int iw, int ih,
                               bool selected, void *user) {
    (void)user;
    if (idx < 0 || idx >= JW_SETTINGS_CATEGORY_COUNT) return;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected)
        cat_draw_pill(ix, pill_y, iw - cat_scale(4), pill_h, theme->highlight);
    ap_color tc = selected ? theme->highlighted_text : theme->text;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
    cat_draw_text_ellipsized(body, kHomeCategoryLabels[idx], ix + cat_scale(12), ty,
                             tc, iw - cat_scale(24));
}

static void jw__render_home(const jw_settings_ui *ui, int x, int y, int w, int h) {
    /* No "Settings" header — the tab bar above already names this screen, so the
       category list fills the full height and scrolls rather than overflowing. */
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    SDL_Rect content = jw__settings_boxes(x, y, w, h, false, 0, NULL, NULL);
    cat_box lb = { content.x, content.y, content.w, content.h, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, JW_SETTINGS_CATEGORY_COUNT, &vis, &item_h);
    ((cat_list_state *)&ui->home_list)->visible_rows = vis;
    cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h, JW_SETTINGS_CATEGORY_COUNT,
                       &ui->home_list, item_h, jw__draw_home_item, NULL);
}

static void jw__render_appearance(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Appearance", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
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
}

static void jw__render_colors(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *t = cat_get_theme();
    jw__draw_header("Colors", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;

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
}

static void jw__render_layout(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Layout", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_PILL_SHAPE,
                        "List Style", kPillShapeLabels[ui->pill_shape_index], true);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_FONT_FAMILY,
                        "Font", kJawakaFontFamilyLabels[ui->font_family_index], true);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_FONT_SIZE,
                        "Font Size", kFontSizeLabels[ui->font_size_index], true);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_TAB_SWITCH,
                        "Tab Switching", kTabSwitchLabels[ui->tab_glide ? 1 : 0], true);
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
    "Hidden", "Icon", "Percent", "Both"
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

/* Status-bar visibility toggles read as Visible/Hidden rather than On/Off. */
static inline const char *jw__vis_label(bool visible) {
    return visible ? "Visible" : "Hidden";
}

static void jw__render_statusbar(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Status Bar", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_HINTS,
                        "Button Hints", jw__vis_label(ui->show_hints), true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_CLOCK,
                        "Clock", kClockStyleLabels[ui->clock_style_index], true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_BATTERY,
                        "Battery",
                        kBatteryModeLabels[jw__battery_mode(ui->show_battery, ui->show_battery_level)],
                        true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_WIFI,
                        "Wifi", jw__vis_label(ui->show_wifi), true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_BLUETOOTH,
                        "Bluetooth", jw__vis_label(ui->show_bluetooth), true);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_VOLUME,
                        "Volume", jw__vis_label(ui->show_volume), true);
}

/* One labelled slider row (Brightness / Volume) on the Display & Sound page. */
static void jw__draw_slider_row(const jw_settings_ui *ui, int x, int y_base, int w,
                                int row, const char *label, int percent) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = jw__display_row_h();
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

static void jw__draw_audio_output_row(const jw_settings_ui *ui, int x, int y_base, int w) {
    jw_platform_audio_output output = ui->audio_output;
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        output = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    }
    /* Only show the cycle affordance when there is more than one output to pick
       (e.g. headphones plugged in), so a lone "Speaker" doesn't look switchable.
       Shares the slider pitch so all three rows line up. */
    int navail = 0;
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        if (ui->audio_available_outputs & JW_PLATFORM_AUDIO_OUTPUT_BIT(i)) {
            navail++;
        }
    }
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_OUTPUT,
                          "Output", jw_platform_audio_output_label(output),
                          navail > 1, jw__display_row_h());
}

static void jw__render_display(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Display & Sound", x, y, w);
    int y_base = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    jw__draw_slider_row(ui, x, y_base, w, JW_DISPLAY_BRIGHTNESS, "Brightness",
                        ui->brightness_percent);
    /* Display refresh rate (60/90 Hz). Cycler when the platform supports it. */
    char refresh_val[16];
    snprintf(refresh_val, sizeof(refresh_val), "%d Hz", ui->refresh_rate_hz);
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_REFRESH_RATE,
                          "Refresh Rate", refresh_val,
                          ui->refresh_rate_supported, jw__display_row_h());
    jw__draw_audio_output_row(ui, x, y_base, w);
    jw__draw_slider_row(ui, x, y_base, w, JW_DISPLAY_VOLUME, "Volume",
                        ui->volume_percent);
    /* Action row: toggles a short clip on the current output so the user can
       verify sound (and which device it lands on). Shows Stop while playing. */
    jw__render_list_row_h(&ui->display_list, x, y_base, w, JW_DISPLAY_TEST_SOUND,
                          "Test Sound", ui->test_sound_playing ? "Stop" : "Play",
                          false, jw__display_row_h());
}

static void jw__render_lighting(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Lighting", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
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
}

static void jw__refresh_wifi(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    if (!jw_wifi_available()) {
        memset(&ui->wifi, 0, sizeof(ui->wifi));
        ui->wifi_strength_cached = 0;
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
    if (!jw_wifi_available()) {
        ui->wifi_network_count = 0;
        if (ui->network_list.cursor >= JW_NETWORK_FIXED_ROWS) {
            ui->network_list.cursor = JW_NETWORK_ROW_WIFI;
        }
        return;
    }
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

    if (!jw_wifi_available()) {
        jw__refresh_adb(ui);
        memset(&ui->wifi, 0, sizeof(ui->wifi));
        ui->wifi_radio_on = false;
        ui->wifi_strength_cached = 0;
        ui->wifi_network_count = 0;
        ui->wifi_next_poll_ms = SDL_GetTicks() + 2000;
        return;
    }

    /* Drain the platform event source every frame (cheap, non-blocking) so a
       fast auth failure isn't missed between the throttled platform polls. */
    jw_wifi_evt evt = ui->wifi_attempt_ssid[0] ? jw_wifi_monitor_poll(ui->wifi_monitor_fd)
                                               : JW_WIFI_EVT_NONE;

    /* Self-throttle the platform Wi-Fi polls to ~2s. */
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
    bool wifi_available;
    bool radio_on;
    int adb_enabled;
    int adb_intent_enabled;
    bool adb_supported;
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
        const char *action = ctx->wifi_available
            ? (ctx->radio_on ? "Turn Off" : "Turn On")
            : "Unavailable";
        int vw = cat_measure_text(body, action);
        cat_draw_text(body, action, ix + iw - vw - cat_scale(16), ty, value_c);
        return;
    }

    if (idx == JW_NETWORK_ROW_ADB) {
        const char *value = ctx->adb_supported ? "Unavailable" : "Unsupported";
        if (ctx->adb_supported && ctx->adb_enabled == 1) {
            value = "Enabled";
        } else if (ctx->adb_supported && ctx->adb_intent_enabled == 1) {
            value = "Repair";
        } else if (ctx->adb_supported && ctx->adb_enabled == 0) {
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
       networks with a stored profile. */
    const char *word = (net->strength >= 3) ? "Strong" :
                       (net->strength == 2) ? "Good"   : "Weak";
    const char *open_prefix = net->secured ? "" : "Open  ";
    const char *saved_suffix = net->saved ? "  saved" : "";
    char value[56];
    snprintf(value, sizeof(value), "%s%s%s", open_prefix, word, saved_suffix);
    int vw = cat_measure_text(body, value);
    cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_network(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    jw__draw_header("Network", x, y, w);
    int item_h = TTF_FontHeight(body) + cat_scale(12);

    const jw_wifi_status_t *wifi = &ui->wifi;

    char status_val[48];
    char signal_val[32];
    bool wifi_available = jw_wifi_available();
    if (!wifi_available) {
        snprintf(status_val, sizeof(status_val), "%s", "Unavailable");
        snprintf(signal_val, sizeof(signal_val), "%s", "—");
    } else if (!ui->wifi_radio_on) {
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

    /* ── Current-connection summary (small caption lines) — drawn from the box
       model's content origin; the list fills the remainder below. ── */
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int line_h = jw__subheader_line_h(small);
    SDL_Rect content = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);
    int dy = content.y;
    char line[160];

    snprintf(line, sizeof(line), "Status: %s", status_val);
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->text);
    dy += line_h;

    snprintf(line, sizeof(line), "Network: %s",
             (wifi->valid && wifi->ssid[0]) ? wifi->ssid : "—");
    cat_draw_text_ellipsized(small, line, x + cat_scale(12), dy, theme->text, w - cat_scale(24));
    dy += line_h;

    snprintf(line, sizeof(line), "Signal: %s", signal_val);
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h;

    snprintf(line, sizeof(line), "IP: %s",
             (wifi->valid && wifi->ip[0]) ? wifi->ip : "—");
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h + cat_scale(6);

    /* Action feedback (always visible, regardless of the hint setting). */
    if (ui->wifi_msg[0]) {
        cat_draw_text_ellipsized(small, ui->wifi_msg, x + cat_scale(12), dy,
                                 theme->accent, w - cat_scale(24));
        dy += line_h + cat_scale(6);
    }

    /* ── List: fixed controls first, then scanned networks ── */
    int count = JW_NETWORK_FIXED_ROWS +
                ((wifi_available && ui->wifi_radio_on) ? ui->wifi_network_count : 0);
    jw__wifi_list_ctx ctx = {
        ui->wifi_networks,
        wifi_available,
        ui->wifi_radio_on,
        ui->adb_enabled,
        ui->adb_intent_enabled,
        ui->adb_supported,
    };
    cat_box lb = { content.x, dy, content.w, (content.y + content.h) - dy, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
    ((cat_list_state *)&ui->network_list)->visible_rows = vis;
    cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h, count, &ui->network_list, item_h,
                       jw__draw_wifi_item, &ctx);
    if (wifi_available && ui->wifi_radio_on && ui->wifi_network_count == 0) {
        cat_draw_text(small, "Scanning…", x + cat_scale(12),
                      dy + item_h * JW_NETWORK_FIXED_ROWS, theme->hint);
    }
}

typedef struct {
    const jw_settings_ui *ui;
} jw__bt_list_ctx;

/* The "Paired"/"Nearby" rows are section labels, not selectable items — the
   cursor must skip over them during navigation. */
static bool jw__bt_row_is_header(const jw_settings_ui *ui, int row) {
    if (!ui || !ui->bt_radio_on) return false;
    return row == JW_BLUETOOTH_FIXED_ROWS ||
           row == JW_BLUETOOTH_FIXED_ROWS + 1 + ui->bt_paired_count;
}

static const jw_bt_device_t *jw__bt_row_device(const jw_settings_ui *ui, int row,
                                               bool *out_paired) {
    if (out_paired) {
        *out_paired = false;
    }
    if (!ui || !ui->bt_radio_on || row < JW_BLUETOOTH_FIXED_ROWS + 1) {
        return NULL;
    }

    int paired_start = JW_BLUETOOTH_FIXED_ROWS + 1;
    int paired_end = paired_start + ui->bt_paired_count;
    if (row >= paired_start && row < paired_end) {
        if (out_paired) {
            *out_paired = true;
        }
        return &ui->bt_paired[row - paired_start];
    }

    int nearby_header = paired_end;
    int nearby_start = nearby_header + 1;
    int nearby_end = nearby_start + ui->bt_nearby_count;
    if (row >= nearby_start && row < nearby_end) {
        return &ui->bt_nearby[row - nearby_start];
    }
    return NULL;
}

static void jw__draw_bt_item(int idx, int ix, int iy, int iw, int ih,
                             bool selected, void *user) {
    const jw__bt_list_ctx *ctx = (const jw__bt_list_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    bool paired_device = false;
    const jw_bt_device_t *dev = jw__bt_row_device(ui, idx, &paired_device);

    if (ui && ui->bt_radio_on &&
        (idx == JW_BLUETOOTH_FIXED_ROWS ||
         idx == JW_BLUETOOTH_FIXED_ROWS + 1 + ui->bt_paired_count)) {
        const char *label = (idx == JW_BLUETOOTH_FIXED_ROWS) ? "Paired" : "Nearby";
        const char *value = NULL;
        if (idx == JW_BLUETOOTH_FIXED_ROWS && ui->bt_paired_count == 0) {
            value = "None";
        } else if (idx != JW_BLUETOOTH_FIXED_ROWS && ui->bt_nearby_count == 0) {
            value = (ui->bt_op == JW_BT_OP_SCAN) ? "Scanning" : "None";
        }
        cat_draw_text(body, label, ix + cat_scale(12), ty, theme->accent);
        if (value) {
            int vw = cat_measure_text(body, value);
            cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, theme->hint);
        }
        return;
    }

    if (selected) {
        cat_draw_pill(ix, pill_y, iw - cat_scale(4), pill_h, theme->highlight);
    }
    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;

    if (idx == JW_BLUETOOTH_ROW_POWER) {
        const char *value = "Unavailable";
        if (ui && ui->bt_status.available) {
            value = ui->bt_radio_on ? "Turn Off" : "Turn On";
        }
        cat_draw_text(body, "Bluetooth", ix + cat_scale(12), ty, label_c);
        int vw = cat_measure_text(body, value);
        cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
        return;
    }

    if (idx == JW_BLUETOOTH_ROW_NAME) {
        const char *name = (ui && ui->bt_status.local_name[0])
                         ? ui->bt_status.local_name : "-";
        cat_draw_text(body, "Bluetooth Name", ix + cat_scale(12), ty, label_c);
        int vw = cat_measure_text(body, name);
        int vx = ix + iw - vw - cat_scale(16);
        if (vx < ix + iw / 2) {
            cat_draw_text_ellipsized(body, name, ix + iw / 2, ty, value_c,
                                     iw / 2 - cat_scale(16));
        } else {
            cat_draw_text(body, name, vx, ty, value_c);
        }
        return;
    }

    if (!dev) {
        return;
    }

    /* Leading device-type icon (headset / controller / generic), theme-tinted, so
       the row sorts by kind at a glance before the name is read. The icon carries
       the type, so Nearby rows no longer need kind text on the right; Paired rows
       keep their Connected/Paired state there. */
    int name_x = ix + cat_scale(12);
    int icon_px = cat_device_icon_px();
    if (icon_px > 0) {
        cat_device_icon dicon =
            (dev->kind == JW_BT_DEVICE_HEADSET) ? CAT_DEVICE_ICON_HEADSET :
            (dev->kind == JW_BT_DEVICE_JOYPAD)  ? CAT_DEVICE_ICON_CONTROLLER :
                                                  CAT_DEVICE_ICON_BLUETOOTH;
        cat_draw_device_icon(dicon, name_x, pill_y + (pill_h - icon_px) / 2, label_c);
        name_x += icon_px + cat_scale(8);
    }

    int right = ix + iw - cat_scale(16);
    if (paired_device) {
        const char *value = dev->connected ? "Connected" : "Paired";
        int vw = cat_measure_text(body, value);
        cat_draw_text(body, value, right - vw, ty, value_c);
        right -= vw + cat_scale(8);
    }

    const char *label = dev->name[0] ? dev->name : dev->mac;
    int name_max = right - name_x;
    if (name_max < cat_scale(24)) {
        name_max = cat_scale(24);
    }
    cat_draw_text_ellipsized(body, label, name_x, ty, label_c, name_max);
}

static void jw__render_bluetooth(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    jw__draw_header("Bluetooth", x, y, w);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int line_h = jw__subheader_line_h(small);
    SDL_Rect content = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL);
    int dy = content.y;

    const char *status = "Unavailable";
    if (ui->bt_status.available) {
        status = ui->bt_radio_on ? "On" : "Off";
    }
    char line[160];
    snprintf(line, sizeof(line), "Status: %s", status);
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->text);
    dy += line_h;

    snprintf(line, sizeof(line), "Connected: %s",
             (ui->bt_status.available && ui->bt_status.any_connected) ? "Yes" : "No");
    cat_draw_text(small, line, x + cat_scale(12), dy, theme->hint);
    dy += line_h;

    if (ui->bt_msg[0]) {
        cat_draw_text_ellipsized(small, ui->bt_msg, x + cat_scale(12), dy,
                                 theme->accent, w - cat_scale(24));
        dy += line_h + cat_scale(6);
    } else {
        dy += cat_scale(6);
    }

    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int rows = jw__bt_row_count(ui);
    jw__bt_list_ctx ctx = { ui };
    cat_box lb = { content.x, dy, content.w, (content.y + content.h) - dy, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, rows, &vis, &item_h);
    ((cat_list_state *)&ui->bluetooth_list)->visible_rows = vis;
    cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h,
                       rows, &ui->bluetooth_list, item_h,
                       jw__draw_bt_item, &ctx);
}

/* ─── Scraping priorities ──────────────────────────────────────────────── */

/* Parse a CSV of catalog values into a full permutation of catalog indices:
   matched values first (in CSV order, the included zone), unmatched catalog
   entries appended in catalog order. Returns the included count. An empty
   CSV applies the fallback value list instead. */
static int jw__scrape_csv_to_order(const char *csv,
                                   const jw_ss_option *catalog, int catalog_count,
                                   const char *const *fallback, int fallback_count,
                                   int *order) {
    bool used[JW_SCRAPE_PRIO_SLOTS] = { false };
    int included = 0;

    if (catalog_count > JW_SCRAPE_PRIO_SLOTS) catalog_count = JW_SCRAPE_PRIO_SLOTS;

    const char *p = (csv && csv[0]) ? csv : NULL;
    while (p && *p && included < catalog_count) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        for (int i = 0; i < catalog_count && len > 0; i++) {
            if (!used[i] && strlen(catalog[i].value) == len &&
                strncmp(catalog[i].value, p, len) == 0) {
                order[included++] = i;
                used[i] = true;
                break;
            }
        }
        p = comma ? comma + 1 : NULL;
    }

    if (included == 0 && fallback) {
        for (int f = 0; f < fallback_count && included < catalog_count; f++) {
            for (int i = 0; i < catalog_count; i++) {
                if (!used[i] && strcmp(catalog[i].value, fallback[f]) == 0) {
                    order[included++] = i;
                    used[i] = true;
                    break;
                }
            }
        }
    }

    int tail = included;
    for (int i = 0; i < catalog_count; i++) {
        if (!used[i]) order[tail++] = i;
    }
    return included;
}

static void jw__scrape_persist_order(const jw_settings_ui *ui, bool region) {
    const jw_ss_option *catalog = region ? jw_ss_regions : jw_ss_media_types;
    const int *order = region ? ui->scrape_region_order : ui->scrape_artwork_order;
    int included = region ? ui->scrape_region_included : ui->scrape_artwork_included;

    char csv[JW_SETTINGS_VALUE_MAX] = "";
    size_t len = 0;
    for (int i = 0; i < included; i++) {
        int n = snprintf(csv + len, sizeof(csv) - len, "%s%s",
                         i > 0 ? "," : "", catalog[order[i]].value);
        if (n < 0 || (size_t)n >= sizeof(csv) - len) break;
        len += (size_t)n;
    }
    jw__persist(ui, region ? "scrape.region_priority" : "scrape.artwork_priority",
                csv);
}

/* Move order[from] to position to, shifting the entries between. */
static void jw__scrape_order_move(int *order, int from, int to) {
    if (from == to) return;
    int v = order[from];
    if (from < to) {
        memmove(order + from, order + from + 1, (size_t)(to - from) * sizeof(int));
    } else {
        memmove(order + to + 1, order + to, (size_t)(from - to) * sizeof(int));
    }
    order[to] = v;
}

typedef struct {
    const jw_settings_ui *ui;
} jw__scrape_edit_ctx;

static void jw__draw_scrape_edit_item(int idx, int ix, int iy, int iw, int ih,
                                      bool selected, void *user) {
    jw__scrape_edit_ctx *ctx = (jw__scrape_edit_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    if (!ui) return;

    bool region = ui->scrape_edit_is_region;
    const jw_ss_option *catalog = region ? jw_ss_regions : jw_ss_media_types;
    int catalog_count = region ? jw_ss_regions_count : jw_ss_media_types_count;
    const int *order = region ? ui->scrape_region_order : ui->scrape_artwork_order;
    int included = region ? ui->scrape_region_included : ui->scrape_artwork_included;
    if (idx < 0 || idx >= catalog_count) return;

    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    bool grabbed_row = selected && ui->scrape_edit_grabbed;
    if (selected) {
        cat_draw_pill(ix, pill_y, iw - cat_scale(4), pill_h,
                      grabbed_row ? theme->accent : theme->highlight);
    }

    bool is_included = idx < included;
    ap_color label_c = selected ? theme->highlighted_text
                                : (is_included ? theme->text : theme->hint);
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    char label[64];
    if (is_included) {
        snprintf(label, sizeof(label), "%d. %s", idx + 1,
                 catalog[order[idx]].display);
    } else {
        snprintf(label, sizeof(label), "%s", catalog[order[idx]].display);
    }
    cat_draw_text_ellipsized(body, label, ix + cat_scale(12), ty, label_c,
                             iw * 2 / 3);

    const char *value = grabbed_row ? "Moving" : (is_included ? "On" : "Off");
    int vw = cat_measure_text(body, value);
    cat_draw_text(body, value, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_scrape_priority(const jw_settings_ui *ui,
                                       int x, int y, int w, int h) {
    bool region = ui->scrape_edit_is_region;
    jw__draw_header(region ? "Region Priority" : "Artwork Priority", x, y, w);

    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int sub_h = jw__subheader_line_h(small) + cat_scale(6);
    SDL_Rect sub;
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, sub_h, NULL, &sub);
    cat_draw_text_ellipsized(small, "A: On/Off   X: Grab to reorder",
                             sub.x + cat_scale(12), sub.y, theme->hint,
                             sub.w - cat_scale(24));

    int count = region ? jw_ss_regions_count : jw_ss_media_types_count;
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    cat_box lb = { c.x, c.y, c.w, c.h, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
    ((cat_list_state *)&ui->scrape_edit_list)->visible_rows = vis;
    jw__scrape_edit_ctx ctx = { ui };
    cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h, count,
                       &ui->scrape_edit_list, item_h,
                       jw__draw_scrape_edit_item, &ctx);
}

static void jw__render_scraping(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Game Art", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;

    char artwork_value[64];
    if (ui->scrape_artwork_included > 0) {
        snprintf(artwork_value, sizeof(artwork_value), "%s first",
                 jw_ss_media_types[ui->scrape_artwork_order[0]].display);
    } else {
        snprintf(artwork_value, sizeof(artwork_value), "None selected");
    }
    jw__render_list_row(&ui->scraping_list, x, ly, w, JW_SCRAPING_ARTWORK,
                        "Artwork Priority", artwork_value, false);

    char region_value[64];
    if (ui->scrape_region_included > 0) {
        snprintf(region_value, sizeof(region_value), "%s first",
                 jw_ss_regions[ui->scrape_region_order[0]].display);
    } else {
        snprintf(region_value, sizeof(region_value), "None selected");
    }
    jw__render_list_row(&ui->scraping_list, x, ly, w, JW_SCRAPING_REGION,
                        "Region Priority", region_value, false);
}

/* Account row: like jw__render_list_row, but the status value marquees while the
   row is selected so a long "Signed in as … threads … quota …" line scrolls into
   view instead of running off the right edge. Unselected rows ellipsize it. The
   value stays right-aligned when it already fits. Returns true while animating. */
static bool jw__render_account_row(const cat_list_state *list, int x, int y,
                                   int w, int row, const char *label,
                                   const char *value, cat_marquee *mq,
                                   uint32_t dt) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
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

    bool anim = false;
    if (value && value[0]) {
        int value_x = x + w / 2;
        int value_w = (x + w - cat_scale(16)) - value_x;
        int vw = cat_measure_text(body, value);
        if (value_w <= 0) {
            /* no room */
        } else if (vw <= value_w) {
            cat_draw_text(body, value, x + w - vw - cat_scale(16), ty, value_c);
        } else if (selected) {
            if (mq) mq->mode = CAT_MARQUEE_LOOP;
            anim = cat_draw_text_marquee(body, value, value_x, ty, value_c,
                                         value_w, mq, dt);
        } else {
            if (mq) mq->elapsed_ms = 0;   /* restart the scroll next time it's selected */
            cat_draw_text_ellipsized(body, value, value_x, ty, value_c, value_w);
        }
    }
    return anim;
}

static void jw__render_accounts(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Accounts", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;

    /* Per-row marquee state (one Accounts screen is live at a time). */
    static cat_marquee mq[JW_ACCOUNTS_ROW_COUNT];
    static uint32_t    last_ms = 0;
    uint32_t now = SDL_GetTicks();
    uint32_t dt  = (last_ms == 0) ? 0u : (now - last_ms);
    last_ms = now;
    bool anim = false;

    /* "Signed in" only after the daemon has validated the credentials against
       the API (scrape-validate IPC); "Saved" means stored but unverified
       (daemon or network unavailable at sign-in). */
    char ss_value[160];
    if (ui->ss_username[0] && ui->ss_verified) {
        char quota[32] = "";
        if (ui->ss_max_requests > 0) {
            snprintf(quota, sizeof(quota), ", quota %d/%d",
                     ui->ss_requests_today, ui->ss_max_requests);
        }
        if (ui->ss_max_threads > 0) {
            snprintf(ss_value, sizeof(ss_value), "Signed in as %s - %d thread%s%s",
                     ui->ss_username, ui->ss_max_threads,
                     ui->ss_max_threads == 1 ? "" : "s", quota);
        } else {
            snprintf(ss_value, sizeof(ss_value), "Signed in as %s%s",
                     ui->ss_username, quota);
        }
    } else if (ui->ss_username[0]) {
        snprintf(ss_value, sizeof(ss_value), "Saved: %s (unverified)", ui->ss_username);
    } else if (ui->ss_rejected) {
        snprintf(ss_value, sizeof(ss_value), "Rejected - wrong username or password");
    } else {
        snprintf(ss_value, sizeof(ss_value), "Not signed in");
    }
    anim |= jw__render_account_row(&ui->accounts_list, x, ly, w,
                                   JW_ACCOUNTS_SCREENSCRAPER, "ScreenScraper.fr",
                                   ss_value, &mq[JW_ACCOUNTS_SCREENSCRAPER], dt);
    char ra_value[96];
    if (ui->ra_username[0]) {
        snprintf(ra_value, sizeof(ra_value), "Saved: %s", ui->ra_username);
    } else {
        snprintf(ra_value, sizeof(ra_value), "Not signed in");
    }
    anim |= jw__render_account_row(&ui->accounts_list, x, ly, w,
                                   JW_ACCOUNTS_RETROACHIEVEMENTS, "RetroAchievements",
                                   ra_value, &mq[JW_ACCOUNTS_RETROACHIEVEMENTS], dt);
    if (anim) cat_request_frame();
}

/* ─── About ────────────────────────────────────────────────────────────── */

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
    { "Fonts: Space Grotesk, Inter, Rounded M+, Nunito, Baloo 2, Fredoka, "
      "Lexend, IBM Plex Sans, Noto Sans, Source Han Sans", "SIL OFL 1.1" },
    { "Keyboard icons (Nerd Fonts)",            "MIT" },
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
    char label[128];   /* roomy enough for the full bundled-font credit line */
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
        /* Every overflowing line scrolls through continuously and wraps (no
           ping-pong bounce) for a calmer, uniform read. */
        label_mq[i].mode = CAT_MARQUEE_LOOP;
        value_mq[i].mode = CAT_MARQUEE_LOOP;
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

/* Read the installed Leaf release from release.json (written by the installer /
   OTA runner). Fills version and release_id; either may stay empty. Returns true
   if at least one was found. The version is the canonical "current installed
   version" shown in About; per the OTA contract an absent file means an older
   install and is shown as "Unknown" rather than blocking anything. */
static bool jw__read_installed_release(char *version, size_t version_size,
                                       char *release_id, size_t release_id_size) {
    if (version_size) version[0] = '\0';
    if (release_id_size) release_id[0] = '\0';
    const char *internal = getenv("UMRK_INTERNAL_DATA_PATH");
    if (!internal || !internal[0]) return false;

    char path[512];
    snprintf(path, sizeof(path), "%s/release.json", internal);
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    char buf[1024];
    size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "release_id");
    if (version_size && cJSON_IsString(v) && v->valuestring)
        snprintf(version, version_size, "%s", v->valuestring);
    if (release_id_size && cJSON_IsString(r) && r->valuestring)
        snprintf(release_id, release_id_size, "%s", r->valuestring);
    cJSON_Delete(root);
    return version[0] || release_id[0];
}

static void jw__render_about(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("About", x, y, w);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(6);
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, pad, NULL, NULL);
    int top = c.y;

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

    char leaf_version[64], leaf_release_id[128];
    bool have_release = jw__read_installed_release(leaf_version, sizeof(leaf_version),
                                                   leaf_release_id, sizeof(leaf_release_id));
    char identity[96];
    snprintf(identity, sizeof(identity), "Leaf  %s",
             (have_release && leaf_version[0]) ? leaf_version : "Unknown");
    jw__about_push(rows, &n, JW_ABOUT_PLAIN, identity, "");

    jw__about_push(rows, &n, JW_ABOUT_HEADING, "System", "");
    /* Release id pins the exact installed artifact; show it when it adds info
       beyond the version string (per the OTA installed-version contract). */
    if (have_release && leaf_release_id[0] && strcmp(leaf_release_id, leaf_version) != 0)
        jw__about_push(rows, &n, JW_ABOUT_FIELD, "Release", leaf_release_id);
    snprintf(buf, sizeof(buf), "%s", info.os_version[0] ? info.os_version : "?");
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "OS", buf);
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
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "IPv4",
                   info.ipv4[0] ? info.ipv4 : (info.ip[0] ? info.ip : "—"));
    jw__about_push(rows, &n, JW_ABOUT_FIELD, "IPv6", info.ipv6[0] ? info.ipv6 : "—");
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
    int view_h    = c.h;        /* fill the content box; a scroll view may clip a partial row at the bottom edge */
    int content_h = n * row_h;
    jw__about_ctx ctx = { rows, n, small, row_h };
    cat_draw_scroll_view(x + pad, top, w - pad * 2, view_h, content_h,
                         (cat_scroll_state *)&ui->about_scroll,
                         jw__draw_about_rows, &ctx);
}

static void jw__render_behavior(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("General", x, y, w);
    int ly = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;
    int tab = (ui->startup_tab_index >= 0 && ui->startup_tab_index < JW_STARTUP_TAB_COUNT)
              ? ui->startup_tab_index : JW_STARTUP_TAB_DEFAULT;
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_STARTUP_TAB,
                        "Startup Tab", kStartupTabLabels[tab], true);
    int sleep_idx = (ui->auto_sleep_index >= 0 && ui->auto_sleep_index < JW_AUTO_SLEEP_COUNT)
                    ? ui->auto_sleep_index : JW_AUTO_SLEEP_DEFAULT;
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_AUTO_SLEEP,
                        "Auto Sleep", kAutoSleepLabels[sleep_idx], true);
    const char *splash = ui->boot_splash_supported
                         ? (ui->boot_splash_enabled ? "On" : "Off")
                         : "Unavailable";
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_BOOT_SPLASH,
                        "Boot Splash", splash, ui->boot_splash_supported);
    int perf_idx = (ui->game_perf_profile >= 0 &&
                    ui->game_perf_profile < JW_GAME_PERF_PROFILE_COUNT)
                 ? ui->game_perf_profile
                 : JW_GAME_PERF_PROFILE_DEFAULT;
    const char *perf = ui->performance_supported
                     ? jw_platform_perf_profile_label(kGamePerfProfiles[perf_idx])
                     : "Unavailable";
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_PERFORMANCE,
                        "Game Performance", perf, ui->performance_supported);
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_TIMEZONE,
                        "Time Zone", jw__timezone_label(ui->timezone), true);
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_RESET_RETROARCH,
                        "Reset RetroArch Config", "Defaults", true);
    jw__render_list_row(&ui->behavior_list, x, ly, w, JW_BEHAVIOR_UNMOUNT_SECONDARY,
                        "Unmount Secondary SD",
                        ui->secondary_sd_status[0] ? ui->secondary_sd_status : "Unavailable",
                        true);
}

static void jw__format_update_size(long long bytes, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (bytes <= 0) {
        snprintf(out, out_size, "%s", "-");
        return;
    }
    if (bytes >= 1024LL * 1024LL * 1024LL) {
        snprintf(out, out_size, "%.1f GB",
                 (double)bytes / (double)(1024LL * 1024LL * 1024LL));
    } else if (bytes >= 1024LL * 1024LL) {
        snprintf(out, out_size, "%.1f MB",
                 (double)bytes / (double)(1024LL * 1024LL));
    } else {
        snprintf(out, out_size, "%.1f KB", (double)bytes / 1024.0);
    }
}

static const char *jw__update_state_label(const jw_ipc_update_status_info *u) {
    if (!u || !u->state[0]) {
        return "Idle";
    }
    if (strcmp(u->state, "up-to-date") == 0) return "Up to date";
    if (strcmp(u->state, "available") == 0) return "Available";
    if (strcmp(u->state, "downloading") == 0) return "Downloading";
    if (strcmp(u->state, "downloaded") == 0) return "Downloaded";
    if (strcmp(u->state, "installing") == 0) return "Installing";
    if (strcmp(u->state, "armed") == 0) return "Restart needed";
    if (strcmp(u->state, "cancelled") == 0) return "Cancelled";
    if (strcmp(u->state, "incompatible") == 0) return "Incompatible";
    if (strcmp(u->state, "error") == 0) return "Error";
    return "Idle";
}

static void jw__update_download_label(const jw_settings_ui *ui,
                                      char *out,
                                      size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const jw_ipc_update_status_info *u = ui ? &ui->update : NULL;
    if (!ui || !ui->update_have_status) {
        snprintf(out, out_size, "%s", "Unavailable");
    } else if (u->download_active) {
        if (u->download_percent >= 0) {
            snprintf(out, out_size, "Cancel %d%%", u->download_percent);
        } else {
            snprintf(out, out_size, "%s", "Cancel");
        }
    } else if (u->downloaded) {
        snprintf(out, out_size, "%s", "Verified");
    } else if (u->compatible && u->artifact_name[0]) {
        snprintf(out, out_size, "%s", "Download");
    } else {
        snprintf(out, out_size, "%s", "Unavailable");
    }
}

static const char *jw__update_blocked_label(const char *reason) {
    if (!reason || !reason[0]) return "Blocked";
    if (strcmp(reason, "install_again") == 0) return "Install Again";
    if (strcmp(reason, "primary_slot_needed") == 0) return "Primary Slot";
    if (strcmp(reason, "not_idle") == 0) return "Close Apps";
    if (strcmp(reason, "space_low") == 0) return "No Space";
    if (strcmp(reason, "battery_low") == 0) return "Battery Low";
    if (strcmp(reason, "download_active") == 0) return "Downloading";
    if (strcmp(reason, "not_downloaded") == 0) return "Download First";
    if (strcmp(reason, "download_missing") == 0) return "Missing";
    if (strcmp(reason, "unsupported_handoff") == 0) return "Unsupported";
    if (strcmp(reason, "unsupported_artifact") == 0) return "Unsupported";
    return "Blocked";
}

static void jw__update_install_label(const jw_settings_ui *ui,
                                     char *out,
                                     size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const jw_ipc_update_status_info *u = ui ? &ui->update : NULL;
    if (!ui || !ui->update_have_status) {
        snprintf(out, out_size, "%s", "Unavailable");
    } else if (u->install_active) {
        snprintf(out, out_size, "%s", "Installing");
    } else if (u->install_armed) {
        snprintf(out, out_size, "%s", "Restart");
    } else if (u->install_ready) {
        snprintf(out, out_size, "%s", "Install");
    } else if (u->install_needs_confirmation) {
        snprintf(out, out_size, "%s", "Confirm");
    } else if (u->install_blocked) {
        snprintf(out, out_size, "%s",
                 jw__update_blocked_label(u->install_reason));
    } else if (u->downloaded) {
        snprintf(out, out_size, "%s", "Ready Check");
    } else if (u->install_result_state[0]) {
        if (strcmp(u->install_result_state, "installed") == 0) {
            snprintf(out, out_size, "%s", "Installed");
        } else if (strcmp(u->install_result_state, "armed") == 0) {
            snprintf(out, out_size, "%s", "Restart Needed");
        } else if (strcmp(u->install_result_state, "error") == 0) {
            snprintf(out, out_size, "%s", "Failed");
        } else {
            snprintf(out, out_size, "%s", u->install_result_state);
        }
    } else {
        snprintf(out, out_size, "%s", "Unavailable");
    }
}

static void jw__draw_update_progress(const jw_settings_ui *ui,
                                     int x, int y, int w) {
    if (!ui || !ui->update.download_active || ui->update.download_percent < 0) {
        return;
    }
    ap_theme *theme = cat_get_theme();
    int track_h = cat_scale(5);
    int fill_w = (w * ui->update.download_percent) / 100;
    cat_draw_rect(x, y, w, track_h, cat_hex_to_color("#ffffff33"));
    cat_draw_rect(x, y, fill_w, track_h, theme->accent);
}

static void jw__draw_update_activity(const jw_settings_ui *ui,
                                     int x, int y, int w) {
    if (!ui || !ui->update.install_active || w <= 0) {
        return;
    }
    ap_theme *theme = cat_get_theme();
    int track_h = cat_scale(5);
    int segment_w = w / 3;
    int min_segment = cat_scale(28);
    if (segment_w < min_segment) {
        segment_w = min_segment;
    }
    if (segment_w > w) {
        segment_w = w;
    }

    int travel = w + segment_w;
    int pos = (int)((SDL_GetTicks() / 8u) % (unsigned)travel) - segment_w;
    int fill_x = x + pos;
    int fill_w = segment_w;
    if (fill_x < x) {
        fill_w -= x - fill_x;
        fill_x = x;
    }
    if (fill_x + fill_w > x + w) {
        fill_w = x + w - fill_x;
    }

    cat_draw_rect(x, y, w, track_h, cat_hex_to_color("#ffffff33"));
    if (fill_w > 0) {
        cat_draw_rect(fill_x, y, fill_w, track_h, theme->accent);
    }
}

static void jw__render_update(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("System Update", x, y, w);
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int line_h = jw__subheader_line_h(small);
    /* Hybrid page: a runtime-variable status block (message + optional progress
       bar) then fixed rows, so it keeps its own dy flow below the content origin
       rather than carving a fixed sub-header strip. */
    int dy = jw__settings_boxes(x, y, w, h, true, 0, NULL, NULL).y;

    const jw_ipc_update_status_info *u = &ui->update;
    char message[320];
    if (ui->update_have_status && u->install_active && u->install_message[0]) {
        snprintf(message, sizeof(message), "%s", u->install_message);
    } else if (ui->update_msg[0]) {
        snprintf(message, sizeof(message), "%s", ui->update_msg);
    } else if (ui->update_have_status) {
        if (u->install_message[0] &&
            (u->install_active || u->install_armed ||
             u->install_blocked || u->install_needs_confirmation ||
             u->install_ready)) {
            snprintf(message, sizeof(message), "%s", u->install_message);
        } else if (u->message[0]) {
            snprintf(message, sizeof(message), "%s", u->message);
        } else if (u->install_result_message[0]) {
            snprintf(message, sizeof(message), "Last update: %s",
                     u->install_result_message);
        } else {
            snprintf(message, sizeof(message), "%s", jw__update_state_label(u));
        }
    } else {
        snprintf(message, sizeof(message), "%s", "Update status unavailable");
    }

    cat_draw_text_ellipsized(small, message, x + cat_scale(12), dy,
                             theme->hint, w - cat_scale(24));
    dy += line_h;

    if (ui->update.download_active) {
        jw__draw_update_progress(ui, x + cat_scale(12), dy + cat_scale(2),
                                 w - cat_scale(24));
        dy += cat_scale(12);
    } else if (ui->update.install_active) {
        jw__draw_update_activity(ui, x + cat_scale(12), dy + cat_scale(2),
                                 w - cat_scale(24));
        dy += cat_scale(12);
    }

    int ly = dy + cat_scale(2);
    char download_value[48];
    char install_value[64];
    char current_value[128];
    char candidate_value[224];
    char check_value[64];
    char size_value[64];
    jw__update_download_label(ui, download_value, sizeof(download_value));
    jw__update_install_label(ui, install_value, sizeof(install_value));

    snprintf(check_value, sizeof(check_value), "%s", jw__update_state_label(u));
    snprintf(current_value, sizeof(current_value), "%s",
             (ui->update_have_status && u->current_release_id[0])
             ? u->current_release_id
             : (ui->update_have_status && u->current_unknown ? "Unknown" : "-"));
    if (ui->update_have_status && u->release_id[0]) {
        jw__format_update_size(u->artifact_size, size_value, sizeof(size_value));
        snprintf(candidate_value, sizeof(candidate_value), "%s (%s)",
                 u->release_id, size_value);
    } else if (ui->update_have_status && u->install_result_release_id[0]) {
        snprintf(candidate_value, sizeof(candidate_value), "Last: %s",
                 u->install_result_release_id);
    } else if (ui->update_have_status && strcmp(u->state, "up-to-date") == 0) {
        snprintf(candidate_value, sizeof(candidate_value), "%s", "None");
    } else {
        snprintf(candidate_value, sizeof(candidate_value), "%s", "-");
    }

    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_CHECK,
                        "Check Releases", check_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_DOWNLOAD,
                        "Download", download_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_INSTALL,
                        "Install", install_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_CURRENT,
                        "Current", current_value, false);
    jw__render_list_row(&ui->update_list, x, ly, w, JW_UPDATE_ROW_AVAILABLE,
                        "Available", candidate_value, false);
}

typedef struct {
    const jw_settings_ui *ui;
} jw__update_picker_ctx;

static const char *jw__update_option_badge(const jw_ipc_update_option_info *option,
                                           int idx) {
    if (!option) {
        return "";
    }
    if (option->selected) {
        return "Selected";
    }
    if (option->installed) {
        return "Installed";
    }
    return idx == 0 ? "Latest" : "Older";
}

static void jw__draw_update_option_item(int idx, int ix, int iy, int iw, int ih,
                                        bool selected, void *user) {
    jw__update_picker_ctx *ctx = (jw__update_picker_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    if (!ui || idx < 0 || idx >= ui->update.option_count ||
        idx >= JW_IPC_UPDATE_MAX_OPTIONS) {
        return;
    }

    const jw_ipc_update_option_info *option = &ui->update.options[idx];
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int pad = cat_scale(10);
    int pill_h = ih - cat_scale(4);
    int pill_y = iy + cat_scale(2);
    if (selected) {
        cat_draw_pill(ix, pill_y, iw - cat_scale(4), pill_h, theme->highlight);
    }

    ap_color main_color = selected ? theme->highlighted_text : theme->text;
    ap_color hint_color = selected ? theme->highlighted_text : theme->hint;
    const char *label = option->release_id[0] ? option->release_id : "Leaf update";
    char detail[320];
    char size[64];
    jw__format_update_size(option->artifact_size, size, sizeof(size));
    snprintf(detail, sizeof(detail), "%s%s%s",
             option->artifact_name[0] ? option->artifact_name : option->artifact_kind,
             size[0] && strcmp(size, "-") != 0 ? "  " : "",
             size[0] && strcmp(size, "-") != 0 ? size : "");

    int badge_w = cat_scale(92);
    cat_draw_text_ellipsized(body, label, ix + pad, iy + cat_scale(5),
                             main_color, iw - pad * 2 - badge_w);
    cat_draw_text_ellipsized(small, detail, ix + pad,
                             iy + cat_scale(5) + TTF_FontHeight(body),
                             hint_color, iw - pad * 2 - badge_w);
    cat_draw_text_ellipsized(small, jw__update_option_badge(option, idx),
                             ix + iw - badge_w - pad, iy + cat_scale(8),
                             hint_color, badge_w);
}

static void jw__render_update_picker(const jw_settings_ui *ui,
                                      int x, int y, int w, int h) {
    jw__draw_header("Pick Update", x, y, w);
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    int dy = y + jw__header_h() + cat_scale(6);
    int count = ui ? ui->update.option_count : 0;
    if (count > JW_IPC_UPDATE_MAX_OPTIONS) {
        count = JW_IPC_UPDATE_MAX_OPTIONS;
    }

    const char *message = count > 0
        ? "Compatible releases"
        : "Check releases first";
    cat_draw_text_ellipsized(small, message, x + cat_scale(12), dy,
                             theme->hint, w - cat_scale(24));
    dy += TTF_FontHeight(small) + cat_scale(8);

    if (count > 0) {
        int item_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) +
                     TTF_FontHeight(small) + cat_scale(16);
        cat_box lb = { x, dy, w, h - (dy - y), 0, 0, 0, 0 };
        int vis = 0;
        SDL_Rect lr = cat_box_fit_rows(&lb, item_h, count, &vis, &item_h);
        ((cat_list_state *)&ui->update_picker_list)->visible_rows = vis;
        jw__update_picker_ctx ctx = { ui };
        cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h, count,
                           &ui->update_picker_list, item_h,
                           jw__draw_update_option_item, &ctx);
    }
}

typedef struct {
    const jw_settings_ui *ui;
} jw__timezone_picker_ctx;

/* Single-line row: friendly label on the left ("* " marks the current zone),
   IANA id on the right in hint color — mirrors the Network/Bluetooth list rows. */
static void jw__draw_timezone_item(int idx, int ix, int iy, int iw, int ih,
                                   bool selected, void *user) {
    jw__timezone_picker_ctx *ctx = (jw__timezone_picker_ctx *)user;
    const jw_settings_ui *ui = ctx ? ctx->ui : NULL;
    if (idx < 0 || idx >= JW_TIMEZONE_COUNT) {
        return;
    }
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);

    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (ih - pill_h) / 2;
    if (selected) {
        cat_draw_pill(ix, pill_y, iw - cat_scale(4), pill_h, theme->highlight);
    }
    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    const char *cur = ui ? ui->timezone : "";
    bool is_current = (cur[0] && strcmp(cur, kTimeZones[idx].tz) == 0);
    char label[48];
    snprintf(label, sizeof(label), "%s%s", is_current ? "* " : "",
             kTimeZones[idx].label);
    cat_draw_text_ellipsized(body, label, ix + cat_scale(12), ty, label_c, iw / 2);

    int vw = cat_measure_text(body, kTimeZones[idx].off);
    cat_draw_text(body, kTimeZones[idx].off, ix + iw - vw - cat_scale(16), ty, value_c);
}

static void jw__render_timezone_picker(const jw_settings_ui *ui,
                                       int x, int y, int w, int h) {
    jw__draw_header("Time Zone", x, y, w);
    ap_theme *theme = cat_get_theme();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int sub_h = jw__subheader_line_h(small) + cat_scale(6);
    SDL_Rect sub;
    SDL_Rect c = jw__settings_boxes(x, y, w, h, true, sub_h, NULL, &sub);
    cat_draw_text_ellipsized(small, "Set your local time zone", sub.x + cat_scale(12),
                             sub.y, theme->hint, sub.w - cat_scale(24));

    int item_h = TTF_FontHeight(body) + cat_scale(12);
    cat_box lb = { c.x, c.y, c.w, c.h, 0, 0, 0, 0 };
    int vis = 0;
    SDL_Rect lr = cat_box_fit_rows(&lb, item_h, JW_TIMEZONE_COUNT, &vis, &item_h);
    ((cat_list_state *)&ui->timezone_picker_list)->visible_rows = vis;
    jw__timezone_picker_ctx ctx = { ui };
    cat_draw_list_pane(lr.x, lr.y, lr.w, lr.h, JW_TIMEZONE_COUNT,
                       &ui->timezone_picker_list, item_h,
                       jw__draw_timezone_item, &ctx);
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
        case JW_SETTINGS_BLUETOOTH:  jw__render_bluetooth(ui, x, y, w, h);  break;
        case JW_SETTINGS_LIGHTING:   jw__render_lighting(ui, x, y, w, h);   break;
        case JW_SETTINGS_ACCOUNTS:   jw__render_accounts(ui, x, y, w, h);                break;
        case JW_SETTINGS_SCRAPING:   jw__render_scraping(ui, x, y, w, h);                break;
        case JW_SETTINGS_SCRAPE_PRIORITY: jw__render_scrape_priority(ui, x, y, w, h);    break;
        case JW_SETTINGS_BEHAVIOR:   jw__render_behavior(ui, x, y, w, h);                 break;
        case JW_SETTINGS_UPDATE:     jw__render_update(ui, x, y, w, h);                  break;
        case JW_SETTINGS_UPDATE_PICKER: jw__render_update_picker(ui, x, y, w, h);        break;
        case JW_SETTINGS_TIMEZONE_PICKER: jw__render_timezone_picker(ui, x, y, w, h);   break;
        case JW_SETTINGS_ABOUT:      jw__render_about(ui, x, y, w, h);                   break;
    }
}

void jw_settings_apply_persisted_overrides(const char *db_path) {
    if (!db_path || !db_path[0]) return;

    char values[JW_SETTING_COUNT][JW_SETTINGS_VALUE_MAX];
    unsigned char found[JW_SETTING_COUNT];
    if (jw__load_setting_values(db_path, values, found) == 0)
        jw__apply_persisted_overrides_from_values(values, found);
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
        if (ui->audio_output >= 0 &&
            ui->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
            ui->audio_volumes[ui->audio_output] = resolved;
        }
        return;
    }
    if (status_buf && status_size > 0)
        snprintf(status_buf, status_size, "%s", "volume failed");
}

static bool jw__audio_output_available(const jw_settings_ui *ui,
                                       jw_platform_audio_output output) {
    return ui && output >= 0 && output < JW_PLATFORM_AUDIO_OUTPUT_COUNT &&
           (ui->audio_available_outputs & JW_PLATFORM_AUDIO_OUTPUT_BIT(output));
}

static jw_platform_audio_output jw__next_audio_output(const jw_settings_ui *ui,
                                                      int direction) {
    jw_platform_audio_output choices[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
    int count = 0;
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        jw_platform_audio_output output = (jw_platform_audio_output)i;
        if (jw__audio_output_available(ui, output)) {
            choices[count++] = output;
        }
    }
    if (count == 0) {
        return JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    }

    int current = 0;
    for (int i = 0; i < count; i++) {
        if (choices[i] == ui->audio_output) {
            current = i;
            break;
        }
    }

    int next = direction < 0
        ? (current + count - 1) % count
        : (current + 1) % count;
    return choices[next];
}

static void jw__set_audio_output(jw_settings_ui *ui, jw_platform_audio_output output,
                                 char *status_buf, size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        if (status_buf && status_size > 0)
            snprintf(status_buf, status_size, "%s", "audio output failed");
        return;
    }
    if (jw_ipc_set_audio_output(ui->socket_path, output, status_buf,
                                (int)status_size) == 0) {
        ui->audio_output = output;
        jw__refresh_audio_status(ui);
        return;
    }
    jw__refresh_audio_status(ui);
}

/* After a Bluetooth headset is disconnected or unpaired the codec is still
   pointed at the (now gone) Bluetooth output, so audio would play to nothing.
   When that is the case, fall back to the wired jack if it is plugged in, else
   the speaker — so sound returns on its own instead of staying silent. */
static void jw__bt_route_back_if_orphaned(jw_settings_ui *ui) {
    if (!ui) {
        return;
    }
    jw__refresh_audio_status(ui);
    if (ui->audio_output != JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH ||
        jw__audio_output_available(ui, JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH)) {
        return;
    }
    jw_platform_audio_output back =
        jw__audio_output_available(ui, JW_PLATFORM_AUDIO_OUTPUT_HEADSET)
            ? JW_PLATFORM_AUDIO_OUTPUT_HEADSET
            : JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    char status[128];
    jw__set_audio_output(ui, back, status, sizeof(status));
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

static bool jw__confirm_update_install(const char *release_id) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Install", .is_confirm = true },
    };
    char message[192];
    snprintf(message, sizeof(message), "Install Leaf %s?",
             release_id && release_id[0] ? release_id : "update");
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_update_unknown_preflight(const char *message) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Continue", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message && message[0]
                   ? message
                   : "Install update even though checks are incomplete?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_update_reboot(void) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Later", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Restart", .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = "Restart now to finish installing?",
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static bool jw__confirm_bt_unpair(const char *name) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Unpair", .is_confirm = true },
    };
    char message[160];
    snprintf(message, sizeof(message), "Unpair %s?", name && name[0] ? name : "device");
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__copy_status(char *status_buf, size_t status_size,
                            const char *value) {
    if (status_buf && status_size > 0) {
        snprintf(status_buf, status_size, "%s", value ? value : "");
    }
}

static void jw__settings_update_from_ipc(jw_settings_ui *ui,
                                         const jw_ipc_update_status_info *info,
                                         const char *message) {
    if (!ui) {
        return;
    }
    if (info) {
        ui->update = *info;
        ui->update_have_status = true;
        int count = ui->update.option_count;
        if (count > JW_IPC_UPDATE_MAX_OPTIONS) {
            count = JW_IPC_UPDATE_MAX_OPTIONS;
        }
        if (count > 0) {
            int cursor = ui->update.selected_option >= 0
                ? ui->update.selected_option
                : ui->update_picker_list.cursor;
            if (cursor < 0) cursor = 0;
            if (cursor >= count) cursor = count - 1;
            ui->update_picker_list.cursor = cursor;
            if (ui->update_picker_list.scroll_offset > cursor) {
                ui->update_picker_list.scroll_offset = cursor;
            }
        } else {
            ui->update_picker_list.cursor = 0;
            ui->update_picker_list.scroll_offset = 0;
        }
    }
    if (message && message[0]) {
        jw__update_msg(ui, "%s", message);
    }
    ui->update_next_poll_ms = SDL_GetTicks() +
        (jw_settings_ui_wants_update_poll(ui) ? 500u : 3000u);
}

static int jw__update_option_count(const jw_settings_ui *ui) {
    if (!ui) {
        return 0;
    }
    int count = ui->update.option_count;
    if (count < 0) {
        count = 0;
    }
    if (count > JW_IPC_UPDATE_MAX_OPTIONS) {
        count = JW_IPC_UPDATE_MAX_OPTIONS;
    }
    return count;
}

static void jw__update_check_releases(jw_settings_ui *ui,
                                      char *status_buf,
                                      size_t status_size);

static bool jw__confirm_update_picker_choice(const jw_settings_ui *ui,
                                             const jw_ipc_update_option_info *option,
                                             int idx) {
    if (!option) {
        return false;
    }
    if (!option->installed && idx == 0) {
        return true;
    }

    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel", .is_confirm = false },
        { .button = CAT_BTN_A, .label = "Pick", .is_confirm = true },
    };
    char message[192];
    if (option->installed) {
        snprintf(message, sizeof(message), "Pick installed Leaf %s again?",
                 option->release_id[0] ? option->release_id : "release");
    } else {
        snprintf(message, sizeof(message), "Pick older Leaf %s?",
                 option->release_id[0] ? option->release_id : "release");
    }
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    (void)ui;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__open_update_picker(jw_settings_ui *ui,
                                   char *status_buf,
                                   size_t status_size) {
    if (!ui) {
        return;
    }
    if (jw__update_option_count(ui) <= 0) {
        jw__update_check_releases(ui, status_buf, status_size);
    }
    int count = jw__update_option_count(ui);
    if (count <= 0) {
        jw__copy_status(status_buf, status_size,
                        ui->update_msg[0] ? ui->update_msg : "No releases available");
        return;
    }
    if (ui->update_picker_list.cursor < 0 ||
        ui->update_picker_list.cursor >= count) {
        ui->update_picker_list.cursor =
            ui->update.selected_option >= 0 && ui->update.selected_option < count
            ? ui->update.selected_option
            : 0;
    }
    ui->screen = JW_SETTINGS_UPDATE_PICKER;
}

static void jw__select_update_picker_choice(jw_settings_ui *ui,
                                            char *status_buf,
                                            size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    int count = jw__update_option_count(ui);
    int idx = ui->update_picker_list.cursor;
    if (idx < 0 || idx >= count) {
        jw__copy_status(status_buf, status_size, "No update release selected");
        jw__update_msg(ui, "No update release selected");
        return;
    }

    const jw_ipc_update_option_info *option = &ui->update.options[idx];
    if (!jw__confirm_update_picker_choice(ui, option, idx)) {
        jw__copy_status(status_buf, status_size, "Update selection canceled");
        jw__update_msg(ui, "Update selection canceled");
        return;
    }

    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    if (jw_ipc_update_select(ui->socket_path, option->index,
                             &info, status, sizeof(status)) == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
        ui->screen = JW_SETTINGS_UPDATE;
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update selection failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update selection failed");
    }
}

static void jw__update_check_releases(jw_settings_ui *ui,
                                      char *status_buf,
                                      size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    if (jw_ipc_update_check(ui->socket_path, NULL, &info,
                            status, sizeof(status)) == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update check failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update check failed");
    }
}

static void jw__update_download_or_cancel(jw_settings_ui *ui,
                                          char *status_buf,
                                          size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    int rc = 0;
    if (ui->update.download_active) {
        rc = jw_ipc_update_cancel(ui->socket_path, &info, status, sizeof(status));
    } else if (ui->update.downloaded) {
        jw__copy_status(status_buf, status_size, "Update already downloaded");
        jw__update_msg(ui, "Update already downloaded");
        return;
    } else if (ui->update.compatible && ui->update.artifact_name[0]) {
        rc = jw_ipc_update_download(ui->socket_path, &info, status, sizeof(status));
    } else {
        jw__copy_status(status_buf, status_size, "Check for updates first");
        jw__update_msg(ui, "Check for updates first");
        return;
    }

    if (rc == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update download failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update download failed");
    }
}

static void jw__update_install_or_reboot(jw_settings_ui *ui,
                                         char *status_buf,
                                         size_t status_size) {
    if (!ui || !ui->socket_path[0]) {
        jw__copy_status(status_buf, status_size, "Update service unavailable");
        if (ui) jw__update_msg(ui, "Update service unavailable");
        return;
    }

    if (ui->update.install_armed) {
        if (!jw__confirm_update_reboot()) {
            jw__copy_status(status_buf, status_size, "Restart canceled");
            jw__update_msg(ui, "Restart canceled");
            return;
        }
        if (jw_ipc_platform_action(ui->socket_path, "reboot", 0) == 0) {
            jw__copy_status(status_buf, status_size, "Restarting");
            jw__update_msg(ui, "Restarting");
        } else {
            jw__copy_status(status_buf, status_size, "Restart failed");
            jw__update_msg(ui, "Restart failed");
        }
        return;
    }

    if (ui->update.install_active) {
        jw__refresh_update_status(ui, false);
        jw__copy_status(status_buf, status_size,
                        ui->update_msg[0] ? ui->update_msg : "Install in progress");
        return;
    }

    if (ui->update.install_blocked) {
        const char *msg = ui->update.install_message[0]
            ? ui->update.install_message
            : "Update install blocked";
        jw__copy_status(status_buf, status_size, msg);
        jw__update_msg(ui, "%s", msg);
        return;
    }

    if (!ui->update.downloaded) {
        jw__copy_status(status_buf, status_size, "Download update first");
        jw__update_msg(ui, "Download update first");
        return;
    }

    jw_ipc_update_status_info info;
    memset(&info, 0, sizeof(info));
    char status[192] = { 0 };
    if (jw_ipc_update_install_preflight(ui->socket_path, false, &info,
                                        status, sizeof(status)) != 0) {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update ready check failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update ready check failed");
        return;
    }
    jw__settings_update_from_ipc(ui, &info, status);

    if (ui->update.install_blocked) {
        const char *msg = ui->update.install_message[0]
            ? ui->update.install_message
            : "Update install blocked";
        jw__copy_status(status_buf, status_size, msg);
        jw__update_msg(ui, "%s", msg);
        return;
    }

    bool confirm_unknown_battery = false;
    if (ui->update.install_needs_confirmation) {
        const char *msg = ui->update.install_message[0]
            ? ui->update.install_message
            : "Install update even though checks are incomplete?";
        if (!jw__confirm_update_unknown_preflight(msg)) {
            jw__copy_status(status_buf, status_size, "Update install canceled");
            jw__update_msg(ui, "Update install canceled");
            return;
        }
        confirm_unknown_battery = true;
    } else if (!ui->update.install_ready) {
        jw__copy_status(status_buf, status_size, "Update is not ready to install");
        jw__update_msg(ui, "Update is not ready to install");
        return;
    }

    if (!jw__confirm_update_install(ui->update.release_id)) {
        jw__copy_status(status_buf, status_size, "Update install canceled");
        jw__update_msg(ui, "Update install canceled");
        return;
    }

    memset(&info, 0, sizeof(info));
    status[0] = '\0';
    if (jw_ipc_update_install(ui->socket_path, confirm_unknown_battery,
                              &info, status, sizeof(status)) == 0) {
        jw__settings_update_from_ipc(ui, &info, status);
        jw__copy_status(status_buf, status_size, status);
        cat_request_frame_in(500);
    } else {
        jw__settings_update_from_ipc(ui, &info,
                                    status[0] ? status : "Update install failed");
        jw__copy_status(status_buf, status_size,
                        status[0] ? status : "Update install failed");
    }
}

static void jw__set_adb(jw_settings_ui *ui, bool enabled,
                        char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!ui->adb_supported || ui->adb_enabled < 0) {
        snprintf(status_buf, status_size, "%s", "ADB unavailable on this platform");
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

static void jw__set_boot_splash(jw_settings_ui *ui, bool enabled,
                                char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!ui->boot_splash_supported) {
        snprintf(status_buf, status_size, "%s", "boot splash unavailable on this platform");
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_set_boot_splash(ui->socket_path, enabled ? 1 : 0,
                               status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s",
                 enabled ? "boot splash enable failed" : "boot splash disable failed");
    }
    jw__refresh_boot_splash(ui);
    jw__persist_bool(ui, "boot_splash_enabled", ui->boot_splash_enabled);
}

static void jw__set_refresh_rate(jw_settings_ui *ui, int hz,
                                 char *status_buf, size_t status_size) {
    if (!ui || !status_buf || status_size == 0) {
        return;
    }

    if (!ui->refresh_rate_supported) {
        snprintf(status_buf, status_size, "%s", "refresh rate unavailable on this platform");
        return;
    }

    status_buf[0] = '\0';
    if (jw_ipc_set_refresh_rate(ui->socket_path, hz, status_buf, (int)status_size) != 0 &&
        !status_buf[0]) {
        snprintf(status_buf, status_size, "%s", "refresh rate change failed");
        return;
    }
    /* The daemon restarts Weston and respawns this launcher, so re-querying now
       would read the pre-restart rate; persist the choice optimistically and let
       the fresh launcher pick up the live rate on its next status poll. */
    ui->refresh_rate_hz = hz;
    jw__persist_int(ui, "refresh_rate_hz", hz);
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
            /* Left/Right jump to the top/bottom of the category list. Only safe on
               this pure-navigation list: the sub-pages use Left/Right to change
               values, so a jump there would be an easy mis-press. */
            case CAT_BTN_LEFT:  cat_list_state_jump(&ui->home_list, 0, JW_SETTINGS_CATEGORY_COUNT); break;
            case CAT_BTN_RIGHT: cat_list_state_jump(&ui->home_list, JW_SETTINGS_CATEGORY_COUNT - 1, JW_SETTINGS_CATEGORY_COUNT); break;
            case CAT_BTN_A: {
                int idx = ui->home_list.cursor;
                if (idx == 0) ui->screen = JW_SETTINGS_APPEARANCE;
                else if (idx == 1) {
                    ui->screen = JW_SETTINGS_DISPLAY;
                    /* Re-sync to live values so an OSD/hardware change made
                       outside settings isn't stale before we adjust. */
                    jw__refresh_brightness(ui);
                    jw__refresh_volume(ui);
                    jw__refresh_audio_status(ui);
                    jw__refresh_refresh_rate(ui);
                }
                else if (idx == 3) {
                    ui->screen = JW_SETTINGS_NETWORK;
                    ui->network_list.cursor = 0;
                    ui->network_list.scroll_offset = 0;
                    ui->wifi_msg[0] = '\0';
                    jw__wifi_attempt_clear(ui);
                    jw__refresh_adb(ui);
                    ui->wifi_radio_on = jw_wifi_available() && jw_wifi_radio_is_on();
                    jw__refresh_wifi(ui);          /* show status immediately */
                    if (ui->wifi_radio_on) {
                        jw_wifi_scan_start();      /* kick a scan */
                        jw__refresh_wifi_scan(ui); /* show any cached results now */
                    } else {
                        ui->wifi_network_count = 0;
                    }
                    unsigned now = SDL_GetTicks();
                    ui->wifi_next_poll_ms = now + 2000;            /* then live every ~2s */
                    ui->wifi_next_scan_ms = now + JW_WIFI_SCAN_INTERVAL_MS;
                }
                else if (idx == 4) {
                    ui->screen = JW_SETTINGS_BLUETOOTH;
                    ui->bluetooth_list.cursor = 0;
                    ui->bluetooth_list.scroll_offset = 0;
                    ui->bt_msg[0] = '\0';
                    ui->bt_op = JW_BT_OP_NONE;
                    ui->bt_op_manual = false;
                    unsigned now = SDL_GetTicks();
                    ui->bt_next_poll_ms = now + JW_BT_ENTRY_DEFER_MS;
                    ui->bt_next_scan_ms = now + JW_BT_ENTRY_DEFER_MS;
                    cat_request_frame_in(JW_BT_ENTRY_DEFER_MS);
                }
                else if (idx == 2) {
                    ui->screen = JW_SETTINGS_LIGHTING;
                    jw__refresh_led(ui);
                }
                else if (idx == 5) {
                    ui->screen = JW_SETTINGS_SCRAPING;
                    ui->scraping_list.cursor = 0;
                    ui->scraping_list.scroll_offset = 0;
                }
                else if (idx == 6) ui->screen = JW_SETTINGS_ACCOUNTS;
                else if (idx == 7) {
                    ui->screen = JW_SETTINGS_BEHAVIOR;
                    jw__refresh_boot_splash(ui);
                    jw__refresh_performance(ui);
                    jw__refresh_secondary_sd_status(ui);   /* Unmount SD row lives here now */
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
                    /* Re-assert the selected family before the bump reload — otherwise
                       cat_set_font_bump reloads via theme.font_path, which can be empty,
                       and falls back to res/font.ttf (clobbering the chosen font). */
                    ap_theme *ft = cat_get_theme();
                    snprintf(ft->font_path, sizeof(ft->font_path), "%s",
                             jw_appearance_font_path_for_index(ui->font_family_index));
                    cat_set_font_bump(kJawakaFontSizeValues[next]);
                    jw__persist_int(ui, "font_size_index", next);
                } else if (row == JW_LAYOUT_TAB_SWITCH) {
                    int next = (ui->tab_glide + dir + JW_TAB_SWITCH_COUNT) % JW_TAB_SWITCH_COUNT;
                    ui->tab_glide = next;
                    jw__persist_int(ui, "tab_glide", next);
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
                } else if (row == JW_STATUSBAR_BLUETOOTH) {
                    ui->show_bluetooth = !ui->show_bluetooth;
                    jw__persist_bool(ui, "show_bluetooth", ui->show_bluetooth);
                    if (ui->show_bluetooth) jw_settings_ui_refresh_bt_state(ui);
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
                else if (ui->display_list.cursor == JW_DISPLAY_REFRESH_RATE) {
                    /* Cycle 60 -> 90 -> 120 (left/right step, A advances). */
                    static const int rates[] = {60, 90, 120};
                    int n = (int)(sizeof(rates) / sizeof(rates[0]));
                    int cur = 0;
                    for (int i = 0; i < n; i++) {
                        if (rates[i] == ui->refresh_rate_hz) { cur = i; break; }
                    }
                    int next = (cur + dir + n) % n;
                    jw__set_refresh_rate(ui, rates[next], status_buf, status_size);
                }
                else if (ui->display_list.cursor == JW_DISPLAY_OUTPUT)
                    jw__set_audio_output(ui, jw__next_audio_output(ui, dir),
                                         status_buf, status_size);
                else if (ui->display_list.cursor == JW_DISPLAY_VOLUME)
                    jw__change_volume(ui, dir * JW_PLATFORM_VOLUME_STEP_PERCENT,
                                      status_buf, status_size);
                else if (ui->display_list.cursor == JW_DISPLAY_TEST_SOUND &&
                         button == CAT_BTN_A) {
                    /* Toggle: the daemon plays the clip on the current output, or
                       stops it if already playing (left/right do nothing here).
                       Reflect the action from the last-known state; the 300ms
                       av-poll then syncs the Play/Stop label. */
                    bool was_playing = ui->test_sound_playing;
                    jw_ipc_platform_action(ui->socket_path, "play-test-sound", 0);
                    ui->test_sound_playing = !was_playing;
                    snprintf(status_buf, status_size, "%s",
                             was_playing ? "Stopped test sound" : "Playing test sound…");
                }
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
        bool wifi_available = jw_wifi_available();
        int row_count = JW_NETWORK_FIXED_ROWS +
                        ((wifi_available && ui->wifi_radio_on) ? ui->wifi_network_count : 0);
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
                    if (!wifi_available) {
                        jw__wifi_msg(ui, "Wi-Fi unavailable on this platform");
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                        }
                        break;
                    }
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
                    if (!ui->adb_supported || ui->adb_enabled < 0) {
                        jw__wifi_msg(ui, "ADB unavailable on this platform");
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, status_size, "%s", ui->wifi_msg);
                        }
                        break;
                    }
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
                    char prompt[160];
                    snprintf(prompt, sizeof(prompt),
                             "Password for %s\nStart: Confirm\nY: Cancel",
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

    /* -- Bluetooth (scan/pair/connect/manage) ------------------------- */
    case JW_SETTINGS_BLUETOOTH: {
        int row_count = jw__bt_row_count(ui);
        if (row_count < 1) {
            row_count = 1;
        }
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->bluetooth_list, -1, row_count);
                for (int g = 0; g < row_count &&
                     jw__bt_row_is_header(ui, ui->bluetooth_list.cursor); g++)
                    cat_list_state_move(&ui->bluetooth_list, -1, row_count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->bluetooth_list, +1, row_count);
                for (int g = 0; g < row_count &&
                     jw__bt_row_is_header(ui, ui->bluetooth_list.cursor); g++)
                    cat_list_state_move(&ui->bluetooth_list, +1, row_count);
                break;
            case CAT_BTN_A: {
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw__bt_msg(ui, "Bluetooth is busy");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }

                int row = ui->bluetooth_list.cursor;
                if (row == JW_BLUETOOTH_ROW_POWER) {
                    bool turning_on = !ui->bt_radio_on;
                    jw__bt_msg(ui, turning_on ? "Turning Bluetooth on..."
                                               : "Turning Bluetooth off...");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    if (jw_bt_set_radio(turning_on) != 0) {
                        jw__bt_msg(ui, turning_on ? "Bluetooth on failed"
                                                  : "Bluetooth off failed");
                    } else {
                        jw__bt_msg(ui, turning_on ? "Bluetooth on"
                                                  : "Bluetooth off");
                    }
                    jw__refresh_bluetooth_lists(ui);
                    ui->bt_next_poll_ms = SDL_GetTicks() + JW_BT_POLL_INTERVAL_MS;
                    break;
                }

                if (row == JW_BLUETOOTH_ROW_NAME) {
                    jw__bt_msg(ui, "Bluetooth name is read-only");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }

                bool paired_device = false;
                const jw_bt_device_t *dev = jw__bt_row_device(ui, row, &paired_device);
                if (!dev) {
                    break;
                }

                if (paired_device && dev->connected) {
                    bool disconnected = (jw_bt_disconnect(dev->mac) == 0);
                    if (disconnected)
                        jw__bt_msg(ui, "Disconnected %s", dev->name);
                    else
                        jw__bt_msg(ui, "Disconnect failed");
                    jw__refresh_bluetooth_lists(ui);
                    if (disconnected)
                        jw__bt_route_back_if_orphaned(ui);
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }

                bool pair_if_needed = !paired_device;
                if (jw_bt_connect_start(dev->mac, pair_if_needed) != 0) {
                    jw__bt_msg(ui, "Could not start Bluetooth connect");
                } else {
                    ui->bt_op = pair_if_needed ? JW_BT_OP_PAIR_CONNECT : JW_BT_OP_CONNECT;
                    ui->bt_op_manual = true;
                    jw__bt_msg(ui, pair_if_needed ? "Pairing %s..."
                                                  : "Connecting %s...",
                               dev->name[0] ? dev->name : dev->mac);
                    cat_request_frame_in(250);
                }
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->bt_msg);
                break;
            }
            case CAT_BTN_Y: {
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw__bt_msg(ui, "Bluetooth is busy");
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, status_size, "%s", ui->bt_msg);
                    break;
                }
                bool paired_device = false;
                const jw_bt_device_t *dev =
                    jw__bt_row_device(ui, ui->bluetooth_list.cursor, &paired_device);
                if (!dev || !paired_device) {
                    break;
                }
                const char *name = dev->name[0] ? dev->name : dev->mac;
                if (!jw__confirm_bt_unpair(name)) {
                    jw__bt_msg(ui, "Unpair canceled");
                    break;
                }
                bool forgot = (jw_bt_forget(dev->mac) == 0);
                if (forgot)
                    jw__bt_msg(ui, "Unpaired %s", name);
                else
                    jw__bt_msg(ui, "Unpair failed");
                jw__refresh_bluetooth_lists(ui);
                if (forgot)
                    jw__bt_route_back_if_orphaned(ui);
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->bt_msg);
                break;
            }
            case CAT_BTN_X:
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw__bt_msg(ui, "Bluetooth is busy");
                    break;
                }
                (void)jw__bt_scan_start(ui, true);
                if (status_buf && status_size > 0)
                    snprintf(status_buf, status_size, "%s", ui->bt_msg);
                break;
            case CAT_BTN_B:
                if (ui->bt_op != JW_BT_OP_NONE) {
                    jw_bt_cancel_operation();
                    ui->bt_op = JW_BT_OP_NONE;
                    ui->bt_op_manual = false;
                }
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
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

    /* ── Accounts (placeholder) ──────────────────────────────────────── */
    case JW_SETTINGS_ACCOUNTS:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->accounts_list, -1, JW_ACCOUNTS_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->accounts_list, +1, JW_ACCOUNTS_ROW_COUNT);
                break;
            case CAT_BTN_A: {
                bool is_ss = ui->accounts_list.cursor == JW_ACCOUNTS_SCREENSCRAPER;
                if (is_ss) {
                    /* ScreenScraper: validate against the API through jawakad
                       (the daemon owns the dev-credential half of API auth).
                       A rejected login is surfaced in the Accounts row itself
                       (ss_rejected) because the hint-line status is hidden when
                       hints are off; an unreachable daemon/network saves the
                       credentials unverified for the first scrape to confirm. */
                    char entered[64];
                    snprintf(entered, sizeof(entered), "%s", ui->ss_username);
                    ui->ss_rejected = false;   /* fresh attempt */
                    char prompt[160];
                    cat_keyboard_result kb;
                    snprintf(prompt, sizeof(prompt),
                             "ScreenScraper username\nStart: Confirm\nY: Cancel");
                    if (cat_keyboard(entered, prompt, CAT_KB_GENERAL, &kb) != CAT_OK ||
                        !kb.text[0]) {
                        snprintf(status_buf, status_size, "Cancelled");
                        break;
                    }
                    snprintf(entered, sizeof(entered), "%.*s",
                             (int)sizeof(entered) - 1, kb.text);
                    cat_keyboard_result pw;
                    snprintf(prompt, sizeof(prompt),
                             "ScreenScraper password\nStart: Confirm\nY: Cancel");
                    if (cat_keyboard("", prompt, CAT_KB_GENERAL, &pw) != CAT_OK ||
                        !pw.text[0]) {
                        snprintf(status_buf, status_size, "Cancelled");
                        break;
                    }

                    jw_ipc_scrape_validate_info info;
                    int rc = ui->socket_path[0]
                        ? jw_ipc_scrape_validate(ui->socket_path, entered,
                                                 pw.text, &info)
                        : -1;
                    if (rc == 0 && info.valid) {
                        snprintf(ui->ss_username, sizeof(ui->ss_username),
                                 "%s", entered);
                        ui->ss_verified = true;
                        ui->ss_rejected = false;
                        ui->ss_max_threads = info.max_threads;
                        ui->ss_requests_today = info.requests_today;
                        ui->ss_max_requests = info.max_requests;
                        jw__persist(ui, "screenscraper_user", ui->ss_username);
                        jw__persist(ui, "screenscraper_pass", pw.text);
                        jw__persist(ui, "screenscraper_verified", "1");
                        jw__persist_int(ui, "screenscraper_maxthreads",
                                        info.max_threads);
                        jw__persist_int(ui, "screenscraper_requests_today",
                                        info.requests_today);
                        jw__persist_int(ui, "screenscraper_max_requests",
                                        info.max_requests);
                        if (info.max_requests > 0) {
                            snprintf(status_buf, status_size,
                                     "Signed in - %d thread%s, quota %d/%d today",
                                     info.max_threads,
                                     info.max_threads == 1 ? "" : "s",
                                     info.requests_today, info.max_requests);
                        } else {
                            snprintf(status_buf, status_size, "Signed in as %s",
                                     ui->ss_username);
                        }
                    } else if (rc == 0 && info.rejected) {
                        /* Wrong username/password — shown on the row (see above). */
                        ui->ss_rejected = true;
                        snprintf(status_buf, status_size,
                                 "Rejected - wrong username or password");
                    } else {
                        /* Daemon or network unavailable: keep them, unverified. */
                        snprintf(ui->ss_username, sizeof(ui->ss_username), "%s",
                                 entered);
                        ui->ss_verified = false;
                        ui->ss_rejected = false;
                        ui->ss_max_threads = 0;
                        ui->ss_requests_today = 0;
                        ui->ss_max_requests = 0;
                        jw__persist(ui, "screenscraper_user", ui->ss_username);
                        jw__persist(ui, "screenscraper_pass", pw.text);
                        jw__persist(ui, "screenscraper_verified", "0");
                        jw__persist(ui, "screenscraper_maxthreads", "");
                        jw__persist(ui, "screenscraper_requests_today", "");
                        jw__persist(ui, "screenscraper_max_requests", "");
                        snprintf(status_buf, status_size,
                                 "Saved - could not verify: %s",
                                 (rc == 0 && info.message[0]) ? info.message
                                                              : "daemon unavailable");
                    }
                    break;
                }

                /* RetroAchievements: stored for RetroArch, which validates at
                   game launch. */
                char prompt[160];
                cat_keyboard_result kb;
                snprintf(prompt, sizeof(prompt),
                         "RetroAchievements username\nStart: Confirm\nY: Cancel");
                if (cat_keyboard(ui->ra_username, prompt, CAT_KB_GENERAL, &kb) != CAT_OK ||
                    !kb.text[0]) {
                    snprintf(status_buf, status_size, "Cancelled");
                    break;
                }
                cat_keyboard_result pw;
                snprintf(prompt, sizeof(prompt),
                         "RetroAchievements password\nStart: Confirm\nY: Cancel");
                if (cat_keyboard("", prompt, CAT_KB_GENERAL, &pw) != CAT_OK ||
                    !pw.text[0]) {
                    snprintf(status_buf, status_size, "Cancelled");
                    break;
                }
                snprintf(ui->ra_username, sizeof(ui->ra_username), "%.*s",
                         (int)sizeof(ui->ra_username) - 1, kb.text);
                jw__persist(ui, "retroachievements_user", ui->ra_username);
                jw__persist(ui, "retroachievements_pass", pw.text);
                snprintf(status_buf, status_size,
                         "Saved - RetroArch signs in at game launch");
                break;
            }
            case CAT_BTN_Y:
                if (ui->accounts_list.cursor == JW_ACCOUNTS_SCREENSCRAPER &&
                    (ui->ss_username[0] || ui->ss_rejected)) {
                    ui->ss_username[0] = '\0';
                    ui->ss_verified = false;
                    ui->ss_rejected = false;
                    ui->ss_max_threads = 0;
                    ui->ss_requests_today = 0;
                    ui->ss_max_requests = 0;
                    jw__persist(ui, "screenscraper_user", "");
                    jw__persist(ui, "screenscraper_pass", "");
                    jw__persist(ui, "screenscraper_verified", "");
                    jw__persist(ui, "screenscraper_maxthreads", "");
                    jw__persist(ui, "screenscraper_requests_today", "");
                    jw__persist(ui, "screenscraper_max_requests", "");
                    snprintf(status_buf, status_size, "Signed out of ScreenScraper");
                } else if (ui->accounts_list.cursor == JW_ACCOUNTS_RETROACHIEVEMENTS &&
                           ui->ra_username[0]) {
                    ui->ra_username[0] = '\0';
                    jw__persist(ui, "retroachievements_user", "");
                    jw__persist(ui, "retroachievements_pass", "");
                    snprintf(status_buf, status_size, "Signed out of RetroAchievements");
                }
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── Scraping ────────────────────────────────────────────────────── */
    case JW_SETTINGS_SCRAPING:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->scraping_list, -1, JW_SCRAPING_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->scraping_list, +1, JW_SCRAPING_ROW_COUNT);
                break;
            case CAT_BTN_A:
                ui->scrape_edit_is_region =
                    ui->scraping_list.cursor == JW_SCRAPING_REGION;
                ui->scrape_edit_grabbed = false;
                ui->scrape_edit_list.cursor = 0;
                ui->scrape_edit_list.scroll_offset = 0;
                ui->screen = JW_SETTINGS_SCRAPE_PRIORITY;
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── Scrape priority editor (artwork or region) ──────────────────── */
    case JW_SETTINGS_SCRAPE_PRIORITY: {
        bool region = ui->scrape_edit_is_region;
        int *order = region ? ui->scrape_region_order : ui->scrape_artwork_order;
        int *included = region ? &ui->scrape_region_included
                               : &ui->scrape_artwork_included;
        int count = region ? jw_ss_regions_count : jw_ss_media_types_count;
        int cursor = ui->scrape_edit_list.cursor;

        switch (button) {
            case CAT_BTN_UP:
            case CAT_BTN_DOWN: {
                int dir = button == CAT_BTN_UP ? -1 : +1;
                if (ui->scrape_edit_grabbed) {
                    /* Reorder within the included zone. */
                    int target = cursor + dir;
                    if (target >= 0 && target < *included) {
                        jw__scrape_order_move(order, cursor, target);
                        cat_list_state_move(&ui->scrape_edit_list, dir, count);
                        jw__scrape_persist_order(ui, region);
                    }
                } else {
                    cat_list_state_move(&ui->scrape_edit_list, dir, count);
                }
                break;
            }
            case CAT_BTN_A: {
                if (ui->scrape_edit_grabbed) {
                    ui->scrape_edit_grabbed = false;
                    break;
                }
                if (cursor < *included) {
                    /* Exclude: sink to the top of the excluded zone. */
                    jw__scrape_order_move(order, cursor, *included - 1);
                    *included -= 1;
                    cat_list_state_jump(&ui->scrape_edit_list, *included, count);
                } else {
                    /* Include: append to the included zone. */
                    jw__scrape_order_move(order, cursor, *included);
                    *included += 1;
                    cat_list_state_jump(&ui->scrape_edit_list, *included - 1, count);
                }
                jw__scrape_persist_order(ui, region);
                break;
            }
            case CAT_BTN_X:
                if (ui->scrape_edit_grabbed) {
                    ui->scrape_edit_grabbed = false;
                } else if (cursor < *included) {
                    ui->scrape_edit_grabbed = true;
                } else {
                    snprintf(status_buf, status_size,
                             "Excluded entries cannot be reordered");
                }
                break;
            case CAT_BTN_B:
                if (ui->scrape_edit_grabbed) {
                    ui->scrape_edit_grabbed = false;
                } else {
                    ui->screen = JW_SETTINGS_SCRAPING;
                }
                break;
            default:
                break;
        }
        break;
    }

    /* ── System Update ───────────────────────────────────────────────── */
    case JW_SETTINGS_UPDATE:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->update_list, -1, JW_UPDATE_ROW_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->update_list, +1, JW_UPDATE_ROW_COUNT);
                break;
            case CAT_BTN_X:
                if (ui->update.download_active) {
                    jw__update_download_or_cancel(ui, status_buf, status_size);
                } else {
                    jw__open_update_picker(ui, status_buf, status_size);
                }
                break;
            case CAT_BTN_A:
                if (ui->update_list.cursor == JW_UPDATE_ROW_CHECK) {
                    jw__update_check_releases(ui, status_buf, status_size);
                } else if (ui->update_list.cursor == JW_UPDATE_ROW_DOWNLOAD) {
                    jw__update_download_or_cancel(ui, status_buf, status_size);
                } else if (ui->update_list.cursor == JW_UPDATE_ROW_INSTALL) {
                    jw__update_install_or_reboot(ui, status_buf, status_size);
                } else if (ui->update_list.cursor == JW_UPDATE_ROW_AVAILABLE) {
                    jw__open_update_picker(ui, status_buf, status_size);
                } else {
                    jw__refresh_update_status(ui, false);
                    jw__copy_status(status_buf, status_size,
                                    ui->update_msg[0] ? ui->update_msg : "Update status refreshed");
                }
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default:
                break;
        }
        break;

    /* ── System Update picker ─────────────────────────────────────────── */
    case JW_SETTINGS_UPDATE_PICKER: {
        int count = jw__update_option_count(ui);
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->update_picker_list, -1, count);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->update_picker_list, +1, count);
                break;
            case CAT_BTN_A:
                jw__select_update_picker_choice(ui, status_buf, status_size);
                break;
            case CAT_BTN_X:
                jw__update_check_releases(ui, status_buf, status_size);
                break;
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_UPDATE;
                break;
            default:
                break;
        }
        break;
    }

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
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_BOOT_SPLASH) {
                    (void)dir;
                    jw__set_boot_splash(ui, !ui->boot_splash_enabled,
                                        status_buf, status_size);
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_PERFORMANCE) {
                    if (!ui->performance_supported) {
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, (size_t)status_size, "%s",
                                     "performance unavailable");
                        }
                        break;
                    }
                    int next = (ui->game_perf_profile + dir + JW_GAME_PERF_PROFILE_COUNT)
                               % JW_GAME_PERF_PROFILE_COUNT;
                    jw_platform_perf_profile profile = kGamePerfProfiles[next];
                    char status[128] = "";
                    if (jw_ipc_set_performance_profile(
                            ui->socket_path, "global",
                            jw_platform_perf_profile_name(profile),
                            status, sizeof(status)) == 0) {
                        ui->game_perf_profile = next;
                        jw__persist(ui, "platform.performance.game_profile",
                                    jw_platform_perf_profile_name(profile));
                        if (status_buf && status_size > 0) {
                            snprintf(status_buf, (size_t)status_size, "%s", status);
                        }
                    } else if (status_buf && status_size > 0) {
                        snprintf(status_buf, (size_t)status_size, "%s",
                                 status[0] ? status : "performance failed");
                    }
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_TIMEZONE) {
                    /* Open the picker (A / Right); a long list isn't a cycler. */
                    if (button == CAT_BTN_A || button == CAT_BTN_RIGHT) {
                        int cur = jw__timezone_index_of(ui->timezone);
                        ui->timezone_picker_list.cursor = cur;
                        /* Scroll so the current zone is on screen when it opens. */
                        int off = cur - (JW_TIMEZONE_VISIBLE_ROWS - 1);
                        ui->timezone_picker_list.scroll_offset = off > 0 ? off : 0;
                        ui->screen = JW_SETTINGS_TIMEZONE_PICKER;
                    }
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_RESET_RETROARCH) {
                    if (button == CAT_BTN_A)
                        jw__reset_retroarch_config(ui, status_buf, status_size);
                } else if (ui->behavior_list.cursor == JW_BEHAVIOR_UNMOUNT_SECONDARY) {
                    if (button == CAT_BTN_A)
                        jw__safe_unmount_secondary_sd(ui, status_buf, status_size);
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_HOME;
                break;
            default: break;
        }
        break;

    /* ── Time Zone picker ─────────────────────────────────────────────── */
    case JW_SETTINGS_TIMEZONE_PICKER:
        switch (button) {
            case CAT_BTN_UP:
                cat_list_state_move(&ui->timezone_picker_list, -1, JW_TIMEZONE_COUNT);
                break;
            case CAT_BTN_DOWN:
                cat_list_state_move(&ui->timezone_picker_list, +1, JW_TIMEZONE_COUNT);
                break;
            case CAT_BTN_A: {
                int idx = ui->timezone_picker_list.cursor;
                if (idx >= 0 && idx < JW_TIMEZONE_COUNT) {
                    snprintf(ui->timezone, sizeof(ui->timezone), "%s",
                             kTimeZones[idx].tz);
                    jw__persist(ui, "timezone", ui->timezone);
                    jw__apply_timezone(ui->timezone);   /* clock updates immediately */
                    if (status_buf && status_size > 0)
                        snprintf(status_buf, (size_t)status_size, "Time zone: %s",
                                 kTimeZones[idx].label);
                }
                ui->screen = JW_SETTINGS_BEHAVIOR;
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_BEHAVIOR;
                break;
            default: break;
        }
        break;
    }

    return ui->open;
}
