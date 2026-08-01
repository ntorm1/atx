"""Python parity gate for the surface-db dispersion route (sprint Task 7).

WHAT THIS IS THE FIRST TEST OF. The C++ side composes
``SurfaceDb -> Clock::from_surface_db -> Clock::between -> dispersion backtest``
in ``atx-vol/tests/surface_db_dispersion_backtest_test.cpp`` and in the
``surface_db_dispersion_backtest`` example CLI. The Python layer already bound
both ends of that chain — ``SurfaceDb`` (``bindings/surface_db.cpp``),
``Clock.from_surface_db`` and ``run_dispersion_backtest``
(``bindings/backtest.cpp`` / ``bindings/dispersion.cpp``) — but nothing anywhere
ran them *together*, and the window subset in the middle
(``Clock::between``) was not bound at all. A binding that exists is not a
binding that composes: the shapes each side hands the other (a `Clock` returned
by value from a staticmethod, a `DispersionUniverse` built field-by-field, a
`DispersionBacktestConfig` whose nested `run` is reached through a reference
property) are only actually exercised when one call's output feeds the next.

WHY THE FIXTURE IS SYNTHETIC AND LOCAL. Everything under ``C:\\atx-data`` is
read-only and the sprint budget forbids provider calls, so the corpus is built
here from ``atxvol`` itself: eSSVI ``PricedSurface``s written into a fresh
``SurfaceDb`` under pytest's ``tmp_path``. It deliberately mirrors
``surface_db_dispersion_backtest_test.cpp``'s ``make_test_db`` — the same
7-slice T grid, the same ``100 * (i+1)`` spot ladder, the same per-symbol vol
bump, the same one-day ``now_ts_ns`` step — so a divergence between the two
suites is a binding defect and not a fixture difference.

WHY SIX PARTITIONS FOR A FOUR-SESSION RUN. ``kRunDates`` in the C++ suite is
six BUSINESS days, 2026-01-05 (Mon) .. 2026-01-12 (Mon), so the weekend of the
10th/11th is simply absent. The run window ``[2026-01-06, 2026-01-09]`` is
strictly inside it on BOTH ends: a ``between`` that silently returned the whole
clock, or that dropped an endpoint, changes the row count, so the assertion on
``len(result.date)`` has teeth it would not have on a window equal to the
corpus.

NO ``pyarrow`` IN THIS MODULE, EVER. ``atxvol._core`` and ``pyarrow.lib`` both
link vcpkg's ``arrow.dll``/``parquet.dll`` by base name and the first loaded
claims the process-wide slot on Windows, so importing both here would break the
extension import for the whole file (see ``atxvol/__init__.py``'s ImportError
note and the README section it cites). Nothing in this module needs it.
"""

from __future__ import annotations

import math

import pytest

import atxvol as av

# ── Fixture corpus, mirroring surface_db_dispersion_backtest_test.cpp ────────

K_R = 0.043
DAY_NS = 86_400_000_000_000
BASE_TS = 1_700_000_000_000_000_000
SLICE_TS = [0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00]

INDEX_SYM = "SPY"
NAMES = ["NM0", "NM1", "NM2"]
SYMBOLS = [INDEX_SYM, *NAMES]

# Six business days; the weekend of the 10th/11th is absent, exactly as a real
# db's partition set is.
DATES = ["2026-01-05", "2026-01-06", "2026-01-07",
         "2026-01-08", "2026-01-09", "2026-01-12"]
WINDOW_LO, WINDOW_HI = "2026-01-06", "2026-01-09"
WINDOW_DATES = ["2026-01-06", "2026-01-07", "2026-01-08", "2026-01-09"]


def make_surface(spot: float, now_ts: int, vol_bump: float, uid: int) -> av.PricedSurface:
    """A synthetic eSSVI surface: flat forward == spot, genuine American premium
    via ``q_eff = 0.02``, 7 slices over T in [0.05, 1.0].

    The ATM-forward straddles the dispersion strategy projects sit at ~30d DTE
    (``DispersionBacktestConfig.target_dte_days`` defaults to 30, i.e. T ~ 0.082),
    which is inside this grid — a shorter grid would leave the strike resolver
    with nothing to interpolate and the book would never open.
    """
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
    """One partition per date, each carrying every symbol.

    Spots are the C++ fixture's ``100 * (i + 1) * (1 + 0.002 * d)`` ladder and
    the vol bump is ``0.01 * i``, so no two names are degenerate and the basket
    genuinely disperses against the index.
    """
    db = av.SurfaceDb.create(str(root))
    for d, date in enumerate(DATES):
        ts = BASE_TS + d * DAY_NS
        items = [
            (symbol,
             make_surface(100.0 * (i + 1) * (1.0 + 0.002 * d), ts, 0.01 * i, i + 1))
            for i, symbol in enumerate(SYMBOLS)
        ]
        db.write_partition(date, items)


