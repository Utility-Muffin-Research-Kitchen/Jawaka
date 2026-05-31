#include "internal/settings/settings.h"

#include "internal/db/db.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT] = {
    "Jawaka-Tabs",
    "Jawaka-Vertical",
    "Jawaka-Horizontal",
    "Jawaka-Coverflow",
};

const char *const kPillShapeLabels[JW_SETTINGS_PILL_SHAPE_COUNT] = {
    "Rounded",
    "Squircle",
    "Square",
};

const float kPillShapeValues[JW_SETTINGS_PILL_SHAPE_COUNT] = {
    1.0f,
    0.25f,
    0.0f,
};

const char *const kFontSizeLabels[JW_SETTINGS_FONT_SIZE_COUNT] = {
    "Small",
    "Default",
    "Large",
    "Extra Large",
};

const int kFontSizeValues[JW_SETTINGS_FONT_SIZE_COUNT] = {
    0,
    2,
    4,
    5,
};

const char *const kClockStyleLabels[JW_SETTINGS_CLOCK_STYLE_COUNT] = {
    "Off",
    "24 Hour",
    "12 Hour",
    "12 Hour (no AM/PM)",
};

static const char *kCategoryLabels[] = {
    "Appearance",
    "Display",
    "Library",
    "Behavior",
    "About",
};
#define JW_SETTINGS_CATEGORY_COUNT 5

#define JW_SETTINGS_DISPLAY_COUNT 1

/* ─── Lifecycle ──────────────────────────────────────────────────────────── */

static int jw__find_theme_index(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < JW_SETTINGS_THEME_COUNT; i++) {
        if (strcmp(kJawakaThemes[i], name) == 0) return i;
    }
    return 0;
}

static void jw__refresh_brightness(jw_settings_ui *ui) {
    if (!ui || !ui->socket_path[0]) {
        return;
    }
    int percent = -1;
    if (jw_ipc_platform_brightness(ui->socket_path, &percent) == 0 && percent >= 0) {
        ui->brightness_percent = jw_platform_clamp_brightness_percent(percent);
    }
}

void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path,
                          const char *initial_theme_name,
                          const char *socket_path) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->open    = false;
    ui->screen  = JW_SETTINGS_HOME;
    cat_list_state_init(&ui->home_list,        JW_SETTINGS_CATEGORY_COUNT);
    cat_list_state_init(&ui->appearance_list,  JW_APPEARANCE_ROW_COUNT);
    cat_list_state_init(&ui->display_list,     JW_SETTINGS_DISPLAY_COUNT);
    cat_list_state_init(&ui->placeholder_list, 1);
    ui->theme_index = jw__find_theme_index(initial_theme_name);
    ui->pill_shape_index = 0;
    ui->font_size_index = 1;  /* Default */
    ui->show_hints = true;
    ui->clock_style_index = 1;  /* 24 Hour default */
    ui->show_battery = true;
    ui->show_wifi = true;
    ui->brightness_percent = 50;

    /* Restore persisted appearance overrides. */
    if (db_path && db_path[0]) {
        char val[32];
        if (jw_db_get_setting(db_path, "pill_shape_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT) {
                ui->pill_shape_index = idx;
                cat_get_theme()->pill_radius_ratio = kPillShapeValues[idx];
            }
        }
        if (jw_db_get_setting(db_path, "accent_color", val, sizeof(val)) == 0 && val[0]) {
            cat_set_theme_color(val);
        }
        if (jw_db_get_setting(db_path, "font_size_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT) {
                ui->font_size_index = idx;
                cat_set_font_bump(kFontSizeValues[idx]);
            }
        }
        if (jw_db_get_setting(db_path, "show_hints", val, sizeof(val)) == 0) {
            ui->show_hints = (strcmp(val, "0") != 0);
        }
        if (jw_db_get_setting(db_path, "clock_style_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_CLOCK_STYLE_COUNT)
                ui->clock_style_index = idx;
        }
        if (jw_db_get_setting(db_path, "show_battery", val, sizeof(val)) == 0) {
            ui->show_battery = (strcmp(val, "0") != 0);
        }
        if (jw_db_get_setting(db_path, "show_wifi", val, sizeof(val)) == 0) {
            ui->show_wifi = (strcmp(val, "0") != 0);
        }
        if (jw_db_get_setting(db_path, "text_color", val, sizeof(val)) == 0 && val[0]) {
            ap_color c = cat_hex_to_color(val);
            ap_theme *t = cat_get_theme();
            t->text = c;
            t->highlighted_text = c;
        }
        if (jw_db_get_setting(db_path, "bg_color", val, sizeof(val)) == 0 && val[0]) {
            cat_get_theme()->background = cat_hex_to_color(val);
        }
    }
    if (db_path && db_path[0])
        snprintf(ui->db_path, sizeof(ui->db_path), "%s", db_path);
    if (socket_path && socket_path[0])
        snprintf(ui->socket_path, sizeof(ui->socket_path), "%s", socket_path);
    jw__refresh_brightness(ui);
}

