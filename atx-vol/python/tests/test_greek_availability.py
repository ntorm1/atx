"""Availability is field-specific; a supplied price survives a Greek refusal."""

from __future__ import annotations

import math

import numpy as np
import pytest

import atxvol as av


FIELDS = ("delta", "gamma", "vega", "theta", "rho", "vanna", "volga", "charm", "price")
CROSSINGS = ((772.99, 811.6395, .0682, .12, .04496, .00162193, av.Side.PUT),
             (811.6395, 772.99, .0682, .12, .00162193, .04496, av.Side.CALL))


def test_availability_enums_preserve_native_ordinals_and_mask_operations():
    mask = av.GreekFieldMask
    assert int(mask.NONE) == 0
    assert int(mask.ALL_GREEKS) == 255
    assert int(mask.ALL) == 511
    for i, field in enumerate(FIELDS):
        assert int(getattr(mask, field.upper())) == 1 << i
    assert (mask.PRICE | mask.VEGA) & mask.VEGA == mask.VEGA
    assert [int(getattr(av.AmericanGreekReason, name)) for name in
            ("NOT_REQUESTED", "COMPUTED_ESTIMATE", "EXERCISED", "UNSUPPORTED_AXIS",
             "STENCIL_CROSSING", "NONFINITE")] == list(range(6))
    assert [int(getattr(av.PriceStatus, name)) for name in
            ("OK", "MODEL_UNAVAILABLE", "NUMERIC_ERROR", "INVALID_CONTRACT",
             "GREEKS_UNAVAILABLE")] == list(range(5))


@pytest.mark.parametrize("contract", CROSSINGS)
@pytest.mark.parametrize("analytic", [False, True])
def test_scalar_partial_greeks_retain_price_and_explicit_reasons(contract, analytic):
    fn = av.american_greeks_al if analytic else av.american_greeks_fd
    g = fn(*contract)
    assert int(g.requested) == 511
    assert int(g.supported) & 256
    assert math.isfinite(g.price)
    assert len(g.reasons) == 9
    for index in (1, 3, 7):
        assert not int(g.supported) & (1 << index)
        assert math.isnan(getattr(g, FIELDS[index]))
        assert g.reasons[index] == av.AmericanGreekReason.STENCIL_CROSSING
    assert g.reasons[8] == av.AmericanGreekReason.COMPUTED_ESTIMATE
    with pytest.raises(AttributeError):
        g.supported = av.GreekFieldMask.ALL


def test_exercised_zero_derivatives_are_supplied_not_unrequested_defaults():
    g = av.american_greeks_al(200., 100., 3., .2, .03, .08, av.Side.CALL)
    assert int(g.supported) == 511
    assert g.delta == 1.
    assert all(getattr(g, field) == 0. for field in FIELDS[1:8])
    assert all(reason == av.AmericanGreekReason.EXERCISED for reason in g.reasons[:8])


