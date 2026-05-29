# Jawaka — Phase 1.5 implementation plan

Phase 1.5 is a focused navigation layer on top of Phase 0+1. Phase 0+1 proved
the daemon/launcher/menu architecture is real. Phase 1.5 makes the launcher
actually navigable by wiring up tab switching, page scrolling, and alphabet
jumping through Catastrophe's existing input system.

Working directory: `/Volumes/Storage/UMRK/Jawaka/`.

---

## 1. Mission

Make the tab bar (Recents, Games, Apps, Settings) interactive. The user should
be able to:

- switch between tabs with L2/R2 (`;` / `t` on keyboard, shoulder triggers on
  controller), with wrap-around
- scroll through tab content with UP/DOWN (list items) and LEFT/RIGHT (page up/
  page down by one screen-full)
- jump to the next/previous alphabetical letter in the current list with
  L1/R1 (`l` / `r`)
- see a stub layout per tab: empty-state Recents, the current systems list for
  Games, an app list for Apps, and stub settings for Settings

No changes to `jawakad` or `jawaka-menu` — this phase touches only
`cmd/jawaka-launcher/main.c`.

---

## 2. Hard constraints

1. **No daemon changes.** The IPC contract stays as-is. Tab state is
   launcher-local; the daemon knows nothing about it.
2. **No Catastrophe widget additions.** Everything uses the existing
   `cat_poll_input`, `cat_button`, and drawing primitives that Phase 0+1
   already depends on.
3. **No new files.** All changes land in `cmd/jawaka-launcher/main.c`. This
   keeps the diff reviewable and avoids premature module splitting.
4. **No `internal/` changes.** `db.h` already exposes `jw_db_list_systems()`
   and `jw_db_read_summary()`. Phase 1.5 queries the same DB for apps via the
   existing daemon `scan-library` IPC flow and reads the SQLite DB directly
   for app listing (since the launcher already opens the DB path — or it
   should; verify this).
5. **Phase 1.5 does not add new IPC messages.** The existing `scan-library`
   response already includes system + game + app counts; the launcher reads
   the DB file directly on the same path to enumerate apps.

---

## 3. Input mapping (definitive)

| Input | Keyboard | Controller | Action |
|-------|----------|------------|--------|
| UP | `↑` | D-pad up | Previous item in list |
| DOWN | `↓` | D-pad down | Next item in list |
| LEFT | `←` | D-pad left | Page up in list (scroll by `visible_rows`) |
| RIGHT | `→` | D-pad right | Page down in list (scroll by `visible_rows`) |
| L1 | `l` | L1 shoulder | Previous alphabetical letter in list |
| R1 | `r` | R1 shoulder | Next alphabetical letter in list |
| L2 | `;` | L2 trigger | Previous tab (wraps) |
| R2 | `t` | R2 trigger | Next tab (wraps) |
| A | `a` | A button | Select current item (stub: "Coming soon") |
| B | `b` | B button | Back / shutdown request |
| Menu | `h` | Menu | Open menu via daemon |
| Y | `y` | Y button | Rescan library |

Tabs wrap: `Recents → Games → Apps → Settings → Recents` and reverse.

---

## 4. Tab layouts

### 4.1 Tab state

Replace the hardcoded `JW_TAB_GAMES` with a `current_tab` field in
`jw_launcher_state`. The tab bar renders tabs from `kTabs[]`, with the active
tab highlighted and underlined.

```c
typedef enum {
    JW_TAB_RECENTS = 0,
    JW_TAB_GAMES,
    JW_TAB_APPS,
    JW_TAB_SETTINGS,
    JW_TAB_COUNT
} jw_tab;
```

### 4.2 Recents tab

Render an empty-state layout with the text "No recent games" centered in the
content area. No list items. This layout stubs the eventual recents carousel
or list that Phase 4 will populate. Status bar/footer still render.

### 4.3 Games tab

The existing two-panel layout (systems list + art placeholder) stays as-is
but becomes the Games tab's view. UP/DOWN scroll systems. LEFT/RIGHT page
through the systems list (same `jw__move_cursor` pattern but by
`visible_rows` instead of 1). L1/R1 jump alphabetically.

L1/R1 letter-jumping: scan the current list of system names, find the next
system whose name starts with a different first letter than the currently
selected item (moving forward/backward). If the current item is `"Game Boy"`,
pressing R1 jumps to the first system starting with `"G"` after `"Game Boy"`
(or the next different letter). Pressing L1 jumps backward analogously.

