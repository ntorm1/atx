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


# The batch signature offers `method` and `opts`. Cover EVERY value it admits:
# pinning only the default is exactly why a knob that parsed and did nothing
# survived a green gate (rev-ws-y C2).
_ENGAGED_OPTS = av.AlOpts(3, 3, 1, 1.0e-1)


@pytest.mark.parametrize("method",
                         [av.AmericanMethod.ANDERSEN_LAKE, av.AmericanMethod.BAW],
                         ids=["andersen_lake", "baw"])
@pytest.mark.parametrize("opts", [None, _ENGAGED_OPTS],
                         ids=["default_opts", "engaged_opts"])
def test_american_price_batch_honours_every_method_and_opts(method, opts):
    spot, strike, t, sigma, r, q, side = _book(24)
    prices, status = av.american_price_batch(spot, strike, t, sigma, r, q, side,
                                             method=method, opts=opts,
                                             isa=av.SimdIsa.FORCE_SCALAR)
    assert np.all(status == av.STATUS_OK)
    expected = np.array([
        av.american_price(spot[i], strike[i], t[i], sigma[i], r[i], q[i],
                          av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT,
                          method=method, opts=opts)
        for i in range(len(strike))
    ])
    # Bit-identity for every engagement the signature admits, not just the
    # default one. A knob the batch cannot honour must not price silently.
    assert prices.tobytes() == expected.tobytes()


def test_american_price_batch_method_is_not_a_no_op():
    # The failure mode C2 describes is an argument that parses and is discarded:
    # every method returned the Andersen-Lake number with STATUS_OK on every
    # lane. Andersen-Lake and BAW are different models, so if these agree
    # bit-for-bit across a whole book, one of them is not being asked for.
    spot, strike, t, sigma, r, q, side = _book(24)
    al, _ = av.american_price_batch(spot, strike, t, sigma, r, q, side,
                                    method=av.AmericanMethod.ANDERSEN_LAKE)
    baw, _ = av.american_price_batch(spot, strike, t, sigma, r, q, side,
                                     method=av.AmericanMethod.BAW)
    assert al.tobytes() != baw.tobytes()

    engaged, _ = av.american_price_batch(spot, strike, t, sigma, r, q, side,
                                         opts=_ENGAGED_OPTS)
    assert engaged.tobytes() != al.tobytes()


def test_american_price_batch_rejects_a_shape_error():
    spot, strike, t, sigma, r, q, side = _book(8)
    with pytest.raises(ValueError):
        av.american_price_batch(spot, strike[:4], t, sigma, r, q, side)


@pytest.mark.parametrize("bad", [77, -1, 2, -2], ids=["77", "minus1", "2", "minus2"])
def test_american_batch_rejects_an_unrecognised_side_code(bad):
    # I2 (rev-ws-y): the decode used to test only for Put, so EVERY other code
    # became a Call. `-1` is a common "put" spelling in external chain data, so a
    # user importing a book got every leg priced as a call with STATUS_OK and no
    # diagnostic. Reject it, with a code a caller can dispatch on.
    spot, strike, t, sigma, r, q, _ = _book(8)
    side = np.full(len(strike), bad, dtype=np.int32)
    with pytest.raises(av.AtxError) as excinfo:
        av.american_price_batch(spot, strike, t, sigma, r, q, side)
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT

    with pytest.raises(av.AtxError):
        av.american_greeks_batch(spot, strike, t, sigma, r, q, side)


def test_american_batch_rejects_a_float_side_column():
    # `forcecast` truncates a float column to int32 before the decoder sees it.
    # A float `-1` put convention therefore arrives as -1 and is rejected. The
    # residue is documented, not fixed: a float that truncates onto a VALID code
    # (0.5 -> 0) is indistinguishable from CALL by value, so `side` should be an
    # integer dtype (see `sides.hpp`).
    spot, strike, t, sigma, r, q, _ = _book(8)
    with pytest.raises(av.AtxError):
        av.american_price_batch(spot, strike, t, sigma, r, q,
                                np.full(len(strike), -1.5))


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


