#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/mlp1"
LOCAL_LOG_DIR="$ROOT_DIR/build/mlp1-ra-command-smoke/$(date +%Y%m%d-%H%M%S)"
REMOTE_DIR="${JAWAKA_RA_SMOKE_REMOTE_DIR:-/tmp/umrk-ra-command-smoke}"
RETROARCH_BIN="${RETROARCH_BIN:-$WORKSPACE_ROOT/retroarch-builds/output/mlp1/bin/retroarch}"
CORE_SO="${CORE_SO:-$WORKSPACE_ROOT/Cores-spruce/output/mlp1/cores/2048_libretro.so}"
CORE_INFO="${CORE_INFO:-$WORKSPACE_ROOT/Cores-spruce/output/mlp1/info/2048_libretro.info}"
RUN_LOG="$REMOTE_DIR/logs/retroarch.log"
CTL="$REMOTE_DIR/bin/jawaka-retroarchctl"
PORT="${JAWAKA_RA_COMMAND_PORT:-55355}"
VIDEO_DRIVER="${VIDEO_DRIVER:-gl}"
VIDEO_CONTEXT_DRIVER="${VIDEO_CONTEXT_DRIVER:-}"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    make -C "$ROOT_DIR" mlp1
fi

for path in \
    "$BUILD_DIR/bin/jawaka-retroarchctl" \
    "$RETROARCH_BIN" \
    "$CORE_SO" \
    "$CORE_INFO"; do
    if [ ! -f "$path" ]; then
        echo "missing required smoke artifact: $path" >&2
        exit 1
    fi
done

if [ -n "${ADB_SERIAL:-}" ]; then
    ADB=(adb -s "$ADB_SERIAL")
else
    serial="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
    if [ -z "${serial:-}" ]; then
        echo "No online adb device found." >&2
        exit 1
    fi
    ADB=(adb -s "$serial")
fi

echo "Using adb device: $("${ADB[@]}" get-serialno)"

mkdir -p "$LOCAL_LOG_DIR"

