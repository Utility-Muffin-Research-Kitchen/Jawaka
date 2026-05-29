# Jawaka Implementation Tracker

Last reviewed: 2026-05-24

Source plan: `/Volumes/Storage/UMRK/plans/jawaka.md`

Implementation repo: `/Volumes/Storage/UMRK/Jawaka`

## Snapshot

Jawaka has a solid Mac-first launcher foundation. The separate repo, three-binary
architecture, daemon-owned startup, IPC, mock SD-card scanning, SQLite indexing,
minimal Catastrophe launcher/menu surfaces, settings theme switching, and early
RetroArch launch plumbing are implemented and build successfully.

The broad roadmap is roughly 45-55% implemented. Phase 0 and most of Phase 1
are complete; Phase 2 is now functionally closed for its intended Mac-first
slice; Phase 3 is usable for games and app launching but still missing recents,
favorites, and broader polish; Phases 4-7 are mostly schema, scaffolding, and
early platform work.

Verification performed during this review:

- `make mockgen && make` builds `jawakad`, `jawaka-launcher`, and `jawaka-menu`.
- `JAWAKA_AUTODEMO=1 JAWAKA_AUTODEMO_DELAY_MS=200 make run-daemon` completes the
  daemon -> launcher -> menu -> shutdown flow.
- The mock library scan indexed 286 games across 15 systems and 2 apps.
- SQLite FTS checks returned Mario game matches, a Tools app match, and 285
  indexed game artwork paths.
- A direct `launch-app` IPC request for `Apps/HelloApp.pak` returned `ok` and
  spawned the mock app as a daemon-owned foreground child.

## Phase Status

| Phase | Status | Estimate | Notes |
| --- | --- | ---: | --- |
| Phase 0 - Repo bootstrap and planning | Done | 100% | Repo exists separately from Catastrophe; plan/docs are present. |
| Phase 1 - Desktop foundation and mock SD card | Mostly done | 90% | Makefile, three binaries, shared internals, platform paths, IPC, mock SD, and SQLite exist. Caveat: `third_party/catastrophe` is still a placeholder; the build currently uses sibling `../Catastrophe`. |
| Phase 2 - Daemon flow, discovery, search, artwork | Done for current scope | 95% | Daemon supervision, scans, Apps discovery, menu invocation, SQLite population, games/apps search, local game art rendering, app `pak.json` metadata, and foreground app launch are implemented. Per-game metadata remains explicitly deferred. |
| Phase 3 - Core launcher UI | Partial | 55% | Games browsing and game launch path exist. Apps can launch from the Apps tab/search, and search results exist. Recents is an empty state; Favorites are missing; broader app/detail flows are still rough. |
| Phase 4 - Stateful launcher behavior | Skeleton | 15% | Tables exist for recents/favorites, and refresh exists. Favorites management, recents tracking, playtime, context restore, and robust persistence rules are not implemented. |
| Phase 5 - Launcher settings and polish | Partial | 30% | Settings UI exists with Appearance/Library/Behavior/About categories. Theme/display selection persists; other settings pages are placeholders. |
| Phase 6 - Real launch integration and platform prep | Early partial | 25% | Game launch can spawn RetroArch with core resolution and append config. MLP1-specific build/path work exists. tg5040/my355 are placeholders; FAT32 validation and broader platform hardening are not complete. |
| Phase 7 - Device-port phases | Scaffolding | 10% | Port directories exist; MLP1 smoke/input scripts exist. tg5040/my355 ports and device-specific tuning are not implemented. |

## Feature Tracker

Legend: `[x]` done, `[~]` partial, `[ ]` not started.

### Architecture And Runtime

- [x] Separate `Jawaka` repo from Catastrophe.
- [x] Makefile-based build for `jawakad`, `jawaka-launcher`, and `jawaka-menu`.
- [~] Catastrophe dependency wired in. Works via sibling `../Catastrophe`; submodule/vendor path is not populated.
- [x] Shared internal layer for logging, IPC, database, discovery, paths, and settings.
- [x] Unix domain socket IPC with length-prefixed JSON.
- [x] `jawakad` normal Mac entrypoint with launcher/menu child supervision.
- [x] Daemon-owned scan and refresh requests.
- [x] Daemon-controlled `jawaka-menu` invocation.
- [x] Daemon-owned game launch request path.
- [x] Daemon-owned app launch request path.

### Content Discovery And Data

- [x] Mock SD-card generator script in the repo.
- [x] Mock tree includes `Roms/`, `Images/`, `Apps/`, `BIOS/`, `Saves/`,
  `States/`, `.umrk/`, and `UMRK/mac/defaults`.