def test_american_batch_unsupported_regime_is_not_an_error_code():
    spot, strike, t, sigma, r, q, side = _book(8)
    t = t.copy()
    t[3] = -1.0

    prices, status = av.american_price_batch(
        spot, strike, t, sigma, r, q, side, isa=av.SimdIsa.FORCE_SCALAR
    )

    assert math.isnan(prices[3])
    assert int(status[3]) == av.AMERICAN_BATCH_UNSUPPORTED_REGIME
    assert int(status[3]) < 0
    assert int(status[3]) != int(av.ErrorCode.NOT_IMPLEMENTED)
    assert all(int(status[i]) == av.STATUS_OK for i in range(len(t)) if i != 3)


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


# ── Black-76 Greeks / value+vega batch (5.1) ────────────────────────────────
#
# The last two unbound public batch kernels. Same parity discipline as above: a
# vectorized path only earns its place if it returns exactly what the scalar
# path returns, so every gate here compares against the bound scalar kernel.


def _slice(n: int = 40):
    """One expiry slice, mixed sides — the shape `value_and_vega` keys on."""
    rng = np.random.default_rng(20260802)
    f = np.full(n, 100.0)
    k = np.linspace(75.0, 125.0, n)
    sigma = 0.14 + 0.3 * rng.random(n)
    t = 0.75
    r = 0.041
    df = np.full(n, math.exp(-r * t))
    side = np.where(np.arange(n) % 2 == 0, int(av.Side.CALL), int(av.Side.PUT)).astype(np.int32)
    return f, k, t, sigma, r, df, side


def test_black76_greeks_batch_matches_the_scalar_loop():
    f, k, t, sigma, r, df, side = _slice()
    t_col = np.full(len(k), t)
    got = av.black76_greeks_batch(f, k, t_col, sigma, np.full(len(k), r), df, side)

    for i in range(len(k)):
        s = av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT
        ref = av.black76_greeks(f[i], k[i], t_col[i], sigma[i], r, df[i], s)
        # The batch dispatches to a 4-lane AVX2 kernel whose interior lanes use
        # deterministic vector transcendentals, so this is the kernel gate's
        # documented envelope (batch.hpp: ~1e-6 abs + 1e-7 rel), not bit-identity.
        for name in ("delta", "gamma", "vega", "theta", "rho", "vanna", "volga", "charm"):
            assert got[name][i] == pytest.approx(getattr(ref.greeks, name), abs=1e-6, rel=1e-7)
        assert got["price"][i] == pytest.approx(ref.price, abs=1e-6, rel=1e-7)


def test_black76_greeks_batch_broadcasts_one_side():
    # `side` is a per-lane column OR one Side. The older `black76_price_batch`
    # only takes one Side for the whole batch, so accepting both keeps a single
    # spelling across the vectorized surface.
    f, k, t, sigma, r, df, _ = _slice(16)
    t_col, r_col = np.full(len(k), t), np.full(len(k), r)
    broadcast = av.black76_greeks_batch(f, k, t_col, sigma, r_col, df, av.Side.PUT)
    column = av.black76_greeks_batch(f, k, t_col, sigma, r_col, df,
                                     np.full(len(k), int(av.Side.PUT), dtype=np.int32))
    for name in ("delta", "gamma", "vega", "theta", "rho", "vanna", "volga", "charm", "price"):
        assert broadcast[name].tobytes() == column[name].tobytes()


def test_black76_greeks_batch_degenerate_lane_does_not_poison_the_batch():
    # These kernels are TOTAL — `black76_greeks` is noexcept and a degenerate lane
    # (T <= 0 or sigma <= 0) collapses to the documented degenerate result rather
    # than failing — so there is no per-lane status column to surface. What must
    # hold is the same property the status convention buys elsewhere: one bad lane
    # neither raises nor corrupts its neighbours, and it agrees with the scalar.
    f, k, t, sigma, r, df, side = _slice(16)
    t_col, r_col = np.full(len(k), t), np.full(len(k), r)
    t_col[3] = -1.0        # expired
    sigma = sigma.copy()
    sigma[9] = 0.0         # zero vol

    got = av.black76_greeks_batch(f, k, t_col, sigma, r_col, df, side)

    for i in range(len(k)):
        s = av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT
        ref = av.black76_greeks(f[i], k[i], t_col[i], sigma[i], r, df[i], s)
        assert got["delta"][i] == pytest.approx(ref.greeks.delta, abs=1e-6, rel=1e-7)
        assert got["price"][i] == pytest.approx(ref.price, abs=1e-6, rel=1e-7)
    # The degenerate lanes really are degenerate, not merely equal to a scalar
    # that is itself wrong: no vega/gamma left on an expired or zero-vol lane.
    for bad in (3, 9):
        assert got["vega"][bad] == 0.0
        assert got["gamma"][bad] == 0.0
    assert np.all(np.isfinite(got["price"]))


