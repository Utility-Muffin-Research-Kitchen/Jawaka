# Jawaka — refactor, launcher layouts, bundled fonts

Three independent workstreams, ordered by what makes the others easier:

1. Audit Jawaka↔Catastrophe duplication and push reusable UI into Catastrophe.
2. Add 3 launcher layout modes (tabs / vertical / horizontal), driven by themes.
3. Bundle Space Grotesk + MiSans (verify licences first).

Doing (1) before (2) keeps the new layouts from being built on top of code that's about to move.

---

## Part 1 — Duplication audit and Catastrophe refactor

### 1.1 What's currently duplicated

Audit of `Jawaka/cmd/jawaka-launcher/main.c` (800 LoC) and `Jawaka/cmd/jawaka-menu/main.c` (302 LoC):

| # | Pattern | Launcher | Menu | Lives where now | Move to |
|---|---|---|---|---|---|
| A | macOS NSApp activation (`jw__platform_activate`) | L17–31 | L15–29 | both binaries (duplicated verbatim) | **Catastrophe** (`cat_activate_window`) |
| B | env helpers (`jw__env_flag`, `jw__env_u32`) | L70–81 | L47–58 | both binaries | **Jawaka internal** (`internal/core/env.h/c`) |
| C | JSON request/response (`jw__request_json`) | L83–100 | L60–77 | both binaries | **Jawaka internal** (`internal/ipc/ipc_client.h/c`) |
| D | `hello` handshake (`jw__send_hello`) | L102–115 | L79–92 | both binaries (only `role` differs) | **Jawaka internal** ipc_client |
| E | `scan-library` request + summary parse | L127–160 | L94–122 | both binaries | **Jawaka internal** ipc_client |
| F | `shutdown` / `open-menu` daemon RPCs | L162–180 | L124–132 | both binaries | **Jawaka internal** ipc_client |
| G | Tab bar drawing (`jw__draw_tab_bar`) | L306–326 | — | launcher only | **Catastrophe** (`cat_draw_tab_bar`) |
| H | Non-blocking list pane + selection pill + scrollbar (`jw__draw_list_items`) | L328–353 | — | launcher only | **Catastrophe** (`cat_draw_list_pane`) |
| I | List+art two-pane layout (45% / 55%) | L408–468, L489–549 | — | launcher only (games/apps) | **Catastrophe** (`cat_draw_list_detail_layout` helper, optional) |
| J | Cursor/scroll state math (`jw__move_cursor`, `jw__page_cursor`, `jw__jump_letter`) | L191–271 | L190–197 | both binaries (menu is a subset) | **Catastrophe** (`cat_list_state` + `cat_list_state_move/page/jump_letter`) |
| K | Autodemo (`JAWAKA_AUTODEMO*`) | L769–791 | L273–294 | both binaries | **Jawaka internal** (`internal/core/autodemo.h/c`) — Catastrophe shouldn't know about Jawaka env vars |
| L | Main-loop skeleton (init Catastrophe, raise+activate, poll input, render, quit) | L734–800 | L239–301 | both binaries | **Jawaka internal** `ui_shell.h/c` (thin wrapper); keep Catastrophe agnostic |

**Net target after refactor**: `jawaka-menu/main.c` shrinks roughly 302 → ~120 LoC, `jawaka-launcher/main.c` shrinks ~800 → ~400 LoC (mostly tab/games/apps/settings render bodies and per-tab input).

### 1.2 New Catastrophe surface

Two new entries in `catastrophe.h` (core) and the rest in `catastrophe_widgets.h` (widgets) — decided 2026-05-15:

