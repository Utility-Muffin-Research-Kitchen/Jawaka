#!/usr/bin/env bash
# Shallow wiring check for the Phase 6 in-game shader UI. Behavioral coverage
# lives in shader-picker-test and on the device; this catches regressions in the
# main.c call sites that those tests cannot link without the full SDL program.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT_DIR/cmd/jawaka-menu/main.c"

body_for() {
    awk -v fn="$1" '
        !inside && $0 ~ "^static .*" fn "\\(" { inside = 1; found = 1 }
        inside {
            print
            opens = gsub(/\{/, "{"); depth += opens
            closes = gsub(/\}/, "}"); depth -= closes
            if (opens > 0) seen_open = 1
            if (seen_open && depth == 0) exit
        }
        END { if (!found) exit 2 }
    ' "$SRC"
}

fail() {
    printf 'shader-menu-contract: %s\n' "$1" >&2
    exit 1
}

require() {
    local text="$1" needle="$2" message="$3"
    grep -Fq -- "$needle" <<<"$text" || fail "$message"
}

reject() {
    local text="$1" needle="$2" message="$3"
    if grep -Fq -- "$needle" <<<"$text"; then fail "$message"; fi
}

require_order() {
    local text="$1" first="$2" second="$3" message="$4"
    local first_line second_line
    first_line="$(grep -nF -- "$first" <<<"$text" | head -1 | cut -d: -f1 || true)"
    second_line="$(grep -nF -- "$second" <<<"$text" | head -1 | cut -d: -f1 || true)"
    [ -n "$first_line" ] && [ -n "$second_line" ] &&
        [ "$first_line" -lt "$second_line" ] || fail "$message"
}

main_render="$(body_for jw__render_ingame_menu)"
perf_render="$(body_for jw__render_ingame_performance)"
shader_render="$(body_for jw__render_ingame_shader)"
main_row="$(body_for jw__draw_ingame_menu_item)"
perf_row="$(body_for jw__draw_ingame_perf_item)"
shader_row="$(body_for jw__draw_ingame_shader_item)"
for body in "$main_render" "$perf_render" "$shader_render"; do
    require "$body" "cat_draw_list_pane_layered(" \
        "an in-game list no longer uses layered focus rendering"
done
for body in "$main_row" "$perf_row" "$shader_row"; do
    require "$body" "cat_draw_color_lerp(" \
        "an in-game row no longer interpolates focus color"
done

thumb="$(body_for jw__ingame_update_thumb)"
require_order "$thumb" "if (state->list.anim_active)" \
    "jw__slot_thumb_path(state, slot" \
    "cold thumbnail lookup can run before focus settles"

picker="$(body_for jw__ingame_show_shader)"
require "$picker" "int cursor = view.list.cursor;" \
    "activation no longer snapshots the logical cursor"
reject "$picker" "int cursor = view.applied_cursor;" \
    "activation uses the previously applied row instead of the logical cursor"
require_order "$picker" \
    "if (view.list.cursor == jw__shader_advanced_index(&view))" \
    "jw_shader_preview_coalescer_schedule(" \
    "Advanced is not excluded before preview scheduling"
require "$picker" "jw__shader_save_choice(socket_path, state, &view," \
    "non-Off shader activation no longer enters the scope chooser"
reject "$picker" "cursor == jw__shader_custom_index" \
    "the custom row bypasses the shared recommendation save route"

item_path="$(body_for jw__shader_item_path)"
require "$item_path" "view->picker.original_path" \
    "the custom row no longer preserves the active Advanced preset"

require "$(<"$SRC")" "#define JW_FUGAZI_RESOLVER_ASSEMBLED false" \
    "All RetroArch was enabled without an assembled Fugazi resolver"
require "$(body_for jw__shader_save_choice)" \
    "jw_shader_picker_scope_enabled(scope, JW_FUGAZI_RESOLVER_ASSEMBLED)" \
    "shader save no longer enforces the assembly gate"
require "$(body_for jw__shader_off_choice)" \
    "jw_shader_picker_scope_enabled(scope, JW_FUGAZI_RESOLVER_ASSEMBLED)" \
    "shader removal no longer enforces the assembly gate"

echo "PASS shader-menu-contract-test"
