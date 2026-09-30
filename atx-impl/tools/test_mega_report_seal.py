"""mega_report seal check (platform v8 E-4, OD-5): date-shaped path parts are refused, hash-named parts are not, and
paths are taken relative to the research output root. Synthetic files only.

Run: python -m pytest atx-impl/tools/test_mega_report_seal.py -q
"""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_tools import research_window as rw  # noqa: E402
from mega_report import data as D  # noqa: E402

READ = ("fp_2e2025f0aa11bb22/droe.f64", "train-2020-2023-lo1/daily.csv", "ic1_" + "2024" * 4 + "/x.json",
        "build-equity/mega-candidate-cache-v71/3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809/a.json")
REFUSED = ("nav-2023-2024/x.csv", "x_20250131.csv", "nav-2026/y.csv", "VAL/z.csv",
           "build-equity/recent-fast-validation-2023-2024-v1/manifest.json", "holdout/a.csv")


class SealRegex(unittest.TestCase):
    def test_first_sealed_year_is_the_research_windows(self):
        self.assertEqual(D.FIRST_SEALED_YEAR, rw.FIRST_SEALED_YEAR)
        self.assertEqual(D.FIRST_SEALED_YEAR, int(rw.SEAL_DATE[:4]))

    def test_seal_regex_hex_vs_date(self):
        for path in READ:
            self.assertFalse(D.path_is_sealed(path), path)
        for path in REFUSED:
            self.assertTrue(D.path_is_sealed(path), path)
        # the year threshold is the window's: one year earlier seals the 2023 part too
        self.assertTrue(D.path_is_sealed("train-2020-2023-lo1/daily.csv", D.FIRST_SEALED_YEAR - 1))
        self.assertFalse(D.path_is_sealed("nav-2023/x.csv"))
        self.assertTrue(D.path_is_sealed(f"nav-{D.FIRST_SEALED_YEAR}/x.csv"))
        self.assertTrue(D.path_is_sealed("a\\nav-2026\\y.csv"))   # Windows separators split too


class SealRegistry(unittest.TestCase):
    def test_seal_paths_are_relative_to_out_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "platform-v8-20260929" / "research-out"   # a run-date-stamped sprint folder
            files = {rel: rel.encode() for rel in READ + REFUSED}
            for rel, data in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(data)
            self.assertTrue(D.path_is_sealed(root.as_posix()))   # the absolute path would be refused
            reg = D.Registry(root)
            for rel in READ:
                self.assertEqual(reg.read_bytes(rel), files[rel], rel)
                self.assertEqual(reg.files[rel]["status"], "read")
            for rel in REFUSED:
                self.assertIsNone(reg.read_bytes(rel), rel)
                self.assertEqual(reg.files[rel]["status"], "refused (sealed)")
                self.assertNotIn("sha256", reg.files[rel])   # never opened, never hashed
            self.assertEqual(reg.read_bytes(root / READ[0]), files[READ[0]])   # absolute under the root: relative key


if __name__ == "__main__":
    unittest.main()
