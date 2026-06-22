# Jawaka

Jawaka is the UMRK launcher stack built on Catastrophe. The normal entrypoint is
`jawakad`, a long-lived coordinator that owns scanning, launch requests, platform
control, process handoff, and the IPC socket. The foreground UI processes are
`jawaka-launcher` and `jawaka-menu`.

Primary target today is the Miniloong Pocket 1 (MLP1). macOS remains the fast
local preview loop with a generated mock SD-card tree.

## What Exists

- `jawakad` daemon with Unix-domain-socket, length-prefixed JSON IPC.
- Catastrophe launcher/menu UIs with Recents, Favorites, Games, Apps, and
  Settings surfaces.
- SQLite library database with FTS search, games, apps, recents, favorites, and
  persisted settings.
- Filesystem scanner for `Roms/`, `Images/`, and platform-guarded `Apps/`
  paks.
- Local box art, system icons, multiple launcher themes, game search, and a
  Select-driven game switcher.
- MLP1 platform integration for launch lifecycle, brightness, volume, audio
  output, Wi-Fi, Bluetooth, ADB pin control, boot splash, secondary SD unmount,
  LEDs, sleep, reboot, power off, and Exit to Stock.
- RetroArch helpers: `jawaka-retroarch-runner`, `jawaka-retroarchctl`, metadata
  catalog support, shared config reset, command-menu integration, and in-game
  menu flow.
- Support helpers: `jawaka-osd`, `jawaka-platformctl`, `jawaka-ledd`, and
  `jawaka-scan-smoke`.

## Build

Jawaka expects Catastrophe to be available locally. The Makefile uses
`../Catastrophe` when it exists; otherwise set `CATASTROPHE_DIR`.

```sh
export CATASTROPHE_DIR=../Catastrophe
make mockgen
make
```

Run this once after clone if system icons are missing:

```sh
./scripts/fetch-system-icons.sh
```

MLP1 build:

```sh
make mlp1
```

Other device Makefile targets (`tg5040`, `tg5050`, `my355`) are placeholders.

## Run On macOS

The daemon-owned path is the main local workflow:

```sh
make run-daemon
```

`run-daemon` runs a short automated demo. For manual testing:

```sh
make run-daemon-interactive
```

To keep only `jawakad` running and attach UI processes yourself:

```sh
make run-daemon-only
make run-launcher
make run-menu
```

Useful smoke/debug targets:

```sh
make phase3-fixture-scan-smoke
make mlp1-adb-smoke
make mlp1-adb-input-capture
make mlp1-adb-ra-command-smoke
```

## Stage To MLP1

Device staging is owned by the sibling `Leaf` repo. From `../Leaf`:

```sh
make stage-jawaka DEVICE=mlp1       # launcher payload only
make stage DEVICE=mlp1              # launcher + RetroArch/cores + apps
make stage-refresh DEVICE=mlp1      # full stage, then restart GUI stack
make refresh-jawaka DEVICE=mlp1     # restart GUI stack only
```

Leaf assembles Jawaka into:

```text
$SDCARD_PATH/.system/leaf/platforms/mlp1/launcher/
  env.sh
  bin/loong_pangu              # staged jawakad
  bin/jawaka-launcher
  bin/jawaka-menu
  bin/jawaka-osd
  bin/jawaka-platformctl
  bin/jawaka-retroarchctl
  bin/jawaka-retroarch-runner
  bin/jawaka-ledd
  res/
```

Activation is controlled by:

```text
$SDCARD_PATH/.system/leaf/platforms/mlp1/enabled
```

Jawaka's ADB restore intent marker lives at:

```text
$SDCARD_PATH/.umrk/mlp1/adb-enabled
```

## Controls

Desktop testing follows Catastrophe's default mapping:

```text
Arrows       d-pad navigation
A           face button A / select
B           face button B / back
X           search or context action
Y           refresh/rescan or secondary action
Enter       Start
Space       Select / game switcher
H           Menu
; / t       L2 / R2 tab switching
Q           desktop-only daemon shutdown
```

On MLP1, Jawaka reads the Loong Gamepad through its platform input proxy.

