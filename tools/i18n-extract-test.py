#!/usr/bin/env python3
"""Regression coverage for extraction boundaries, independent of real sources."""
from pathlib import Path
import runpy
import tempfile
import unittest

extract = runpy.run_path(str(Path(__file__).with_name("i18n-extract.py")))['extract']


def extract_from(sources):
    """The keys extract() finds in `sources`, a {relative path: C text} tree."""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for name, source in sources.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding='utf-8')
        previous = extract.__globals__['ROOT']
        try:
            extract.__globals__['ROOT'] = root
            return set(extract())
        finally:
            extract.__globals__['ROOT'] = previous


class ExtractionTest(unittest.TestCase):
    def test_only_translatable_messages_are_collected(self):
        sources = {
            'internal/launcher/sample_test.c':
                'jw__launch_notice(state, "fixture notice"); '
                'T("fixture translation"); JW_UI("fixture marker");',
            'cmd/jawakad/main.c':
                'jw__reply_error(client, "fixed daemon reason"); '
                'jw__reply_error(client, "formatted daemon reason: %s", reason);',
            'internal/discovery/delete.c':
                'jw__error(out, n, "fixed delete reason"); '
                'jw__error(out, n, "cannot delete %.160s: %s", path, reason);',
            'internal/platform/device_mlp1.c':
                'JW_UI("audio output route failed"); '
                'JW_UI("%s output unavailable"); JW_UI("audio output: %s"); '
                'JW_UI("switching to %d Hz"); JW_UI("Color temperature %d K"); '
                'JW_UI("HDMI output %s");',
            'internal/catalog/merge.c':
                'JW_UI("%s %s claims %s already owned by the release catalog"); '
                'JW_UI("%s %s claims %s=%s, also claimed by %s; both refused"); '
                'JW_UI("system %s needs core %s, which is not available after merge; " '
                '"the system is refused too rather than producing a tile that cannot launch"); '
                'JW_UI("system_extensions names %s, which no system provides after merge; extension dropped"); '
                'JW_UI("system_extensions for %s names core %s, absent after merge; " '
                '"that name dropped, extension accepted");',
            'internal/discovery/discovery.c':
                'JW_UI("content contribution refused during manifest validation (%s)"); '
                'JW_UI("optional content art metadata ignored");',
            'internal/scrape/ss_client.c':
                'jw__ss_set_error("ScreenScraper returned HTTP %d", code);',
            'cmd/jawaka-launcher/main.c':
                'snprintf(out, n, count == 1 ? T("%d game") : T("%d games"), count);',
        }
        self.assertEqual(extract_from(sources), {
            'fixed daemon reason', 'fixed delete reason',
            'audio output route failed', 'optional content art metadata ignored',
            'ScreenScraper returned HTTP %d', '%d game', '%d games',
        })

    def test_c_escapes_become_the_bytes_the_compiler_makes(self):
        # The runtime looks up the compiled bytes, so "\xe2\x80\x94" must
        # reach leaf.pot as the em dash, and a hex escape ends at its quote.
        sources = {
            'cmd/jawaka-launcher/main.c':
                'T("Quicksave \\xe2\\x80\\x94 A resumes"); '
                'T("Dot \\302\\267 octal"); '
                'T("\\xe2\\x80\\x94" "abc"); '
                'T("Line\\nbreak \\"quoted\\"");',
        }
        self.assertEqual(extract_from(sources), {
            'Quicksave \u2014 A resumes', 'Dot \u00b7 octal', '\u2014abc',
            'Line\nbreak "quoted"',
        })


if __name__ == '__main__':
    unittest.main()
