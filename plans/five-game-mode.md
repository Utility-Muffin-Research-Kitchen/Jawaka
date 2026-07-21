# 5-Game Mode (locked focus mode) — build spec

A deliberate minimal mode: curate up to 5 games, hide everything else, and lock the
device into a stripped focus screen behind a PIN or button-combo. MinUI-barebones
in spirit, but Leaf-native and lockable. Curation-first cousin of the kids/kiosk
idea; this ships the curate + lock halves as one flow.

Status: DESIGNED 2026-07-20/21 (grilled through with Eric, mockups approved in both
Theme and B&W). NOT built. Owner: Jawaka.

## Design decisions (locked)

- **Flow, not config.** Turning it on IS the setup. A wizard runs, then the device
  drops into focus mode. Nothing about the mode is edited from a normal settings page.
- **Entry point:** an ACTION, not a setting — a `Start 5-Game Mode` row in the System
  menu's **Actions** tab (next to Rescan / Update), verb-labeled. Re-entering with a
  remembered set is one tap.
- **Curation:** pick **up to 5** games (1-5) from the normal **tab-switched game
  browser** (multi-select), NOT the coverflow. **Games only** (no apps). The set is
  **remembered** so re-entry pre-fills the last choice.
- **Order:** the arranged order is the tile order. Reorder via the **Home-Tabs
  X-grab** interaction (X grabs a tile, move, X drops).
- **Lock:** offered at setup — **None / PIN / button-combo** (offer both PIN and combo;
  personal preference). PIN is a short numeric code; combo is a fixed hold (e.g.
  `L + R + Start`, held ~2s).
- **Focus screen:** the ≤5 games as **rounded-square box-art tiles, 2 up / 3 down**,
  centered, nothing else on screen. No box art -> the tile shows the game **title**
  centered in the square. **No titles under art tiles.** Selected tile = brighter,
  thicker **accent border** (+ faint ring). Optional: selected tile scales up slightly
  (decide in impl; border alone is fine).
- **Style:** a `Style: Theme / Black & white` pick in the flow, **default Theme**.
  Theme uses the current color scheme (tile borders in the accent color); B&W is stark
  white-on-black.
- **Battery:** **monochrome glyph, level shown by fill bars, no color-coding**, in both
  styles. Always visible, tucked in a corner. **Blink at critical-low** so "plug in now"
  still shouts without reintroducing color.
- **Wi-Fi / RetroAchievements:** **Wi-Fi OFF on enter** in v1 (helps BT coexistence +
  battery; the only thing that needs Wi-Fi here is RetroAchievements, deferred). **Restore
  the prior radio state on exit.** No RA in v1. (Follow-up: `leaf-backlog` note to check
  whether the RA build's rcheevos already queues offline unlocks; if so, offer a "Wi-Fi
  on for RA" option later — applies to normal mode too.)
- **Bluetooth:** pair beforehand in normal Leaf; rely on **reconnect-on-boot**. No
  pairing UI in focus mode. A **BT pip** shows in the corner **only when a headset is
  paired but disconnected**. The MENU/unlock overlay carries a single **Reconnect
  headset** action (no pruned settings page). (`bluetooth-mlp1`: reconnect-on-boot is
  flaky today; hardening it is a good parallel follow-up.)
- **Escape hatch:** **MENU** in focus mode opens the **unlock overlay** — the ONLY way
  out. POWER/SLEEP behave normally (never locked). Quitting a game returns to the focus
  screen. The device **boots straight into focus mode** until unlocked.
- **Recovery:** an **SD lock file** (see below) plus documentation. Delete the file from
  a computer to release the lock without a factory reset.

## Screens (the flow)

1. **Pick** — the tab-switched game browser with multi-select. Header shows `Pick games`
   + a live `N / 5` counter. A/select toggles a game; a Done action advances. Enforce the
   cap at 5 (block a 6th mark, or bump the oldest — pick one; recommend block + a brief
   "5 max" nudge).
2. **Arrange** — the chosen games as tiles in order; X-grab to reorder.
3. **Lock** — `None / PIN / Button combo`. If PIN: set + confirm the code. If Combo:
   show/confirm the fixed combo.
4. **Style** — `Theme / Black & white` (default Theme). (Can be folded into an earlier
   step if the flow feels long; keep as its own beat for clarity.)
5. **Confirm** — a `Start 5-Game Mode?` popup previewing the 5 tiles, with Cancel / Start.
   This is the "don't lock yourself out by accident" gate; on Start, apply Wi-Fi-off and
   enter the mode.
6. **Focus mode** — the 2-up/3-down tile screen. Corner status: battery (always) + BT pip
   (disconnected only).
7. **Unlock overlay** (MENU) — title (`Enter PIN to exit`, or `Hold L+R+Start to exit` for
   combo), PIN entry (4 slots, active highlighted) or the combo prompt, a `Reconnect
   headset` action (only if a headset is paired), and B to cancel back to the games.

## Data model / storage

Reuse the existing settings + DB plumbing (`internal/settings/`, `internal/db/`).
Verify exact APIs during impl; the shape:

- New settings keys (mirror the `JW_SETTING_HOME_TAB_ORDER = "home_tab_order"` pattern in
  `internal/settings/settings.c`):
  - `five_game_active` (bool) — is the mode currently on.
  - `five_game_ids` — the ordered chosen set (CSV of game ids/keys, cap 5). This is the
    "remembered" set; it persists even when inactive so re-entry pre-fills.
  - `five_game_lock` — `none | pin | combo`.
  - `five_game_pin_hash` — salted hash of the PIN (never store the plaintext).
  - `five_game_style` — `theme | bw`.
  - `five_game_wifi_prev` — radio state captured on enter, to restore on exit.
- Game identity: reuse whatever the launcher already keys favorites/recents by
  (`jw_db_set_favorite(db_path, kind, target_id, on)` uses `kind` + `target_id`). Store
  the same stable key so a picked game survives a rescan; on entry, drop any id whose ROM
  no longer resolves (dangling reference -> silently omit, so a deleted game can't wedge
  the mode).

### SD lock file (recovery)

- On enter with a lock, write a marker file to a **FAT-safe, top-level** SD path, e.g.
  `/<sdroot>/.leaf-focus-lock` (confirm the right root via the runtime env; keep it off
  the release-managed `.system` tree so it survives updates and is easy to find). Contents
  can be a small JSON echo of the lock config (no plaintext PIN).
- On boot, the daemon checks: if `five_game_active` AND the lock file exists -> enter focus
  mode locked. **If the lock file is absent, treat the device as unlocked** (deleting the
  file from a computer is the documented recovery). Unlocking via PIN/combo clears
  `five_game_active` and removes the file.
- Document in leaf-docs: "Forgot the code? Mount the SD on a computer and delete
  `.leaf-focus-lock`."

## Implementation phases (each independently testable)

1. **Storage + state.** Settings keys, PIN hash, the remembered-set serialization, the SD
   lock-file write/read, and the boot check. No UI yet; drive with a settings toggle +
   manual DB edits to prove enter/exit/persist across reboot.
2. **Focus screen render.** The 2-up/3-down tile grid (Theme + B&W), box-art vs title
   fallback, selection, launch a game, return-to-focus on quit, corner battery. Gate the
   normal launcher off while active.
3. **MENU gate + unlock overlay.** Intercept MENU in focus mode -> overlay; PIN entry and
   combo detection; Cancel; success clears state + lock file. Ensure Settings/System are
   unreachable while locked. Reconnect-headset action.
4. **Setup flow.** Pick (multi-select over the tab browser) -> Arrange (X-grab) -> Lock ->
   Style -> Confirm. The Actions-tab entry point. Remembered-set pre-fill.
5. **Radio lifecycle.** Wi-Fi off on enter (capture prior state), restore on exit; BT pip
   (paired-but-disconnected) + reconnect action wired to the BT stack.
6. **Polish + docs.** Critical-battery blink, the "5 max" nudge, dangling-game pruning,
   leaf-docs page incl. the lock-file recovery.

## Owning files / subsystems (starting points)

- `internal/settings/settings.{c,h}` — new keys, the flow screens, the Actions-tab row,
  reuse the Home-Tabs X-grab reorder (`home_tabs_grabbed`, `JW_LAYOUT_HOME_TABS`).
- `internal/launcher/` — the focus screen render + the MENU gate; the tab-switched browser
  is the picker base. (Coverflow is NOT used here.)
- `internal/db/db.{c,h}` — game listing for the picker; stable ids for the chosen set.
- `internal/platform/` (device) — Wi-Fi toggle (`loong_service` / libloong
  `WriteConfig WIFI_PARAM`, per `wifi` memory) + BT reconnect/status (`bluetooth-mlp1`).
- `cmd/jawakad/main.c` — boot check (focus-active + lock-file) that enters locked focus
  before the normal launcher; mirrors the persisted-setting apply pattern.

## Edge cases

- **Dangling picks:** a chosen game's ROM deleted/moved -> omit on entry; if that drops the
  set to zero, fall back to the normal launcher (can't lock an empty focus screen).
- **Empty library / <1 pickable game:** the Actions row is unavailable or the flow blocks.
- **Boot with the lock file but 0 valid games:** treat as unlocked, clear state.
- **Charging / low-battery / HDMI plug while locked:** show the normal system overlays; do
  not expose Settings through them.
- **In-game (RetroArch) hotkeys:** out of scope — a determined kid can reach RA's own menu.
  Note it; not this feature's job to lock the emulator.
- **Update prompts / OTA:** suppress in focus mode (no network anyway with Wi-Fi off).

## Open / defer

- Selected-tile scale-up vs border-only (impl call).
- Exact combo (recommend `L + R + Start`, hold ~2s) and PIN length (recommend 4).
- Fold Style into the Lock step if the 5-step flow feels long.
- RA offline-unlock investigation (`leaf-backlog`), which could later add a "Wi-Fi on for
  RA" option to this flow and to normal mode.
- Kids/kiosk convergence: this is the curate + lock core; a future "kiosk" could reuse the
  same lock for the full library rather than a 5-game set.
