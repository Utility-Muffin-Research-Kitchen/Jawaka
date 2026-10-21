#!/usr/bin/env python3
"""Extract Leaf's translatable strings into i18n/leaf.pot.

Why not xgettext: `--keyword=T` only sees strings written inside T(...), and
most of Leaf's UI text is not. Labels are literals at funnel call sites
(jw__render_list_row(..., "Auto Sleep", ...)) that a shared renderer translates,
footer hints live in cat_footer_item initializers, and a handful of label
arrays (tabs, system names) are translated where they are indexed. An extractor
that does not know those shapes reports a fraction of the real universe, and
the coverage gate would gate on the wrong total.

This extractor knows exactly four shapes, listed in FUNNELS / ARRAYS below.
When a new funnel is added, add it here -- the CI check will not notice its
strings otherwise. That is the maintenance contract, and it is spelled out in
umrk-workspace/plans/leaf-i18n-chinese.md.

Usage:
    tools/i18n-extract.py                 # rewrite i18n/leaf.pot
    tools/i18n-extract.py --check         # fail if the committed .pot is stale
    tools/i18n-extract.py --check --po i18n/zh_CN.po
                                          # ...and fail on orphan keys in the .po
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POT = ROOT / "i18n" / "leaf.pot"
# Committed so it can be read without a checkout or a build: the README badges
# point shields.io at this file's raw URL, and --check keeps it honest, so a
# badge can never claim a coverage the repo does not actually have.
COVERAGE = ROOT / "i18n" / "coverage.json"

# Sources that draw UI. The i18n test's T() calls are fixtures, not UI.
SOURCE_GLOBS = ["cmd/**/*.c", "internal/**/*.c"]
EXCLUDE_PARTS = ("third_party", "cmd/jawaka-i18n-test")

# Funnel calls whose string-literal arguments are all UI text. The extractor
# takes every double-quoted literal inside the call's parentheses; each of
# these was checked by hand to carry no non-UI literals at any call site.
FUNNELS = [
    "T",
    "jw__render_list_row",
    "jw__render_list_row_h",
    "jw__render_toggle_row",
    "jw__render_switch_row",
    "jw__render_nav_row",
    "jw__draw_header",
    "jw__draw_slider_row",
    "jw__draw_slider_row_ex",
    "jw__render_account_row",
    "jw__draw_info_title",
    "jw__about_push",
    "jw__fsetup_header",
    "jw__fsetup_footer",
    "jw__update_msg",
    "jw__copy_status",
    # Message sinks whose literal arguments are user-facing text. Added after
    # the 2026-10 audit: these carried 400+ strings that never reached the .pot
    # (daemon error replies, Settings Wi-Fi/Bluetooth toasts, update-runner
    # progress, Pak Rat notices, storage-ui feedback).
    "jw__launch_notice",
    "jw__bt_msg",
    "jw__wifi_msg",
    "jw__set_message",
    "jw__pakrat_notice",
    "jw__storage_ui_message",
    "jw__set_pakrat_message",
]

# Funnels that only hold UI text in ONE file. jw__reply_error answers a UI
# client in the daemon (the text becomes a notice/status line), but in the OSD
# it answers the daemon itself -- a protocol diagnostic the user never sees.
# A global funnel would put those 12 OSD strings into the .pot for translators.
FILE_FUNNELS = [
    ("cmd/jawakad/main.c", "jw__reply_error"),
    # Error-message writers whose text is displayed verbatim by the Delete
    # dialog, the scrape queue detail or the core-metadata notice.
    ("internal/discovery/content.c", "jw__fail"),
    ("internal/discovery/delete.c", "jw__error"),
    ("internal/scrape/ss_client.c", "jw__ss_set_error"),
    ("internal/retroarch/catalog.c", "jw_ra_set_error"),
]

# Calls where only ONE argument is UI text, so they cannot be funnels (the
# other arguments are ids, states or printf values). Index is 0-based.
ARG_SINKS = [
    ("jw__write_result", 2),     # (req, state, message)
    ("jw__pakrat_append", 3),    # (message, capacity, &used, fmt, ...)
    ("CAT_LIST_ITEM", 0),        # (label, id)
]

# A run of adjacent string literals -- C concatenates "a" "b" into one string,
# and several dialog messages span lines that way. Every extraction site below
# must treat the run as ONE key or the runtime key never matches.
STR = r'"(?:[^"\\]|\\.)*"'
RUN = re.compile(STR + r"(?:\s*" + STR + r")*")


def run_to_text(run: str) -> str:
    return "".join(m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', run))


# Struct-initializer shapes. Positional footers ({ CAT_BTN_X, "Label" }) and the
# designated-init dialogs (.message = "...", .label = "...") are both UI text --
# verified by hand against every occurrence in the tree.
FOOTER_RE = re.compile(r"\{\s*CAT_BTN_[A-Z0-9_]+\s*,\s*(" + RUN.pattern + r")")
# JW_UI("...") -- the marker for UI text in a static initializer. See i18n.h.
JW_UI_RE  = re.compile(r"\bJW_UI\s*\(\s*(" + RUN.pattern + r")\s*\)")
DESIG_RE  = re.compile(r"\.(?:message|label|title|help_text|text|body|hint)\s*=\s*(" + RUN.pattern + r")")
# Test fixtures use .title/.body for sample data, not UI. Keep the historical
# .message/.label shape there (which the committed .pot already reflects).
DESIG_TEST_RE = re.compile(r"\.(?:message|label)\s*=\s*(" + RUN.pattern + r")")

# Label arrays translated where they are indexed (T(kTabs[i]) and the like).
# kSystemDisplayNames rows are {"ID", "Display Name"}; only the name is UI.
ARRAYS = [
    ("cmd/jawaka-launcher/main.c", "kTabs", "all"),
    ("cmd/jawaka-launcher/main.c", "kSysMenuTabs", "all"),
    ("cmd/jawaka-launcher/main.c", "kSysActions", "all"),
    ("cmd/jawaka-launcher/main.c", "kSysInfo", "all"),
    ("cmd/jawaka-menu/main.c", "kInGameItems", "all"),
    ("internal/settings/settings.c", "kStartupTabLabels", "all"),
    ("internal/settings/settings.c", "kAutoSleepLabels", "all"),
    ("internal/launcher/system_names.c", "kSystemDisplayNames", "second"),
    ("internal/settings/settings.c", "kHomeCategories", "first"),
    ("internal/settings/timezones.c", "kJawakaTimeZones", "first"),
    ("cmd/jawaka-menu/main.c", "kCpuPerfOptions", "first"),
    ("cmd/jawaka-menu/main.c", "kGpuPerfOptions", "first"),
    ("cmd/jawaka-menu/main.c", "kDmcPerfOptions", "first"),
]

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_unescape(s: str) -> str:
    return (s.replace(r"\n", "\n").replace(r"\t", "\t")
             .replace(r"\"", '"').replace(r"\\", "\\"))


def strip_comments(src: str) -> str:
    """Drop comments so a commented-out call is not extracted. String contents
    are preserved (comment markers inside string literals survive because the
    scanner tracks quoting)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n and (src[j] != '"' or src[j - 1] == "\\"):
                j += 1
            out.append(src[i:j + 1]); i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c); i += 1
    return "".join(out)


