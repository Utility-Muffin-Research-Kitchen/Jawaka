# Jawaka IGM — switcher current-game art + slot management

Date: 2026-06-09
Status: implemented (Items 0, 2, 3) — builds clean, launcher smoke green;
on-device RA verification pending.

## Implementation notes (deviations from the original design)
- **Keep** is `jw_ra_promote_switcher_slot()` in `internal/retroarch/states.c`,
  called directly from the menu on `Y` (states.c is linked into jawaka-menu), not
  a `keep-switcher-save` daemon IPC action. Simpler and self-contained; still a
  pure file copy that doesn't disturb the running game.
- **Auto-slot fix:** decoupling means "Slot Auto" must target slot −1 explicitly.
  `jw_ra_save_state_slot(-1)` now mirrors `jw_ra_load_state_slot(-1)` (SET slot
  −1, then SAVE_STATE), and the daemon routes `save-state`/`load-state` through
  the slot variants whenever a value is present (`internal/retroarch/command.c`,
  `cmd/jawakad/main.c`).

## Context

Two deferred items from the game-switcher / in-game-menu (IGM) pass:

1. **The in-game switcher's current-game tile looks empty** before the game has
   any saved screenshot. The overlay already grabs the live *paused frame* via
   `kmsgrab` for its dimmed backdrop (`jw__ingame_capture_still`, captured before
   the window maps), but the carousel's center tile only falls back to cover art
   or a placeholder card. Opening the switcher must stay instant — no new capture.

2. **Slot 99 (`JW_RA_GAME_SWITCHER_STATE_SLOT`) leaks into the regular slot UX.**
   The IGM Save/Load slot cycle (`jw__set_state_slot_delta`, `jawakad/main.c`) is
   an unbounded raw number with no awareness of which slots have saves or that 99
   is reserved. Users want: 99 hidden from normal use, slots sorted by last
   created, and a concise way to keep the switcher quicksave (slot 99) as a normal
   numbered slot — all without bloating the IGM.

Scope: **IGM-only** for item 2's current-tile change (the launcher switcher has no
live frame) and for all of item 3. Item 0 below is an independent jawakad
correctness fix folded into the same pass.

## Item 0 — RA config must live on the SD card, consistently (bug)

**Symptom:** game launches read/write RA config under `/userdata/root/.config/
retroarch/` on device internal storage (e.g. `FCEUmm.opt`), while the standalone
`--menu` runner puts it on the SD card. The two paths disagree.

**Root cause:** the runner sets `HOME` to the SD state dir before launching RA
(`cmd/jawaka-retroarch-runner/main.c:167`, `setenv("HOME",
jw_retroarch_state_dir(sdcard_root), 1)`), so RA resolves `$HOME/.config/
retroarch/` (config dir, per-core `.opt` options) onto the SD card. The direct
jawakad launch (`cmd/jawakad/main.c:3030`) execs RA with `--config <runtime>` but
**never sets `HOME`**, so RA inherits the daemon's `HOME=/userdata/root`. The
prepared config (`internal/platform/paths.c:1255`) pins the storage dirs
(system/savefile/savestate/libretro/info/screenshot/autoconfig) to SD but does
**not** pin the config dir / `core_options_path`, which follow `HOME`.

**Fix:** set `HOME` for the directly-launched RetroArch to the same SD state dir
the runner uses, so all RA config lives on the SD card jawaka was launched from.

- `cmd/jawakad/main.c`, RA launch (`jw__launch_retroarch` around line 2983):
  - In the **parent**, before `fork`, compute
    `char *ra_home = jw_retroarch_state_dir(state->sdcard_root);` (does `mkdir`;
    log a warning if NULL). Use `state->sdcard_root` so all games share one
    canonical RA config location, matching the runner.
  - In the **child** (`pid == 0`), before `execv`:
    `if (ra_home && ra_home[0]) setenv("HOME", ra_home, 1);`
  - `free(ra_home)` in the parent after the fork and on the `fail` path.
  - **Do not** set `HOME` in the parent — jawakad is a daemon; that would change
    its own `HOME` and every subsequent child (menu, future launches).

