import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).parents[1] / "tools" / "download_occ_ess.py"
SPEC = importlib.util.spec_from_file_location("download_occ_ess", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


REPORT = (
    b"1THE OPTIONS CLEARING CORPORATION\r\n"
    b" NON-STANDARD SETTLEMENTS MRD REPORT ACTIVITY DATE 01/02/26\r\n"
    b"0706  ACET1 OSTK USD EU 01 01 ACET 6 CNS 100 0.0 20260102 00000000\r\n"
)


class DownloadOccEssTest(unittest.TestCase):
    def test_dates_are_validated_deduplicated_and_sorted(self):
        self.assertEqual(
            MODULE.parse_dates(["2026-01-06,2026-01-02", "2026-01-06"]),
            ["2026-01-02", "2026-01-06"],
        )
        with self.assertRaises(ValueError):
            MODULE.parse_dates(["01/02/2026"])

    def test_publish_is_atomic_and_date_scoped(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            target, digest = MODULE.publish_report(out, "2026-01-02", REPORT)
            self.assertEqual(target.read_bytes(), REPORT)
            self.assertEqual(len(digest), 64)
            self.assertFalse(target.with_suffix(".txt.pending").exists())
            with self.assertRaises(ValueError):
                MODULE.publish_report(out, "2026-01-05", REPORT)


if __name__ == "__main__":
    unittest.main()
