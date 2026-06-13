#ifndef JW_SETTINGS_H
#define JW_SETTINGS_H

#include "catastrophe.h"
#include "catastrophe_widgets.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/bluetooth.h"
#include "internal/platform/device.h"
#include "internal/platform/wifi.h"

#include <stdbool.h>
#include <stddef.h>

/* ─── Data tables ──────────────────────────────────────────────────────── */

#define JW_SETTINGS_THEME_COUNT 4
extern const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT];
extern const bool        kJawakaThemeEnabled[JW_SETTINGS_THEME_COUNT];

/* Display labels live here; the matching value tables (radius, corner mask,
   font bump) are the canonical ones in appearance.h, shared with the daemon. */
#define JW_SETTINGS_PILL_SHAPE_COUNT 4
#define JW_SETTINGS_PILL_SHAPE_DEFAULT 3   /* "Leaf" — the default list style */
extern const char *const kPillShapeLabels[JW_SETTINGS_PILL_SHAPE_COUNT];

#define JW_SETTINGS_FONT_SIZE_COUNT 4
extern const char *const kFontSizeLabels[JW_SETTINGS_FONT_SIZE_COUNT];

#define JW_SETTINGS_CLOCK_STYLE_COUNT 4
extern const char *const kClockStyleLabels[JW_SETTINGS_CLOCK_STYLE_COUNT];

/* ─── Screen states ────────────────────────────────────────────────────── */

typedef enum {
    JW_SETTINGS_HOME = 0,
    JW_SETTINGS_APPEARANCE,
    JW_SETTINGS_COLORS,
    JW_SETTINGS_LAYOUT,
    JW_SETTINGS_STATUS_BAR,
    JW_SETTINGS_DISPLAY,
    JW_SETTINGS_NETWORK,
    JW_SETTINGS_BLUETOOTH,
    JW_SETTINGS_LIGHTING,
    JW_SETTINGS_LIBRARY,
    JW_SETTINGS_ACCOUNTS,
    JW_SETTINGS_SCRAPING,
    JW_SETTINGS_SCRAPE_PRIORITY,   /* artwork or region editor, see scrape_edit_is_region */
    JW_SETTINGS_BEHAVIOR,
    JW_SETTINGS_UPDATE,
    JW_SETTINGS_UPDATE_PICKER,
    JW_SETTINGS_TIMEZONE_PICKER,
    JW_SETTINGS_ABOUT,
} jw_settings_screen;

/* ─── Row indices per sub-page ─────────────────────────────────────────── */

/* Appearance sub-menu */
#define JW_APPEAR_THEME    0
#define JW_APPEAR_COLORS   1
#define JW_APPEAR_LAYOUT   2
#define JW_APPEAR_STATUSBAR 3
#define JW_APPEAR_ROW_COUNT 4

/* Colors page — ordered by visual impact (most visible first). */
#define JW_COLOR_ACCENT      0
#define JW_COLOR_BACKGROUND  1
#define JW_COLOR_TEXT        2
#define JW_COLOR_HIGHLIGHT   3
#define JW_COLOR_HINT        4
#define JW_COLOR_BTN_TEXT    5
#define JW_COLOR_BTN_BG     6
#define JW_COLOR_ROW_COUNT   7

/* Layout page */
#define JW_LAYOUT_PILL_SHAPE  0
#define JW_LAYOUT_FONT_FAMILY 1
#define JW_LAYOUT_FONT_SIZE   2
#define JW_LAYOUT_TAB_SWITCH  3
#define JW_LAYOUT_ROW_COUNT   4

/* Status Bar page */
#define JW_STATUSBAR_HINTS   0
#define JW_STATUSBAR_CLOCK   1
#define JW_STATUSBAR_BATTERY 2   /* 4-way cycler: Off / Icon / Percent / Both */
#define JW_STATUSBAR_WIFI    3
#define JW_STATUSBAR_BLUETOOTH 4
#define JW_STATUSBAR_VOLUME  5
#define JW_STATUSBAR_ROW_COUNT 6

/* Display & Sound page */
#define JW_DISPLAY_BRIGHTNESS 0
#define JW_DISPLAY_OUTPUT     1
#define JW_DISPLAY_VOLUME     2
#define JW_DISPLAY_ROW_COUNT  3