## Runtime Environment

Jawaka consumes the shared Leaf runtime contract from:

```text
$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh
```

Important variables:

| Variable | Purpose |
| --- | --- |
| `CATASTROPHE_DIR` | Catastrophe checkout used by local builds |
| `PLATFORM` / `DEVICE` | platform id, usually `mac` or `mlp1` |
| `SDCARD_PATH` | mock or device SD-card root |
| `SDCARD_PATHS` | colon-separated SD roots, primary first |
| `ROMS_PATHS`, `IMAGES_PATHS`, `APPS_PATHS` | plural content roots scanned by Jawaka |
| `UMRK_RUNTIME_PATH` | runtime socket and scratch directory |
| `UMRK_PLATFORM_PATH` / `SYSTEM_PATH` | platform payload root |
| `UMRK_INTERNAL_DATA_PATH` | launcher-owned state root |
| `UMRK_LAUNCHER_PATH` | launcher bundle root |
| `UMRK_RETROARCH_BIN`, `CORES_PATH`, `INFO_PATH` | RetroArch runtime paths |
| `JAWAKA_THEME` | local preview theme override |
| `JAWAKA_AUTODEMO` | `1` enables the short automated run-daemon flow |
| `JAWAKA_AUTODEMO_DELAY_MS` | auto-demo delay, default `1200` |

`JAWAKA_SDCARD_ROOT`, `JAWAKA_RUNTIME_DIR`, `JAWAKA_RETROARCH_BIN`, and
`JAWAKA_RETROARCH_CORES_DIR` remain compatibility aliases. New scripts and docs
should prefer the `SDCARD_PATH` / `UMRK_*` variables from the umbrella runtime
contract.

## SD Layout

Jawaka scans content from the Leaf/UMRK SD shape:

```text
Roms/<SYSTEM_CODE>/<title>.<ext>
Images/<SYSTEM_CODE>/<title>.png
Apps/<platform>/<Name>.pak/
Apps/shared/<Name>.pak/
BIOS/
Saves/
States/
Cheats/
.umrk/<platform>/library.db
```

For app paks, `pak.json.platform` must match the containing platform directory
or be `shared`. Icon paths are relative to the containing `.pak` directory
unless they are absolute. Flat `Apps/<Name>.pak/` entries are ignored.

## Settings

The current Settings tree includes:

```text
Appearance       color scheme, colors, layout (list style, fonts, font size,
                 tab switching), status bar
Display & Sound  brightness, refresh rate (60/90/120), black frame insertion
                 (120Hz only), audio output, volume, test sound
Lighting         MLP1 LED enable/mode/color/brightness/speed
Network          Wi-Fi scan/connect/forget and ADB enable/disable
Bluetooth        scan, pair/connect, disconnect/forget
Game Art         scrape artwork (all/per-system, missing or replace-all), live
                 scrape queue, artwork-type and region priority
Accounts         ScreenScraper / RetroAchievements sign-in
General          startup tab, auto-sleep, boot splash, game performance,
                 time zone, reset RetroArch config, unmount secondary SD
```

System Update and About are not in the Settings tree; they live in the **System**
menu (the Menu-button popup), which also offers a library rescan and the
session/power actions (return to launcher, sleep, exit to stock, reboot, power off).

Jawaka exports Catastrophe `CAT_*` appearance variables before launching
`jawaka-launcher`, `jawaka-menu`, and Catastrophe-based `.pak` apps. Apps should
consume the inherited environment rather than read Jawaka's SQLite DB.

## Repo Notes

- `scripts/mockgen.sh` creates the local mock SD-card tree.
- `third_party/cjson/` is vendored and used by the IPC/config paths.
- `third_party/catastrophe/` is still a placeholder; use `CATASTROPHE_DIR` or an
  adjacent `../Catastrophe` checkout.
- `docs/PLAN.md` and `docs/ARCHITECTURE_DECISIONS.md` are historical planning
  docs. Current behavior is best reflected by this README, the Makefile, and
  the implementation under `cmd/` and `internal/`.

## License

Jawaka is released under the MIT License. See `LICENSE`.
