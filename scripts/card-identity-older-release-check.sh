#!/usr/bin/env bash
# Runs the scanner from an older release against a library with parked rows, then checks
# that this tree keeps the newer progress when it takes the library back.
#   OLD_COMMIT      commit to build the older scanner from (default: Jawaka in Leaf v0.13.0)
#   OLD_SOURCE_DIR  an already extracted older tree, instead of using git history
set -euo pipefail

JAWAKA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OLD_COMMIT="${OLD_COMMIT:-915c485}"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-card-older.XXXXXX")"
trap 'rm -rf "$ROOT"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

OLD_DIR="$ROOT/old"
mkdir -p "$OLD_DIR"
if [[ -n "${OLD_SOURCE_DIR:-}" ]]; then
    cp -R "$OLD_SOURCE_DIR/." "$OLD_DIR/"
else
    git -C "$JAWAKA_DIR" archive "$OLD_COMMIT" | tar -x -C "$OLD_DIR"
fi
make -C "$JAWAKA_DIR" BUILD="$ROOT/build-new" jawaka-scan-smoke >/dev/null
make -C "$OLD_DIR" BUILD="$ROOT/build-old" jawaka-scan-smoke >/dev/null
NEW_SCAN="$ROOT/build-new/bin/jawaka-scan-smoke"
OLD_SCAN="$ROOT/build-old/bin/jawaka-scan-smoke"

DB="$ROOT/library.db"
mkdir -p "$ROOT/primary/Roms" "$ROOT/cards/A/Roms/NES" "$ROOT/cards/B/Roms/NES"
touch "$ROOT"/cards/A/Roms/NES/a{1,2,3}.nes "$ROOT"/cards/B/Roms/NES/b{1,2}.nes
export SDCARD_PATHS="$ROOT/primary:$ROOT/slot" JAWAKA_DISABLE_RETROARCH_V2=1
unset JAWAKA_TEST_CARD_KEY_secondary_sd

q() { sqlite3 "$DB" "$1"; }
insert() { mv "$ROOT/cards/$1" "$ROOT/slot"; }
remove() { mv "$ROOT/slot" "$ROOT/cards/$1"; }
new_scan() { JAWAKA_TEST_CARD_KEY_secondary_sd="$1" "$NEW_SCAN" "$ROOT/primary" "$DB" >/dev/null 2>"$ROOT/new.log"; }
old_scan() { "$OLD_SCAN" "$ROOT/primary" "$DB" >"$ROOT/old.out" 2>&1; }
parked() { q "SELECT COUNT(*)||':'||COALESCE(SUM(playtime_s),0) FROM games WHERE source_id LIKE 'card-%'"; }

insert A; new_scan fat:aaaa0001; remove A
insert B; new_scan fat:bbbb0002
ID="$(q "SELECT id FROM games WHERE source_id='secondary_sd' AND rom_relpath='NES/b1.nes'")"
q "UPDATE games SET playtime_s=10 WHERE id=$ID;
   INSERT INTO favorites(kind,target_id,added_at) VALUES('game',$ID,1);
   INSERT INTO game_settings(game_id,key,value,updated_at) VALUES($ID,'core_id','fixture',1);"
remove B; insert A; new_scan fat:aaaa0001
[[ "$(q 'PRAGMA user_version')" == 6 ]] || fail "user_version changed"
BEFORE="$(parked)"
[[ "$BEFORE" == "2:10" ]] || fail "B wasn't parked: $BEFORE"

old_scan || fail "older scanner rejected the library"
[[ "$(parked)" == "$BEFORE" ]] || fail "older scanner changed parked rows with the owner in the slot"
grep -q '/.leaf-parked/' "$ROOT/old.out" || fail "expected parked rows in the older scanner's list"

remove A; insert B
old_scan || fail "older scanner rejected the library with B"
[[ "$(parked)" == "$BEFORE" ]] || fail "older scanner changed parked rows with B in the slot"
q "UPDATE games SET playtime_s=500 WHERE source_id='secondary_sd' AND rom_relpath='NES/b1.nes'"

new_scan fat:bbbb0002
grep -q 'second card recovered' "$ROOT/new.log" || fail "expected recovery: $(cat "$ROOT/new.log")"
[[ "$(q "SELECT COUNT(*) FROM games WHERE source_id LIKE 'card-%'")" == 0 ]] || fail "parked rows remain"
ROW="$(q "SELECT g.source_id,g.playtime_s,
          (SELECT COUNT(*) FROM favorites f WHERE f.target_id=g.id),
          (SELECT value FROM game_settings s WHERE s.game_id=g.id AND s.key='core_id')
          FROM games g WHERE g.rom_relpath='NES/b1.nes'")"
[[ "$ROW" == "secondary_sd|510|1|fixture" ]] || fail "recovered row was $ROW"
[[ "$(q "SELECT card_key FROM library_cards WHERE is_live=1")" == fat:bbbb0002 ]] || fail "wrong owner"
echo "PASS card-identity-older-release-check ($OLD_COMMIT)"
