# Jawaka-Coverflow — 4th theme: icon carousel with animated coverflow

This plan is written so a coding LLM (or contributor) can implement it end-to-end without ambiguity. Design questions were resolved during a /grill-me pass; rationale is preserved inline where helpful. The plan touches **Catastrophe** (one enum value + struct fields + JSON parser + one public accessor), **Jawaka** (one new render function, one new icon loader, one dispatch case in the main render switch, animation state on `jw_launcher_state`), and ships a new theme dir under `res/themes/`.

## 1. Context

The existing `Jawaka-Horizontal` theme renders a parallelogram-skewed text carousel ([cmd/jawaka-launcher/main.c:809 `jw__render_horizontal`](Jawaka/cmd/jawaka-launcher/main.c:809), [`jw__draw_carousel_tile`](Jawaka/cmd/jawaka-launcher/main.c:679)). The reference style we want (Game Boy / SNES picker screenshot) is **not parallelograms**; it's a **coverflow of real console icons** — the active console rendered large with its full label below, and a single neighbour each side rendered smaller and dimmer. Dragging left/right slides icons smoothly between positions (size + alpha tween, not just x).

The existing theme is keeping its current look. Coverflow ships as a separate fourth theme — `Jawaka-Coverflow` — selectable via the same theme picker that already lists the other three.

## 2. Design decisions (resolved up-front)

| # | Decision | Choice |
|---|---|---|
| 1 | Icon source | Theme-bundled with user override. Loader order: `/Roms/<SYSTEM>/icon.png` (user) → `<theme_dir>/system_icons/<SYSTEM>.png` (bundled) → `<theme_dir>/system_icons/_default.png` (fallback). |
| 2 | Icon assets origin | Libretro Systematic (CC BY-SA 4.0), pinned to a specific commit. Renamed to Jawaka short codes during import. |
| 3 | Icon shape handling | Aspect-preserved fit-within-square. Reuses `jw__draw_image_fit` ([main.c:951](Jawaka/cmd/jawaka-launcher/main.c:951)). |
| 4 | Animation | Eased slide. **Position + size + alpha** all tween together. ~180ms ease-out-cubic. Rapid input restarts the animation from the current animated position. |
| 5 | Neighbors visible | **1 each side.** Cleaner than Horizontal's 3. |
| 6 | Sizing config | Independent: `coverflow_icon_size` (center, default 256) and `coverflow_side_size` (neighbours, default 160). |
| 7 | Label fonts | `CAT_FONT_EXTRA_LARGE` for system name, `CAT_FONT_SMALL` for game-count subtitle. |
| 8 | Wallpaper | Solid `background_color` from stylesheet — no bundled wallpaper PNG. |
| 9 | Tools tile | Treated as a pseudo-system with its own icon (`_tools.png`, hand-drawn, MIT-clean). |
| 10 | ARCADE icon | Map to libretro `MAME.png`. |
| 11 | PORTS icon | Falls back to `_default.png`. |
| 12 | Footer / overlays | Mirror Horizontal verbatim (same buttons, same Tools / Settings overlays). |
| 13 | flat_items model | Use `jw__build_carousel_list` ([main.c:107](Jawaka/cmd/jawaka-launcher/main.c:107)) — systems + Tools tile, no Recents/Favorites/Apps in the carousel itself. |
| 14 | Asset import | `scripts/fetch-coverflow-icons.sh` curls libretro Systematic at a pinned commit, renames per the mapping table, drops into `res/themes/Jawaka-Coverflow/system_icons/`. |

## 3. File-by-file changes

### 3.1 `Catastrophe/include/catastrophe.h`

**Add enum value** at the existing `cat_launcher_layout` definition ([catastrophe.h:429](Catastrophe/include/catastrophe.h:429)):

```c
typedef enum {
    CAT_LAUNCHER_TABBED     = 0,
    CAT_LAUNCHER_VERTICAL   = 1,
    CAT_LAUNCHER_HORIZONTAL = 2,
    CAT_LAUNCHER_COVERFLOW  = 3,  /* icon coverflow carousel */
} cat_launcher_layout;
```

**Extend `cat_stylesheet_launcher`** ([catastrophe.h:435](Catastrophe/include/catastrophe.h:435)):

