#!/usr/bin/env python3
"""Export a .po to the device's live-override TSV.

The TSV is the review vehicle: it ignores fuzzy marks on purpose, so a seeded
translation can be seen on the panel before anyone has confirmed it. The
compiled table (i18n-compile.py) is the opposite -- fuzzy never ships. Both
read the same .po, which stays the single source of truth.

    tools/i18n-po2tsv.py i18n/zh_CN.po -o /tmp/zh_CN.tsv
"""

import argparse
import re
import sys


def unesc(s: str) -> str:
    return (s.replace(r"\n", "\n").replace(r"\t", "\t")
             .replace(r"\"", '"').replace("\\\\", "\\"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("po")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    text = open(args.po, encoding="utf-8").read()
    lines = ["# Generated from " + args.po + " by tools/i18n-po2tsv.py.",
             "# Live-override review file: includes FUZZY entries on purpose."]
    n = 0
    for m in re.finditer(
            r'msgid "((?:[^"\\]|\\.)+)"\nmsgstr "((?:[^"\\]|\\.)*)"', text):
        key, val = unesc(m.group(1)), unesc(m.group(2))
        if not key or not val:
            continue
        if "\t" in key or "\n" in key or "\t" in val or "\n" in val:
            print(f"skipping (embedded tab/newline): {key!r}", file=sys.stderr)
            continue
        lines.append(f"{key}\t{val}")
        n += 1
    open(args.output, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print(f"{args.output}: {n} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
