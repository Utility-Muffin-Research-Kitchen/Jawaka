#!/usr/bin/env bash
# CONTENT-1 end-to-end ordering/classification smoke. Proves that install and
# uninstall affect the ROM folder in the same scan and that pure/hybrid app
# classification follows executable launch.sh, including refused shared paks.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD:-build/content-catalog-smoke}"
PAYLOAD="${PAYLOAD:-}"
TMP_ROOT="$(mktemp -d /tmp/jw-content.XXXXXX)"
trap 'status=$?; rm -rf "$TMP_ROOT"; exit "$status"' EXIT

if [ -z "$PAYLOAD" ]; then
    PAYLOAD="$(ls -td "$ROOT_DIR"/../Leaf/build/release/sd-*/.system/leaf/releases/*/platforms/mlp1 \
        2>/dev/null | head -1 || true)"
fi
if [ -z "$PAYLOAD" ] || [ ! -f "$PAYLOAD/defaults/systems.json" ]; then
    echo "content-catalog-smoke: no release payload found; pass PAYLOAD=<platform dir>" >&2
    exit 2
fi

SD="$TMP_ROOT/sd"
PLATFORM_DIR="$TMP_ROOT/platform"
STATE_DIR="$TMP_ROOT/state"
DB="$TMP_ROOT/library.db"
mkdir -p "$SD/Apps/mac" "$SD/Apps/shared" "$SD/Roms/CONTENTTEST" \
         "$SD/Images" "$PLATFORM_DIR/defaults" "$STATE_DIR"
