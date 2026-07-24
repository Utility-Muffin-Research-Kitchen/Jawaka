# Rumble / haptics for Leaf (MLP1)

Status: **designed, not built.** Design settled with Eric on 2026-07-24 via an
on-device exploration of the motor plus a full design grill. Phase 1 (UI haptics)
is built first; Phase 2 (game rumble) is documented here and built after.

The MLP1 has a rumble motor. Stock LoongOS drives it; Leaf never has. This wires it up.

---

## 1. Hardware findings (verified on Puff)

The rumble motor is a **single PWM-driven motor**, not a Linux force-feedback input
device. Confirmed on device:

- The `Loong Gamepad` input nodes (`event4`/`event5`) report `EV=b` (SYN+KEY+ABS) with
  **no `EV_FF` bit** — so there is no `/dev/input` FF interface. Emulator/SDL rumble
  will not reach the motor without help (this drives the whole Phase 2 approach).
- The motor is **`/sys/class/pwm/pwmchip0/pwm0`** (`npwm=1`, a dedicated single channel).
- Control: `duty_cycle` = strength, `enable` = 1/0, `period` = frequency.
- **Period is `1,000,000 ns` (1 kHz)** — set by the stock startup `/etc/init.d/S50loong`
  (`echo 1000000 > .../pwm0/period`). libloong itself only writes `duty_cycle` + `enable`;
  it assumes the period is already set. On a fresh export the period reads back `0`, so
  **Leaf must set the period itself.**
- Vendor plumbing exists but Leaf will not use it: `libloong_gui.so` exposes
  `lgui::LSound::vibrateOnce(int)`, `setVibration(bool)`, `setVibrationLevel(int)`, and the
  stock `SOUND_PARAM` config carries `vibrateFb` / `vibrateLevel`. We drive the PWM
  directly instead (no C++ vendor-lib coupling).

### The stuck-motor lesson (do not repeat)

During exploration the motor got **stranded on** twice. Root causes, and the rule that
prevents them:

- `echo 0 > enable` does **not** reliably stop this motor — the output pin latches its
  last state.
- **Unexporting** the channel while it is driven latches it on.
- Under the stock default `polarity=inversed`, `duty_cycle=0` is **full on**, not off.

**Rule:** the reliable "off" is to *actively drive 0% output*, never to disable/unexport.
The drive model below is built around this.

---

## 2. Drive model (the technical core)

Configure the channel **once** at daemon init and then only ever modulate `duty_cycle`:

```
export → enable=0 → polarity=normal → period=1000000 → duty_cycle=0 → enable=1
```

From then on:

- **buzz** = `duty_cycle = <strength>`  (higher = stronger; 1,000,000 = full)
- **stop** = `duty_cycle = 0`  (channel stays enabled — 0% output = motor off)
- **never** toggle `enable`, **never** unexport.

Decisions and rationale:

- **`polarity=normal`** (not the stock `inversed`). Verified on device that normal polarity
  with a positive duty buzzes and `duty=0` is off. Normal gives the intuitive frame
  (higher duty = stronger, `0` = off) with no inversion math. Inversed would also work with
  `off = duty=period`, but normal is proven and simpler.
- **Always-enabled / modulate-duty-only** is what makes "off" bulletproof — it is impossible
  to strand the motor because the channel is always actively driving a defined level.
- **Energy:** keeping the channel enabled at 0% costs nothing meaningful. The motor draws
  zero at 0% duty (the only real consumer); the PWM block's idle clock/leakage is
  microamp-scale, dwarfed by SoC/panel/Wi-Fi. Not worth optimizing.
- **Forced-off on state transitions:** the daemon forces `duty_cycle=0` on **game launch,
  sleep/suspend, and shutdown**, so a pulse can never be left stranded across a transition.

### Strength & the "amplitude vs pattern" finding

On-device A/B testing showed **duty (amplitude) mostly changes the audible buzz, not the
perceived force** — a light vs strong duty is hard to tell apart by feel, easy by ear.
**Short sharp burst *patterns* differentiate far better** than amplitude. So:

- **Patterns** (burst count) carry *meaning* (see the vocabulary).
- **Strength** (duty) is a single user intensity control on top; it is a scale/ceiling, not
  the differentiator.

Tick shape that felt right in testing: **~40 ms on**, **~80 ms gap** between bursts in a
pattern. Strength → duty mapping is a slider percentage of the 1,000,000 ns period, **to be
tuned on device**: motor response is non-linear and low duty may not overcome stiction, so
there is likely a **hard floor (~40%)** below which the motor will not reliably move. Map
the slider `0–100%` onto `[floor%, 100%]` of period.

---

## 3. Architecture

- **jawakad owns the motor.** It is the root process already driving LED/brightness/PWM
  sysfs. It configures the channel at init, owns the event→pattern table and the tick
  timings, scales amplitude by the user's strength setting, and owns the safe-stop.
- **UI processes trigger by naming a semantic event.** The launcher / menu send a
  fire-and-forget `rumble` IPC action (mirroring the existing `led` / `brightness` /
  `hdmi_output` actions) carrying an *event name*, not raw duty/duration. The daemon maps the
  event → pattern → duty. This keeps all feel-tuning in one place and lets us retune patterns
  later without touching launcher/menu code.
  - IPC shape: `{ "action": "rumble", "event": "select" | "commit" | "blocked" | "nav" }`
  - Fire-and-forget (no wait for a reply) so per-press haptics add no perceptible latency.
- **Non-blocking pulses.** A small rumble worker thread in the daemon (like the screenshot
  worker) takes a pattern request and pulses `duty` with sleeps, so the IPC loop never
  blocks. A new request coalesces (restarts) rather than queueing, so rapid input does not
  back up a buzz train.

---

## 4. Phase 1 — UI haptics (build first)

### Vocabulary — event → pattern (fixed, not user-configurable)

Patterns are keyed to the **semantic weight of the action**, so users learn the language
(double = "into a game", triple = "nope") without configuring anything:

| Pattern       | Meaning              | Fires on |
|---------------|----------------------|----------|
| **Single tick** | routine / registered | cursor select, back/cancel, toggling a setting, a slider notch; and cursor **move** *only if* nav-tick is on |
| **Double tick** | commit / significant  | launching a game (leaving into it), confirming a modal/important dialog, applying a change |
| **Triple tick** | blocked / negative    | hitting a list boundary, a denied/locked action (e.g. a 5-Game-Mode lock), an operation error |

The UI emits named events (`RUMBLE_NAV`, `_SELECT`, `_COMMIT`, `_BLOCKED`); the daemon maps
`nav/select → single`, `commit → double`, `blocked → triple`.

### Settings — new "Controls & Feedback" section

A **new top-level Settings section, "Controls & Feedback"** (the current top level is
Appearance / Display & Sound / Lighting / Network / Bluetooth / … / General; internal page
name for "General" is `JW_SETTINGS_BEHAVIOR`). Rumble does not belong in Display & Sound.

Seed the section with:

- **Rumble** — master on/off. **Default ON.**
- **Strength** — a **percent slider** reusing the existing brightness/volume track+fill
  widget (`settings.c` ~1866–1871, L/R to adjust), default ~65%, with a **live preview
  buzz** as you drag so you feel the level while setting it. (Slider only — no named
  presets; the slider supersedes Light/Med/Strong.)
- **Navigation tick** — on/off, **default OFF** (per-move buzz is the most polarizing; opt-in).
  Greyed unless Rumble is on.
- **Screenshots (Menu+L1)** — **migrated here** out of General (it is a hotkey/input feature,
  not general behavior), so the new section launches with substance and General loses a
  grab-bag item.

DB keys: `rumble_enabled` (bool, default true), `rumble_strength` (int %, default ~65),
`rumble_nav` (bool, default false). The daemon caches these (like it caches
`screenshots_enabled`) so an incoming `rumble` action knows whether/how hard to buzz.

Room to grow in this section later: controller button mapping, menu A/B swap, stick-calibration
link, and the Phase-2 game-rumble toggle.