void jw_settings_ui_enter(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = true;
    /* Always re-enter at home; home cursor is preserved across enter/close. */
    ui->screen = JW_SETTINGS_HOME;
    jw__refresh_brightness(ui);
}

void jw_settings_ui_close(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = false;
    ui->screen = JW_SETTINGS_HOME;
}

bool jw_settings_ui_is_open(const jw_settings_ui *ui) {
    return ui && ui->open;
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
    /* Clock style: 0=Off, 1=24h, 2=12h, 3=12h no AM/PM */
    if (ui->clock_style_index == 0) {
        out->show_clock = CAT_CLOCK_HIDE;
    } else {
        out->show_clock = CAT_CLOCK_SHOW;
        out->use_24h = (ui->clock_style_index == 1);
        out->no_ampm = (ui->clock_style_index == 3);
    }
    out->show_battery = ui->show_battery;
    out->show_wifi = ui->show_wifi;
}

/* ─── Render ─────────────────────────────────────────────────────────────── */

static void jw__draw_header(const char *title, int x, int y, int w) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);
    int large_h = TTF_FontHeight(large);
    cat_draw_text_ellipsized(large, title, x + cat_scale(4), y, theme->text, w - cat_scale(8));
    cat_draw_rect(x, y + large_h + cat_scale(4),
                  w, 1, cat_hex_to_color("#ffffff20"));
}

static void jw__render_home(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);

    jw__draw_header("Settings", x, y, w);
    int header_h = TTF_FontHeight(large) + cat_scale(10);

    int list_y = y + header_h;
    int list_h = h - header_h;
    int item_h = TTF_FontHeight(body) + cat_scale(12);

    for (int i = 0; i < JW_SETTINGS_CATEGORY_COUNT; i++) {
        int iy = list_y + i * item_h;
        if (iy + item_h > list_y + list_h) break;
        bool selected = (ui->home_list.cursor == i);
        int pill_h = TTF_FontHeight(body) + cat_scale(6);
        int pill_y = iy + (item_h - pill_h) / 2;
        if (selected)
            cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);
        ap_color tc = selected ? theme->highlighted_text : theme->text;
        int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;
        cat_draw_text_ellipsized(body, kCategoryLabels[i],
                                  x + cat_scale(12), ty, tc, w - cat_scale(24));
    }
}

static void jw__render_appearance_row(const jw_settings_ui *ui, int x, int y,
                                      int w, int row, const char *label,
                                      const char *value) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int item_h = TTF_FontHeight(body) + cat_scale(12);
    int iy = y + row * item_h;
    bool selected = (ui->appearance_list.cursor == row);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (item_h - pill_h) / 2;
    if (selected)
        cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    cat_draw_text_ellipsized(body, label, x + cat_scale(12), ty, label_c,
                              w / 2 - cat_scale(20));

    char value_str[96];
    snprintf(value_str, sizeof(value_str), "\xe2\x80\xb9 %s \xe2\x80\xba", value);
    int vw = cat_measure_text(body, value_str);
    int vx = x + w - vw - cat_scale(16);
    if (vx < x + w / 2) vx = x + w / 2;
    cat_draw_text(body, value_str, vx, ty, value_c);
}

