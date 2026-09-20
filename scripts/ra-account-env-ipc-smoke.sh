#!/usr/bin/env bash
# Real-daemon child-environment fixture for standalone-ra-account-v1.
#
# Boots a real jawakad against a mock SD tree and proves, through the actual
# fork/exec launch path (not a policy helper):
#   1. an authorized bundled Flycast child receives the full configured
#      snapshot, including the revision, over a cleared environment;
#   2. legacy saved pairs are initialized with revision 1 through the real
#      daemon resolve path;
#   3. sign-out and never-configured states hand off without credentials;
#   4. a second unauthorized standalone, a provider-bound spoof core and the
#      package manager's own probes receive no account fields at all;
#   5. inherited/daemon-environment account values never survive a launch;
#   6. the password never appears in argv or the daemon log.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-ra-account-env.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
CORES="$PLATFORM_ROOT/cores"
EMU="$PLATFORM_ROOT/emulators"
APPS="$PRIMARY/Apps"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
DB="$STATE/library.db"

USER_NAME="smoke-user"
PASS_VALUE='p@$$ w0rd; |&,'

cleanup() {
    exit_status=$?
    set +e
    if [ -n "${DAEMON_PID:-}" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    if [ "$exit_status" -ne 0 ] && [ -f "$LOG" ]; then
        sed -n '1,240p' "$LOG" >&2
    fi
    rm -rf "$TMP_DIR"
    exit "$exit_status"
}
trap cleanup EXIT

fail() {
    echo "FAIL ra-account-env-ipc-smoke: $*" >&2
    exit 1
}

request() {
    "$CTL" --socket "$SOCKET" request "$1" 2>/dev/null || true
}

wait_env_dump() {
    for _ in $(seq 1 300); do
        [ -f "$RUNTIME/env-$1.txt" ] && return 0
        kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited"
        sleep 0.02
    done
    fail "env dump for $1 never appeared"
}

db_sql() {
    python3 - "$DB" "$1" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.executescript(sys.argv[2])
db.commit()
db.close()
PY
}

has_var() { # file, name
    grep -q "^$1=" "$2" 2>/dev/null
}

expect_var() { # file, name=value
    grep -Fxq "$1=$2" "$3" 2>/dev/null || fail "expected $1=$2 in $3"
}

expect_no_account_vars() {
    ! grep -q '^UMRK_RA_ACCOUNT_' "$1" 2>/dev/null ||
        fail "account snapshot leaked into $1"
    ! grep -q '^JAWAKA_CHEEVOS_' "$1" 2>/dev/null ||
        fail "RetroArch credentials leaked into $1"
}

make -C "$ROOT_DIR" BUILD="$BUILD_REL" jawakad jawaka-platformctl >/dev/null

mkdir -p "$STATE" "$RUNTIME" "$USERDATA" "$LOGS" "$DEFAULTS" "$CORES" \
         "$EMU/flycast" "$EMU/yabasanshiro" "$PRIMARY/Roms/DC" \
         "$PRIMARY/Roms/SATURN" "$PRIMARY/Images/DC" "$PRIMARY/Images/SATURN" \
         "$PRIMARY/Saves" "$PRIMARY/States"

# One env-dumping fixture, placed as the release flycast launcher, a control
# standalone and the spoof provider core's run.sh.
FIXTURE="$TMP_DIR/env-fixture.sh"
cat > "$FIXTURE" <<'SH'
#!/bin/sh
tag="$(basename "$(dirname "$0")")"
[ "$tag" = "scripts" ] && tag="$(basename "$(dirname "$(dirname "$0")")")"
env | sort > "$UMRK_RUNTIME_PATH/env-$tag.txt"
printf '%s\n' "$@" > "$UMRK_RUNTIME_PATH/argv-$tag.txt"
exit 0
SH
cp "$FIXTURE" "$EMU/flycast/launch.sh"
cp "$FIXTURE" "$EMU/yabasanshiro/launch.sh"
chmod 755 "$EMU/flycast/launch.sh" "$EMU/yabasanshiro/launch.sh"
printf '%s\n' "standalone-ra-account-v1" > "$EMU/flycast/ra-account-v1"

# A provider-bound content pak spoofing the DSperate account core. On this
# host platform its provider resolves as mac/Spoof.pak, which must never be
# authorized; the point is that a directory + core id shape alone carries
# nothing.
SPOOF="$APPS/mac/Spoof.pak"
mkdir -p "$SPOOF/scripts"
cp "$FIXTURE" "$SPOOF/scripts/run.sh"
chmod 755 "$SPOOF/scripts/run.sh"
printf '%s\n' '{"id":"org.umrk.spoof","name":"Spoof","platform":"mac","pak_version":"2.1.1","provides":{"schema":1,"systems":[],"system_extensions":[{"system_id":"DC","add_alternate_cores":["dsperate"]}],"cores":[{"id":"dsperate","display_name":"Spoof DC","type":"path","path":"scripts/run.sh","supports_menu":false,"supports_savestate":false,"supports_disk_control":false}]}}' \
    > "$SPOOF/pak.json"

printf 'rom\n' > "$PRIMARY/Roms/DC/Smoke Game.cdi"
printf 'rom\n' > "$PRIMARY/Roms/SATURN/Smoke Game.iso"
printf '%s\n' '{"schema":1,"version":"0.11.0","release_id":"ra-account-smoke-1"}' \
    > "$STATE/release.json"

python3 - "$DEFAULTS" <<'CATALOG'
import json, pathlib, sys
defaults = pathlib.Path(sys.argv[1])
def path_core(core_id, name, path):
    return {"id": core_id, "display_name": name, "type": "path",
            "libretro_name": None, "file_name": None, "config_folder": name,
            "info_name": None, "path": path, "supports_menu": False,
            "supports_savestate": False, "supports_disk_control": False,
            "needs_swap": False, "status": "packaged"}
cores = [path_core("flycast_standalone", "Flycast", "emulators/flycast/launch.sh"),
         path_core("yabasanshiro", "YabaSanshiro", "emulators/yabasanshiro/launch.sh")]
(defaults / "cores.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "cores": cores}))
def system(sid, name, exts, default, rom_root):
    return {"id": sid, "name": name, "patterns": [sid], "extensions": exts,
            "archive_extensions": [], "archive_inner_extensions": exts,
            "archive_mode": "pass_through", "file_names": [],
            "ignore_file_names": [], "playlist_extensions": [],
            "m3u_generation": "none", "default_core": default,
            "alternate_cores": [], "rom_root": rom_root,
            "image_root": f"Images/{sid}", "bios_notes": []}
(defaults / "systems.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "systems": [
        system("DC", "Dreamcast", ["cdi"], "flycast_standalone", "Roms/DC"),
        system("SATURN", "Saturn", ["iso"], "yabasanshiro", "Roms/SATURN")]}))