```c
typedef struct {
    cat_launcher_layout layout;
    float               list_split;
    int                 carousel_skew;
    /* Coverflow tunables (CAT_LAUNCHER_COVERFLOW only) */
    int                 coverflow_icon_size;     /* center icon px (logical), default 256 */
    int                 coverflow_side_size;     /* side icon px (logical), default 160 */
    int                 coverflow_spacing;       /* px between icon centers, default 280 */
    uint8_t             coverflow_side_alpha;    /* default 140 */
    uint32_t            coverflow_anim_ms;       /* slide duration, default 180 */
    char                coverflow_icon_dir[128]; /* relative to theme dir, default "system_icons" */
} cat_stylesheet_launcher;
```

**Init defaults** in `cat__stylesheet_launcher_init_default()` ([catastrophe.h:1371](Catastrophe/include/catastrophe.h:1371)):

```c
static void cat__stylesheet_launcher_init_default(cat_stylesheet_launcher *l) {
    l->layout = CAT_LAUNCHER_TABBED;
    l->list_split = 0.45f;
    l->carousel_skew = 30;
    l->coverflow_icon_size  = 256;
    l->coverflow_side_size  = 160;
    l->coverflow_spacing    = 280;
    l->coverflow_side_alpha = 140;
    l->coverflow_anim_ms    = 180;
    strncpy(l->coverflow_icon_dir, "system_icons",
            sizeof(l->coverflow_icon_dir) - 1);
    l->coverflow_icon_dir[sizeof(l->coverflow_icon_dir) - 1] = '\0';
}
```

**Parse new fields** in `cat__stylesheet_load_launcher()` ([catastrophe.h:1492](Catastrophe/include/catastrophe.h:1492)). After the existing `layout` and `carousel_skew` parsing, also accept `"coverflow"` for layout and read the new keys:

```c
/* Inside cat__stylesheet_load_launcher() */
v = cJSON_GetObjectItem(obj, "layout");
if (v && cJSON_IsString(v)) {
    if (strcmp(v->valuestring, "tabbed") == 0)
        l->layout = CAT_LAUNCHER_TABBED;
    else if (strcmp(v->valuestring, "vertical") == 0)
        l->layout = CAT_LAUNCHER_VERTICAL;
    else if (strcmp(v->valuestring, "horizontal") == 0)
        l->layout = CAT_LAUNCHER_HORIZONTAL;
    else if (strcmp(v->valuestring, "coverflow") == 0)
        l->layout = CAT_LAUNCHER_COVERFLOW;
}

/* ... existing list_split / carousel_skew parsing ... */

v = cJSON_GetObjectItem(obj, "coverflow_icon_size");
if (v && cJSON_IsNumber(v)) l->coverflow_icon_size = v->valueint;
v = cJSON_GetObjectItem(obj, "coverflow_side_size");
if (v && cJSON_IsNumber(v)) l->coverflow_side_size = v->valueint;
v = cJSON_GetObjectItem(obj, "coverflow_spacing");
if (v && cJSON_IsNumber(v)) l->coverflow_spacing = v->valueint;
v = cJSON_GetObjectItem(obj, "coverflow_side_alpha");
if (v && cJSON_IsNumber(v)) l->coverflow_side_alpha = (uint8_t)v->valueint;
v = cJSON_GetObjectItem(obj, "coverflow_anim_ms");
if (v && cJSON_IsNumber(v)) l->coverflow_anim_ms = (uint32_t)v->valueint;
v = cJSON_GetObjectItem(obj, "coverflow_icon_dir");
if (v && cJSON_IsString(v)) {
    strncpy(l->coverflow_icon_dir, v->valuestring,
            sizeof(l->coverflow_icon_dir) - 1);
    l->coverflow_icon_dir[sizeof(l->coverflow_icon_dir) - 1] = '\0';
}
```

**Expose active theme dir** (currently private state on `cat__g`). Add public accessor at the end of the API section (~line 519, near `cat_stylesheet_list_wallpapers`):

```c
/* Returns absolute path to the directory containing the active theme, or
   empty string if no theme loaded. Pointer is owned by Catastrophe. */
const char *cat_get_active_theme_dir(void);
const char *cat_get_active_theme_name(void);
```

