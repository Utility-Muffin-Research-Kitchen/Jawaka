#include "internal/settings/appearance.h"

#include "internal/db/db.h"
#include "internal/platform/bluetooth.h"
#include "internal/settings/theme_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JW_COLOR_BUF_LEN 16

/* Mirror of Catastrophe's CAT_CORNER_* bits. catastrophe.h is not on the daemon
   include path (jawakad links appearance.c without it), so we can't reference
   the enum here. TL=1 TR=2 BL=4 BR=8. */
#define JW_CORNER_TL  0x1
#define JW_CORNER_TR  0x2
#define JW_CORNER_BL  0x4
#define JW_CORNER_BR  0x8
#define JW_CORNER_ALL (JW_CORNER_TL | JW_CORNER_TR | JW_CORNER_BL | JW_CORNER_BR)

const char *const kJawakaFontFamilyLabels[JW_APPEARANCE_FONT_FAMILY_COUNT] = {
    "Space Grotesk",
    "Inter",
    "Rounded M+",
    "Nunito",
    "Baloo 2",
    "Fredoka",
    "Lexend",
    "IBM Plex Sans",
    "Noto Sans",
};

const char *const kJawakaFontFamilyPaths[JW_APPEARANCE_FONT_FAMILY_COUNT] = {
    "fonts/SpaceGrotesk/SpaceGrotesk-Regular.ttf",
    "fonts/Inter/Inter.ttf",
    "fonts/MPlusRounded1c/MPLUSRounded1c-Bold.ttf",
    "fonts/Nunito/Nunito-Bold.ttf",
    "fonts/Baloo2/Baloo2-Bold.ttf",
    "fonts/Fredoka/Fredoka-Bold.ttf",
    "fonts/Lexend/Lexend-Bold.ttf",
    "fonts/IBMPlexSans/IBMPlexSans-Bold.ttf",
    "fonts/NotoSans/NotoSans-Bold.ttf",
};

const float kJawakaPillRadiusValues[JW_APPEARANCE_PILL_SHAPE_COUNT] = {
    1.0f,   /* Rounded */
    0.25f,  /* Soft */
    0.0f,   /* Square */
    1.0f,   /* Leaf: full radius on its two rounded corners */
};

/* Leaf rounds top-left + bottom-right only (the others stay sharp) for a
   directional highlight. */
const int kJawakaPillCornerMasks[JW_APPEARANCE_PILL_SHAPE_COUNT] = {
    JW_CORNER_ALL,
    JW_CORNER_ALL,
    JW_CORNER_ALL,
    JW_CORNER_TL | JW_CORNER_BR,
};

const int kJawakaFontSizeValues[JW_APPEARANCE_FONT_SIZE_COUNT] = {
    0,
    2,
    4,
    5,
};

static int jw__read_index(const char *db_path, const char *key, int count, int fallback) {
    if (!db_path || !db_path[0] || !key || count <= 0) return fallback;

    char val[32];
    if (jw_db_get_setting(db_path, key, val, sizeof(val)) != 0 || !val[0])
        return fallback;

    int idx = atoi(val);
    return (idx >= 0 && idx < count) ? idx : fallback;
}

static void jw__read_setting_or_default(const char *db_path, const char *key,
                                        const char *fallback,
                                        char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    if (db_path && db_path[0] && key &&
        jw_db_get_setting(db_path, key, out, out_size) == 0 && out[0]) {
        return;
    }

    snprintf(out, out_size, "%s", fallback ? fallback : "");
}

static const char *jw__clock_token_for_index(int idx) {
    switch (idx) {
        case 0: return "hide";
        case 2: return "12";
        case 3: return "no-ampm";
        case 1:
        default: return "24";
    }
}

static int jw__bt_state_now(void) {
    if (!jw_bt_radio_is_on()) return 0;
    return (jw_bt_any_connected() == 1) ? 2 : 1;
}

int jw_appearance_font_family_index_from_db(const char *db_path) {
    return jw__read_index(db_path, "font_family_index",
                          JW_APPEARANCE_FONT_FAMILY_COUNT,
                          JW_APPEARANCE_FONT_FAMILY_DEFAULT);
}

const char *jw_appearance_font_path_for_index(int index) {
    if (index < 0 || index >= JW_APPEARANCE_FONT_FAMILY_COUNT)
        index = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
    return kJawakaFontFamilyPaths[index];
}