```c
/* catastrophe.h additions */
void  cat_activate_window(void);      /* no-op outside macOS; raises + activates */
void  cat_draw_tab_bar(const char *const *labels, int count, int active_index);
/* SDL_RenderGeometry-backed quad blit, for the horizontal mode parallelogram carousel */
void  cat_draw_textured_parallelogram(SDL_Texture *tex, const SDL_FPoint quad[4],
                                       uint8_t alpha);

/* catastrophe_widgets.h additions — non-blocking sibling of cat_list */
typedef struct {
    int cursor;
    int scroll_offset;
    int visible_rows;
} cat_list_state;

typedef void (*cat_list_item_draw_fn)(int idx, int x, int y, int w, int h,
                                       bool selected, void *user);

void  cat_list_state_init(cat_list_state *s, int visible_rows);
void  cat_list_state_move(cat_list_state *s, int delta, int count);
void  cat_list_state_page(cat_list_state *s, int delta, int count);
void  cat_list_state_jump(cat_list_state *s, int target, int count);
int   cat_list_state_jump_letter(cat_list_state *s,
                                  const char *(*get_label)(int, void*),
                                  void *user, int count, int direction);

/* Stateless one-shot draw: caller owns state, used inside non-blocking render loops.
   This intentionally does NOT take its own event loop (unlike cat_list). */
void  cat_draw_list_pane(int x, int y, int w, int h,
                          int item_count, const cat_list_state *state,
                          int item_height, cat_list_item_draw_fn draw_item, void *user);
```

Reasoning: `cat_list` already exists but is **blocking** (owns its own event loop). Jawaka's launcher needs continuous rendering with async daemon status updates, so a non-blocking helper is required. Placement: both `cat_list_state` and `cat_draw_list_pane` go in widgets to colocate with `cat_list` — call this out in the widgets header docs so users pick the right API. `cat_draw_tab_bar` and `cat_draw_textured_parallelogram` stay in core because they're pure drawing primitives with no state.

### 1.3 New Jawaka internal surface

```
Jawaka/internal/
├── core/
│   ├── env.h           # jw_env_flag / jw_env_u32
│   └── autodemo.h      # jw_autodemo_init / jw_autodemo_should_fire
└── ipc/
    └── ipc_client.h    # jw_ipc_hello / jw_ipc_scan_library / jw_ipc_open_menu / jw_ipc_shutdown
```

`jw_ipc_hello(role)` parameterises the role string so launcher passes `"launcher"` and menu passes `"menu"`.

### 1.4 Refactor order

1. Add `cat_activate_window` + `cat_draw_tab_bar` + `cat_list_state*` to Catastrophe with one example in `Catastrophe/examples/` exercising them. Ship Catastrophe submodule bump.
2. Pull A, G, H, J from Jawaka into the new Catastrophe APIs.
3. Add `internal/ipc/ipc_client.{h,c}` and migrate C/D/E/F.
4. Add `internal/core/env.{h,c}` and `internal/core/autodemo.{h,c}`; migrate B, K.
5. (Optional) `internal/core/ui_shell.{h,c}` for L.
6. Diff verify: both binaries still pass mock-SD scan and the existing `JAWAKA_AUTODEMO` smoke.

### 1.5 Explicit non-goals for this refactor

- Not changing IPC wire format.
- Not changing `cat_list` (blocking widget stays).
- Not pulling Jawaka-specific concepts (jawakad RPCs, autodemo env keys) into Catastrophe.

---

## Part 2 — Three launcher layout modes, theme-driven

### 2.1 Modes

| Mode | Style | Source of inspiration | Use when |
|---|---|---|---|
| `tabs` | Horizontal tab bar at top + per-tab body (current) | Allium / current Jawaka | Default — works on tiny screens |
| `vertical` | Vertical menu of sections + body to the right (NextUI-style) | `/Volumes/Storage/GitHub/nextui-prd/workspace/all/nextui/nextui.c` | Sections feel like a real sidebar nav |
| `horizontal` | Horizontal carousel/grid of system tiles (kUI-style) | `/Volumes/Storage/GitHub/kUI/workspace/all/nextui/nextui.c` (carousel logic at L728–812+) | Big system art with the system tile front-and-center |

### 2.2 Theme stylesheet extension

Add a new section to `cat_stylesheet` (Catastrophe owns parsing; Jawaka reads it):

```json
{
  "launcher": {
    "layout": "vertical",         // "tabs" | "vertical" | "horizontal"
    "system_tile_width": 220,     // horizontal mode only; px before scale
    "system_tile_aspect": 1.4,    // tile h/w
    "sidebar_width_pct": 30       // vertical mode only
  }
}
```

C-side struct in `catastrophe.h`:

```c
typedef enum {
    CAT_LAYOUT_TABS = 0,
    CAT_LAYOUT_VERTICAL,
    CAT_LAYOUT_HORIZONTAL,
} cat_launcher_layout;

typedef struct {
    cat_launcher_layout layout;
    int   system_tile_width;
    float system_tile_aspect;
    int   sidebar_width_pct;
} cat_stylesheet_launcher;
```

- Add `launcher` to `cat_stylesheet`.
- Add defaults in `cat__stylesheet_*_init_default`.
- Parse in the existing stylesheet JSON loader.
- Surface via `cat_get_theme()` (add to `ap_theme`) or via `cat_get_stylesheet()` (already accessible).

### 2.3 Jawaka launcher refactor

In `jawaka-launcher/main.c`, replace the current `jw__render_launcher` switch with a dispatch on `cat_get_stylesheet()->launcher.layout`:

```c
switch (layout) {
    case CAT_LAYOUT_TABS:       jw__render_tabs(&state); break;
    case CAT_LAYOUT_VERTICAL:   jw__render_vertical(&state); break;
    case CAT_LAYOUT_HORIZONTAL: jw__render_horizontal(&state); break;
}
```

Mirror this for input handling — each layout has its own `jw__handle_input_*` so e.g. horizontal can use Left/Right for system change and Up/Down for game list, while vertical keeps the sidebar/body split.

Shared state (`jw_launcher_state`) stays as-is. The three render functions reuse `cat_draw_list_pane`, `cat_draw_tab_bar`, footer, status bar — all from Part 1.

### 2.4 New example themes

Create under `Catastrophe/res/themes/`:

- `Jawaka-Tabs/stylesheet.json` (Mode 1 default, copy of current Catastrophe-Demo + `"launcher": { "layout": "tabs" }`)
- `Jawaka-Vertical/stylesheet.json` (Mode 2)
- `Jawaka-Horizontal/stylesheet.json` (Mode 3 — bigger highlight, accent tuned for tile borders)

Each theme should be visually distinct enough to be a believable showcase, not just a layout switch. Wallpaper + accent colour per theme.

### 2.5 Vertical mode (NextUI-like) — sketch

Matches the NextUI reference screenshots: a **left list with pill highlight on the focused row** + a **right preview panel** that updates as the cursor moves. Two screens with the same layout — one shows sections, one shows games.

**Section screen** (top-level entry point):

```
┌───────────────────────────────────────────────────┐
│                                       (((•))) [87] 14:30 │
│  Recently Played                                   │
│  BitPal                       ┌─────────────────┐  │
│  Favorites                    │                 │  │
│  Arcade                       │   <section      │  │
│  Game Boy                     │    box art /    │  │
│ ╭──────────────────╮          │    preview>     │  │
│ │ Game Boy Color   │          │                 │  │
│ ╰──────────────────╯          │                 │  │
│  Game Boy Advance             └─────────────────┘  │
│ ╭──────────────╮                                   │
│ │ POWER  SLEEP │                       ╭ A  OPEN ╮ │
│ ╰──────────────╯                       ╰─────────╯ │
└───────────────────────────────────────────────────┘
```

**Game-list screen** (after entering a section):

```
┌───────────────────────────────────────────────────┐
│                                  (((•))) [87] 2:56 PM│
│  Donkey Kong Lan…                                 │
│  Donkey Kong Lan…             ┌─────────────────┐  │
│  Dr. Mario                    │                 │  │
│  Final Fantasy Ad…            │   <game art /   │  │
│  Gargoyle's Quest             │    in-game      │  │
│  Harvest Moon GB              │    screenshot>  │  │
│ ╭──────────────────╮          │                 │  │
│ │ Kirby's Dream La…│          └─────────────────┘  │
│ ╰──────────────────╯                               │
│ ╭──────────────╮                  ╭────────────╮   │
│ │ POWER  SLEEP │                  │ B BACK A OP│   │
│ ╰──────────────╯                  ╰────────────╯   │
└───────────────────────────────────────────────────┘
```

