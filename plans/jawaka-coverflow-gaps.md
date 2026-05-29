# Jawaka Coverflow — gaps after Sonnet's pass

Reviewed: uncommitted changes in `Jawaka/` and `Catastrophe/` against [jawaka-coverflow-theme.md](jawaka-coverflow-theme.md).

**Verdict:** the implementation is ~90% complete and structurally faithful to the plan. Build is clean (`make` succeeds, no new warnings). Code matches the plan section-by-section. Two real bugs and one delivery gap remain; rest is cosmetic.

## Status summary

| Plan section | Status |
|---|---|
| 3.1 Catastrophe enum + struct + defaults + parser + accessors | ✅ matches plan |
| 3.2.1 `jw_coverflow_anim` struct on launcher state | ✅ |
| 3.2.2 `jw__load_system_icon` | ⚠️ wrong roms-root resolution (see G1) |
| 3.2.3 animation helpers (ease-out-cubic, visual cursor, start_anim) | ✅ |
| 3.2.4 input handler (LEFT/RIGHT/UP/DOWN behaviour) | ✅ functional; minor style (G4) |
| 3.2.5 `jw__render_coverflow` two-pass draw + label | ✅ |
| 3.2.6 dispatch + list-builder branch | ✅ |
| 3.3 theme dir + stylesheet.json + LICENSE-ASSETS.md | ✅ |
| 3.4 `scripts/fetch-coverflow-icons.sh` | ✅ committed and executable; not yet run (G2) |
| 3.5 hand-authored `_default.png` / `_tools.png` | ✅ both present, 256×256 RGBA |
| Build & integration | ✅ compiles clean, logging includes `coverflow` |
| Verification (plan §5) | ❌ not yet performed (G3) |

## Gaps to close

### G1 — Wrong roms root for user-override icon lookup (bug, device-only)

`jw__load_system_icon` at [cmd/jawaka-launcher/main.c:997-999](Jawaka/cmd/jawaka-launcher/main.c:997) reads:

```c
const char *roms_root = getenv("JAWAKA_ROMS_DIR");
if (!roms_root || !roms_root[0]) roms_root = "./mock-sdcard/Roms";
snprintf(path, sizeof(path), "%s/%s/icon.png", roms_root, system_code);
```

This works on Mac in the dev sandbox but on the MLP1 device the sdcard lives at whatever `jw_sdcard_root()` returns (see [internal/platform/paths.c:239](Jawaka/internal/platform/paths.c:239) — `jw_sdcard_root()` is the canonical resolver, also used by `internal/discovery/discovery.c:50-52` to build the roms path). Hardcoded `./mock-sdcard/Roms` will never resolve on device, so user-supplied icons in `/Roms/<SYSTEM>/icon.png` are silently invisible there.

**Fix:** replace the env-var dance with the platform helper:

```c
if (system_code[0] != '_') {
    char *sdcard_root = jw_sdcard_root();
    if (sdcard_root) {
        snprintf(path, sizeof(path), "%s/Roms/%s/icon.png", sdcard_root, system_code);
        SDL_Texture *t = jw__load_cached_image(path, out_w, out_h);
        free(sdcard_root);
        if (t) return t;
    }
}
```

`jw_sdcard_root()` returns a heap-allocated string — must `free()` it. Mirrors the pattern used in [internal/discovery/discovery.c:50](Jawaka/internal/discovery/discovery.c:50) and [internal/platform/paths.c:248-258](Jawaka/internal/platform/paths.c:248).

Drop the `JAWAKA_ROMS_DIR` env-var path entirely — it's not used elsewhere in the codebase and adds an undocumented convention. The mock-sdcard fallback inside `jw_sdcard_root()` ([paths.c:244](Jawaka/internal/platform/paths.c:244)) already handles the dev case.

Update the doc-comment above the function to reflect the actual resolution order.

### G2 — Bundled console PNGs not in the repo yet

Only `_default.png` and `_tools.png` exist under [res/themes/Jawaka-Coverflow/system_icons/](Jawaka/res/themes/Jawaka-Coverflow/system_icons/). The 14 libretro Systematic icons that the theme's whole identity rests on are not committed — they're produced by the script but the script hasn't been run.

Today, the theme renders every real system (GB, SFC, MD, etc.) as `_default.png`. The visual outcome is identical to running coverflow with no icons.

**Fix:**

