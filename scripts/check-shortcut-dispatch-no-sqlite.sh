#!/usr/bin/env bash
# The shortcut dispatch path must not reach SQLite.
#
# jw__on_shortcut_chord() runs from the input proxy's evdev callback, on the
# same loop that drains the gamepad. jw_db_* opens a connection, reapplies
# schema and can wait on SQLite's busy timeout, so one call there turns a
# settings lookup into dropped or delayed controller input.
#
# This is a reachability check over the call graph rooted at the dispatch
# callback, not a grep of one function: the regression this guards against was
# a *lazy* database read two frames down, inside a hotkey handler, behind a TTL
# that only expired sometimes.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT_DIR/cmd/jawakad/main.c"

# Functions reachable from the callback. Kept explicit rather than parsed: the
# set is tiny by design, and a new call added here should be a deliberate edit
# to this list, which is the point at which someone rechecks the constraint.
ROOTS=(
    jw__on_shortcut_chord
    jw__input_game_switcher
    jw__on_screenshot_hotkey
    jw__on_record_hotkey
)

status=0
for fn in "${ROOTS[@]}"; do
    body="$(awk -v f="static bool $fn(" '
        index($0, f) { depth = 0; inside = 1 }
        inside {
            print
            n = gsub(/\{/, "{"); depth += n
            m = gsub(/\}/, "}"); depth -= m
            if (depth == 0 && NR > 1 && /\}/) { inside = 0 }
        }
    ' "$SRC")"
    if [ -z "$body" ]; then
        echo "check-shortcut-dispatch: could not find $fn in main.c" >&2
        exit 2
    fi
    if hit="$(printf '%s\n' "$body" | grep -nE 'jw_db_[a-z_]+\(|sqlite3_' || true)"; [ -n "$hit" ]; then
        echo "check-shortcut-dispatch: $fn reaches SQLite from the input path:" >&2
        printf '%s\n' "$hit" | sed 's/^/  /' >&2
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    echo "" >&2
    echo "Cache the value and refresh it from jw__tick_feature_flags() on the" >&2
    echo "main loop instead of reading it from the input callback." >&2
    exit 1
fi
echo "shortcut dispatch: no SQLite reachable from the input path"