def call_spans(src: str, name: str):
    """Yield the text between the parentheses of each `name(...)` call."""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", src):
        depth, j = 1, m.end()
        while j < len(src) and depth:
            c = src[j]
            if c == '"':
                j += 1
                while j < len(src) and (src[j] != '"' or src[j - 1] == "\\"):
                    j += 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            j += 1
        yield src[m.end():j - 1]


def split_args(span: str):
    """Split the text between a call's parentheses into top-level arguments."""
    args, depth, start, i = [], 0, 0, 0
    n = len(span)
    while i < n:
        c = span[i]
        if c == '"':
            i += 1
            while i < n and (span[i] != '"' or span[i - 1] == "\\"):
                i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            args.append(span[start:i])
            start = i + 1
        i += 1
    args.append(span[start:])
    return args


def array_literals(src: str, name: str, mode: str):
    m = re.search(re.escape(name) + r"[^=]*=\s*\{", src)
    if not m:
        return
    depth, j = 1, m.end()
    while j < len(src) and depth:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
        j += 1
    body = src[m.end():j - 1]
    if mode == "all":
        for lit in STRING_RE.findall(body):
            yield lit
    else:  # struct rows: "second" takes {"ID","Name"}, "first" takes {"Label",...}
        idx = 1 if mode == "second" else 0
        for row in re.finditer(r"\{([^{}]*)\}", body):
            lits = STRING_RE.findall(row.group(1))
            if len(lits) > idx:
                yield lits[idx]


