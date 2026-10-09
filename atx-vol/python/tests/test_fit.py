"""End-to-end gate for the fit front-end (PY-F / Y2).

The umbrella's blessed path is chain -> fit -> priced surface -> archive -> book
(`vol.hpp`). Before this, Python could only enter it half-way: a hand-authored
parametric `PricedSurface`. A quant could not fit a surface from quotes at all,
which is the product's actual pitch. This module drives the front half entirely
from Python:

    QuoteFrame -> OptionChain.from_frame -> PricerFitter.fit
      -> PricerFitter.value_chain(ModelIV | Greeks)      (numpy SoA)
      -> FittedSurface.to_priced_surface()               (into the existing API)

No external data: the board is the deterministic known-truth synthetic SPY panel
the C++ lifecycle test uses, so this runs everywhere.
"""

from __future__ import annotations

import math
import os
import subprocess
import sys
import textwrap

import numpy as np
import pytest

import atxvol as av

# Directory that `import atxvol` resolved to, so a child interpreter exercises
# THIS package rather than whatever an editable install would find first.
_SRC = os.path.dirname(os.path.dirname(os.path.abspath(av.__file__)))

_PREAMBLE = """
import sys
sys.path.insert(0, {src!r})
# A scikit-build-core editable install registers a meta-path finder that
# outranks sys.path; drop it so the assert below can actually bind.
sys.meta_path[:] = [f for f in sys.meta_path
                   if "ScikitBuild" not in type(f).__name__]
import faulthandler
faulthandler.enable()
import atxvol as av
assert av.__file__.startswith({src!r}), av.__file__
assert av._core.__file__.startswith({src!r}), av._core.__file__


def _fitter():
    panel = av.make_spy_synthetic_panel()
    chain = av.OptionChain.from_frame(panel.frame, panel.env)
    cfg = av.PricerConfig()
    cfg.preset = av.FitPreset.FAST
    cfg.curve_kind = av.VolCurveKind.CONVEX_DENSE
    cfg.n_threads = 1
    f = av.PricerFitter(cfg)
    f.fit(chain)
    return chain, f
"""


def _in_fresh_interpreter(body: str) -> subprocess.CompletedProcess:
    """Run `body` in a brand-new interpreter and hand back the result.

    A use-after-free does not raise — it takes the process down with an access
    violation. Driving it out-of-process turns that into an exit code this
    module can assert on, instead of killing the whole pytest session.
    """
    script = _PREAMBLE.format(src=_SRC) + textwrap.dedent(body)
    return subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True, text=True, timeout=900,
    )


@pytest.fixture(scope="module")
def panel():
    return av.make_spy_synthetic_panel()


@pytest.fixture(scope="module")
def fitted(panel):
    chain = av.OptionChain.from_frame(panel.frame, panel.env)
    cfg = av.PricerConfig()
    cfg.preset = av.FitPreset.FAST
    cfg.curve_kind = av.VolCurveKind.CONVEX_DENSE
    cfg.n_threads = 1
    fitter = av.PricerFitter(cfg)
    fitter.fit(chain)
    return chain, fitter


# ── Ingestion ───────────────────────────────────────────────────────────────

def test_synthetic_panel_installs_a_chain(panel):
    chain = av.OptionChain.from_frame(panel.frame, panel.env)
    assert chain.size() == len(panel.frame)
    assert chain.spot == pytest.approx(panel.env.spot)
    assert chain.now_ns == panel.env.now_ns
    ids = chain.ids()
    assert len(ids) == chain.size()
    # ids() is a stable deterministic order, independent of quote content.
    assert list(chain.ids()) == list(ids)

    snap = chain.snapshot()
    assert set(snap) >= {"ids", "T", "strike", "bid", "ask", "mid", "side"}
    assert len(snap["strike"]) == chain.size()
    assert np.all(snap["T"] > 0.0)
    assert np.all(snap["ask"] >= snap["bid"])


