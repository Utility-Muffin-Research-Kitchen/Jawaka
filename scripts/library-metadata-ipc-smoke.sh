#!/usr/bin/env bash
# A library editor's view of a real jawakad: library-status says what it supports,
# and library-metadata-changed moves the generation the launcher reloads on,
# without starting a scan.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-library-metadata.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
PLATFORM="$TMP_DIR/platform"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ "$status" -ne 0 ] && [ -f "$LOG" ]; then
        cat "$LOG" >&2
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad mockgen >/dev/null
cp -R "$ROOT_DIR/mock-sdcard" "$PRIMARY"
mkdir -p "$STATE" "$RUNTIME" "$PLATFORM/defaults"

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" \
    UMRK_INTERNAL_DATA_PATH="$STATE" \
    UMRK_PLATFORM_PATH="$PLATFORM" \
    JAWAKA_SDCARD_ROOT="$PRIMARY" \
    SDCARD_PATHS="$PRIMARY" \
    build/bin/jawakad --daemon-only

for _ in $(seq 1 500); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || { cat "$LOG" >&2; exit 1; }
    sleep 0.02
done
[ -S "$SOCKET" ] || { echo "jawakad socket did not appear" >&2; exit 1; }

python3 - "$SOCKET" "$STATE/library.db" <<'PY'
import json
import socket
import sqlite3
import struct
import sys
import time

socket_path, db_path = sys.argv[1:]


def request(payload):
    body = json.dumps(payload).encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(socket_path)
        sock.sendall(struct.pack("!I", len(body)) + body)
        size = struct.unpack("!I", sock.recv(4))[0]
        data = bytearray()
        while len(data) < size:
            chunk = sock.recv(size - len(data))
            if not chunk:
                raise RuntimeError("short IPC response")
            data.extend(chunk)
    return json.loads(data)


deadline = time.monotonic() + 15
while True:
    before = request({"type": "library-status"})
    if not before.get("scan_running") and not before.get("pending_rescan"):
        break
    if time.monotonic() > deadline:
        raise RuntimeError(f"first scan did not finish: {before}")
    time.sleep(0.02)

caps = before.get("metadata_caps")
if not isinstance(caps, list) or not {"user_synopsis", "metadata_changed", "sort_rank", "folder_rows"} <= set(caps):
    raise RuntimeError(f"metadata_caps missing: {before}")

reply = request({"type": "library-metadata-changed"})
if reply.get("type") != "ok":
    raise RuntimeError(f"library-metadata-changed rejected: {reply}")

after = request({"type": "library-status"})
if after.get("generation") != before.get("generation") + 1:
    raise RuntimeError(f"generation did not move by one: {before} -> {after}")
if after.get("scan_running") or after.get("pending_rescan"):
    raise RuntimeError(f"library-metadata-changed started a scan: {after}")

# The rows of one level, as the launcher would draw them.
db = sqlite3.connect(db_path, timeout=5)
system = db.execute("SELECT system, COUNT(*) c FROM games GROUP BY 1 ORDER BY c DESC").fetchone()[0]
folder_prefix = db.execute("SELECT rom_relpath FROM games WHERE system=?", (system,)).fetchone()[0].split("/")[0]
# This scan is flat, so two games in a folder go in directly.
for name in ("one", "two"):
    relpath = f"{folder_prefix}/Pinned Folder/{name}.gb"
    db.execute(
        "INSERT INTO games (system, name, source_id, rom_relpath, rom_path) "
        "VALUES (?, ?, 'primary', ?, ?)",
        (system, name, relpath, "Roms/" + relpath),
    )
db.commit()


def execute(sql, *args):
    db.execute(sql, args)
    db.commit()


def level(folder=""):
    reply = request({"type": "library-folder-rows", "system": system, "folder": folder})
    if reply.get("type") != "library-folder-rows":
        raise RuntimeError(f"library-folder-rows rejected: {reply}")
    return reply


if request({"type": "library-folder-rows"}).get("type") != "error":
    raise RuntimeError("library-folder-rows accepted a request with no system")

flat = level()
count = db.execute("SELECT COUNT(*) FROM games WHERE system=?", (system,)).fetchone()[0]
if flat["show_folders"] or len(flat["rows"]) != count or flat["rows"][0]["kind"] != "game":
    raise RuntimeError(f"flat level wrong: {flat['show_folders']} {len(flat['rows'])} of {count}")

# A pinned game leads the flat list.
last = next(row for row in reversed(flat["rows"]) if "Pinned Folder" not in row["path"])
game_id = db.execute(
    "SELECT id FROM games WHERE source_id=? AND rom_relpath=?", (last["source"], last["path"])
).fetchone()[0]
execute("INSERT INTO game_settings VALUES (?, 'sort_rank', '1', 0)", game_id)
pinned = level()["rows"][0]
if pinned["path"] != last["path"] or pinned["rank"] != 1:
    raise RuntimeError(f"pinned game not first: {pinned}")

# With Show Folders on, the folder model decides the rows.
execute("INSERT OR REPLACE INTO settings (key, value) VALUES ('game_folders', '1')")
root = level()
if not root["show_folders"] or root["folder"] != "":
    raise RuntimeError(f"root level wrong: {root}")
if root["rows"][0].get("path") != last["path"]:
    raise RuntimeError(f"pinned game not first among folders: {root['rows'][:3]}")
folders = [row for row in root["rows"] if row["kind"] == "folder"]
ours = next(row for row in folders if row["name"] == "Pinned Folder")
if ours["games"] != 2 or ours["rank"] != 0:
    raise RuntimeError(f"folder row wrong: {ours}")

execute(
    "INSERT INTO system_settings VALUES (?, ?, '1', 0)",
    system, "folder_rank:" + ours["path"].lower(),
)
root = level()
if root["rows"][0].get("kind") != "folder" or root["rows"][0]["path"] != ours["path"]:
    raise RuntimeError(f"tied folder should lead: {root['rows'][:3]}")
if root["rows"][0]["rank"] != 1:
    raise RuntimeError(f"folder rank not reported: {root['rows'][0]}")

inner = level(ours["path"])
if inner.get("parent") != "" or {row["name"] for row in inner["rows"]} != {"one", "two"}:
    raise RuntimeError(f"folder level wrong: {inner}")
gone = level(ours["path"] + "/Nowhere")
if gone["folder"] != ours["path"]:
    raise RuntimeError(f"missing folder should land on its ancestor: {gone['folder']}")
PY

echo "library-metadata-ipc-smoke: ok"
