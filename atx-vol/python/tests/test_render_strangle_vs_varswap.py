"""Gate test for tools/render_strangle_vs_varswap.py.

The renderer's input is the track TSV `atx-vol-strangle-varswap-driver` writes
through `atx::vol::write_backtest_pnl_tsv`: a `# key=value` meta header, the 27
pinned series columns, then one dynamic column per signal — the eight
`StrangleVsVarswapStrategy` comparison signals plus the `swap_pv`/`swap_pnl`
pair the driver appends (those two are deliberately absent from the frozen
serialized column set, so the driver rides them in as signals rather than
touching `kBacktestSeriesColumns`).

The fixture below is hand-built rather than produced by a run, and it encodes
the three data facts the renderer has to survive:

  * a ONE-LEGGED TAIL. The last cycle of a corpus whose calendar runs out
    mid-tenor carries no swap, so every `swap_*` signal is NaN on those rows.
    That is data, not an error.
  * `swap_theta` is NaN ON ITS OWN while the swap is LIVE (`deriv_greeks`
    declines the roll stencil inside one bump width of expiry), so liveness is
    keyed off `swap_vega` and never off `swap_theta`.
  * `skipped_restrikes` / `skipped_swaps` are CUMULATIVE, so the per-session
    event count is the consecutive-row difference.
"""

from __future__ import annotations

import importlib.util
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

import pytest


_ATX_VOL_ROOT = pathlib.Path(__file__).resolve().parents[2]
_TOOL = _ATX_VOL_ROOT / "tools" / "render_strangle_vs_varswap.py"

# The renderer imports matplotlib + pandas at MODULE scope, and this suite
# declares neither: `atx-vol/python/pyproject.toml`'s test extra is
# `["pytest>=7", "numpy>=1.23"]`. Executing it unconditionally would turn a
# missing OPTIONAL dependency into a COLLECTION error, and a collection error in
# one module reds the WHOLE `atx-vol-python` ctest lane — every other module in
# this directory would report failure because a plotting library is absent.
#
# Skipping at module level is the policy the lane already applies one level up:
# `_ctest_pytest_driver.py` exits `SKIP_RETURN_CODE` 77 when the compiled
# extension is missing, so the lane reads as Skipped-with-a-reason rather than
# red. An absent prerequisite is a skip; only a present one that misbehaves is a
# failure.
#
# WIDENING THE SHARED TEST EXTRA IS DELIBERATELY NOT THE FIX. That list is the
# install contract for every consumer of `pip install .[test]`, including the
# scikit-build-core wheel build, and adding a plotting stack to it to serve one
# module charges every other module for a dependency none of them import.
# `pytest` is itself declared, so this guard adds no new dependency of its own.
try:
    import matplotlib  # noqa: F401
    import pandas  # noqa: F401
except ImportError as exc:  # pragma: no cover — depends on the host environment
    pytest.skip(
        "tools/render_strangle_vs_varswap.py needs matplotlib + pandas, which the "
        f"atx-vol Python test extra does not declare: {exc}",
        allow_module_level=True,
    )

_spec = importlib.util.spec_from_file_location("render_strangle_vs_varswap", _TOOL)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"cannot load renderer from {_TOOL}")
renderer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = renderer
_spec.loader.exec_module(renderer)


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

META = {
    "strategy": "xom_strangle_vs_varswap",
    "symbol": "XOM",
    "data_source": "surface_db",
    "db_root": "C:/atx-data/surface-db/sp100-2026",
    "window_start": "2026-01-02",
    "window_end": "2026-01-08",
    "n_steps": "5",
    "delta_target": "0.4",
    "tenor_days": "91",
    "contracts": "100",
    "hedge": "delta_to_zero_daily",
    "skipped_restrikes": "1",
    "skipped_swaps": "1",
    "total_return": "700.0",
    "sharpe": "0.9",
    "max_drawdown": "800.0",
}

