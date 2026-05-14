# Jawaka

Jawaka is the planned launcher repo for UMRK. This repository currently
implements the first honest desktop slice of the architecture:

- `jawakad` as the long-lived coordinator
- `jawaka-launcher` as the full launcher surface
- `jawaka-menu` as the daemon-invoked contextual menu

Phase 0 + 1 is intentionally small, but real. The daemon owns startup and
scanning, SQLite opens and is populated from a mock SD-card tree, and the
launcher/menu render minimal Catastrophe-backed UI shells on macOS.

## Status

Current scope:

- Makefile-driven macOS build
- Unix domain socket IPC with length-prefixed JSON
- SQLite schema with FTS5
- mock SD-card generator
- real game/app scan into the database
- minimal launcher and menu surfaces

Deferred:

- richer Catastrophe widgets
- polished browsing UX
- scraper/downloader work
- handheld cross-compilation

## Build

Jawaka expects Catastrophe to be available locally while the future submodule is
still pending.

By default, the build will use `../Catastrophe` if it exists. Otherwise point
it explicitly at a checkout:

```sh
cd /Volumes/Storage/UMRK/Jawaka
export CATASTROPHE_DIR=/Volumes/Storage/UMRK/Catastrophe
make mockgen
make
```

## Run

```sh
make run-daemon
```

`run-daemon` defaults to a short phase-0/1 auto-demo so the full
daemon -> launcher -> menu -> shutdown flow completes on its own. Override that
for manual interaction:

```sh
JAWAKA_AUTODEMO=0 make run-daemon
```

To keep `jawakad` running without auto-spawning a UI, use daemon-only mode:

```sh
make run-daemon-only
```

Then start the launcher separately against that daemon:

```sh
make run-launcher
```

## Keyboard controls

### Launcher

- `M` open the menu through the daemon
- `R` rescan the mock SD-card library
- `Q` request shutdown through the daemon

### Menu

- `R` rescan the mock SD-card library
- `Q` request shutdown through the daemon
- `Esc` close the menu and return to the launcher

## Environment

| Variable | Purpose |
|----------|---------|
| `CATASTROPHE_DIR` | local Catastrophe checkout used for build headers |
| `JAWAKA_SDCARD_ROOT` | mock SD-card root, defaults to `./mock-sdcard` |
| `JAWAKA_RUNTIME_DIR` | runtime socket dir, defaults to `/tmp/jawaka-$USER` |
| `JAWAKA_AUTODEMO` | `1` enables the short automated launcher/menu flow |
| `JAWAKA_AUTODEMO_DELAY_MS` | delay before auto actions, defaults to `1200` |

`build/bin/jawakad` also accepts `--daemon-only` to skip the initial
launcher spawn when you want to attach `jawaka-launcher` manually.

## Layout

Phase 0 + 1 targets a Done Set Three-inspired content tree:

```text
mock-sdcard/
  Roms/<SYSTEM_CODE>/<title>.<ext>
  Images/<SYSTEM_CODE>/<title>.png
  BIOS/
  Apps/<Name>.pak/
  .jawaka/library.db
```

## Plans

- `docs/PLAN.md` mirrors the current broader Jawaka roadmap
- `docs/ARCHITECTURE_DECISIONS.md` mirrors the binding architecture decisions
- `/Volumes/Storage/UMRK/plans/jawaka-phase-0-1.md` remains the execution plan

## License

Jawaka is released under the MIT License. See [LICENSE](LICENSE).
