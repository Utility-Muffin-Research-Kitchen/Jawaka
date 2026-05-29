# Jawaka MLP1 Device POC Plan

Date: 2026-05-23

Goal: get a safe proof-of-concept Jawaka launcher running on a Miniloong Pocket 1
through the existing marker-gated `loong_pangu` replacement path.

## Current Facts

### Device

- The attached device is RK3566 / Cortex-A55 / aarch64.
- OS is Buildroot 2021.11 on Linux 5.10.209.
- glibc is 2.38.
- Weston is running and exposes Wayland at `/run/wayland-0`.
- The stock GUI environment uses `XDG_RUNTIME_DIR=/var/run` and Weston-specific
  variables such as `WESTON_DRM_KEEP_RATIO=1`.
- SDL2, SDL2_ttf, SDL2_image, sqlite3, freetype, harfbuzz, libpng, and jpeg are
  already present under `/usr/lib`.
- Stock SQLite supports FTS5, so Jawaka's current schema is viable on-device.
- Kernel/DRM reports the panel as `720x960`; the desired launcher layout is
  expected to be logical landscape `960x720`.
- `/mnt/sdcard` was not mounted during this probe, so SD-card tests still need
  a mounted FAT32/ext4 card.
- The switcher wrapper is already installed as `/loong/loong_pangu`, with the
  stock binary backed up at `/loong/loong_pangu.stock.umrk`.
- The wrapper has already proven the contract: marker present -> copy SD bundle
  to `/tmp/umrk-launcher` -> exec custom `bin/loong_pangu`; marker absent ->
  stock fallback.
- The stock firmware includes RetroArch:
  `/userdata/app/retroarch/retroarch`.
- Stock libretro cores are present under:
  `/userdata/app/retroarch/.config/cores/`.

### Input

The main gamepad is `/dev/input/event4`, named `Loong Gamepad`.

It exposes:

- `BTN_SOUTH`, `BTN_EAST`, `BTN_NORTH`, `BTN_WEST`
- `BTN_TL`, `BTN_TR`, `BTN_TL2`, `BTN_TR2`
- `BTN_SELECT`, `BTN_START`, `BTN_MODE`, `BTN_THUMBL`
- `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`
- `ABS_X`, `ABS_Y`, `ABS_HAT0X`, `ABS_HAT0Y`

This is close to a normal Linux joystick, but physical button semantics still
need one capture pass before the Catastrophe mapping is locked.

### Build State

- `mlp1-toolchain` has a working local Docker image:
  `ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local`.
- The launcher-switcher POC binary verifies against the target ABI:
  aarch64 ELF, interpreter `/lib/ld-linux-aarch64.so.1`, max glibc 2.34.
- Jawaka's root Makefile is Mac-first and has no `mlp1` port target yet.
- A one-off Jawaka cross-build fails with strict `-std=c11` because Buildroot's
  glibc headers hide `PATH_MAX` and POSIX/GNU functions.
- The same one-off cross-build succeeds with `CSTD=-std=gnu11`.
- The produced `jawakad`, `jawaka-launcher`, and `jawaka-menu` binaries verify
  against the target ABI.
- A non-UI on-device smoke of the cross-built `jawakad --daemon-only` succeeds
  from `/tmp`, creates `.umrk/library.db`, and initializes the FTS5 tables.

## What Is Missing

### 1. Catastrophe Needs An MLP1 Platform

Adding `mlp1` only in Jawaka is not enough. Catastrophe is where device mode,
input, screen metrics, theme defaults, status assets, and power-button handling
are decided.

Needed:

- Add `PLATFORM_MLP1` and `CAT_PLATFORM_NAME "mlp1"`.
- Mark it as `CAT_PLATFORM_IS_DEVICE=1`.
- Add a Loong Gamepad mapping rather than reusing TrimUI or Miyoo Flip mapping.
- Decide how to handle `KEY_VOLUMEUP` and `KEY_VOLUMEDOWN`.
- Treat MLP1 as Wayland-first for the stock OS POC.
- Add MLP1 display metrics. Start from logical `960x720`, but respect the
  probed native `720x960`/rotation behavior.
- Add MLP1 font/theme/status-asset search defaults, or rely on explicit
  `CAT_THEMES_DIR`, `CAT_FONTS_DIR`, and `CAT_STATUS_ASSETS_DIR` from the
  launcher entrypoint.
- Audit `cat_set_power_handler()` on MLP1. The POC log saw repeated Power key
  events, so power handling should be intentionally mapped or disabled until
  confirmed.

### 2. Jawaka Needs An MLP1 Port Target

Needed:

- Add `ports/mlp1/Makefile`.
- Add a root `make mlp1` target.
- Use the MLP1 toolchain image and the existing SDK environment.
- Build with `-std=gnu11` or an explicit feature macro such as
  `_DEFAULT_SOURCE`.
- Compile UI binaries with `-DPLATFORM_MLP1`.
- Keep target outputs separate from the host Mac build, for example
  `build/mlp1/bin/`.
- Run `mlp1-toolchain/scripts/verify-binary.sh` on every produced binary.

### 3. Jawaka's Platform Paths Need To Stop Being Mac-Only

Current path logic still assumes:

- `UMRK/mac/defaults/cores.json`
- `UMRK/mac/cores`
- `.dylib` libretro core names
- macOS RetroArch paths

Needed:

- Introduce a platform id, initially compile-time or env-driven:
  `mlp1`.
- Resolve defaults from `UMRK/mlp1/defaults/`.
- Use `.so` core names for MLP1.
- Default RetroArch binary to `/userdata/app/retroarch/retroarch` for the
  first POC.
- Default cores dir to `/userdata/app/retroarch/.config/cores` for the first
  POC.
- Keep `JAWAKA_RETROARCH_BIN` and `JAWAKA_RETROARCH_CORES_DIR` as overrides.
- Keep runtime sockets in `/tmp`.
- Keep library state on the SD root under `.umrk/`.

### 4. The Switcher Needs A Jawaka Bundle Mode

The switcher currently packages `src/poc_launcher.c` as
`umrk-launcher/bin/loong_pangu`.

Needed:

- Add a Jawaka packaging target, not replacing the tiny POC target yet.
- Package all three Jawaka binaries:
  `jawakad`, `jawaka-launcher`, `jawaka-menu`.
- Preserve the wrapper contract by making `bin/loong_pangu` start the daemon.
  Recommended POC approach: copy the `jawakad` binary to `bin/loong_pangu` so
  the foreground process still has the stock entrypoint name.
- Include Jawaka themes under the staged bundle, for example
  `umrk-launcher/res/themes/`.
- Include Catastrophe fonts under `umrk-launcher/res/fonts/`.
- Include status assets or set `CAT_STATUS_ASSETS_DIR` only after assets are
  staged.
- Extend `adb-stage-sd-bundle.sh` so it can stage:
  `/mnt/sdcard/umrk-launcher/` and `/mnt/sdcard/UMRK/mlp1/`.
- Keep stock fallback and uninstall exactly as-is.

### 5. Add An MLP1 Platform Payload Contract

Mirror the existing Mac contract with a device-specific tree:

```text
/mnt/sdcard/
  .umrk-launcher
  umrk-launcher/
    bin/
      loong_pangu
      jawakad
      jawaka-launcher
      jawaka-menu
    res/
      themes/
      fonts/
      assets/
  UMRK/
    mlp1/
      manifest.json
      defaults/
        cores.json
        retroarch.cfg
```

Initial `cores.json` should point to the stock core filenames under
`/userdata/app/retroarch/.config/cores`.

Initial `retroarch.cfg` should be portable and avoid absolute save/state paths.
Jawaka should continue writing per-launch absolute paths through
`--appendconfig`.

### 6. Decide First POC Scope

Recommended first POC:

1. Jawaka daemon boots through the switcher.
2. `jawaka-launcher` opens fullscreen under stock Weston.
3. It scans `/mnt/sdcard/Roms`, `/mnt/sdcard/Images`, and `/mnt/sdcard/Apps`.
4. Menu handoff works.
5. Marker removal falls back to stock.

Recommended second POC:

1. Select a real ROM.
2. Launcher sends `launch-game`.
3. Daemon exits/hides launcher and spawns stock RetroArch.
4. RetroArch uses stock cores plus Jawaka's per-launch save/state config.
5. When RetroArch exits, daemon respawns launcher.

Using stock RetroArch/cores is the fastest path for POC. Building and packaging
UMRK-owned RetroArch and MLP1 cores should be a follow-up once launcher bring-up
is stable.

## Subproject Responsibilities

### `miniloong-launcher-switcher`

Owns installation, fallback, recovery, and staging.

Changes:

- Add Jawaka bundle packaging.
- Add ADB run/stage helpers for Jawaka.
- Export the runtime env needed by Jawaka:
  `XDG_RUNTIME_DIR=/var/run`, `SDL_VIDEODRIVER=wayland`,
  `JAWAKA_SDCARD_ROOT=/mnt/sdcard`, `CAT_THEMES_DIR`, `CAT_FONTS_DIR`, and
  optional RetroArch override vars.
- Preserve the small SDL POC as the recovery-grade minimal test.

### `mlp1-toolchain`

Owns cross-compilation and binary verification.

Changes:

- No blocking SDK change found for the launcher.
- Consider adding helper targets/scripts for building Jawaka, but do not move
  Jawaka build logic into the toolchain repo.
- Revisit the Buildroot sqlite defconfig later so it explicitly enables FTS5,
  even though the stock device already has FTS5.

### `Catastrophe`

Owns device platform behavior.

Changes:

- Add `PLATFORM_MLP1`.
- Add input mapping and display defaults.
- Add or document MLP1 resource lookup defaults.
- Audit power handling on MLP1.
- Keep the change generic enough that Jawaka does not need to special-case SDL
  input.

### `Jawaka`

Owns launcher behavior and platform path resolution.

Changes:

- Add `ports/mlp1`.
- Add MLP1 defaults/manifest generation.
- Make platform path resolution data-driven by `mlp1`.
- Keep SD state in `.umrk/`.
- Keep daemon as the normal entrypoint.
- Add device logging redirection, likely from the `loong_pangu` entrypoint or
  wrapper, so logs land under `/userdata/umrk-launcher.log`.

### `retroarch-builds`

Not blocking for first POC if stock RetroArch is used.

Later:

- Add or restore an MLP1 build lane for an owned RetroArch binary.
- Package it under `UMRK/mlp1/retroarch/`.

### `Cores-spruce`

Not blocking for first POC if stock cores are used.

Later:

- Add or adapt an MLP1 core build lane if stock cores should not be depended on.
- Package `.so` cores and `.info` files under `UMRK/mlp1/cores` and
  `UMRK/mlp1/info`.

### `miniloong-adb-keeper`

No changes required for Jawaka POC.

It remains part of the recovery and iteration story because ADB access is
needed after stock GUI/storage services change USB mode.

## Execution Order

### Phase 0: Recovery Guardrails

- Confirm `/loong/loong_pangu.stock.umrk` exists before each install test.
- Confirm `/usr/bin/umrk-launcher-switcher-uninstall.sh` exists.
- Keep `make adb-uninstall-wrapper` tested.
- Keep marker-off fallback tested before replacing the POC bundle with Jawaka.

### Phase 1: Make Jawaka Cross-Build Official

- Add `ports/mlp1/Makefile`.
- Add root `make mlp1`.
- Switch target C standard to `gnu11` or define the needed feature macros.
- Verify `jawakad`, `jawaka-launcher`, and `jawaka-menu`.
- Keep warnings visible, but do not block POC on existing path truncation
  warnings unless they affect the MLP1 staging paths.

Acceptance:

- `make mlp1` succeeds from `Jawaka/`.
- All three binaries are aarch64 ELF with interpreter
  `/lib/ld-linux-aarch64.so.1`.

### Phase 2: Add Catastrophe MLP1 Platform

- Add `PLATFORM_MLP1`.
- Add Loong Gamepad mapping.
- Add MLP1 metrics and resource defaults.
- Build Jawaka UI binaries with `-DPLATFORM_MLP1`.