def test_black76_greeks_batch_rejects_a_shape_error():
    f, k, t, sigma, r, df, side = _slice(8)
    t_col, r_col = np.full(len(k), t), np.full(len(k), r)
    with pytest.raises(ValueError):
        av.black76_greeks_batch(f, k[:4], t_col, sigma, r_col, df, side)
    with pytest.raises(ValueError):
        av.black76_greeks_batch(f, k, t_col, sigma, r_col, df, side[:4])
    with pytest.raises(ValueError):
        # Rank, not length: a 2-D column is a malformed call.
        av.black76_greeks_batch(f.reshape(2, 4), k, t_col, sigma, r_col, df, side)


def test_black76_greeks_batch_rejects_a_float_side_column():
    # I2/FIX-5: a float `side` is refused before any cast, with a dispatchable
    # code — never truncated onto int(Side.CALL) == 0.
    f, k, t, sigma, r, df, _ = _slice(8)
    t_col, r_col = np.full(len(k), t), np.full(len(k), r)
    with pytest.raises(av.AtxError) as excinfo:
        av.black76_greeks_batch(f, k, t_col, sigma, r_col, df, np.full(len(k), 0.5))
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT

    with pytest.raises(av.AtxError):
        av.black76_greeks_batch(f, k, t_col, sigma, r_col, df,
                                np.full(len(k), -1, dtype=np.int32))


def test_black76_value_and_vega_batch_matches_the_scalar_loop():
    f, k, t, sigma, r, df, side = _slice()
    value, vega = av.black76_value_and_vega_batch(f, k, t, sigma, df, side)

    assert len(value) == len(k) and len(vega) == len(k)
    for i in range(len(k)):
        s = av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT
        ref = av.black76_value_and_vega(f[i], k[i], t, sigma[i], df[i], s)
        assert value[i] == pytest.approx(ref.price, abs=1e-6, rel=1e-7)
        assert vega[i] == pytest.approx(ref.vega, abs=1e-6, rel=1e-7)
    # The premium must also agree with the plain pricer — same kernel, fused.
    prices = av.black76_price_batch(f, k, np.full(len(k), t), sigma, df, av.Side.CALL)
    calls = side == int(av.Side.CALL)
    np.testing.assert_allclose(value[calls], prices[calls], atol=1e-6, rtol=1e-7)


def test_black76_value_and_vega_batch_honours_the_sqrt_t_sentinel():
    # `sqrt_t >= 0` is used AS GIVEN; the default -1 means "compute sqrt(T)".
    # A knob that parses and is discarded is the exact failure class rev-ws-y C2
    # names, so pin that a wrong sqrt_t actually changes the answer.
    f, k, t, sigma, r, df, side = _slice(12)
    default, _ = av.black76_value_and_vega_batch(f, k, t, sigma, df, side)
    supplied, _ = av.black76_value_and_vega_batch(f, k, t, sigma, df, side,
                                                  sqrt_t=math.sqrt(t))
    assert default.tobytes() == supplied.tobytes()

    wrong, _ = av.black76_value_and_vega_batch(f, k, t, sigma, df, side, sqrt_t=math.sqrt(2.0 * t))
    assert wrong.tobytes() != default.tobytes()


def test_black76_value_and_vega_batch_degenerate_lane_does_not_poison_the_batch():
    f, k, t, sigma, r, df, side = _slice(16)
    sigma = sigma.copy()
    sigma[6] = -0.1        # degenerate: collapses to discounted intrinsic

    value, vega = av.black76_value_and_vega_batch(f, k, t, sigma, df, side)

    for i in range(len(k)):
        s = av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT
        ref = av.black76_value_and_vega(f[i], k[i], t, sigma[i], df[i], s)
        assert value[i] == pytest.approx(ref.price, abs=1e-6, rel=1e-7)
        assert vega[i] == pytest.approx(ref.vega, abs=1e-6, rel=1e-7)
    assert vega[6] == 0.0
    assert np.all(np.isfinite(value))


