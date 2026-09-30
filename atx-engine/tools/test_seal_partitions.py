"""A partition that begins on or after the research seal is never opened (platform v8 W0-1).

Readers covered: the holdings year readers (FTD, Reg SHO threshold, short volume ext: ``Stage.years``), the 13F
``parts/source=YYYYqN`` data sets, the FSDS SUB quarters of build_fundamental_events, and the research_window helpers
they share. The Form 4 quarters are covered in test_prepare_research_fields_sec.py
(``test_insider_sealed_quarter_present_on_disk_is_never_opened``). Every date is relative to ``research_window.SEAL``,
so the checks hold under the window conftest.py binds and under the repository window alike. Synthetic data only.
"""
import datetime as dt
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import pyarrow as pa

import build_fundamental_events as bfe
import research_fields_holdings as hold
import research_window as rw
import test_research_fields_holdings as hfx

YEAR_READERS = (("ftd", "year={}/ftd.parquet"), ("regsho_threshold", "year={}/threshold.parquet"),
                ("short_volume_ext", "year={}/short_volume_ext.parquet"))


class WindowPartitionHelpers(unittest.TestCase):
    def test_partition_is_sealed_from_the_seal_on(self):
        seal = rw.SEAL
        self.assertEqual((seal.month, seal.day), (1, 1))   # both windows seal on a year start
        self.assertFalse(rw.partition_is_sealed(seal.year - 1))
        self.assertTrue(rw.partition_is_sealed(seal.year))
        self.assertTrue(rw.partition_is_sealed(seal.year + 1))
        self.assertFalse(rw.partition_is_sealed(seal.year - 1, 4))
        self.assertTrue(rw.partition_is_sealed(seal.year, 1))
        with self.assertRaises(ValueError):
            rw.partition_is_sealed(seal.year, 5)

    def test_last_quarter_before_seal_ends_before_it(self):
        year, quarter = rw.last_quarter_before_seal()
        nxt = (year, quarter + 1) if quarter < 4 else (year + 1, 1)
        self.assertLess(rw.period_begin(*nxt) - dt.timedelta(days=1), rw.SEAL)   # its last day is before the seal
        self.assertEqual(rw.period_begin(*nxt), rw.SEAL)                            # a year-start seal: the next
        self.assertTrue(rw.partition_is_sealed(*nxt))                               # quarter is sealed
        self.assertEqual(bfe.LAST_SUB_QUARTER, f"{year}q{quarter}")


class HoldingsYearReaders(unittest.TestCase):
    def test_a_sealed_year_present_on_disk_is_never_listed_or_opened(self):
        open_year, sealed_year = rw.SEAL.year - 1, rw.SEAL.year
        table = pa.table({"x": pa.array([1], pa.int64())})
        for key, pattern in YEAR_READERS:
            with self.subTest(key), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / key
                sha = hfx.write_stage(root, key, {pattern.format(open_year): table, pattern.format(sealed_year): table})
                self.assertTrue((root / pattern.format(sealed_year)).is_file())
                stage = hold.Stage(key, root, sha)
                files = stage.years(pattern, open_year - 1, sealed_year + 1)
                self.assertEqual(files, [pattern.format(open_year)])
                for rel in files:
                    stage.table(rel, ["x"])
                self.assertEqual([Path(s["path"]).parent.name for s in stage.read], [f"year={open_year}"])


class ThirteenFParts(unittest.TestCase):
    def test_source_part_dated_at_or_after_the_seal_is_sealed(self):
        y = rw.SEAL.year
        self.assertFalse(hold.source_part_is_sealed(f"{y - 1}q4"))
        self.assertTrue(hold.source_part_is_sealed(f"{y}q1"))
        self.assertTrue(hold.source_part_is_sealed(f"{y + 1}q3"))
        self.assertFalse(hold.source_part_is_sealed("legacy-undated"))   # not dated: not refused here


class SubQuarters(unittest.TestCase):
    def test_no_sub_quarter_after_the_last_open_one_is_listed(self):
        year, quarter = rw.last_quarter_before_seal()
        with tempfile.TemporaryDirectory() as tmp:
            fsds = Path(tmp)
            quarters = {}
            for q in (f"{year - 1}q4", f"{year}q{quarter}", f"{rw.SEAL.year}q1", f"{rw.SEAL.year}q2"):
                path = fsds / "sub" / f"{q}.parquet"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"not a parquet file: opening it would fail")
                quarters[q] = {"tables": {"sub": {"path": f"sub/{q}.parquet", "parquet_sha256": "0" * 64}}}
            manifest = fsds / "fsds-staging-manifest.json"
            manifest.write_text(json.dumps({"quarters": quarters}), encoding="utf-8")
            _, listed = bfe.sub_quarters(fsds, hashlib.sha256(manifest.read_bytes()).hexdigest())
        self.assertEqual([x["quarter"] for x in listed], sorted({f"{year - 1}q4", f"{year}q{quarter}"}))


if __name__ == "__main__":
    unittest.main()
