#!/usr/bin/env bash
# A Pak Rat library refresh waits for another writer, such as the daemon's scan,
# rather than reporting "database is locked". The lock outlives jw_db_open's 2 s
# default busy timeout, which is what this used to fail on.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/pakrat-rescan-busy-smoke}"
HOLD_SECONDS=4

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-pakrat-busy.XXXXXX")"
HOLDER=""
cleanup() {
    [ -z "$HOLDER" ] || kill "$HOLDER" 2>/dev/null || true
    rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

SD_ROOT="$TMP_ROOT/sd"
STATE_DIR="$SD_ROOT/.umrk/mlp1"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
DB_PATH="$STATE_DIR/library.db"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" jawaka-pakrat-smoke >/dev/null
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"

mkdir -p "$PLATFORM_ROOT" "$STATE_DIR/store" "$SD_ROOT/Roms/GB"
printf '{ "managed_apps": [] }\n' >"$PLATFORM_ROOT/manifest.json"

run_rescan() {
    env -u PAKRAT_CATALOG_BASE_URL "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" rescan
}

count_games() {
    python3 - "$DB_PATH" <<'PY'
import sqlite3
import sys

print(sqlite3.connect(sys.argv[1]).execute("SELECT COUNT(*) FROM games").fetchone()[0])
PY
}

: >"$SD_ROOT/Roms/GB/First.gb"
run_rescan >/dev/null
[ "$(count_games)" = "1" ] || { echo "the first rescan didn't index its game" >&2; exit 1; }

READY="$TMP_ROOT/holder-ready"
python3 - "$DB_PATH" "$READY" "$HOLD_SECONDS" <<'PY' &
import pathlib
import sqlite3
import sys
import time

db = sqlite3.connect(sys.argv[1], isolation_level=None)
db.execute("BEGIN IMMEDIATE")
pathlib.Path(sys.argv[2]).touch()
time.sleep(float(sys.argv[3]))
db.execute("COMMIT")
PY
HOLDER=$!
for _ in $(seq 1 100); do
    [ -e "$READY" ] && break
    kill -0 "$HOLDER" 2>/dev/null || { echo "the lock holder exited early" >&2; exit 1; }
    sleep 0.05
done
[ -e "$READY" ] || { echo "the lock holder never took its lock" >&2; exit 1; }

: >"$SD_ROOT/Roms/GB/Second.gb"
started=$SECONDS
if ! run_rescan >"$TMP_ROOT/busy.out" 2>&1; then
    cat "$TMP_ROOT/busy.out" >&2
    echo "the rescan failed while another writer held the library" >&2
    exit 1
fi
waited=$((SECONDS - started))
wait "$HOLDER"
HOLDER=""

[ "$(count_games)" = "2" ] || { echo "the rescan returned without indexing the new game" >&2; exit 1; }
[ "$waited" -ge 2 ] || { echo "the rescan didn't wait for the lock (${waited} s)" >&2; exit 1; }

echo "Pak Rat rescan busy smoke passed (waited ${waited} s)"
