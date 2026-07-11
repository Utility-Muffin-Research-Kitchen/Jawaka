#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-inhibit-smoke.XXXXXX")"
RUNTIME_DIR="$TMP_DIR/runtime"
SOCKET="$RUNTIME_DIR/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"

cleanup() {
    set +e
    if [ -n "${DAEMON_PID:-}" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad mockgen >/dev/null
mkdir -p "$RUNTIME_DIR"
(
    cd "$ROOT_DIR"
    UMRK_RUNTIME_PATH="$RUNTIME_DIR" \
    UMRK_DAEMON_SOCKET="$SOCKET" \
    JAWAKA_SDCARD_ROOT="$ROOT_DIR/mock-sdcard" \
    build/bin/jawakad --daemon-only >"$LOG" 2>&1
) &
DAEMON_PID=$!

for _ in $(seq 1 100); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || { cat "$LOG" >&2; exit 1; }
    sleep 0.02
done
[ -S "$SOCKET" ] || { echo "jawakad socket did not appear" >&2; cat "$LOG" >&2; exit 1; }

python3 "$ROOT_DIR/scripts/suspend-inhibit-ipc-smoke.py" "$SOCKET"
