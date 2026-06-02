#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
UMRK_ROOT="$(cd "$JAWAKA_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/phase3-fixture}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-phase3-scan.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

SD_ROOT="$TMP_ROOT/sd"
SECONDARY_ROOT="$TMP_ROOT/secondary-sd"
DB_PATH="$TMP_ROOT/library.db"
OUT_PATH="$TMP_ROOT/scan.tsv"
OUT_PRUNE_PATH="$TMP_ROOT/scan-prune.tsv"

mkdir -p \
    "$SD_ROOT/UMRK/mlp1/defaults" \
    "$SD_ROOT/Roms/MD" \
    "$SD_ROOT/Roms/GBA" \
    "$SD_ROOT/Roms/ARCADE" \
    "$SD_ROOT/Roms/PS" \
    "$SD_ROOT/Roms/PORTS" \
    "$SD_ROOT/Roms/UNKNOWN" \
    "$SD_ROOT/Images/MD" \
    "$SECONDARY_ROOT/Roms/GBA/Imgs"

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
printf 'echo test\n' >"$SD_ROOT/Roms/PORTS/Test.sh"
printf 'ignore me\n' >"$SD_ROOT/Roms/UNKNOWN/readme.txt"
: >"$SD_ROOT/Images/MD/Sonic.png"
printf 'secondary duplicate\n' >"$SECONDARY_ROOT/Roms/GBA/Example.gba.zip"
printf 'secondary\n' >"$SECONDARY_ROOT/Roms/GBA/Secondary.gba"
: >"$SECONDARY_ROOT/Roms/GBA/Imgs/Secondary.png"

make -C "$JAWAKA_DIR" \
    BUILD="$BUILD_DIR" \
    PLATFORM=mlp1 \
    jawaka-scan-smoke >/dev/null

SDCARD_PATHS="$SD_ROOT:$SECONDARY_ROOT" \
    "$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-scan-smoke" "$SD_ROOT" "$DB_PATH" >"$OUT_PATH"

grep -F $'summary\tgames=7\tsystems=4\tapps=0' "$OUT_PATH" >/dev/null
grep -F $'game\tMD\tSonic\tRoms/MD/Sonic.md\tImages/MD/Sonic.png' "$OUT_PATH" >/dev/null
grep -F $'game\tMD\tSonic\tRoms/MD/Sonic.md.zip\tImages/MD/Sonic.png' "$OUT_PATH" >/dev/null
grep -F $'game\tGBA\tExample\tRoms/GBA/Example.gba.zip\t' "$OUT_PATH" >/dev/null
grep -F "game"$'\t'"GBA"$'\t'"Example"$'\t'"$SECONDARY_ROOT/Roms/GBA/Example.gba.zip"$'\t' "$OUT_PATH" >/dev/null
grep -F "game"$'\t'"GBA"$'\t'"Secondary"$'\t'"$SECONDARY_ROOT/Roms/GBA/Secondary.gba"$'\t'"$SECONDARY_ROOT/Roms/GBA/Imgs/Secondary.png" "$OUT_PATH" >/dev/null
grep -F $'game\tARCADE\tmslug\tRoms/ARCADE/mslug.zip\t' "$OUT_PATH" >/dev/null
grep -F $'game\tPS\tGame\tRoms/PS/Game.m3u\t' "$OUT_PATH" >/dev/null

if grep -E 'neogeo|UNKNOWN|readme|PORTS|Test\.sh' "$OUT_PATH" >/dev/null; then
    cat "$OUT_PATH" >&2
    echo "phase3 fixture scan included an ignored, unknown, or unsupported file" >&2
    exit 1
fi

rm -rf "$SECONDARY_ROOT"
SDCARD_PATHS="$SD_ROOT:$SECONDARY_ROOT" \
    "$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-scan-smoke" "$SD_ROOT" "$DB_PATH" >"$OUT_PRUNE_PATH"

grep -F $'summary\tgames=5\tsystems=4\tapps=0' "$OUT_PRUNE_PATH" >/dev/null
if grep -F "$SECONDARY_ROOT" "$OUT_PRUNE_PATH" >/dev/null ||
   grep -F $'game\tGBA\tSecondary' "$OUT_PRUNE_PATH" >/dev/null; then
    cat "$OUT_PRUNE_PATH" >&2
    echo "phase3 fixture scan did not prune removed secondary SD rows" >&2
    exit 1
fi

cat "$OUT_PATH"
cat "$OUT_PRUNE_PATH"