def test_quote_frame_from_arrays_builds_a_fittable_chain():
    # The product entry point: a caller's own numpy columns, no C++ fixture.
    strikes = np.arange(80.0, 121.0, 5.0)
    expiries = ["2026-09-18", "2026-12-18"]
    rows_k, rows_e, rows_side, bids, asks = [], [], [], [], []
    spot, r, sigma = 100.0, 0.03, 0.22
    now = av.iso_to_ns("2026-06-19")
    for e in expiries:
        t = (av.iso_to_ns(e) - now) / (365.25 * 86_400 * 1e9)
        for k in strikes:
            for side in (av.Side.CALL, av.Side.PUT):
                mid = av.american_price(spot, float(k), t, sigma, r, 0.0, side)
                hw = max(0.02, 0.01 * mid)
                rows_k.append(k)
                rows_e.append(e)
                rows_side.append(int(side))
                bids.append(max(0.0, mid - hw))
                asks.append(mid + hw)

    frame = av.QuoteFrame.from_arrays(
        uid="TEST",
        snapshot_iso="2026-06-19",
        spot=spot,
        rate=r,
        expiry_iso=rows_e,
        strike=np.array(rows_k),
        side=np.array(rows_side, dtype=np.int32),
        bid=np.array(bids),
        ask=np.array(asks),
    )
    assert len(frame) == len(rows_k)

    chain = av.OptionChain.from_frame(frame, av.MarketEnv.flat(spot, r, now))
    assert chain.size() == len(rows_k)
    assert chain.spot == pytest.approx(spot)


def test_term_yield_curve_and_nondefault_curve_config_reach_the_fit(panel):
    # F-3: this is the C++ MarketEnv/CurveConfig contract, not a Python-side
    # approximation. Pillars are validated by YieldCurve::create and the exact
    # nested CurveConfig is handed to PricerFitter.
    curve = av.YieldCurve.create(
        [0.05, 0.25, 0.75, 2.0],
        [0.028, 0.031, 0.034, 0.039],
    )
    assert len(curve) == 4
    assert curve.zero(0.25) == pytest.approx(0.031, abs=1.0e-15)
    assert curve.zero(0.75) == pytest.approx(0.034, abs=1.0e-15)
    assert curve.disc(0.75) == pytest.approx(math.exp(-0.034 * 0.75))

    env = av.MarketEnv.flat(
        panel.env.spot, 0.99, panel.env.now_ns, panel.env.cash_divs
    )
    env.yield_curve = curve
    assert env.rate_at(0.25) == pytest.approx(0.031, abs=1.0e-15)
    assert env.rate_at(0.75) == pytest.approx(0.034, abs=1.0e-15)
    assert env.rate_at(0.25) != env.flat_rate

    curve_cfg = av.CurveConfig()
    curve_cfg.kind = av.VolCurveKind.CONVEX_DENSE
    curve_cfg.convex.lambda_ = 2.5e-3
    curve_cfg.convex.node_cap = 32
    curve_cfg.convex.max_iter = 175
    curve_cfg.convex.bound_slope_below = True
    curve_cfg.parametric.huber_k = 1.25
    curve_cfg.parametric.max_obs_per_slice = 48
    curve_cfg.parametric.optimization_level = av.OptimizationLevel.REFERENCE
    curve_cfg.spline.lambda_ = 0.03
    curve_cfg.spline.mult_ceil = 2.75

    cfg = av.PricerConfig()
    cfg.preset = av.FitPreset.FAST
    cfg.curve = curve_cfg
    cfg.n_threads = 1
    cfg.fit_workers = 1
    assert cfg.curve.kind == av.VolCurveKind.CONVEX_DENSE
    assert cfg.curve.convex.node_cap == 32
    assert cfg.curve.parametric.max_obs_per_slice == 48
    assert cfg.curve.spline.mult_ceil == pytest.approx(2.75)

    chain = av.OptionChain.from_frame(panel.frame, env)
    fitter = av.PricerFitter(cfg)
    fitter.fit(chain)
    valued = fitter.value_chain(chain, av.OutputField.MODEL_IV, n_threads=1)
    assert np.isfinite(valued["model_iv"]).all()


@pytest.mark.parametrize(
    ("pillars", "rates"),
    [
        ([], []),
        ([0.5, 0.25], [0.03, 0.04]),
        ([0.25], [0.03, 0.04]),
        ([0.25, float("nan")], [0.03, 0.04]),
        ([0.25, 0.5], [0.03, float("inf")]),
        ([0.0, 0.5], [0.03, 0.04]),
    ],
)
def test_yield_curve_rejects_invalid_pillars(pillars, rates):
    with pytest.raises(av.AtxError) as excinfo:
        av.YieldCurve.create(pillars, rates)
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT


def test_quote_frame_from_arrays_rejects_an_unrecognised_side_code():
    # I2 (rev-ws-y), the ingestion copy of the same decode. This one is the worst
    # of the three: a whole board imported with the +1/-1 convention would be
    # INSTALLED with every leg as a call, and every number downstream — fit,
    # value_chain, the priced surface — would be silently wrong.
    n = 6
    with pytest.raises(av.AtxError) as excinfo:
        av.QuoteFrame.from_arrays(
            uid="TEST",
            snapshot_iso="2026-06-19",
            spot=100.0,
            rate=0.03,
            expiry_iso=["2026-09-18"] * n,
            strike=np.linspace(90.0, 110.0, n),
            side=np.full(n, -1, dtype=np.int32),
            bid=np.full(n, 1.0),
            ask=np.full(n, 1.1),
        )
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT


def test_option_chain_update_quotes_is_visible_in_the_snapshot(panel):
    chain = av.OptionChain.from_frame(panel.frame, panel.env)
    ids = chain.ids()[:4]
    before = chain.snapshot()
    chain.update_quotes(ids, np.full(len(ids), 1.25), np.full(len(ids), 1.75))
    after = chain.snapshot()

    rows = [i for i, cid in enumerate(after["ids"]) if cid in set(ids)]
    assert len(rows) == len(ids)
    for i in rows:
        assert after["bid"][i] == pytest.approx(1.25)
        assert after["ask"][i] == pytest.approx(1.75)
        assert after["mid"][i] == pytest.approx(1.50)
    assert after["quote_revision"] if "quote_revision" in after else True
    # Untouched rows are unchanged.
    untouched = [i for i in range(len(after["ids"])) if i not in set(rows)]
    assert untouched
    np.testing.assert_array_equal(after["bid"][untouched], before["bid"][untouched])


# ── Fit ─────────────────────────────────────────────────────────────────────

def test_fit_produces_a_served_surface(fitted):
    _chain, fitter = fitted
    assert fitter.fitted
    surface = fitter.surface()
    assert surface is not None
    assert surface.generation >= 1