def extract():
    keys = {}

    def add(lit, where):
        text = c_unescape(lit)
        if not text or text == "%s":
            return
        keys.setdefault(text, where)

    files = []
    for pattern in SOURCE_GLOBS:
        files.extend(ROOT.glob(pattern))
    for f in sorted(set(files)):
        rel = f.relative_to(ROOT).as_posix()
        if any(part in rel for part in EXCLUDE_PARTS):
            continue
        src = strip_comments(f.read_text(encoding="utf-8", errors="replace"))
        funnels = FUNNELS + [fn for file, fn in FILE_FUNNELS if rel == file]
        for fn in funnels:
            for span in call_spans(src, fn):
                for m in RUN.finditer(span):
                    add(run_to_text(m.group(0)), rel)
        for m in FOOTER_RE.finditer(src):
            add(run_to_text(m.group(1)), rel)
        is_test = rel.endswith("_test.c")
        for m in (DESIG_TEST_RE if is_test else DESIG_RE).finditer(src):
            add(run_to_text(m.group(1)), rel)
        # Calls where exactly one argument is UI text (the others carry states,
        # ids or values), so a funnel would over-collect.
        for fn, idx in ([] if is_test else ARG_SINKS):
            for span in call_spans(src, fn):
                args = split_args(span)
                if len(args) > idx:
                    m = RUN.fullmatch(args[idx].strip())
                    if m:
                        add(run_to_text(m.group(0)), rel)
        for arr_file, arr_name, mode in ARRAYS:
            if rel == arr_file:
                for lit in array_literals(src, arr_name, mode):
                    add(lit, rel)
        # JW_UI("...") marks a UI string that cannot be wrapped in T() where it
        # is written, because it sits in a `static const` initializer. Preferred
        # over adding a table to ARRAYS: the mark lives next to the string, so a
        # missing one shows up in review rather than as a silent gap in a list
        # nobody reads. ARRAYS stays only for the tables that predate this.
        for m in JW_UI_RE.finditer(src):
            add(run_to_text(m.group(1)), rel)
    return keys


def write_pot(keys, path: Path):
    lines = [
        "# Leaf UI strings. GENERATED by tools/i18n-extract.py -- do not edit;",
        "# regenerate with `make i18n-pot`. The English string is the key, and",
        "# a context prefix (\"verb|Open\") is part of the key.",
        'msgid ""',
        'msgstr "Content-Type: text/plain; charset=UTF-8\\n"',
        "",
    ]
    for key in sorted(keys):
        esc = key.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        lines.append(f"#: {keys[key]}")
        lines.append(f'msgid "{esc}"')
        lines.append('msgstr ""')
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def pot_keys(path: Path):
    out = set()
    for m in re.finditer(r'^msgid "((?:[^"\\]|\\.)*)"', path.read_text(encoding="utf-8"), re.M):
        if m.group(1):
            out.add(c_unescape(m.group(1)))
    return out