static void jw__render_appearance(const jw_settings_ui *ui, int x, int y, int w, int h) {
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);
    jw__draw_header("Appearance", x, y, w);
    int header_h = TTF_FontHeight(large) + cat_scale(10);
    int list_y = y + header_h;

    /* Row 0: Theme */
    jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_THEME,
                              "Theme", kJawakaThemes[ui->theme_index]);

    /* Row 1: Accent Color */
    {
        ap_theme *theme = cat_get_theme();
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                 theme->accent.r, theme->accent.g, theme->accent.b);
        jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_ACCENT,
                                  "Accent Color", hex);

        /* Draw a color swatch next to the value */
        TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
        int item_h = TTF_FontHeight(body) + cat_scale(12);
        int swatch_sz = cat_scale(14);
        int swatch_x = x + w - cat_scale(20) - swatch_sz;
        int swatch_y = list_y + JW_APPEARANCE_ACCENT * item_h +
                       (item_h - swatch_sz) / 2;
        cat_draw_rect(swatch_x, swatch_y, swatch_sz, swatch_sz, theme->accent);
    }

    /* Row 2: List Style (pill shape) */
    jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_PILL_SHAPE,
                              "List Style", kPillShapeLabels[ui->pill_shape_index]);

    /* Row 3: Font Size */
    jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_FONT_SIZE,
                              "Font Size", kFontSizeLabels[ui->font_size_index]);

    /* Row 4: Show Hints */
    jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_SHOW_HINTS,
                              "Button Hints", ui->show_hints ? "On" : "Off");

    /* Row 5: Clock Style */
    jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_SHOW_CLOCK,
                              "Clock", kClockStyleLabels[ui->clock_style_index]);

    /* Row 6: Show Battery */
    jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_SHOW_BATTERY,
                              "Show Battery", ui->show_battery ? "On" : "Off");

    /* Row 7: Show Wifi */
    jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_SHOW_WIFI,
                              "Show Wifi", ui->show_wifi ? "On" : "Off");

    /* Row 8: Text Color */
    {
        ap_theme *t = cat_get_theme();
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X", t->text.r, t->text.g, t->text.b);
        jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_TEXT_COLOR,
                                  "Text Color", hex);
        TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
        int item_h = TTF_FontHeight(body) + cat_scale(12);
        int swatch_sz = cat_scale(14);
        int swatch_x = x + w - cat_scale(20) - swatch_sz;
        int swatch_y = list_y + JW_APPEARANCE_TEXT_COLOR * item_h +
                       (item_h - swatch_sz) / 2;
        cat_draw_rect(swatch_x, swatch_y, swatch_sz, swatch_sz, t->text);
    }

    /* Row 9: Background Color */
    {
        ap_theme *t = cat_get_theme();
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                 t->background.r, t->background.g, t->background.b);
        jw__render_appearance_row(ui, x, list_y, w, JW_APPEARANCE_BG_COLOR,
                                  "Background", hex);
        TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
        int item_h = TTF_FontHeight(body) + cat_scale(12);
        int swatch_sz = cat_scale(14);
        int swatch_x = x + w - cat_scale(20) - swatch_sz;
        int swatch_y = list_y + JW_APPEARANCE_BG_COLOR * item_h +
                       (item_h - swatch_sz) / 2;
        cat_draw_rect(swatch_x, swatch_y, swatch_sz, swatch_sz, t->background);
    }

    (void)h;
}

static void jw__render_display(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);

    jw__draw_header("Display", x, y, w);
    int header_h = TTF_FontHeight(large) + cat_scale(10);
    int item_h = TTF_FontHeight(body) + cat_scale(28);
    int iy = y + header_h;
    bool selected = (ui->display_list.cursor == 0);
    int pill_h = item_h - cat_scale(6);
    int pill_y = iy + cat_scale(3);
    if (selected)
        cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + cat_scale(8);

    cat_draw_text_ellipsized(body, "Brightness", x + cat_scale(12), ty, label_c,
                              w / 2 - cat_scale(20));

    char value_str[32];
    snprintf(value_str, sizeof(value_str), "%d%%", ui->brightness_percent);
    int vw = cat_measure_text(body, value_str);
    cat_draw_text(body, value_str, x + w - vw - cat_scale(16), ty, value_c);

    int track_x = x + cat_scale(12);
    int track_y = pill_y + pill_h - cat_scale(16);
    int track_w = w - cat_scale(32);
    int fill_w = (track_w * ui->brightness_percent) / 100;
    cat_draw_rect(track_x, track_y, track_w, cat_scale(4), cat_hex_to_color("#ffffff33"));
    cat_draw_rect(track_x, track_y, fill_w, cat_scale(4), value_c);
    (void)h;
}