# The 27 pinned columns of write_backtest_pnl_tsv (date, ts_ns, then the 25 F64
# series of backtest_series_columns.hpp), in order.
PINNED_COLUMNS = [
    "date", "ts_ns", "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega", "pnl_vanna",
    "pnl_volga", "pnl_theta", "pnl_rho", "pnl_charm", "pnl_unexplained", "pnl_settlement",
    "pnl_shares", "financing", "cost", "nav", "cash", "gross_delta", "gross_gamma",
    "gross_vega", "gross_theta", "turnover_notional", "turnover_vega", "n_open_lots",
    "n_unpriced_lots", "n_unpriced_greeks",
]

# The dynamic signal tail: the strategy's eight, then the driver's two.
SIGNAL_COLUMNS = [
    "swap_delta", "swap_gamma", "swap_vega", "swap_theta", "swap_rho", "strangle_vega",
    "skipped_restrikes", "skipped_swaps", "swap_pv", "swap_pnl",
]

DATES = ["2026-01-02", "2026-01-05", "2026-01-06", "2026-01-07", "2026-01-08"]
BASE_TS = 1767398400000000000
DAY_NS = 86_400_000_000_000

PNL_TOTAL = [0.0, 1200.0, -800.0, 450.0, -150.0]
SWAP_PNL = [0.0, 500.0, -300.0, 100.0, 0.0]
SWAP_PV = [0.0, 500.0, 200.0, 0.0, 0.0]
GROSS_VEGA = [12000.0, 11800.0, 11500.0, 6000.0, 5800.0]
GROSS_DELTA = [0.0, 1e-7, -2e-7, 5e-8, 0.0]
GROSS_GAMMA = [3.2, 3.4, 3.6, 1.1, 1.0]
GROSS_THETA = [-450.0, -460.0, -470.0, -220.0, -215.0]
# Row 1 has a LIVE swap with a NaN theta; rows 3-4 are the one-legged tail.
SWAP_VEGA = [12000.0, 11750.0, 11400.0, math.nan, math.nan]
SWAP_THETA = [-120.0, math.nan, -125.0, math.nan, math.nan]
SWAP_DELTA = [-30.0, -28.0, -25.0, math.nan, math.nan]
SWAP_GAMMA = [0.9, 0.95, 1.0, math.nan, math.nan]
SWAP_RHO = [40.0, 41.0, 42.0, math.nan, math.nan]
STRANGLE_VEGA = [12000.0, 11800.0, 11500.0, 6000.0, 5800.0]
SKIPPED_RESTRIKES = [0.0, 0.0, 1.0, 1.0, 1.0]
SKIPPED_SWAPS = [0.0, 0.0, 0.0, 1.0, 1.0]

# nav is the engine's running Σ step_total, i.e. the cumulative pnl_total.
NAV = [0.0, 1200.0, 400.0, 850.0, 700.0]


def _cell(v: float) -> str:
    # write_backtest_pnl_tsv writes %.17g, which renders a quiet NaN as "nan".
    return "nan" if isinstance(v, float) and math.isnan(v) else repr(float(v))


def _row(i: int, *, with_swap: bool) -> list[str]:
    pinned = {
        "date": DATES[i],
        "ts_ns": str(BASE_TS + i * DAY_NS),
        "pnl_total": _cell(PNL_TOTAL[i]),
        "pnl_delta": _cell(0.0),
        "pnl_gamma": _cell(0.4 * PNL_TOTAL[i]),
        "pnl_vega": _cell(0.3 * PNL_TOTAL[i]),
        "pnl_vanna": _cell(0.0),
        "pnl_volga": _cell(0.0),
        "pnl_theta": _cell(0.2 * PNL_TOTAL[i]),
        "pnl_rho": _cell(0.0),
        "pnl_charm": _cell(0.0),
        "pnl_unexplained": _cell(0.1 * PNL_TOTAL[i]),
        "pnl_settlement": _cell(0.0),
        "pnl_shares": _cell(0.0),
        "financing": _cell(0.0),
        "cost": _cell(0.0),
        "nav": _cell(NAV[i]),
        "cash": _cell(1_000_000.0 - NAV[i]),
        "gross_delta": _cell(GROSS_DELTA[i]),
        "gross_gamma": _cell(GROSS_GAMMA[i]),
        "gross_vega": _cell(GROSS_VEGA[i]),
        "gross_theta": _cell(GROSS_THETA[i]),
        "turnover_notional": _cell(250_000.0),
        "turnover_vega": _cell(4200.0),
        "n_open_lots": _cell(2.0),
        "n_unpriced_lots": _cell(0.0),
        "n_unpriced_greeks": _cell(0.0),
    }
    out = [pinned[c] for c in PINNED_COLUMNS]
    if with_swap:
        signals = {
            "swap_delta": SWAP_DELTA[i],
            "swap_gamma": SWAP_GAMMA[i],
            "swap_vega": SWAP_VEGA[i],
            "swap_theta": SWAP_THETA[i],
            "swap_rho": SWAP_RHO[i],
            "strangle_vega": STRANGLE_VEGA[i],
            "skipped_restrikes": SKIPPED_RESTRIKES[i],
            "skipped_swaps": SKIPPED_SWAPS[i],
            "swap_pv": SWAP_PV[i],
            "swap_pnl": SWAP_PNL[i],
        }
        out.extend(_cell(signals[c]) for c in SIGNAL_COLUMNS)
    return out