Acceptance:

- The UI initializes as a device build, not as generic Linux desktop preview.
- Input events map to Catastrophe buttons.

### Phase 3: Add Jawaka MLP1 Runtime Paths

- Add `UMRK/mlp1/defaults/cores.json`.
- Point RetroArch defaults at stock paths for POC.
- Make core suffix `.so`.
- Keep env overrides.

Acceptance:

- `launch-game` validation can find stock RetroArch and at least one stock
  core on-device.

### Phase 4: Package Jawaka Behind The Switcher

- Add switcher target: `make jawaka-package` or equivalent.
- Stage Jawaka binaries/resources as `umrk-launcher`.
- Stage MLP1 defaults as `UMRK/mlp1`.
- Use `bin/loong_pangu` as the daemon entrypoint.

Acceptance:

- `adb-stage-sd-bundle` can stage Jawaka to a mounted SD card.
- Marker-on starts Jawaka.
- Marker-off starts stock.

### Phase 5: Non-Installed ADB UI Smoke

- Push the Jawaka bundle to `/tmp`.
- Pause stock `loong_pangu`.
- Run Jawaka with `JAWAKA_AUTODEMO=1` and a short timeout.
- Resume stock.

Acceptance:

- Jawaka initializes SDL/Wayland.
- It renders at the intended size/orientation.
- Daemon -> launcher -> menu -> shutdown auto-demo completes.

### Phase 6: Installed Switcher POC

- Stage bundle on SD.
- Enable `.umrk-launcher`.
- Restart `/etc/init.d/S50loong` or reboot.
- Validate logs under `/userdata/umrk-launcher.log`.
- Remove marker and restart to prove fallback.

Acceptance:

- Device boots into Jawaka when marker is present.
- Device boots into stock when marker is absent.
- ADB uninstall restores stock entrypoint.

### Phase 7: Game Launch POC

- Put one known-good ROM on SD.
- Use stock RetroArch and stock cores.
- Select ROM in Jawaka.
- Confirm RetroArch starts and returns to Jawaka on exit.

Acceptance:

- One real ROM launches from Jawaka on MLP1.
- Saves/states go to the intended SD-root folders.
- Launcher returns after RetroArch exits.

## Grill Decisions

Question 1: Should the first UI bring-up treat MLP1 as logical landscape
`960x720`, even though the kernel reports `720x960`?

Decision: yes. Treat `960x720` as the Jawaka design target, but add a tiny
display probe/log in the first UI smoke so we can confirm what SDL/Wayland
actually reports before adding any manual rotation logic.

Question 2: For the first game-launch POC, should we use the stock RetroArch
and stock cores already on the MLP1 instead of building/package-owning our own
MLP1 RetroArch stack first?

Decision: yes. Use stock RetroArch and stock cores for the first launch POC.
This gets Jawaka to a real device game-launch milestone quickly, while keeping
`retroarch-builds` and `Cores-spruce` as the later owned-artifact path once
launcher bring-up is stable.

Question 3: Should the first installed Jawaka POC replace the current switcher
POC bundle, or should we add a separate `jawaka` packaging/staging target while
keeping the tiny SDL POC available?

Decision: keep the tiny SDL POC and add a separate Jawaka target. The tiny POC
remains a known-good recovery-grade visual test, but the Jawaka switcher payload
must also be self-contained for a fresh stock device with no launcher POC
already installed. A fresh-install payload must install the wrapper, create the
stock `/loong/loong_pangu.stock.umrk` backup if absent, stage the complete
Jawaka bundle, and be able to enable the marker without depending on prior
`/mnt/sdcard/umrk-launcher` contents.

Question 4: Should the Jawaka SD payload default to enabling the marker on fresh
install, or should install and activation be separate steps?

Decision: keep install and activation separate. Generate/stage an install-only
payload with marker off by default, and provide explicit activation commands
for quick ADB testing. It is still useful to keep an "activated" payload/target
for one-card bring-up once the Jawaka bundle is trusted, but the safe default is
wrapper installed, bundle staged, marker absent.

