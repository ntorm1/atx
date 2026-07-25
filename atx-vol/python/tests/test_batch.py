"""Numpy-native batch American pricing / greeks / IV and surface grids (Y3).

The review's performance finding: "No vectorized path for anything American" —
the C++ SoA laned flagship (`american_batch.hpp`) and the strike-axis American IV
batch were entirely unbound, and `PricedSurface` queries were scalar-per-call, so
chain-scale valuation from Python was a Python for-loop over ~1-2 us of pybind
dispatch each.

Every gate here is a PARITY gate, not a timing one: a vectorized path is only
worth having if it returns exactly what the scalar path returns. All of these
follow the Y1(c) NaN + per-lane status convention (`atxvol.STATUS_OK`).
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import atxvol as av


# ── American price batch ────────────────────────────────────────────────────

def _book(n: int = 64):
    rng = np.random.default_rng(20260724)
    spot = np.full(n, 100.0)
    strike = np.linspace(70.0, 130.0, n)
    t = np.linspace(0.05, 2.0, n)
    sigma = 0.15 + 0.25 * rng.random(n)
    r = np.full(n, 0.043)
    q = np.full(n, 0.012)
    side = np.where(np.arange(n) % 2 == 0, int(av.Side.CALL), int(av.Side.PUT)).astype(np.int32)
    return spot, strike, t, sigma, r, q, side


def test_american_price_batch_matches_the_scalar_loop():
    spot, strike, t, sigma, r, q, side = _book()
    prices, status = av.american_price_batch(spot, strike, t, sigma, r, q, side,
                                             isa=av.SimdIsa.FORCE_SCALAR)

    assert len(prices) == len(strike)
    assert np.all(status == av.STATUS_OK)
    expected = np.array([
        av.american_price(spot[i], strike[i], t[i], sigma[i], r[i], q[i],
                          av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT)
        for i in range(len(strike))
    ])
    # Bit-identity, not a tolerance: the batch groups the early-exercise lanes
    # into one pack and patches everything else through the exact scalar
    # andersen_lake, so on the ForceScalar route it IS the scalar answer.
    assert prices.tobytes() == expected.tobytes()


def test_american_price_batch_rejects_a_shape_error():
    spot, strike, t, sigma, r, q, side = _book(8)
    with pytest.raises(ValueError):
        av.american_price_batch(spot, strike[:4], t, sigma, r, q, side)


def test_american_greeks_batch_matches_the_scalar_fd_loop():
    spot, strike, t, sigma, r, q, side = _book(32)
    got = av.american_greeks_batch(spot, strike, t, sigma, r, q, side,
                                   isa=av.SimdIsa.FORCE_SCALAR)

    assert np.all(got["status"] == av.STATUS_OK)
    for i in range(len(strike)):
        s = av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT
        ref = av.american_greeks_fd(spot[i], strike[i], t[i], sigma[i], r[i], q[i], s)
        # The FD route (analytic_greeks=False) fans the scalar american_greeks_fd
        # per lane, so this is exact.
        assert got["delta"][i] == ref.delta
        assert got["gamma"][i] == ref.gamma
        assert got["vega"][i] == ref.vega
        assert got["theta"][i] == ref.theta


def test_american_implied_vol_batch_reports_lane_status():
    # Strike-axis inversion, shared (S, T, r, q, side). One lane is handed a
    # price below intrinsic, which must NaN that lane and leave the rest intact.
    strikes = np.linspace(80.0, 120.0, 12)
    spot, t, r, q, sigma = 100.0, 0.75, 0.043, 0.0, 0.28
    prices = np.array([
        av.american_price(spot, float(k), t, sigma, r, q, av.Side.PUT) for k in strikes
    ])
    bad = 5
    prices[bad] = -1.0

    ivs, status = av.american_implied_vol_batch(prices, spot, strikes, t, r, q, av.Side.PUT)

    good = [i for i in range(len(strikes)) if i != bad]
    np.testing.assert_allclose(ivs[good], sigma, atol=2.0e-6, rtol=0.0)
    assert math.isnan(ivs[bad])
    assert all(int(status[i]) == av.STATUS_OK for i in good)
    assert int(status[bad]) != av.STATUS_OK


def test_american_implied_vol_batch_matches_the_scalar_inverter():
    strikes = np.linspace(85.0, 115.0, 8)
    spot, t, r, q, sigma = 100.0, 0.5, 0.03, 0.01, 0.22
    prices = np.array([
        av.american_price(spot, float(k), t, sigma, r, q, av.Side.CALL) for k in strikes
    ])
    ivs, status = av.american_implied_vol_batch(prices, spot, strikes, t, r, q, av.Side.CALL)
    assert np.all(status == av.STATUS_OK)
    expected = np.array([
        av.american_implied_vol(float(p), spot, float(k), t, r, q, av.Side.CALL)
        for p, k in zip(prices, strikes)
    ])
    assert ivs.tobytes() == expected.tobytes()


# ── PricedSurface grid ──────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def priced():
    panel = av.make_spy_synthetic_panel()
    chain = av.OptionChain.from_frame(panel.frame, panel.env)
    cfg = av.PricerConfig()
    cfg.preset = av.FitPreset.FAST
    cfg.curve_kind = av.VolCurveKind.CONVEX_DENSE
    cfg.n_threads = 1
    fitter = av.PricerFitter(cfg)
    fitter.fit(chain)
    return fitter.surface().to_priced_surface()


def _grid_points(priced):
    ctx = priced.context
    ks, ts, sides = [], [], []
    for slice_ctx in ctx:
        for m in (0.90, 0.95, 1.0, 1.05, 1.10):
            ks.append(slice_ctx.forward * m)
            ts.append(slice_ctx.T)
            sides.append(int(av.Side.PUT) if m < 1.0 else int(av.Side.CALL))
    return (np.array(ks), np.array(ts), np.array(sides, dtype=np.int32))


def test_priced_surface_grid_matches_the_scalar_queries(priced):
    k, t, side = _grid_points(priced)
    got = priced.grid(k, t, side)

    assert len(got["iv"]) == len(k)
    assert np.all(got["status"] == av.STATUS_OK)
    for i in range(len(k)):
        s = av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT
        assert got["iv"][i] == priced.iv(float(k[i]), float(t[i]))
        assert got["total_variance"][i] == priced.total_variance(float(k[i]), float(t[i]))
        assert got["fair_value"][i] == priced.fair_value(float(k[i]), float(t[i]), s)
        ref = priced.greeks(float(k[i]), float(t[i]), s)
        assert got["delta"][i] == ref.delta
        assert got["vega"][i] == ref.vega


def test_priced_surface_grid_nans_a_failing_point_instead_of_raising(priced):
    # A degenerate point (T <= 0) must NaN its own lane and carry a status, not
    # abort the grid — the Y1(c) convention, reused.
    k, t, side = _grid_points(priced)
    t = t.copy()
    t[2] = -1.0
    got = priced.grid(k, t, side)

    assert math.isnan(got["fair_value"][2])
    assert int(got["status"][2]) != av.STATUS_OK
    assert all(int(got["status"][i]) == av.STATUS_OK for i in range(len(k)) if i != 2)


def test_priced_surface_grid_rejects_a_shape_error(priced):
    k, t, side = _grid_points(priced)
    with pytest.raises(ValueError):
        priced.grid(k, t[:-1], side)


def test_priced_surface_grid_releases_the_gil(priced):
    import threading

    k, t, side = _grid_points(priced)
    k = np.tile(k, 200)
    t = np.tile(t, 200)
    side = np.tile(side, 200)

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
        priced.grid(k, t, side)
        advanced = counter - before
    finally:
        stop.set()
        spinner.join(timeout=5.0)

    assert advanced > 0
