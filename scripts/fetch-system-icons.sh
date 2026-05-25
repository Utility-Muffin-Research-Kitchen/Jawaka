#!/usr/bin/env bash
set -euo pipefail

# Fetches libretro Systematic console icons at a pinned commit and renames
# them to Jawaka system short codes. Safe to re-run; overwrites existing
# files. Requires curl.

REPO_RAW="https://git.libretro.com/libretro-assets/retroarch-assets/-/raw"
COMMIT="e11d6708b49a893f392b238effc713c6c7cfadef"
BASE="$REPO_RAW/$COMMIT/xmb/systematic/png"

DEST="$(cd "$(dirname "$0")/.." && pwd)/res/system_icons"
mkdir -p "$DEST"

# Mapping: jawaka_code|libretro filename (no .png suffix)
declare -a MAPPING=(
  "GB|Nintendo - Game Boy"
  "GBC|Nintendo - Game Boy Color"
  "GBA|Nintendo - Game Boy Advance"
  "FC|Nintendo - Nintendo Entertainment System"
  "SFC|Nintendo - Super Nintendo Entertainment System"
  "NDS|Nintendo - Nintendo DS"
  "MD|Sega - Mega Drive - Genesis"
  "MS|Sega - Master System - Mark III"
  "GG|Sega - Game Gear"
  "LYNX|Atari - Lynx"
  "NEOGEO|SNK - Neo Geo"
  "PCE|NEC - PC Engine - TurboGrafx 16"
  "PS|Sony - PlayStation"
  "ARCADE|MAME"
)

for entry in "${MAPPING[@]}"; do
  code="${entry%%|*}"
  source_name="${entry#*|}"
  url="$BASE/$(printf '%s' "$source_name" | sed 's/ /%20/g').png"
  out="$DEST/$code.png"
  echo "-> $code <- $source_name"
  curl --fail --silent --show-error -L "$url" -o "$out"
done

echo "Done. ${#MAPPING[@]} icons in $DEST"
echo "Note: _tools.png and _default.png are hand-authored and shipped in the repo."
