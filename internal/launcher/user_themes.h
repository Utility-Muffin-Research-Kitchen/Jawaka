/* User themes: card-root Themes/<name>/ folders with a required theme.json.
 *
 * Enumerates, parses and validates them, and resolves asset paths per view.
 * Knows nothing about the launcher's state or the candidate-order builder;
 * the launcher asks for paths and pushes them into the builder itself so one
 * builder keeps owning the whole resolution order.
 *
 * Format: umrk-workspace/plans/grid-view-and-user-themes.md ("User themes"). */
#ifndef JW_LAUNCHER_USER_THEMES_H
#define JW_LAUNCHER_USER_THEMES_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define JW_USER_THEME_MAX      32
#define JW_USER_THEME_NAME_MAX 96

/* Tile artwork limits. 512 is the documented target; 1024 is a hard cap that
   protects a 1 GB device from a 30-tile theme of 4000 px photos. Anything
   between is accepted and flagged, never rejected. */
#define JW_USER_THEME_ICON_TARGET_PX 512
#define JW_USER_THEME_ICON_MAX_PX    1024

typedef enum {
    JW_USER_THEME_STATUS_AUTO = 0,   /* derive light/dark from the wallpaper */
    JW_USER_THEME_STATUS_LIGHT,      /* light icons: the wallpaper is dark */
    JW_USER_THEME_STATUS_DARK,       /* dark icons: the wallpaper is light */
} jw_user_theme_status_style;

typedef struct {
    char dir[128];                          /* folder name under Themes/ */
    char name[JW_USER_THEME_NAME_MAX];      /* theme.json "name", else dir */
    char author[64];
    char version[32];
    int  grid_cols, grid_rows;              /* recommended density; 0 = unset */
    jw_user_theme_status_style status_style;
    bool has_grid_icons;
    bool has_grid_labels;
    bool has_coverflow_icons;
    bool has_wallpaper;
} jw_user_theme;

typedef struct {
    char          root[PATH_MAX];           /* <sdcard>/Themes */
    jw_user_theme items[JW_USER_THEME_MAX];
    int           count;
} jw_user_theme_catalog;

/* Rescan <sdcard_root>/Themes. Folders without a parseable theme.json are
   skipped (and logged): the file is required so the picker has a name to
   show and so a malformed theme fails visibly rather than as missing tiles.
   Sorted by folder name. Returns the count. */
int  jw_user_themes_scan(jw_user_theme_catalog *cat, const char *sdcard_root);

/* Index of the theme whose folder is `dir`, or -1. */
int  jw_user_themes_find(const jw_user_theme_catalog *cat, const char *dir);

/* Asset paths for one theme and one view ("grid", "coverflow"). Each returns
   false when the theme has no such asset. `_default` is never resolvable:
   it is Leaf's final fallback, not a tile. */
bool jw_user_theme_icon_path(const jw_user_theme_catalog *cat, int idx,
                             const char *view, const char *system_code,
                             char *out, size_t out_size);
bool jw_user_theme_label_path(const jw_user_theme_catalog *cat, int idx,
                              const char *view, const char *system_code,
                              char *out, size_t out_size);
/* Per-view wallpaper first, then the theme-wide one. png/jpg/jpeg. */
bool jw_user_theme_wallpaper_path(const jw_user_theme_catalog *cat, int idx,
                                  const char *view, char *out, size_t out_size);

/* Read a PNG's IHDR dimensions without decoding it. False if not a PNG. */
bool jw_user_theme_png_dims(const char *path, int *w, int *h);

/* Validate one theme's grid tiles against the guidelines by scanning its
   grid/icons folder. `present` is how many PNGs it holds, `flagged` how many
   are not the documented square target (accepted, contain-fit). Returns how
   many exceed the hard cap or are not PNGs (rejected at load). The caller
   composes the user-facing text so it can be translated. */
int  jw_user_theme_validate(const jw_user_theme_catalog *cat, int idx,
                            int *present, int *flagged);

#endif