/* Bluetooth page */
#define JW_BLUETOOTH_ROW_POWER 0
#define JW_BLUETOOTH_ROW_NAME  1
#define JW_BLUETOOTH_FIXED_ROWS 2
#define JW_BLUETOOTH_LIST_ROWS 8

/* Lighting page (LED ring) */
#define JW_LIGHTING_ENABLE     0
#define JW_LIGHTING_MODE       1
#define JW_LIGHTING_COLOR      2
#define JW_LIGHTING_BRIGHTNESS 3
#define JW_LIGHTING_SPEED      4
#define JW_LIGHTING_ROW_COUNT  5

/* Library page */
#define JW_LIBRARY_RESET_RETROARCH 0
#define JW_LIBRARY_UNMOUNT_SECONDARY 1
#define JW_LIBRARY_ROW_COUNT 2

/* Accounts page */
#define JW_ACCOUNTS_SCREENSCRAPER     0
#define JW_ACCOUNTS_RETROACHIEVEMENTS 1
#define JW_ACCOUNTS_ROW_COUNT         2

/* Scraping page */
#define JW_SCRAPING_ARTWORK   0
#define JW_SCRAPING_REGION    1
#define JW_SCRAPING_ROW_COUNT 2
/* Capacity for the priority editors (catalogs are 10 entries each today). */
#define JW_SCRAPE_PRIO_SLOTS  16

/* Behavior page */
#define JW_BEHAVIOR_STARTUP_TAB 0
#define JW_BEHAVIOR_AUTO_SLEEP  1
#define JW_BEHAVIOR_BOOT_SPLASH 2
#define JW_BEHAVIOR_PERFORMANCE 3
#define JW_BEHAVIOR_TIMEZONE    4   /* opens the Time Zone picker screen */
#define JW_BEHAVIOR_ROW_COUNT   5

/* System Update page */
#define JW_UPDATE_ROW_CHECK     0
#define JW_UPDATE_ROW_DOWNLOAD  1
#define JW_UPDATE_ROW_INSTALL   2
#define JW_UPDATE_ROW_CURRENT   3
#define JW_UPDATE_ROW_AVAILABLE 4
#define JW_UPDATE_ROW_COUNT     5

/* ─── State ────────────────────────────────────────────────────────────── */