- [x] Games scanned from `Roms/<SYSTEM>/`.
- [x] Apps scanned from structured `Apps/<Name>.pak/`.
- [x] Metadata loading for current scope. `pak.json` app metadata is parsed; per-game metadata is intentionally skipped for now.
- [x] Local box art support for current scope. `Images/<SYSTEM>/<title>.png` paths are indexed and rendered in game detail views.
- [x] SQLite schema covers games, apps, recents, favorites, settings, and FTS5.
- [x] Search-facing data exists through FTS5 schema and triggers for games and apps.
- [x] Search query API and launcher search UX.
- [~] FAT32-safe layout assumptions are reflected in the tree and docs; no validator/hardening pass yet.

### Launcher And Menu UI

- [~] Top-level Games section.
- [~] Top-level Apps section. Apps list and tabbed activation exist; fuller app browsing/detail flows are pending.
- [~] Top-level Recents section. Empty state exists; data is not tracked.
- [ ] Favorites management and real Favorites section.
- [~] Settings section. Theme/display switching works; Library/Behavior/About are placeholders.
- [x] Catastrophe-backed launcher surface.
- [x] Catastrophe-backed contextual menu surface.
- [x] Menu quick actions: rescan, return to launcher, shutdown.
- [~] List/detail navigation. Game system and ROM browsing work; app/detail flows are incomplete.
- [~] Empty and error states. Basic messages exist, but not comprehensive.
- [x] Search entry/results flow.
- [~] Artwork-aware browsing. Game detail art is rendered; broader thumbnail/icon browsing is pending.
- [~] Basic game launch action plumbing via daemon.
- [x] App launch action plumbing via daemon.
- [x] Multiple launcher layouts are available through themes: tabbed, vertical, horizontal.

### Stateful Behavior

- [ ] Favorites add/remove/list.
- [ ] Recents tracking on launch.
- [ ] Playtime tracking.
- [ ] Last-opened context restoration.
- [~] Index refresh behavior. Full rescan works; incremental update behavior is not implemented.
- [ ] Robust persistence rules across rescans, especially stable IDs for recents/favorites.
- [~] Launcher/menu/daemon handoff rules. Menu and game launch handoff exist; richer state handoff is pending.

### Settings And Polish

- [x] Launcher-owned settings surface exists.
- [~] Theme/display preferences persist through SQLite.
- [ ] Content root configuration UI.
- [ ] Search/display behavior preferences.
- [~] Library refresh controls. Rescan exists on `Y` and in menu, but not as a settings page flow.
- [~] Visual polish pass. UI is functional with multiple layouts; final browsing polish is pending.

### Platform And Device Prep

- [~] Real game launch integration. RetroArch/core paths and append config exist, but this still depends on external payloads being present.
- [x] Real app launch integration for `Apps/<Name>.pak/launch.sh` foreground children.
- [~] Platform abstraction layer. Mac and MLP1 paths/core defaults exist; tg5040/my355 are not hardened.
- [~] SD-card portability assumptions are documented and represented in mock output.
- [ ] FAT32 validation pass.
- [~] MLP1 cross-compile dispatcher and ADB smoke/input tools.
- [ ] tg5040-class port.
- [ ] my355-class port.
- [ ] Per-device input and layout tuning.

## Evidence Pointers

- Build and run targets: `/Volumes/Storage/UMRK/Jawaka/Makefile`
- Daemon supervision, scanning, menu, launch IPC handling: `/Volumes/Storage/UMRK/Jawaka/cmd/jawakad/main.c`
- Launcher browsing, settings entry, game browser, menu IPC: `/Volumes/Storage/UMRK/Jawaka/cmd/jawaka-launcher/main.c`
- Menu quick actions: `/Volumes/Storage/UMRK/Jawaka/cmd/jawaka-menu/main.c`
- SQLite schema/runtime access: `/Volumes/Storage/UMRK/Jawaka/internal/db/db.c`
- Reviewer-facing schema copy: `/Volumes/Storage/UMRK/Jawaka/internal/db/schema.sql`
- ROM/app scanning: `/Volumes/Storage/UMRK/Jawaka/internal/discovery/discovery.c`
- Runtime/platform paths and RetroArch launch config: `/Volumes/Storage/UMRK/Jawaka/internal/platform/paths.c`
- Settings UI/theme persistence: `/Volumes/Storage/UMRK/Jawaka/internal/settings/settings.c`
- Mock SD-card generation: `/Volumes/Storage/UMRK/Jawaka/scripts/mockgen.sh`
- Mac payload contract: `/Volumes/Storage/UMRK/Jawaka/docs/MAC_PACKAGING_CONTRACT.md`
- Port scaffolding: `/Volumes/Storage/UMRK/Jawaka/ports/`

## Recommended Next Slices

1. Finish the Phase 3 core UI gap: Favorites section, real recents data, richer app browsing/detail flows, and better search result polish.
2. Add Phase 4 persistence rules using stable target keys so recents/favorites survive rescans.
3. Expand settings with Library controls: content root, refresh action, and search/display behavior preferences.
4. Broaden artwork/icon usage beyond the game detail pane where it improves browsing.
