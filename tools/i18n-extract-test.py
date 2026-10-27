#!/usr/bin/env python3
"""Regression coverage for extraction boundaries, independent of real sources."""
from pathlib import Path
import runpy
import tempfile
import unittest

extract = runpy.run_path(str(Path(__file__).with_name("i18n-extract.py")))['extract']


class ExtractionTest(unittest.TestCase):
    def test_only_displayable_messages_are_collected(self):
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
                    'ScreenScraper returned HTTP %d', '%d game', '%d games',
                })
            finally:
                extract.__globals__['ROOT'] = previous


if __name__ == '__main__':
    unittest.main()
