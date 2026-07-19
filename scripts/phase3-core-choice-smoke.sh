#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/phase3-core-choice}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-core-choice.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

SD_ROOT="$TMP_ROOT/sd"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mac"
CORES_DIR="$PLATFORM_ROOT/cores"
DEFAULTS_DIR="$PLATFORM_ROOT/defaults"

mkdir -p \
    "$CORES_DIR" \
    "$DEFAULTS_DIR" \
    "$PLATFORM_ROOT/emulators/mupen64plus" \
    "$PLATFORM_ROOT/emulators/flycast"

cat >"$DEFAULTS_DIR/cores.json" <<'JSON'
{
  "version": 2,
  "platform": "mac",
  "cores": [
    {
      "id": "mupen64plus_standalone",
      "display_name": "Mupen64Plus Standalone",
      "type": "path",
      "libretro_name": null,
      "file_name": null,
      "config_folder": "Mupen64Plus Standalone",
      "info_name": null,
      "path": "emulators/mupen64plus/launch.sh",
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    },
    {
      "id": "mupen64plus_next",
      "display_name": "Mupen64Plus Next",
      "type": "retroarch",
      "libretro_name": "mupen64plus_next",
      "file_name": "mupen64plus_next_libretro.dylib",
      "config_folder": "Mupen64Plus-Next",
      "info_name": "mupen64plus_next_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    },
    {
      "id": "flycast_standalone",
      "display_name": "Flycast Standalone",
      "type": "path",
      "libretro_name": null,
      "file_name": null,
      "config_folder": "Flycast Standalone",
      "info_name": null,
      "path": "emulators/flycast/launch.sh",
      "supports_menu": false,
      "supports_savestate": false,
      "supports_disk_control": false,
      "needs_swap": false,
      "requires_direct_drm": true,
      "status": "packaged"
    },
    {
      "id": "flycast",
      "display_name": "Flycast",
      "type": "retroarch",
      "libretro_name": "flycast",
      "file_name": "flycast_libretro.dylib",
      "config_folder": "Flycast",
      "info_name": "flycast_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": true,
      "needs_swap": false,
      "requires_direct_drm": false,
      "status": "packaged"
    },
    {
      "id": "mgba",
      "display_name": "mGBA",
      "type": "retroarch",
      "libretro_name": "mgba",
      "file_name": "mgba_libretro.so",
      "config_folder": "mGBA",
      "info_name": "mgba_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    },
    {
      "id": "gpsp",
      "display_name": "gpSP",
      "type": "retroarch",
      "libretro_name": "gpsp",
      "file_name": "gpsp_libretro.so",
      "config_folder": "gpSP",
      "info_name": "gpsp_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    }
  ]
}
JSON

cat >"$DEFAULTS_DIR/systems.json" <<'JSON'
{
  "version": 2,
  "platform": "mac",
  "systems": [
    {
      "id": "N64",
      "name": "Nintendo 64",
      "patterns": ["N64"],
      "extensions": ["n64", "v64", "z64"],
      "archive_extensions": ["zip", "7z"],
      "archive_inner_extensions": ["n64", "v64", "z64"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "mupen64plus_standalone",
      "alternate_cores": ["mupen64plus_next"],
      "rom_root": "Roms/N64",
      "image_root": "Images/N64",
      "bios_notes": []
    },
    {
      "id": "N64ALT",
      "name": "Nintendo 64 Alternate",
      "patterns": ["N64ALT"],
      "extensions": ["n64", "v64", "z64"],
      "archive_extensions": ["zip", "7z"],
      "archive_inner_extensions": ["n64", "v64", "z64"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "mupen64plus_next",
      "alternate_cores": ["mupen64plus_standalone"],
      "rom_root": "Roms/N64",
      "image_root": "Images/N64",
      "bios_notes": []
    },
    {
      "id": "DC",
      "name": "Sega Dreamcast",
      "patterns": ["DC"],
      "extensions": ["chd", "cdi", "gdi"],
      "archive_extensions": [],
      "archive_inner_extensions": ["chd", "cdi", "gdi"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": ["m3u"],
      "m3u_generation": "manual",
      "default_core": "flycast_standalone",
      "alternate_cores": ["flycast"],
      "rom_root": "Roms/DC",
      "image_root": "Images/DC",
      "bios_notes": []
    },
    {
      "id": "GBA",
      "name": "Game Boy Advance",
      "patterns": ["GBA"],
      "extensions": ["gba"],
      "archive_extensions": ["zip"],
      "archive_inner_extensions": ["gba"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "mgba",
      "alternate_cores": ["gpsp"],
      "rom_root": "Roms/GBA",
      "image_root": "Images/GBA",
      "bios_notes": []
    }
  ]
}
JSON

printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/flycast/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/flycast/launch.sh"
: >"$CORES_DIR/mupen64plus_next_libretro.dylib"
: >"$CORES_DIR/flycast_libretro.dylib"
: >"$CORES_DIR/mgba_libretro.so"
: >"$CORES_DIR/gpsp_libretro.so"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" \
    jawaka-catalog-smoke jawaka-core-override-smoke >/dev/null
SMOKE="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-catalog-smoke"
OVERRIDE_SMOKE="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-core-override-smoke"

UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64 "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64ALT "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64alt.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" DC "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/dc.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" GBA "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/gba.tsv"

grep -F $'count\t2' "$TMP_ROOT/n64.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_standalone\tpath\tdefault\tMupen64Plus Standalone\temulators/mupen64plus/launch.sh' "$TMP_ROOT/n64.tsv" >/dev/null
grep -F $'choice\t1\tmupen64plus_next\tretroarch\talternate\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64.tsv" >/dev/null

grep -F $'count\t2' "$TMP_ROOT/n64alt.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_next\tretroarch\tdefault\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64alt.tsv" >/dev/null
grep -F $'choice\t1\tmupen64plus_standalone\tpath\talternate\tMupen64Plus Standalone\temulators/mupen64plus/launch.sh' "$TMP_ROOT/n64alt.tsv" >/dev/null

grep -F $'count\t2' "$TMP_ROOT/dc.tsv" >/dev/null
grep -F $'choice\t0\tflycast_standalone\tpath\tdefault\tFlycast Standalone\temulators/flycast/launch.sh\tdirect-drm' "$TMP_ROOT/dc.tsv" >/dev/null
grep -F $'choice\t1\tflycast\tretroarch\talternate\tFlycast\tflycast_libretro.dylib\tshared-drm' "$TMP_ROOT/dc.tsv" >/dev/null

grep -F $'count\t2' "$TMP_ROOT/gba.tsv" >/dev/null
grep -F $'choice\t0\tmgba\tretroarch\tdefault\tmGBA\tmgba_libretro.so' "$TMP_ROOT/gba.tsv" >/dev/null
grep -F $'choice\t1\tgpsp\tretroarch\talternate\tgpSP\tgpsp_libretro.so' "$TMP_ROOT/gba.tsv" >/dev/null

UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$OVERRIDE_SMOKE" "$SD_ROOT" GBA "$CORES_DIR" "$PLATFORM_ROOT" \
    "$TMP_ROOT/library.db" "Roms/GBA/smoke.gba" gpsp >"$TMP_ROOT/gba-override.tsv"
grep -F $'choices\tmgba\tgpsp' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F $'persisted\tsystem\tgpsp' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F $'persisted\tgame\tgpsp' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F $'fallback\tmgba' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F 'PASS core-override-smoke' "$TMP_ROOT/gba-override.tsv" >/dev/null

chmod 644 "$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64 "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64-noexec.tsv"
grep -F $'count\t1' "$TMP_ROOT/n64-noexec.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_next\tretroarch\talternate\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64-noexec.tsv" >/dev/null
if grep -F 'mupen64plus_standalone' "$TMP_ROOT/n64-noexec.tsv" >/dev/null; then
    cat "$TMP_ROOT/n64-noexec.tsv" >&2
    echo "non-executable path core appeared in core choices" >&2
    exit 1
fi

chmod 644 "$PLATFORM_ROOT/emulators/flycast/launch.sh"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" DC "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/dc-noexec.tsv"
grep -F $'count\t1' "$TMP_ROOT/dc-noexec.tsv" >/dev/null
grep -F $'choice\t0\tflycast\tretroarch\talternate\tFlycast\tflycast_libretro.dylib\tshared-drm' "$TMP_ROOT/dc-noexec.tsv" >/dev/null
if grep -F 'flycast_standalone' "$TMP_ROOT/dc-noexec.tsv" >/dev/null; then
    cat "$TMP_ROOT/dc-noexec.tsv" >&2
    echo "non-executable Flycast path core appeared in core choices" >&2
    exit 1
fi

cat "$TMP_ROOT/n64.tsv"
cat "$TMP_ROOT/n64alt.tsv"
cat "$TMP_ROOT/n64-noexec.tsv"
cat "$TMP_ROOT/dc.tsv"
cat "$TMP_ROOT/dc-noexec.tsv"
cat "$TMP_ROOT/gba.tsv"
cat "$TMP_ROOT/gba-override.tsv"
