"""End-to-end gates for the backtest half of the wrapper.

Drives the same library pipeline `examples/spy_dispersion_pnl.cpp` composes —
SurfaceDb -> Clock -> make_dispersion_strangle_spec -> DeclarativeStrategy ->
run_backtest -> tearsheet -> write_backtest_pnl_tsv — entirely from Python,
including building the fixture SurfaceDb. Mirrors the C++ gates in
`tests/spy_dispersion_pnl_test.cpp` so the bindings are held to the same
economic invariants as the library.
"""

from __future__ import annotations

import math

import pytest

import atxvol as av

K_R = 0.043
DAY_NS = 86_400_000_000_000
BASE_TS = 1_700_000_000_000_000_000

NAMES = ["AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "META", "TSLA"]
INDEX_SYM = "SPY"
BASE_SPOT = [195.0, 410.0, 175.0, 185.0, 120.0, 480.0, 250.0]
VOL_BUMP = [0.00, 0.01, 0.02, 0.03, 0.04, 0.05, 0.06]
INDEX_SPOT = 560.0
NUM_DATES = 12
SLICE_TS = [0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00]


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
    for d in range(NUM_DATES):
        ts = BASE_TS + d * DAY_NS
        items = [(name, make_surface(BASE_SPOT[i] * (1.0 + 0.004 * d), ts, VOL_BUMP[i], i + 1))
                 for i, name in enumerate(NAMES)]
        items.append((INDEX_SYM,
                      make_surface(INDEX_SPOT * (1.0 + 0.003 * d), ts, 0.0, len(NAMES) + 1)))
        db.write_partition(f"2026-03-{d + 1:02d}", items)


def d4_config(hold_to_expiry: bool = True, hedge: bool = True) -> av.DispersionStrangleConfig:
    cfg = av.DispersionStrangleConfig()
    cfg.names = NAMES
    cfg.index_symbol = INDEX_SYM
    cfg.target_abs_delta = 0.40
    cfg.tenor_days = 6.0        # integer tenor over a daily clock => aligned expiries
    cfg.close_dte_days = 2.0
    cfg.theta_per_name_daily = 10.0
    cfg.hold_to_expiry = hold_to_expiry
    cfg.missing = av.MissingNameSpec(av.MissingNamePolicy.DROP_RENORMALIZE, 4)
    if hedge:
        cfg.hedge = av.HedgeSpec(av.HedgeSpec.Kind.DELTA_TO_ZERO, av.HedgeSpec.Cadence.DAILY, 0.0)
    return cfg


@pytest.fixture(scope="module")
def db_root(tmp_path_factory):
    root = tmp_path_factory.mktemp("atxvol_db") / "db"
    build_fixture_db(root)
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


def test_surface_db_roundtrip(db_root):
    db = av.SurfaceDb.open(str(db_root))
    partitions = db.partitions()
    assert len(partitions) == NUM_DATES
    assert [p.key for p in partitions] == [f"2026-03-{d + 1:02d}" for d in range(NUM_DATES)]
    assert all(p.surface_count == len(NAMES) + 1 for p in partitions)

    surface = db.load_surface("2026-03-01", "SPY")
    assert surface.n_slices == len(SLICE_TS)
    assert surface.forward_at(0.5) == pytest.approx(INDEX_SPOT, rel=1e-9)
    assert surface.iv(INDEX_SPOT, 0.5) > 0.0
    assert surface.fair_value(INDEX_SPOT, 0.5, av.Side.CALL) > 0.0


def test_surface_db_session_ts_matches_the_partition_surface(db_root):
    db = av.SurfaceDb.open(str(db_root))
    for day in (0, NUM_DATES - 1):
        key = f"2026-03-{day + 1:02d}"
        surface_ts = db.load_surface(key, INDEX_SYM).pricing.now_ts_ns
        assert db.session_ts(key) == surface_ts == BASE_TS + day * DAY_NS


def test_loaded_surface_exposes_rate_without_owned_reconstruction(db_root):
    db = av.SurfaceDb.open(str(db_root))
    mapped = db.map_surface("2026-03-01", INDEX_SYM)
    owned = db.load_surface("2026-03-01", INDEX_SYM)
    assert mapped.rate_at(0.5) == owned.rate_at(0.5)


def test_clock_spans_every_partition(db_root):
    clock = av.Clock.from_surface_db(av.SurfaceDb.open(str(db_root)))
    assert len(clock) == NUM_DATES
    assert clock.refs[0].date == "2026-03-01"
    assert clock.refs[-1].date == "2026-03-12"


def test_spec_shape_hold_to_expiry_hedged():
    spec = av.make_dispersion_strangle_spec(d4_config())
    assert len(spec.legs) == len(NAMES) + 1
    assert spec.lifecycle.holding == av.LifecycleSpec.Holding.HOLD_TO_EXPIRY
    assert spec.lifecycle.entry == av.LifecycleSpec.Entry.EVERY_STEP
    assert spec.hedge.kind == av.HedgeSpec.Kind.DELTA_TO_ZERO
    assert spec.hedge.cadence == av.HedgeSpec.Cadence.DAILY
    assert spec.constraint.kind == av.CrossLegConstraint.Kind.FLAT_VEGA
    assert (spec.constraint.group_a, spec.constraint.group_b) == ("basket", "index")


def test_invalid_config_raises():
    cfg = d4_config()
    cfg.target_abs_delta = 1.5  # outside (0, 1)
    with pytest.raises(av.AtxError):
        av.make_dispersion_strangle_spec(cfg)


def test_run_produces_full_series(run):
    assert len(run) == NUM_DATES
    columns = run.to_dict()
    # 26 fixed series + date + ts_ns.
    assert len(columns) >= 28
    assert len(columns["nav"]) == NUM_DATES
    assert columns["nav"][-1] == pytest.approx(run.nav[-1])
    assert run.n_open_lots.max() > 0.0


def test_daily_delta_hedge_flattens_net_delta(run):
    # HedgeSpec band 0 => post-hedge book delta is ~0 on every row.
    assert abs(run.gross_delta).max() < 1e-8


def test_held_to_expiry_settles(db_root):
    settled = run_with(db_root, d4_config(hold_to_expiry=True))
    control = run_with(db_root, d4_config(hold_to_expiry=False))
    assert abs(settled.pnl_settlement).sum() > 0.0
    assert abs(control.pnl_settlement).sum() == 0.0


def run_with(db_root, cfg) -> av.BacktestResult:
    db = av.SurfaceDb.open(str(db_root))
    clock = av.Clock.from_surface_db(db)
    strat = av.DeclarativeStrategy(av.make_dispersion_strangle_spec(cfg))
    rc = av.RunConfig()
    rc.snapshot_cache = av.SnapshotCache()
    return av.run_backtest(clock, strat, rc)


def test_attribution_closure_identity(run):
    t = av.tearsheet(run)
    closure = (t.attr_delta + t.attr_gamma + t.attr_vega + t.attr_vanna + t.attr_volga
               + t.attr_theta + t.attr_rho + t.attr_charm + t.attr_unexplained
               + t.attr_settlement + t.attr_shares + t.attr_financing - t.attr_cost)
    assert closure == pytest.approx(t.total_return, abs=1e-9)
    assert t.total_return == pytest.approx(run.nav[-1], abs=1e-12)


def test_determinism_two_runs_and_threads(db_root):
    a = run_with(db_root, d4_config())
    b = run_with(db_root, d4_config())
    for column in ("pnl_total", "nav", "gross_vega", "gross_delta", "pnl_settlement", "cash"):
        # Bit-identity, not tolerance: the engine's reproducibility contract.
        assert getattr(a, column).tobytes() == getattr(b, column).tobytes(), column

    db = av.SurfaceDb.open(str(db_root))
    clock = av.Clock.from_surface_db(db)
    strat = av.DeclarativeStrategy(av.make_dispersion_strangle_spec(d4_config()))
    rc = av.RunConfig()
    rc.snapshot_cache = av.SnapshotCache()
    rc.price.n_threads = 4
    threaded = av.run_backtest(clock, strat, rc)
    assert threaded.nav.tobytes() == a.nav.tobytes()


def test_pnl_tsv_roundtrip(run, tmp_path):
    sheet = av.tearsheet(run)
    meta = {"strategy": "spy_dispersion_vega_flat", "n_steps": str(len(run)),
            "total_return": f"{sheet.total_return:.10g}"}
    path = tmp_path / "pnl_track.tsv"
    av.write_backtest_pnl_tsv(run, meta, str(path))

    lines = path.read_text(encoding="utf-8").splitlines()
    header = [ln for ln in lines if ln.startswith("#")]
    body = [ln for ln in lines if not ln.startswith("#")]
    assert header[0] == "# strategy=spy_dispersion_vega_flat"
    assert len(body) == len(run) + 1  # column-name row + one row per step
    assert body[0].split("\t")[0] == "date"

    # A sequence of pairs is accepted alongside a dict.
    av.write_backtest_pnl_tsv(run, list(meta.items()), str(tmp_path / "pairs.tsv"))
    assert (tmp_path / "pairs.tsv").read_bytes() == path.read_bytes()


# ── Config defaults are INHERITED from the engine, never re-declared here ────

def test_run_config_defaults_mirror_the_engine_header():
    """`RunConfig()` must be exactly the C++ `RunConfig{}`, whatever that is.

    The binding is `py::init<>()` — a passthrough — and that is the deliberate
    choice: the Python layer must never be silently *more permissive* than the
    engine, and a Python-side override would be a second source of truth that
    drifts from `backtest.hpp` the first time a default moves.

    The consequence is that an engine-side default flip changes the Python
    contract too. This test is the tripwire that makes that explicit: when a
    default legitimately moves in `backtest.hpp`, update the expectation HERE and
    the README's backtest section in the SAME commit, so the new Python behaviour
    is reviewed rather than discovered as an exception at a caller's call site.

    Pinned against `backtest.hpp` RunConfig as of the WS-F merge, which is the
    first time this tripwire actually fired: WS-F's F1 leak family flipped the
    engine default from EXCLUDE_AND_REPORT to ERROR so that an unpriced lot fails
    closed instead of quietly leaving the book. Python inherits that, by the rule
    above, rather than staying more permissive than the engine.
    """
    cfg = av.RunConfig()
    assert cfg.unpriced == av.UnpricedLotPolicy.ERROR
    assert cfg.record_every_n == 1
    assert cfg.prefetch_snapshots is True
    assert cfg.settlement_mark_memo is True
    assert cfg.query_pricing_tier == av.QueryPricingTier.LEGACY_COMPATIBLE
    assert cfg.query_cache_build_policy == av.QueryCacheBuildPolicy.EAGER
    assert cfg.surface_provenance_policy == av.SurfaceProvenancePolicy.COMPATIBILITY


def test_run_config_prefetch_depth_round_trips():
    cfg = av.RunConfig()
    # 2 since S6-T32 measured the depth ladder on the 135-session projected replay
    # (1 -> 2 was +15.2% median and won 11/12 interleaved rounds; 2 -> 4 and 4 -> 8 are
    # washes). `prefetch_depth` is not a constructor kwarg, so this reads the C++
    # default straight through the `def_readwrite` binding — which is why the assertion
    # is worth keeping: it is the one place a silent drift in that default is caught on
    # the Python side. Output is bit-identical at any depth, so no other test moves.
    assert cfg.prefetch_depth == 2
    cfg.prefetch_depth = 7
    assert cfg.prefetch_depth == 7


# Main's compact binding-plumbing gates remain in addition to the feature
# branch's economic parity suite.
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
    assert len(run) == NUM_DATES
    columns = run.to_dict()
    assert len(columns) >= 28
    assert len(columns["nav"]) == NUM_DATES
    assert math.isfinite(float(run.nav[-1]))

    sheet = av.tearsheet(run)
    assert sheet.total_return == pytest.approx(run.nav[-1], abs=1e-12)

    path = tmp_path / "pnl_track.tsv"
    av.write_backtest_pnl_tsv(
        run, {"strategy": "example", "n_steps": str(len(run))}, str(path)
    )
    lines = path.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "# strategy=example"
    body = [line for line in lines if not line.startswith("#")]
    assert len(body) == len(run) + 1


def test_invalid_config_surfaces_as_atxerror():
    cfg = d4_config()
    cfg.target_abs_delta = 1.5
    with pytest.raises(av.AtxError):
        av.make_dispersion_strangle_spec(cfg)


def test_session_snap_fields_round_trip():
    """The three session-snap fields are read/write from Python and default off."""
    tenor = av.TenorSpec()
    assert not tenor.snap_to_sessions
    tenor.snap_to_sessions = True
    assert tenor.snap_to_sessions

    spec = av.StrategySpec()
    assert list(spec.session_ts) == []
    sessions = [BASE_TS, BASE_TS + DAY_NS, BASE_TS + 3 * DAY_NS]
    spec.session_ts = sessions
    assert list(spec.session_ts) == sessions

    cfg = d4_config()
    assert not cfg.snap_expiry_to_sessions
    cfg.snap_expiry_to_sessions = True
    assert cfg.snap_expiry_to_sessions


def test_snap_expiry_propagates_onto_every_leg():
    """The builder copies the flag onto every leg and stays corpus-agnostic."""
    off = av.make_dispersion_strangle_spec(d4_config())
    assert not any(leg.tenor.snap_to_sessions for leg in off.legs)

    cfg = d4_config()
    cfg.snap_expiry_to_sessions = True
    on = av.make_dispersion_strangle_spec(cfg)
    assert len(on.legs) == len(NAMES) + 1
    assert all(leg.tenor.snap_to_sessions for leg in on.legs)
    # session_ts is the CALLER's job: the builder never invents a calendar.
    assert list(on.session_ts) == []


def test_skip_entry_on_missing_index_round_trips_and_propagates():
    """The entry-skip opt-in is read/write, defaults off, and reaches the spec."""
    spec = av.StrategySpec()
    assert not spec.skip_entry_on_missing_index
    spec.skip_entry_on_missing_index = True
    assert spec.skip_entry_on_missing_index

    cfg = d4_config()
    assert not cfg.skip_entry_on_missing_index
    assert not av.make_dispersion_strangle_spec(cfg).skip_entry_on_missing_index

    cfg.skip_entry_on_missing_index = True
    assert cfg.skip_entry_on_missing_index
    assert av.make_dispersion_strangle_spec(cfg).skip_entry_on_missing_index
