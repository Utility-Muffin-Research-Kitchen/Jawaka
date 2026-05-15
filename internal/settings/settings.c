#include "internal/settings/settings.h"

#include "internal/db/db.h"

#include <stdio.h>
#include <string.h>

const char *const kJawakaThemes[JW_SETTINGS_THEME_COUNT] = {
    "Jawaka-Tabs",
    "Jawaka-Vertical",
    "Jawaka-Horizontal",
};

static const char *kCategoryLabels[] = {
    "Appearance",
    "Library",
    "Behavior",
    "About",
};
#define JW_SETTINGS_CATEGORY_COUNT 4

#define JW_SETTINGS_APPEARANCE_COUNT 1

/* ─── Lifecycle ──────────────────────────────────────────────────────────── */

static int jw__find_theme_index(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < JW_SETTINGS_THEME_COUNT; i++) {
        if (strcmp(kJawakaThemes[i], name) == 0) return i;
    }
    return 0;
}

void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path,
                          const char *initial_theme_name) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->open    = false;
    ui->screen  = JW_SETTINGS_HOME;
    cat_list_state_init(&ui->home_list,        JW_SETTINGS_CATEGORY_COUNT);
    cat_list_state_init(&ui->appearance_list,  JW_SETTINGS_APPEARANCE_COUNT);
    cat_list_state_init(&ui->placeholder_list, 1);
    ui->theme_index = jw__find_theme_index(initial_theme_name);
    if (db_path && db_path[0])
        snprintf(ui->db_path, sizeof(ui->db_path), "%s", db_path);
}

void jw_settings_ui_enter(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = true;
    /* Always re-enter at home; home cursor is preserved across enter/close. */
    ui->screen = JW_SETTINGS_HOME;
}

void jw_settings_ui_close(jw_settings_ui *ui) {
    if (!ui) return;
    ui->open = false;
    ui->screen = JW_SETTINGS_HOME;
}

bool jw_settings_ui_is_open(const jw_settings_ui *ui) {
    return ui && ui->open;
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

static void jw__render_appearance(const jw_settings_ui *ui, int x, int y, int w, int h) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *large = cat_get_font(CAT_FONT_LARGE);

    jw__draw_header("Appearance", x, y, w);
    int header_h = TTF_FontHeight(large) + cat_scale(10);

    int list_y = y + header_h;
    int item_h = TTF_FontHeight(body) + cat_scale(12);

    /* Theme row */
    int iy = list_y;
    bool selected = (ui->appearance_list.cursor == 0);
    int pill_h = TTF_FontHeight(body) + cat_scale(6);
    int pill_y = iy + (item_h - pill_h) / 2;
    if (selected)
        cat_draw_pill(x, pill_y, w - cat_scale(4), pill_h, theme->highlight);

    ap_color label_c = selected ? theme->highlighted_text : theme->text;
    ap_color value_c = selected ? theme->highlighted_text : theme->hint;
    int ty = pill_y + (pill_h - TTF_FontHeight(body)) / 2;

    cat_draw_text_ellipsized(body, "Theme", x + cat_scale(12), ty, label_c,
                              w / 2 - cat_scale(20));

    /* Value: "‹  Jawaka-Vertical  ›" */
    const char *current = kJawakaThemes[ui->theme_index];
    char value_str[96];
    snprintf(value_str, sizeof(value_str), "\xe2\x80\xb9 %s \xe2\x80\xba", current);
    int vw = cat_measure_text(body, value_str);
    int vx = x + w - vw - cat_scale(16);
    if (vx < x + w / 2) vx = x + w / 2;
    cat_draw_text(body, value_str, vx, ty, value_c);

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
                    else if (idx == 1) ui->screen = JW_SETTINGS_LIBRARY;
                    else if (idx == 2) ui->screen = JW_SETTINGS_BEHAVIOR;
                    else if (idx == 3) ui->screen = JW_SETTINGS_ABOUT;
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
                    cat_list_state_move(&ui->appearance_list, -1,
                                         JW_SETTINGS_APPEARANCE_COUNT);
                    break;
                case CAT_BTN_DOWN:
                    cat_list_state_move(&ui->appearance_list, +1,
                                         JW_SETTINGS_APPEARANCE_COUNT);
                    break;
                case CAT_BTN_LEFT:
                    if (ui->appearance_list.cursor == 0)
                        jw__cycle_theme(ui, -1, status_buf, status_size, theme_changed);
                    break;
                case CAT_BTN_RIGHT:
                    if (ui->appearance_list.cursor == 0)
                        jw__cycle_theme(ui, +1, status_buf, status_size, theme_changed);
                    break;
                case CAT_BTN_A:
                    if (ui->appearance_list.cursor == 0)
                        jw__cycle_theme(ui, +1, status_buf, status_size, theme_changed);
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
