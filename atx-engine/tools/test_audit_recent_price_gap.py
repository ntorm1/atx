"""Bounded synthetic checks after implementation; no real archive access."""
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest

import pyarrow as pa
import pyarrow.parquet as pq

import audit_recent_price_gap as tool


def fixture(path, statistics=True):
    # Groups separately prove ID pruning, date pruning, and retained duplicates.
    dates = [dt.date(2020, 1, 2)] * 2 + [dt.date(2020, 1, 3)] * 2 + [dt.date(2021, 1, 4)] * 2
    table = pa.table({
        "tradingDate": pa.array(dates, type=pa.date32()),
        "securityID": pa.array([99, 99, 7, 7, 7, 7], type=pa.int64()),
        "ticker_tk": ["OTHER", "OTHER", "OLD.A", "OLD.A", "FUTURE", "FUTURE"],
        "close": pa.array([1, 1, 84.9, 84.9, 999, 999], type=pa.float32()),
        "volume": pa.array([1, 1, 100, None, 999, 999], type=pa.float64()),
        "cumulReturnFactor": pa.array([1, 1, 2, 2, 999, 999], type=pa.float64()),
        "returnFactor": pa.array([1, 1, -0.0, float("nan"), 999, 999], type=pa.float64()),
        "totalReturn": pa.array([1, 1, float("inf"), -1, 999, 999], type=pa.float64()),
    })
    pq.write_table(table, path, row_group_size=2, write_statistics=statistics)
    return hashlib.sha256(path.read_bytes()).hexdigest()


class RecentPriceGapAudit(unittest.TestCase):
    def test_exact_fields_original_duplicates_bits_and_pruned_groups(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"; out = base / "audit.json"
            pin = fixture(source)
            receipt = tool.audit(source, out, pin, 7, "2020-01-01", "2020-01-10", 10)
            result = json.loads(out.read_bytes())
            self.assertEqual(receipt["sha256"], hashlib.sha256(out.read_bytes()).hexdigest())
            self.assertEqual(receipt["row_groups_decoded"], 1)
            self.assertEqual([g["pruned_by"] for g in result["row_groups"]],
                             ["securityID-footer-range", None, "date-footer-range"])
            self.assertEqual(len(result["records"]), 2)
            a, b = result["records"]
            self.assertEqual((a["row_group"], a["row_in_group"], b["row_in_group"]), (1, 0, 1))
            self.assertEqual(a["values"]["ticker_tk"], "OLD.A")
            self.assertEqual(a["ieee754_le_hex"]["close"], struct.pack("<f", 84.9).hex())
            self.assertEqual(a["ieee754_le_hex"]["returnFactor"], struct.pack("<d", -0.0).hex())
            self.assertEqual(a["values"]["totalReturn"], {"nonfinite": "+inf"})
            self.assertEqual(b["values"]["returnFactor"], {"nonfinite": "nan"})
            self.assertIsNone(b["values"]["volume"])
            self.assertNotIn("volume", b["ieee754_le_hex"])
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), pin)
            self.assertEqual(sorted(p.name for p in base.iterdir()), ["audit.json", "source.parquet"])

    def test_missing_statistics_decodes_bounded_projection_without_changing_records(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"; out = base / "audit.json"
            pin = fixture(source, statistics=False)
            receipt = tool.audit(source, out, pin, 7, "2020-01-03", "2020-01-04", 10)
            result = json.loads(out.read_bytes())
            self.assertEqual(receipt["row_groups_decoded"], 3)
            self.assertEqual(len(result["records"]), 2)
            self.assertTrue(all(r["values"]["tradingDate"] == "2020-01-03" for r in result["records"]))
            self.assertTrue(all(not g["id_stats_available"] for g in result["row_groups"]))

    def test_bad_pin_budget_and_existing_output_refuse_without_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"; out = base / "audit.json"
            pin = fixture(source)
            with self.assertRaisesRegex(ValueError, "external pin"):
                tool.audit(source, out, "0" * 64, 7, "2020-01-01", "2020-01-10", 10)
            self.assertFalse(out.exists())
            with self.assertRaisesRegex(ValueError, "no truncation"):
                tool.audit(source, out, pin, 7, "2020-01-01", "2020-01-10", 10, max_records=1)
            self.assertFalse(out.exists())
            out.write_bytes(b"existing-authority")
            with self.assertRaisesRegex(ValueError, "new file"):
                tool.audit(source, out, pin, 7, "2020-01-01", "2020-01-10", 10)
            self.assertEqual(out.read_bytes(), b"existing-authority")

    def test_empty_window_is_evidence_not_invented_terminal_value_and_deadline_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"; out = base / "audit.json"
            pin = fixture(source)
            tool.audit(source, out, pin, 7, "2020-01-06", "2020-01-11", 10)
            result = json.loads(out.read_bytes())
            self.assertEqual(result["records"], [])
            self.assertNotIn("terminal_return", result)
            for seconds in [math.nan, math.inf, 0, -1, 121]:
                with self.assertRaises(ValueError):
                    tool.Deadline(seconds)


if __name__ == "__main__":
    unittest.main()
