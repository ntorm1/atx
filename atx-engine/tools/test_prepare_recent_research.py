"""Small synthetic postimplementation checks; no real archive/warehouse access."""
import contextlib
import datetime as dt
import io
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_recent_research as tool


def fixture(path, future=False):
    days = np.busday_offset(np.datetime64("2018-06-01"), np.arange(445)).astype("datetime64[D]")
    date = np.repeat(days, 6)
    ids = np.tile(np.arange(1, 7, dtype=np.int64), len(days))
    raw = np.tile(np.array([100, 90, 80, 70, 60, 50], dtype=np.float32), len(days))
    volume = np.full(len(date), 100000.0)
    # Entire invalid session must remain in the source calendar.
    raw[10 * 6:11 * 6] = np.nan
    keep = ~((date == days[400]) & (ids == 1))
    date, ids, raw, volume = date[keep], ids[keep], raw[keep], volume[keep]
    if future:
        raw[(date >= days[420]) & (ids == 6)] = 50000
    # One positive duplicate key: both numerical rows must be quarantined.
    duplicate = int(np.flatnonzero((date == days[50]) & (ids == 2))[0])
    date = np.concatenate((date, date[[duplicate]], np.array(["2025-02-01"], dtype="datetime64[D]")))
    ids = np.concatenate((ids, ids[[duplicate]], [1]))
    raw = np.concatenate((raw, raw[[duplicate]], [np.nan])).astype(np.float32)
    volume = np.concatenate((volume, volume[[duplicate]], [-1.0]))
    # Source order deliberately not chronological; projection owns sorting.
    order = np.arange(len(date))[::-1]
    table = pa.table({"tradingDate": pa.array(date[order], type=pa.date32()),
        "securityID": ids[order], "close": raw[order], "volume": volume[order],
        "cumulReturnFactor": np.full(len(date), 2.0)})
    pq.write_table(table, path, row_group_size=1000)
    return [str(x) for x in days]


class RecentResearch(unittest.TestCase):
    def test_nonfinite_or_unbounded_deadline_is_refused(self):
        for seconds in (float("nan"), float("inf"), 0, -1, 601):
            with self.assertRaises(ValueError):
                tool.Limits(seconds)

    def test_projection_filters_before_qa_preserves_calendar_and_quarantines_duplicates(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            base = Path(temp); source = base / "source.parquet"; days = fixture(source)
            m = tool.prepare_cache(source, base / "cache", days[0], "2021-01-01", tool.Limits(30))
            self.assertEqual(m["selected_rows"], 445 * 6)  # one missing + one duplicate
            self.assertEqual(m["rejected_rows"], 8)  # six bad prices + both duplicate rows
            self.assertEqual(len(m["session_days"]), 445)
            values = list(tool.calendar_rows(base / "cache/accepted.parquet", m["session_days"], tool.Limits(30)))
            self.assertEqual(len(values[10][1][0]), 0)
            self.assertNotIn(2, values[50][1][0])
            self.assertEqual(float(values[0][1][3][0]), 200.0)

    def test_lagged_membership_independent_presence_full_warmup_union_and_future_prefix(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            base = Path(temp); source = base / "source.parquet"; days = fixture(source)
            tool.prepare_cache(source, base / "cache", days[0], "2021-01-01", tool.Limits(30))
            m = tool.create_role(base / "cache", base / "role", days[0], days[400], "2021-01-01", tool.Limits(30), top_n=2)
            ids = np.fromfile(base / "role/ids.u64", dtype="<u8")
            member = np.fromfile(base / "role/member.u8", dtype="u1").reshape(m["dates"], -1)
            present = np.fromfile(base / "role/present.u8", dtype="u1").reshape(member.shape)
            raw = np.fromfile(base / "role/raw_close.f64", dtype="<f8").reshape(member.shape)
            self.assertFalse(member[:63].any())
            one = int(np.flatnonzero(ids == 1)[0])
            self.assertEqual(member[400, one], 1)
            self.assertEqual(present[400, one], 0)
            self.assertTrue(np.isnan(raw[400, one]))
            self.assertEqual(member[401, one], 0)
            self.assertTrue(np.all(member.sum(axis=1) <= 2))
            self.assertGreaterEqual(m["score_begin"], 383)
            # Changed future eligibility may enlarge the stable union, but cannot
            # alter any earlier historical rank population/member decision.
            other = base / "other.parquet"; fixture(other, True)
            tool.prepare_cache(other, base / "cache2", days[0], "2021-01-01", tool.Limits(30))
            m2 = tool.create_role(base / "cache2", base / "role2", days[0], days[400], "2021-01-01", tool.Limits(30), top_n=2)
            ids2 = np.fromfile(base / "role2/ids.u64", dtype="<u8")
            member2 = np.fromfile(base / "role2/member.u8", dtype="u1").reshape(m2["dates"], -1)
            for t in range(421):
                self.assertEqual(set(ids[member[t] != 0]), set(ids2[member2[t] != 0]))

    def test_unpublished_mutated_cache_budget_and_warmup_refuse(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            base = Path(temp); source = base / "source.parquet"; days = fixture(source)
            tool.prepare_cache(source, base / "cache", days[0], "2021-01-01", tool.Limits(30))
            with self.assertRaisesRegex(ValueError, "warmup"):
                tool.create_role(base / "cache", base / "short", days[0], days[380], "2021-01-01", tool.Limits(30), top_n=2)
            with self.assertRaisesRegex(ValueError, "invalid role budget"):
                tool.create_role(base / "cache", base / "budget", days[0], days[400], "2021-01-01", tool.Limits(30), top_n=2, max_output_bytes=16)
            with (base / "cache/accepted.parquet").open("ab") as f:
                f.write(b"x")
            with self.assertRaisesRegex(ValueError, "immutable receipt"):
                tool.cache_receipt(base / "cache", tool.Limits(30))


if __name__ == "__main__":
    unittest.main()