def po_entries(path: Path):
    """(all keys, translated keys). A key with an empty msgstr is present but
    untranslated -- it must count for the orphan check and NOT for coverage,
    or a fully-seeded file reads as 100% before anyone has reviewed a word.

    Continuation lines are joined: a .po wraps any long string as `msgstr ""`
    followed by one quoted fragment per line, and it is still one string. This
    used to read only the first line, so every wrapped translation looked empty
    and was counted untranslated -- understating every language's coverage
    (Mexican Spanish read 90% on a complete file) -- and a wrapped msgid was
    never registered at all, so the orphan check could not see it.

    Fuzzy entries count as present but not translated, matching i18n-compile.py,
    which never ships them: coverage should describe what a player sees."""
    text = path.read_text(encoding="utf-8")
    all_keys, translated = set(), set()
    for block in re.split(r"\n\s*\n", text):
        fuzzy = False
        cur = None
        parts = {"msgctxt": [], "msgid": [], "msgstr": []}
        for line in block.splitlines():
            if line.startswith("#~"):
                cur = None
                continue
            if line.startswith("#,") and "fuzzy" in line:
                fuzzy = True
                continue
            if line.startswith("#"):
                continue
            m = re.match(r'^(msgctxt|msgid|msgstr) "((?:[^"\\]|\\.)*)"', line)
            if m:
                cur = m.group(1)
                parts[cur].append(m.group(2))
                continue
            m = re.match(r'^"((?:[^"\\]|\\.)*)"', line)
            if m and cur:
                parts[cur].append(m.group(1))
        val = c_unescape("".join(parts["msgid"]))
        if not val:
            continue                      # the header entry, or no entry at all
        ctx = c_unescape("".join(parts["msgctxt"]))
        key = f"{ctx}|{val}" if ctx else val
        all_keys.add(key)
        if "".join(parts["msgstr"]) and not fuzzy:
            translated.add(key)
    return all_keys, translated


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail if i18n/leaf.pot is stale instead of rewriting it")
    ap.add_argument("--po", nargs="*", default=[],
                    help="translation files to validate against the extracted set")
    args = ap.parse_args()

    keys = extract()
    print(f"extracted {len(keys)} keys")

    rc = 0
    if args.check:
        if not POT.exists():
            print(f"FAIL: {POT} does not exist; run tools/i18n-extract.py", file=sys.stderr)
            return 1
        committed = pot_keys(POT)
        fresh = set(keys)
        missing = sorted(fresh - committed)
        stale = sorted(committed - fresh)
        if missing or stale:
            for k in missing[:20]:
                print(f"  not in committed .pot: {k!r}", file=sys.stderr)
            if len(missing) > 20:
                print(f"  ... and {len(missing) - 20} more", file=sys.stderr)
            for k in stale[:20]:
                print(f"  in .pot but no longer in code: {k!r}", file=sys.stderr)
            if len(stale) > 20:
                print(f"  ... and {len(stale) - 20} more", file=sys.stderr)
            print("FAIL: i18n/leaf.pot is stale; run `make i18n-pot` and commit it",
                  file=sys.stderr)
            rc = 1
        else:
            print("i18n/leaf.pot is current")
    else:
        write_pot(keys, POT)
        print(f"wrote {POT.relative_to(ROOT)}")

    universe = set(keys)
    coverage: dict[str, dict[str, int]] = {}
    for po in args.po:
        p = Path(po)
        if not p.exists():
            continue
        pk, translated = po_entries(p)
        orphans = sorted(pk - universe)
        if orphans:
            for k in orphans[:20]:
                print(f"  {p.name}: key not in the code: {k!r}", file=sys.stderr)
            print(f"FAIL: {p.name} has {len(orphans)} orphan key(s) -- an English "
                  "string was probably renamed; msgmerge the translation forward",
                  file=sys.stderr)
            rc = 1
        else:
            covered = len(translated & universe)
            print(f"{p.name}: {covered}/{len(universe)} translated "
                  f"({covered * 100 // max(1, len(universe))}%)")
            coverage[p.stem] = {
                "translated": covered,
                "percent": covered * 100 // max(1, len(universe)),
            }

    # Only rewrite the coverage document when every .po was inspected; a partial
    # run (one --po argument) would otherwise drop the languages it did not see.
    if args.po and rc == 0:
        doc = {
            "_comment": "Generated by tools/i18n-extract.py. Read by the README "
                        "coverage badges; regenerate with `make i18n-pot`.",
            "total": len(universe),
            "languages": dict(sorted(coverage.items())),
        }
        text = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
        if args.check:
            current = COVERAGE.read_text() if COVERAGE.exists() else ""
            if current != text:
                print("FAIL: i18n/coverage.json is stale; run `make i18n-pot` "
                      "and commit it", file=sys.stderr)
                rc = 1
            else:
                print("i18n/coverage.json is current")
        else:
            COVERAGE.write_text(text)
            print(f"wrote {COVERAGE.relative_to(ROOT)}")

    return rc


if __name__ == "__main__":
    sys.exit(main())