def test_value_chain_columns_match_the_librarys_own_scalar_serve(fitted):
    # The C++ golden: `value_chain` is a fanned-out batch over the SAME session
    # the scalar accessors serve, so every populated cell must equal the scalar
    # answer. This is what makes the numpy path trustworthy rather than merely
    # fast — a divergence here means the batch route is not the served route.
    chain, fitter = fitted
    fields = av.OutputField.MODEL_IV | av.OutputField.MODEL_PRICE | av.OutputField.GREEKS
    val = fitter.value_chain(chain, fields, n_threads=1)
    snap = chain.snapshot()

    n = chain.size()
    assert len(val["ids"]) == n
    np.testing.assert_array_equal(val["ids"], snap["ids"])
    for column in ("model_iv", "model_price", "delta", "gamma", "vega", "theta"):
        assert len(val[column]) == n

    surface = fitter.surface()
    checked = 0
    for i in range(0, n, max(1, n // 40)):
        k = float(snap["strike"][i])
        t = float(snap["T"][i])
        side = av.Side.CALL if snap["side"][i] == int(av.Side.CALL) else av.Side.PUT
        if not math.isfinite(val["model_iv"][i]):
            continue
        assert val["model_iv"][i] == surface.iv(k, t)
        assert val["model_price"][i] == surface.fair_value(k, t, side)
        assert val["delta"][i] == surface.greeks(k, t, side).delta
        checked += 1
    assert checked >= 10


def test_value_chain_only_fills_the_requested_fields(fitted):
    chain, fitter = fitted
    val = fitter.value_chain(chain, av.OutputField.MODEL_IV, n_threads=1)
    assert len(val["model_iv"]) == chain.size()
    assert len(val["model_price"]) == 0
    assert len(val["delta"]) == 0


def test_value_chain_is_bit_identical_across_n_threads(fitted):
    # `PricerFitter::value_chain` documents "DETERMINISTIC: the result is
    # bit-identical for any thread count (disjoint output slots, pure const
    # reads)". Driving that from Python is the point: a binding that fanned out
    # through its own pool, or that let the GIL reorder accumulation, would
    # break it invisibly.
    chain, fitter = fitted
    fields = av.OutputField.MODEL_IV | av.OutputField.MODEL_PRICE | av.OutputField.GREEKS
    base = fitter.value_chain(chain, fields, n_threads=1)
    columns = [k for k, v in base.items() if isinstance(v, np.ndarray)]
    assert "model_iv" in columns and "vega" in columns
    for workers in (2, 4, 8):
        other = fitter.value_chain(chain, fields, n_threads=workers)
        for column in columns:
            # tobytes(), not allclose: the contract is BIT-identity, and a
            # tolerance here would hide exactly the reordering it exists to catch.
            assert base[column].tobytes() == other[column].tobytes(), (
                f"column {column!r} differs between n_threads=1 and {workers}"
            )
        # The scalar diagnostics fan out the same way.
        for key in ("filled", "n_bid_unset", "n_ask_unset", "n_bid_iv_fail", "n_ask_iv_fail"):
            assert base[key] == other[key]


def test_value_chain_releases_the_gil(fitted):
    # `value_chain` is const, internally parallel and genuinely long, so it must
    # not hold the interpreter — the pattern run_backtest already proves. Same
    # probe shape as the AloPricer GIL test, inverted: here the release is the
    # contract. `fit` releases it too (see test_fit_releases_the_gil); what makes
    # that safe is the binding's reader/writer lock, not the GIL.
    import threading

    chain, fitter = fitted
    counter = 0
    stop = threading.Event()

    def spin() -> None:
        nonlocal counter
        while not stop.is_set():
            counter += 1

    spinner = threading.Thread(target=spin, daemon=True)
    spinner.start()
    try:
        start = counter
        while counter == start:
            pass
        before = counter
        fitter.value_chain(chain, av.OutputField.All, n_threads=1)
        advanced = counter - before
    finally:
        stop.set()
        spinner.join(timeout=5.0)

    assert advanced > 0


# ── Hand-off into the existing surface API ──────────────────────────────────

def test_to_priced_surface_hands_the_fit_to_the_priced_surface_api(fitted):
    chain, fitter = fitted
    priced = fitter.surface().to_priced_surface()
    assert priced.n_slices >= 1

    snap = chain.snapshot()
    i = int(np.argmin(np.abs(snap["strike"] - chain.spot)))
    k = float(snap["strike"][i])
    t = float(snap["T"][i])
    # The sealed snapshot serves the same IV the live session does.
    assert priced.iv(k, t) == pytest.approx(fitter.surface().iv(k, t), rel=1e-9)


def test_fit_releases_the_gil(fitted):
    # The other half of I1's contract. `fit` is the longest call in this module,
    # so holding the GIL for its whole duration would freeze every unrelated
    # Python thread in the process. It releases — and is safe while released
    # because the binding takes a WRITER lock, not because the GIL is helping.
    import threading

    chain, fitter = fitted
    counter = 0
    stop = threading.Event()

    def spin() -> None:
        nonlocal counter
        while not stop.is_set():
            counter += 1

    spinner = threading.Thread(target=spin, daemon=True)
    spinner.start()
    try:
        start = counter
        while counter == start:
            pass
        before = counter
        fitter.fit(chain)
        advanced = counter - before
    finally:
        stop.set()
        spinner.join(timeout=5.0)

    assert advanced > 0


def test_concurrent_fit_and_value_chain_on_one_fitter_is_safe():
    # I1 (rev-ws-y): `pricer_fitter.hpp` is explicit that "`fit` mutates (stores
    # the surface) and needs exclusive access", while `value_chain` is const and
    # safe to call concurrently. Both bindings release the GIL, so the GIL cannot
    # be the thing providing that exclusivity — `fit()` reassigning the
    # non-atomic `shared_ptr market_mark_surface_` while `value_chain()` copies
    # it is a data race whose failure mode is refcount corruption.
    #
    # Out-of-process, because the failure is an access violation rather than an
    # exception. Pre-fix this crashed 3 runs in 4 at eight rounds; the writer/
    # reader lock makes it deterministic.
    proc = _in_fresh_interpreter(
        """
        import threading

        chain, f = _fitter()
        rounds = 40
        errors = []

        def refit():
            for _ in range(rounds):
                try:
                    f.fit(chain)
                except Exception as exc:
                    errors.append(repr(exc))

        def price():
            for _ in range(rounds):
                try:
                    f.value_chain(chain, av.OutputField.MODEL_IV, n_threads=1)
                    f.surface().iv(600.0, 0.25)
                except Exception as exc:
                    errors.append(repr(exc))

        threads = [threading.Thread(target=refit)]
        threads += [threading.Thread(target=price) for _ in range(3)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert not errors, errors[:3]
        assert f.fitted
        print("SURVIVED", rounds)
        """
    )
    assert proc.returncode == 0, (
        f"concurrent fit/value_chain on one fitter: exit {proc.returncode}\n"
        f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr[:4000]}"
    )
    assert "SURVIVED" in proc.stdout, proc.stdout


def test_concurrent_quote_mutation_is_atomic_for_fit_and_value_chain():
    # C-7: OptionChain itself has a separate "many readers OR one writer"
    # contract. The fitter lock above cannot protect it: update_quotes does not
    # touch the fitter, while fit/value_chain both release the GIL and read the
    # chain's quote vectors and revision state. Drive both reader paths in a
    # child process because the pre-fix failure includes native crashes.
    #
    # The value phase has a stronger oracle than mere survival. Each update
    # atomically installs one of two whole-board states, so every valuation must
    # exactly match one of the two sequential reference valuations. A mixture is
    # a torn read. The fit phase then overlaps the same batch writer with the
    # longer chain reader and checks publication remains usable.
    proc = _in_fresh_interpreter(
        """
        import threading
        import numpy as np

        chain, f = _fitter()
        snap = chain.snapshot()
        ids = snap["ids"]
        bid_a = snap["bid"].copy()
        ask_a = snap["ask"].copy()
        spread = ask_a - bid_a
        bid_b = bid_a + 0.25 * spread
        ask_b = ask_a - 0.25 * spread
        assert np.any(bid_a != bid_b)

        fields = av.OutputField.BID_IV | av.OutputField.ASK_IV
        chain.update_quotes(ids, bid_a, ask_a)
        expected_a = f.value_chain(chain, fields, n_threads=1)
        chain.update_quotes(ids, bid_b, ask_b)
        expected_b = f.value_chain(chain, fields, n_threads=1)

        def matches(actual, expected):
            return (
                np.array_equal(actual["bid_iv"], expected["bid_iv"], equal_nan=True)
                and np.array_equal(actual["ask_iv"], expected["ask_iv"], equal_nan=True)
            )

        assert not (
            np.array_equal(
                expected_a["bid_iv"], expected_b["bid_iv"], equal_nan=True
            )
            and np.array_equal(
                expected_a["ask_iv"], expected_b["ask_iv"], equal_nan=True
            )
        ), "the two quote states need distinct valuation signatures"

        value_errors = []
        value_start = threading.Barrier(4)
        value_revision = chain.quote_revision
        update_rounds = 2000

        def update_for_values():
            try:
                value_start.wait()
                for i in range(update_rounds):
                    state = (bid_a, ask_a) if i % 2 == 0 else (bid_b, ask_b)
                    chain.update_quotes(ids, *state)
            except Exception as exc:
                value_errors.append(("update", repr(exc)))

        def value():
            try:
                value_start.wait()
                for _ in range(20):
                    actual = f.value_chain(chain, fields, n_threads=1)
                    if not (matches(actual, expected_a) or matches(actual, expected_b)):
                        value_errors.append(("torn valuation",))
                        return
            except Exception as exc:
                value_errors.append(("value", repr(exc)))

        writer = threading.Thread(target=update_for_values)
        readers = [threading.Thread(target=value) for _ in range(3)]
        writer.start()
        for thread in readers:
            thread.start()
        for thread in [writer, *readers]:
            thread.join(timeout=180)
        assert not any(thread.is_alive() for thread in [writer, *readers]), (
            "value/update lock deadlock"
        )
        assert not value_errors, value_errors[:3]
        assert chain.quote_revision == value_revision + update_rounds

        # Exercise the longer reader independently. Both states are valid
        # synthetic markets, so no fit exception is expected at either atomic
        # boundary; a torn state or native race is the only source of failure.
        chain.update_quotes(ids, bid_a, ask_a)
        f.fit(chain)
        chain.update_quotes(ids, bid_b, ask_b)
        f.fit(chain)
        fit_errors = []
        fit_start = threading.Barrier(2)
        fit_revision = chain.quote_revision
        fit_update_rounds = 1000

        def update_for_fits():
            try:
                fit_start.wait()
                for i in range(fit_update_rounds):
                    state = (bid_a, ask_a) if i % 2 == 0 else (bid_b, ask_b)
                    chain.update_quotes(ids, *state)
            except Exception as exc:
                fit_errors.append(("update", repr(exc)))

        def refit():
            try:
                fit_start.wait()
                for _ in range(20):
                    f.fit(chain)
            except Exception as exc:
                fit_errors.append(("fit", repr(exc)))

        fit_writer = threading.Thread(target=update_for_fits)
        fit_reader = threading.Thread(target=refit)
        fit_writer.start()
        fit_reader.start()
        for thread in (fit_writer, fit_reader):
            thread.join(timeout=180)
        assert not fit_writer.is_alive() and not fit_reader.is_alive(), (
            "fit/update lock deadlock"
        )
        assert not fit_errors, fit_errors[:3]
        assert chain.quote_revision == fit_revision + fit_update_rounds

        chain.update_quotes(ids, bid_a, ask_a)
        f.fit(chain)
        final = f.value_chain(chain, av.OutputField.MODEL_IV, n_threads=1)
        assert len(final["model_iv"]) == len(ids)
        assert np.any(np.isfinite(final["model_iv"]))
        print("SURVIVED", update_rounds, fit_update_rounds)
        """
    )
    assert proc.returncode == 0, (
        f"concurrent update/fit/value_chain: exit {proc.returncode}\n"
        f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr[:4000]}"
    )
    assert "SURVIVED" in proc.stdout, proc.stdout


