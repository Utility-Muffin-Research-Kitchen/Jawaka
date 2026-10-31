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

python3 - "$SOCKET" <<'PY'
import json
import socket
import struct
import sys
import time

socket_path = sys.argv[1]


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
if not isinstance(caps, list) or not {"user_synopsis", "metadata_changed"} <= set(caps):
    raise RuntimeError(f"metadata_caps missing: {before}")

reply = request({"type": "library-metadata-changed"})
if reply.get("type") != "ok":
    raise RuntimeError(f"library-metadata-changed rejected: {reply}")

after = request({"type": "library-status"})
if after.get("generation") != before.get("generation") + 1:
    raise RuntimeError(f"generation did not move by one: {before} -> {after}")
if after.get("scan_running") or after.get("pending_rescan"):
    raise RuntimeError(f"library-metadata-changed started a scan: {after}")
PY

echo "library-metadata-ipc-smoke: ok"
