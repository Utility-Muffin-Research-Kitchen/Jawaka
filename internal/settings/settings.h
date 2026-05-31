#ifndef JW_SETTINGS_H
#define JW_SETTINGS_H

#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include <stdbool.h>
#include <stddef.h>

/* ─── Data tables ──────────────────────────────────────────────────────── */

#define JW_SETTINGS_THEME_COUNT 4
extern const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT];

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

/* Colors page */
#define JW_COLOR_ACCENT      0
#define JW_COLOR_TEXT        1
#define JW_COLOR_HINT        2
#define JW_COLOR_HIGHLIGHT   3
#define JW_COLOR_BACKGROUND  4
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
    cat_list_state     placeholder_list;
    int                theme_index;
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

void jw_settings_ui_render(const jw_settings_ui *ui,
                            int x, int y, int w, int h);

bool jw_settings_ui_handle_button(jw_settings_ui *ui, cat_button button,
                                   char *status_buf, size_t status_buf_size,
                                   bool *theme_changed);

#endif