### Init & lifecycle

- At daemon start: configure the channel per §2 (set the period — Leaf must, since libloong
  does not).
- Force `duty=0` on game launch, sleep/suspend, shutdown.
- Respect `rumble_enabled` (drop all pulses when off) and `rumble_nav` (drop `nav` events
  when off) daemon-side.

---

## 5. Phase 2 — game rumble (documented; built after Phase 1)

Because the motor is not an FF/SDL device, emulator rumble needs an explicit path to the PWM.

### RetroArch cores (the bulk of systems) — Phase 2a

- Enable the deferred **`retroarch-builds/patches/common/0003-sysfs-rumble-fallback.patch`**
  in the RA build, targeting our PWM. The patch writes sysfs directly when the joypad
  driver's native rumble returns false (it always will here — no FF device).
- Because the drive model leaves the channel **always enabled at duty 0**, RA only has to
  **write `duty_cycle`** to buzz and `0` to stop — it never touches export/polarity/period/
  enable. Adapt the patch's motor mode to write the strength value to
  `/sys/class/pwm/pwmchip0/pwm0/duty_cycle`.
- **jawakad pre-arms the PWM and injects env at RA launch** (it already sets launch env via
  `jw__setenv_default`): e.g. `RUMBLE_SYSFS_PATH=/sys/class/pwm/pwmchip0/pwm0/duty_cycle`
  plus a scale, only when Leaf rumble + game-rumble are enabled. On game exit the daemon
  forces `duty=0` and reclaims the channel.
- RA core rumble (N64/PSX/GBA etc. emit it via the libretro rumble interface) scales into
  duty, **capped by the strength slider**.

### Standalone emulators (Flycast / PPSSPP / DraStic / mupen64) — Phase 2b

Each has its own vibration setting (`VirtualGamepadVibration`, `rumble_power`) but no path to
the motor. Each needs its own small patch to route its rumble to the same `duty_cycle` write.
**Deferred** — RA covers most systems first.

### Settings

A **Game rumble** sub-toggle in Controls & Feedback (default on), sharing the master Rumble
on/off and the strength slider (slider acts as the intensity ceiling; game supplies the
variable magnitude). Lets a user keep UI haptics without game rumble, or vice versa.

### Ownership / arbitration

No new arbitration: during a game there is no UI, so the daemon hands duty-writing to RA for
the session and reclaims it (forces off) on exit — still a single-owner PWM.

### Open Phase-2 details (flagged, not solved here)

- **sysfs permission:** `duty_cycle` is root-owned; the game may not run as root. Either the
  daemon `chmod`s the node writable around a game launch and reverts on exit, or confirm the
  emulator already runs as root. TBC.
- **Standalone scope:** decide per-emulator whether the payoff justifies the patch, or leave
  standalone game-rumble off.

---

## 6. To tune / verify on device

- Strength→duty curve and the **stiction floor** (the min duty that reliably moves the motor).
- Final tick/gap timings for single/double/triple (start ~40 ms on / ~80 ms gap).
- Confirm the live-preview-on-slide feel.
- Phase 2: sysfs write permission for the game process.

## 7. Files (anticipated)

- `cmd/jawakad/main.c` — rumble module: channel init, `jw__rumble_*` worker + event→pattern
  table + duty scaling + safe-stop, force-off on launch/sleep/shutdown, `rumble` IPC action,
  cache the three DB settings, Phase-2 env injection at RA launch.
- `internal/settings/settings.{c,h}` — new **Controls & Feedback** page + rows (Rumble,
  Strength slider, Navigation tick, migrated Screenshots); new DB keys; page enum + top-level
  list entry.
- `cmd/jawaka-launcher/main.c` (+ menu) — emit named rumble events at select / commit /
  boundary / (optional) nav sites; strength-slider live preview.
- `retroarch-builds/patches/common/0003-sysfs-rumble-fallback.patch` — Phase 2: enable +
  adapt to write `duty_cycle`.
- Docs (leaf-docs) at release time.