Implement near the existing `cat_stylesheet_apply()` body in catastrophe.h:

```c
const char *cat_get_active_theme_dir(void)  { return cat__g.active_theme_dir; }
const char *cat_get_active_theme_name(void) { return cat__g.active_theme; }
```

### 3.2 `Jawaka/cmd/jawaka-launcher/main.c`

#### 3.2.1 Add coverflow animation state to `jw_launcher_state`

Find the struct around [main.c:80](Jawaka/cmd/jawaka-launcher/main.c:80) (where `flat_items[]` and `flat_count` live). Add:

```c
typedef struct {
    bool      active;       /* true while animation in flight */
    int       from_cursor;  /* logical position at animation start (was state->list.cursor) */
    int       to_cursor;    /* target position */
    uint32_t  start_ms;     /* SDL_GetTicks() when animation started */
    float     from_progress;/* if a previous animation interrupted, where it had reached
                               on the [from_cursor..to_cursor] segment (0..1).
                               Used to compute starting visual offset when chained. */
} jw_coverflow_anim;
```

And inside `jw_launcher_state`:

```c
jw_coverflow_anim coverflow_anim;
```

Zero-initialise it on `jw__build_carousel_list` or on launcher init so first render is a no-op snap.

#### 3.2.2 System-icon loader

Add near the existing `jw__load_cached_image` at [main.c:921](Jawaka/cmd/jawaka-launcher/main.c:921):

```c
/* Loads the system icon for a given Jawaka system code, following:
     1. /Roms/<SYSTEM>/icon.png   (user override; only checked if Roms root known)
     2. <theme_dir>/<active_theme>/<coverflow_icon_dir>/<SYSTEM>.png   (bundled)
     3. <theme_dir>/<active_theme>/<coverflow_icon_dir>/_default.png   (fallback)
   Returns NULL only if all three fail (which means even the bundled default
   is missing — log once and let the caller skip rendering).

   For the Tools tile, pass the literal "_tools" as system_code.
*/
static SDL_Texture *jw__load_system_icon(const char *system_code,
                                         int *out_w, int *out_h) {
    const cat_stylesheet *ss = cat_get_stylesheet();
    const char *theme_dir   = cat_get_active_theme_dir();
    const char *theme_name  = cat_get_active_theme_name();
    char path[1024];

    /* (1) User override under /Roms/<SYSTEM>/icon.png — only for real systems,
           not for the Tools pseudo-system (system_code starting with '_'). */
    if (system_code[0] != '_') {
        const char *roms_root = getenv("JAWAKA_ROMS_DIR");
        if (!roms_root || !roms_root[0]) roms_root = "./mock-sdcard/Roms";
        snprintf(path, sizeof(path), "%s/%s/icon.png", roms_root, system_code);
        SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
        if (t) return t;
    }

    /* (2) Theme-bundled */
    if (theme_dir[0] && theme_name[0]) {
        snprintf(path, sizeof(path), "%s/%s/%s/%s.png",
                 theme_dir, theme_name,
                 ss->launcher.coverflow_icon_dir, system_code);
        SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
        if (t) return t;
    }

    /* (3) Bundled _default.png */
    if (theme_dir[0] && theme_name[0]) {
        snprintf(path, sizeof(path), "%s/%s/%s/_default.png",
                 theme_dir, theme_name,
                 ss->launcher.coverflow_icon_dir);
        SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
        if (t) return t;
    }

    return NULL;
}
```

The texture cache (`jw__load_cached_image`) means repeated calls are cheap — no need to pre-load all icons.

#### 3.2.3 Animation helpers

