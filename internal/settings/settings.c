#include "internal/settings/settings.h"

#include "internal/db/db.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/device.h"

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

static const char *kHomeCategoryLabels[] = {
    "Appearance",
    "Display",
    "Library",
    "Behavior",
    "About",
};
#define JW_SETTINGS_CATEGORY_COUNT 5
#define JW_SETTINGS_DISPLAY_COUNT 1

static const char *kAppearLabels[] = {
    "Theme",
    "Colors",
    "Layout",
    "Status Bar",
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
    cat_list_state_init(&ui->home_list,       JW_SETTINGS_CATEGORY_COUNT);
    cat_list_state_init(&ui->appearance_list,  JW_APPEAR_ROW_COUNT);
    cat_list_state_init(&ui->colors_list,      JW_COLOR_ROW_COUNT);
    cat_list_state_init(&ui->layout_list,      JW_LAYOUT_ROW_COUNT);
    cat_list_state_init(&ui->statusbar_list,   JW_STATUSBAR_ROW_COUNT);
    cat_list_state_init(&ui->display_list,     JW_SETTINGS_DISPLAY_COUNT);
    cat_list_state_init(&ui->placeholder_list, 1);
    ui->theme_index       = jw__find_theme_index(initial_theme_name);
    ui->pill_shape_index  = 0;
    ui->font_size_index   = 1;
    ui->show_hints        = true;
    ui->clock_style_index = 1;
    ui->show_battery      = true;
    ui->show_wifi         = true;
    ui->brightness_percent = 50;

    if (db_path && db_path[0])
        snprintf(ui->db_path, sizeof(ui->db_path), "%s", db_path);
    if (socket_path && socket_path[0])
        snprintf(ui->socket_path, sizeof(ui->socket_path), "%s", socket_path);

    /* Restore persisted overrides. */
    if (ui->db_path[0]) {
        char val[32];
        if (jw_db_get_setting(db_path, "pill_shape_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_PILL_SHAPE_COUNT) {
                ui->pill_shape_index = idx;
                cat_get_theme()->pill_radius_ratio = kPillShapeValues[idx];
            }
        }
        if (jw_db_get_setting(db_path, "accent_color", val, sizeof(val)) == 0 && val[0])
            cat_set_theme_color(val);
        if (jw_db_get_setting(db_path, "font_size_index", val, sizeof(val)) == 0) {
            int idx = atoi(val);
            if (idx >= 0 && idx < JW_SETTINGS_FONT_SIZE_COUNT) {
                ui->font_size_index = idx;
                cat_set_font_bump(kFontSizeValues[idx]);
            }
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
        if (jw_db_get_setting(db_path, "show_wifi", val, sizeof(val)) == 0)
            ui->show_wifi = (strcmp(val, "0") != 0);
        if (jw_db_get_setting(db_path, "text_color", val, sizeof(val)) == 0 && val[0]) {
            ap_color c = cat_hex_to_color(val);
            ap_theme *t = cat_get_theme();
            t->text = c;
            t->highlighted_text = c;
        }
        if (jw_db_get_setting(db_path, "bg_color", val, sizeof(val)) == 0 && val[0])
            cat_get_theme()->background = cat_hex_to_color(val);
    }

    jw__refresh_brightness(ui);
}

void jw_settings_ui_enter(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = true;
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
                                const char *value) {
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
        char value_str[96];
        snprintf(value_str, sizeof(value_str), "\xe2\x80\xb9 %s \xe2\x80\xba", value);
        int vw = cat_measure_text(body, value_str);
        int vx = x + w - vw - cat_scale(16);
        if (vx < x + w / 2) vx = x + w / 2;
        cat_draw_text(body, value_str, vx, ty, value_c);
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
    int sz = cat_scale(14);
    int sx = x + w - cat_scale(20) - sz;
    int sy = list_y + row * item_h + (item_h - sz) / 2;
    cat_draw_rect(sx, sy, sz, sz, c);
}

/* ─── Page renderers ───────────────────────────────────────────────────── */

static void jw__render_home(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Settings", x, y, w);
    int ly = y + jw__header_h();
    for (int i = 0; i < JW_SETTINGS_CATEGORY_COUNT; i++)
        jw__render_nav_row(&ui->home_list, x, ly, w, i, kHomeCategoryLabels[i]);
    (void)h;
}

static void jw__render_appearance(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Appearance", x, y, w);
    int ly = y + jw__header_h();
    jw__render_list_row(&ui->appearance_list, x, ly, w, JW_APPEAR_THEME,
                        "Theme", kJawakaThemes[ui->theme_index]);
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
        { "Text",              t->text },
        { "Secondary Text",    t->hint },
        { "Selection",         t->highlight },
        { "Background",        t->background },
        { "Button Text",       t->button_label },
        { "Button Background", t->button_glyph_bg },
    };

    for (int i = 0; i < JW_COLOR_ROW_COUNT; i++) {
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                 rows[i].color.r, rows[i].color.g, rows[i].color.b);
        jw__render_list_row(&ui->colors_list, x, ly, w, i, rows[i].label, hex);
        jw__render_color_swatch(x, ly, w, i, rows[i].color);
    }
    (void)h;
}