@pytest.fixture(scope="module")
def synthetic_db_root(tmp_path_factory):
    root = tmp_path_factory.mktemp("atxvol_disp_db") / "db"
    build_fixture_db(root)
    return root


def equal_weight_universe() -> av.DispersionUniverse:
    """The bound constructor shape is ``py::init<>()`` + ``def_readwrite`` on both
    ``DispersionUniverse`` and ``DispersionMember`` (``bindings/dispersion.cpp``),
    so members are built by FIELD ASSIGNMENT, not by kwargs.

    ``uid`` stays 0 on purpose: uids are snapshot-local and are rebound on every
    step by the engine's ``resolve_universe_uids`` via ``MarketSnapshot::uid_of``,
    which is the same contract ``universe_from_surface_db`` produces on the C++
    side.
    """
    index = av.DispersionMember()
    index.symbol = INDEX_SYM
    index.uid = 0
    index.weight = 1.0

    members = []
    for name in NAMES:
        member = av.DispersionMember()
        member.symbol = name
        member.uid = 0
        member.weight = 1.0 / len(NAMES)
        members.append(member)

    universe = av.DispersionUniverse()
    universe.index = index
    universe.names = members
    return universe


def dispersion_config() -> av.DispersionBacktestConfig:
    cfg = av.DispersionBacktestConfig()
    cfg.min_names = 2          # three names, so the floor is genuinely cleared
    cfg.entry_every_n = 1
    cfg.run.price.n_threads = 1  # correctness gate; thread-identity is pinned in C++
    return cfg


# ── The composition ─────────────────────────────────────────────────────────

def test_surface_db_clock_between_and_dispersion_run(synthetic_db_root):
    db = av.SurfaceDb.open(str(synthetic_db_root))
    clock = av.Clock.from_surface_db(db).between(WINDOW_LO, WINDOW_HI)

    # `between` is INCLUSIVE on both ends and carries the refs whole.
    assert len(clock) == len(WINDOW_DATES)
    assert [ref.date for ref in clock.refs] == WINDOW_DATES
    assert all(ref.archive_path.endswith(".atxvsa") for ref in clock.refs)

    result = av.run_dispersion_backtest(clock, equal_weight_universe(), dispersion_config())

    # EXACTLY the window's sessions: the corpus has six partitions, so seeing six
    # rows would mean `between` was a no-op.
    assert len(result.date) == len(WINDOW_DATES)
    assert list(result.date) == WINDOW_DATES
    assert all(math.isfinite(x) for x in result.nav)

    # TEETH. Every assertion above is satisfied by four rows of zeros — a run that
    # opened no book at all. What makes this a dispersion BACKTEST is that lots
    # were opened and the book carried vega.
    assert result.n_open_lots.max() > 0.0
    assert abs(result.gross_vega).max() > 0.0
    # Fail-closed is the engine default and this corpus never loses a name, so
    # nothing may be dropped as unpriced.
    assert av.RunConfig().unpriced == av.UnpricedLotPolicy.ERROR
    assert result.n_unpriced_lots.max() == 0.0


def test_clock_between_is_non_mutating_and_clamps(synthetic_db_root):
    db = av.SurfaceDb.open(str(synthetic_db_root))
    clock = av.Clock.from_surface_db(db)
    assert len(clock) == len(DATES)

    # Bounds outside the corpus CLAMP — an operator asking for "2020..2030" wants
    # everything there is, which is not an error.
    assert len(clock.between("2020-01-01", "2030-01-01")) == len(DATES)
    assert len(clock.between("2020-01-01", "2026-01-06")) == 2
    assert len(clock.between("2026-01-09", "2030-01-01")) == 2
    # A single-date window keeps exactly that date.
    one = clock.between("2026-01-12", "2026-01-12")
    assert [ref.date for ref in one.refs] == ["2026-01-12"]
    # Subsetting is non-mutating.
    assert len(clock) == len(DATES)


def test_clock_between_empty_window_raises(synthetic_db_root):
    db = av.SurfaceDb.open(str(synthetic_db_root))
    clock = av.Clock.from_surface_db(db)

    # A window with no partition in it, and an inverted one, are both
    # InvalidArgument — and the message names the AVAILABLE range so the caller
    # can self-serve the correction rather than dump the manifest.
    for lo, hi in (("2026-02-01", "2026-02-28"), ("2026-01-09", "2026-01-06")):
        with pytest.raises(av.AtxError) as excinfo:
            clock.between(lo, hi)
        assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT
        message = str(excinfo.value)
        assert DATES[0] in message, message
        assert DATES[-1] in message, message