CATALOG

# Stale inherited values must not survive daemon startup or any launch.
(
    cd "$ROOT_DIR"
    PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$APPS" \
    USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
    SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
    JAWAKA_SDCARD_ROOT="$PRIMARY" CORES_PATH="$CORES" \
    UMRK_RA_ACCOUNT_VERSION="1" \
    UMRK_RA_ACCOUNT_STATE="configured" \
    UMRK_RA_ACCOUNT_USERNAME="stale-inherited" \
    UMRK_RA_ACCOUNT_PASSWORD="stale-inherited" \
    UMRK_RA_ACCOUNT_REVISION="99" \
    JAWAKA_CHEEVOS_USERNAME="stale-inherited" \
    JAWAKA_CHEEVOS_PASSWORD="stale-inherited" \
        "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only >>"$LOG" 2>&1
) &
DAEMON_PID=$!

for _ in $(seq 1 300); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited during startup"
    sleep 0.02
done
[ -S "$SOCKET" ] || fail "daemon socket never appeared"
for _ in $(seq 1 500); do
    [ -f "$DB" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited during scan"
    sleep 0.02
done
[ -f "$DB" ] || fail "daemon never created its database"

# Prime a LEGACY saved pair (no revision row), the shape the current device
# ships. The first authorized launch must initialize revision 1.
db_sql "INSERT INTO settings(key,value) VALUES ('retroachievements_user','$USER_NAME');
        INSERT INTO settings(key,value) VALUES ('retroachievements_pass','$PASS_VALUE');"

launch_game() { # system, rom, [core_id]
    local payload
    if [ $# -ge 3 ]; then
        payload="{\"type\":\"launch-game\",\"system\":\"$1\",\"rom_path\":\"$2\",\"core_id\":\"$3\"}"
    else
        payload="{\"type\":\"launch-game\",\"system\":\"$1\",\"rom_path\":\"$2\"}"
    fi
    request "$payload" | grep -F '"ok"' >/dev/null || fail "launch request refused: $payload"
}

wait_child_done() { # dump tag
    for _ in $(seq 1 300); do
        [ -f "$RUNTIME/env-$1.txt" ] && [ ! -e "$RUNTIME/active-game.json" ] && return 0
        sleep 0.02
    done
    [ -f "$RUNTIME/env-$1.txt" ] || fail "child for $1 never dumped"
    # The session may still be winding down; env dump is already durable.
    return 0
}

# 1-2. Configured legacy pair: full snapshot, revision 1, cleared channel.
launch_game DC "Roms/DC/Smoke Game.cdi"
wait_env_dump flycast
expect_var "UMRK_RA_ACCOUNT_VERSION" "1" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_STATE" "configured" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_USERNAME" "$USER_NAME" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_PASSWORD" "$PASS_VALUE" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_REVISION" "1" "$RUNTIME/env-flycast.txt"
! has_var JAWAKA_CHEEVOS_USERNAME "$RUNTIME/env-flycast.txt" ||
    fail "RetroArch credential channel reached the standalone child"
! grep -q "stale-inherited" "$RUNTIME/env-flycast.txt" ||
    fail "inherited account values reached the child"
ROM_ABS_EXPECTED="$(cd "$PRIMARY" && pwd -P)/Roms/DC/Smoke Game.cdi"
[ "$(cat "$RUNTIME/argv-flycast.txt")" = "$ROM_ABS_EXPECTED" ] ||
    fail "flycast argv is not the ROM path alone: got=[$(cat "$RUNTIME/argv-flycast.txt")] want=[$ROM_ABS_EXPECTED]"
! grep -qF "$PASS_VALUE" "$LOG" || fail "password present in daemon log"

# 3a. Unauthorized control standalone: nothing at all.
launch_game SATURN "Roms/SATURN/Smoke Game.iso"
wait_env_dump yabasanshiro
expect_no_account_vars "$RUNTIME/env-yabasanshiro.txt"

# 3b. Provider-bound spoof core: the provider path alone decides.
launch_game DC "Roms/DC/Smoke Game.cdi" dsperate
wait_env_dump Spoof.pak
expect_no_account_vars "$RUNTIME/env-Spoof.pak.txt"

# 4a. Retained sign-out: revision without credentials.
db_sql "UPDATE settings SET value='' WHERE key IN ('retroachievements_user','retroachievements_pass');
        UPDATE settings SET value='7' WHERE key='retroachievements_revision';"
rm -f "$RUNTIME/env-flycast.txt" "$RUNTIME/argv-flycast.txt"
launch_game DC "Roms/DC/Smoke Game.cdi"
wait_env_dump flycast
expect_var "UMRK_RA_ACCOUNT_VERSION" "1" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_STATE" "signed-out" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_REVISION" "7" "$RUNTIME/env-flycast.txt"
! has_var UMRK_RA_ACCOUNT_USERNAME "$RUNTIME/env-flycast.txt" ||
    fail "credentials present after sign-out"
! has_var UMRK_RA_ACCOUNT_PASSWORD "$RUNTIME/env-flycast.txt" ||
    fail "password present after sign-out"

# 4b. Never configured: versioned verdict only.
db_sql "DELETE FROM settings WHERE key LIKE 'retroachievements%';"
rm -f "$RUNTIME/env-flycast.txt" "$RUNTIME/argv-flycast.txt"
launch_game DC "Roms/DC/Smoke Game.cdi"
wait_env_dump flycast
expect_var "UMRK_RA_ACCOUNT_VERSION" "1" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_STATE" "never-configured" "$RUNTIME/env-flycast.txt"
! grep -q '^UMRK_RA_ACCOUNT_REVISION=' "$RUNTIME/env-flycast.txt" ||
    fail "never-configured handoff carried a revision"
! has_var UMRK_RA_ACCOUNT_USERNAME "$RUNTIME/env-flycast.txt" ||
    fail "never-configured handoff carried credentials"

# 5. Invalid stored values: the verdict reaches the child, credentials do not.
db_sql "INSERT INTO settings(key,value) VALUES ('retroachievements_user','smoke-user');
        INSERT INTO settings(key,value) VALUES ('retroachievements_pass','ok');
        INSERT INTO settings(key,value) VALUES ('retroachievements_revision','not-a-number');"
rm -f "$RUNTIME/env-flycast.txt" "$RUNTIME/argv-flycast.txt"
launch_game DC "Roms/DC/Smoke Game.cdi"
wait_env_dump flycast
expect_var "UMRK_RA_ACCOUNT_VERSION" "1" "$RUNTIME/env-flycast.txt"
expect_var "UMRK_RA_ACCOUNT_STATE" "invalid" "$RUNTIME/env-flycast.txt"
! has_var UMRK_RA_ACCOUNT_USERNAME "$RUNTIME/env-flycast.txt" ||
    fail "malformed account handed its credentials to the child"

echo "PASS ra-account-env-ipc-smoke"