static void jw__render_layout(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Layout", x, y, w);
    int ly = y + jw__header_h();
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_PILL_SHAPE,
                        "List Style", kPillShapeLabels[ui->pill_shape_index]);
    jw__render_list_row(&ui->layout_list, x, ly, w, JW_LAYOUT_FONT_SIZE,
                        "Font Size", kFontSizeLabels[ui->font_size_index]);
    (void)h;
}

static void jw__render_statusbar(const jw_settings_ui *ui, int x, int y, int w, int h) {
    jw__draw_header("Status Bar", x, y, w);
    int ly = y + jw__header_h();
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_HINTS,
                        "Button Hints", ui->show_hints ? "On" : "Off");
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_CLOCK,
                        "Clock", kClockStyleLabels[ui->clock_style_index]);
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_BATTERY,
                        "Battery", ui->show_battery ? "On" : "Off");
    jw__render_list_row(&ui->statusbar_list, x, ly, w, JW_STATUSBAR_WIFI,
                        "Wifi", ui->show_wifi ? "On" : "Off");
    (void)h;
}

static void jw__render_display(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    jw__draw_header("Display", x, y, w);
    int hh = jw__header_h();
    int item_h = TTF_FontHeight(body) + cat_scale(28);
    int iy = y + hh;
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
    jw__draw_header(title, x, y, w);
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    int hh = jw__header_h();
    const char *msg = "Coming soon";
    int body_h = TTF_FontHeight(body);
    int mw = cat_measure_text(body, msg);
    cat_draw_text(body, msg, x + (w - mw) / 2,
                  y + hh + (h - hh - body_h) / 2,
                  cat_get_theme()->hint);
    (void)ui;
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
        case JW_SETTINGS_LIBRARY:    jw__render_placeholder(ui, "Library", x, y, w, h);  break;
        case JW_SETTINGS_BEHAVIOR:   jw__render_placeholder(ui, "Behavior", x, y, w, h); break;
        case JW_SETTINGS_ABOUT:      jw__render_placeholder(ui, "About", x, y, w, h);    break;
    }
}

/* ─── Theme helpers ────────────────────────────────────────────────────── */

static void jw__reapply_user_overrides(jw_settings_ui *ui) {
    ap_theme *t = cat_get_theme();
    t->pill_radius_ratio = kPillShapeValues[ui->pill_shape_index];

    if (!ui->db_path[0]) return;

    char val[32];
    if (jw_db_get_setting(ui->db_path, "accent_color", val, sizeof(val)) == 0 && val[0])
        t->accent = cat_hex_to_color(val);
    if (jw_db_get_setting(ui->db_path, "text_color", val, sizeof(val)) == 0 && val[0]) {
        t->text = cat_hex_to_color(val);
        t->highlighted_text = t->text;
    }
    if (jw_db_get_setting(ui->db_path, "hint_color", val, sizeof(val)) == 0 && val[0])
        t->hint = cat_hex_to_color(val);
    if (jw_db_get_setting(ui->db_path, "highlight_color", val, sizeof(val)) == 0 && val[0])
        t->highlight = cat_hex_to_color(val);
    if (jw_db_get_setting(ui->db_path, "bg_color", val, sizeof(val)) == 0 && val[0])
        t->background = cat_hex_to_color(val);
    if (jw_db_get_setting(ui->db_path, "button_label_color", val, sizeof(val)) == 0 && val[0])
        t->button_label = cat_hex_to_color(val);
    if (jw_db_get_setting(ui->db_path, "button_glyph_bg_color", val, sizeof(val)) == 0 && val[0])
        t->button_glyph_bg = cat_hex_to_color(val);
}

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

    jw__persist(ui, "theme_name", name);
    cat_stylesheet_apply(&ss);
    ui->theme_index = new_index;

    jw__reapply_user_overrides(ui);

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
    if (status_buf && status_size > 0)
        snprintf(status_buf, status_size, "%s", "brightness failed");
}

