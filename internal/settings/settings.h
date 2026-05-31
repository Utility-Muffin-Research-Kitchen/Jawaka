#ifndef JW_SETTINGS_H
#define JW_SETTINGS_H

#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include <stdbool.h>
#include <stddef.h>

#define JW_SETTINGS_THEME_COUNT 4
extern const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT];

/* Pill shape presets: maps to pill_radius_ratio values. */
#define JW_SETTINGS_PILL_SHAPE_COUNT 3
extern const char *const kPillShapeLabels[JW_SETTINGS_PILL_SHAPE_COUNT];
extern const float       kPillShapeValues[JW_SETTINGS_PILL_SHAPE_COUNT];

typedef enum {
    JW_SETTINGS_HOME = 0,
    JW_SETTINGS_APPEARANCE,
    JW_SETTINGS_DISPLAY,
    JW_SETTINGS_LIBRARY,
    JW_SETTINGS_BEHAVIOR,
    JW_SETTINGS_ABOUT,
} jw_settings_screen;

/* Appearance page rows. */
#define JW_APPEARANCE_THEME       0
#define JW_APPEARANCE_ACCENT      1
#define JW_APPEARANCE_PILL_SHAPE  2
#define JW_APPEARANCE_FONT_SIZE   3
#define JW_APPEARANCE_SHOW_HINTS  4
/* Clock style presets. */
#define JW_SETTINGS_CLOCK_STYLE_COUNT 4
extern const char *const kClockStyleLabels[JW_SETTINGS_CLOCK_STYLE_COUNT];

#define JW_APPEARANCE_SHOW_CLOCK  5
#define JW_APPEARANCE_SHOW_BATTERY 6
#define JW_APPEARANCE_SHOW_WIFI   7
#define JW_APPEARANCE_TEXT_COLOR  8
#define JW_APPEARANCE_BG_COLOR   9
#define JW_APPEARANCE_ROW_COUNT  10

/* Font size presets: maps to font_bump values (0 = auto, additive). */
#define JW_SETTINGS_FONT_SIZE_COUNT 4
extern const char *const kFontSizeLabels[JW_SETTINGS_FONT_SIZE_COUNT];
extern const int         kFontSizeValues[JW_SETTINGS_FONT_SIZE_COUNT];

typedef struct {
    bool               open;
    jw_settings_screen screen;
    cat_list_state     home_list;
    cat_list_state     appearance_list;
    cat_list_state     display_list;
    cat_list_state     placeholder_list;
    int                theme_index;
    int                pill_shape_index;
    int                font_size_index;
    bool               show_hints;
    int                clock_style_index;  /* 0=Off, 1=24h, 2=12h, 3=12h no AM/PM */
    bool               show_battery;
    bool               show_wifi;
    int                brightness_percent;
    char               db_path[1024];
    char               socket_path[1024];
} jw_settings_ui;

/* Lifecycle */
void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path,
                         const char *initial_theme_name,
                         const char *socket_path);
void jw_settings_ui_enter(jw_settings_ui *ui);
void jw_settings_ui_close(jw_settings_ui *ui);
bool jw_settings_ui_is_open(const jw_settings_ui *ui);
bool jw_settings_show_hints(const jw_settings_ui *ui);
void jw_settings_status_bar_opts(const jw_settings_ui *ui, cat_status_bar_opts *out);

/* Render. Draws into the rect (x, y, w, h). */
void jw_settings_ui_render(const jw_settings_ui *ui,
                            int x, int y, int w, int h);

/* Handle a button press. Returns true if the settings UI is still open
 * after handling (false if B closed it from home).
 *
 * If a theme change was successfully applied, sets *theme_changed=true
 * and writes the resolved status into status_buf (e.g. "theme: Jawaka-Vertical").
 * If a theme change failed, theme_changed stays false but status_buf is
 * set to "theme load failed".
 *
 * status_buf may be NULL if the caller doesn't care.
 */
bool jw_settings_ui_handle_button(jw_settings_ui *ui, cat_button button,
                                   char *status_buf, size_t status_buf_size,
                                   bool *theme_changed);

#endif