Since systems currently sort by their folder code (e.g. `ARCADE`, `FC`, `GBA`,
`GB`...), letter jumping should work over the display-name order.

### 4.4 Apps tab

Query the SQLite DB directly for app entries from the `apps` table. Render a
simple list (same pill style as the systems list). Each row shows the app
`name`. The right panel (art area) shows the app name centered, or the app's
icon path if populated.

`jw_db_list_apps()` — add this function to `internal/db/db.h` and `db.c`:

```c
int jw_db_list_apps(const char *db_path, jw_app_entry *out, int max, int *count);
```

Where `jw_app_entry` is:

```c
typedef struct {
    char name[256];
    char pak_dir[512];
    char icon[256];
} jw_app_entry;
```

This is an exception to the "no internal changes" rule — it's a trivial
query wrapper following the existing `jw_db_list_systems()` pattern.

### 4.5 Settings tab

Render a short list of stub settings items:

1. **Rescan Library** — triggers `scan-library` IPC
2. **Return to Menu** — sends `open-menu` IPC (hides launcher)
3. **Shutdown** — sends `shutdown` IPC

Each item is rendered as a pill (same style as systems list). Pressing A
activates the selected item. The list replaces the art panel area and uses
the full content width.

---

## 5. Module changes

### 5.1 `cmd/jawaka-launcher/main.c`

Add to state:

```c
typedef enum { ... } jw_tab;

typedef struct {
    jw_tab               current_tab;
    jw_library_summary   summary;
    jw_system_entry      systems[JW_MAX_SYSTEMS];
    int                  system_count;
    int                  cursor;           /* Index within current tab's list */
    int                  scroll_offset;
    int                  visible_rows;
    jw_app_entry         apps[JW_MAX_SYSTEMS]; /* reuse MAX_SYSTEMS; apps << systems in practice */
    int                  app_count;
    char                 status[256];
    bool                 scan_ready;
} jw_launcher_state;
```

New input handlers:

- `jw__switch_tab(state, direction)`: change `current_tab`, reset cursor/
  scroll_offset to 0.
- `jw__page_cursor(state, delta)`: move by `visible_rows` instead of 1.
- `jw__jump_letter(state, direction)`: find next/prev system name with a
  different initial letter.
- `jw__render_tab(state)`: dispatch to `jw__render_recents`,
  `jw__render_games`, `jw__render_apps`, `jw__render_settings`.
- `jw__activate_tab_item(state)`: stub action per tab (toast "Coming soon"
  for Games/apps, activate action for Settings items).

The existing `jw__move_cursor` and `jw__render_launcher` functions are
refactored to be tab-aware.

### 5.2 `internal/db/db.h` + `db.c`

Add:

```c
typedef struct {
    char name[256];
    char pak_dir[512];
    char icon[256];
} jw_app_entry;

int jw_db_list_apps(const char *db_path, jw_app_entry *out, int max, int *count);
```

Implementation queries `SELECT name, pak_dir, icon FROM apps ORDER BY name`.

---

## 6. Render flow

The render function now:

1. Draw the tab bar header (same as before, but active tab is dynamic)
2. Draw the footer (may vary slightly per tab, e.g. Settings shows "Select"
   instead of "Coming soon")
3. Draw tab-specific content area via dispatch

The tab bar drawing loop uses `kTabs[i]` with `active = (i == state->current_tab)`.

---

## 7. Acceptance criteria

1. `make` compiles with zero warnings.
2. `make run-daemon-interactive` lets the user:
   - Press `t`/`;` to cycle through all 4 tabs (wrapping)
   - See different content per tab
   - Navigate systems on the Games tab with UP/DOWN
   - Page through systems with LEFT/RIGHT
   - Jump alphabetically with L1/R1 (`l`/`r`)
   - See a "No recent games" empty state on Recents
   - See app entries listed on Apps tab
   - See 3 stub settings items on Settings tab
   - Activate a settings item with A
   - Open menu with `h`, return with B
3. Tab bar visually indicates the active tab.
4. Tabs wrap: pressing `;` (L2) on Recents goes to Settings; pressing `t` (R2)
   on Settings goes to Recents.

---

## 8. Explicitly out of scope

- Game list view (drilling into a system's games) — Phase 3
- Recents tracking and data — Phase 4
- Favorites — Phase 4
- Settings persistence — Phase 5
- Search UX — Phase 2/3
- Artwork loading — Phase 2
- `jawakad` or `jawaka-menu` changes
- New IPC messages
- Theme/stylesheet changes
- Catastrophe widget additions (lists, etc.)
