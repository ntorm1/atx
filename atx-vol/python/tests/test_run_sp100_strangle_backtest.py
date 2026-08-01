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

import pytest

import atxvol as av
from atxvol.report import dispersion as report_dispersion

# The driver lives in `atx-vol/tools`, which is not a package. `test_sp100_universe.py`
# reaches `pull_opra_hive` the same way.
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


def build_fixture_db(root) -> None:
    db = av.SurfaceDb.create(str(root))
    for d, date in enumerate(DATES):
        ts = BASE_TS + OFFSETS[d] * DAY_NS
        items = [
            (symbol,
             make_surface(100.0 * (i + 1) * (1.0 + 0.002 * d), ts, 0.01 * i, i + 1))
            for i, symbol in enumerate(SYMBOLS)
        ]
        db.write_partition(date, items)


def write_universe(path: Path) -> Path:
    """The shipped `data/universe/sp100_2026-07.csv` header format, verbatim."""
    rows = [("2026-01-01", INDEX_SYM, "100.0", "synthetic-fixture", "2026-01-01")]
    for i, name in enumerate(NAMES):
        rows.append(("2026-01-01", name, f"{10.0 - i:.1f}", "synthetic-fixture", "2026-01-01"))
    rows.append(("2026-01-01", ABSENT, "0.5", "synthetic-fixture", "2026-01-01"))
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


# ── The contract ────────────────────────────────────────────────────────────

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
    assert header["unpriced_policy"] == "EXCLUDE_AND_REPORT"
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
    pairs = {}
    for line in (out / "tearsheet.tsv").read_text(encoding="utf-8").splitlines():
        key, tab, value = line.partition("\t")
        assert tab, line
        pairs[key] = value
    assert "final_nav" in pairs
    assert math.isfinite(float(pairs["final_nav"]))
    # The library's own fold, not a re-derivation here.
    for key in ("total_return", "max_drawdown", "sharpe", "avg_gross_vega"):
        assert key in pairs, key


def test_report_html_is_rendered_and_names_the_run(completed):
    _, out = completed
    html = (out / "report.html").read_text(encoding="utf-8")
    assert LABEL in html
    # A stub or an error page would be a few hundred bytes; the real document
    # inlines its stylesheet and its SVG figures.
    assert (out / "report.html").stat().st_size > 20_000


def test_two_identical_invocations_write_a_byte_identical_track(corpus, tmp_path):
    first, second = tmp_path / "a", tmp_path / "b"
    assert driver.main(argv_for(corpus, first)) == 0
    assert driver.main(argv_for(corpus, second)) == 0
    # Bit-identity, not tolerance: the engine's reproducibility contract reaches
    # the artifact, and nothing wall-clock-derived leaks into the meta header.
    assert (first / "track.tsv").read_bytes() == (second / "track.tsv").read_bytes()


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
                 "mean |net vega|", "unpriced"):
        assert stat in text, stat


def test_exclusion_is_case_insensitive_and_recorded_canonically(corpus, tmp_path):
    """`--exclude bk` and `--exclude BK` are the same request."""
    out = tmp_path / "lowercased"
    argv = argv_for(corpus, out)
    argv[argv.index("--exclude") + 1] = ABSENT.lower()
    assert driver.main(argv) == 0
    header = meta_header(out / "track.tsv")
    assert header["excluded"] == ABSENT
    assert header["universe_sha256"] == driver.universe_digest(NAMES)


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


def test_a_window_outside_the_corpus_is_an_engine_error(corpus, tmp_path, capsys):
    db_root, universe = corpus
    code = driver.main([
        "--db", str(db_root), "--universe", str(universe),
        "--from", "2027-01-01", "--to", "2027-02-01", "--out", str(tmp_path / "o"),
        "--exclude", ABSENT,
    ])
    assert code == 1
    err = capsys.readouterr().err
    # The engine's own message, which NAMES the available range.
    assert DATES[0] in err and DATES[-1] in err
