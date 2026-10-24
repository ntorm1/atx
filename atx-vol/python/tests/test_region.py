"""Exercise-region classification and the D1 discrete-dividend batch through Python.

Lane PY (2026-10-23). The library contracts are pinned by
atx-vol/tests/american_region_test.cpp and the discrete-div production tests; this file
pins what the binding adds: the numpy batch contract (equal-length columns, a bare Side
broadcasts, a refused lane is NaN plus a typed status, a malformed call raises), parity
with the existing scalar / batch entry points, and enum completeness.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import atxvol as av

PUT, CALL = av.Side.PUT, av.Side.CALL


def _book():
    S = np.array([100.0, 100.0, 50.0, 100.0, 120.0])
    K = np.array([90.0, 100.0, 100.0, 110.0, 100.0])
    T = np.array([0.25, 0.5, 1.0, 0.75, 2.0])
    sigma = np.array([0.2, 0.3, 0.2, 0.25, 0.35])
    r = np.full(5, 0.04)
    q = np.array([0.0, 0.01, 0.0, 0.02, 0.03])
    return S, K, T, sigma, r, q


def test_kernel_floor_is_bit_identical_to_the_existing_american_price_route():
    S, K, T, sigma, r, q = _book()
    price, boundary, cls, status = av.andersen_lake_classified_batch(S, K, T, sigma, r, q, PUT)
    assert price.dtype == np.float64 and boundary.dtype == np.float64
    assert cls.dtype == np.uint8 and status.dtype == np.int32
    assert price.shape == boundary.shape == cls.shape == status.shape == (5,)
    assert (status == av.STATUS_OK).all()
    reference, ref_status = av.american_price_batch(S, K, T, sigma, r, q, PUT,
                                                    isa=av.SimdIsa.FORCE_SCALAR)
    assert (ref_status == av.STATUS_OK).all()
    np.testing.assert_array_equal(price, reference)
    for i in range(5):
        scalar = av.andersen_lake_classified(S[i], K[i], T[i], sigma[i], r[i], q[i], PUT)
        assert scalar[0] == price[i]
        assert scalar[2] == av.AlExerciseClass(int(cls[i]))
        # The classifier agrees with the class the route used, at the boundary it solved.
        assert av.classify_exercise_region(S[i], K[i], T[i], r[i], q[i], sigma[i], PUT,
                                           scalar[1]) == scalar[2]
    again = av.classify_exercise_region_batch(S, K, T, r, q, sigma, PUT, boundary)
    np.testing.assert_array_equal(again, cls)


def test_the_intrinsic_rule_prices_the_exercise_plateau_at_intrinsic_exactly():
    # A deep in-the-money put (S = 50, K = 100) sits on its exercise plateau.
    price, boundary, cls = av.andersen_lake_classified(50.0, 100.0, 1.0, 0.2, 0.04, 0.0, PUT,
                                                       rule=av.AlExerciseRegion.INTRINSIC)
    assert cls == av.AlExerciseClass.EXERCISE_PLATEAU
    assert price == 50.0
    assert 50.0 <= boundary
    floor_price, _, floor_cls = av.andersen_lake_classified(50.0, 100.0, 1.0, 0.2, 0.04, 0.0,
                                                            PUT)
    assert floor_cls == cls and floor_price >= price


def test_a_refused_lane_is_nan_with_a_typed_status_and_a_malformed_call_raises():
    S, K, T, sigma, r, q = _book()
    K = K.copy()
    K[1] = -1.0
    price, boundary, cls, status = av.andersen_lake_classified_batch(S, K, T, sigma, r, q, PUT)
    assert math.isnan(price[1]) and math.isnan(boundary[1])
    assert status[1] == int(av.ErrorCode.INVALID_ARGUMENT)
    assert (np.delete(status, 1) == av.STATUS_OK).all()
    with pytest.raises(av.AtxError) as raised:
        av.andersen_lake_classified(100.0, -1.0, 0.5, 0.2, 0.04, 0.0, CALL)
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT
    with pytest.raises(ValueError, match="same length"):
        av.andersen_lake_classified_batch(S, K[:3], T, sigma, r, q, PUT)
    sides = np.array([int(PUT), int(CALL), int(PUT), int(CALL), int(PUT)], dtype=np.int32)
    _, _, _, mixed = av.andersen_lake_classified_batch(S, np.abs(K), T, sigma, r, q, sides)
    assert (mixed == av.STATUS_OK).all()


def _schedule():
    return [(100.0, 0.5, 0.03, 0.0, [(0.25, 1.0)]), (-1.0, 0.5, 0.03, 0.0, [])]


def test_the_discrete_dividend_batch_prices_inverts_and_types_every_refusal():
    batch = av.DiscreteDivBatch()
    assert batch.prepare(_schedule()) == 1
    assert batch.groups == 2 and batch.group_ok(0) and not batch.group_ok(1)
    route = batch.route(0)
    assert route["events"] == 1 and route["steps"] >= 4
    group = np.array([0, 0, 0, 1, 0], dtype=np.uint32)
    K = np.array([90.0, 100.0, 110.0, 100.0, -5.0])
    sigma = np.full(5, 0.25)
    price, outcome = batch.price(group, K, PUT, sigma)
    assert price.dtype == np.float64 and outcome.dtype == np.uint8 and price.shape == (5,)
    Outcome = av.DiscreteDivBatchOutcome
    assert [Outcome(int(o)) for o in outcome] == [
        Outcome.PRICED, Outcome.PRICED, Outcome.PRICED, Outcome.REFUSED_GROUP,
        Outcome.REFUSED_INPUT]
    assert (price[:3] > 0.0).all() and np.isnan(price[3:]).all()
    assert price[0] < price[1] < price[2]

    quote = price.copy()
    sigma_hat, iv_outcome, fallback, screen, accurate = batch.implied_vol(group, K, PUT, quote)
    assert fallback.dtype == np.uint8 and screen.dtype == np.uint16
    assert accurate.dtype == np.uint16
    assert (iv_outcome[:3] == int(Outcome.PRICED)).all()
    np.testing.assert_allclose(sigma_hat[:3], 0.25, atol=1e-6)
    assert iv_outcome[3] == int(Outcome.REFUSED_GROUP)

    greeks = batch.greeks(group[:3], K[:3], PUT, sigma[:3])
    np.testing.assert_array_equal(greeks["price"], price[:3])
    assert (greeks["delta"] < 0.0).all() and (greeks["vega"] > 0.0).all()
    assert greeks["unsupported"].dtype == np.uint16


def test_every_region_and_batch_enum_names_every_library_value():
    for name, enum in (("AlExerciseClass", av.AlExerciseClass),
                       ("DiscreteDivBatchOutcome", av.DiscreteDivBatchOutcome),
                       ("DiscreteDivIvFallback", av.DiscreteDivIvFallback),
                       ("DiscreteDivMethod", av.DiscreteDivMethod)):
        library = av._core._enum_names(name)
        assert set(library) == {int(v) for v in enum.__members__.values()}, name
    assert av.AL_EXPIRY_LIMIT_T == 1.0e-12 and av.AL_ZERO_VOL_SIGMA == 1.0e-8