Layout rules pulled from the screenshots:

- Left column is a vertical list, items left-aligned, ellipsized when wider than the column.
- Focused item drawn as a **rounded white pill** (highlight) with dark text — reuse `cat_draw_pill` + the existing theme `highlight` / `highlighted_text` colours.
- Right panel sits roughly in the right ~45–50% of the screen and shows preview art for the focused row (system art for sections, in-game screenshot / box art for games). Rounded-corner panel.
- Status bar pill in the top-right (Wi-Fi + battery + time), drawn by the existing `cat_draw_status_bar` with the standard `cat_status_bar_opts`.
- Footer hint pills bottom-left (POWER/SLEEP) and bottom-right (A OPEN, B BACK on game screen, A OPEN only on section screen) — drawn by the existing `cat_draw_footer`.
- No persistent tab bar — the section list *is* the navigation.

Input:

- Up/Down: navigate list.
- A: enter / open focused item. From section list, switches the screen to the game-list view for that section. From game list, launches via daemon.
- B: back (game list → section list; section list → no-op or quit).
- Left/Right or L/R triggers: jump letter inside long lists (mirrors existing tabbed mode behaviour).
- Menu: open `jawaka-menu`.

Implementation:

- One render function `jw__render_vertical(state)` parameterised by `state->vertical_screen` ∈ {`SECTIONS`, `GAMES`}.
- Both screens reuse the same `cat_draw_list_pane` for the left list and the same right-preview drawing helper.
- Sections list is **flat** (decided 2026-05-15): `Recently Played`, `Favorites`, then each individual system in alphabetical order, then `Apps`, then `Settings`. No `Games` umbrella row. Apps and Settings always last so their positions are stable as systems come and go.

### 2.6 Horizontal mode (kUI-like) — sketch

Matches the kUI parallelogram-carousel reference screenshots: **full-screen background of the focused system's art** with a **skewed/parallelogram strip of system tiles** running across the centre. Selected system shows its name overlaid in big text in the middle.

```
┌─────────────────────────────────────────────────────────┐
│                                          ╭─────╮         │
│                                          │((•))│ [▮]     │
│  ▒▒▒▒▒▒░░░░╲                ╲▓▓▓▓▓▓▓▓▓▓▓▓╲                │
│  ▒▒system▒▒░╲   GAME  BOY    ╲▓▓system+1▓▓╲   …          │
│  ▒▒  -1  ▒▒░░╲   COLOR        ╲▓▓▓▓▓▓▓▓▓▓▓▓╲              │
│  ▒▒▒▒▒▒▒▒░░░░╲                ╲▓▓▓▓▓▓▓▓▓▓▓▓╲              │
│  ▒▒▒▒▒▒▒▒░░░░ ╲              ╲ ▓▓▓▓▓▓▓▓▓▓▓▓ ╲             │
│                <  selected system art   >                │
│ ╭──────────────╮                            ╭──────────╮ │
│ │ POWER  SLEEP │                            │ A   OPEN │ │
│ ╰──────────────╯                            ╰──────────╯ │
└─────────────────────────────────────────────────────────┘
```

Visual rules pulled from the screenshots:

- The whole screen is the **focused system's art**, full-bleed.
- A series of **parallelogram-shaped panels** are laid out left→right across the middle, each one displaying a neighbouring system's art clipped to the parallelogram. The center one is the focused system and shows the system name (big text, two-line treatment — "GAME BOY" / "COLOR") centered.
- All parallelograms share the same skew angle. Neighbours are dim (lower alpha) so the focused panel reads as the foreground.
- Status bar pill (Wi-Fi + battery, no clock in the photo — but follow theme stylesheet) in the top-right.
- Footer pills (POWER/SLEEP bottom-left, A OPEN bottom-right) over the art — drawn by `cat_draw_footer` as usual.
- No tab bar, no list.

Input:

- Left/Right: change focused system. Animate the carousel scroll.
- A: enter the focused system's game list. On A, transition to a vertical-mode-style game-list screen (reuses `jw__render_vertical(GAMES)`) — this gives consistent behaviour across modes without inventing a third game-list view.
- Menu: open `jawaka-menu`.
- L/R triggers: jump multiple systems at once.
- Y: rescan (matches current tabbed mode).

Implementation notes:

- Parallelogram clipping: SDL2 doesn't have a direct skewed-quad blit, so options are (a) `SDL_RenderGeometry` (SDL ≥ 2.0.18) feeding a quad with skewed UVs — preferred; (b) render the system art to a texture, then draw it through a shader or per-row stretched blits. (a) is the right call; both Catastrophe targets already use a modern enough SDL.
- New Catastrophe helper `cat_draw_textured_parallelogram(SDL_Texture *tex, const SDL_FPoint quad[4], uint8_t alpha)` (see §1.2). The launcher composes the strip itself; Catastrophe owns the math.
- System art comes from `Images/<system>.png` (per ARCHITECTURE_DECISIONS.md §6). Fallback to a solid theme-coloured panel with the system name if no art is present.
- Background art for the focused system is the same `Images/<system>.png` blown up to full-screen with a dark overlay (~50% alpha) so the carousel and footer stay readable.
- Game-list view after A reuses the vertical mode's game list, so we don't ship a third list implementation.

**Tools tile** (decided 2026-05-15): a single fixed tile is pinned at the end of the carousel after all systems. Its art is theme-supplied (`res/themes/<theme>/tools_tile.png`) with a solid theme-colour fallback. Selecting it (A) opens a quick menu listing `Recents`, `Favorites`, `Apps`, `Settings`. The quick menu is a simple `cat_list` (blocking widget already in Catastrophe) — selecting Recents/Favorites jumps into the vertical mode's game-list screen filtered to that collection; Apps/Settings open their own screens reusing the existing tab bodies.

### 2.7 Implementation order

