#!/usr/bin/env python3
"""Gate test for tools/spy_dispersion_pnl_report.py (WS-D D5).

Writes a small synthetic PnL-track TSV in the EXACT shape
`atx::vol::write_backtest_pnl_tsv` emits (a `# key=value` meta header then a
tab-separated series with the pinned column set), invokes the renderer via
subprocess exactly as a caller would, and asserts on the emitted PNG: it
exists, begins with the PNG magic bytes, and is non-trivial (a real 150-dpi
matplotlib figure is > 20 KB). This is the acceptance render path end-to-end:
a (fixture) backtest PnL track -> a rendered PNG.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).parents[1] / "tools" / "spy_dispersion_pnl_report.py"

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# The meta keys examples/spy_dispersion_pnl.cpp writes (a representative subset;
# the renderer tolerates missing keys with sensible fallbacks).
META = {
    "strategy": "spy_dispersion_vega_flat",
    "names": "NVDA,AAPL,MSFT,AMZN,GOOGL",
    "index_symbol": "SPY",
    "data_source": "surface_db",
    "db_root": "C:/atx-data/surfdb/spy_disp_ytd",
    "db_generation": "1",
    "window_start": "2026-01-02",
    "window_end": "2026-07-17",
    "n_steps": "8",
    "n_names": "5",
    "delta_target": "0.4",
    "tenor_days": "90",
    "theta_per_name_daily": "10",
    "entry_every_n_days": "1",
    "multiplier": "100",
    "hold_to_expiry": "on",
    "hedge": "delta_to_zero_daily",
    "frictions": "off",
    "missing_policy": "drop_renormalize",
    "min_names": "4",
    "dropped_alphabet_class": "GOOG",
    "total_return": "12500.0",
    "ann_return": "0.31",
    "ann_vol": "8200.0",
    "sharpe": "1.45",
    "max_drawdown": "3400.0",
    "hit_rate": "0.62",
    "avg_gross_vega": "42000.0",
    "avg_gross_gamma": "120.0",
    "return_on_gross_vega": "0.0067",
    "peak_open_lots": "160",
    "wall_clock_ms": "480.0",
    "steps_per_s": "16.7",
}

# The pinned column order of write_backtest_pnl_tsv (tearsheet.cpp).
SERIES_COLUMNS = [
    "date", "ts_ns", "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega", "pnl_vanna",
    "pnl_volga", "pnl_theta", "pnl_rho", "pnl_charm", "pnl_unexplained", "pnl_settlement",
    "pnl_shares", "financing", "cost", "nav", "cash", "gross_delta", "gross_gamma",
    "gross_vega", "gross_theta", "turnover_notional", "turnover_vega", "n_open_lots",
    "n_unpriced_lots", "n_unpriced_greeks",
]


def _synthetic_rows(n: int = 8) -> list[dict[str, object]]:
    """A small deterministic PnL walk covering every rendered column."""
    rows = []
    nav = 0.0
    base_ts = 1767398400000000000
    for i in range(n):
        pnl = 1500.0 + 900.0 * ((i % 3) - 1)  # oscillating daily P&L
        nav += pnl if i > 0 else 0.0
        rows.append({
            "date": f"2026-01-{i + 2:02d}",
            "ts_ns": base_ts + i * 86_400_000_000_000,
            "pnl_total": pnl if i > 0 else 0.0,
            "pnl_delta": 0.0,
            "pnl_gamma": 0.30 * pnl,
            "pnl_vega": 0.10 * pnl,
            "pnl_vanna": 0.0,
            "pnl_volga": 0.0,
            "pnl_theta": 0.55 * pnl,
            "pnl_rho": 0.0,
            "pnl_charm": 0.0,
            "pnl_unexplained": 0.05 * pnl,
            "pnl_settlement": 0.0,
            "pnl_shares": -0.02 * pnl,
            "financing": 0.0,
            "cost": 0.0,
            "nav": nav,
            "cash": 1_000_000.0 - nav,
            "gross_delta": 1e-7,  # post-hedge net delta ~ 0
            "gross_gamma": 100.0 + 3.0 * i,
            "gross_vega": 40_000.0 + 500.0 * i,
            "gross_theta": 3650.0,
            "turnover_notional": 250_000.0,
            "turnover_vega": 4200.0,
            "n_open_lots": 16.0 * min(i + 1, 5),
            "n_unpriced_lots": 0.0,
            "n_unpriced_greeks": 0.0,
        })
    return rows


def write_tsv(path: pathlib.Path) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        for k, v in META.items():
            fh.write(f"# {k}={v}\n")
        fh.write("\t".join(SERIES_COLUMNS) + "\n")
        for row in _synthetic_rows():
            fh.write("\t".join(str(row[c]) for c in SERIES_COLUMNS) + "\n")


def run_script(tsv: pathlib.Path, out: pathlib.Path | None = None) -> subprocess.CompletedProcess:
    args = [sys.executable, str(SCRIPT), str(tsv)]
    if out is not None:
        args.append(str(out))
    return subprocess.run(args, text=True, capture_output=True, check=False)


class SpyDispersionPnlReportTest(unittest.TestCase):
    def test_renders_pnl_track_png_default_path(self):
        with tempfile.TemporaryDirectory() as directory:
            tsv = pathlib.Path(directory) / "pnl_track.tsv"
            write_tsv(tsv)
            result = run_script(tsv)
            self.assertEqual(result.returncode, 0, result.stderr)

            out = tsv.with_name(tsv.stem + "_pnl_track.png")
            self.assertTrue(out.exists(), "default output PNG was not written")
            data = out.read_bytes()
            self.assertTrue(data.startswith(PNG_MAGIC), "output is not a PNG")
            # A real 150-dpi multi-panel figure is well over 20 KB; guard against
            # a truncated / blank render.
            self.assertGreater(len(data), 20_000, "PNG is trivially small")

    def test_renders_with_calendar_gap_annotation(self):
        # I1(c): when meta records missing sessions / a narrowed window, the
        # renderer still produces a PNG (the gap-annotation branch is taken).
        with tempfile.TemporaryDirectory() as directory:
            tsv = pathlib.Path(directory) / "gap.tsv"
            gap_meta = dict(META)
            gap_meta.update({
                "requested_start": "2026-01-02",
                "requested_end": "2026-07-17",
                "window_start": "2026-01-02",
                "window_end": "2026-07-10",
                "calendar_source": "expected_sessions_file",
                "expected_sessions": "10",
                "missing_sessions": "2",
                "missing_sessions_list": "2026-03-06,2026-05-11",
                "window_narrowed": "yes",
            })
            with tsv.open("w", encoding="utf-8", newline="\n") as fh:
                for k, v in gap_meta.items():
                    fh.write(f"# {k}={v}\n")
                fh.write("\t".join(SERIES_COLUMNS) + "\n")
                for row in _synthetic_rows():
                    fh.write("\t".join(str(row[c]) for c in SERIES_COLUMNS) + "\n")
            result = run_script(tsv)
            self.assertEqual(result.returncode, 0, result.stderr)
            out = tsv.with_name(tsv.stem + "_pnl_track.png")
            self.assertTrue(out.exists())
            data = out.read_bytes()
            self.assertTrue(data.startswith(PNG_MAGIC))
            self.assertGreater(len(data), 20_000)

    def test_custom_output_path(self):
        with tempfile.TemporaryDirectory() as directory:
            tsv = pathlib.Path(directory) / "run.tsv"
            write_tsv(tsv)
            out = pathlib.Path(directory) / "acceptance.png"
            result = run_script(tsv, out)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(out.exists())
            self.assertTrue(out.read_bytes().startswith(PNG_MAGIC))

    def test_missing_input_is_a_clean_error(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = pathlib.Path(directory) / "nope.tsv"
            result = run_script(missing)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
