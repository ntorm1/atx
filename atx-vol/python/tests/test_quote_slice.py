"""Quote-slice identifiability and snapshot metadata through the Python object (lane C0).

The typed price-only answer, the per-row volatility convention, the fit's model and
identifiability policy, its snapshot provenance and the one-spot carry rebase must all be
readable from Python, not only from C++ (atx-vol/tests/quote_slice_identification_test.cpp
and quote_slice_carry_test.cpp pin the same contracts in the library).
"""

from __future__ import annotations

import math

import pytest

import atxvol as av

SPOT = 100.0
YEARS = 0.25
SIGMA = 0.25
RATE = 0.03


def _crr_american(spot, strike, years, sigma, rate, call, steps=200):
    dt = years / steps
    u = math.exp(sigma * math.sqrt(dt))
    d = 1.0 / u
    p = (math.exp(rate * dt) - d) / (u - d)
    disc = math.exp(-rate * dt)
    sign = 1.0 if call else -1.0
    values = [max(sign * (spot * u**j * d ** (steps - j) - strike), 0.0) for j in range(steps + 1)]
    for i in range(steps - 1, -1, -1):
        for j in range(i + 1):
            s = spot * u**j * d ** (i - j)
            cont = disc * (p * values[j + 1] + (1.0 - p) * values[j])
            values[j] = max(cont, sign * (s - strike))
    return values[0]


def _wing_chain():
    """Strikes 70-140 every 2, both sides, +/- 1 % spreads (at least 0.01)."""
    quotes = []
    for strike in range(70, 141, 2):
        for side in (av.Side.CALL, av.Side.PUT):
            price = _crr_american(SPOT, float(strike), YEARS, SIGMA, RATE, side == av.Side.CALL)
            spread = max(0.01, 0.01 * price)
            quotes.append(
                av.QuoteSliceQuote(float(strike), side, max(price - spread, 0.0), price + spread)
            )
    return av.QuoteSliceMarket(SPOT, YEARS, YEARS, RATE, 0.0, 0.0), quotes


@pytest.fixture(scope="module")
def wing():
    market, quotes = _wing_chain()
    config = av.QuoteSliceConfig()
    config.floor_policy = av.QuoteSliceFloorPolicy.PRICE_ONLY
    fit = av.fit_quote_slice(market, quotes, config)
    assert fit.fit_status == av.QuoteSliceFitStatus.SUCCESS, fit.fit_error
    return market, quotes, fit


def test_rows_and_queries_report_their_volatility_convention(wing):
    _, quotes, fit = wing
    assert len(fit.rows) == len(quotes)
    served = 0
    for quote, row in zip(quotes, fit.rows):
        if row.evaluation_status != av.QuoteSliceEvaluationStatus.SUCCESS:
            assert row.vol_convention == av.QuoteSliceVolConvention.NONE
            continue
        assert row.vol_convention in (
            av.QuoteSliceVolConvention.FITTED_CURVE,
            av.QuoteSliceVolConvention.REIMPLIED_REPAIRED,
        )
        assert row.price_only_reason == av.QuoteSlicePriceOnlyReason.NONE
        assert math.isnan(row.iv_lower) and math.isnan(row.iv_upper)
        answer = av.query_quote_slice(fit, quote.strike, quote.side)
        assert answer.status == av.QuoteSliceEvaluationStatus.SUCCESS
        assert answer.vol_convention == row.vol_convention
        assert answer.price == row.fitted_price
        served += 1
    assert served > 0
    # Between two nodes the certified price is re-implied, never the curve's own value.
    between = av.query_quote_slice(fit, 103.0, av.Side.CALL)
    assert between.status == av.QuoteSliceEvaluationStatus.SUCCESS
    assert between.vol_convention == av.QuoteSliceVolConvention.REIMPLIED_REPAIRED
    value = av.evaluate_quote_slice(fit, 103.0, av.Side.CALL)
    assert value.volatility == between.volatility and value.price == between.price