def write_track(path: pathlib.Path, *, with_swap: bool = True) -> None:
    """The exact bytes write_backtest_pnl_tsv would emit for this fixture."""
    columns = PINNED_COLUMNS + (SIGNAL_COLUMNS if with_swap else [])
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        for k, v in META.items():
            fh.write(f"# {k}={v}\n")
        fh.write("\t".join(columns) + "\n")
        for i in range(len(DATES)):
            fh.write("\t".join(_row(i, with_swap=with_swap)) + "\n")


class TrackReaderTests(unittest.TestCase):
    def test_reads_meta_header_and_tab_separated_series(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            track = pathlib.Path(raw) / "track.tsv"
            write_track(track)

            meta, df = renderer.read_track(track)

            self.assertEqual(meta["symbol"], "XOM")
            self.assertEqual(meta["strategy"], "xom_strangle_vs_varswap")
            self.assertEqual(len(df), 5)
            self.assertIn("swap_pnl", df.columns)
            self.assertIn("swap_vega", df.columns)
            # `date` is parsed, so the panels can put a real time axis on x.
            self.assertEqual(str(df["date"].iloc[0].date()), "2026-01-02")


class LegSplitTests(unittest.TestCase):
    def _legs(self, *, with_swap: bool = True):
        with tempfile.TemporaryDirectory() as raw:
            track = pathlib.Path(raw) / "track.tsv"
            write_track(track, with_swap=with_swap)
            _meta, df = renderer.read_track(track)
            return renderer.split_legs(df)

    def test_strangle_leg_is_pnl_total_net_of_the_swap_column(self) -> None:
        legs = self._legs()

        # pnl_total is the engine's whole step total (options + settlement +
        # hedge shares + financing - cost + swap), so the OPTIONS-side leg is
        # what is left once the swap's flow column is taken back out.
        self.assertEqual(list(legs.strangle_step), [0.0, 700.0, -500.0, 350.0, -150.0])
        self.assertEqual(list(legs.strangle_cum), [0.0, 700.0, 200.0, 550.0, 400.0])

    def test_swap_leg_is_the_cumulative_swap_pnl_column(self) -> None:
        legs = self._legs()

        self.assertEqual(list(legs.swap_step), SWAP_PNL)
        self.assertEqual(list(legs.swap_cum), [0.0, 500.0, 200.0, 300.0, 300.0])

    def test_two_legs_sum_back_to_the_runs_nav(self) -> None:
        legs = self._legs()

        for got_strangle, got_swap, nav in zip(legs.strangle_cum, legs.swap_cum, NAV):
            self.assertAlmostEqual(got_strangle + got_swap, nav, places=9)

    def test_swap_liveness_keys_off_vega_not_theta(self) -> None:
        legs = self._legs()

        # Row 1 has a live swap with a NaN theta; rows 3-4 are the one-legged
        # tail. Keying liveness off swap_theta would drop row 1 as well.
        self.assertEqual(list(legs.swap_live), [True, True, True, False, False])
        self.assertEqual(legs.n_swap_live_rows, 3)

    def test_cumulative_skip_counters_are_differenced_into_per_step_events(self) -> None:
        legs = self._legs()

        self.assertEqual(list(legs.restrike_events), [0.0, 0.0, 1.0, 0.0, 0.0])
        self.assertEqual(list(legs.swap_skip_events), [0.0, 0.0, 0.0, 1.0, 0.0])
        self.assertEqual(legs.total_restrike_skips, 1.0)
        self.assertEqual(legs.total_swap_skips, 1.0)

    def test_nan_swap_rows_do_not_poison_the_cumulative_leg(self) -> None:
        legs = self._legs()

        for v in legs.swap_cum:
            self.assertFalse(math.isnan(v), "a NaN swap greek must not reach the P&L leg")

    def test_a_track_with_no_swap_columns_still_splits(self) -> None:
        legs = self._legs(with_swap=False)

        # An options-only run (enable_swap_leg off) is legal input: the swap leg
        # is flat-zero and no row reports a live swap.
        self.assertEqual(list(legs.strangle_step), PNL_TOTAL)
        self.assertEqual(list(legs.swap_cum), [0.0] * 5)
        self.assertEqual(legs.n_swap_live_rows, 0)


class RenderTests(unittest.TestCase):
    def test_renders_a_png_over_a_track_with_nan_swap_rows(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "report.png"
            write_track(track)

            summary = renderer.render(track, out)

            self.assertTrue(out.exists())
            png = out.read_bytes()
            self.assertEqual(png[:8], PNG_MAGIC)
            self.assertGreater(len(png), 20_000, "a real 150-dpi figure is > 20 KB")
            self.assertEqual(summary["n_rows"], 5)
            self.assertEqual(summary["n_swap_live_rows"], 3)
            self.assertAlmostEqual(summary["strangle_total"], 400.0, places=9)
            self.assertAlmostEqual(summary["swap_total"], 300.0, places=9)
            self.assertEqual(summary["skipped_restrikes"], 1.0)
            self.assertEqual(summary["skipped_swaps"], 1.0)

    def test_every_comparison_series_reaches_a_panel(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            write_track(track)
            _meta, df = renderer.read_track(track)

            fig, panels = renderer.build_figure(META, df)
            try:
                # One entry per rendered comparison panel, keyed by the pair of
                # columns it overlays — this is the renderer's contract with the
                # signal names Task 3 froze.
                self.assertEqual(
                    set(panels),
                    {"pnl", "vega", "delta", "gamma", "theta"},
                )
                self.assertEqual(panels["vega"], ("gross_vega", "swap_vega"))
                self.assertEqual(panels["delta"], ("gross_delta", "swap_delta"))
                self.assertEqual(panels["gamma"], ("gross_gamma", "swap_gamma"))
                self.assertEqual(panels["theta"], ("gross_theta", "swap_theta"))
                self.assertEqual(panels["pnl"], ("strangle_cum", "swap_cum"))
            finally:
                renderer.close_figure(fig)

    def test_renders_a_self_contained_html_page(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "report.html"
            write_track(track)

            renderer.render(track, out)

            html = out.read_text(encoding="utf-8")
            self.assertIn("<html", html.lower())
            # Self-contained: the figure is inlined, never a sidecar reference.
            self.assertIn("data:image/png;base64,", html)
            self.assertIn("XOM", html)

    def test_renders_an_options_only_track(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "report.png"
            write_track(track, with_swap=False)

            summary = renderer.render(track, out)

            self.assertEqual(out.read_bytes()[:8], PNG_MAGIC)
            self.assertEqual(summary["n_swap_live_rows"], 0)
            self.assertEqual(summary["swap_total"], 0.0)


class CommandLineTests(unittest.TestCase):
    def test_cli_writes_the_report_it_is_given(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = pathlib.Path(raw)
            track = directory / "track.tsv"
            out = directory / "cli.png"
            write_track(track)

            proc = subprocess.run(
                [sys.executable, str(_TOOL), str(track), str(out)],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(out.read_bytes()[:8], PNG_MAGIC)

    def test_cli_reports_usage_without_arguments(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(_TOOL)], capture_output=True, text=True, check=False
        )

        self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