cleanup() {
    set +e
    "${ADB[@]}" shell "
REMOTE='$REMOTE_DIR'
if [ -f \"\$REMOTE/retroarch.pid\" ]; then
    pid=\$(cat \"\$REMOTE/retroarch.pid\" 2>/dev/null || true)
    if [ -n \"\$pid\" ]; then
        kill \"\$pid\" 2>/dev/null || true
    fi
fi
if [ -f \"\$REMOTE/paused-pids\" ]; then
    while read pid; do
        [ -n \"\$pid\" ] && kill -CONT \"\$pid\" 2>/dev/null || true
    done < \"\$REMOTE/paused-pids\"
fi
" >/dev/null 2>&1 || true
    "${ADB[@]}" pull "$REMOTE_DIR/logs" "$LOCAL_LOG_DIR/" >/dev/null 2>&1 || true
    set -e
}
trap cleanup EXIT

echo "Staging RetroArch command smoke to $REMOTE_DIR"
"${ADB[@]}" shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR/bin' '$REMOTE_DIR/cores' '$REMOTE_DIR/info' '$REMOTE_DIR/home' '$REMOTE_DIR/logs' '$REMOTE_DIR/system' '$REMOTE_DIR/saves' '$REMOTE_DIR/states'"
"${ADB[@]}" push "$RETROARCH_BIN" "$REMOTE_DIR/bin/retroarch" >/dev/null
"${ADB[@]}" push "$BUILD_DIR/bin/jawaka-retroarchctl" "$REMOTE_DIR/bin/jawaka-retroarchctl" >/dev/null
"${ADB[@]}" push "$CORE_SO" "$REMOTE_DIR/cores/2048_libretro.so" >/dev/null
"${ADB[@]}" push "$CORE_INFO" "$REMOTE_DIR/info/2048_libretro.info" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE_DIR/bin/retroarch' '$REMOTE_DIR/bin/jawaka-retroarchctl' '$REMOTE_DIR/cores/2048_libretro.so'"

"${ADB[@]}" shell "cat > '$REMOTE_DIR/retroarch.cfg' <<'CFG'
config_save_on_exit = \"false\"
video_driver = \"$VIDEO_DRIVER\"
video_context_driver = \"$VIDEO_CONTEXT_DRIVER\"
audio_driver = \"alsa\"
input_driver = \"sdl2\"
input_joypad_driver = \"sdl2\"
input_autodetect_enable = \"true\"
network_cmd_enable = \"true\"
network_cmd_port = \"$PORT\"
libretro_directory = \"$REMOTE_DIR/cores\"
libretro_info_path = \"$REMOTE_DIR/info\"
system_directory = \"$REMOTE_DIR/system\"
savefile_directory = \"$REMOTE_DIR/saves\"
savestate_directory = \"$REMOTE_DIR/states\"
rgui_browser_directory = \"$REMOTE_DIR\"
menu_driver = \"rgui\"
video_fullscreen = \"true\"
video_windowed_fullscreen = \"false\"
pause_nonactive = \"false\"
pause_on_disconnect = \"false\"
quit_press_twice = \"false\"
stdin_cmd_enable = \"false\"
log_verbosity = \"true\"
CFG"

"${ADB[@]}" shell "cat > '$REMOTE_DIR/launch-retroarch.sh' <<'SH'
#!/bin/sh
REMOTE='$REMOTE_DIR'
cd \"\$REMOTE\" || exit 1
export XDG_RUNTIME_DIR=/var/run
export SDL_VIDEODRIVER=wayland
export HOME=\"\$REMOTE/home\"
exec \"\$REMOTE/bin/retroarch\" --config \"\$REMOTE/retroarch.cfg\" -L \"\$REMOTE/cores/2048_libretro.so\" --verbose > '$RUN_LOG' 2>&1 < /dev/null
SH
chmod 755 '$REMOTE_DIR/launch-retroarch.sh'"

echo "Pausing current launcher/Jawaka pids for screen ownership"
"${ADB[@]}" shell "
REMOTE='$REMOTE_DIR'
: > \"\$REMOTE/paused-pids\"
for name in loong_pangu jawaka-launcher jawakad; do
    for pid in \$(pidof \"\$name\" 2>/dev/null || true); do
        echo \"\$pid\" >> \"\$REMOTE/paused-pids\"
        kill -STOP \"\$pid\" 2>/dev/null || true
    done
done
kill \$(pidof retroarch 2>/dev/null) 2>/dev/null || true
"

echo "Launching RetroArch 2048 smoke core"
"${ADB[@]}" shell "rm -f '$REMOTE_DIR/retroarch.pid' && start-stop-daemon -S -b -m -p '$REMOTE_DIR/retroarch.pid' -x '$REMOTE_DIR/launch-retroarch.sh'"

echo "Waiting for RetroArch command port $PORT"
ready=0
for _ in $(seq 1 20); do
    if "${ADB[@]}" shell "grep -q 'bringing_up_command_interface_at_port $PORT' '$RUN_LOG' 2>/dev/null"; then
        ready=1
        break
    fi
    sleep 0.5
done

if [ "$ready" != "1" ]; then
    echo "RetroArch command interface did not become ready." >&2
    "${ADB[@]}" shell "tail -120 '$RUN_LOG' 2>/dev/null || true"
    exit 1
fi

run_ctl() {
    echo "== $* =="
    "${ADB[@]}" shell "$CTL --timeout-ms 1500 --port '$PORT' $*"
}

run_ctl status
run_ctl pause
run_ctl status
run_ctl resume
run_ctl status
run_ctl menu-toggle
run_ctl menu-toggle
run_ctl save-state
sleep 1
"${ADB[@]}" shell "state_files=\$(find '$REMOTE_DIR/states' -type f -print 2>/dev/null | sed -n '1,20p'); if [ -n \"\$state_files\" ]; then printf '%s\n' \"\$state_files\"; else echo 'state_file=not_observed; 2048 smoke currently verifies save/load send path only'; fi"
run_ctl load-state
run_ctl quit

sleep 2
if "${ADB[@]}" shell "pidof retroarch >/dev/null 2>&1"; then
    echo "RetroArch is still running after quit." >&2
    exit 1
fi

echo "RetroArch command smoke log tail:"
"${ADB[@]}" shell "tail -120 '$RUN_LOG' 2>/dev/null || true"

echo "RetroArch command smoke completed. Logs: $LOCAL_LOG_DIR"
