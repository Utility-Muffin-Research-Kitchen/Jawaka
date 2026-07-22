# On-device screenshots (Leaf / Jawaka)

Capture whatever is on screen (launcher, in-game/RetroArch, or the 5-Game Mode
focus screen) with a button hotkey handled by the resident daemon, saved as a PNG
to the SD card. Works with Wi-Fi off (no ADB/SSH) because it is entirely on-device.

## Decisions (settled with Eric via /grill-me, 07-22)

- **Hotkey:** Menu + L1 (`BTN_MODE` + `BTN_TL`). Detected in the input proxy,
  **consumed** (neither reaches the running game), edge-triggered on the L1 press,
  and it suppresses the Menu-tap (like the Menu+Select switcher chord).
- **Two capture paths, dispatched on the foreground child:**
  - **In-game (RA running):** jawakad sends RA the `SCREENSHOT` network command
    (`jw_ra_screenshot`). RA writes the PNG to its `screenshot_directory`, which is
    the ephemeral tmpfs `<runtime_dir>/shots/` (Leaf already uses this for the
    paused-menu background — do NOT repoint it). jawakad copies the new file out to
    the persistent folder. RA shows its own "saved" toast.
  - **Launcher / focus mode (Weston up):** jawakad runs on-device `kmsgrab` to grab
    the composited CRTC scanout, converts XR24 (BGRX LE, 720x960, stride 2880) →
    RGB, **rotates 90° CW**, and PNG-encodes with `stb_image_write.h` (already on
    jawakad's include path).
- **Storage:** `/mnt/sdcard/Screenshots/` (top-level, flat, browsable, survives
  updates), PNG. In-game filename `<RomName>-YYYYMMDD-HHMMSS.png`; UI filename
  `screenshot-YYYYMMDD-HHMMSS.png`. jawakad owns the final file in both paths.
- **Feedback:** brief on-screen white flash in the UI (jawakad signals the launcher
  via `SIGUSR1`; captured frame is grabbed BEFORE the flash) + RA's own toast
  in-game. Covers focus mode (it is the launcher process).
- **Setting:** a "Screenshots" on/off toggle, **default OFF** (opt-in). The proxy
  callback checks it synchronously so that when disabled Menu+L1 forwards normally.

## Files

1. `internal/platform/input_proxy.h` — add `jw_input_screenshot_cb` typedef +
   `screenshot` field to `jw_input_proxy` (set post-init in jawakad; no init-signature
   change, so `init`/`init_watch` callers are untouched).
2. `internal/platform/input_proxy_mlp1.c` — in `jw__handle_key`, handle `BTN_TL`
   like `BTN_SELECT`: on press while `menu_held && !menu_forwarded`, call
   `proxy->screenshot`; if it returns true, set `chord_active` + a new
   `screenshot_chord_consumed` flag and drop the press; swallow the paired release.
3. `cmd/jawakad/main.c` — `jw__on_screenshot_hotkey(userdata)`: read
   `screenshots_enabled` (off → return false so L1 forwards); ~1s throttle; spawn a
   **detached worker thread** and return true. Worker dispatches on foreground child:
   UI → kmsgrab+encode+`SIGUSR1` flash; in-game → `jw_ra_screenshot` + poll `shots/`
   + copy out (named by ROM). Register `state->input_proxy.screenshot` after proxy init.
4. `cmd/jawaka-launcher/main.c` — `SIGUSR1` handler sets an atomic `flash_pending` +
   requests a frame; render loop draws a full-screen white overlay for ~1 frame, clears.
5. `internal/settings/settings.{c,h}` — "Screenshots" on/off row (mirror the BFI/HDMI
   toggle), DB key `screenshots_enabled`, default off.
6. `internal/platform/paths.c` — `jw_screenshots_dir()` (`<sdcard>/Screenshots`, mkdir
   on first use); expose the RA `shots/` runtime path for the in-game poll.

## Must validate on-device (esp. once a game runs)

1. A command-triggered RA `SCREENSHOT` writes to `shots/` AND shows a toast (the
   pause-background uses the same command/dir — confirm a user one lands + notifies).
2. Orientation of RA's screenshot (likely already landscape-correct; the kmsgrab path
   needs the 90° rotate).
3. `SIGUSR1` flash has no side effects in the launcher.

## Build order

- **Phase 1:** settings toggle + proxy trigger + UI/focus (kmsgrab) path + launcher
  flash. Testable immediately (launcher/focus).
- **Phase 2:** in-game RA path. Needs a running game to validate (#1/#2 above).