def test_black76_value_and_vega_batch_rejects_a_shape_error():
    f, k, t, sigma, r, df, side = _slice(8)
    with pytest.raises(ValueError):
        av.black76_value_and_vega_batch(f, k[:4], t, sigma, df, side)
    with pytest.raises(ValueError):
        av.black76_value_and_vega_batch(f, k, t, sigma, df, side[:4])
    with pytest.raises(av.AtxError):
        av.black76_value_and_vega_batch(f, k, t, sigma, df, np.full(len(k), 7, dtype=np.int32))


def test_b76_batch_entry_points_release_the_gil():
    # Same probe the PricedSurface grid uses: a spinner thread must make progress
    # while the kernel runs. Both kernels are pure functions over caller-owned
    # spans, which is what makes the release safe (contrast PY-5's AloPricer).
    import threading

    f, k, t, sigma, r, df, side = _slice(64)
    f, k, sigma, df, side = (np.tile(x, 400) for x in (f, k, sigma, df, side))
    t_col, r_col = np.full(len(k), t), np.full(len(k), r)

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
        av.black76_greeks_batch(f, k, t_col, sigma, r_col, df, side)
        av.black76_value_and_vega_batch(f, k, t, sigma, df, side)
        advanced = counter - before
    finally:
        stop.set()
        spinner.join(timeout=5.0)

    assert advanced > 0


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
    for family in ("iv", "total_variance", "fair_value", "greeks"):
        assert np.all(got[f"{family}_status"] == av.STATUS_OK)
        assert np.all(got[f"{family}_valid"])
    for name in ("delta", "gamma", "vega", "theta", "rho", "vanna", "volga", "charm"):
        assert np.all(got[f"{name}_valid"])
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


def test_priced_surface_grid_has_lossless_per_family_status_and_validity(priced):
    # F-5: independently failing families must not be compressed into one
    # ambiguous row status. The compatibility status is merely the first family
    # failure; every family also carries its own code and validity mask.
    k, t, side = _grid_points(priced)
    t = t.copy()
    t[1] = -1.0     # degenerate row
    t[4] = 0.0      # degenerate row
    got = priced.grid(k, t, side)

    for i in range(len(k)):
        s = av.Side.CALL if side[i] == int(av.Side.CALL) else av.Side.PUT
        iv_ok = math.isfinite(priced.iv(float(k[i]), float(t[i])))
        w_ok = math.isfinite(priced.total_variance(float(k[i]), float(t[i])))
        try:
            priced.fair_value(float(k[i]), float(t[i]), s)
            fv_code = av.STATUS_OK
        except av.AtxError as err:
            fv_code = int(err.code)
        try:
            priced.greeks(float(k[i]), float(t[i]), s)
            g_code = av.STATUS_OK
        except av.AtxError as err:
            g_code = int(err.code)

        iv_code = av.STATUS_OK if iv_ok else int(av.ErrorCode.OUT_OF_RANGE)
        w_code = av.STATUS_OK if w_ok else int(av.ErrorCode.OUT_OF_RANGE)
        assert bool(got["iv_valid"][i]) is iv_ok
        assert bool(got["total_variance_valid"][i]) is w_ok
        assert bool(got["fair_value_valid"][i]) is (fv_code == av.STATUS_OK)
        assert bool(got["greeks_valid"][i]) is (g_code == av.STATUS_OK)
        for name in ("delta", "gamma", "vega", "theta", "rho", "vanna", "volga", "charm"):
            assert bool(got[f"{name}_valid"][i]) is math.isfinite(got[name][i])
        assert int(got["iv_status"][i]) == iv_code
        assert int(got["total_variance_status"][i]) == w_code
        assert int(got["fair_value_status"][i]) == fv_code
        assert int(got["greeks_status"][i]) == g_code

        expected = next(
            (code for code in (iv_code, w_code, fv_code, g_code)
             if code != av.STATUS_OK),
            av.STATUS_OK,
        )
        assert int(got["status"][i]) == expected, (
            f"row {i}: compatibility status did not preserve the first "
            f"family failure"
        )


def test_priced_surface_grid_rejects_an_unrecognised_side_code(priced):
    # Third copy of the same decode (I2). Same rule, same coded error.
    k, t, side = _grid_points(priced)
    with pytest.raises(av.AtxError) as excinfo:
        priced.grid(k, t, np.full(len(k), -1, dtype=np.int32))
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT


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
