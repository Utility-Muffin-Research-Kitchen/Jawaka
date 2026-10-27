#!/usr/bin/env python3
"""Regression coverage for extraction boundaries, independent of real sources."""
from pathlib import Path
import runpy
import tempfile
import unittest

extract = runpy.run_path(str(Path(__file__).with_name("i18n-extract.py")))['extract']


class ExtractionTest(unittest.TestCase):
    def test_only_translatable_messages_are_collected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
            for name, source in sources.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding='utf-8')
            previous = extract.__globals__['ROOT']
            try:
                extract.__globals__['ROOT'] = root
                self.assertEqual(set(extract()), {
                    'fixed daemon reason', 'fixed delete reason',
                    'audio output route failed', 'optional content art metadata ignored',
                    'ScreenScraper returned HTTP %d', '%d game', '%d games',
                })
            finally:
                extract.__globals__['ROOT'] = previous


if __name__ == '__main__':
    unittest.main()
