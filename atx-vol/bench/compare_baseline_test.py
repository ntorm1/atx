"""Tests for compare_baseline.py — the bench regression gate.

Focus: the M1 fit-row blind spot. The corpus rows ``fit/e2e/{spy_real,100name}``
run ``Iterations(1)`` and emit no aggregate row, so they used to be invisible to
the gate: a regression or a crash on the two headline fit rows passed silently.
These tests pin the iteration-``real_time`` fallback, the CV-unguarded advisory
verdict, and the crash → MISSING → fail path (incl. ``error_occurred``).
"""

import contextlib
import io
import json
import tempfile
import unittest
import unittest.mock
from pathlib import Path

import compare_baseline


HOST_CTX = {
    "host_name": "test-host",
    "num_cpus": 16,
    "mhz_per_cpu": 2100,
    "library_build_type": "release",
    "caches": [],
}


def _iter_row(name: str, real_time: float, *, error: bool = False) -> dict:
    row = {
        "name": f"{name}/iterations:1/real_time",
        "run_name": f"{name}/iterations:1/real_time",
        "run_type": "iteration",
        "real_time": real_time,
        "cpu_time": real_time,
    }
    if error:
        row["error_occurred"] = True
        row["error_message"] = "boom"
    return row


def _agg_rows(name: str, median: float, cv: float) -> list:
    run_name = f"{name}/repeats:5/real_time"
    rows = []
    # A couple of iteration rows (as real Google Benchmark JSON emits) that must
    # NOT be double-counted as a separate unguarded record.
    for _ in range(2):
        rows.append({
            "name": run_name, "run_name": run_name,
            "run_type": "iteration", "real_time": median,
        })
    for agg_name, value in (("median", median), ("mean", median), ("cv", cv)):
        rows.append({
            "name": run_name, "run_name": run_name,
            "run_type": "aggregate", "aggregate_name": agg_name,
            "real_time": value,
        })
    return rows


def _doc(benchmarks: list) -> dict:
    return {"context": dict(HOST_CTX), "benchmarks": benchmarks}


def _run_main(base_doc: dict, new_doc: dict, *extra_args: str):
    """Run compare_baseline.main on two docs; return (exit_code, stdout)."""
    with tempfile.TemporaryDirectory() as tmp:
        base_path = Path(tmp) / "base.json"
        new_path = Path(tmp) / "new.json"
        base_path.write_text(json.dumps(base_doc), encoding="utf-8")
        new_path.write_text(json.dumps(new_doc), encoding="utf-8")
        argv = ["compare_baseline.py", str(base_path), str(new_path), *extra_args]
        buf = io.StringIO()
        with unittest.mock.patch("sys.argv", argv), contextlib.redirect_stdout(buf):
            code = compare_baseline.main()
    return code, buf.getvalue()


class CollectRowsTest(unittest.TestCase):
    def test_iteration_only_row_falls_back_and_is_flagged_unguarded(self) -> None:
        doc = _doc([_iter_row("fit/e2e/spy_real", 1031.4)])
        rows = compare_baseline.collect_rows(doc)
        key = "fit/e2e/spy_real/iterations:1/real_time"
        self.assertIn(key, rows)
        self.assertEqual(rows[key]["median"], 1031.4)
        self.assertTrue(rows[key].get("unguarded"))
        self.assertIsNone(rows[key].get("cv"))

    def test_aggregated_benchmark_is_not_duplicated_as_unguarded(self) -> None:
        doc = _doc(_agg_rows("price/backtest/cold", 200.0, 0.01))
        rows = compare_baseline.collect_rows(doc)
        self.assertEqual(len(rows), 1)
        rec = next(iter(rows.values()))
        self.assertEqual(rec["median"], 200.0)
        self.assertEqual(rec["cv"], 0.01)
        self.assertFalse(rec.get("unguarded"))

    def test_error_occurred_iteration_row_contributes_no_data(self) -> None:
        doc = _doc([_iter_row("fit/e2e/spy_real", 0.0, error=True)])
        self.assertEqual(compare_baseline.collect_rows(doc), {})


class GateBehaviourTest(unittest.TestCase):
    def test_self_compare_surfaces_fit_rows_and_passes(self) -> None:
        doc = _doc([
            _iter_row("fit/e2e/spy_real", 1031.4),
            _iter_row("fit/e2e/100name", 211834.0),
        ])
        code, out = _run_main(doc, doc)
        self.assertEqual(code, 0)
        self.assertIn("fit/e2e/spy_real", out)
        self.assertIn("fit/e2e/100name", out)
        self.assertIn("ok*", out)  # CV-unguarded verdict

    def test_crashed_fit_row_fails_loudly_as_missing(self) -> None:
        base = _doc([
            _iter_row("fit/e2e/spy_real", 1031.4),
            _iter_row("fit/e2e/100name", 211834.0),
        ])
        # 100name crashed → produced no row at all in the new run.
        new = _doc([_iter_row("fit/e2e/spy_real", 1031.4)])
        code, out = _run_main(base, new)
        self.assertEqual(code, 1)
        self.assertIn("MISSING", out)
        self.assertIn("fit/e2e/100name", out)
        self.assertIn("FAIL", out)

    def test_errored_fit_row_reads_as_missing_and_fails(self) -> None:
        base = _doc([_iter_row("fit/e2e/spy_real", 1031.4)])
        new = _doc([_iter_row("fit/e2e/spy_real", 0.0, error=True)])
        code, out = _run_main(base, new)
        self.assertEqual(code, 1)
        self.assertIn("MISSING", out)

    def test_unguarded_regression_is_surfaced_but_advisory(self) -> None:
        base = _doc([_iter_row("fit/e2e/spy_real", 1000.0)])
        new = _doc([_iter_row("fit/e2e/spy_real", 1500.0)])  # +50%
        code, out = _run_main(base, new)
        # Not silent: printed with the ``*`` unguarded regression marker...
        self.assertIn("REGRESS?*", out)
        self.assertIn("unguarded_moves=1", out)
        # ...but advisory: a lone iteration has no CV, so it does not hard-fail.
        self.assertEqual(code, 0)
        self.assertIn("PASS", out)

    def test_aggregate_regression_on_matching_host_still_gates(self) -> None:
        base = _doc(_agg_rows("price/backtest/cold", 200.0, 0.01))
        new = _doc(_agg_rows("price/backtest/cold", 260.0, 0.01))  # +30%, CV 1%
        code, out = _run_main(base, new)
        self.assertEqual(code, 1)
        self.assertIn("REGRESS", out)
        self.assertIn("FAIL", out)


if __name__ == "__main__":
    unittest.main()
