#ifndef JW_SETTINGS_H
#define JW_SETTINGS_H

#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include <stdbool.h>
#include <stddef.h>

/* ─── Data tables ──────────────────────────────────────────────────────── */

#define JW_SETTINGS_THEME_COUNT 4
extern const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT];
extern const bool        kJawakaThemeEnabled[JW_SETTINGS_THEME_COUNT];

#define JW_SETTINGS_PILL_SHAPE_COUNT 3
extern const char *const kPillShapeLabels[JW_SETTINGS_PILL_SHAPE_COUNT];
extern const float       kPillShapeValues[JW_SETTINGS_PILL_SHAPE_COUNT];

#define JW_SETTINGS_FONT_SIZE_COUNT 4
extern const char *const kFontSizeLabels[JW_SETTINGS_FONT_SIZE_COUNT];
extern const int         kFontSizeValues[JW_SETTINGS_FONT_SIZE_COUNT];

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
    JW_SETTINGS_LIBRARY,
    JW_SETTINGS_BEHAVIOR,
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
#define JW_LAYOUT_PILL_SHAPE 0
#define JW_LAYOUT_FONT_SIZE  1
#define JW_LAYOUT_ROW_COUNT  2

/* Status Bar page */
#define JW_STATUSBAR_HINTS   0
#define JW_STATUSBAR_CLOCK   1
#define JW_STATUSBAR_BATTERY 2
#define JW_STATUSBAR_WIFI    3
#define JW_STATUSBAR_ROW_COUNT 4

/* Library page */
#define JW_LIBRARY_RESET_RETROARCH 0
#define JW_LIBRARY_ROW_COUNT 1

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
    cat_list_state     library_list;
    cat_list_state     placeholder_list;
    cat_scroll_state   about_scroll;
    int                theme_index;
    int                color_scheme_index;   /* -1 = custom (manually edited) */
    int                pill_shape_index;
    int                font_size_index;
    bool               show_hints;
    int                clock_style_index;
    bool               show_battery;
    bool               show_wifi;
    int                brightness_percent;
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
bool jw_settings_show_hints(const jw_settings_ui *ui);
void jw_settings_status_bar_opts(const jw_settings_ui *ui, cat_status_bar_opts *out);

/* Applies all persisted appearance overrides (the 7 color roles, list pill
 * shape, and font size) from the SQLite DB onto the current Catastrophe theme.
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