```c
static float jw__ease_out_cubic(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* Returns continuous cursor position [0..flat_count-1] for the current frame.
   When animating, interpolates between from_cursor and to_cursor using
   ease-out-cubic over coverflow_anim_ms. */
static float jw__coverflow_visual_cursor(const jw_launcher_state *state) {
    const jw_coverflow_anim *a = &state->coverflow_anim;
    if (!a->active) return (float)state->list.cursor;

    const cat_stylesheet *ss = cat_get_stylesheet();
    uint32_t elapsed = SDL_GetTicks() - a->start_ms;
    if (elapsed >= ss->launcher.coverflow_anim_ms) {
        return (float)a->to_cursor;
    }
    float t = (float)elapsed / (float)ss->launcher.coverflow_anim_ms;
    float eased = jw__ease_out_cubic(t);
    return (float)a->from_cursor + ((float)a->to_cursor - (float)a->from_cursor) * eased;
}

/* Called when the user moves the cursor while in coverflow mode. */
static void jw__coverflow_start_anim(jw_launcher_state *state, int new_cursor) {
    jw_coverflow_anim *a = &state->coverflow_anim;
    /* Snapshot whatever visual position we'd render this very moment so the
       new tween starts there. This implements "restart from current animated
       position" — no rubber-banding back to the previous start. */
    float current_visual = jw__coverflow_visual_cursor(state);
    a->active      = true;
    a->from_cursor = (int)roundf(current_visual * 1000.0f) / 1000;  /* keep float in start-ms domain below */
    /* Store the visual position as a float by abusing from_cursor + from_progress: */
    a->from_cursor   = (int)floorf(current_visual);
    a->from_progress = current_visual - (float)a->from_cursor;
    a->to_cursor     = new_cursor;
    a->start_ms      = SDL_GetTicks();
    state->list.cursor = new_cursor;  /* logical cursor updates immediately */
}
```

Note on `from_progress`: the simpler form is to store a float `from_visual` directly. Use that — drop the trick with `from_cursor + from_progress`:

```c
typedef struct {
    bool      active;
    float     from_visual;   /* visual cursor at animation start */
    int       to_cursor;     /* logical target */
    uint32_t  start_ms;
} jw_coverflow_anim;

static float jw__coverflow_visual_cursor(const jw_launcher_state *state) {
    const jw_coverflow_anim *a = &state->coverflow_anim;
    if (!a->active) return (float)state->list.cursor;
    const cat_stylesheet *ss = cat_get_stylesheet();
    uint32_t elapsed = SDL_GetTicks() - a->start_ms;
    if (elapsed >= ss->launcher.coverflow_anim_ms) return (float)a->to_cursor;
    float t = (float)elapsed / (float)ss->launcher.coverflow_anim_ms;
    float eased = jw__ease_out_cubic(t);
    return a->from_visual + ((float)a->to_cursor - a->from_visual) * eased;
}

static void jw__coverflow_start_anim(jw_launcher_state *state, int new_cursor) {
    jw_coverflow_anim *a = &state->coverflow_anim;
    a->from_visual = jw__coverflow_visual_cursor(state);
    a->to_cursor   = new_cursor;
    a->start_ms    = SDL_GetTicks();
    a->active      = true;
    state->list.cursor = new_cursor;
}
```

Replace any earlier draft with this cleaner version.

#### 3.2.4 Input hook

Find the input handler for the current Horizontal mode (search `cat_input_is_pressed` near the render dispatch, or in the launcher's main loop). For coverflow mode, when Left/Right is pressed:

```c
if (ss->launcher.layout == CAT_LAUNCHER_COVERFLOW) {
    int new_cursor = state->list.cursor;
    if (left_pressed)  new_cursor = (new_cursor > 0) ? new_cursor - 1 : 0;
    if (right_pressed) new_cursor = (new_cursor < state->flat_count - 1)
                                       ? new_cursor + 1 : state->flat_count - 1;
    if (new_cursor != state->list.cursor) {
        jw__coverflow_start_anim(state, new_cursor);
    }
} else {
    /* existing horizontal/vertical/tabs input */
}
```

While `state->coverflow_anim.active` is true, force the main loop to redraw every frame (existing mechanism via `cat_request_redraw()` or similar — confirm during implementation by reading the loop in `cmd/jawaka-launcher/main.c`).

Mark `active = false` when `SDL_GetTicks() - start_ms >= coverflow_anim_ms` at the top of `jw__render_coverflow()`.

#### 3.2.5 Render function

```c
static void jw__render_coverflow(jw_launcher_state *state) {
    cat_clear_screen();

    const cat_stylesheet *ss = cat_get_stylesheet();
    ap_theme *theme = cat_get_theme();
    TTF_Font *label_font = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int fh = cat_get_footer_height();

    int icon_c  = CAT_S(ss->launcher.coverflow_icon_size);
    int icon_s  = CAT_S(ss->launcher.coverflow_side_size);
    int spacing = CAT_S(ss->launcher.coverflow_spacing);
    int cx0     = sw / 2;
    int cy      = sh / 2 - fh / 2 - CAT_S(20);
    int count   = state->flat_count;

    /* Retire the animation if it's finished — done at the top of render
       so input handler and visual cursor stay in sync. */
    jw_coverflow_anim *a = &state->coverflow_anim;
    if (a->active && SDL_GetTicks() - a->start_ms >= ss->launcher.coverflow_anim_ms) {
        a->active = false;
    }

    float v_cursor = jw__coverflow_visual_cursor(state);

    /* For each system in range [floor(v_cursor) - 1, ceil(v_cursor) + 1],
       compute (cx, size, alpha) interpolating between center and side
       presentation based on its distance from v_cursor.
       Note: during animation, *two* items can be partially "center-ish"
       — this is what creates the smooth size/alpha transition. */
    int lo = (int)floorf(v_cursor) - 1;
    int hi = (int)floorf(v_cursor) + 2;
    if (lo < 0) lo = 0;
    if (hi > count - 1) hi = count - 1;

    /* Draw far-to-near: anything with abs(dist) >= 1 first, then center,
       so the center icon visually overlaps neighbours at the moment they
       cross under it. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = lo; i <= hi; i++) {
            float dist = (float)i - v_cursor;       /* -1..0..+1 typical, can be -1.5..+1.5 during anim */
            float adist = fabsf(dist);
            if (adist > 2.0f) continue;             /* out of carousel reach */

            /* Pass 0 = sides, Pass 1 = center. Centre = adist < 0.5. */
            bool is_center_pass = adist < 0.5f;
            if (pass == 0 && is_center_pass) continue;
            if (pass == 1 && !is_center_pass) continue;

            /* Interpolation factor c in [0..1] where 1 = full center, 0 = full side */
            float c = 1.0f - fminf(adist, 1.0f);    /* clamp at adist=1 → c=0 (side) */
            /* Beyond adist=1 (only happens transiently during anim) we keep
               shrinking by extrapolating c into negative; clamp to 0. */
            if (c < 0.0f) c = 0.0f;

            int size_px = (int)((1.0f - c) * (float)icon_s + c * (float)icon_c);
            uint8_t alpha;
            {
                float side_a = (float)ss->launcher.coverflow_side_alpha;
                alpha = (uint8_t)((1.0f - c) * side_a + c * 255.0f);
            }
            int cx = cx0 + (int)(dist * (float)spacing);

            /* Resolve icon code for this entry */
            const jw_flat_item *it = &state->flat_items[i];
            const char *code;
            if (it->kind == JW_FLAT_SYSTEM)      code = state->systems[it->system_idx].name;
            else if (it->kind == JW_FLAT_TOOLS)  code = "_tools";
            else                                 continue; /* coverflow only handles systems + tools */

            int tw, th;
            SDL_Texture *tex = jw__load_system_icon(code, &tw, &th);
            if (!tex) continue;

            SDL_SetTextureAlphaMod(tex, alpha);
            jw__draw_image_fit(tex, tw, th,
                               cx - size_px / 2, cy - size_px / 2,
                               size_px, size_px);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }

    /* Label + game count for the active item.
       During animation we render the *target* item's label (state->list.cursor),
       not the visually-centre item — feedback that the press registered. */
    if (count > 0 && state->list.cursor < count) {
        const jw_flat_item *it = &state->flat_items[state->list.cursor];
        const char *label = jw__flat_label(state, state->list.cursor);
        int lw = cat_measure_text(label_font, label);
        int ly = cy + icon_c / 2 + CAT_S(20);
        cat_draw_text(label_font, label, (sw - lw) / 2, ly, theme->text);

        if (it->kind == JW_FLAT_SYSTEM) {
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "%d games",
                     state->systems[it->system_idx].game_count);
            int cw = cat_measure_text(small_font, cnt);
            cat_draw_text(small_font, cnt, (sw - cw) / 2,
                          ly + TTF_FontHeight(label_font) + CAT_S(6),
                          theme->hint);
        }
    }

    /* Tools / Settings overlays + footer — copy verbatim from
       jw__render_horizontal (main.c:866-901) without modification. */
    if (state->tools_open) jw__draw_tools_menu(state);
    if (jw_settings_ui_is_open(&state->settings)) {
        /* identical to horizontal's settings overlay block */
    } else {
        cat_footer_item footer[] = {
            { CAT_BTN_LEFT, "Navigate", false, "\xe2\x86\x90\xe2\x86\x92" },
            { CAT_BTN_A,    "Select",   false, "A" },
            { CAT_BTN_X,    "Search",   false, "X" },
            { CAT_BTN_MENU, "Menu",     false, "H" },
            { CAT_BTN_Y,    "Rescan",   false, "Y" },
        };
        cat_draw_footer(footer, 5);
    }
    cat_present();
}
```

#### 3.2.6 Dispatch + list build

Find the render dispatch (search for `case CAT_LAUNCHER_HORIZONTAL` or where `jw__render_horizontal` is called from the main loop). Add a case:

```c
case CAT_LAUNCHER_COVERFLOW:
    jw__render_coverflow(state);
    break;
```

Find the list builder dispatch (the place that chooses `jw__build_flat_list` vs `jw__build_carousel_list`). Coverflow uses the same shape as Horizontal:

```c
if (layout == CAT_LAUNCHER_HORIZONTAL || layout == CAT_LAUNCHER_COVERFLOW)
    jw__build_carousel_list(state);
else
    jw__build_flat_list(state);
```

Confirm by grep: `grep -n "build_carousel_list\|build_flat_list" cmd/jawaka-launcher/main.c`.

### 3.3 New theme directory

Create `Jawaka/res/themes/Jawaka-Coverflow/` with two files (the icons themselves come from the fetch script in 3.4):

**`stylesheet.json`**:

```json
{
  "ui": {
    "ui_font": { "path": "fonts/SpaceGrotesk/SpaceGrotesk-Regular.ttf", "size": 36 },
    "text_color": "#f0f0f0",
    "background_color": "#0a1428",
    "highlight_color": "#3b82f6",
    "highlight_text_color": "#ffffff",
    "disabled_color": "#6b7e9a",
    "tab_color": "#f0f0f070",
    "tab_selected_color": "#f0f0f0",
    "pill_radius_ratio": 0.15
  },
  "status_bar": { "show_battery_level": true, "show_clock": true, "show_wifi": true, "text_color": "#f0f0f0" },
  "button_hints": {
    "button_a_color": "#3b82f6",
    "button_b_color": "#3b82f6",
    "button_x_color": "#3b82f6",
    "button_y_color": "#3b82f6",
    "button_bg_color": "#152844",
    "button_text_color": "#ffffff",
    "text_color": "#f0f0f0"
  },
  "menu": { "background_color": "#0a1428" },
  "cjk_font": { "path": "fonts/SourceHanSansCN/SourceHanSansCN-Heavy.otf", "size": 36 },
  "launcher": {
    "layout": "coverflow",
    "coverflow_icon_size": 256,
    "coverflow_side_size": 160,
    "coverflow_spacing": 280,
    "coverflow_side_alpha": 140,
    "coverflow_anim_ms": 180
  }
}
```

Colours chosen to evoke the reference screenshot's navy backdrop with cool-blue highlights. Tunable later.

**`LICENSE-ASSETS.md`**:

```markdown
# Asset attribution

The console icons in `system_icons/` (excluding `_tools.png` and `_default.png`)
are derived from the libretro Systematic asset pack:

  Source:  https://git.libretro.com/libretro-assets/retroarch-assets
  Path:    xmb/systematic/
  Commit:  e11d6708b49a893f392b238effc713c6c7cfadef
  Licence: CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/)

Original authors: libretro team and contributors. Files were renamed from
their libretro display names to Jawaka short codes during import — see
`scripts/fetch-coverflow-icons.sh` for the mapping.

`_tools.png` and `_default.png` are original work, MIT-licensed under the
top-level Jawaka LICENSE.
```

Top-level `README.md` gets a one-line note in its assets / attribution section pointing here.

### 3.4 Asset fetch script

**`scripts/fetch-coverflow-icons.sh`** — committed alongside source, idempotent, safe to re-run:

```bash
#!/usr/bin/env bash
set -euo pipefail

# Fetches libretro Systematic console icons at a pinned commit and renames
# them to Jawaka system short codes. Safe to re-run; overwrites existing
# files. Requires curl.

REPO_RAW="https://git.libretro.com/libretro-assets/retroarch-assets/-/raw"
COMMIT="e11d6708b49a893f392b238effc713c6c7cfadef"
BASE="$REPO_RAW/$COMMIT/xmb/systematic/png"

DEST="$(cd "$(dirname "$0")/.." && pwd)/res/themes/Jawaka-Coverflow/system_icons"
mkdir -p "$DEST"

# Mapping: jawaka_code -> libretro filename (no .png suffix)
declare -a MAPPING=(
  "GB|Nintendo - Game Boy"
  "GBC|Nintendo - Game Boy Color"
  "GBA|Nintendo - Game Boy Advance"
  "FC|Nintendo - Nintendo Entertainment System"
  "SFC|Nintendo - Super Nintendo Entertainment System"
  "NDS|Nintendo - Nintendo DS"
  "MD|Sega - Mega Drive - Genesis"
  "MS|Sega - Master System - Mark III"
  "GG|Sega - Game Gear"
  "LYNX|Atari - Lynx"
  "NEOGEO|SNK - Neo Geo"
  "PCE|NEC - PC Engine - TurboGrafx 16"
  "PS|Sony - PlayStation"
  "ARCADE|MAME"
)

for entry in "${MAPPING[@]}"; do
  code="${entry%%|*}"
  source_name="${entry#*|}"
  url="$BASE/$(printf '%s' "$source_name" | sed 's/ /%20/g').png"
  out="$DEST/$code.png"
  echo "→ $code ← $source_name"
  curl --fail --silent --show-error -L "$url" -o "$out"
done

echo "Done. ${#MAPPING[@]} icons in $DEST"
echo "Note: _tools.png and _default.png are hand-authored and shipped in the repo."
```

Add a one-line entry in the top-level `README.md` Build section: *Run `./scripts/fetch-coverflow-icons.sh` once to populate console icons for the Coverflow theme.*

### 3.5 Hand-authored icons

Two PNGs the script does **not** fetch, both authored by the project:

- `Jawaka/res/themes/Jawaka-Coverflow/system_icons/_default.png` — generic console silhouette or puzzle-piece, 256×256, transparent background. Used as fallback for PORTS and any unmapped system.
- `Jawaka/res/themes/Jawaka-Coverflow/system_icons/_tools.png` — a 2×2 dotted grid icon or gear, 256×256, transparent, monochrome white. Renders well at both center and side sizes.

If the implementing LLM cannot generate raster art, ship a minimal SVG of each next to the PNG and document a one-line `rsvg-convert` step in `scripts/fetch-coverflow-icons.sh` to rasterise them. Either path works as long as both `_default.png` and `_tools.png` end up in `system_icons/`.

## 4. Render geometry reference

For maintenance — what determines the visible neighbour count is **`spacing` relative to screen width**, not a "neighbours_visible" config. With defaults:

- Screen 1024px wide, `spacing = 280` → centres at `cx0 - 280`, `cx0`, `cx0 + 280`
- A `coverflow_side_size = 160` icon spans roughly 80px each side of its centre
- So the left neighbour's right edge sits at `cx0 - 280 + 80 = cx0 - 200`, well clear of the centre icon's left edge at `cx0 - 128` — they don't overlap visibly
- Items beyond ±1 only become relevant during animation (briefly visible at the edge during the slide); a third out-of-range item never reaches the visible viewport on a 1024-wide screen

If a smaller device (480×320 MLP1) makes spacing tight, theme author can override `coverflow_spacing` and `coverflow_side_size` in the device-specific stylesheet.

## 5. Verification

1. **Build**: `cd Jawaka && make` — no new warnings. The struct addition is backwards-compatible (defaults init handle existing themes).
2. **Fetch assets**: `./scripts/fetch-coverflow-icons.sh` — confirms 14 PNGs land in `res/themes/Jawaka-Coverflow/system_icons/`. Confirm `_tools.png` and `_default.png` are already present (hand-authored).
3. **Theme picker**: Boot launcher → Settings → Theme. Confirm `Jawaka-Coverflow` appears in the list (discovered automatically by `cat_stylesheet_available_themes`).
4. **Visual checks**:
   - Centre icon ~256px, full system label below in EXTRA_LARGE font, "N games" subtitle below in SMALL font
   - One neighbour each side, ~160px, ~55% alpha
   - Background is solid navy from `background_color`
   - Tools tile at the end of the carousel shows `_tools.png`, not text
   - A system with no matching PNG falls back to `_default.png` (delete `GB.png` temporarily to verify)
5. **Animation checks**:
   - Press right once: smooth ~180ms slide; centre icon shrinks + fades into side position while the right neighbour grows + brightens into centre. Game-count subtitle updates instantly to the new system.
   - Mash right rapidly: each press restarts the tween from the current animated position — icons never rubber-band back, never queue up.
   - Press left at the boundary (cursor = 0): nothing happens, no animation start. Same on the right edge.
6. **No regressions**: switch back to `Jawaka-Horizontal`, `Jawaka-Vertical`, `Jawaka-Tabs` — each renders unchanged. The shared touch points are the layout enum and the launcher struct, both backwards-compatible.
7. **Input mode**: in coverflow, A enters the selected system → game browser (same as Horizontal); B from game browser returns to coverflow with cursor on the just-exited system; H opens settings overlay; Y triggers rescan; X opens search. All inherited from existing logic — verify nothing was bypassed.

## 6. Out of scope

- **Per-game coverflow within a system.** Coverflow is system-level only. The game browser keeps its existing layout.
- **3D perspective / floor reflection.** The alpha+size tween approximates depth well enough; reflection/perspective transforms are a v2.
- **Per-system wallpapers.** Background stays solid; if a user wants a backdrop they set the theme-level `wallpaper` field.
- **DB-stored icon paths.** `jw_system_entry` is not extended — the loader resolves icons by filesystem convention, not DB lookup.
- **Animating cursor on entry/exit of the launcher.** Initial render snaps to the persisted cursor; no slide-in animation.
- **Carousel wrap-around.** Cursor clamps at 0 and `flat_count - 1` like Horizontal does today.

## 7. Sequence for the implementing LLM

Recommended order so each step compiles cleanly:

1. Catastrophe enum value + struct fields + defaults + JSON parser (3.1).
2. Public accessors `cat_get_active_theme_dir` / `cat_get_active_theme_name` (3.1).
3. Build Jawaka — confirm no breakage; existing themes still work.
4. Create the theme directory and stylesheet.json (3.3) — switch to it and confirm the launcher cleanly falls back to no rendering (or whatever the unknown-layout default does) without crashing.
5. Add `jw__load_system_icon`, animation struct, helpers, and `jw__render_coverflow` (3.2). At this point only `_default.png` exists; verify it renders.
6. Add the dispatch case + list-builder branch (3.2.6). Launcher now renders coverflow with placeholder icons.
7. Author `_tools.png` and `_default.png` (3.5).
8. Write and run `scripts/fetch-coverflow-icons.sh` (3.4). Commit the resulting PNGs and `LICENSE-ASSETS.md`.
9. Walk through all verification steps in section 5.

## 8. Files summary

| Path | Change |
|---|---|
| `Catastrophe/include/catastrophe.h` | Enum value + struct fields + defaults + JSON parse + 2 public accessors |
| `Jawaka/cmd/jawaka-launcher/main.c` | New: `jw__load_system_icon`, `jw_coverflow_anim` struct + 3 helpers, `jw__render_coverflow`. Edit: input handler, render dispatch, list-builder dispatch |
| `Jawaka/res/themes/Jawaka-Coverflow/stylesheet.json` | New |
| `Jawaka/res/themes/Jawaka-Coverflow/LICENSE-ASSETS.md` | New |
| `Jawaka/res/themes/Jawaka-Coverflow/system_icons/*.png` | 14 PNGs from script + 2 hand-authored |
| `Jawaka/scripts/fetch-coverflow-icons.sh` | New, executable |
| `Jawaka/README.md` | One-line note pointing at the fetch script |
