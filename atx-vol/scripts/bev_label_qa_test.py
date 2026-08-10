"""Tests for bev_label_qa.py (atx-vol Task 5: label-corpus QA report).

Pure stdlib (unittest), self-contained tmp-dir fixtures (synthetic 22-column
TSVs written by this file, mirroring bev_label_factory.cpp's append_rows_tsv
layout exactly) -- no network, no real corpus/driver dependency.

Run: python -m pytest atx-vol/scripts/bev_label_qa_test.py -q
"""

from __future__ import annotations

import json
import math
import statistics
import tempfile
import unittest
from pathlib import Path

from bev_label_qa import (
    FEATURE_COLUMNS,
    build_report,
    compute_feature_coverage,
    compute_leakage,
    compute_row_accounting,
    compute_target_distribution,
    delta_band,
    find_duplicates,
    load_rows,
    main,
    mean_std,
    parse_tsv_file,
    pearson,
    percentile,
    tenor_band,
)

# The exact 22-column header bev_label_factory.cpp's append_rows_tsv() emits.
_HEADER = (
    "entry_ts_ns\tuid\tstrike\texpiry_ns\tside\tsigma_be\tsigma_entry_iv\tlog_ratio\t"
    "premium\tvega\tn_days\titers\tflag\tsnapped\t"
    "log_moneyness\ttenor_years\tmarket_vol\trv_21d\trv_63d\tiv_minus_rv\t"
    "n_events_to_expiry\tdelta_abs"
)


def _row(
    entry_ts_ns: int,
    uid: str,
    strike: float,
    expiry_ns: int,
    side: str,
    sigma_be: float,
    sigma_entry_iv: float,
    log_ratio: float,
    n_days: int,
    iters: int,
    flag: int,
    snapped: int,
    log_moneyness: float,
    tenor_years: float,
    market_vol: float,
    rv_21d: float,
    rv_63d: float,
    iv_minus_rv: float,
    n_events_to_expiry: float,
    delta_abs: float,
) -> str:
    fields = [
        entry_ts_ns, uid, strike, expiry_ns, side, sigma_be, sigma_entry_iv, log_ratio,
        1.0, 0.05, n_days, iters, flag, snapped,
        log_moneyness, tenor_years, market_vol, rv_21d, rv_63d, iv_minus_rv,
        n_events_to_expiry, delta_abs,
    ]
    return "\t".join(str(f) for f in fields)


# -- Fixture rows (values chosen so bucket/coverage/duplicate math is
# hand-verifiable) --
#
# File A: 3 rows.
#   A1: flag=0, snapped=0, tenor=0.10 (band <=0.12), delta=0.20 (band <0.25)
#   A2: flag=1 (NoBracket, kept per the driver's own contract), snapped=1,
#       same tenor/delta bucket as A1
#   A3: flag=0, snapped=0, tenor=0.50 (band <=0.60), delta=0.60 (band >=0.5),
#       rv_21d/rv_63d/iv_minus_rv/n_events_to_expiry all NaN (short history /
#       no calendar)
_ROW_A1 = _row(1_000_000_000, "SPY", 100.0, 2_000_000_000, "C",
               0.20, 0.18, math.log(0.20 / 0.18), 30, 5, 0, 0,
               0.01, 0.10, 0.18, 0.15, 0.16, 0.18 - 0.15, 0.0, 0.20)
_ROW_A2 = _row(1_000_000_001, "SPY", 105.0, 2_000_000_000, "P",
               0.25, 0.22, math.log(0.25 / 0.22), 30, 7, 1, 1,
               0.02, 0.10, 0.22, math.nan, math.nan, math.nan, math.nan, 0.20)
_ROW_A3 = _row(1_000_000_002, "SPY", 110.0, 2_000_000_000, "C",
               0.30, 0.28, math.log(0.30 / 0.28), 45, 3, 0, 0,
               0.03, 0.50, 0.28, math.nan, math.nan, math.nan, math.nan, 0.60)

# File B: 2 rows.
#   B1: deliberate cross-file duplicate of A1's key (entry_ts_ns, uid,
#       expiry_ns, strike, side) -- different numeric payload, same key.
#   B2: unique key, same bucket as A1/A2 (tenor=0.05 <=0.12, delta=0.10 <0.25)
_ROW_B1 = _row(1_000_000_000, "SPY", 100.0, 2_000_000_000, "C",
               0.21, 0.19, math.log(0.21 / 0.19), 30, 5, 0, 0,
               0.01, 0.10, 0.19, 0.15, 0.16, 0.19 - 0.15, 0.0, 0.20)
_ROW_B2 = _row(1_000_000_003, "SPY", 95.0, 2_000_000_000, "P",
               0.19, 0.20, math.log(0.19 / 0.20), 30, 2, 0, 1,
               -0.01, 0.05, 0.20, 0.14, 0.15, 0.20 - 0.14, 1.0, 0.10)

_META = "# tool=bev_label_factory\n# feature_schema=1\n"


def _write_file(tmp: Path, name: str, rows: list[str]) -> Path:
    path = tmp / name
    path.write_text(_META + _HEADER + "\n" + "\n".join(rows) + "\n", encoding="utf-8")
    return path


class _TmpDirCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmpdir_obj = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir_obj.name)

    def tearDown(self) -> None:
        self._tmpdir_obj.cleanup()


class ParseTsvFileTest(_TmpDirCase):
    def test_meta_lines_skipped_and_nan_strings_parse(self) -> None:
        path = _write_file(self.tmp, "a.tsv", [_ROW_A2])
        header, rows = parse_tsv_file(path)

        self.assertEqual(header, _HEADER.split("\t"))
        self.assertEqual(len(rows), 1)
        self.assertTrue(math.isnan(float(rows[0]["rv_21d"])))
        self.assertTrue(math.isnan(float(rows[0]["rv_63d"])))
        self.assertTrue(math.isnan(float(rows[0]["iv_minus_rv"])))
        self.assertTrue(math.isnan(float(rows[0]["n_events_to_expiry"])))

    def test_header_only_file_yields_zero_rows(self) -> None:
        path = _write_file(self.tmp, "empty.tsv", [])
        header, rows = parse_tsv_file(path)
        self.assertEqual(header, _HEADER.split("\t"))
        self.assertEqual(rows, [])


class StatsHelperTest(unittest.TestCase):
    def test_mean_std_matches_stdlib_population_stats(self) -> None:
        values = [0.1, 0.2, 0.35, -0.4, 1.2]
        mean, std = mean_std(values)
        self.assertAlmostEqual(mean, statistics.mean(values), places=12)
        self.assertAlmostEqual(std, statistics.pstdev(values), places=12)

    def test_mean_std_single_value_has_zero_stddev(self) -> None:
        mean, std = mean_std([5.0])
        self.assertEqual(mean, 5.0)
        self.assertEqual(std, 0.0)

    def test_mean_std_empty_is_nan(self) -> None:
        mean, std = mean_std([])
        self.assertTrue(math.isnan(mean))
        self.assertTrue(math.isnan(std))

    def test_percentile_linear_interpolation_matches_known_values(self) -> None:
        values = [1.0, 2.0, 3.0, 4.0]
        self.assertAlmostEqual(percentile(values, 50), 2.5, places=12)
        self.assertAlmostEqual(percentile(values, 0), 1.0, places=12)
        self.assertAlmostEqual(percentile(values, 100), 4.0, places=12)
        self.assertAlmostEqual(percentile(values, 25), 1.75, places=12)

    def test_percentile_single_value(self) -> None:
        self.assertEqual(percentile([7.0], 50), 7.0)

    def test_percentile_empty_is_nan(self) -> None:
        self.assertTrue(math.isnan(percentile([], 50)))

    def test_pearson_perfect_positive_correlation(self) -> None:
        xs = [1.0, 2.0, 3.0, 4.0]
        ys = [2.0, 4.0, 6.0, 8.0]
        self.assertAlmostEqual(pearson(xs, ys), 1.0, places=12)

    def test_pearson_perfect_negative_correlation(self) -> None:
        xs = [1.0, 2.0, 3.0, 4.0]
        ys = [8.0, 6.0, 4.0, 2.0]
        self.assertAlmostEqual(pearson(xs, ys), -1.0, places=12)

    def test_pearson_zero_variance_denominator_returns_none(self) -> None:
        # Constant column -> zero-variance Pearson denominator: must not
        # raise ZeroDivisionError, must report undefined (None), not 0.0.
        self.assertIsNone(pearson([1.0, 1.0, 1.0], [2.0, 3.0, 4.0]))

    def test_pearson_fewer_than_two_paired_points_returns_none(self) -> None:
        self.assertIsNone(pearson([1.0], [2.0]))
        self.assertIsNone(pearson([], []))

    def test_pearson_excludes_nan_pairs(self) -> None:
        xs = [1.0, 2.0, math.nan, 4.0]
        ys = [2.0, 4.0, 99.0, 8.0]
        self.assertAlmostEqual(pearson(xs, ys), 1.0, places=12)

    def test_tenor_band_boundaries(self) -> None:
        self.assertEqual(tenor_band(0.12), "tenor<=0.12")
        self.assertEqual(tenor_band(0.13), "tenor<=0.30")
        self.assertEqual(tenor_band(0.30), "tenor<=0.30")
        self.assertEqual(tenor_band(0.31), "tenor<=0.60")
        self.assertEqual(tenor_band(0.60), "tenor<=0.60")
        self.assertEqual(tenor_band(0.61), "tenor>0.60")
        self.assertIsNone(tenor_band(math.nan))

    def test_delta_band_boundaries(self) -> None:
        self.assertEqual(delta_band(0.10), "delta<0.25")
        self.assertEqual(delta_band(0.24999), "delta<0.25")
        self.assertEqual(delta_band(0.25), "delta[0.25,0.5)")
        self.assertEqual(delta_band(0.49999), "delta[0.25,0.5)")
        self.assertEqual(delta_band(0.50), "delta>=0.5")
        self.assertIsNone(delta_band(math.nan))


class RowAccountingTest(_TmpDirCase):
    def test_exact_flag_and_snapped_counts_and_rows_per_file(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1, _ROW_A2, _ROW_A3])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B1, _ROW_B2])

        rows, per_file = load_rows([path_a, path_b])
        acc = compute_row_accounting(rows, per_file)

        self.assertEqual(acc["rows_per_file"][str(path_a)], 3)
        self.assertEqual(acc["rows_per_file"][str(path_b)], 2)
        self.assertEqual(acc["total_rows"], 5)
        # flag=0: A1, A3, B1, B2 = 4; flag=1 (NoBracket): A2 = 1
        self.assertEqual(acc["rows_by_flag"][0], 4)
        self.assertEqual(acc["rows_by_flag"][1], 1)
        # snapped=0: A1, A3, B1 = 3; snapped=1: A2, B2 = 2
        self.assertEqual(acc["rows_by_snapped"][0], 3)
        self.assertEqual(acc["rows_by_snapped"][1], 2)


class FeatureCoverageTest(_TmpDirCase):
    def test_exact_nan_counts_and_fractions(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1, _ROW_A2, _ROW_A3])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B1, _ROW_B2])
        rows, _ = load_rows([path_a, path_b])

        coverage = compute_feature_coverage(rows)

        self.assertEqual(set(coverage.keys()), set(FEATURE_COLUMNS))
        # rv_21d/rv_63d/iv_minus_rv/n_events_to_expiry are NaN on A2 and A3
        # only -> 2 of 5 rows.
        for col in ("rv_21d", "rv_63d", "iv_minus_rv", "n_events_to_expiry"):
            self.assertEqual(coverage[col]["nan_count"], 2)
            self.assertEqual(coverage[col]["total"], 5)
            self.assertAlmostEqual(coverage[col]["fraction"], 0.4, places=12)
        # log_moneyness/tenor_years/market_vol/delta_abs are never NaN here.
        for col in ("log_moneyness", "tenor_years", "market_vol", "delta_abs"):
            self.assertEqual(coverage[col]["nan_count"], 0)
            self.assertAlmostEqual(coverage[col]["fraction"], 0.0, places=12)

    def test_all_nan_column_reports_fraction_one(self) -> None:
        row = _row(1, "SPY", 1.0, 2, "C", 0.2, 0.18, 0.1, 1, 1, 0, 0,
                   0.0, 0.1, 0.18, math.nan, math.nan, math.nan, math.nan, 0.2)
        path = _write_file(self.tmp, "allnan.tsv", [row])
        rows, _ = load_rows([path])
        coverage = compute_feature_coverage(rows)
        self.assertEqual(coverage["rv_21d"]["fraction"], 1.0)

    def test_empty_input_reports_nan_fraction_not_crash(self) -> None:
        path = _write_file(self.tmp, "empty.tsv", [])
        rows, _ = load_rows([path])
        coverage = compute_feature_coverage(rows)
        for col in FEATURE_COLUMNS:
            self.assertEqual(coverage[col]["total"], 0)
            self.assertTrue(math.isnan(coverage[col]["fraction"]))


class TargetDistributionTest(_TmpDirCase):
    def test_bucketed_mean_matches_hand_computed_value_to_1e12(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1, _ROW_A2, _ROW_A3])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B1, _ROW_B2])
        rows, _ = load_rows([path_a, path_b])

        dist = compute_target_distribution(rows)

        # A1, A2, B1, B2 all fall in tenor<=0.12 x delta<0.25.
        bucket = dist["buckets"]["tenor<=0.12 x delta<0.25"]
        expected_values = [
            math.log(0.20 / 0.18),
            math.log(0.25 / 0.22),
            math.log(0.21 / 0.19),
            math.log(0.19 / 0.20),
        ]
        expected_mean = sum(expected_values) / len(expected_values)
        self.assertEqual(bucket["n"], 4)
        self.assertAlmostEqual(bucket["mean"], expected_mean, delta=1e-12)
        self.assertAlmostEqual(bucket["stddev"], statistics.pstdev(expected_values), delta=1e-12)

        # A3 alone falls in tenor<=0.60 x delta>=0.5.
        bucket2 = dist["buckets"]["tenor<=0.60 x delta>=0.5"]
        self.assertEqual(bucket2["n"], 1)
        self.assertAlmostEqual(bucket2["mean"], math.log(0.30 / 0.28), delta=1e-12)
        self.assertEqual(bucket2["stddev"], 0.0)

        # An unpopulated bucket in the fixed 4x3 grid still reports n=0, not
        # a KeyError.
        empty_bucket = dist["buckets"]["tenor>0.60 x delta<0.25"]
        self.assertEqual(empty_bucket["n"], 0)
        self.assertTrue(math.isnan(empty_bucket["mean"]))

    def test_overall_stats_cover_all_five_rows(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1, _ROW_A2, _ROW_A3])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B1, _ROW_B2])
        rows, _ = load_rows([path_a, path_b])

        dist = compute_target_distribution(rows)
        expected_values = [
            math.log(0.20 / 0.18), math.log(0.25 / 0.22), math.log(0.30 / 0.28),
            math.log(0.21 / 0.19), math.log(0.19 / 0.20),
        ]
        self.assertEqual(dist["overall"]["n"], 5)
        self.assertAlmostEqual(dist["overall"]["mean"], statistics.mean(expected_values), delta=1e-12)
        self.assertAlmostEqual(
            dist["overall"]["p50"], percentile(sorted(expected_values), 50), delta=1e-12
        )


class DuplicateDetectionTest(_TmpDirCase):
    def test_cross_file_duplicate_key_is_named(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1, _ROW_A2, _ROW_A3])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B1, _ROW_B2])
        rows, _ = load_rows([path_a, path_b])

        dupes = find_duplicates(rows)

        self.assertEqual(len(dupes), 1)
        key, files = dupes[0]
        self.assertEqual(key, ("1000000000", "SPY", "2000000000", "100.0", "C"))
        self.assertEqual(sorted(files), sorted([str(path_a), str(path_b)]))

    def test_no_duplicates_across_unique_files(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B2])
        rows, _ = load_rows([path_a, path_b])
        self.assertEqual(find_duplicates(rows), [])


class LeakageTripwireTest(unittest.TestCase):
    def test_perfect_correlation_reported_near_one(self) -> None:
        rows = [
            {"log_ratio": "1.0", "iv_minus_rv": "1.0", "market_vol": "5.0"},
            {"log_ratio": "2.0", "iv_minus_rv": "2.0", "market_vol": "5.0"},
            {"log_ratio": "3.0", "iv_minus_rv": "3.0", "market_vol": "5.0"},
        ]
        leakage = compute_leakage(rows)
        self.assertAlmostEqual(leakage["corr_log_ratio_iv_minus_rv"], 1.0, places=12)
        # market_vol is constant here -> zero-variance denominator -> None.
        self.assertIsNone(leakage["corr_log_ratio_market_vol"])


class BuildReportAndMainTest(_TmpDirCase):
    def test_report_contains_expected_counts_and_fractions(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1, _ROW_A2, _ROW_A3])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B2])  # no duplicate here

        report_md, dupes = build_report([path_a, path_b])

        self.assertEqual(dupes, [])
        self.assertIn("Total rows: 4", report_md)
        self.assertIn(str(path_a), report_md)
        self.assertIn(str(path_b), report_md)

    def test_main_writes_report_and_exits_zero_without_duplicates(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B2])
        out_md = self.tmp / "report.md"

        rc = main(["--out-md", str(out_md), str(path_a), str(path_b)])

        self.assertEqual(rc, 0)
        self.assertTrue(out_md.exists())
        self.assertIn("Total rows: 2", out_md.read_text(encoding="utf-8"))

    def test_main_exits_nonzero_and_names_key_on_duplicate(self) -> None:
        path_a = _write_file(self.tmp, "a.tsv", [_ROW_A1, _ROW_A2, _ROW_A3])
        path_b = _write_file(self.tmp, "b.tsv", [_ROW_B1, _ROW_B2])
        out_md = self.tmp / "report.md"

        rc = main(["--out-md", str(out_md), str(path_a), str(path_b)])

        self.assertNotEqual(rc, 0)
        self.assertTrue(out_md.exists())
        text = out_md.read_text(encoding="utf-8")
        # The report names the duplicated key's fields.
        self.assertIn("1000000000", text)
        self.assertIn("100.0", text)


if __name__ == "__main__":
    unittest.main()