/* ─── Color picker helper ──────────────────────────────────────────────── */

static bool jw__pick_color(jw_settings_ui *ui, ap_color *target,
                           const char *db_key) {
    ap_color picked;
    if (cat_color_picker(*target, &picked) == CAT_OK) {
        *target = picked;
        jw__persist_color(ui, db_key, picked);
        return true;
    }
    return false;
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
                else if (idx == 1) ui->screen = JW_SETTINGS_DISPLAY;
                else if (idx == 2) ui->screen = JW_SETTINGS_LIBRARY;
                else if (idx == 3) ui->screen = JW_SETTINGS_BEHAVIOR;
                else if (idx == 4) ui->screen = JW_SETTINGS_ABOUT;
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
                    jw__cycle_theme(ui, -1, status_buf, status_size, theme_changed);
                break;
            case CAT_BTN_RIGHT:
            case CAT_BTN_A: {
                int row = ui->appearance_list.cursor;
                if (row == JW_APPEAR_THEME)
                    jw__cycle_theme(ui, +1, status_buf, status_size, theme_changed);
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
                if (row == JW_COLOR_ACCENT) {
                    if (jw__pick_color(ui, &t->accent, "accent_color"))
                        cat_set_theme_color(NULL); /* force re-derive */
                } else if (row == JW_COLOR_TEXT) {
                    if (jw__pick_color(ui, &t->text, "text_color"))
                        t->highlighted_text = t->text;
                } else if (row == JW_COLOR_HINT) {
                    jw__pick_color(ui, &t->hint, "hint_color");
                } else if (row == JW_COLOR_HIGHLIGHT) {
                    jw__pick_color(ui, &t->highlight, "highlight_color");
                } else if (row == JW_COLOR_BACKGROUND) {
                    jw__pick_color(ui, &t->background, "bg_color");
                } else if (row == JW_COLOR_BTN_TEXT) {
                    jw__pick_color(ui, &t->button_label, "button_label_color");
                } else if (row == JW_COLOR_BTN_BG) {
                    jw__pick_color(ui, &t->button_glyph_bg, "button_glyph_bg_color");
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
                    cat_get_theme()->pill_radius_ratio = kPillShapeValues[next];
                    jw__persist_int(ui, "pill_shape_index", next);
                } else if (row == JW_LAYOUT_FONT_SIZE) {
                    int next = (ui->font_size_index + dir + JW_SETTINGS_FONT_SIZE_COUNT) % JW_SETTINGS_FONT_SIZE_COUNT;
                    ui->font_size_index = next;
                    cat_set_font_bump(kFontSizeValues[next]);
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
                    ui->show_battery = !ui->show_battery;
                    jw__persist_bool(ui, "show_battery", ui->show_battery);
                } else if (row == JW_STATUSBAR_WIFI) {
                    ui->show_wifi = !ui->show_wifi;
                    jw__persist_bool(ui, "show_wifi", ui->show_wifi);
                }
                break;
            }
            case CAT_BTN_B:
                ui->screen = JW_SETTINGS_APPEARANCE;
                break;
            default: break;
        }
        break;

    /* ── Display (brightness) ────────────────────────────────────────── */
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
            default: break;
        }
        break;

    /* ── Placeholders ────────────────────────────────────────────────── */
    case JW_SETTINGS_LIBRARY:
    case JW_SETTINGS_BEHAVIOR:
    case JW_SETTINGS_ABOUT:
        if (button == CAT_BTN_B)
            ui->screen = JW_SETTINGS_HOME;
        break;
    }

    return ui->open;
}
