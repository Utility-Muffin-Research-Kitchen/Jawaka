#ifndef JW_SETTINGS_H
#define JW_SETTINGS_H

#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include <stdbool.h>
#include <stddef.h>

#define JW_SETTINGS_THEME_COUNT 4
extern const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT];

typedef enum {
    JW_SETTINGS_HOME = 0,
    JW_SETTINGS_APPEARANCE,
    JW_SETTINGS_LIBRARY,
    JW_SETTINGS_BEHAVIOR,
    JW_SETTINGS_ABOUT,
} jw_settings_screen;

typedef struct {
    bool               open;
    jw_settings_screen screen;
    cat_list_state     home_list;
    cat_list_state     appearance_list;
    cat_list_state     placeholder_list;
    int                theme_index;
    char               db_path[1024];
} jw_settings_ui;

/* Lifecycle */
void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path,
                         const char *initial_theme_name);
void jw_settings_ui_enter(jw_settings_ui *ui);
void jw_settings_ui_close(jw_settings_ui *ui);
bool jw_settings_ui_is_open(const jw_settings_ui *ui);

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
