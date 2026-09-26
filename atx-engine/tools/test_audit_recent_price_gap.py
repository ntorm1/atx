"""Bounded synthetic checks after implementation; no real archive access."""
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

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

    def test_batch_counts_zero_matches_and_exact_single_id_record_parity(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"
            pin = fixture(source)
            batch_out, single_out = base / "batch.json", base / "single.json"
            with patch.object(tool.hashlib, "sha256", wraps=hashlib.sha256) as hashes:
                receipt = tool.audit(source, batch_out, pin, None, "2020-01-01", "2020-01-10", 10,
                                     instrument_ids=[777, 99, 7])
                # The streaming whole-source digest is initialized once; the
                # footer/record/output digests receive already bounded bytes.
                self.assertEqual(sum(not c.args and not c.kwargs for c in hashes.call_args_list), 1)
            batch = json.loads(batch_out.read_bytes())
            self.assertEqual(batch["schema"], "atx.recent-price-gap-batch-audit/v1")
            self.assertEqual(batch["instrument_ids"], [7, 99, 777])
            self.assertNotIn("instrument_id", batch)
            self.assertEqual(batch["record_counts_by_id"], {"7": 2, "99": 2, "777": 0})
            self.assertEqual(receipt["record_counts_by_id"], batch["record_counts_by_id"])
            tool.audit(source, single_out, pin, 7, "2020-01-01", "2020-01-10", 10)
            single = json.loads(single_out.read_bytes())
            self.assertEqual(single["schema"], "atx.recent-price-gap-audit/v1")
            self.assertEqual(single["instrument_id"], 7)
            self.assertEqual(single["records"], [r for r in batch["records"] if r["values"]["securityID"] == 7])

    def test_batch_id_admission_and_global_record_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"; out = base / "audit.json"
            pin = fixture(source)
            for ids in ([], list(range(1, 66)), [7, 7], [0], [-1], [1 << 63], [True]):
                with self.assertRaises(ValueError):
                    tool.audit(source, out, pin, None, "2020-01-01", "2020-01-10", 10,
                               instrument_ids=ids)
            with self.assertRaises(ValueError):
                tool.audit(source, out, pin, 7, "2020-01-01", "2020-01-10", 10, instrument_ids=[7])
            with self.assertRaisesRegex(ValueError, "no truncation"):
                tool.audit(source, out, pin, None, "2020-01-01", "2020-01-10", 10,
                           max_records=3, instrument_ids=[7, 99])
            self.assertFalse(out.exists())

    def test_exact_tickers_keep_id_record_bytes_case_and_zero_match_counts(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"
            pin = fixture(source)
            out, id_out = base / "tickers.json", base / "id.json"
            with patch.object(tool.hashlib, "sha256", wraps=hashlib.sha256) as hashes:
                receipt = tool.audit(source, out, pin, None, "2020-01-01", "2020-01-10", 10,
                                     tickers=["old.a", "OLD.A", "NONE"])
                self.assertEqual(sum(not c.args and not c.kwargs for c in hashes.call_args_list), 1)
            result = json.loads(out.read_bytes())
            self.assertEqual(result["schema"], "atx.recent-price-gap-ticker-audit/v1")
            self.assertEqual(result["historical_tickers"], ["NONE", "OLD.A", "old.a"])
            self.assertEqual(result["record_counts_by_ticker"], {"NONE": 0, "OLD.A": 2, "old.a": 0})
            self.assertEqual(receipt["record_counts_by_ticker"], result["record_counts_by_ticker"])
            self.assertNotIn("instrument_id", result)
            self.assertNotIn("record_counts_by_id", result)
            self.assertEqual([g["pruned_by"] for g in result["row_groups"]],
                             ["ticker_tk-footer-range", None, "date-footer-range"])
            tool.audit(source, id_out, pin, 7, "2020-01-01", "2020-01-10", 10)
            self.assertEqual(result["records"], json.loads(id_out.read_bytes())["records"])

    def test_ticker_reuse_does_not_infer_identity_or_normalize_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"
            fixture(source)
            original = pq.read_table(source)
            # The same exact text occurs on two distinct source IDs. Neither
            # case variants nor suffixes are aliases; later dates stay out.
            table = original.set_column(2, "ticker_tk", pa.array(["PE", "PE", "PE", "PE.A", "PE", "pe"]))
            results = []
            for stats in (True, False):
                pq.write_table(table, source, row_group_size=2, write_statistics=stats)
                pin = hashlib.sha256(source.read_bytes()).hexdigest()
                out = base / f"ticker-{stats}.json"
                receipt = tool.audit(source, out, pin, None, "2020-01-01", "2020-01-10", 10, tickers=["PE"])
                result = json.loads(out.read_bytes()); results.append(result["records"])
                self.assertEqual(receipt["record_counts_by_ticker"], {"PE": 3})
                self.assertEqual([r["values"]["securityID"] for r in result["records"]], [99, 99, 7])
                self.assertTrue(all(r["values"]["ticker_tk"] == "PE" for r in result["records"]))
                if not stats:
                    self.assertEqual(receipt["row_groups_decoded"], 3)
                    self.assertTrue(all(not g["ticker_stats_available"] for g in result["row_groups"]))
            self.assertEqual(results[0], results[1])

    def test_ticker_selector_bounds_mutual_exclusion_and_global_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base / "source.parquet"; out = base / "audit.json"
            pin = fixture(source)
            for tickers in ([], [str(i) for i in range(65)], ["PE", "PE"], [""], [1],
                            ["x" * 129], ["PE\n"], ["PE\0"]):
                with self.assertRaises(ValueError):
                    tool.audit(source, out, pin, None, "2020-01-01", "2020-01-10", 10, tickers=tickers)
            for kwargs in ({"instrument_id": 7}, {"instrument_id": None, "instrument_ids": [7]}):
                with self.assertRaises(ValueError):
                    tool.audit(source, out, pin, start="2020-01-01", end="2020-01-10", tickers=["OLD.A"], **kwargs)
            with self.assertRaisesRegex(ValueError, "no truncation"):
                tool.audit(source, out, pin, None, "2020-01-01", "2020-01-10", 10,
                           tickers=["OLD.A"], max_records=1)
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
