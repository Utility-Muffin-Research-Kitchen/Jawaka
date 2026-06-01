# Jawaka IGM Background (MLP1)

Date: 2026-06-01

Goal: the in-game menu opens snappily over the paused game with the game already
visible behind it — no late "pop-in" of the background.

## Approach (shipped)

The IGM is the normal opaque Catastrophe SDL menu (so the header, status bar,
button labels, themes and all Jawaka settings stay correct). The paused frame is
used as the menu background, captured straight from the DRM scanout with the
on-device `kmsgrab` tool and uploaded as a texture.

Key property: the frame is grabbed **synchronously, before the window is mapped**
(`jw__ingame_capture_still()` runs ahead of `cat_show_window()` in
`cmd/jawaka-menu/main.c`). The very first visible frame already shows the game
behind the menu, so there is no pop-in. Since the game is frozen on screen at the
moment Menu is pressed, the reveal is seamless.

Flow:
1. `jawakad` pauses RetroArch, then signals the resident menu (no screenshot).
2. The menu grabs the scanout (`kmsgrab --crtc <id>`), rotates it 90° CW into the
   960x720 landscape space, forces opaque alpha, uploads an ARGB texture.
3. The menu maps its window and draws: captured frame + dim scrim + IGM UI.
4. Header shows the resolved game title (`games.name` by `rom_path`, else cleaned
   ROM basename) and console display name (RA catalog, else raw system id).

The CRTC id is probed once at warm-up (the no-arg `kmsgrab` probe is ~2s) and
cached; per-open capture is the `kmsgrab --crtc` read only (~90-170ms today).

## Why not transparency / a native overlay

Both were tried and rejected on the MLP1 stock stack:

- **SDL transparent window**: every SDL present path (accelerated and software)
  goes through the proprietary Mali `mali_buffer_sharing` Wayland extension and
  lands as an opaque `XBGR8888` buffer (no alpha), so Weston never blends it. Not
  fixable from SDL. (Raw `wl_shm` ARGB *does* composite — proven by `jawaka-osd`.)
- **Native `wl_shm` overlay**: works (transparent over the game), but re-implements
  the IGM drawing and drifts from Catastrophe (wrong button labels etc.). Dropped
  to keep a single, correct Catastrophe-rendered IGM.

## Possible follow-up

Capture is the only remaining cost in the open path. Replacing the `kmsgrab`
subprocess with an in-process libdrm read (open the DRM fd once at warm-up, then
map the current scanout FB and `memcpy`) should cut capture to ~20-40ms for a
snappier (~60-80ms) open. Risk: replicating the vendor `kmsgrab` FB-mapping
method (dumb-buffer `MAP_DUMB` vs Mali GEM PRIME) without its source.