Question 5: Should our first Jawaka UI smoke run from `/tmp` over ADB before
we exercise the installed switcher path?

Decision: yes. Add an ADB smoke target that builds the MLP1 Jawaka bundle,
pushes it to `/tmp`, pauses stock `loong_pangu`, runs the daemon/launcher/menu
flow with a short timeout or autodemo, captures logs, and then resumes stock.
This tests SDL/Wayland, orientation, input initialization, and process handoff
without touching `/loong/loong_pangu` or requiring SD marker activation.

Question 6: Should the `/tmp` Jawaka ADB smoke live in `Jawaka/` or in
`miniloong-launcher-switcher/`?

Decision: start it in `Jawaka/` as a target such as `make mlp1-adb-smoke`.
It primarily validates Jawaka's cross-build and runtime behavior. The switcher
should consume Jawaka's packaged output later for install/stage flows.

Question 7: For the first MLP1 Jawaka build target, should we implement the
proper `PLATFORM_MLP1` Catastrophe platform immediately, or first add a
temporary generic Linux/Wayland build just to run the UI?

Decision: implement `PLATFORM_MLP1` immediately. A generic Linux path would
hide the exact device facts the POC needs to validate: device-mode fullscreen
behavior, Loong input mapping, resource lookup, and power handling.

Question 8: Should the first MLP1 Catastrophe input mapping treat the Loong
Gamepad as a raw joystick using the Linux button/axis codes we probed, with a
short ADB input-capture pass to confirm physical labels?

Decision: yes, but explicitly map both navigation devices. The MLP1 exposes a
d-pad as `ABS_HAT0X`/`ABS_HAT0Y` and an analog stick as `ABS_X`/`ABS_Y`; both
should map into Catastrophe directional buttons. Button labels still need a
short capture pass where each physical control is pressed once before A/B/X/Y,
shoulders, menu, start/select, stick-click, and volume behavior are locked.

Question 9: Should volume up/down be handled inside Catastrophe/Jawaka for the
first POC, or left to stock services?

Decision: leave volume up/down to stock services initially. The MLP1 mapping
may log `KEY_VOLUMEUP` and `KEY_VOLUMEDOWN` during input capture, but Jawaka
should not bind them until we understand which stock service owns volume or
brightness behavior.

Question 10: Should Jawaka's first MLP1 payload keep launcher state on the SD
card under `.umrk/`, or use `/userdata` while SD handling is still being
figured out?

Decision: keep real launcher state on the SD card under `.umrk/`. ADB smoke
tests may set `JAWAKA_SDCARD_ROOT=/tmp/...` so they do not depend on SD
mounting, but installed payloads should preserve the portable SD-root model.

Question 11: Should the switcher's fresh-install SD payload include the Jawaka
bundle itself, or should install only place the wrapper and expect a later
ADB/SD staging step?

Decision: include the full Jawaka bundle in the fresh-install SD payload. The
marker still stays off by default, but a new stock device should be fully
prepared from one SD install without needing a prior ADB staging step.

Question 12: For the `bin/loong_pangu` entrypoint in the Jawaka bundle, should
we copy/rename `jawakad` to `loong_pangu`, or use a small shell wrapper that
execs `jawakad` with environment/log setup?

Decision: use a small shell wrapper. It can export `CAT_*`, `JAWAKA_*`, and
`SDL_*` variables, redirect logs, run preflight checks, and then `exec`
`./jawakad`. The outer switcher remains the authoritative recovery gate:
marker absent means stock GUI; marker present means attempt Jawaka. The marker
must remain the only activation switch for installed payloads.

Recovery rule: the Jawaka entrypoint wrapper must fail closed. If required
files are missing, `jawakad` is not executable, the SD root is absent, or the
environment cannot be prepared, it should log the reason and exit nonzero so
the outer switcher / stock service path can recover. Fresh install still leaves
`.umrk-launcher` absent by default.

Question 13: Should we add crash-loop protection to the switcher/Jawaka
entrypoint, so repeated Jawaka failures automatically disable the marker and
fall back to stock?