typedef struct {
    bool               open;
    jw_settings_screen screen;
    cat_list_state     home_list;
    cat_list_state     appearance_list;
    cat_list_state     colors_list;
    cat_list_state     layout_list;
    cat_list_state     statusbar_list;
    cat_list_state     display_list;
    cat_list_state     network_list;
    cat_list_state     bluetooth_list;
    cat_list_state     lighting_list;
    cat_list_state     library_list;
    cat_list_state     accounts_list;
    cat_list_state     scraping_list;
    cat_list_state     scrape_edit_list;
    cat_list_state     behavior_list;
    cat_list_state     update_list;
    cat_list_state     update_picker_list;
    cat_list_state     timezone_picker_list;
    cat_list_state     placeholder_list;
    cat_scroll_state   about_scroll;
    int                theme_index;
    int                color_scheme_index;   /* -1 = custom (manually edited) */
    int                pill_shape_index;
    int                font_family_index;
    int                font_size_index;
    int                tab_glide;            /* 0 = Snap (instant), 1 = Glide (slide) */
    bool               show_hints;
    int                clock_style_index;
    bool               show_battery;
    bool               show_battery_level;  /* numeric % next to the battery icon */
    bool               show_wifi;
    bool               show_bluetooth;      /* bluetooth icon in the status bar */
    bool               show_volume;         /* speaker icon in the status bar */
    char               timezone[64];        /* IANA zone id exported as TZ; "" = system default */
    char               ss_username[64];     /* ScreenScraper account ("" = signed out); password
                                               lives only in the settings DB for the scrape worker */
    bool               ss_verified;         /* credentials confirmed against the API at sign-in */
    int                ss_max_threads;      /* account thread allowance from validation; 0 unknown */
    int                ss_requests_today;   /* quota snapshot from validation */
    int                ss_max_requests;     /* quota snapshot from validation; 0 unknown */
    /* Scrape priority editors: permutations of the scrape catalogs as catalog
       indices; the first *_included entries are active, the rest excluded.
       Persisted as CSV of included values in scrape.artwork_priority /
       scrape.region_priority (read by the daemon's scrape worker). */
    int                scrape_artwork_order[JW_SCRAPE_PRIO_SLOTS];
    int                scrape_artwork_included;
    int                scrape_region_order[JW_SCRAPE_PRIO_SLOTS];
    int                scrape_region_included;
    bool               scrape_edit_is_region;  /* which list the editor edits */
    bool               scrape_edit_grabbed;    /* X grabbed the cursor row */
    char               ra_username[64];     /* RetroAchievements account ("" = signed out); exported
                                               to RetroArch's session config, which validates it */
    int                startup_tab_index;   /* jw_tab the launcher opens on */
    int                auto_sleep_index;    /* idle-sleep timeout (index into kAutoSleep*) */
    bool               boot_splash_enabled; /* Leaf boot transition/artwork on next boot */
    bool               boot_splash_supported;
    int                game_perf_profile;   /* Settings > Behavior game profile */
    bool               performance_supported;
    int                brightness_percent;
    int                volume_percent;
    jw_platform_audio_output audio_output;
    unsigned           audio_available_outputs;
    int                audio_volumes[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
    bool               led_enabled;
    int                led_mode;            /* jw_led_mode */
    ap_color           led_color;
    int                led_brightness;      /* 0..JW_LED_BRIGHTNESS_MAX */
    int                led_speed;           /* 0..JW_LED_SPEED_MAX */
    char               secondary_sd_status[32];
    jw_wifi_status_t   wifi;                /* last-read Wi-Fi status (Network page) */
    unsigned           wifi_next_poll_ms;   /* throttle for the live Network poll */
    jw_wifi_network_t  wifi_networks[JW_WIFI_MAX_NETWORKS];  /* latest scan results */
    int                wifi_network_count;
    unsigned           wifi_next_scan_ms;   /* throttle for triggering a new scan */
    char               wifi_msg[128];       /* last Network-page action feedback (toast) */
    unsigned           wifi_msg_ms;         /* when wifi_msg was set (0 = none); auto-expires */
    char               wifi_attempt_ssid[64];  /* network we're trying to join ("" = none) */
    unsigned           wifi_attempt_ms;     /* when the join attempt started */
    int                wifi_monitor_fd;     /* wpa event-socket fd during a join (-1 = none) */
    bool               wifi_radio_on;       /* Wi-Fi on/off toggle state */
    int                wifi_strength_cached;/* 0..3 for the status-bar icon; polled on a throttle */
    int                bt_state_cached;     /* 0=off,1=on,2=connected for the status-bar icon; throttled */
    bool               adb_supported;       /* platform advertises ADB control */
    int                adb_enabled;         /* -1 unavailable, 0 disabled, 1 pinned */
    int                adb_intent_enabled;  /* -1 unavailable, 0 no boot restore, 1 restore at boot */
    jw_ipc_update_status_info update;
    bool               update_have_status;
    unsigned           update_next_poll_ms;
    char               update_msg[192];
    unsigned           update_msg_ms;
    jw_bt_status_t     bt_status;           /* last-read Bluetooth status */
    bool               bt_radio_on;
    jw_bt_device_t     bt_paired[JW_BT_MAX_DEVICES];
    int                bt_paired_count;
    jw_bt_device_t     bt_nearby[JW_BT_MAX_DEVICES];
    int                bt_nearby_count;
    unsigned           bt_next_poll_ms;
    unsigned           bt_next_scan_ms;
    char               bt_msg[128];
    unsigned           bt_msg_ms;
    jw_bt_operation_kind bt_op;
    bool               bt_op_manual;
    char               db_path[1024];
    char               socket_path[1024];
} jw_settings_ui;

/* ─── Public API ───────────────────────────────────────────────────────── */

void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path,
                         const char *initial_theme_name,
                         const char *socket_path);
void jw_settings_ui_enter(jw_settings_ui *ui);
void jw_settings_ui_close(jw_settings_ui *ui);
bool jw_settings_ui_is_open(const jw_settings_ui *ui);
/* Current sub-page, so the launcher can pick page-specific footer hints. */
jw_settings_screen jw_settings_ui_screen(const jw_settings_ui *ui);
bool jw_settings_show_hints(const jw_settings_ui *ui);
/* True when tab switches should slide (Glide); false for an instant cut (Snap). */
bool jw_settings_tab_glide(const jw_settings_ui *ui);

