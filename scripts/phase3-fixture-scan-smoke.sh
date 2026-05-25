#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
UMRK_ROOT="$(cd "$JAWAKA_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/phase3-fixture}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-phase3-scan.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

SD_ROOT="$TMP_ROOT/sd"
DB_PATH="$TMP_ROOT/library.db"
OUT_PATH="$TMP_ROOT/scan.tsv"

mkdir -p \
    "$SD_ROOT/UMRK/mlp1/defaults" \
    "$SD_ROOT/Roms/MD" \
    "$SD_ROOT/Roms/GBA" \
    "$SD_ROOT/Roms/ARCADE" \
    "$SD_ROOT/Roms/PS" \
    "$SD_ROOT/Roms/UNKNOWN" \
    "$SD_ROOT/Images/MD"

cp "$UMRK_ROOT/miniloong-launcher-switcher/device/mlp1/defaults/cores.json" \
   "$SD_ROOT/UMRK/mlp1/defaults/cores.json"
cp "$UMRK_ROOT/miniloong-launcher-switcher/device/mlp1/defaults/systems.json" \
   "$SD_ROOT/UMRK/mlp1/defaults/systems.json"

printf 'rom\n' >"$SD_ROOT/Roms/MD/Sonic.md"
printf 'archive\n' >"$SD_ROOT/Roms/MD/Sonic.md.zip"
printf 'archive\n' >"$SD_ROOT/Roms/GBA/Example.gba.zip"
printf 'bios\n' >"$SD_ROOT/Roms/ARCADE/neogeo.zip"
printf 'rom\n' >"$SD_ROOT/Roms/ARCADE/mslug.zip"
printf 'disc\n' >"$SD_ROOT/Roms/PS/Game.m3u"
printf 'ignore me\n' >"$SD_ROOT/Roms/UNKNOWN/readme.txt"
: >"$SD_ROOT/Images/MD/Sonic.png"

make -C "$JAWAKA_DIR" \
    BUILD="$BUILD_DIR" \
    CFLAGS_PLATFORM="-DPLATFORM_MLP1" \
    jawaka-scan-smoke >/dev/null

"$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-scan-smoke" "$SD_ROOT" "$DB_PATH" >"$OUT_PATH"

grep -F $'summary\tgames=5\tsystems=4\tapps=0' "$OUT_PATH" >/dev/null
grep -F $'game\tMD\tSonic\tRoms/MD/Sonic.md\tImages/MD/Sonic.png' "$OUT_PATH" >/dev/null
grep -F $'game\tMD\tSonic\tRoms/MD/Sonic.md.zip\tImages/MD/Sonic.png' "$OUT_PATH" >/dev/null
grep -F $'game\tGBA\tExample\tRoms/GBA/Example.gba.zip\t' "$OUT_PATH" >/dev/null
grep -F $'game\tARCADE\tmslug\tRoms/ARCADE/mslug.zip\t' "$OUT_PATH" >/dev/null
grep -F $'game\tPS\tGame\tRoms/PS/Game.m3u\t' "$OUT_PATH" >/dev/null

if grep -E 'neogeo|UNKNOWN|readme' "$OUT_PATH" >/dev/null; then
    cat "$OUT_PATH" >&2
    echo "phase3 fixture scan included an ignored or unknown file" >&2
    exit 1
fi

cat "$OUT_PATH"