cp -R "$PAYLOAD/info" "$PLATFORM_DIR/info"
cp "$PAYLOAD"/defaults/* "$PLATFORM_DIR/defaults/"

python3 - "$PLATFORM_DIR" <<'PY'
import json, pathlib, sys
base = pathlib.Path(sys.argv[1], "defaults")
for name in ("systems.json", "cores.json", "cores.v2.json"):
    path = base / name
    if not path.exists():
        continue
    doc = json.loads(path.read_text())
    doc["platform"] = "mac"
    for core in doc.get("cores", []):
        core["platforms"] = ["mac"]
    path.write_text(json.dumps(doc, indent=2))
PY
printf '%s\n' '{"schema":1,"version":"0.11.0","release_id":"content-smoke-1"}' \
    > "$STATE_DIR/release.json"
printf '%s\n' rom > "$SD/Roms/CONTENTTEST/game.ctest"

make_pak() {
    pak_dir="$1" platform="$2" name="$3"
    mkdir -p "$pak_dir/art" "$pak_dir/cores" "$pak_dir/info" \
             "$pak_dir/emulators"
    printf '%s\n' PNG > "$pak_dir/art/icon.png"
    printf '%s\n' PHOTO > "$pak_dir/art/icon-photo.png"
    printf '%s\n' fake-core > "$pak_dir/cores/contenttest_libretro.so"
    printf '%s\n' 'display_name = "Content Test"' \
        > "$pak_dir/info/contenttest_libretro.info"
    printf '%s\n' '#!/bin/sh' 'exit 0' > "$pak_dir/emulators/run.sh"
    chmod +x "$pak_dir/emulators/run.sh"
    python3 - "$pak_dir/pak.json" "$platform" "$name" <<'PY'
import json, sys
path, platform, name = sys.argv[1:]
json.dump({
  "name": name, "platform": platform, "pak_version": "1.0.0",
  "min_leaf_version": "0.11.0",
  "provides": {"schema": 1, "systems": [{
    "id": "CONTENTTEST", "name": "Content Test", "patterns": ["CONTENTTEST"],
    "extensions": ["ctest"], "rom_root": "Roms/CONTENTTEST",
    "image_root": "Images/CONTENTTEST", "default_core": "contenttest",
    "alternate_cores": ["contentpath"],
    "icon_flat": "art/icon.png", "icon_photographic": "art/icon-photo.png"
  }], "cores": [{
    "id": "contenttest", "display_name": "Content Test", "type": "retroarch",
    "libretro_name": "contenttest",
    "file_name": "cores/contenttest_libretro.so",
    "info_name": "info/contenttest_libretro.info",
    "config_folder": "ContentTest"
  }, {
    "id": "contentpath", "display_name": "Content Path", "type": "path",
    "path": "emulators/run.sh"
  }]}
}, open(path, "w"), indent=2)
PY
}

make_pak "$SD/Apps/mac/ContentTest.pak" mac "Content Test"
make_pak "$SD/Apps/shared/SharedPure.pak" shared "Shared Pure"
make_pak "$SD/Apps/shared/SharedHybrid.pak" shared "Shared Hybrid"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$SD/Apps/shared/SharedHybrid.pak/launch.sh"
chmod +x "$SD/Apps/shared/SharedHybrid.pak/launch.sh"

export PLATFORM=mac
export SDCARD_PATH="$SD"
export APPS_PATH="$SD/Apps"
export ROMS_PATH="$SD/Roms"
export IMAGES_PATH="$SD/Images"
export UMRK_PLATFORM_PATH="$PLATFORM_DIR"
export UMRK_INTERNAL_DATA_PATH="$STATE_DIR"

make -C "$ROOT_DIR" -s BUILD="$BUILD_DIR" \
    jawaka-scan-smoke jawaka-content-runtime-smoke
SCAN="$ROOT_DIR/$BUILD_DIR/bin/jawaka-scan-smoke"
RUNTIME_SMOKE="$ROOT_DIR/$BUILD_DIR/bin/jawaka-content-runtime-smoke"

FIRST="$TMP_ROOT/first.txt"
"$SCAN" "$SD" "$DB" > "$FIRST"
grep -q $'^game\tCONTENTTEST\tgame\t' "$FIRST" \
    || { echo "FAIL: installed content folder missed the same scan" >&2; exit 1; }
! grep -q $'^app\tContent Test\t' "$FIRST" \
    || { echo "FAIL: pure content pak appeared in Apps" >&2; exit 1; }
! grep -q $'^app\tShared Pure\t' "$FIRST" \
    || { echo "FAIL: refused shared pure content pak appeared in Apps" >&2; exit 1; }
grep -q $'^app\tShared Hybrid\t' "$FIRST" \
    || { echo "FAIL: refused shared hybrid lost its executable app" >&2; exit 1; }
python3 - "$STATE_DIR/catalog/diagnostics.json" <<'PY'
import json, sys
entries=json.load(open(sys.argv[1]))["entries"]
providers={e.get("provider") for e in entries if e.get("reason") == "shared-content-unsupported"}
assert providers == {"shared/SharedPure.pak", "shared/SharedHybrid.pak"}, providers
PY
FIRST_GEN="$(tr -d '\n' < "$STATE_DIR/catalog/current")"
python3 - "$STATE_DIR/catalog/$FIRST_GEN" <<'PY'
import json, sys
from pathlib import Path
generation=Path(sys.argv[1])
stamp=json.load(open(generation / "stamp.json"))
assert [c["provider"] for c in stamp["contributors"]] == ["mac/ContentTest.pak"]
files=stamp["contributors"][0]["files"]
assert [f["rel"] for f in files] == [
    "art/icon-photo.png", "art/icon.png", "cores/contenttest_libretro.so",
    "emulators/run.sh", "info/contenttest_libretro.info"
]
assert stamp["base"]["systems_sha256"] != stamp["output"]["systems_sha256"]
assert stamp["base"]["info_sha256"] != stamp["output"]["info_sha256"]
assert (generation / "info/contenttest_libretro.info").read_text() == \
       'display_name = "Content Test"\n'
PY
echo "ok: install is visible in the same scan; pure/hybrid classification is correct"

# P1-7: declared bytes are stamp inputs even when the merged JSON is unchanged.
printf '%s\n' PNG-CHANGED > "$SD/Apps/mac/ContentTest.pak/art/icon.png"
"$SCAN" "$SD" "$DB" > "$TMP_ROOT/fingerprint.txt"
SECOND_GEN="$(tr -d '\n' < "$STATE_DIR/catalog/current")"
test "$SECOND_GEN" != "$FIRST_GEN" \
    || { echo "FAIL: declared-file replacement did not move the generation" >&2; exit 1; }
RUNTIME_OUT="$TMP_ROOT/runtime.txt"
"$RUNTIME_SMOKE" "$SD" "$PLATFORM_DIR/cores" "$PLATFORM_DIR" \
    CONTENTTEST "$FIRST_GEN" "$SECOND_GEN" > "$RUNTIME_OUT"
python3 - "$RUNTIME_OUT" "$SD" "$STATE_DIR/catalog" "$FIRST_GEN" "$SECOND_GEN" <<'PY'
from pathlib import Path
import sys
rows=[line.rstrip("\n").split("\t") for line in open(sys.argv[1])]
assert [row[0] for row in rows] == ["old", "new"], rows
sd=Path(sys.argv[2]); catalog=Path(sys.argv[3])
core=str(sd / "Apps/mac/ContentTest.pak/cores/contenttest_libretro.so")
standalone=str(sd / "Apps/mac/ContentTest.pak/emulators/run.sh")
flat=str(sd / "Apps/mac/ContentTest.pak/art/icon.png")
photo=str(sd / "Apps/mac/ContentTest.pak/art/icon-photo.png")
assert rows[0][1:] == [core, standalone, str(catalog / sys.argv[4] / "info"), flat, photo]
assert rows[1][1:] == [core, standalone, str(catalog / sys.argv[5] / "info"), flat, photo]
PY
echo "ok: pak-relative core/standalone paths resolve live; generation swap moves info"

rm -rf "$SD/Apps/mac/ContentTest.pak"
SECOND="$TMP_ROOT/second.txt"
"$SCAN" "$SD" "$DB" > "$SECOND"
! grep -q $'^game\tCONTENTTEST\t' "$SECOND" \
    || { echo "FAIL: uninstalled content survived the same scan" >&2; exit 1; }
echo "ok: uninstall is removed in the same scan"
echo "content-catalog-smoke: ok"