def test_a_deep_itm_put_at_its_exercise_value_is_price_only_with_its_price(wing):
    _, _, fit = wing
    assert 138.0 in fit.node_strikes and 140.0 in fit.node_strikes
    answer = av.query_quote_slice(fit, 139.0, av.Side.PUT)
    # The certified price between two exercise-region nodes is the exercise value itself:
    # no volatility above the floor reproduces it, so it is served price-only.
    assert answer.status == av.QuoteSliceEvaluationStatus.PRICE_ONLY
    assert answer.reason in (
        av.QuoteSlicePriceOnlyReason.FLOOR_VOL,
        av.QuoteSlicePriceOnlyReason.FLAT_EXERCISE_REGION,
    )
    assert answer.vol_convention == av.QuoteSliceVolConvention.NONE
    assert answer.price == pytest.approx(139.0 - SPOT, abs=1e-9)
    assert math.isnan(answer.volatility)
    assert answer.iv_lower == 0.0 and answer.iv_upper >= 0.005
    # The untyped call refuses the same contract instead of returning the floor volatility.
    with pytest.raises(av.AtxError) as raised:
        av.evaluate_quote_slice(fit, 139.0, av.Side.PUT)
    assert raised.value.code == av.ErrorCode.OUT_OF_RANGE


def test_the_fit_carries_its_model_policy_and_snapshot_provenance(wing):
    market, quotes, fit = wing
    assert fit.floor_policy == av.QuoteSliceFloorPolicy.PRICE_ONLY
    assert fit.model_kind == av.QuoteSliceModelKind.ANDERSEN_LAKE
    assert fit.dividend_model == av.QuoteSliceDividendModel.CONTINUOUS_YIELD
    # fit_quote_slice observes no clock: provenance is "not supplied" until a caller
    # (fit_quote_surface, the bench) stamps it.
    snap = fit.snapshot
    assert (snap.quote_ns, snap.underlying_ns, snap.spot_version, snap.carry_version) == (
        0,
        0,
        0,
        0,
    )
    assert snap.carry_convention == av.QuoteSliceCarryConvention.SUPPLIED
    assert math.isnan(snap.raw_spot) and snap.spot_stale is False
    legacy = av.fit_quote_slice(market, quotes)
    assert legacy.floor_policy == av.QuoteSliceFloorPolicy.LEGACY
    info = av.QuoteSliceSnapshotInfo()
    info.spot_version = 2
    info.carry_convention = av.QuoteSliceCarryConvention.FORWARD_PRESERVING
    assert info.spot_version == 2


def _forward(market):
    return av.fit_quote_slice(market, []).forward


def test_the_one_spot_rebase_keeps_the_forward_on_the_calendar_clock():
    market = av.QuoteSliceMarket(773.00, 2.0 / 252.0, 2.0 / 365.0, 0.045, 0.012, 0.0)
    rebased = av.rebase_quote_slice_spot(
        market, 772.98, av.QuoteSliceDividendModel.CONTINUOUS_YIELD
    )
    assert rebased.market.spot == 772.98
    expected = math.log(772.98 / 773.00) / market.calendar_years
    assert rebased.yield_shift == pytest.approx(expected, rel=1e-12)
    assert rebased.market.continuous_yield == market.continuous_yield + rebased.yield_shift
    before = _forward(market)
    assert _forward(rebased.market) == pytest.approx(before, rel=1e-12)
    assert rebased.forward == pytest.approx(before, rel=1e-12)


def test_the_rebase_refuses_a_spot_no_carry_can_reconcile():
    market = av.QuoteSliceMarket(100.0, 0.5, 0.5, 0.04, 0.01, 0.0)
    with pytest.raises(av.AtxError) as raised:
        av.rebase_quote_slice_spot(market, 0.0, av.QuoteSliceDividendModel.CONTINUOUS_YIELD)
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT
    with pytest.raises(av.AtxError):
        av.rebase_quote_slice_spot(
            market, 100.1, av.QuoteSliceDividendModel.CONTINUOUS_YIELD, [(0.1, 1.0)]
        )