1. Run `./scripts/fetch-coverflow-icons.sh` from the Jawaka repo root.
2. Spot-check the output — 14 PNGs in `res/themes/Jawaka-Coverflow/system_icons/`, each non-empty, each opens as a PNG (`file system_icons/*.png` should report all RGBA).
3. **Decide on commit strategy.** Two options:
   - **Commit the PNGs.** Faster onboarding, repo grows by maybe 1-2 MB. Matches what the plan called for. Add a note in `LICENSE-ASSETS.md` (already there) and confirm the CC BY-SA 4.0 attribution is in the repo.
   - **Don't commit; require the script.** Smaller repo, but every contributor and every CI run needs network access. Probably worse — pick this only if asset licence-compliance review wants attribution kept distinct.

   The plan said **commit them**; recommend sticking with that unless legal review pushes back.

4. If the script fails for any one mapping (URL 404), investigate that one entry — libretro display names occasionally use hyphens or apostrophes that need re-encoding. The likely candidates to break first: `SNK - Neo Geo` (sometimes named "SNK - Neo Geo MVS"), `NEC - PC Engine - TurboGrafx 16` (path includes `–` em-dash in some commits). Fix the mapping in the script if so, re-run, re-commit.

### G3 — Verification (plan §5) not yet done

Plan section 5 listed 7 verification steps; only step 1 (build) has been confirmed by this review. Steps 3-6 require running the launcher.

**Fix:** walk the checklist on a Mac dev build:

1. Launch with `JAWAKA_THEME=Jawaka-Coverflow ./build/bin/jawaka-launcher` (or wire it via Settings).
2. Confirm centre icon size, side icon size + alpha, label font size, tools tile rendering, fallback to `_default.png`.
3. Press right rapidly — confirm tween restarts from current animated position (no rubber-band).
4. Press left at cursor=0 and right at the end — confirm no animation starts.
5. Press A on a system — confirm game browser opens; press B — confirm cursor restored to the just-exited system.
6. Switch back to the other three themes — confirm nothing regressed.

If any of these fail, treat them as additional gaps and amend this plan.

### G4 — Minor style: missing braces in else-if chain (trivial)

[main.c:1794-1797](Jawaka/cmd/jawaka-launcher/main.c:1794) and [main.c:1803-1806](Jawaka/cmd/jawaka-launcher/main.c:1803):

```c
} else if (layout == CAT_LAUNCHER_HORIZONTAL)
    cat_list_state_move(&state->list, -1, count);
else
    cat_list_state_page(&state->list, -1, count);
```

Functionally correct but the rest of the file uses braces uniformly. Pick one or the other for consistency — recommend braces.

### G5 — Pre-existing warnings in `internal/platform/paths.c` (out of scope)

`make` surfaced two unused-function warnings ([paths.c:74](Jawaka/internal/platform/paths.c:74), [paths.c:83](Jawaka/internal/platform/paths.c:83)) — `jw__path_is_within` and `jw__option_list_has`. Both predate coverflow; not introduced by this work. Flagged here purely so you know the warning count didn't go up.

Not part of this gap-closing pass. Either delete the dead helpers or mark them `__attribute__((unused))` in a separate cleanup.

## Recommended close-out sequence

1. **G1 fix** — 5-line edit to `jw__load_system_icon`, rebuild, confirm it still works against mock-sdcard.
2. **G2 fix** — run the fetch script, spot-check, commit PNGs alongside the source changes.
3. **G3 verification** — manual walk-through against the plan §5 checklist. If a step fails, fix or document.
4. **G4 style sweep** — add braces. Optional but cheap.
5. **Commit** — one commit per logical unit (Catastrophe enum/struct, Jawaka coverflow render, theme assets) so the history reads cleanly.

After G1-G3 the implementation will fully match the original plan.

## Things the implementation got right (worth preserving on rebase / refactor)

- **Animation chain math.** Sampling `from_visual = jw__coverflow_visual_cursor()` at the moment of the new keypress (rather than snapping to `list.cursor`) is the correct way to implement "restart from current animated position." Matches the plan exactly and feels right.
- **`cat_request_frame()` only while `a->active`.** Avoids burning frames when idle — the launcher correctly drops back to event-driven rendering after the tween finishes.
- **Retiring animation at the top of render** instead of at the bottom, so the visual-cursor function and the request-frame check see consistent state inside the same frame.
- **Two-pass draw (sides first, centre second).** Centre icon correctly overlaps neighbours during the slide.
- **`SDL_SetTextureAlphaMod(..., 255)` after each draw.** Prevents alpha leaking into other cached uses of the same texture.
- **`memset(&state->coverflow_anim, 0, sizeof(...))` in `jw__rebuild_for_layout`.** Defensive — if the user switches themes back and forth, no stale animation state survives.
- **`logo_name` log line includes `"coverflow"`.** Easy to grep in production logs.
