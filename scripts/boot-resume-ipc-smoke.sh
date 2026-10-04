#!/usr/bin/env bash
# Resume Game on Boot, driven on the host: a real jawakad on the mock platform
# plays two boots of one card.
#
# Boot A arms: a power-hold save (scripted power edges and the fake RetroArch,
# as in power-hold-save-ipc-smoke.sh) ends `saved` with both settings On and
# writes boot-resume.json. Every later row is boot B: a fresh daemon with the
# record (as armed, or edited for the row), the library and settings from boot
# A, and the saved state on the card; it must delete the record and then
# resume, discard, or fail to the launcher exactly as the plan's acceptance
# table says.
#
# Test hooks, host builds of jawakad only (never under -DPLATFORM_MLP1):
#   JAWAKA_TEST_BOOT_ID=<id>            the boot id, so one host plays two boots
#   JAWAKA_TEST_SOURCE_FINGERPRINT=<fp> the mock card's persistent identity
# plus the power-hold save's two (see that smoke) for the arming boot, and the
# mock proxy's JAWAKA_MOCK_HELD_BUTTONS=b for the bypass row. The fake
# RetroArch speaks LOAD_STATE_SYNC when FAKE_RA_LOAD_STATE is set.
#
# Daemon smokes run in a Linux container (gcc:14 --init), not on the Mac.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
FAKE_RA_PY="$ROOT_DIR/scripts/fake-retroarch.py"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-boot-resume.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
CORES="$PLATFORM_ROOT/cores"
FAKE_RA="$TMP_DIR/retroarch"
CORE_FOLDER="Fixture RA"
ROM="Roms/N64/Hold Game.n64"
STATES_DIR="$PRIMARY/States/$CORE_FOLDER"
FINAL_STATE="$STATES_DIR/Hold Game.state99"
STATE_BYTES=1048576
CARD="uuid:SMOKE-CARD"
RECORD="$STATE/boot-resume.json"
SEED="$TMP_DIR/seed"
CASE=""
LOG=""

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    pkill -f "$TMP_DIR" 2>/dev/null
    if [ "$status" -ne 0 ]; then
        for log in "$TMP_DIR"/*.log; do
            [ -f "$log" ] || continue
            echo "---- $log" >&2
            sed -n '1,400p' "$log" >&2
        done
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

fail() { echo "FAIL boot-resume-ipc-smoke${CASE:+ ($CASE)}: $*" >&2; exit 1; }

if python3 - <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.bind(("127.0.0.1", 55355))
except OSError:
    sys.exit(0)
sys.exit(1)
PY
then
    fail "UDP 127.0.0.1:55355 is already in use"
fi

make -C "$ROOT_DIR" BUILD="$BUILD_REL" jawakad jawaka-platformctl >/dev/null

mkdir -p "$USERDATA" "$LOGS" "$DEFAULTS" "$CORES" "$PRIMARY/Roms/N64" \
         "$PRIMARY/Images/N64" "$PRIMARY/Saves" "$PRIMARY/States" "$SEED"
printf 'core\n' >"$CORES/fixture_libretro.so"
printf 'rom\n' >"$PRIMARY/$ROM"
cat >"$FAKE_RA" <<EOF
#!/bin/sh
exec python3 "$FAKE_RA_PY" "\$@"
EOF
chmod 755 "$FAKE_RA"

python3 - "$DEFAULTS" "$CORE_FOLDER" <<'CATALOG'
import json, pathlib, sys
defaults = pathlib.Path(sys.argv[1])
core = {"id": "fixture_ra", "display_name": "Fixture RetroArch",
        "type": "retroarch", "libretro_name": "fixture",
        "file_name": "fixture_libretro.so", "config_folder": sys.argv[2],
        "info_name": "fixture_libretro.info", "path": None,
        "supports_menu": True, "supports_savestate": True,
        "supports_disk_control": False, "needs_swap": False,
        "status": "packaged"}
(defaults / "cores.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "cores": [core]}))
(defaults / "systems.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "systems": [{
        "id": "N64", "name": "Nintendo 64", "patterns": ["N64"],
        "extensions": ["n64"], "archive_extensions": [],
        "archive_inner_extensions": ["n64"], "archive_mode": "pass_through",
        "file_names": [], "ignore_file_names": [], "playlist_extensions": [],
        "m3u_generation": "none", "default_core": "fixture_ra",
        "alternate_cores": [], "rom_root": "Roms/N64",
        "image_root": "Images/N64", "bios_notes": []}]}))
CATALOG

request() {
    "$CTL" --socket "$SOCKET" request "$1" 2>/dev/null || true
}

wait_log() { # pattern, tenths of a second
    local i
    for i in $(seq 1 "$2"); do
        grep -F -q -- "$1" "$LOG" && return 0
        sleep 0.1
    done
    return 1
}

expect() { grep -F -q -- "$1" "$LOG" || fail "missing log line: $1"; }
expect_not() { ! grep -F -q -- "$1" "$LOG" || fail "unexpected log line: $1"; }

set_settings() { # save resume
    python3 - "$STATE/library.db" "$1" "$2" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT OR REPLACE INTO settings(key,value) VALUES ('save_state_on_power_hold',?)", (sys.argv[2],))
db.execute("INSERT OR REPLACE INTO settings(key,value) VALUES ('resume_game_on_boot',?)", (sys.argv[3],))
db.commit()
PY
}

# start_daemon NAME [NAME=VALUE ...]: fresh runtime dir, same card and state.
start_daemon() {
    CASE="$1"
    shift
    RUNTIME="$TMP_DIR/runtime-$CASE"
    SOCKET="$RUNTIME/jawakad.sock"
    LOG="$TMP_DIR/$CASE.log"
    RA_COMMANDS="$TMP_DIR/$CASE.ra-commands"
    EDGES="$TMP_DIR/$CASE.edges"
    rm -rf "$RUNTIME"
    mkdir -p "$RUNTIME"
    if [ -n "${RUNTIME_HOOK:-}" ]; then
        eval "$RUNTIME_HOOK"
    fi
    : >"$RA_COMMANDS"
    : >"$EDGES"
    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
        USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
        SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" CORES_PATH="$CORES" \
        UMRK_RETROARCH_BIN="$FAKE_RA" \
        JAWAKA_MOCK_POWER_EDGES="$EDGES" \
        FAKE_RA_QUIT_LOG="$RA_COMMANDS" FAKE_RA_READY="$RUNTIME/ra-ready" \
        FAKE_RA_STATES_DIR="$STATES_DIR" \
        JAWAKA_TEST_SOURCE_FINGERPRINT="$CARD" \
        "$@" \
        "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only
    local i
    for i in $(seq 1 300); do
        [ -S "$SOCKET" ] && return 0
        kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited during startup"
        sleep 0.02
    done
    fail "daemon socket never appeared"
}

wait_library() {
    local i status
    for i in $(seq 1 500); do
        status="$(request '{"type":"library-status"}')"
        if python3 -c 'import json,sys; d=json.load(sys.stdin); raise SystemExit(d.get("scan_running", True) or d.get("generation", 0) <= 0)' <<<"$status" 2>/dev/null; then
            return 0
        fi
        sleep 0.02
    done
    fail "library scan never finished"
}

expect_daemon_gone() {
    local i
    for i in $(seq 1 300); do
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            wait "$DAEMON_PID" 2>/dev/null || true
            DAEMON_PID=""
            return 0
        fi
        sleep 0.1
    done
    fail "jawakad did not exit after the long press"
}

# arm NAME RESUME(0|1): boot A, a released power-hold save with the save
# setting On and Resume Game on Boot as given.
arm() {
    local name="$1" resume="$2"
    rm -rf "$STATE" "$PRIMARY/States"
    mkdir -p "$STATE" "$PRIMARY/States"
    start_daemon "$name" JAWAKA_TEST_BOOT_ID=boot-a \
        JAWAKA_TEST_POWER_HOLD_SAVE_ANY_PLATFORM=1 JAWAKA_TEST_POWER_HARD_CUT_S=10 \
        FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES"
    wait_library
    set_settings 1 "$resume"
    wait_log 'power-hold save: setting on' 80 || fail "save setting never read as on"
    if [ "$resume" = 1 ]; then
        wait_log 'boot resume: setting on' 80 || fail "resume setting never read as on"
    fi
    request "{\"type\":\"launch-game\",\"system\":\"N64\",\"rom_path\":\"$ROM\"}" |
        grep -F '"type":"ok"' >/dev/null || fail "launch-game refused"
    wait_log 'RetroArch session started pid=' 100 || fail "RetroArch session never started"
    local i
    for i in $(seq 1 200); do
        [ -s "$RUNTIME/ra-ready" ] && break
        sleep 0.02
    done
    [ -s "$RUNTIME/ra-ready" ] || fail "fake RetroArch never bound its port"
    sleep 0.2
    printf '%s\n' "down +0" "up +2600" >>"$EDGES"
    expect_daemon_gone
    expect 'power-hold save: saved elapsed_ms='
}

# --- Boot A, Resume Game on Boot Off: saved, nothing armed. ----------------
arm arm-off 0
expect 'power-hold save: saved; Resume Game on Boot is off, nothing armed'
expect_not 'resume armed'
expect_not 'resume not armed'
[ ! -e "$RECORD" ] || fail "a record was written with Resume Game on Boot Off"
echo "row arm-off: saved, no record"

# --- Boot A, both On: saved, then the record, before teardown. -------------
arm arm 1
expect 'power-hold save: resume armed request='
expect 'write_ms='
[ -f "$RECORD" ] || fail "no record at $RECORD"
[ -f "$FINAL_STATE" ] || fail "no saved state at $FINAL_STATE"
python3 - "$RECORD" "$STATE_BYTES" "$CARD" <<'PY' || fail "record does not name the save"
import json, sys
r = json.load(open(sys.argv[1]))
want = {"schema": 1, "platform": "mac", "boot_id": "boot-a",
        "source_fingerprint": sys.argv[3], "system": "N64",
        "rom_path": "N64/Hold Game.n64", "core_id": "fixture_ra",
        "core_config_folder": "Fixture RA", "provider": "", "slot": 99,
        "state_path": "Fixture RA/Hold Game.state99",
        "state_bytes": int(sys.argv[2])}
for key, value in want.items():
    if r.get(key) != value:
        sys.exit("%s=%r, want %r" % (key, r.get(key), value))
if set(r) != set(want) | {"request_id"} or not r["request_id"]:
    sys.exit("unexpected keys %r" % sorted(r))
PY
armed_line="$(grep -n -F 'resume armed' "$LOG" | head -1 | cut -d: -f1)"
quit_line="$(grep -n -F 'quit grace: sent QUIT' "$LOG" | head -1 | cut -d: -f1)"
[ "$armed_line" -lt "$quit_line" ] || fail "the record was written after child teardown began"
REQUEST_ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["request_id"])' "$RECORD")"
cp "$RECORD" "$SEED/boot-resume.json"
cp "$STATE/library.db" "$SEED/library.db"
cp "$FINAL_STATE" "$SEED/state99"
echo "row arm: saved, record armed request=$REQUEST_ID"

# boot NAME [NAME=VALUE ...]: boot B with the armed record, the library and
# settings from boot A, and the saved state back in place.
boot() {
    local name="$1"
    shift
    rm -rf "$STATE"
    mkdir -p "$STATE" "$STATES_DIR"
    cp "$SEED/library.db" "$STATE/library.db"
    cp "$SEED/boot-resume.json" "$RECORD"
    cp "$SEED/state99" "$FINAL_STATE"
    rm -f "$FINAL_STATE.png"
    if [ -n "${PREPARE_HOOK:-}" ]; then
        eval "$PREPARE_HOOK"
    fi
    start_daemon "$name" "$@"
}

edit_record() { # python expression over r
    python3 - "$RECORD" "$1" <<'PY'
import json, sys
path, expr = sys.argv[1], sys.argv[2]
r = json.load(open(path))
exec(expr)
open(path, "w").write(json.dumps(r))
PY
}

expect_record_gone() { [ ! -e "$RECORD" ] || fail "the record is still there"; }
expect_state_untouched() { cmp -s "$FINAL_STATE" "$SEED/state99" || fail "the saved state changed"; }
expect_no_launch() {
    expect_not 'RetroArch session started'
    expect_not 'boot resume: launching'
}
stop() { smoke_daemon_stop || fail "jawakad did not stop"; }

RESUME_RA=(FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES")

# --- Same-boot record (daemon restart, direct jawakad): deleted, ignored. --
boot same-boot JAWAKA_TEST_BOOT_ID=boot-a "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
wait_log 'boot resume: record discarded (same-boot)' 50 || fail "same-boot record not discarded"
stop
expect_record_gone
expect_no_launch
echo "row same-boot: deleted and ignored"

# --- Either setting Off at boot: deleted, ignored. --------------------------
PREPARE_HOOK='set_settings 1 0' boot setting-off JAWAKA_TEST_BOOT_ID=boot-b \
    "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
wait_log 'boot resume: record discarded (setting-off)' 50 || fail "Off setting did not discard"
stop
expect_record_gone
expect_no_launch
PREPARE_HOOK='set_settings 0 1' boot save-off JAWAKA_TEST_BOOT_ID=boot-b \
    "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
wait_log 'boot resume: record discarded (setting-off)' 50 || fail "Off save setting did not discard"
stop
expect_record_gone
expect_no_launch
echo "row setting-off: deleted and ignored for either setting"

# --- B held before the proxy starts: launcher, record deleted, state kept. --
boot bypass JAWAKA_TEST_BOOT_ID=boot-b JAWAKA_MOCK_HELD_BUTTONS=b \
    "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
wait_log 'boot resume: record discarded (bypass)' 50 || fail "held B did not bypass"
stop
expect 'input proxy (mock): b held at start'
expect_record_gone
expect_no_launch
expect_state_untouched
echo "row bypass: held B skips, state and setting untouched"

# --- Recovery this boot (a LIFE-1 launch recovered): recovery wins. --------
RUNTIME_HOOK='printf "{\"launch_id\":\"x\",\"source_id\":\"primary\",\"saves_path\":\"%s/Saves\",\"states_path\":\"%s/States\"}" "$PRIMARY" "$PRIMARY" >"$RUNTIME/active-game.json"' \
    boot recovery JAWAKA_TEST_BOOT_ID=boot-b "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
wait_log 'boot resume: record discarded (storage-recovery)' 50 || fail "recovery did not win"
stop
expect 'life1: recovered active launch'
expect_record_gone
expect_no_launch
echo "row recovery: record deleted, recovery wins"

# --- Malformed, future-schema and traversal records: deleted and rejected. -
for variant in malformed schema traversal; do
    case "$variant" in
        malformed) hook='printf "{\"schema\":1," >"$RECORD"'; want='(malformed)' ;;
        schema)    hook='edit_record "r[\"schema\"] = 2"'; want='(schema)' ;;
        traversal) hook='edit_record "r[\"rom_path\"] = \"../../etc/passwd\""'; want='(rom_path)' ;;
    esac
    PREPARE_HOOK="$hook" boot "reject-$variant" JAWAKA_TEST_BOOT_ID=boot-b \
        "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
    wait_log "boot resume: record rejected $want and removed" 50 || fail "$variant record not rejected"
    stop
    expect_record_gone
    expect_no_launch
done
echo "row reject: malformed, future-schema and traversal records deleted, nothing run"

# --- A record that cannot be removed (read-only card): no launch, kept. ----
PREPARE_HOOK='rm -f "$RECORD"; mkdir "$RECORD"; printf x >"$RECORD/keep"' \
    boot unremovable JAWAKA_TEST_BOOT_ID=boot-b "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
wait_log 'boot resume: record could not be removed (unlink-failed); not resuming this boot' 50 ||
    fail "an unremovable record was acted on"
stop
[ -e "$RECORD/keep" ] || fail "the unremovable record was touched"
expect_no_launch
rm -rf "$RECORD"
echo "row unremovable: left in place, no launch"

# --- Validation failures before launch: notice, nothing substituted. -------
check_refused() { # name prepare want
    PREPARE_HOOK="$2" boot "$1" JAWAKA_TEST_BOOT_ID=boot-b "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
    wait_log "boot resume: failed ($3)" 50 || fail "$1 was not refused with $3"
    stop
    expect 'the launcher opens with a notice'
    expect_record_gone
    expect_no_launch
}
check_refused state-missing 'rm -f "$FINAL_STATE"' 'state-missing'
check_refused state-empty ': >"$FINAL_STATE"' 'state-size'
check_refused state-size 'truncate -s 1000 "$FINAL_STATE"' 'state-size'
# Another slot of the same game is never a substitute.
check_refused other-slot 'mv "$FINAL_STATE" "${FINAL_STATE%99}98"' 'state-missing'
check_refused unknown-card 'edit_record "r[\"source_fingerprint\"] = \"uuid:OTHER-CARD\""' \
    'card uuid:OTHER-CARD not found'
check_refused missing-rom 'edit_record "r[\"rom_path\"] = \"N64/Gone.n64\"; r[\"state_path\"] = \"Fixture RA/Gone.state99\""' \
    'ROM missing'
check_refused core-changed 'edit_record "r[\"core_id\"] = \"other_core\""' \
    'core changed: other_core/Fixture RA is now fixture_ra/Fixture RA'
check_refused core-folder 'edit_record "r[\"core_config_folder\"] = \"Other Folder\"; r[\"state_path\"] = \"Other Folder/Hold Game.state99\""' \
    'core changed: fixture_ra/Other Folder is now fixture_ra/Fixture RA'
check_refused core-missing 'mv "$CORES/fixture_libretro.so" "$CORES/fixture_libretro.so.gone"' \
    'core fixture_ra unavailable'
mv "$CORES/fixture_libretro.so.gone" "$CORES/fixture_libretro.so"
echo "row refused: missing/empty/short state, other slot, unknown card, missing ROM, changed core"

# --- Both On, the record intact: one LOAD_STATE_SYNC, then gameplay. -------
boot resume-ok JAWAKA_TEST_BOOT_ID=boot-b "${RESUME_RA[@]}" FAKE_RA_LOAD_STATE=ok
wait_log 'boot resume: loaded request=' 100 || fail "the state never loaded"
expect "boot resume: launching system=N64 rom=$ROM core=fixture_ra request=$REQUEST_ID"
expect 'RetroArch session started pid='
expect "core_id=fixture_ra core_folder=Fixture RA"
expect_record_gone
sleep 0.3
[ "$(grep -c -x "LOAD_STATE_SYNC $REQUEST_ID 99" "$RA_COMMANDS")" -eq 1 ] ||
    fail "not exactly one LOAD_STATE_SYNC $REQUEST_ID 99"
grep -q -x "LOAD_STATE_SYNC $REQUEST_ID -1" "$RA_COMMANDS" || fail "no capability probe"
probe_line="$(grep -n -x "LOAD_STATE_SYNC $REQUEST_ID -1" "$RA_COMMANDS" | cut -d: -f1)"
load_line="$(grep -n -x "LOAD_STATE_SYNC $REQUEST_ID 99" "$RA_COMMANDS" | cut -d: -f1)"
unpause_line="$(grep -n -x 'UNPAUSE' "$RA_COMMANDS" | head -1 | cut -d: -f1)"
[ -n "$unpause_line" ] || fail "RetroArch was never unpaused"
[ "$probe_line" -lt "$load_line" ] && [ "$load_line" -lt "$unpause_line" ] ||
    fail "probe, load and UNPAUSE out of order"
grep -q 'LOAD_STATE_SLOT\|^-e' "$RA_COMMANDS" && fail "a slot load or entry slot was used"
expect_not 'switcher resume'
expect_state_untouched
stop
expect_not 'no play recorded'
echo "row resume-ok: one load, UNPAUSE, gameplay"

# --- LOAD_STATE_SYNC fails, or this RetroArch predates it: QUIT, launcher. --
check_load_failure() { # name load-mode want; NO_STATE_INFO=1: no GET_STATE_SAVE_INFO either
    local ra_env=("${RESUME_RA[@]}")
    [ -z "${NO_STATE_INFO:-}" ] || ra_env=()
    boot "$1" JAWAKA_TEST_BOOT_ID=boot-b ${ra_env[@]+"${ra_env[@]}"} FAKE_RA_LOAD_STATE="$2"
    wait_log "boot resume: failed ($3)" 100 || fail "$1 did not fail with $3"
    wait_log 'RetroArch session ended pid=' 100 || fail "$1: RetroArch did not exit"
    stop
    expect 'boot resume: sent QUIT to RetroArch pid='
    expect 'the launcher opens with a notice'
    expect 'boot resume: no play recorded'
    grep -q -x 'QUIT' "$RA_COMMANDS" || fail "$1: no QUIT"
    [ "$(grep -c '^LOAD_STATE_SYNC .* 99$' "$RA_COMMANDS")" -le 1 ] || fail "$1: the load was retried"
    grep -q -x 'UNPAUSE' "$RA_COMMANDS" && fail "$1: unpaused a failed resume"
    expect_record_gone
    expect_state_untouched
}
check_load_failure load-error UNSERIALIZE 'LOAD_STATE_SYNC UNSERIALIZE'
check_load_failure load-busy BUSY 'LOAD_STATE_SYNC BUSY'
check_load_failure load-old-retroarch silent 'RetroArch cannot sync-load (probe=timeout)'
grep -q -x "LOAD_STATE_SYNC $REQUEST_ID 99" "$RA_COMMANDS" &&
    fail "an older RetroArch was sent the load"
# Older still: no GET_STATE_SAVE_INFO either (before the power-hold save).
NO_STATE_INFO=1 check_load_failure load-oldest-retroarch silent \
    'RetroArch cannot sync-load (state info=timeout)'
grep -q '^LOAD_STATE_SYNC' "$RA_COMMANDS" && fail "the oldest RetroArch was probed for the load"
echo "row load-failure: error, busy and older RetroArch builds quit to the launcher"

echo "PASS boot-resume-ipc-smoke"
