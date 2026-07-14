from __future__ import annotations

import math

import numpy as np
import pytest

import atxvol


def test_version_and_black76_iv_roundtrip() -> None:
    assert atxvol.__version__ == "0.1.0"
    f, k, t, sigma, r = 102.0, 100.0, 0.5, 0.27, 0.04
    df = math.exp(-r * t)
    price = atxvol.black76_price(f, k, t, sigma, df, atxvol.Side.CALL)
    assert price > 0.0
    assert atxvol.implied_vol(price, f, k, t, df, atxvol.Side.CALL) == pytest.approx(
        sigma, abs=1.0e-10
    )
    result = atxvol.black76_greeks(f, k, t, sigma, r, df, atxvol.Side.CALL)
    assert result.price == pytest.approx(price, abs=1.0e-12)
    assert result.greeks.vega > 0.0


def test_numpy_batch_matches_scalar() -> None:
    f = np.array([100.0, 101.0, 102.0])
    k = np.array([95.0, 100.0, 105.0])
    t = np.array([0.25, 0.5, 1.0])
    sigma = np.array([0.2, 0.25, 0.3])
    df = np.exp(-0.03 * t)
    actual = atxvol.black76_price_batch(f, k, t, sigma, df, atxvol.Side.PUT)
    expected = np.array(
        [
            atxvol.black76_price(fi, ki, ti, si, dfi, atxvol.Side.PUT)
            for fi, ki, ti, si, dfi in zip(f, k, t, sigma, df, strict=True)
        ]
    )
    np.testing.assert_array_equal(actual, expected)
    np.testing.assert_allclose(
        atxvol.implied_vol_batch(actual, f, k, t, df, atxvol.Side.PUT),
        sigma,
        atol=1.0e-10,
        rtol=0.0,
    )


def test_american_roundtrip_and_slice() -> None:
    args = dict(spot=100.0, strike=105.0, T=0.5, sigma=0.3, r=0.04, q=0.01)
    price = atxvol.american_price(**args, side=atxvol.Side.PUT)
    iv = atxvol.american_implied_vol(
        price,
        args["spot"],
        args["strike"],
        args["T"],
        args["r"],
        args["q"],
        atxvol.Side.PUT,
    )
    assert iv == pytest.approx(args["sigma"], abs=2.0e-6)

    strikes = np.array([95.0, 100.0, 105.0])
    values = atxvol.american_price_slice(
        strikes, 100.0, 0.5, 0.3, 0.04, 0.01, atxvol.Side.PUT
    )
    expected = np.array(
        [
            atxvol.american_price(100.0, k, 0.5, 0.3, 0.04, 0.01, atxvol.Side.PUT)
            for k in strikes
        ]
    )
    np.testing.assert_allclose(values, expected, atol=1.0e-10, rtol=1.0e-10)


def test_lightweight_surface_interpolates_total_variance() -> None:
    surface = atxvol.EssviSurface(2)
    surface.set_slice(0, atxvol.EssviSlice(0.04, 1.0, -0.2, 0.5))
    surface.set_slice(1, atxvol.EssviSlice(0.09, 0.8, -0.15, 1.0))
    assert surface.n_slices == 2
    assert surface.w(0.0, 0.5) == pytest.approx(0.04)
    assert surface.iv(0.0, 0.5) == pytest.approx(math.sqrt(0.04 / 0.5))
    batch = atxvol.essvi_w_batch(
        atxvol.EssviSlice(0.04, 1.0, -0.2, 0.5), np.array([-0.1, 0.0, 0.1])
    )
    assert batch.shape == (3,)
    assert batch[1] == pytest.approx(0.04)


def test_calibration_grade_surface_and_errors() -> None:
    params = atxvol.EssviParams()
    params.theta = 0.04
    params.phi = 1.0
    params.rho = -0.2
    params.T = 0.5
    params.F = 100.0
    surface = atxvol.VolSurface(7, atxvol.Parametrization.ESSVI, 1)
    surface.set_slice_essvi(0, params)
    assert surface.uid == 7
    assert surface.iv(0.0, 0.5) == pytest.approx(math.sqrt(0.04 / 0.5))

    with pytest.raises(atxvol.AtxError, match="InvalidArgument"):
        atxvol.implied_vol(1.0, -1.0, 100.0, 0.5, 0.99, atxvol.Side.CALL)