void jw_appearance_resolve(const char *db_path, jw_appearance_env *out) {
    if (!out) return;

    int font_idx = jw_appearance_font_family_index_from_db(db_path);
    int font_size_idx = jw__read_index(db_path, "font_size_index", JW_APPEARANCE_FONT_SIZE_COUNT, 1);
    int pill_idx = jw__read_index(db_path, "pill_shape_index", JW_APPEARANCE_PILL_SHAPE_COUNT,
                                  JW_APPEARANCE_PILL_SHAPE_DEFAULT);

    if (jw_resolve_theme_name(db_path, out->theme_name, sizeof(out->theme_name)) != 0)
        snprintf(out->theme_name, sizeof(out->theme_name), "%s", "Jawaka-Tabs");

    /* The font path table is static const, so the pointer stays valid across a
       later fork()/execv() in the child. */
    out->font_path = jw_appearance_font_path_for_index(font_idx);

    /* Pre-format the numeric values here in the parent so the child only has to
       call setenv (no vsnprintf) after fork(). */
    snprintf(out->font_bump, sizeof(out->font_bump), "%d", kJawakaFontSizeValues[font_size_idx]);
    snprintf(out->pill_radius_ratio, sizeof(out->pill_radius_ratio), "%.2f", kJawakaPillRadiusValues[pill_idx]);
    snprintf(out->pill_corner_mask, sizeof(out->pill_corner_mask), "%d", kJawakaPillCornerMasks[pill_idx]);

    /* Defaults mirror Settings' Leaf scheme so apps inherit the identity theme
       even before the first settings session persists color rows. */
    jw__read_setting_or_default(db_path, "accent_color", "#1E331E", out->accent, sizeof(out->accent));
    jw__read_setting_or_default(db_path, "bg_color", "#0F160E", out->bg, sizeof(out->bg));
    jw__read_setting_or_default(db_path, "text_color", "#E8F1E3", out->text, sizeof(out->text));
    jw__read_setting_or_default(db_path, "hint_color", "#7E9579", out->hint, sizeof(out->hint));
    jw__read_setting_or_default(db_path, "highlight_color", "#7FB069", out->highlight, sizeof(out->highlight));
    jw__read_setting_or_default(db_path, "button_label_color", "#0F160E", out->button_label, sizeof(out->button_label));
    jw__read_setting_or_default(db_path, "button_glyph_bg_color", "#7FB069", out->button_glyph_bg, sizeof(out->button_glyph_bg));

    /* Button-hints visibility ("0"/"1"), so apps hide their footers when the
       user turned hints off in the launcher. Default on. */
    jw__read_setting_or_default(db_path, "show_hints", "1", out->show_hints, sizeof(out->show_hints));

    /* Status-bar visibility mirrors the launcher so native apps wear the same
       chrome without opening the settings DB themselves. */
    jw__read_setting_or_default(db_path, "show_wifi", "1",
                                out->status_show_wifi, sizeof(out->status_show_wifi));
    jw__read_setting_or_default(db_path, "show_battery", "1",
                                out->status_show_battery, sizeof(out->status_show_battery));
    jw__read_setting_or_default(db_path, "show_battery_level", "0",
                                out->status_show_battery_level,
                                sizeof(out->status_show_battery_level));
    jw__read_setting_or_default(db_path, "show_bluetooth", "1",
                                out->status_show_bluetooth,
                                sizeof(out->status_show_bluetooth));
    snprintf(out->status_clock, sizeof(out->status_clock), "%s",
             jw__clock_token_for_index(jw__read_index(db_path, "clock_style_index", 4, 1)));
    snprintf(out->status_bt_state, sizeof(out->status_bt_state), "%d",
             jw__bt_state_now());
    jw__read_setting_or_default(db_path, "timezone", "",
                                out->timezone, sizeof(out->timezone));
}

int jw_appearance_apply_env(const jw_appearance_env *env) {
    if (!env) return -1;

    int rc = 0;
    rc |= setenv("CAT_THEME_NAME", env->theme_name, 1);
    rc |= setenv("CAT_FONT_PATH", env->font_path ? env->font_path : "", 1);
    rc |= setenv("CAT_FONT_BUMP", env->font_bump, 1);
    rc |= setenv("CAT_PILL_RADIUS_RATIO", env->pill_radius_ratio, 1);
    rc |= setenv("CAT_PILL_CORNER_MASK", env->pill_corner_mask, 1);
    rc |= setenv("CAT_COLOR_ACCENT", env->accent, 1);
    rc |= setenv("CAT_COLOR_BACKGROUND", env->bg, 1);
    rc |= setenv("CAT_COLOR_TEXT", env->text, 1);
    rc |= setenv("CAT_COLOR_HINT", env->hint, 1);
    rc |= setenv("CAT_COLOR_HIGHLIGHT", env->highlight, 1);
    rc |= setenv("CAT_COLOR_BUTTON_LABEL", env->button_label, 1);
    rc |= setenv("CAT_COLOR_BUTTON_GLYPH_BG", env->button_glyph_bg, 1);
    rc |= setenv("CAT_SHOW_HINTS", env->show_hints[0] ? env->show_hints : "1", 1);
    rc |= setenv("CAT_STATUS_SHOW_WIFI",
                 env->status_show_wifi[0] ? env->status_show_wifi : "1", 1);
    rc |= setenv("CAT_STATUS_SHOW_BATTERY",
                 env->status_show_battery[0] ? env->status_show_battery : "1", 1);
    rc |= setenv("CAT_STATUS_SHOW_BATTERY_LEVEL",
                 env->status_show_battery_level[0] ? env->status_show_battery_level : "0", 1);
    rc |= setenv("CAT_STATUS_SHOW_BLUETOOTH",
                 env->status_show_bluetooth[0] ? env->status_show_bluetooth : "1", 1);
    rc |= setenv("CAT_STATUS_CLOCK", env->status_clock[0] ? env->status_clock : "24", 1);
    rc |= setenv("CAT_STATUS_BT_STATE",
                 env->status_bt_state[0] ? env->status_bt_state : "0", 1);
    if (env->timezone[0]) {
        rc |= setenv("TZ", env->timezone, 1);
    }

    return rc == 0 ? 0 : -1;
}

int jw_appearance_export_env(const char *db_path) {
    jw_appearance_env env;
    jw_appearance_resolve(db_path, &env);
    return jw_appearance_apply_env(&env);
}
