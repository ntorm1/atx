"""A minimal worked example of the backtest half of the wrapper.

This is deliberately NOT a test suite for the engine. The economic invariants —
delta-hedge flattening, attribution closure, settlement, run-to-run bit
determinism, threading equivalence — belong to the C++ gates in
`atx-vol/tests/spy_dispersion_pnl_test.cpp`, which own them properly and run far
faster than any of this. Re-asserting them through pybind11 bought duplicate
coverage at roughly a thousand times the cost: the old version of this file built
96 Andersen-Lake-priced surfaces and ran six full backtests.

What is left is the part only Python can break: that the binding plumbs a real
pipeline end to end (SurfaceDb -> Clock -> spec -> DeclarativeStrategy ->
run_backtest -> tearsheet -> TSV) and that C++ errors surface as `AtxError`.

The fixture is sized for speed, not realism: the smallest shape that still
exercises the plumbing. `AmericanMethod.BAW` is a closed form, so no surface
costs an Andersen-Lake exercise-boundary solve.
"""

from __future__ import annotations

import math

import pytest

import atxvol as av

K_R = 0.043
DAY_NS = 86_400_000_000_000
BASE_TS = 1_700_000_000_000_000_000

NAMES = ["AAPL", "MSFT", "GOOGL", "AMZN", "NVDA"]
INDEX_SYM = "SPY"
BASE_SPOT = [195.0, 410.0, 175.0, 185.0, 120.0]
INDEX_SPOT = 560.0
NUM_DATES = 8
SLICE_TS = [0.05, 0.50]


def make_surface(spot: float, now_ts: int, uid: int) -> av.PricedSurface:
    curves = av.CurveSurface()
    context = []
    for i, t in enumerate(SLICE_TS):
        p = av.EssviParams()
        p.theta = 0.04 + 0.005 * i
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
    pricing.method = av.AmericanMethod.BAW  # closed form: no AL boundary solve
    pricing.uid = uid
    return av.PricedSurface.create(curves, context, pricing)


def d4_config() -> av.DispersionStrangleConfig:
    cfg = av.DispersionStrangleConfig()
    cfg.names = NAMES
    cfg.index_symbol = INDEX_SYM
    cfg.target_abs_delta = 0.40
    cfg.tenor_days = 6.0        # integer tenor over a daily clock => aligned expiries
    cfg.close_dte_days = 2.0
    cfg.theta_per_name_daily = 10.0
    cfg.hold_to_expiry = True
    cfg.missing = av.MissingNameSpec(av.MissingNamePolicy.DROP_RENORMALIZE, 4)
    cfg.hedge = av.HedgeSpec(av.HedgeSpec.Kind.DELTA_TO_ZERO, av.HedgeSpec.Cadence.DAILY, 0.0)
    return cfg


@pytest.fixture(scope="module")
def db_root(tmp_path_factory):
    root = tmp_path_factory.mktemp("atxvol_db") / "db"
    db = av.SurfaceDb.create(str(root))
    for d in range(NUM_DATES):
        ts = BASE_TS + d * DAY_NS
        items = [(name, make_surface(BASE_SPOT[i] * (1.0 + 0.004 * d), ts, i + 1))
                 for i, name in enumerate(NAMES)]
        items.append((INDEX_SYM, make_surface(INDEX_SPOT * (1.0 + 0.003 * d), ts, len(NAMES) + 1)))
        db.write_partition(f"2026-03-{d + 1:02d}", items)
    return root


@pytest.fixture(scope="module")
def run(db_root):
    db = av.SurfaceDb.open(str(db_root))
    clock = av.Clock.from_surface_db(db)
    strat = av.DeclarativeStrategy(av.make_dispersion_strangle_spec(d4_config()))
    cfg = av.RunConfig()
    cfg.snapshot_cache = av.SnapshotCache()
    cfg.unpriced = av.UnpricedLotPolicy.EXCLUDE_AND_REPORT
    return av.run_backtest(clock, strat, cfg)


def test_surface_db_and_clock_round_trip(db_root):
    db = av.SurfaceDb.open(str(db_root))
    partitions = db.partitions()
    assert len(partitions) == NUM_DATES
    assert all(p.surface_count == len(NAMES) + 1 for p in partitions)

    surface = db.load_surface("2026-03-01", INDEX_SYM)
    assert surface.n_slices == len(SLICE_TS)
    assert surface.forward_at(0.5) == pytest.approx(INDEX_SPOT, rel=1e-9)
    assert surface.fair_value(INDEX_SPOT, 0.5, av.Side.CALL) > 0.0

    clock = av.Clock.from_surface_db(db)
    assert len(clock) == NUM_DATES
    assert clock.refs[0].date == "2026-03-01"


def test_pipeline_runs_end_to_end_and_writes_a_tearsheet(run, tmp_path):
    # The whole point of the wrapper: a real run reaches Python with its columns
    # intact, folds through the library tearsheet, and serializes.
    assert len(run) == NUM_DATES
    columns = run.to_dict()
    assert len(columns) >= 28  # 26 fixed series + date + ts_ns
    assert len(columns["nav"]) == NUM_DATES
    assert math.isfinite(float(run.nav[-1]))

    sheet = av.tearsheet(run)
    assert sheet.total_return == pytest.approx(run.nav[-1], abs=1e-12)

    path = tmp_path / "pnl_track.tsv"
    av.write_backtest_pnl_tsv(run, {"strategy": "example", "n_steps": str(len(run))}, str(path))
    lines = path.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "# strategy=example"
    body = [ln for ln in lines if not ln.startswith("#")]
    assert len(body) == len(run) + 1  # column-name row + one row per step


def test_invalid_config_surfaces_as_atxerror():
    # Error translation across the binding boundary is a Python-side contract.
    cfg = d4_config()
    cfg.target_abs_delta = 1.5  # outside (0, 1)
    with pytest.raises(av.AtxError):
        av.make_dispersion_strangle_spec(cfg)
