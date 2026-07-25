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
