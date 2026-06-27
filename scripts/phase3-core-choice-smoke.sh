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
    "$PLATFORM_ROOT/emulators/mupen64plus"

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
    }
  ]
}
JSON

printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
: >"$CORES_DIR/mupen64plus_next_libretro.dylib"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" jawaka-catalog-smoke >/dev/null
SMOKE="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-catalog-smoke"

UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64 "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64ALT "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64alt.tsv"

grep -F $'count\t2' "$TMP_ROOT/n64.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_standalone\tpath\tdefault\tMupen64Plus Standalone\temulators/mupen64plus/launch.sh' "$TMP_ROOT/n64.tsv" >/dev/null
grep -F $'choice\t1\tmupen64plus_next\tretroarch\talternate\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64.tsv" >/dev/null

grep -F $'count\t2' "$TMP_ROOT/n64alt.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_next\tretroarch\tdefault\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64alt.tsv" >/dev/null
grep -F $'choice\t1\tmupen64plus_standalone\tpath\talternate\tMupen64Plus Standalone\temulators/mupen64plus/launch.sh' "$TMP_ROOT/n64alt.tsv" >/dev/null

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

cat "$TMP_ROOT/n64.tsv"
cat "$TMP_ROOT/n64alt.tsv"
cat "$TMP_ROOT/n64-noexec.tsv"