static void jw__render_placeholder(const jw_settings_ui *ui, const char *title,
                                    int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);

    jw__draw_header(title, x, y, w);
    int header_h = TTF_FontHeight(large) + cat_scale(10);

    const char *msg = "Coming soon";
    int body_h = TTF_FontHeight(body);
    int mw = cat_measure_text(body, msg);
    cat_draw_text(body, msg,
                  x + (w - mw) / 2,
                  y + header_h + (h - header_h - body_h) / 2,
                  theme->hint);
    (void)ui;
}

void jw_settings_ui_render(const jw_settings_ui *ui,
                            int x, int y, int w, int h) {
    if (!ui) return;
    switch (ui->screen) {
        case JW_SETTINGS_HOME:
            jw__render_home(ui, x, y, w, h);
            break;
        case JW_SETTINGS_APPEARANCE:
            jw__render_appearance(ui, x, y, w, h);
            break;
        case JW_SETTINGS_DISPLAY:
            jw__render_display(ui, x, y, w, h);
            break;
        case JW_SETTINGS_LIBRARY:
            jw__render_placeholder(ui, "Library", x, y, w, h);
            break;
        case JW_SETTINGS_BEHAVIOR:
            jw__render_placeholder(ui, "Behavior", x, y, w, h);
            break;
        case JW_SETTINGS_ABOUT:
            jw__render_placeholder(ui, "About", x, y, w, h);
            break;
    }
}

/* ─── Input handling ─────────────────────────────────────────────────────── */

static bool jw__apply_theme(jw_settings_ui *ui, int new_index,
                             char *status_buf, size_t status_size,
                             bool *theme_changed) {
    if (new_index < 0 || new_index >= JW_SETTINGS_THEME_COUNT) return false;
    const char *name = kJawakaThemes[new_index];

    cat_stylesheet ss;
    if (cat_stylesheet_load_theme(&ss, name) != CAT_OK) {
        if (status_buf && status_size > 0)
            snprintf(status_buf, status_size, "%s", "theme load failed");
        return false;
    }

    /* Persist first only after a successful load. */
    if (ui->db_path[0])
        jw_db_set_setting(ui->db_path, "theme_name", name);

    cat_stylesheet_apply(&ss);
    ui->theme_index = new_index;

    if (status_buf && status_size > 0)
        snprintf(status_buf, status_size, "theme: %s", name);
    if (theme_changed) *theme_changed = true;
    return true;
}

static void jw__cycle_theme(jw_settings_ui *ui, int direction,
                             char *status_buf, size_t status_size,
                             bool *theme_changed) {
    int next = (ui->theme_index + direction) % JW_SETTINGS_THEME_COUNT;
    if (next < 0) next += JW_SETTINGS_THEME_COUNT;
    if (next == ui->theme_index) return;
    jw__apply_theme(ui, next, status_buf, status_size, theme_changed);
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

    if (status_buf && status_size > 0) {
        snprintf(status_buf, status_size, "%s", "brightness failed");
    }
}

