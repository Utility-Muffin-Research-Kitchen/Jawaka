#include "internal/settings/appearance.h"

#include "internal/db/db.h"
#include "internal/settings/theme_resolve.h"

#include <stdio.h>
#include <stdarg.h>
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
};

const char *const kJawakaFontFamilyPaths[JW_APPEARANCE_FONT_FAMILY_COUNT] = {
    "fonts/SpaceGrotesk/SpaceGrotesk-Regular.ttf",
    "fonts/Inter/Inter.ttf",
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

static int jw__setenvf(const char *name, const char *fmt, ...) {
    if (!name || !fmt) return -1;

    char value[256];
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(value, sizeof(value), fmt, args);
    va_end(args);
    if (needed < 0 || needed >= (int)sizeof(value)) return -1;
    return setenv(name, value, 1);
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

int jw_appearance_export_env(const char *db_path) {
    char theme_name[256];
    char accent[JW_COLOR_BUF_LEN];
    char bg[JW_COLOR_BUF_LEN];
    char text[JW_COLOR_BUF_LEN];
    char hint[JW_COLOR_BUF_LEN];
    char highlight[JW_COLOR_BUF_LEN];
    char btn_label[JW_COLOR_BUF_LEN];
    char btn_bg[JW_COLOR_BUF_LEN];

    int font_idx = jw_appearance_font_family_index_from_db(db_path);
    int font_size_idx = jw__read_index(db_path, "font_size_index", JW_APPEARANCE_FONT_SIZE_COUNT, 1);
    int pill_idx = jw__read_index(db_path, "pill_shape_index", JW_APPEARANCE_PILL_SHAPE_COUNT, 0);

    if (jw_resolve_theme_name(db_path, theme_name, sizeof(theme_name)) != 0)
        snprintf(theme_name, sizeof(theme_name), "%s", "Jawaka-Tabs");

    /* Defaults mirror Settings' Leaf scheme so apps inherit the identity theme
       even before the first settings session persists color rows. */
    jw__read_setting_or_default(db_path, "accent_color", "#1E331E", accent, sizeof(accent));
    jw__read_setting_or_default(db_path, "bg_color", "#0F160E", bg, sizeof(bg));
    jw__read_setting_or_default(db_path, "text_color", "#E8F1E3", text, sizeof(text));
    jw__read_setting_or_default(db_path, "hint_color", "#7E9579", hint, sizeof(hint));
    jw__read_setting_or_default(db_path, "highlight_color", "#7FB069", highlight, sizeof(highlight));
    jw__read_setting_or_default(db_path, "button_label_color", "#0F160E", btn_label, sizeof(btn_label));
    jw__read_setting_or_default(db_path, "button_glyph_bg_color", "#7FB069", btn_bg, sizeof(btn_bg));

    int rc = 0;
    rc |= setenv("CAT_THEME_NAME", theme_name, 1);
    rc |= setenv("CAT_FONT_PATH", jw_appearance_font_path_for_index(font_idx), 1);
    rc |= jw__setenvf("CAT_FONT_BUMP", "%d", kJawakaFontSizeValues[font_size_idx]);
    rc |= jw__setenvf("CAT_PILL_RADIUS_RATIO", "%.2f", kJawakaPillRadiusValues[pill_idx]);
    rc |= jw__setenvf("CAT_PILL_CORNER_MASK", "%d", kJawakaPillCornerMasks[pill_idx]);
    rc |= setenv("CAT_COLOR_ACCENT", accent, 1);
    rc |= setenv("CAT_COLOR_BACKGROUND", bg, 1);
    rc |= setenv("CAT_COLOR_TEXT", text, 1);
    rc |= setenv("CAT_COLOR_HINT", hint, 1);
    rc |= setenv("CAT_COLOR_HIGHLIGHT", highlight, 1);
    rc |= setenv("CAT_COLOR_BUTTON_LABEL", btn_label, 1);
    rc |= setenv("CAT_COLOR_BUTTON_GLYPH_BG", btn_bg, 1);

    return rc == 0 ? 0 : -1;
}
