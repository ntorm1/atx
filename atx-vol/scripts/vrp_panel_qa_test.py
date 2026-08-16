"""Tests for vrp_panel_qa.py (vrp_panel_v1 QA report, round-2 F1 tripwire).

Pure stdlib (unittest), self-contained tmp-dir fixtures (synthetic frozen
vrp_panel_v1 TSVs written by this file, mirroring analytics/vrp_panel.hpp's
writer layout exactly) -- no real corpus/driver dependency; mirrors
bev_label_qa_test.py's shape.

Run: python -m pytest atx-vol/scripts/vrp_panel_qa_test.py -q
"""

from __future__ import annotations

import math
import tempfile
import unittest
from pathlib import Path

from vrp_panel_qa import (
    COLUMNS,
    HORIZON_SESSIONS,
    build_report,
    check_label_identity,
    check_t21_successors,
    find_duplicates,
    load_rows,
    main,
    parse_tsv_file,
)

_META = "# schema=vrp_panel_v1\n# horizon_days=21\n"
_HEADER = "\t".join(COLUMNS)

# The label identity the checker recomputes -- SAME expression, so the float
# arithmetic matches bit-for-bit and a clean fixture has exactly zero error.
_HORIZON_YEARS = 21 / 252


def _panel_row(
    symbol: str,
    date: str,
    ts: int,
    iv: float = 0.2,
    rv: float | None = 0.25,
    label_override: str | None = None,
) -> str:
    """One frozen-layout row. rv=None -> tail/predict-time row (NaN rv+label).
    label_override plants a deliberate identity violation."""
    if rv is None:
        rv_s = "nan"
        label_s = "nan"
    else:
        rv_s = repr(rv)
        label_s = repr((rv * rv - iv * iv) * _HORIZON_YEARS)
    if label_override is not None:
        label_s = label_override
    feats = [
        "-3.0",            # f0_log_rv1
        "-3.1",            # f1_log_rv5
        "-3.2",            # f2_log_rv21
        repr(math.log(iv * iv)),  # f3_iv_level
        "0.01",            # f4_term_slope
        "-0.05",           # f5_hv_iv_gap
        "0.001",           # f6_vrp_lag
        "0.02",            # f7_ret_21d
        "0",               # f8_jump_recent
        "0.12",            # f9_vov_63d
    ]
    fields = [symbol, date, str(ts), "100.0", repr(iv), repr(iv * 1.03), rv_s, label_s] + feats
    return "\t".join(fields)


def _symbol_rows(symbol: str, n_rows: int, n_labeled: int, ts0: int = 1_000) -> list[str]:
    """n_rows sessions for one symbol, the first n_labeled labeled, the rest
    NaN-tail. n_labeled = n_rows - 21 reproduces the panel builder's shape."""
    rows = []
    for i in range(n_rows):
        rows.append(
            _panel_row(
                symbol,
                f"2024-{i:03d}",
                ts0 + i,
                rv=(0.25 if i < n_labeled else None),
            )
        )
    return rows


def _write_panel(tmp: Path, name: str, rows: list[str]) -> Path:
    path = tmp / name
    path.write_text(_META + _HEADER + "\n" + "\n".join(rows) + "\n", encoding="utf-8")
    return path


class _TmpDirCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmpdir_obj = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmpdir_obj.name)

    def tearDown(self) -> None:
        self._tmpdir_obj.cleanup()


class TestParseTsvFile(_TmpDirCase):
    def test_parses_clean_panel(self) -> None:
        path = _write_panel(self.tmp, "a.tsv", _symbol_rows("AAA", 30, 9))
        header, rows = parse_tsv_file(path)
        self.assertEqual(tuple(header), COLUMNS)
        self.assertEqual(len(rows), 30)

    def test_rejects_reordered_header(self) -> None:
        path = self.tmp / "bad.tsv"
        bad_header = "\t".join(("date", "symbol") + COLUMNS[2:])
        path.write_text(_META + bad_header + "\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            parse_tsv_file(path)

    def test_rejects_missing_schema_line(self) -> None:
        path = self.tmp / "noschema.tsv"
        path.write_text(_HEADER + "\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            parse_tsv_file(path)


class TestT21Successors(_TmpDirCase):
    def test_clean_panel_has_no_violations(self) -> None:
        # 30 rows, 9 labeled: labeled row p=8 has exactly 21 later rows.
        path = _write_panel(self.tmp, "clean.tsv", _symbol_rows("AAA", 30, 9))
        rows, _counts = load_rows([path])
        result = check_t21_successors(rows)
        self.assertEqual(result["n_checked"], 9)
        self.assertEqual(result["violations"], [])

    def test_truncated_tail_violates_per_row(self) -> None:
        # Drop the last 2 tail rows: labeled p=8 has 19 successors, p=7 has
        # 20 -- exactly two violations, the earlier labeled rows stay clean.
        rows_txt = _symbol_rows("AAA", 30, 9)[:-2]
        path = _write_panel(self.tmp, "trunc.tsv", rows_txt)
        rows, _counts = load_rows([path])
        result = check_t21_successors(rows)
        self.assertEqual(len(result["violations"]), 2)
        self.assertIn("AAA/2024-008", result["violations"][1])
        self.assertIn("need 21", result["violations"][0])

    def test_interior_hole_away_from_tail_is_clean(self) -> None:
        # Dropping a mid-panel session does NOT trip the invariant (pure row
        # counting, no adjacency assumption): 31 rows, 9 labeled, drop row 12
        # (a tail-NaN row) -> every labeled row still has >= 21 later rows.
        rows_txt = _symbol_rows("AAA", 31, 9)
        del rows_txt[12]
        path = _write_panel(self.tmp, "hole.tsv", rows_txt)
        rows, _counts = load_rows([path])
        result = check_t21_successors(rows)
        self.assertEqual(result["violations"], [])

    def test_symbols_are_independent(self) -> None:
        # BBB is truncated, AAA is clean: only BBB rows violate.
        rows_txt = _symbol_rows("AAA", 30, 9) + _symbol_rows("BBB", 25, 9, ts0=10_000)
        path = _write_panel(self.tmp, "two.tsv", rows_txt)
        rows, _counts = load_rows([path])
        result = check_t21_successors(rows)
        self.assertEqual(len(result["violations"]), 5)
        for v in result["violations"]:
            self.assertTrue(v.startswith("BBB/"))


class TestHardTierWiring(_TmpDirCase):
    def test_clean_panel_exits_zero(self) -> None:
        path = _write_panel(self.tmp, "clean.tsv", _symbol_rows("AAA", 30, 9))
        out_md = self.tmp / "report.md"
        self.assertEqual(main([str(path), "--out-md", str(out_md)]), 0)
        report = out_md.read_text(encoding="utf-8")
        self.assertIn("## 5. F1 t+21 session-coverage (report-only by default)", report)
        self.assertIn("No violations.", report)

    def test_f1_violating_panel_is_report_only_by_default(self) -> None:
        # Round-2 fix: expected thin-history attrition (rows the trainer
        # rejects per-row by design) must NOT fail QA by default -- exit 0,
        # with the count and the affected labeled rows still in the report.
        rows_txt = _symbol_rows("AAA", 30, 9)[:-2]
        path = _write_panel(self.tmp, "trunc.tsv", rows_txt)
        out_md = self.tmp / "report.md"
        self.assertEqual(main([str(path), "--out-md", str(out_md)]), 0)
        report = out_md.read_text(encoding="utf-8")
        self.assertIn("report-only by default", report)
        self.assertIn("2 violation(s)", report)
        self.assertIn("AAA/2024-007", report)
        self.assertIn("AAA/2024-008", report)

    def test_f1_threshold_flag_exceeded_exits_one(self) -> None:
        # Opt-in hard gate: 2 violations > N=1 -> exit 1.
        rows_txt = _symbol_rows("AAA", 30, 9)[:-2]
        path = _write_panel(self.tmp, "trunc.tsv", rows_txt)
        out_md = self.tmp / "report.md"
        self.assertEqual(
            main([str(path), "--out-md", str(out_md), "--max-t21-violations", "1"]), 1
        )

    def test_f1_threshold_flag_met_exactly_exits_zero(self) -> None:
        # Exit 1 only when the count EXCEEDS N: 2 violations, N=2 -> exit 0.
        rows_txt = _symbol_rows("AAA", 30, 9)[:-2]
        path = _write_panel(self.tmp, "trunc.tsv", rows_txt)
        out_md = self.tmp / "report.md"
        self.assertEqual(
            main([str(path), "--out-md", str(out_md), "--max-t21-violations", "2"]), 0
        )

    def test_f1_threshold_zero_on_clean_panel_exits_zero(self) -> None:
        path = _write_panel(self.tmp, "clean.tsv", _symbol_rows("AAA", 30, 9))
        out_md = self.tmp / "report.md"
        self.assertEqual(
            main([str(path), "--out-md", str(out_md), "--max-t21-violations", "0"]), 0
        )

    def test_f1_threshold_negative_is_a_usage_error(self) -> None:
        # A sign typo must be a usage error at argparse (exit 2), never a
        # fake gate failure: pre-fix, -1 on a ZERO-violation panel exited 1
        # with "0 violation(s) exceed the -1 threshold" (fix-2 review minor).
        path = _write_panel(self.tmp, "clean.tsv", _symbol_rows("AAA", 30, 9))
        out_md = self.tmp / "report.md"
        with self.assertRaises(SystemExit) as ctx:
            main([str(path), "--out-md", str(out_md), "--max-t21-violations", "-1"])
        self.assertEqual(ctx.exception.code, 2)
        self.assertFalse(out_md.exists())  # rejected before any work

    def test_duplicate_keys_still_exit_one(self) -> None:
        a = _write_panel(self.tmp, "a.tsv", _symbol_rows("AAA", 30, 9))
        b = _write_panel(self.tmp, "b.tsv", _symbol_rows("AAA", 30, 9))
        rows, _counts = load_rows([a, b])
        self.assertTrue(find_duplicates(rows))
        out_md = self.tmp / "report.md"
        self.assertEqual(main([str(a), str(b), "--out-md", str(out_md)]), 1)

    def test_label_identity_violation_still_exits_one(self) -> None:
        rows_txt = _symbol_rows("AAA", 30, 9)
        rows_txt[0] = _panel_row("AAA", "2024-000", 1_000, label_override="0.999")
        path = _write_panel(self.tmp, "ident.tsv", rows_txt)
        rows, _counts = load_rows([path])
        self.assertEqual(len(check_label_identity(rows)["violations"]), 1)
        out_md = self.tmp / "report.md"
        self.assertEqual(main([str(path), "--out-md", str(out_md)]), 1)

    def test_nan_mismatch_still_exits_one(self) -> None:
        # rv NaN but label finite: the 4a NaN-ness tripwire, not the F1 one.
        rows_txt = _symbol_rows("AAA", 30, 9)
        fields = rows_txt[0].split("\t")
        fields[6] = "nan"  # rv_fwd_21d
        rows_txt[0] = "\t".join(fields)
        path = _write_panel(self.tmp, "nanmix.tsv", rows_txt)
        out_md = self.tmp / "report.md"
        self.assertEqual(main([str(path), "--out-md", str(out_md)]), 1)

    def test_malformed_file_exits_two(self) -> None:
        path = self.tmp / "malformed.tsv"
        path.write_text("not a panel\n", encoding="utf-8")
        out_md = self.tmp / "report.md"
        self.assertEqual(main([str(path), "--out-md", str(out_md)]), 2)

    def test_horizon_constant_matches_frozen_contract(self) -> None:
        self.assertEqual(HORIZON_SESSIONS, 21)
        _report, hard, n_t21 = build_report(
            [_write_panel(self.tmp, "c.tsv", _symbol_rows("AAA", 30, 9))]
        )
        self.assertFalse(hard)
        self.assertEqual(n_t21, 0)


if __name__ == "__main__":
    unittest.main()