bool jw_settings_ui_handle_button(jw_settings_ui *ui, cat_button button,
                                    char *status_buf, size_t status_size,
                                    bool *theme_changed) {
    if (!ui || !ui->open) return false;
    if (theme_changed) *theme_changed = false;

    switch (ui->screen) {
        case JW_SETTINGS_HOME:
            switch (button) {
                case CAT_BTN_UP:
                    cat_list_state_move(&ui->home_list, -1, JW_SETTINGS_CATEGORY_COUNT);
                    break;
                case CAT_BTN_DOWN:
                    cat_list_state_move(&ui->home_list, +1, JW_SETTINGS_CATEGORY_COUNT);
                    break;
                case CAT_BTN_A: {
                    int idx = ui->home_list.cursor;
                    if (idx == 0) ui->screen = JW_SETTINGS_APPEARANCE;
                    else if (idx == 1) ui->screen = JW_SETTINGS_DISPLAY;
                    else if (idx == 2) ui->screen = JW_SETTINGS_LIBRARY;
                    else if (idx == 3) ui->screen = JW_SETTINGS_BEHAVIOR;
                    else if (idx == 4) ui->screen = JW_SETTINGS_ABOUT;
                    break;
                }
                case CAT_BTN_B:
                    jw_settings_ui_close(ui);
                    return false;
                default:
                    break;
            }
            break;

        case JW_SETTINGS_APPEARANCE:
            switch (button) {
                case CAT_BTN_UP:
                    cat_list_state_move(&ui->appearance_list, -1, JW_APPEARANCE_ROW_COUNT);
                    break;
                case CAT_BTN_DOWN:
                    cat_list_state_move(&ui->appearance_list, +1, JW_APPEARANCE_ROW_COUNT);
                    break;
                case CAT_BTN_LEFT:
                case CAT_BTN_RIGHT: {
                    int dir = (button == CAT_BTN_LEFT) ? -1 : 1;
                    int row = ui->appearance_list.cursor;
                    if (row == JW_APPEARANCE_THEME) {
                        jw__cycle_theme(ui, dir, status_buf, status_size, theme_changed);
                        /* Reapply user overrides after theme change. */
                        cat_get_theme()->pill_radius_ratio =
                            kPillShapeValues[ui->pill_shape_index];
                    } else if (row == JW_APPEARANCE_PILL_SHAPE) {
                        int next = (ui->pill_shape_index + dir) % JW_SETTINGS_PILL_SHAPE_COUNT;
                        if (next < 0) next += JW_SETTINGS_PILL_SHAPE_COUNT;
                        ui->pill_shape_index = next;
                        cat_get_theme()->pill_radius_ratio = kPillShapeValues[next];
                        if (ui->db_path[0]) {
                            char val[8];
                            snprintf(val, sizeof(val), "%d", next);
                            jw_db_set_setting(ui->db_path, "pill_shape_index", val);
                        }
                    } else if (row == JW_APPEARANCE_FONT_SIZE) {
                        int next = (ui->font_size_index + dir) % JW_SETTINGS_FONT_SIZE_COUNT;
                        if (next < 0) next += JW_SETTINGS_FONT_SIZE_COUNT;
                        ui->font_size_index = next;
                        cat_set_font_bump(kFontSizeValues[next]);
                        if (ui->db_path[0]) {
                            char val[8];
                            snprintf(val, sizeof(val), "%d", next);
                            jw_db_set_setting(ui->db_path, "font_size_index", val);
                        }
                    } else if (row == JW_APPEARANCE_SHOW_HINTS) {
                        ui->show_hints = !ui->show_hints;
                        if (ui->db_path[0])
                            jw_db_set_setting(ui->db_path, "show_hints",
                                              ui->show_hints ? "1" : "0");
                    } else if (row == JW_APPEARANCE_SHOW_CLOCK) {
                        int next = (ui->clock_style_index + dir + JW_SETTINGS_CLOCK_STYLE_COUNT) % JW_SETTINGS_CLOCK_STYLE_COUNT;
                        ui->clock_style_index = next;
                        if (ui->db_path[0]) {
                            char val[8];
                            snprintf(val, sizeof(val), "%d", next);
                            jw_db_set_setting(ui->db_path, "clock_style_index", val);
                        }
                    } else if (row == JW_APPEARANCE_SHOW_BATTERY) {
                        ui->show_battery = !ui->show_battery;
                        if (ui->db_path[0])
                            jw_db_set_setting(ui->db_path, "show_battery",
                                              ui->show_battery ? "1" : "0");
                    } else if (row == JW_APPEARANCE_SHOW_WIFI) {
                        ui->show_wifi = !ui->show_wifi;
                        if (ui->db_path[0])
                            jw_db_set_setting(ui->db_path, "show_wifi",
                                              ui->show_wifi ? "1" : "0");
                    }
                    break;
                }
                case CAT_BTN_A: {
                    int row = ui->appearance_list.cursor;
                    if (row == JW_APPEARANCE_THEME) {
                        jw__cycle_theme(ui, +1, status_buf, status_size, theme_changed);
                        cat_get_theme()->pill_radius_ratio =
                            kPillShapeValues[ui->pill_shape_index];
                    } else if (row == JW_APPEARANCE_ACCENT) {
                        ap_color picked;
                        if (cat_color_picker(cat_get_theme()->accent, &picked) == CAT_OK) {
                            char hex[16];
                            snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                     picked.r, picked.g, picked.b);
                            cat_set_theme_color(hex);
                            if (ui->db_path[0])
                                jw_db_set_setting(ui->db_path, "accent_color", hex);
                        }
                    } else if (row == JW_APPEARANCE_PILL_SHAPE) {
                        int next = (ui->pill_shape_index + 1) % JW_SETTINGS_PILL_SHAPE_COUNT;
                        ui->pill_shape_index = next;
                        cat_get_theme()->pill_radius_ratio = kPillShapeValues[next];
                        if (ui->db_path[0]) {
                            char val[8];
                            snprintf(val, sizeof(val), "%d", next);
                            jw_db_set_setting(ui->db_path, "pill_shape_index", val);
                        }
                    } else if (row == JW_APPEARANCE_FONT_SIZE) {
                        int next = (ui->font_size_index + 1) % JW_SETTINGS_FONT_SIZE_COUNT;
                        ui->font_size_index = next;
                        cat_set_font_bump(kFontSizeValues[next]);
                        if (ui->db_path[0]) {
                            char val[8];
                            snprintf(val, sizeof(val), "%d", next);
                            jw_db_set_setting(ui->db_path, "font_size_index", val);
                        }
                    } else if (row == JW_APPEARANCE_SHOW_HINTS) {
                        ui->show_hints = !ui->show_hints;
                        if (ui->db_path[0])
                            jw_db_set_setting(ui->db_path, "show_hints",
                                              ui->show_hints ? "1" : "0");
                    } else if (row == JW_APPEARANCE_SHOW_CLOCK) {
                        int next = (ui->clock_style_index + 1) % JW_SETTINGS_CLOCK_STYLE_COUNT;
                        ui->clock_style_index = next;
                        if (ui->db_path[0]) {
                            char val[8];
                            snprintf(val, sizeof(val), "%d", next);
                            jw_db_set_setting(ui->db_path, "clock_style_index", val);
                        }
                    } else if (row == JW_APPEARANCE_SHOW_BATTERY) {
                        ui->show_battery = !ui->show_battery;
                        if (ui->db_path[0])
                            jw_db_set_setting(ui->db_path, "show_battery",
                                              ui->show_battery ? "1" : "0");
                    } else if (row == JW_APPEARANCE_SHOW_WIFI) {
                        ui->show_wifi = !ui->show_wifi;
                        if (ui->db_path[0])
                            jw_db_set_setting(ui->db_path, "show_wifi",
                                              ui->show_wifi ? "1" : "0");
                    } else if (row == JW_APPEARANCE_TEXT_COLOR) {
                        ap_color picked;
                        if (cat_color_picker(cat_get_theme()->text, &picked) == CAT_OK) {
                            char hex[16];
                            snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                     picked.r, picked.g, picked.b);
                            ap_theme *t = cat_get_theme();
                            t->text = picked;
                            t->highlighted_text = picked;
                            if (ui->db_path[0])
                                jw_db_set_setting(ui->db_path, "text_color", hex);
                        }
                    } else if (row == JW_APPEARANCE_BG_COLOR) {
                        ap_color picked;
                        if (cat_color_picker(cat_get_theme()->background, &picked) == CAT_OK) {
                            char hex[16];
                            snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                     picked.r, picked.g, picked.b);
                            cat_get_theme()->background = picked;
                            if (ui->db_path[0])
                                jw_db_set_setting(ui->db_path, "bg_color", hex);
                        }
                    }
                    break;
                }
                case CAT_BTN_B:
                    ui->screen = JW_SETTINGS_HOME;
                    break;
                default:
                    break;
            }
            break;

        case JW_SETTINGS_DISPLAY:
            switch (button) {
                case CAT_BTN_LEFT:
                    jw__change_brightness(ui, -JW_PLATFORM_BRIGHTNESS_STEP_PERCENT,
                                          status_buf, status_size);
                    break;
                case CAT_BTN_RIGHT:
                case CAT_BTN_A:
                    jw__change_brightness(ui, JW_PLATFORM_BRIGHTNESS_STEP_PERCENT,
                                          status_buf, status_size);
                    break;
                case CAT_BTN_B:
                    ui->screen = JW_SETTINGS_HOME;
                    break;
                default:
                    break;
            }
            break;

        case JW_SETTINGS_LIBRARY:
        case JW_SETTINGS_BEHAVIOR:
        case JW_SETTINGS_ABOUT:
            if (button == CAT_BTN_B)
                ui->screen = JW_SETTINGS_HOME;
            break;
    }

    return true;
}
