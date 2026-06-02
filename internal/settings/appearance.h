#ifndef JW_SETTINGS_APPEARANCE_H
#define JW_SETTINGS_APPEARANCE_H

#include <stddef.h>

#define JW_APPEARANCE_FONT_FAMILY_COUNT 2
#define JW_APPEARANCE_FONT_FAMILY_DEFAULT 0

extern const char *const kJawakaFontFamilyLabels[JW_APPEARANCE_FONT_FAMILY_COUNT];
extern const char *const kJawakaFontFamilyPaths[JW_APPEARANCE_FONT_FAMILY_COUNT];

/* Canonical appearance value tables. Defined in appearance.c — the one TU
   linked into both jawakad and the UI binaries — so the daemon's env export and
   the settings UI share a single source of truth. settings.c owns the matching
   display labels and static-asserts its row counts against these. */
#define JW_APPEARANCE_PILL_SHAPE_COUNT 4
#define JW_APPEARANCE_FONT_SIZE_COUNT  4

extern const float kJawakaPillRadiusValues[JW_APPEARANCE_PILL_SHAPE_COUNT];
extern const int   kJawakaPillCornerMasks[JW_APPEARANCE_PILL_SHAPE_COUNT];
extern const int   kJawakaFontSizeValues[JW_APPEARANCE_FONT_SIZE_COUNT];

int jw_appearance_font_family_index_from_db(const char *db_path);
const char *jw_appearance_font_path_for_index(int index);
int jw_appearance_export_env(const char *db_path);

#endif
