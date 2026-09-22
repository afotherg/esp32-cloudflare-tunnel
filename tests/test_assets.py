import gzip
from pathlib import Path
import re
import runpy
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AssetTests(unittest.TestCase):
    def test_gzip_matches_original_and_is_deterministic(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'web').mkdir()
            (root/'src').mkdir()
            original = (ROOT/'web/dashboard.html').read_bytes()
            (root/'web/dashboard.html').write_bytes(original)
            def generate():
                runpy.run_path(str(ROOT/'tools/embed_dashboard.py'),
                               init_globals={'Import': lambda _: None, 'env': {'PROJECT_DIR': folder}})
                return (root/'src/dashboard_asset.hpp').read_text()
            output = generate()
            data = bytes(map(int, re.search(r'DASHBOARD_GZIP\[\] = \{(.*?)\}', output).group(1).split(',')))
            self.assertEqual(gzip.decompress(data), original)
            self.assertLess(len(data), len(original) // 2)
            self.assertEqual(generate(), output)