Decision: yes. The installed path needs crash-loop protection before it is
used as a fresh-device flow. Keep it simple: record failure timestamps/counts
under `/userdata`, and after a small threshold such as 3 failed starts within a
short window, remove or rename `/mnt/sdcard/.umrk-launcher`, log the reason,
and exec stock. This protects devices from bad Jawaka bundles even when ADB is
not available.

Question 14: Should crash-loop protection live in the outer switcher wrapper,
the inner Jawaka `bin/loong_pangu` entrypoint, or both?

Decision: put crash-loop protection in the outer switcher wrapper. It is the
small trusted shim that always runs before Jawaka and already knows how to exec
stock. The inner Jawaka entrypoint should still preflight and log clearly, but
the outer wrapper owns counting failures and disabling the marker.

## Live Device Checkpoint: 2026-05-23

ADB target `b1622a9e81b735ad` now has `/mnt/sdcard` mounted read-write as
FAT/vfat at `/mnt/sdcard`.

Completed on device:

- Staged the Jawaka SD bundle with marker off via
  `make adb-stage-jawaka-sd-bundle-no-marker`.
- Installed the updated guarded switcher wrapper via `make adb-install-wrapper`.
  The stock backup remained preserved at `/loong/loong_pangu.stock.umrk`.
- Verified installed wrapper hash matches local `device/loong_pangu.wrapper`.
- Restarted the Loong stack with marker absent and confirmed stock fallback:
  wrapper logged `marker absent, using stock gui` and stock `loong_pangu`
  started.
- Enabled `/mnt/sdcard/.umrk-launcher`, restarted the Loong stack, and confirmed
  the installed switcher path launches Jawaka from `/tmp/umrk-launcher`.

Observed active Jawaka state:

- `jawakad` and `jawaka-launcher` are running.
- Marker remains present; no `.umrk-launcher.disabled.*` crash-loop marker was
  created.
- Crash guard state is `count=1`, expected for the first guarded activation.
- Catastrophe reports platform `mlp1`, joystick `Loong Gamepad`, native display
  mode `960x720`, screen size `960x720`, status assets loaded from the staged
  bundle, and power handler disabled pending audit.
- Jawaka created `/mnt/sdcard/.umrk/library.db`.
- Library scan indexed `0` games because the mounted SD currently contains no
  ROM files beyond the staged launcher/install payload.

Remaining immediate checks:

- Capture physical input labels for d-pad, analog stick, face buttons,
  shoulders, start/select/menu, stick-click, and volume keys.
- Add a small ROM fixture or real ROM folder on SD to verify discovery and the
  stock RetroArch launch path.
- Decide whether to leave marker enabled for hands-on UI testing or disable it
  before any unattended reboot.

## MLP1 Input Capture: 2026-05-23

Added `make mlp1-adb-input-capture` in `Jawaka/`. It finds the Loong Gamepad
event node, runs `evtest --grab`, and stores local raw/summary logs under
`Jawaka/build/mlp1-input-capture/`.

Capture log:

- Raw:
  `Jawaka/build/mlp1-input-capture/20260523-235427-evtest.log`
- Summary:
  `Jawaka/build/mlp1-input-capture/20260523-235427-summary.txt`

Confirmed physical control mapping from the requested press order:

- D-pad up/down: `ABS_HAT0Y` `-1` / `+1`
- D-pad left/right: `ABS_HAT0X` `-1` / `+1`
- Analog stick up/down: `ABS_Y` negative / positive
- Analog stick left/right: `ABS_X` negative / positive
- A: `BTN_EAST`
- B: `BTN_SOUTH`
- X: `BTN_NORTH`
- Y: `BTN_WEST`
- L1/R1: `BTN_TL` / `BTN_TR`
- L2/R2: `BTN_TL2` / `BTN_TR2`
- Select/Start/Menu: `BTN_SELECT` / `BTN_START` / `BTN_MODE`
- Stick-click: `BTN_THUMBL`
- Volume down/up: `KEY_VOLUMEDOWN` / `KEY_VOLUMEUP`

Code update made from capture:

- `Catastrophe/include/catastrophe.h` now maps the MLP1 face buttons to the
  captured physical labels: A=`BTN_EAST`, B=`BTN_SOUTH`, X=`BTN_NORTH`,
  Y=`BTN_WEST`.
- Volume keys remain intentionally unbound in Jawaka/Catastrophe for now.

Follow-up fix found during restage:

- Running the inner Jawaka entrypoint as a shell wrapper that directly execs
  `jawakad` made the process table show `jawakad` instead of `loong_pangu`.
  Stock `S50loong restart` did not kill those old daemon processes, so repeated
  restarts could leave duplicate Jawaka daemons/launchers.
- The switcher wrapper now performs PID-based stale cleanup for `jawakad`,
  `jawaka-launcher`, and `jawaka-menu` before launching the staged custom
  bundle.
- The Jawaka entrypoint now copies `jawakad` to
  `/tmp/jawaka-runtime/loong_pangu`, copies `jawaka-launcher` and `jawaka-menu`
  beside it, and execs the runtime `loong_pangu`. This preserves the shell
  preflight/env/log setup while making stock process management see a
  `loong_pangu` daemon again.
- A broken intermediate runtime proved the crash-loop guard works: after three
  failed starts inside the guard window, the wrapper renamed the marker to a
  `.disabled.<timestamp>` file and started stock. After fixing the runtime
  child-binary path, the disabled marker artifact was removed and the current
  device state is clean.

Current post-fix device state:

- Marker present at `/mnt/sdcard/.umrk-launcher`.
- Exactly one `loong_pangu` daemon and one `jawaka-launcher` process are
  running; no stale `jawakad` processes remain.
- `build/sd` was regenerated as an unmarked-safe fresh-install payload with the
  input mapping and process-name fixes.

## SD-Exec Switcher Rework: 2026-05-24

Implemented direct SD execution for the MLP1 Jawaka switcher path.

Key changes:

- The switcher wrapper now remounts `/mnt/sdcard` as executable only when
  `/mnt/sdcard/.umrk-launcher` is present. It preserves existing mount options,
  replaces `noexec` with `exec`, and adds `nosuid`.
- Marker-absent stock fallback now best-effort restores `/mnt/sdcard` to
  `noexec`.
- If executable remount fails, the wrapper disables the marker, restores
  `noexec`, and starts stock.
- The Jawaka package now installs the native daemon ELF as
  `/mnt/sdcard/umrk-launcher/bin/loong_pangu`; the old inner shell entrypoint is
  no longer packaged.
- The outer switcher wrapper owns Jawaka env setup, resource preflight,
  logging redirection, stale Jawaka child cleanup, and direct SD exec.
- Jawaka now verifies that SD-hosted app launchers and SD-hosted RetroArch cores
  are not on a `noexec` SD mount before launching them. It logs a clear
  switcher-remount regression message and fails cleanly; it does not remount.

Validation:

- `sh -n` passed for the wrapper and generated installer.
- Clean MLP1 Jawaka rebuild completed in the toolchain container.
- `build/sd/umrk-launcher/bin/loong_pangu` is an AArch64 ELF daemon binary, not
  a shell wrapper, and no `bin/jawakad` is packaged.
- All three generated ARM binaries passed `verify-binary.sh` in the MLP1
  toolchain container.
- `make jawaka-sd-payload` regenerated an unmarked-safe fresh-install payload.
- Live marker-on ADB validation confirmed:
  `/mnt/sdcard` remounted without `noexec`, `loong_pangu` executed from
  `/mnt/sdcard/umrk-launcher/bin/loong_pangu`, `jawaka-launcher` executed from
  `/mnt/sdcard/umrk-launcher/bin/jawaka-launcher`, and the process table had one
  daemon plus one launcher.
- Live marker-off ADB validation confirmed:
  stock `/loong/loong_pangu.stock.umrk` started, `/mnt/sdcard` returned to
  `noexec`, and stale Jawaka children were removed.

Device state after validation was restored to the pre-test marker/mount state:
marker present, `/mnt/sdcard` mounted `noexec`. Because the marker is present,
the next Loong restart will use the new wrapper and remount SD as exec before
starting Jawaka.