**Optional hardening** (belt-and-suspenders, can skip if the HOME fix is enough):
also pin `rgui_config_directory` and `core_options_path` to the SD config dir in
`jw_prepare_retroarch_config` so config location is independent of `HOME`.

**Verify (on-device):** launch a game directly (not via the runner), change a
core option, quit; confirm the `.opt`/`retroarch.cfg` writes land under the SD
state dir (`<sd-state>/retroarch/.config/retroarch/`) and nothing new appears
under `/userdata/root/.config/retroarch/`. Confirm the runner and game launches
now read the same config.

## Item 2 — current-game tile = the live paused frame

Reuse the `still_tex` already captured for the backdrop as the carousel's
current-game tile. Zero extra cost, truest "where you are right now" image.

- `internal/launcher/game_switcher.{h,c}`:
  - Add a **borrowed** texture pointer to `jw_game_switcher`:
    `SDL_Texture *current_tex;` (NOT owned — never freed by the switcher).
  - Add `void jw_game_switcher_set_current_texture(jw_game_switcher *sw,
    SDL_Texture *tex);`.
  - In `jw__switcher_draw_tile`, when drawing the tile at `current_index` and
    `current_tex` is set, draw `current_tex` **center-cropped to fill** the square
    tile (the still is 960×720 landscape; fill reads better than letterboxing).
    Add a fill variant alongside the existing `jw__switcher_draw_image_fit`.
- `cmd/jawaka-menu/main.c`, `jw__ingame_show_switcher`: after
  `jw__ingame_capture_still(state)` (already called before `cat_show_window`),
  call `jw_game_switcher_set_current_texture(&switcher, state->still_tex)`.
  Ownership stays with `state` (freed in `jw__ingame_free_imagery`).

Fallback order for the current tile when capture failed: slot-99 thumb → cover →
placeholder (unchanged for non-current tiles).

## Item 3 — slot management in the IGM

Keep the two existing rows (**Save State**, **Load State**); make the cycle smart
and decouple the two selections. Slot 99 never shows as "Slot 99".

### Model (decoupled, recency-aware)
The menu drives **explicit** slots instead of RetroArch's single shared
`state_slot` (the `save-state`/`load-state` IPC already takes the slot as
`value`, and `value < 0` → Auto). New menu state in `jw_ingame_state`:
- `int  save_slot;`   — Save target (Auto=-1, 0–9), cycles numerically, skips 99.
- `int  load_index;`  — index into the recency-ordered existing-saves list.
- a small per-ROM slot list: `{ int slot; time_t mtime; } load_slots[…];`
  `int load_count;` plus `bool latest_is_switcher;` (true when slot 99 is newer
  than every numbered save for this ROM).

Built on open and refreshed after any save/load/keep.

### New states helper
`internal/retroarch/states.{h,c}`:
`bool jw_ra_list_slots(const char *states_dir, const char *rom_path,
                       jw_ra_slot_info *out, int max, int *count);`
Scans `States/` (flat + one level of per-core subfolders, like the existing
helpers) for `<stem>.state`, `.state.auto`, `.stateN`; records slot + mtime;
flags slot 99 separately. Returns existing saves; the menu sorts newest-first.

### Save State row
- Left/Right cycles **Auto + 0–9**, skipping 99 (`jw__ingame_adjust` updates
  `save_slot` locally — no IPC).
- `A` → `save-state` with `value = save_slot`.
- Caption (made clear): `Slot 3 · empty` or `Slot 3 · overwrites`.

### Load State row (recency-ordered)
- Left/Right cycles existing saves **newest-first** (Auto/0–9 that exist),
  excluding 99 as a number. When `latest_is_switcher`, a **`Latest`** entry is
  prepended (this is slot 99, never shown as "99").