1. Extend `cat_stylesheet` + parser + defaults; add example themes (only `layout` set, no visual change yet).
2. Dispatch `jw__render_*` and `jw__handle_input_*` by layout, with vertical and horizontal initially calling the same code path as tabs. Confirm switching themes flips render functions.
3. Implement vertical mode.
4. Implement horizontal mode + tile artwork loader (cached via Catastrophe's existing texture cache if applicable).
5. Polish + theme tuning.

---

## Part 3 — Bundle Space Grotesk and Source Han Sans CN Heavy

> **Decided 2026-05-15:** MiSans is out — after reading the Xiaomi EULA the redistribution terms aren't compatible with an open-source bundle. Substitute **Source Han Sans CN Heavy** (Adobe/Google, dual-licensed under SIL OFL 1.1).

### 3.1 Licence summary

**Space Grotesk** — listed on Google Fonts.
- License: **SIL Open Font License 1.1 (OFL)**.
- ✅ Bundling and redistribution allowed; OFL text must ship alongside.
- Source: https://fonts.google.com/specimen/Space+Grotesk → "License" tab.

**Source Han Sans CN Heavy** — Adobe + Google, distributed via the Source Han Sans GitHub release (`SourceHanSansCN-Heavy.otf` in the `SubsetOTF/CN/` directory of the release).
- License: **SIL Open Font License 1.1 (OFL)**.
- ✅ Bundling and redistribution allowed; OFL text must ship alongside.
- `CN` = Simplified Chinese subset (smallest file size of the four region variants, ~10 MB for Heavy weight).
- `Heavy` weight chosen to match the visual weight of Space Grotesk for the launcher UI.

### 3.2 Where the fonts live

Catastrophe owns fonts (`Catastrophe/res/font.ttf` today). The new fonts are toolkit-level, not Jawaka-specific, so they go in Catastrophe:

```
Catastrophe/
└── res/
    └── fonts/
        ├── Inter/                     # existing default, renamed for clarity
        │   ├── font.ttf
        │   └── LICENSE.txt
        ├── SpaceGrotesk/
        │   ├── SpaceGrotesk-Regular.ttf
        │   ├── SpaceGrotesk-Bold.ttf  # only if bold variant is actually used
        │   └── OFL.txt
        └── SourceHanSansCN/
            ├── SourceHanSansCN-Heavy.otf
            └── OFL.txt
```

Update `Catastrophe/res/font.LICENSE.txt` → folder-per-family pattern. Keep a top-level `res/fonts/README.md` listing each family + license + intended use (Latin / CJK).

### 3.3 Theme stylesheet wiring

Themes already point at a font via `ui.ui_font.path`. The current loader resolves it relative to the theme directory; extend it to also try `res/fonts/<family>/<file>.ttf` (or `.otf`) so themes can reference shared families without copying files:

```json
"ui": {
  "ui_font": { "path": "fonts/SpaceGrotesk/SpaceGrotesk-Regular.ttf", "size": 36 }
}
```

Wire the existing `cjk_font` slot (already in `cat_stylesheet`) to Source Han Sans CN Heavy by default:

```json
"cjk_font": { "path": "fonts/SourceHanSansCN/SourceHanSansCN-Heavy.otf", "size": 36 }
```

Confirm Catastrophe's text rendering actually falls back to `cjk_font` for non-Latin codepoints. If it doesn't yet, that's an additional Catastrophe task (text-shaping fallback) — call it out as a follow-up rather than expanding scope here.

Note: SDL2_ttf supports OTF as well as TTF, so no loader change is needed for the `.otf` extension. Verify with a smoke render of Chinese text in one of the example builds.

### 3.4 Build/packaging touches

- `Catastrophe/Makefile`: copy `res/fonts/**` into example build dirs (already copies `res/`).
- `Jawaka/Makefile`: copy fonts into the mock-sdcard / device package so themes referencing them work at runtime.
- Verify the font files are not stripped by the FAT32-safe assertions.
- Add CI sanity: run a smoke that loads each font path and renders "Hello / 你好" in each example.

### 3.5 Implementation order

1. Capture Space Grotesk and confirmed-MiSans-licence files locally; commit `LICENSE.txt` per family.
2. Restructure `Catastrophe/res/` to the per-family layout; keep a default path alias so existing themes don't break.
3. Wire fallback path resolution (`res/fonts/<family>/...`).
4. Update one example theme (`Catastrophe-Demo`) to use Space Grotesk + MiSans to validate end-to-end.
5. Add the Jawaka theme variants from Part 2 with these fonts wired in.

---

## Suggested overall sequencing

1. **Part 1 first** (1–2 days). Lands the building blocks (`cat_draw_tab_bar`, `cat_draw_list_pane`, `cat_list_state`, IPC client) the new layouts will use. Low visual risk.
2. **Part 3 second** (half day + licence research). Self-contained, unblocks any theming work that wants nicer fonts.
3. **Part 2 last** (largest, 3–5 days). New code rests on the now-clean Catastrophe surface and the new fonts.

## Decisions captured (2026-05-15)

- **CJK font:** Source Han Sans CN Heavy (OFL), not MiSans. EULA-clean to bundle.
- **Vertical mode:** matches NextUI screenshots — left list with pill highlight + right preview panel. Same layout for the section screen and the game-list screen. No focus-switch button (the list always has focus; the preview is passive). See §2.5.
- **Horizontal mode:** matches kUI screenshots — full-screen background of the focused system + skewed parallelogram carousel of system tiles with the centre tile showing the system name. Game lists reached via A reuse the vertical mode game list. See §2.6.
- **Vertical mode section list:** flat list — `Recently Played`, `Favorites`, each system alphabetically, then `Apps`, `Settings`. No `Games` umbrella. See §2.5.
- **Horizontal mode non-system browse:** one fixed "Tools" tile pinned at the end of the carousel; selecting it opens a quick `cat_list` menu with Recents / Favorites / Apps / Settings. See §2.6.
- **`cat_list_state` + `cat_draw_list_pane` placement:** both go in `catastrophe_widgets.h` to colocate with `cat_list`. `cat_draw_tab_bar` and `cat_draw_textured_parallelogram` stay in core as pure drawing primitives. See §1.2.
