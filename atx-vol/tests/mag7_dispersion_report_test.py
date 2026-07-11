#!/usr/bin/env python3
"""Gate test for tools/mag7_dispersion_report.py.

Writes a minimal synthetic run dir matching the T6 output-dir contract
(examples/mag7_dispersion_backtest.cpp: series.csv, strategy_metrics.csv,
engine_metrics.csv, db_stats.csv, optional populate_stats.csv -- `# k=v` meta
lines then a header row), invokes the report script via subprocess (exactly
as a real caller would from the command line), and asserts on the emitted
HTML: it exists, embeds real inline SVG, carries the three pinned section
headings, documents the pinned strategy defaults in its header block, and
contains no external-asset markers (self-containment).
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

# Import the module to test _fmt_value directly
sys.path.insert(0, str(pathlib.Path(__file__).parents[1]))
from tools.mag7_dispersion_report import _fmt_value


SCRIPT = pathlib.Path(__file__).parents[1] / "tools" / "mag7_dispersion_report.py"

# The 18-key shared meta block every T6-emitted file carries verbatim
# (examples/mag7_dispersion_backtest.cpp, MetaKv `meta`).
SHARED_META = {
    "strategy": "mag7_dispersion_strangle",
    "names": "AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA",
    "index_symbol": "SPY",
    "data_source": "surface_db",
    "db_root": "C:/fake/db",
    "db_generation": "1",
    "window_start": "2026-01-05",
    "window_end": "2026-01-07",
    "n_steps": "3",
    "delta_target": "0.4",
    "tenor_days": "90",
    "close_dte_days": "10",
    "theta_per_name_daily": "10",
    "entry_every_n_days": "1",
    "multiplier": "100",
    "frictions": "off",
    "missing_policy": "drop_renormalize",
    "min_names": "4",
}

SERIES_HEADER = (
    "date,ts_ns,pnl_total,nav,pnl_delta,pnl_gamma,pnl_vega,pnl_vanna,pnl_volga,"
    "pnl_theta,pnl_rho,pnl_charm,pnl_unexplained,pnl_settlement,pnl_shares,"
    "financing,cost,cash,gross_delta,gross_gamma,gross_vega,gross_theta,"
    "turnover_notional,turnover_vega,n_open_lots,n_unpriced_lots,n_unpriced_greeks"
)

SERIES_ROWS = [
    "2026-01-05,1767571200000000000,120.5,120.5,10.0,5.0,80.0,1.0,2.0,25.0,0.5,0.1,-3.6,0.0,0.0,-1.2,-0.4,998.8,15000.0,120.0,42000.0,-260.0,900000.0,4200.0,16,0,0",
    "2026-01-06,1767657600000000000,-45.2,75.3,-8.0,4.0,-30.0,0.5,1.5,26.0,0.3,0.2,-39.7,0.0,0.0,-1.1,-0.3,997.4,14500.0,118.0,41500.0,-255.0,120000.0,600.0,32,0,0",
    "2026-01-07,1767744000000000000,210.9,286.2,12.0,6.0,150.0,0.8,1.9,27.0,0.4,0.15,12.85,0.0,0.0,-1.3,-0.5,996.6,16000.0,125.0,43000.0,-262.0,150000.0,700.0,48,0,0",
]

STRATEGY_METRIC_ROWS = [
    ("total_return", "286.2"),
    ("ann_return", "0.31"),
    ("ann_vol", "0.12"),
    ("sharpe", "1.45"),
    ("max_drawdown", "-45.2"),
    ("hit_rate", "0.6667"),
    ("avg_turnover", "390000"),
    ("total_cost", "3.6"),
    ("total_financing", "-1.2"),
    ("attr_delta", "14.0"),
    ("attr_gamma", "15.0"),
    ("attr_vega", "200.0"),
    ("attr_vanna", "2.3"),
    ("attr_volga", "5.4"),
    ("attr_theta", "78.0"),
    ("attr_rho", "1.2"),
    ("attr_charm", "0.45"),
    ("attr_unexplained", "-30.45"),
    ("return_on_gross_vega", "0.0067"),
    ("vega_adj_sharpe", "1.1"),
    ("pnl_per_vega_traded", "0.05"),
    ("avg_gross_vega", "121.0"),
    ("avg_gross_gamma", "5.0"),
    ("total_pnl", "286.2"),
    ("avg_daily_pnl", "82.85"),
    ("avg_net_vega", "121.0"),
    ("avg_net_theta", "42166.67"),
    ("avg_open_lots", "32.0"),
    ("peak_open_lots", "48"),
    ("total_unpriced_lots", "0"),
    ("total_unpriced_greeks", "0"),
    ("n_steps", "3"),
]

ENGINE_METRIC_ROWS = [
    ("wall_clock_ms", "42.5"),
    ("steps_per_s", "70.6"),
    ("n_steps", "3"),
    ("cache_loads", "24"),
    ("cache_hits", "16"),
    ("cache_prefetches", "8"),
]

DB_STATS_ROWS = [
    ("2026-01-05", "8", "40960", "1767571200000000000"),
    ("2026-01-06", "8", "41200", "1767657600000000000"),
    ("2026-01-07", "8", "41500", "1767744000000000000"),
]

POPULATE_STATS_ROWS = [
    ("AAPL", "3", "3", "0", "0", "1.0", "0.92"),
    ("MSFT", "3", "2", "1", "0", "0.6667", "0.88"),
    ("TSLA", "3", "3", "0", "0", "1.0", "nan"),
]


def _write_meta_csv(path: pathlib.Path, meta: dict[str, str], header: str, rows: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        for k, v in meta.items():
            fh.write(f"# {k}={v}\n")
        fh.write(header + "\n")
        for row in rows:
            fh.write(row + "\n")


def _write_rows(path: pathlib.Path, meta: dict[str, str], header: str,
                 rows: list[tuple[str, ...]]) -> None:
    _write_meta_csv(path, meta, header, [",".join(r) for r in rows])


def write_run_dir(root: pathlib.Path, with_populate: bool = True) -> pathlib.Path:
    """Write a minimal synthetic run dir matching the T6 output-dir contract."""
    _write_meta_csv(root / "series.csv", SHARED_META, SERIES_HEADER, SERIES_ROWS)
    _write_rows(
        root / "strategy_metrics.csv", SHARED_META, "metric,value", STRATEGY_METRIC_ROWS
    )
    _write_rows(
        root / "engine_metrics.csv", SHARED_META, "metric,value", ENGINE_METRIC_ROWS
    )
    db_meta = dict(SHARED_META)
    db_meta.update(
        {
            "db_root": SHARED_META["db_root"],
            "generation": "1",
            "n_symbols": "8",
            "n_partitions": "3",
            "total_file_size": "123660",
        }
    )
    _write_rows(
        root / "db_stats.csv", db_meta, "key,surface_count,file_size,created_ts_ns",
        DB_STATS_ROWS,
    )
    if with_populate:
        populate_meta = {
            "n_boards": "3", "n_ok": "8", "n_failed": "1", "n_dates_written": "3",
        }
        _write_rows(
            root / "populate_stats.csv", populate_meta,
            "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band",
            POPULATE_STATS_ROWS,
        )
    return root


def run_script(run_dir: pathlib.Path, out: pathlib.Path | None = None) -> subprocess.CompletedProcess:
    args = [sys.executable, str(SCRIPT), str(run_dir)]
    if out is not None:
        args.append(str(out))
    return subprocess.run(args, text=True, capture_output=True, check=False)


class Mag7DispersionReportTest(unittest.TestCase):
    def test_full_run_dir_renders_self_contained_report(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = write_run_dir(pathlib.Path(directory), with_populate=True)
            result = run_script(run_dir)
            self.assertEqual(result.returncode, 0, result.stderr)

            out = run_dir / "mag7_dispersion_report.html"
            self.assertTrue(out.exists(), "default output HTML was not written")
            text = out.read_text(encoding="utf-8")

            # Real inline SVG chart(s), not a placeholder / external image.
            self.assertIn("<svg", text)

            # The three exact, pinned section headings.
            self.assertIn("Strategy metrics", text)
            self.assertIn("Engine metrics", text)
            self.assertIn("Surface/db statistics", text)

            # Pinned strategy defaults documented in the report header.
            self.assertIn("theta_per_name_daily", text)
            self.assertIn("delta_target", text)
            self.assertIn("tenor_days", text)
            self.assertIn("multiplier", text)
            self.assertIn("frictions", text)
            self.assertIn("missing_policy", text)

            # Self-containment: no external assets, no script, no network refs.
            self.assertNotIn("http://", text)
            self.assertNotIn("https://", text)
            self.assertNotIn("src=", text)
            self.assertNotIn("<script", text)

            # Per-symbol fit table renders when populate_stats.csv is present.
            self.assertIn("AAPL", text)
            self.assertIn("mean_oos_in_band", text)

            # UNPRICED_METRIC_KEYS rows (total_unpriced_lots, total_unpriced_greeks)
            # belong on the engine panel only, not duplicated onto the strategy
            # table (STRATEGY_METRIC_ROWS above carries both keys).
            strategy_start = text.index("Strategy metrics")
            engine_start = text.index("Engine metrics")
            surface_start = text.index("Surface/db statistics")
            strategy_html = text[strategy_start:engine_start]
            engine_html = text[engine_start:surface_start]
            self.assertNotIn("total_unpriced_lots", strategy_html)
            self.assertNotIn("total_unpriced_greeks", strategy_html)
            self.assertIn("total_unpriced_lots", engine_html)
            self.assertIn("total_unpriced_greeks", engine_html)

    def test_populate_stats_absent_still_renders(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = write_run_dir(pathlib.Path(directory), with_populate=False)
            self.assertFalse((run_dir / "populate_stats.csv").exists())
            out_path = run_dir / "custom_report.html"
            result = run_script(run_dir, out_path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(out_path.exists())
            text = out_path.read_text(encoding="utf-8")
            self.assertIn("<svg", text)
            self.assertIn("Strategy metrics", text)
            self.assertIn("Engine metrics", text)
            self.assertIn("Surface/db statistics", text)
            self.assertNotIn("http://", text)
            self.assertNotIn("https://", text)
            self.assertNotIn("src=", text)

    def test_missing_required_file_is_a_clean_error(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = pathlib.Path(directory)
            result = run_script(run_dir)
            self.assertNotEqual(result.returncode, 0)


class FmtValueTest(unittest.TestCase):
    """Unit tests for _fmt_value formatter, especially edge cases."""

    def test_fmt_value_nan(self):
        """NaN should format as 'nan'."""
        self.assertEqual(_fmt_value(float("nan")), "nan")

    def test_fmt_value_positive_infinity(self):
        """Positive infinity should format as 'inf'."""
        self.assertEqual(_fmt_value(float("inf")), "inf")

    def test_fmt_value_negative_infinity(self):
        """Negative infinity should format as '-inf'."""
        self.assertEqual(_fmt_value(float("-inf")), "-inf")

    def test_fmt_value_integers(self):
        """Integers should format with thousands separators, no decimals."""
        self.assertEqual(_fmt_value(42), "42")
        self.assertEqual(_fmt_value(1000), "1,000")
        self.assertEqual(_fmt_value(-1000), "-1,000")

    def test_fmt_value_floats(self):
        """Floats should format with 4 decimals and thousands separators."""
        self.assertEqual(_fmt_value(3.14159), "3.1416")
        self.assertEqual(_fmt_value(1234.5678), "1,234.5678")


if __name__ == "__main__":
    unittest.main()