- `A` → `load-state` with the selected entry's slot (`Latest` → slot 99).
- Captions (made clear): numbered saves show `Slot 7 · newest · 5m ago` and an
  `n of N` position; the `Latest` entry shows `Latest · unsaved · 2m ago`.
- When `Latest` is selected, a **status line** explains it:
  `Quicksave from Save & Quit — A resumes · Y keeps it as a numbered slot`.
- Footer hint on Save/Load rows: `← → browse saves`.

### Keep (promote slot 99 → numbered slot)
- New daemon action (`jw__handle_retroarch_action`), e.g. `keep-switcher-save`:
  copies `<rom>.state99` (+ `.state99.png`) → the **lowest empty** slot in 0–9
  (overwrites the **oldest** numbered slot if all full), touches the new file's
  mtime to now, and returns the chosen `N` in the reply. Pure file copy — does
  not disturb the running game. Uses the session's `rom_path` + `source_root`.
- Menu: when the `Latest` entry is reachable, footer shows **`Y Keep`**; `Y`
  sends the action, then status `Kept as Slot N` and refreshes the slot list.
  Because slot N is now newest, the `Latest` entry disappears (it's been kept).

### Hide-99 scope
Always hidden as a number, everywhere in the regular UX; **no** advanced toggle.
The `Latest`/`Keep` flow is the only user-facing access to the switcher save.

### Preview thumbnail
`jw__ingame_update_thumb` keys off the **selected row's** slot (Save → `save_slot`;
Load → selected entry's slot; `Latest` → slot 99) instead of
`session.state_slot`, so the preview pane matches the decoupled selection.

## Files touched
- `internal/launcher/game_switcher.{h,c}` — borrowed `current_tex` + fill draw.
- `internal/retroarch/states.{h,c}` — `jw_ra_list_slots` slot enumeration.
- `cmd/jawaka-menu/main.c` — decoupled Save/Load state, recency Load + `Latest`,
  captions/status, `Y Keep`, preview keyed to the selected row, set current tex.
- `cmd/jawakad/main.c` — set `HOME` to the SD state dir in the RA launch child
  (Item 0); `keep-switcher-save` daemon action.
- `internal/platform/paths.c` — only if doing Item 0's optional hardening (pin
  `rgui_config_directory` / `core_options_path`).
- `internal/ipc/ipc_client.{c,h}` — only if a typed helper is preferred over the
  generic `jw_ipc_retroarch_action` for `keep-switcher-save`.

Reused: `jw__ingame_capture_still`, `still_tex`; `jw_ra_find_slot_thumb`;
`JW_RA_GAME_SWITCHER_STATE_SLOT`; `save-state`/`load-state` explicit-slot path.

## Verification
- Build: `export CATASTROPHE_DIR=../Catastrophe && make mockgen && make`;
  launcher smoke via the `run-jawaka` skill (boot/init/render regression only —
  no pixels, no RA).
- On-device (MLP1 + real RetroArch), the parts the desktop mock can't exercise:
  1. **Item 2:** start a brand-new game (no cover, no save) → Menu+Select → the
     center tile shows the live paused frame; opening stays instant.
  2. **Save row:** cycle shows Auto + 0–9 only (never 99); captions read
     `empty`/`overwrites`; Save writes the chosen slot.
  3. **Load row:** lists existing saves newest-first with `… ago` timestamps and
     `n of N`; selecting and `A` loads the right slot.
  4. **Latest + Keep:** after Save & Quit and re-entry, Load shows a `Latest`
     entry (slot 99) with the explanatory status; `Y Keep` copies it to the
     lowest free numbered slot, shows `Kept as Slot N`, the new slot has a
     `.stateN`+`.stateN.png`, and the `Latest` entry then disappears.
  5. Slot 99 never appears as "Slot 99" anywhere in the regular UX.