/* True while the Display & Sound page is showing. The launcher polls this and
   calls jw_settings_ui_refresh_av() so the sliders track hardware volume/
   brightness keys (which jawakad's input proxy consumes — the UI never sees
   those events). */
bool jw_settings_ui_wants_av_poll(const jw_settings_ui *ui);
void jw_settings_ui_refresh_av(jw_settings_ui *ui);

/* True while the Network page is open. The launcher calls
 * jw_settings_ui_refresh_wifi() each frame so the status follows live changes;
 * the refresh self-throttles (re-polls at most every ~2s) so it never forks
 * platform Wi-Fi every frame. */
bool jw_settings_ui_wants_wifi_poll(const jw_settings_ui *ui);
void jw_settings_ui_refresh_wifi(jw_settings_ui *ui);

/* True while the Bluetooth page is open. The launcher calls the refresh every
 * frame; it self-throttles ordinary status/list polling and only polls async
 * scan/connect workers every frame. */
bool jw_settings_ui_wants_bluetooth_poll(const jw_settings_ui *ui);
void jw_settings_ui_refresh_bluetooth(jw_settings_ui *ui);
bool jw_settings_ui_wants_update_poll(const jw_settings_ui *ui);
void jw_settings_ui_refresh_update(jw_settings_ui *ui);

/* True if the status-bar wifi icon is enabled. The launcher uses this to decide
 * whether to keep the wifi strength polled on the home screen. */
bool jw_settings_show_wifi(const jw_settings_ui *ui);
/* Poll live wifi strength into ui->wifi_strength_cached so the status-bar icon
 * stays current while idle. Call on a throttle. */
void jw_settings_ui_refresh_wifi_strength(jw_settings_ui *ui);

/* Poll live Bluetooth state into ui->bt_state_cached (0=off,1=on,2=connected)
   so the status-bar icon reflects the radio without shelling out every frame. */
void jw_settings_ui_refresh_bt_state(jw_settings_ui *ui);
/* The raw poll behind refresh_bt_state: live 0=off/1=on/2=connected without
   touching any jw_settings_ui state, so the launcher's background status
   poller can call it off the render thread. Blocks (shells out). */
int  jw_settings_bt_state_now(void);

/* True if the status-bar speaker icon is enabled. The launcher uses this to
 * decide whether to keep volume polled on the home screen. */
bool jw_settings_show_volume(const jw_settings_ui *ui);
/* Poll the current volume via IPC into ui->volume_percent (a lighter subset of
 * refresh_av) so the status-bar speaker icon stays current while idle. */
void jw_settings_ui_refresh_volume(jw_settings_ui *ui);

/* Flip the LED on/off and apply immediately (e.g. a stick-press shortcut from
   the launcher). No-op if the platform has no LED. */
void jw_settings_toggle_led(jw_settings_ui *ui);
void jw_settings_status_bar_opts(const jw_settings_ui *ui, cat_status_bar_opts *out);

/* Applies all persisted appearance overrides (the 7 color roles, list pill
 * shape, font family, and font size) from the SQLite DB onto the current Catastrophe theme.
 * Shared by the launcher's settings UI and jawaka-menu so both render with the
 * user's chosen colors and layout. db_path may be NULL/empty (no-op). */
void jw_settings_apply_persisted_overrides(const char *db_path);

/* Reads the persisted status-bar and button-hint preferences straight from the
 * DB, for processes that don't own a jw_settings_ui (e.g. jawaka-menu). Either
 * out param may be NULL. Missing keys fall back to the same defaults the
 * settings UI uses. db_path may be NULL/empty (defaults only). */
void jw_settings_load_status_prefs(const char *db_path,
                                   cat_status_bar_opts *out_opts,
                                   bool *out_show_hints);

void jw_settings_ui_render(const jw_settings_ui *ui,
                            int x, int y, int w, int h);

bool jw_settings_ui_handle_button(jw_settings_ui *ui, cat_button button,
                                   char *status_buf, size_t status_buf_size,
                                   bool *theme_changed);

#endif