@pytest.mark.parametrize("analytic", [False, True])
@pytest.mark.parametrize("isa", [av.SimdIsa.FORCE_SCALAR, av.SimdIsa.AUTO])
def test_batch_metadata_preserves_refused_denominator_and_owns_storage(analytic, isa):
    # The crossing lanes remain in the denominator beside a supplied European
    # lane and an invalid lane. A bad row must not erase its neighbours' metadata.
    contracts = [*CROSSINGS, (100., 100., .5, .2, .03, 0., av.Side.CALL),
                 (100., 0., .5, .2, .03, 0., av.Side.CALL)]
    args = [np.array([c[i] for c in contracts]) for i in range(6)]
    sides = np.array([int(c[6]) for c in contracts], dtype=np.int32)
    got = av.american_greeks_batch(*args, sides, analytic=analytic, isa=isa)
    assert got["requested"].shape == got["supported"].shape == (4,)
    assert got["requested"].dtype == got["supported"].dtype == np.uint32
    assert got["reasons"].shape == (4, 9)
    assert got["reasons"].dtype == np.uint8
    assert np.all(got["requested"] == 511)
    assert np.all(got["supported"][:2] & 256)
    assert np.all(np.isfinite(got["price"][:3]))
    for index in (1, 3, 7):
        assert np.all((got["supported"][:2] & (1 << index)) == 0)
        assert np.all(got["reasons"][:2, index] == int(av.AmericanGreekReason.STENCIL_CROSSING))
    assert int(got["supported"][2]) == 511
    assert int(got["supported"][3]) == 0
    assert np.all(got["reasons"][3] == int(av.AmericanGreekReason.NONFINITE))
    assert np.all(got["status"][:2] != av.STATUS_OK)
    assert got["status"][2] == av.STATUS_OK
    assert got["status"][3] == av.AMERICAN_BATCH_UNSUPPORTED_REGIME
    assert av.STATUS_OK == -1
    assert av.AMERICAN_BATCH_UNSUPPORTED_REGIME == -2
    if isa == av.SimdIsa.FORCE_SCALAR:
        fn = av.american_greeks_al if analytic else av.american_greeks_fd
        for i, contract in enumerate(contracts[:3]):
            scalar = fn(*contract)
            assert got["requested"][i] == int(scalar.requested)
            assert got["supported"][i] == int(scalar.supported)
            assert list(got["reasons"][i]) == [int(v) for v in scalar.reasons]
    frozen = {key: got[key].tobytes() for key in ("requested", "supported", "reasons")}
    av.american_greeks_batch(*args, sides, analytic=analytic, isa=isa)
    assert all(got[key].tobytes() == value for key, value in frozen.items())


def test_empty_batch_metadata_keeps_a_nine_field_axis():
    empty = np.empty(0)
    got = av.american_greeks_batch(*([empty] * 6), av.Side.CALL)
    assert got["requested"].shape == got["supported"].shape == (0,)
    assert got["reasons"].shape == (0, 9)


def test_surface_grid_preserves_partial_greeks_and_independent_price():
    s, k, t, sigma, r, q, side = CROSSINGS[0]
    p = av.EssviParams()
    p.T, p.F, p.theta = t, s * math.exp((r - q) * t), sigma * sigma * t
    p.phi = p.rho = 0.
    p.psi = p.p = p.lambda_ = .5
    curves = av.CurveSurface()
    curves.push_essvi(p, math.exp(-r * t))
    pricing = av.PricingContext()
    pricing.S, pricing.r, pricing.uid = s, r, 7
    pricing.al_opts = av.AlOpts()
    context = [av.SliceContext(T=t, forward=p.F, borrow=0., q_eff=q,
                              n_used=100, n_dropped=0)]
    surface = av.PricedSurface.create(curves, context, pricing)
    scalar = surface.greeks(k, t, side)
    assert scalar.reasons[1] == av.AmericanGreekReason.STENCIL_CROSSING
    grid = surface.grid(np.array([k, -1.]), np.array([t, t]),
                        np.array([int(side)] * 2, dtype=np.int32))
    assert grid["requested"].dtype == grid["supported"].dtype == np.uint32
    assert grid["reasons"].shape == (2, 9)
    assert grid["reasons"].dtype == np.uint8
    assert grid["requested"][0] == int(scalar.requested)
    assert grid["supported"][0] == int(scalar.supported)
    assert list(grid["reasons"][0]) == [int(reason) for reason in scalar.reasons]
    assert grid["fair_value_valid"][0]
    assert math.isfinite(grid["fair_value"][0])
    assert grid["greek_price"][0] == scalar.price
    assert not grid["greeks_valid"][0]
    for index, name in enumerate(FIELDS[:8]):
        assert bool(grid[name + "_valid"][0]) == bool(int(scalar.supported) & (1 << index))
    assert grid["requested"][1] == 511
    assert grid["supported"][1] == 0
    assert np.all(grid["reasons"][1] == int(av.AmericanGreekReason.NONFINITE))
    assert math.isnan(grid["greek_price"][1])
