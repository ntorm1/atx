from __future__ import annotations

import math
import threading

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
    vols, status = atxvol.implied_vol_batch(actual, f, k, t, df, atxvol.Side.PUT)
    np.testing.assert_allclose(vols, sigma, atol=1.0e-10, rtol=0.0)
    np.testing.assert_array_equal(status, np.full(3, atxvol.STATUS_OK, dtype=np.int32))


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


# ── PY-1: the raised exception must carry the structured code ───────────────

def test_atx_error_exposes_the_structured_error_code() -> None:
    # Regression (PY-1): the translator used to stringify the Error and drop
    # `AtxException::code()`, so programmatic dispatch (retry on Unavailable,
    # skip on NotFound) had to regex-match prose that is not a stable contract.
    with pytest.raises(atxvol.AtxError) as excinfo:
        atxvol.implied_vol(1.0, -1.0, 100.0, 0.5, 0.99, atxvol.Side.CALL)

    assert excinfo.value.code == atxvol.ErrorCode.INVALID_ARGUMENT
    # The message is still the human-readable one the older tests match on.
    assert "InvalidArgument" in str(excinfo.value)


# ── PY-3: batch IV keeps the NaN + per-lane status convention ───────────────

def _iv_batch_inputs(n: int, bad_lane: int) -> tuple[np.ndarray, ...]:
    f = np.full(n, 100.0)
    k = np.linspace(90.0, 110.0, n)
    t = np.full(n, 0.5)
    df = np.full(n, 1.0)
    sigma = np.full(n, 0.25)
    price = np.array(atxvol.black76_price_batch(f, k, t, sigma, df, atxvol.Side.CALL))
    price[bad_lane] = -1.0  # below intrinsic: this lane cannot be inverted
    return price, f, k, t, df, sigma


def test_implied_vol_batch_returns_nan_and_status_for_a_bad_lane() -> None:
    # Regression (PY-3): one uninvertible quote used to raise AtxError and throw
    # away every successfully inverted lane. Real NBBO chains always contain
    # some, so the vectorized path was unusable on the workload it exists for.
    n, bad = 10, 7
    price, f, k, t, df, sigma = _iv_batch_inputs(n, bad)

    vols, status = atxvol.implied_vol_batch(price, f, k, t, df, atxvol.Side.CALL)

    good = [i for i in range(n) if i != bad]
    np.testing.assert_allclose(vols[good], sigma[good], atol=1.0e-10, rtol=0.0)
    assert math.isnan(vols[bad])
    assert all(int(status[i]) == atxvol.STATUS_OK for i in good)
    assert int(status[bad]) != atxvol.STATUS_OK
    assert int(status[bad]) == int(atxvol.ErrorCode.OUT_OF_RANGE)


def test_implied_vol_batch_still_raises_on_a_shape_error() -> None:
    # Batch-level throw survives: only per-LANE failures become statuses.
    price, f, k, t, df, _ = _iv_batch_inputs(4, 1)
    with pytest.raises(ValueError):
        atxvol.implied_vol_batch(price, f, k[:3], t, df, atxvol.Side.CALL)


# ── PY-5: AloPricer.price mutates cached state; it must hold the GIL ────────

_GIL_PROBE_CALLS = 200


def test_alo_pricer_price_holds_the_gil() -> None:
    # Regression (PY-5): `AloPricer::price` is non-const — it mutates the cached
    # exercise boundary — but the binding wrapped it in `gil_scoped_release`, so
    # two Python threads sharing one pricer raced in C++ with no lock and no GIL.
    #
    # Observation: a bound call that HOLDS the GIL cannot let another Python
    # thread execute a single bytecode, so a spinner's counter can only move in
    # the handful of bytecodes that bracket the call. A call that RELEASES it
    # hands the interpreter over on every single invocation.
    #
    # Measured separation on this binding (200 calls, spinner thread):
    #   with `call_guard<gil_scoped_release>` (the defect):  200/200 advanced
    #   without it (the fix):                                  6/200 advanced
    # `control` samples an adjacent, call-free window so the ambient switch rate
    # is visible in the failure message rather than inferred.
    pricer = atxvol.AloPricer(100.0, 100.0, 1.0, 0.04, 0.01, atxvol.Side.PUT)
    pricer.price(0.25)  # warm the boundary cache; ignore the first solve

    counter = 0
    stop = threading.Event()

    def spin() -> None:
        nonlocal counter
        while not stop.is_set():
            counter += 1

    spinner = threading.Thread(target=spin, daemon=True)
    spinner.start()
    try:
        # Make sure the spinner is actually running before we sample.
        start = counter
        while counter == start:
            pass

        advanced = 0
        control = 0
        for i in range(_GIL_PROBE_CALLS):
            a = counter
            b = counter
            control += b != a
            before = counter
            pricer.price(0.10 + 0.001 * i)
            if counter != before:
                advanced += 1
    finally:
        stop.set()
        spinner.join(timeout=5.0)

    assert advanced <= _GIL_PROBE_CALLS // 10, (
        f"{advanced}/{_GIL_PROBE_CALLS} AloPricer.price calls let another Python "
        f"thread run (ambient switch rate {control}/{_GIL_PROBE_CALLS}): the "
        "binding is releasing the GIL around a mutating call"
    )

# NOTE (deliberately no value-identity hammer test): `AloPricer::price` documents
# that it "warm-starts the boundary from the previous call", so its result is
# order-dependent to within `AlOpts::tol`. Interleaving two threads' sigma
# sequences legitimately changes each thread's warm-start chain even when the
# calls are perfectly serialized, so comparing threaded output against a serial
# reference would assert something the class never promised. The GIL probe above
# is the gate: it observes the actual defect (a mutating call executing outside
# the interpreter lock) rather than a downstream symptom.

