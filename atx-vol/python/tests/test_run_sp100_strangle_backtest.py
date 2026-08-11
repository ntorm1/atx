"""Gate for the SP100 projection-strangle driver (`tools/run_sp100_strangle_backtest.py`).

WHAT IS UNDER TEST. Tasks 1-2 landed the two engine capabilities the SP100 run
needs (synthetic expiries snapped onto the run's session grid; ExcludeAndReport
extended to hedge trading and deferred settlement). Neither is reachable from an
operator's hands until something composes
``SurfaceDb -> Clock.between -> session_ts -> make_dispersion_strangle_spec ->
DeclarativeStrategy -> run_backtest -> tearsheet -> write_backtest_pnl_tsv ->
atxvol.report.dispersion.build_report``. This module gates that composition
through the driver's OBSERVABLE contract — its exit codes, the three files it
writes into ``--out``, and their content — rather than through its internals, so
the driver stays free to change how it reaches those outputs.

THE FIXTURE IS SYNTHETIC AND LOCAL, AND IT IS DELIBERATELY *NOT* THREE NAMES.
Everything under ``C:\\atx-data`` is read-only, so the corpus is built here from
``atxvol`` itself, mirroring ``test_backtest.py``'s proven eSSVI ladder (same
slice grid, same ``theta``/``phi``/``rho`` progression, same one-day
``now_ts_ns`` step) so a divergence between the suites is a driver defect and
not a fixture difference. The name count is FOUR because
``DispersionStrangleConfig``'s default ``missing.min_names`` is 4 and
``make_dispersion_strangle_spec`` rejects ``min_names > names.size()``
outright — a three-name fixture would fail in the spec builder, before the
driver's own logic ran, and would gate nothing. The universe file carries a
fifth symbol that is NOT in the db precisely so ``--exclude`` has something to
remove: excluding it is what brings the list back to four, which is also how
this file proves the exclusion actually reaches the strategy config.

SESSION GEOMETRY HAS TEETH. The eight partitions are weekdays 2026-01-05..14 and
their timestamps advance by CALENDAR days (0,1,2,3,4,7,8,9), so the weekend is a
real three-day gap in ``session_ts`` and not a hidden one-day step. The run
window starts one session AFTER the corpus does, so a driver that filled
``session_ts`` from the whole clock instead of from the ``Clock.between`` window
the run uses would ship a grid the run never visits.

NO ``pyarrow`` IN THIS MODULE, EVER. ``atxvol._core`` and ``pyarrow.lib`` both
link vcpkg's ``arrow.dll``/``parquet.dll`` by base name and the first loaded
claims the process-wide slot on Windows, so importing both breaks the extension
import for the whole file (see ``atxvol/__init__.py``'s ImportError note).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

import atxvol as av
from atxvol.report import dispersion as report_dispersion

# The driver lives in `atx-vol/tools`, which is not a package. `test_sp100_universe.py`
# reaches `pull_opra_hive` the same way.
#
# NOTE, deliberately: importing the driver runs its source-pinning preamble, which
# rewrites `sys.meta_path` and `sys.path` for the whole interpreter. It has to be
# before its own `import atxvol`, so it cannot be deferred into `main()`. It is
# inert here — `conftest.py` hard-fails a contaminated resolution before collection
# and the ctest driver has already stripped the ScikitBuild finder — but a module
# that reconfigures imports as a side effect of being imported should be flagged at
# the import, not discovered.
TOOLS = Path(__file__).resolve().parents[2] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import run_sp100_strangle_backtest as driver  # noqa: E402

# ── Fixture corpus ──────────────────────────────────────────────────────────

K_R = 0.043
DAY_NS = 86_400_000_000_000
BASE_TS = 1_700_000_000_000_000_000
SLICE_TS = [0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00]

INDEX_SYM = "SPY"
NAMES = ["NM0", "NM1", "NM2", "NM3"]
ABSENT = "BK"  # in the universe file, never in the db — the `--exclude` target
SYMBOLS = [INDEX_SYM, *NAMES]

# Weekdays; the timestamp offsets are CALENDAR days, so 01-09 -> 01-12 is a real
# three-day step in the session grid.
DATES = ["2026-01-05", "2026-01-06", "2026-01-07", "2026-01-08",
         "2026-01-09", "2026-01-12", "2026-01-13", "2026-01-14"]
OFFSETS = [0, 1, 2, 3, 4, 7, 8, 9]
WINDOW_LO, WINDOW_HI = "2026-01-06", "2026-01-14"
WINDOW_DATES = DATES[1:]
LABEL = "sp100-strangle-fixture"

SOLVE_LEDGER_KEYS = (
    "sl_al_boundary_solves",
    "sl_al_premium_evals",
    "sl_greeks_fd",
    "sl_greeks_analytic",
    "sl_greeks_adjoint",
    "sl_iv_newton_iters",
    "sl_duplicate_mark_solves",
    "sl_cache_carry_drift",
    # Task P-6 (VarSwap book memo). Matches `ledger::Solve` (counters.hpp),
    # whose new counter is appended before `Count_` so existing indices stay
    # stable. Caller-visible through `av.solve_ledger()`.
    "sl_var_swap_strip_evals",
    # Task F-2 (GammaSwap strip evals, review .../task-F-2-review.md, I-1):
    # appended after `sl_var_swap_strip_evals`, matching `ledger::Solve`'s own
    # `GammaSwapStripEvals` (counters.hpp) appended right before `Count_`.
    # `av.solve_ledger()` now returns a 10-key dict (this file's own 9-tuple,
    # unextended, is exactly what the review's I-1 finding broke); order
    # verified by an executed probe against `counters::ledger::kNames`
    # directly, not by reading the C++ source alone -- see this fix round's
    # report for the probe's output.
    "sl_gamma_swap_strip_evals",
)


def make_surface(spot: float, now_ts: int, vol_bump: float, uid: int) -> av.PricedSurface:
    curves = av.CurveSurface()
    context = []
    for i, t in enumerate(SLICE_TS):
        p = av.EssviParams()
        p.theta = 0.04 + 0.005 * i + vol_bump
        p.phi = 1.5 - 0.05 * i
        p.rho = -0.4 + 0.02 * i
        p.psi = 0.5
        p.p = 0.5
        p.lambda_ = 0.5
        p.T = t
        p.F = spot
        p.expiry_id = i
        curves.push_essvi(p, math.exp(-K_R * t))
        context.append(av.SliceContext(T=t, forward=spot, borrow=0.0, q_eff=0.02,
                                       n_used=250, n_dropped=7))
    pricing = av.PricingContext()
    pricing.S = spot
    pricing.r = K_R
    pricing.now_ts_ns = now_ts
    pricing.method = av.AmericanMethod.ANDERSEN_LAKE
    pricing.al_opts = av.AlOpts.fast()
    pricing.uid = uid
    return av.PricedSurface.create(curves, context, pricing)


def build_fixture_db(root, symbols=None, gaps=None) -> None:
    """One partition per date, each carrying every symbol.

    ``gaps`` maps a date to the symbols that partition OMITS — the shape a real
    corpus's one-session provider gap has, and the only thing that makes
    `DROP_RENORMALIZE` and `EXCLUDE_AND_REPORT` do anything at all.
    """
    symbols = symbols if symbols is not None else SYMBOLS
    gaps = gaps or {}
    db = av.SurfaceDb.create(str(root))
    for d, date in enumerate(DATES):
        ts = BASE_TS + OFFSETS[d] * DAY_NS
        items = [
            (symbol,
             make_surface(100.0 * (i + 1) * (1.0 + 0.002 * d), ts, 0.01 * i, i + 1))
            for i, symbol in enumerate(symbols)
            if symbol not in gaps.get(date, ())
        ]
        db.write_partition(date, items)


def write_universe(path: Path, names=None, extra=(ABSENT,)) -> Path:
    """The shipped `data/universe/sp100_2026-07.csv` header format, verbatim."""
    names = names if names is not None else NAMES
    rows = [("2026-01-01", INDEX_SYM, "100.0", "synthetic-fixture", "2026-01-01")]
    for i, name in enumerate([*names, *extra]):
        rows.append(("2026-01-01", name, f"{10.0 - i:.1f}", "synthetic-fixture", "2026-01-01"))
    lines = ["\t".join(("effective_date", "symbol", "raw_weight", "source", "as_of"))]
    lines += ["\t".join(row) for row in rows]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


@pytest.fixture(scope="module")
def corpus(tmp_path_factory):
    root = tmp_path_factory.mktemp("sp100_driver")
    db_root = root / "db"
    build_fixture_db(db_root)
    universe = write_universe(root / "universe.csv")
    return db_root, universe


# ── The gap corpus: five names, one of them absent for one session ──────────
#
# FIVE names, not four, because the default `missing.min_names` is 4: dropping
# one must leave the basket ON the floor rather than under it, or the run fails
# for the wrong reason and the policy is never reached.
GAP_NAMES = ["NM0", "NM1", "NM2", "NM3", "NM4"]
GAP_SYMBOLS = [INDEX_SYM, *GAP_NAMES]
GAP_DATE = "2026-01-08"     # mid-window, so lots on NM4 are already open
GAP_NAME = "NM4"


@pytest.fixture(scope="module")
def gap_corpus(tmp_path_factory):
    root = tmp_path_factory.mktemp("sp100_gap")
    db_root = root / "db"
    build_fixture_db(db_root, GAP_SYMBOLS, gaps={GAP_DATE: (GAP_NAME,)})
    universe = write_universe(root / "universe.csv", GAP_NAMES, extra=())
    return db_root, universe


def argv_for(corpus, out: Path, *extra: str) -> list[str]:
    db_root, universe = corpus
    return [
        "--db", str(db_root),
        "--universe", str(universe),
        "--from", WINDOW_LO,
        "--to", WINDOW_HI,
        "--out", str(out),
        "--exclude", ABSENT,
        "--index", INDEX_SYM,
        "--tenor-days", "4",
        "--label", LABEL,
        *extra,
    ]


@pytest.fixture(scope="module")
def completed(corpus, tmp_path_factory):
    """One successful run, shared by the artifact assertions."""
    out = tmp_path_factory.mktemp("sp100_out")
    code = driver.main(argv_for(corpus, out))
    return code, out


def meta_header(path: Path) -> dict[str, str]:
    header = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("#"):
            break
        key, _, value = line[1:].strip().partition("=")
        header[key] = value
    return header


def tearsheet_pairs(path: Path) -> dict[str, str]:
    pairs = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, tab, value = line.partition("\t")
        assert tab, line
        pairs[key] = value
    return pairs


# ── The contract ────────────────────────────────────────────────────────────

def test_solve_ledger_snapshot_and_reset_contract():
    snapshot = av.solve_ledger()
    assert tuple(snapshot) == SOLVE_LEDGER_KEYS
    assert all(isinstance(value, int) and not isinstance(value, bool)
               for value in snapshot.values())

    av.reset_solve_ledger()
    assert av.solve_ledger() == {key: 0 for key in SOLVE_LEDGER_KEYS}


def test_run_exits_zero_and_writes_three_artifacts(completed):
    code, out = completed
    assert code == 0
    for name in ("track.tsv", "tearsheet.tsv", "report.html"):
        assert (out / name).exists(), name


def test_track_header_carries_the_regime_and_every_knob(completed):
    _, out = completed
    text = (out / "track.tsv").read_text(encoding="utf-8")
    assert "# friction_regime=" in text

    header = meta_header(out / "track.tsv")
    assert header["friction_regime"] in report_dispersion.REGIMES
    assert header["friction_regime"] == "frictionless"
    # Every knob the CLI exposes, so a track can be reproduced from its own head.
    assert header["target_abs_delta"] == "0.4"
    assert header["tenor_days"] == "4"
    assert header["theta_per_name_daily"] == "10"
    assert header["hedge_band"] == "0"
    assert header["hold_to_expiry"] == "True"
    assert header["snap_expiry_to_sessions"] == "True"
    assert header["entry_every_n_days"] == "1"
    # The two brief MUSTs. Both are ECHOES of the objects that ran (the
    # `RunConfig` handed to `run_backtest`, the `DispersionStrangleConfig` handed
    # to the spec builder), not literals, so these lines fail when the run
    # changes — which is exactly what they could not do while the header carried
    # hand-written constants.
    assert header["unpriced_policy"] == "EXCLUDE_AND_REPORT"
    assert header["record_every_n"] == "1"
    assert header["prefetch_depth"] == "1"
    assert float(header["flat_rate"]) == pytest.approx(K_R, abs=1e-12)
    assert header["missing_policy"] == "DROP_RENORMALIZE"
    assert header["min_names"] == str(av.DispersionStrangleConfig().missing.min_names)
    assert header["index_symbol"] == INDEX_SYM
    assert header["label"] == LABEL
    # The RESOLVED window, not the requested one: the corpus is clamped/subset.
    assert header["date_lo"] == WINDOW_DATES[0]
    assert header["date_hi"] == WINDOW_DATES[-1]
    assert header["n_sessions"] == str(len(WINDOW_DATES))
    # Universe identity: the count AND a hash, so two tracks are comparable.
    # The digest pins the RESOLVED list exactly — index removed, `--exclude`
    # honoured, order preserved, symbols canonicalised — which a bare count
    # would not: a run that kept SPY and dropped a name also counts four.
    assert header["n_names"] == str(len(NAMES))
    assert header["universe_sha256"] == driver.universe_digest(NAMES)
    assert header["excluded"] == ABSENT


def test_the_track_body_is_the_windowed_session_set(completed):
    _, out = completed
    lines = (out / "track.tsv").read_text(encoding="utf-8").splitlines()
    body = [ln for ln in lines if not ln.startswith("#")]
    assert body[0].split("\t")[0] == "date"
    dates = [ln.split("\t")[0] for ln in body[1:]]
    # The window starts one session AFTER the corpus does: a driver that ran the
    # whole clock would show eight rows here.
    assert dates == WINDOW_DATES


def test_tearsheet_tsv_is_key_value_and_carries_final_nav(completed):
    _, out = completed
    pairs = tearsheet_pairs(out / "tearsheet.tsv")
    assert "final_nav" in pairs
    assert math.isfinite(float(pairs["final_nav"]))
    # The library's own fold, not a re-derivation here.
    for key in ("total_return", "max_drawdown", "sharpe", "avg_gross_vega"):
        assert key in pairs, key
    # Backtest timing lands here (and stdout), never in the byte-gated track.
    # cpu_s may quantize to 0.0 on a tiny fixture (Windows process_time ticks
    # at ~15.6 ms), so only wall is asserted strictly positive.
    assert float(pairs["backtest_wall_s"]) > 0.0
    assert float(pairs["backtest_cpu_s"]) >= 0.0
    assert int(pairs["sl_al_boundary_solves"]) > 0
    for key in SOLVE_LEDGER_KEYS:
        assert int(pairs[key]) >= 0


def test_report_html_is_rendered_and_names_the_run(completed):
    _, out = completed
    html = (out / "report.html").read_text(encoding="utf-8")
    assert LABEL in html
    # A stub or an error page would be a few hundred bytes; the real document
    # inlines its stylesheet and its SVG figures.
    assert (out / "report.html").stat().st_size > 20_000


def test_the_report_masthead_describes_this_run_and_not_the_spy_proxy(completed):
    """The renderer defaults to the SPY listed-options proxy's masthead.

    For THIS run that default is false in three of its four clauses — straddles
    (these are strangles), a common listed monthly expiry (this is a synthetic
    tenor snapped onto the run's own sessions), and a roll (this is held to
    expiry) — and it contradicts the byline two lines below it, which the driver
    already fills honestly. The `<title>` is the one line a circulated HTML file
    is guaranteed to be read by, so it must be this run's.
    """
    _, out = completed
    html = (out / "report.html").read_text(encoding="utf-8")

    assert f"<title>{driver.REPORT_TITLE}</title>" in html
    assert driver.REPORT_TITLE in html
    # None of the SPY proxy's masthead may survive anywhere in the document.
    assert report_dispersion.DEFAULT_TITLE not in html
    assert report_dispersion.DEFAULT_EYEBROW not in html
    assert "ATM straddles" not in html
    assert "listed monthly expiry" not in html

    # The standfirst is BUILT from the config that ran, so it moves when the run
    # does rather than being a second prose constant to drift.
    assert "strangles" in html
    assert "snapped onto the run&#x27;s own session grid" in html or \
           "snapped onto the run&#39;s own session grid" in html
    assert "held to expiry, never rolled" in html


def test_a_caller_supplied_masthead_cannot_inject_markup(tmp_path):
    """`Report.standfirst` was the one masthead field interpolated UNESCAPED.

    Harmless while both in-repo renderers hard-coded their own prose; a live
    injection the moment a caller can supply it, which is exactly what
    `build_report`'s new keyword arguments allow. Escaping lives in
    `components.Report` so no call site can reintroduce the hole.
    """
    result = av.BacktestResult()
    result.resize(2)
    result.date = ["2026-01-02", "2026-01-05"]
    result.ts_ns = [1, 2]
    result.pnl_total = [0.0, 100.0]
    result.nav = [0.0, 100.0]
    sheet = av.tearsheet(result)

    path = str(tmp_path / "inject.html")
    report_dispersion.build_report(
        result, sheet, {"friction_regime": "frictionless"}, path,
        title="T & <b>", eyebrow="<i>e</i>", standfirst="<script>alert(1)</script>",
    )
    html = (tmp_path / "inject.html").read_text(encoding="utf-8")
    assert "<script>alert(1)</script>" not in html
    assert "&lt;script&gt;alert(1)&lt;/script&gt;" in html
    assert "<i>e</i>" not in html and "&lt;i&gt;e&lt;/i&gt;" in html
    assert "T &amp; &lt;b&gt;" in html


def test_two_identical_invocations_write_a_byte_identical_track(corpus, tmp_path):
    first, second = tmp_path / "a", tmp_path / "b"
    assert driver.main(argv_for(corpus, first)) == 0
    assert driver.main(argv_for(corpus, second)) == 0
    # Bit-identity, not tolerance: the engine's reproducibility contract reaches
    # the artifact, and nothing wall-clock-derived leaks into the meta header.
    assert (first / "track.tsv").read_bytes() == (second / "track.tsv").read_bytes()
    first_ledger = tearsheet_pairs(first / "tearsheet.tsv")
    second_ledger = tearsheet_pairs(second / "tearsheet.tsv")
    assert {key: first_ledger[key] for key in SOLVE_LEDGER_KEYS} == {
        key: second_ledger[key] for key in SOLVE_LEDGER_KEYS
    }


def track_columns(out: Path) -> dict[str, list[float]]:
    lines = (out / "track.tsv").read_text(encoding="utf-8").splitlines()
    body = [ln for ln in lines if not ln.startswith("#")]
    names = body[0].split("\t")
    rows = [ln.split("\t") for ln in body[1:]]
    return {name: [float(row[i]) for row in rows]
            for i, name in enumerate(names) if name != "date"}


def test_the_book_actually_opens(completed):
    """TEETH. Every assertion above is satisfied by seven rows of zeros."""
    _, out = completed
    cols = track_columns(out)
    # Five symbols x two strangle wings on the first entry, growing while the
    # first cohorts are still alive.
    assert max(cols["n_open_lots"]) >= 2 * (len(NAMES) + 1)
    assert max(abs(v) for v in cols["gross_gamma"]) > 0.0
    assert max(abs(v) for v in cols["turnover_vega"]) > 0.0


def test_the_configured_knobs_reach_the_engine(completed):
    """Three config knobs, each with an observable signature in the series.

    - ``hedge=DELTA_TO_ZERO/DAILY`` with a zero band => post-hedge book delta is
      ~0 on EVERY row, not merely small.
    - the vega-flat cross-leg constraint => the NET (signed) book vega stays
      negligible against the vega actually traded. The comparison is a RATIO,
      not an absolute floor, because the constraint binds AT ENTRY: living
      cohorts drift with the surfaces between entries, so an absolute epsilon
      would be a fixture-specific number rather than a statement about the
      constraint. A book that is merely EMPTY fails the turnover half.
    """
    _, out = completed
    cols = track_columns(out)
    assert max(abs(v) for v in cols["gross_delta"]) < 1e-8
    traded = max(abs(v) for v in cols["turnover_vega"])
    net = max(abs(v) for v in cols["gross_vega"])
    assert traded > 1.0
    assert net / traded < 1e-2, (net, traded)


def test_a_snapped_expiry_settles_inside_the_window(completed):
    """THE POINT OF TASK 1, observed end-to-end.

    ``--tenor-days 4`` from the first session (calendar offset 1) lands on raw
    offset 5, which is a WEEKEND — no partition, no session. Unsnapped, that lot
    would never settle inside this corpus. Snapped, it anchors on the greatest
    session at or before it (offset 4 = 2026-01-09) and settles there, so a
    non-zero ``pnl_settlement`` is direct evidence that ``session_ts`` was
    filled from the run's own window AND that the tenor snapped onto it.
    """
    _, out = completed
    cols = track_columns(out)
    settled = [v for v in cols["pnl_settlement"] if v != 0.0]
    assert settled, "no lot reached expiry inside the window: the snap did nothing"
    # And the open-lot count must FALL on a settlement row: settlement that does
    # not retire the lot would be double-counted for the rest of the run.
    lots = cols["n_open_lots"]
    settle_rows = [i for i, v in enumerate(cols["pnl_settlement"]) if v != 0.0]
    assert any(lots[i] < lots[i - 1] for i in settle_rows if i > 0)


def test_headline_stats_and_one_line_per_artifact_are_printed(corpus, tmp_path, capsys):
    """An operator reads stdout, not the files. Both halves must be there."""
    out = tmp_path / "printed"
    assert driver.main(argv_for(corpus, out)) == 0
    text = capsys.readouterr().out
    for artifact in ("track.tsv", "tearsheet.tsv", "report.html"):
        assert artifact in text, artifact
    for stat in ("sessions", "names", "final NAV", "total PnL", "max drawdown",
                 "mean |net vega|", "unpriced", "solve ledger"):
        assert stat in text, stat
    for key in SOLVE_LEDGER_KEYS:
        assert key in text


def test_exclusion_is_case_insensitive_and_recorded_canonically(corpus, tmp_path):
    """`--exclude bk` and `--exclude BK` are the same request."""
    out = tmp_path / "lowercased"
    argv = argv_for(corpus, out)
    argv[argv.index("--exclude") + 1] = ABSENT.lower()
    assert driver.main(argv) == 0
    header = meta_header(out / "track.tsv")
    assert header["excluded"] == ABSENT
    assert header["universe_sha256"] == driver.universe_digest(NAMES)


def test_the_numeric_knobs_are_wired_from_argparse_not_hardcoded(corpus, tmp_path):
    """Every other invocation in this file leaves `--delta`/`--theta-per-name`/
    `--hedge-band`/`--prefetch-depth` at their defaults, and the header assertions
    check those same defaults — so a config line that ignored the flag entirely
    would pass. One run at non-default values closes all four at once."""
    out = tmp_path / "knobs"
    assert driver.main(argv_for(corpus, out, "--delta", "0.25",
                                "--theta-per-name", "3", "--hedge-band", "0.5",
                                "--prefetch-depth", "3")) == 0
    header = meta_header(out / "track.tsv")
    assert header["target_abs_delta"] == "0.25"
    assert header["theta_per_name_daily"] == "3"
    assert header["hedge_band"] == "0.5"
    assert header["prefetch_depth"] == "3"
    # The renderer reads the hedge band under its own name; it must move too.
    assert header["delta_band"] == "0.5"


def test_run_config_applies_prefetch_depth():
    assert driver.run_config().prefetch_depth == 1
    assert driver.run_config(4).prefetch_depth == 4


def test_session_timestamps_are_the_window_s_and_not_the_corpus_s(corpus):
    """The grid must come from the clock the RUN walks, not from the whole db.

    The end-to-end assertions cannot see this on their own: the only instant a
    whole-corpus grid adds here is the session before the window, and
    `upper_bound` can never select an instant that precedes every entry. So the
    function is gated directly — one entry per ref of the clock handed in, and a
    strictly larger grid when the whole clock is handed in instead.
    """
    db_root, _ = corpus
    db = av.SurfaceDb.open(str(db_root))
    full = av.Clock.from_surface_db(db)
    windowed = full.between(WINDOW_LO, WINDOW_HI)

    window_grid = driver.session_timestamps(db, windowed)
    full_grid = driver.session_timestamps(db, full)

    assert len(window_grid) == len(windowed.refs) == len(WINDOW_DATES)
    assert len(full_grid) == len(full.refs) == len(DATES)
    assert window_grid == sorted(window_grid)
    # The corpus's first instant is NOT in the window's grid — that difference is
    # the whole reason the grid has to be taken from the subset clock.
    assert full_grid[0] == BASE_TS + OFFSETS[0] * DAY_NS
    assert full_grid[0] not in window_grid
    assert window_grid == full_grid[1:]


def test_session_timestamps_uses_the_header_only_db_api():
    expected = {
        "2026-01-05": BASE_TS,
        "2026-01-06": BASE_TS + DAY_NS,
    }

    class HeaderOnlyDb:
        def __init__(self):
            self.calls = []

        def session_ts(self, key):
            self.calls.append(key)
            return expected[key]

        def load_surface(self, *_args):
            raise AssertionError("session_timestamps reconstructed a surface")

    db = HeaderOnlyDb()
    clock = SimpleNamespace(refs=[SimpleNamespace(date=key) for key in expected])

    assert driver.session_timestamps(db, clock) == list(expected.values())
    assert db.calls == list(expected)


def test_corpus_rate_maps_without_reconstructing_a_surface():
    class SurfaceView:
        @staticmethod
        def rate_at(at_T):
            assert at_T == 0.5
            return K_R

    class MapOnlyDb:
        @staticmethod
        def map_surface(date, symbol):
            assert (date, symbol) == ("2026-01-05", INDEX_SYM)
            return SurfaceView()

        @staticmethod
        def load_surface(*_args):
            raise AssertionError("corpus_rate reconstructed a surface")

    clock = SimpleNamespace(refs=[SimpleNamespace(date="2026-01-05")])
    assert driver.corpus_rate(MapOnlyDb(), clock, [INDEX_SYM], 0.5) == K_R


# ── The missing-name / unpriced policies, EXERCISED rather than labelled ────

def test_the_missing_name_policies_are_exercised_not_merely_labelled(gap_corpus, tmp_path):
    """The brief's two policy MUSTs, gated by a corpus that actually needs them.

    On a clean fixture neither policy binds — nothing is ever missing — so both
    were previously invisible: flipping `missing` to `MissingNamePolicy.ERROR` or
    `unpriced` to `UnpricedLotPolicy.ERROR` left the whole suite green. This
    corpus has a real one-session provider gap (NM4 is absent from 2026-01-08),
    which is the shape that makes both do work:

    * that session's ENTRY must drop NM4 and renormalize onto the surviving four
      — `MissingNamePolicy.ERROR` fails the resolve and aborts the run;
    * the NM4 lots ALREADY OPEN from the two prior sessions cannot be priced that
      day — `UnpricedLotPolicy.ERROR` aborts the run.

    So exit 0 here is only reachable with both policies as the brief requires,
    and the non-zero unpriced count proves the second was reached rather than
    merely configured. It is also the only place in this file that exercises
    Task 2's ExcludeAndReport work through the driver.
    """
    out = tmp_path / "gap"
    assert driver.main(argv_for(gap_corpus, out)) == 0

    header = meta_header(out / "track.tsv")
    assert header["n_names"] == str(len(GAP_NAMES))
    assert header["missing_policy"] == "DROP_RENORMALIZE"
    assert header["unpriced_policy"] == "EXCLUDE_AND_REPORT"
    # The gap was REPORTED, not silently absorbed.
    assert float(header["n_unpriced_lots_max"]) > 0.0

    cols = track_columns(out)
    dates = [ln.split("\t")[0] for ln in
             (out / "track.tsv").read_text(encoding="utf-8").splitlines()
             if not ln.startswith("#")][1:]
    gap_row = dates.index(GAP_DATE)
    assert cols["n_unpriced_lots"][gap_row] > 0.0
    # And only there: the sessions around it are whole.
    assert cols["n_unpriced_lots"][gap_row - 1] == 0.0
    # The run kept going — a book still exists on the gap session.
    assert cols["n_open_lots"][gap_row] > 0.0


# ── Failure modes ───────────────────────────────────────────────────────────

def test_a_duplicate_universe_symbol_is_a_usage_error(corpus, tmp_path, capsys):
    db_root, universe = corpus
    bad = tmp_path / "dupe.csv"
    text = universe.read_text(encoding="utf-8").splitlines()
    bad.write_text("\n".join(text + [text[2]]) + "\n", encoding="utf-8")
    code = driver.main([
        "--db", str(db_root), "--universe", str(bad),
        "--from", WINDOW_LO, "--to", WINDOW_HI, "--out", str(tmp_path / "o"),
        "--exclude", ABSENT,
    ])
    assert code == 2
    assert "duplicate" in capsys.readouterr().err.lower()


def test_a_missing_universe_file_is_a_usage_error(corpus, tmp_path, capsys):
    db_root, _ = corpus
    code = driver.main([
        "--db", str(db_root), "--universe", str(tmp_path / "nope.csv"),
        "--from", WINDOW_LO, "--to", WINDOW_HI, "--out", str(tmp_path / "o"),
    ])
    assert code == 2
    assert "nope.csv" in capsys.readouterr().err


@pytest.mark.parametrize("bad_date", ["2026-1-6", "not-a-date", "20260106"])
def test_a_malformed_date_is_a_usage_error_naming_the_flag(corpus, tmp_path, capsys,
                                                           bad_date):
    """A date typo is the caller's mistake, so it belongs in the usage bucket.

    Left to fall through, it reaches `Clock::between`, whose comparison is
    LEXICOGRAPHIC — so `--from not-a-date` came back as exit 1 with
    "date_lo 'not-a-date' > date_hi '2026-01-14'", an ordering complaint about
    something that is not a date at all.
    """
    db_root, universe = corpus
    code = driver.main([
        "--db", str(db_root), "--universe", str(universe),
        "--from", bad_date, "--to", WINDOW_HI, "--out", str(tmp_path / "o"),
        "--exclude", ABSENT,
    ])
    assert code == 2
    err = capsys.readouterr().err
    assert "--from" in err and bad_date in err and "YYYY-MM-DD" in err


def test_a_window_outside_the_corpus_is_an_engine_error(corpus, tmp_path, capsys):
    db_root, universe = corpus
    out = tmp_path / "o"
    code = driver.main([
        "--db", str(db_root), "--universe", str(universe),
        "--from", "2027-01-01", "--to", "2027-02-01", "--out", str(out),
        "--exclude", ABSENT,
    ])
    assert code == 1
    err = capsys.readouterr().err
    # The engine's own message, which NAMES the available range.
    assert DATES[0] in err and DATES[-1] in err
    # A failed run leaves no output directory behind to be mistaken for a stale one.
    assert not out.exists()
