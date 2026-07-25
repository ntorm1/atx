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

import numpy as np
import pytest

import atxvol as av


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
    # fit / value_chain are pure C++ over const state, so they must not hold the
    # interpreter — the pattern run_backtest already proves. Same probe shape as
    # the AloPricer GIL test, inverted: here the release is the contract.
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


def test_fit_rejects_a_chain_it_did_not_fit(panel, fitted):
    _chain, fitter = fitted
    other = av.OptionChain.from_frame(panel.frame, panel.env)
    with pytest.raises(av.AtxError) as excinfo:
        fitter.value_chain(other, av.OutputField.MODEL_IV, n_threads=1)
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT
