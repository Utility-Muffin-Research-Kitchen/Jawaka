# Jawaka — settings implementation plan

This is an executable plan for a coding agent. It covers the first real settings
slice for Jawaka: build the long-term settings structure now, but only fully
wire up theme selection in the first milestone.

Working directory: `/Volumes/Storage/UMRK/Jawaka/`.

Reference material:

- Jawaka roadmap: `docs/PLAN.md`
- Jawaka architecture constraints: `docs/ARCHITECTURE_DECISIONS.md`
- Current launcher/menu code:
  - `cmd/jawaka-launcher/main.c`
  - `cmd/jawaka-menu/main.c`
- Current DB/schema code:
  - `internal/db/db.c`
  - `internal/db/db.h`
- Current path helpers:
  - `internal/platform/paths.c`
- Theme assets:
  - `res/themes/Jawaka-Tabs/`
  - `res/themes/Jawaka-Vertical/`
  - `res/themes/Jawaka-Horizontal/`
- Allium inspiration:
  - launcher-owned settings, category list with child screens
  - inline theme selector rather than a separate theme app

---

## 1. Mission

Replace Jawaka's current stub Settings surface with a real, launcher-owned,
Allium-inspired settings flow that:

- lives in `jawaka-launcher`, not `jawaka-menu`
- uses a category list with child screens
- persists launcher settings in the existing SQLite DB under `.jawaka/`
- supports immediate theme switching
- keeps the selected theme shared across launcher and menu

The first milestone is intentionally narrow:

- build the settings architecture now
- wire only the `Theme` setting for real
- leave future settings categories in place as real navigable placeholder screens

This work should leave Jawaka with a credible settings foundation without
overcommitting to device-owned settings such as Wi-Fi, clock, power, or system
updates.

---

## 2. Locked decisions

These decisions are already settled and should not be re-litigated while
implementing this plan.

1. **Full settings live in `jawaka-launcher` only.**
   `jawaka-menu` remains a quick-action surface, not a second full settings app.

2. **Build the long-term settings structure now.**
   Do not ship a one-off theme picker that will need to be thrown away.

3. **Use an Allium-style settings home with child screens.**
   Top-level settings is a category list; selecting a category opens a child
   screen.

4. **Top-level settings categories are:**
   - `Appearance`
   - `Library`
   - `Behavior`
   - `About`

5. **Only `Appearance` is functionally wired in the first milestone.**
   `Library`, `Behavior`, and `About` open real placeholder child screens.

6. **Only one real setting exists initially: `Theme`.**
   Do not wire wallpaper, font, localization, box-art width, status-bar
   toggles, or other appearance controls in this milestone.

7. **Theme options are limited to the three first-party Jawaka themes:**
   - `Jawaka-Tabs`
   - `Jawaka-Vertical`
   - `Jawaka-Horizontal`

8. **Theme changes apply immediately and persist immediately.**
   No separate Apply/Save button.

9. **The selected theme is shared across launcher and menu.**
   `jawaka-menu` should load the same persisted theme as the launcher.

10. **For now, theme selection also selects layout.**
    These three Jawaka themes already bundle both styling and launcher layout.
    Do not split "theme" and "layout" into separate settings yet.

11. **The `Theme` control is an inline selector row.**
    Do not open a separate full-screen theme picker.

12. **Inline theme input behavior is fixed:**
    - LEFT/RIGHT cycles theme choices
    - `A` also advances to the next theme for convenience

13. **After a live theme/layout change, keep the user inside**
    `Settings > Appearance > Theme`.

14. **Preserve in-session settings cursor state.**
    Backing out of a child screen returns to the previously selected category.

15. **Launcher settings persist in SQLite. `JAWAKA_THEME` is an override.**
    Precedence is `env > DB > default`. Setting `JAWAKA_THEME` always wins so
    the dev workflow (`JAWAKA_THEME=Jawaka-Vertical make run-interactive`)
    keeps working after the DB has a persisted value. When env is unset, the
    DB value wins; when both are unset, fall back to `Jawaka-Tabs`.

16. **`B` is always "back" while Settings is open.**
    The launcher's `B = Shutdown` binding is suppressed while
    `settings.open == true`. `B` on Settings home closes Settings and
    returns to the launcher main view. There is no "Exit Settings" row.

17. **Horizontal mode reaches Settings through the Tools tile.**
    The end-of-carousel Tools tile already opens an overlay sub-menu with
    Recents / Favorites / Apps / Settings. The Settings item in that overlay
    opens the same settings flow as the tabbed and vertical entry points.

18. **Hot reload is limited to colors, layout, tab/footer styling, and
    wallpaper.** `cat_stylesheet_apply` does **not** reload fonts (would
    segfault per Catastrophe's own comment). All three Jawaka themes share
    Space Grotesk so this is fine in this milestone. A future theme with a
    different font would require a launcher restart, which is acceptable.

19. **`FORCE=1` mock regeneration wipes persisted settings.**
    `scripts/mockgen.sh` only deletes `$JAWAKA_SDCARD_ROOT` when `FORCE=1`.
    Default `make run-*` invocations preserve the DB and the saved theme.
    Wiping on `FORCE=1` is intentional and acceptable.

---

## 3. Current codebase starting point

### 3.1 Launcher

`cmd/jawaka-launcher/main.c` already contains:

- a `Settings` tab in tabbed mode
- a `Settings` row in vertical/horizontal flat navigation
- stub settings entries:
  - `Rescan Library`
  - `Return to Menu`
  - `Shutdown`
- startup theme loading from `JAWAKA_THEME`, defaulting to `Jawaka-Tabs`

Important current behavior:

- the active stylesheet is loaded once near startup
- layout mode is then derived from `cat_get_stylesheet()->launcher.layout`
- changing theme at runtime does **not** exist yet
- flat-layout settings currently resolve to `"Coming soon"`

### 3.2 Menu

`cmd/jawaka-menu/main.c` currently:

- does not load a Jawaka-specific theme explicitly
- uses Catastrophe defaults after `cat_init()`
- remains a minimal quick-action menu

### 3.3 Persistence

`internal/db/db.c` currently owns:

- schema application
- `games`, `apps`, `favorites`, `recents`, and `games_fts`
- no launcher settings table yet

`internal/platform/paths.c` currently ensures:

- runtime socket path under `/tmp/jawaka-$USER/`
- DB path under `<sdcard>/.jawaka/library.db`

That existing `.jawaka/` storage location is the correct home for launcher
settings too.

---

## 4. UX target

### 4.1 Top-level settings home

The Jawaka settings home should be a list of categories:

1. `Appearance`
2. `Library`
3. `Behavior`
4. `About`

Selecting one opens a child screen. `B` returns to the previous screen. When
the user backs out of a child screen, restore the previously selected category.

### 4.2 Appearance child screen

The first milestone's `Appearance` screen contains one inline setting row:

- `Theme`

The row should show the current theme value on the right.

Interaction:

- LEFT = previous theme
- RIGHT = next theme
- `A` = next theme
- `B` = back to the settings home

Changing theme should:

1. write the new theme to SQLite immediately
2. reload and apply the stylesheet immediately
3. rebuild layout-dependent launcher state
4. keep the user on the `Theme` row

### 4.3 Placeholder child screens

`Library`, `Behavior`, and `About` should open real screens, but with simple
placeholder content for now.

Their role in this milestone:

- prove the category-based settings architecture is real
- reserve the correct information architecture for future work
- avoid mixing unrelated launcher actions into the settings hierarchy

Suggested placeholder copy:

- `Library` — future content root / refresh / indexing settings
- `Behavior` — future launcher-owned navigation, display, and search settings
- `About` — future version/build/about information

### 4.4 What should no longer be the main Settings content

The old stub actions:

- `Rescan Library`
- `Return to Menu`
- `Shutdown`

should **not** remain as the main settings list.

Why:

- `Rescan Library` is already available from `Y`
- `Return to Menu` is already available from `H`
- `Shutdown` is already available from `B` or menu flow
- these are quick actions, not persistent launcher-owned settings

Leave those actions in their current quick-action paths; do not redesign them as
part of this milestone.

---

## 5. Persistence and schema plan

### 5.1 Add a generic settings table

Extend the authoritative schema string in `internal/db/db.c` with:

```sql
CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

Use a generic key/value table rather than a dedicated `theme_settings` table.
This is sufficient for the current milestone and gives future launcher settings
a home without schema churn.

### 5.2 Schema version

- bump `PRAGMA user_version` from `1` to `2`
- keep schema application idempotent
- do not add destructive migration logic

This repository is still early-stage and the mock SD-card DB can tolerate a
simple forward schema expansion.

### 5.3 Keys

Use a stable string key for theme:

- `theme_name`

Do not add speculative keys for future settings in this milestone.

### 5.4 DB helper surface

Add generic string-setting helpers to `internal/db/db.h` / `db.c`:

```c
int jw_db_get_setting(const char *db_path, const char *key,
                      char *out, size_t out_size);
int jw_db_set_setting(const char *db_path, const char *key, const char *value);
```

And one convenience helper for launcher startup:

```c
int jw_db_get_theme_name(const char *db_path, char *out, size_t out_size);
```

Recommended behavior:

- `jw_db_get_setting(...)`
  - returns `0` on successful query
  - writes empty string if the key does not exist
- `jw_db_set_setting(...)`
  - performs `INSERT INTO ... ON CONFLICT(key) DO UPDATE`
- `jw_db_get_theme_name(...)`
  - wraps `jw_db_get_setting(..., "theme_name", ...)`

Both helpers take a path and open/close their own connection. For a single
key/value write, the open/close overhead is microseconds and happens at most
once per LEFT/RIGHT press in the settings UI — the symmetry simplifies the
contract more than caching a handle saves.

---

## 6. Theme resolution rules

Theme resolution becomes (per locked decision 15):

1. `JAWAKA_THEME` environment variable — explicit override, always wins
2. persisted DB setting (`settings.key = 'theme_name'`)
3. hard default: `Jawaka-Tabs`

Use this same precedence in both:

- `cmd/jawaka-launcher/main.c`
- `cmd/jawaka-menu/main.c`

Important compatibility rule:

- env override is for the dev workflow (`JAWAKA_THEME=Jawaka-Horizontal make
  run-interactive`) and as an escape hatch when a persisted theme name has
  gone bad. The settings UI still writes the DB on change; the next launch
  without env honors the DB value.

---

## 7. New internal module split

Do not leave the entire settings system hardcoded in `cmd/jawaka-launcher/main.c`.
This feature is large enough to justify a real internal module.

Add a new module:

```text
internal/settings/
  settings.h
  settings.c
```

Responsibilities:

- settings screen/state model
- top-level category list
- child-screen state
- inline theme selector logic
- rendering helpers for settings home / appearance / placeholders
- bridging between launcher input and settings actions

This matches the existing long-term repository direction in `docs/PLAN.md`,
which already anticipates a `settings` module inside the shared Jawaka layer.

### 7.1 Suggested state model

Use a dedicated settings UI state owned by the launcher, for example:

```c
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
```

The struct name is `jw_settings_ui` to match the public API in section 11.3.
`placeholder_list` is shared by Library / Behavior / About because none of
them have real content yet; when one of those screens gains its own content,
give it its own `cat_list_state` field at that point.

Notes:

- `open` indicates whether the launcher is currently inside the settings flow
- `screen` indicates which child screen is active
- separate `cat_list_state` fields make cursor restoration simple
- `theme_index` should map directly to the three Jawaka themes

The exact struct names may change, but the implementation should preserve the
same responsibilities.

### 7.2 Theme catalog

Do **not** use `cat_stylesheet_available_themes()` for the first milestone's
user-facing list.

Instead, define a fixed first-party theme catalog:

```c
static const char *kJawakaThemes[] = {
    "Jawaka-Tabs",
    "Jawaka-Vertical",
    "Jawaka-Horizontal",
};
```

Reasons:

- only these themes are guaranteed to match Jawaka's current IA
- they are the only approved choices for this milestone
- Catastrophe's broader theme discovery can remain available for later work

Still validate selected themes by actually calling `cat_stylesheet_load_theme()`
before applying them.

---

## 8. Launcher integration plan

### 8.1 Replace current stub Settings behavior

In `cmd/jawaka-launcher/main.c`:

- remove the old `kSettingsItems[]` stub list as the primary settings content
- replace current Settings rendering with calls into `internal/settings/settings.c`
- route **three** entry points into the same settings flow:
  - tabbed mode: `JW_TAB_SETTINGS` activation
  - vertical mode: `JW_FLAT_SETTINGS` activation
  - horizontal mode: the `Settings` item inside the Tools tile overlay
    (currently `jw__draw_tools_menu()` index 3)

This ensures Settings behaves consistently across `Jawaka-Tabs`,
`Jawaka-Vertical`, and `Jawaka-Horizontal`.

### 8.2 Launcher-owned settings open state

The launcher should treat settings as a local sub-flow. Recommended model:

- entering Settings from a tab or flat row sets `settings.open = true`
- the active layout continues to own the outer chrome
- the settings module owns the inner content area and local input handling

This avoids special-casing settings as a separate process or daemon mode.

### 8.3 Layout-specific entry, shared settings content

The launcher has three layout modes:

- tabbed
- vertical
- horizontal

All three should reuse the same settings content module. The only layout-specific
difference should be how the user reaches Settings and how outer navigation is
suspended while the settings sub-flow is open.

### 8.4 Footer behavior

When settings is open, the footer hints should reflect settings navigation:

- Navigate
- Select / Change
- Back
- Menu (if appropriate to preserve current launcher convention)

Do not keep footer hints that imply `Rescan` or `Shutdown` as the primary
settings actions.

---

## 9. Live theme application plan

### 9.1 Apply path

When the user changes the theme from the inline row:

1. determine the next theme index
2. load the corresponding theme into a temporary `cat_stylesheet`
3. if load succeeds:
   - persist `theme_name` to SQLite
   - call `cat_stylesheet_apply(&ss)`
   - rebuild launcher layout-dependent state
4. if load fails:
   - leave the prior theme active
   - leave `theme_index` pointing at the prior theme (do not advance)
   - surface an explicit status message (`"theme load failed"`)

Do not write the DB first and then risk leaving the app in a bad visual state if
theme loading fails.

**Hot reload scope.** `cat_stylesheet_apply` reloads:

- colors (theme + stylesheet)
- layout mode (`cat_stylesheet.launcher.layout`)
- tab bar / footer styling
- wallpaper texture (if changed)

It does **not** reload fonts — per Catastrophe's own comment
(`catastrophe.h:1655`), TTF_CloseFont/OpenFont mid-execution causes segfaults.
All three Jawaka themes share `SpaceGrotesk-Regular.ttf`, so font hot reload
is unnecessary for this milestone. A future theme with a different font would
require a launcher restart; that's acceptable.

### 9.2 Rebuild requirements after a theme switch

Because theme selection also changes layout, the launcher must rebuild any state
that depends on `cat_get_stylesheet()->launcher.layout`.

At minimum, revisit:

- current layout enum
- flat/carousel navigation list generation
- visible row calculations
- list cursor clamping
- any derived sidebar/detail pane geometry that assumes the previous layout

Do **not** rerun the full library scan just because the theme changes.

### 9.3 Focus retention after rebuild

After applying a new theme:

- remain inside settings
- remain on the `Appearance` screen
- remain on the `Theme` row

The selected theme value should update immediately and the user should be able to
press LEFT/RIGHT/A again without re-entering the settings flow.

### 9.4 Status messaging

Use the existing launcher status line for concise feedback:

- `"theme: Jawaka-Vertical"`
- `"theme: Jawaka-Horizontal"`
- `"theme load failed"`

Keep messages short and consistent with current launcher status usage.

---

## 10. Menu integration plan

Update `cmd/jawaka-menu/main.c` so the menu loads the persisted theme on startup.

Recommended sequence:

1. resolve `socket_path`
2. perform `jw_ipc_hello(...)`
3. resolve `db_path`
4. `cat_init(...)`
5. resolve theme via DB -> env -> default
6. load/apply stylesheet
7. continue with normal render loop

This keeps launcher and menu visually coherent once the user changes theme.

Do **not** add a full settings UI to `jawaka-menu`.

---

## 11. Detailed file plan

### 11.1 `internal/db/db.c`

- add `settings` table to the schema string
- bump `user_version`
- add generic get/set helpers
- add theme convenience helper if useful

### 11.2 `internal/db/db.h`

- declare new setting helper APIs
- keep interfaces narrow and string-oriented for now

### 11.3 `internal/settings/settings.h`

Add the launcher-facing API surface, for example:

```c
typedef struct jw_settings_ui jw_settings_ui;

void jw_settings_ui_init(jw_settings_ui *ui, const char *db_path);
void jw_settings_ui_enter(jw_settings_ui *ui);
bool jw_settings_ui_is_open(const jw_settings_ui *ui);
void jw_settings_ui_render(jw_settings_ui *ui, const char *status);
int  jw_settings_ui_handle_button(jw_settings_ui *ui, cat_button button,
                                  char *status, size_t status_size,
                                  bool *theme_changed);
```

The exact function list may differ, but the interface should make it easy for
`jawaka-launcher/main.c` to delegate settings-specific work.

### 11.4 `internal/settings/settings.c`

Implement:

- category list rendering
- child-screen rendering
- inline theme selector row
- DB-backed theme load/save
- logic for cycling theme indices
- placeholder child screens

### 11.5 `cmd/jawaka-launcher/main.c`

- initialize the settings UI state
- load the starting theme using DB -> env -> default precedence
- integrate settings entry points from both tabbed and flat layouts
- route input to settings when settings is open
- rebuild layout-dependent state when `theme_changed` is signaled

### 11.6 `cmd/jawaka-menu/main.c`

- resolve `db_path`
- load/apply the persisted theme before rendering

### 11.7 `README.md`

Update if needed to mention:

- launcher theme now persists in `.jawaka/library.db`
- `JAWAKA_THEME` remains a fallback

Only update docs if the resulting behavior would otherwise be surprising.

---

## 12. Suggested implementation order

1. **DB foundation**
   - add schema table and helper functions
   - compile before touching launcher UI

2. **Theme resolution helper**
   - centralize DB -> env -> default logic
   - use it in launcher startup first

3. **Internal settings module**
   - build settings state structs
   - build home screen + child screen scaffolding

4. **Appearance screen**
   - add the inline `Theme` row
   - wire LEFT/RIGHT/A theme cycling

5. **Live theme apply**
   - apply stylesheet in-place
   - rebuild layout-dependent launcher state
   - preserve settings focus

6. **Menu theme loading**
   - teach `jawaka-menu` to load the persisted theme

7. **Placeholder screens + polish**
   - `Library`, `Behavior`, `About`
   - footer/status cleanup

8. **Documentation touch-up**
   - update README only if needed

---

## 13. Acceptance criteria

The task is complete when all of the following are true:

1. `jawaka-launcher` no longer uses the old three-item stub settings list as its
   main Settings content.

2. Entering Settings opens a category list with:
   - `Appearance`
   - `Library`
   - `Behavior`
   - `About`

3. Selecting `Appearance` opens a child screen containing a `Theme` row.

4. The `Theme` row:
   - shows the current theme name
   - cycles through `Jawaka-Tabs`, `Jawaka-Vertical`, `Jawaka-Horizontal`
   - responds to LEFT/RIGHT
   - also advances on `A`

5. Changing the theme:
   - persists immediately in SQLite
   - applies immediately without restarting the launcher
   - changes the launcher layout when switching among the three Jawaka themes
   - keeps the user inside `Settings > Appearance > Theme`
   - does **not** trigger a library rescan

6. Exiting and reopening the launcher restores the chosen theme (DB > default).
   `JAWAKA_THEME=Jawaka-X make run-interactive` still overrides the DB value
   (env > DB > default) — this developer workflow must keep working.

7. Opening `jawaka-menu` after a theme change uses the same theme.

8. Backing out of a child settings screen restores the previous category
   selection rather than resetting to the top.

9. `Library`, `Behavior`, and `About` are real navigable child screens, even if
   their bodies are placeholder content. Each shows the screen name as title
   and one centered subdued "Coming soon" line. Footer hint: `B Back`.

10. Existing quick actions still work through their current paths when
    Settings is **closed**:
    - `Y` rescans
    - `H` opens menu
    - `B` shuts down (current behavior on launcher main view)

11. While Settings is **open** at any depth, `B` is always "back" and never
    triggers Shutdown. `B` on Settings home closes Settings and returns to
    the launcher main view.

12. Cycling theme wraps: LEFT past the first theme wraps to the last; RIGHT
    past the last wraps to the first.

13. If the active theme at startup is not in `kJawakaThemes` (e.g.
    `JAWAKA_THEME=Catastrophe-Demo`), the launcher still starts in that
    theme, but the settings UI displays `theme_index = 0` (Jawaka-Tabs) until
    the user cycles, at which point the standard 3-theme cycle resumes.

---

## 14. Explicitly out of scope

- Wi-Fi settings
- clock/date/time settings
- power or brightness settings
- OTA/system update settings
- localization/language settings
- font selection
- wallpaper selection
- status-bar visibility toggles
- search tuning
- content-root configuration
- emulator/core/RetroArch settings ownership
- moving settings into a separate process
- exposing all discoverable Catastrophe themes
- splitting theme and layout into separate end-user settings

Those can be layered on later once the settings architecture and DB-backed
launcher preferences are in place.

---

## 15. Notes for the coding agent

- Read the current launcher's Settings code carefully before editing; there is
  already a tabbed-mode and flat-mode entry point to unify.
- Prefer extracting settings behavior into `internal/settings/` rather than
  growing `cmd/jawaka-launcher/main.c` into another large monolith.
- Reuse Catastrophe list-state and drawing primitives already adopted by Jawaka.
- Do not redesign daemon IPC as part of this work.
- Do not let the theme setting accidentally trigger a library rescan.
- Preserve current manual workflows by keeping `JAWAKA_THEME` as a fallback path.

## About page (done, 2026-06-01)

Settings → About is a real page (was a placeholder). Three sections in one
scroll view: **System** (Stock OS via `/loong/loong_version`, kernel, device,
memory, SD free, IP, battery, CPU temp, uptime), **Library** (game/system/app
counts from `jw_db_read_summary`), and **Open-source components** (licenses).

- Lower-layer facts come from a new platform helper: `jw_system_info` +
  `jw_platform_system_info()` in `platform_id_mlp1.c` (pure libc/sysfs — also
  compiles into jawakad), with a `platform_id_mock.c` stub for desktop.
- Built on the new Catastrophe `cat_draw_scroll_view` (non-selectable scroll).
  Long values marquee instead of truncating: device-info loops, components
  ping-pong (`cat_marquee.mode`).
- NOTE: the page is arguably overdone for an About screen — a future pass may
  strip it back (fewer live fields, or drop the per-cell marquee).