def test_surface_handle_survives_a_refit():
    # C1 (rev-ws-y): `PricerFitter` owns its surfaces as
    # `shared_ptr<const FittedSurface>` and `fit()` REPLACES the stored
    # generation. A binding that hands Python a raw pointer under
    # `reference_internal` keeps the FITTER alive and nothing at all keeps the
    # GENERATION alive, so the second fit frees the object a live Python handle
    # points at. Four lines, and the interpreter dies with 0xC0000005:
    #
    #     f.fit(c); s = f.surface(); f.fit(c); s.iv(...)
    #
    # `keep_alive` cannot fix it — the fitter legitimately outlives the
    # generation — so the handle has to be a co-owner, not an observer.
    proc = _in_fresh_interpreter(
        """
        chain, f = _fitter()
        s = f.surface()
        before = s.iv(600.0, 0.25)
        f.fit(chain)                 # replaces the fitter's shared_ptr generation
        after = s.iv(600.0, 0.25)    # <- access violation while `surface()` observes
        assert before == after, (before, after)
        assert after == after, "surface served NaN"
        # The old handle keeps its OWN generation alive and serving, side by side
        # with the fitter's new one.
        fresh = f.surface()
        assert fresh.iv(600.0, 0.25) == after
        print("SURVIVED", before, after)
        """
    )
    assert proc.returncode == 0, (
        f"refit invalidated the live surface handle: exit {proc.returncode}\n"
        f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
    )
    assert "SURVIVED" in proc.stdout, proc.stdout


def test_surface_outlives_the_fitter_that_produced_it():
    # The other half of the same ownership question: dropping the fitter must
    # not invalidate a surface a caller still holds.
    proc = _in_fresh_interpreter(
        """
        chain, f = _fitter()
        s = f.surface()
        expected = s.iv(600.0, 0.25)
        del f
        import gc; gc.collect()
        assert s.iv(600.0, 0.25) == expected
        print("SURVIVED", expected)
        """
    )
    assert proc.returncode == 0, (
        f"dropping the fitter invalidated the surface: exit {proc.returncode}\n"
        f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
    )
    assert "SURVIVED" in proc.stdout, proc.stdout


def test_fit_rejects_a_chain_it_did_not_fit(panel, fitted):
    _chain, fitter = fitted
    other = av.OptionChain.from_frame(panel.frame, panel.env)
    with pytest.raises(av.AtxError) as excinfo:
        fitter.value_chain(other, av.OutputField.MODEL_IV, n_threads=1)
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT
