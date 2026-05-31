# Phase 4 — Stateful Behavior

Tracks Helaas's "Stateful Behavior" list. Order is dependency-driven: stable
ids underpin favorites/recents/playtime, so that lands first.

## Status

- [x] **Stable ids / non-destructive rescan** (foundation; also the `[~]`
  incremental-refresh item). Rescan no longer wipes and rebuilds the library —
  it upserts keyed on the UNIQUE `rom_path` / `pak_dir`, so a game/app keeps its
  id across refreshes, and prunes only entries whose path vanished from disk.
- [ ] Favorites add/remove/list.
- [ ] Recents tracking on launch.
- [ ] Playtime tracking.
- [ ] Last-opened context restoration.
- [~] Index refresh — full rescan is now additive; targeted single-path
  incremental update still TODO.
- [ ] Robust persistence across rescans — covered for recents/favorites by
  stable ids; revisit if other state is added.
- [~] Launcher/menu/daemon handoff — menu + game launch handoff exist; richer
  state handoff pending.

## Stable ids (done)

`internal/db/db.c`, `internal/db/db.h`, `internal/discovery/discovery.c`.

Problem: `jw_db_reset_library` did `DELETE FROM games/apps` and inserts used
`INSERT OR REPLACE`; both reassign the `INTEGER PRIMARY KEY`, so `favorites` /
`recents` rows (which reference `target_id`) broke on every rescan.

Fix:
- `jw_db_insert_game` / `jw_db_insert_app` upsert via `ON CONFLICT(rom_path|
  pak_dir) DO UPDATE`, preserving the id. `last_played` / `playtime_s` are left
  untouched. Each insert records its path in a per-scan temp table.
- `jw_db_scan_begin` creates/clears the temp seen tables; `jw_db_scan_prune`
  deletes only unseen games/apps and any orphaned favorites/recents. FTS stays
  consistent through the existing triggers.
- `jw_scan_library` runs `scan_begin -> scan -> scan_prune` in one transaction.

Verified: scan-smoke double-scan (id stable, no churn, favorites/recents
survive; vanished ROM pruned + orphans cleaned) and on-device reboot rescan.

## Next: favorites

The launcher already shows an empty "Favorites" section and the `favorites`
table exists (`kind`, `target_id`, `added_at`). Needs: db CRUD
(add/remove/toggle/list), a toggle action in the launcher, and wiring the
section to the live list. Stable ids make `target_id` reliable.

## Then: recents + playtime

The daemon already tracks RA sessions — `jw__retroarch_session_finish` computes
`runtime_s` and knows `rom_path` + `system`, it just doesn't persist. One hook
there writes the `recents` row (`last_opened`, `duration_s`) and increments
`games.playtime_s` / sets `last_played`, keyed by `rom_path` -> id. Apps need an
equivalent on app-launch.
