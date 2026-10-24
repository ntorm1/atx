"""A1 served analytics through Python (lane PY, 2026-10-23).

The library contracts are pinned by atx-vol/tests/served_analytics_test.cpp. This file pins
what the binding adds: every number carries its AnalyticsDeclaration (model / version /
hedge convention / clock) and a typed AnalyticsStatus, a refusal is a status with NaN
numbers (never a silent value), and scenario matrices come back as numpy arrays shaped
(n_spot, n_vol, n_time[, k]) in the C++ [spot][vol][time] order.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import atxvol as av
from test_quote_slice import _wing_chain

PUT = av.Side.PUT
FAMILY = av.ServeTier.FAMILY


@pytest.fixture(scope="module")
def record():
    market, quotes = _wing_chain()
    config = av.QuoteSliceConfig()
    config.floor_policy = av.QuoteSliceFloorPolicy.PRICE_ONLY
    fit = av.fit_quote_slice(market, quotes, config)
    assert fit.fit_status == av.QuoteSliceFitStatus.SUCCESS, fit.fit_error
    return av.build_serve_record(fit)


@pytest.fixture
def book(record):
    served = av.ServeBook()
    assert served.publish_expiry(av.ServeBookEntry(7, record, tag=5)) == 1
    return served


def _portfolio():
    return [av.ServedPosition(1, 7, 100.0, PUT, 2.0), av.ServedPosition(2, 7, 104.0, PUT, -1.0)]


def _spec():
    spec = av.ScenarioSpec()
    spec.spot_rel = [-0.01, 0.0, 0.01]
    spec.vol_abs = [0.0, 0.01]
    spec.time_years = [0.0]
    spec.spot = 100.0
    spec.tier = FAMILY
    spec.keep_legs = True
    return spec


def test_hedge_greeks_carry_their_declaration_and_status(record, book):
    reader = book.reader()
    hedge = av.hedge_greeks(reader, av.HedgeGreeksRequest(
        7, 100.0, PUT, 100.0, convention=av.HedgeConvention.CONTRACT_SIGMA, tier=FAMILY))
    assert hedge.status == av.AnalyticsStatus.OK
    assert hedge.input_class == av.AnalyticsInputClass.CERTIFIED
    assert hedge.certificate_class == av.ServeBoundClass.EXACT
    assert hedge.price > 0.0 and -1.0 < hedge.delta < 0.0
    assert hedge.reason == [av.HedgeGreekReason.SERVED] * 4
    declaration = hedge.declaration
    assert (declaration.book_version, declaration.expiry_id, declaration.entry_tag) == (1, 7, 5)
    assert declaration.model == av.AnalyticsModel.QUOTING_RECORD
    assert declaration.convention == av.HedgeConvention.CONTRACT_SIGMA
    assert declaration.clock.tau == record.tau
    assert declaration.record_fit_sha256 == record.version.fit_sha256

    # A sampled (ladder-tier) answer is an Estimate, never silently certified.
    estimate = av.hedge_greeks(reader, av.HedgeGreeksRequest(7, 100.0, PUT, 100.0))
    assert estimate.status == av.AnalyticsStatus.ESTIMATE
    assert estimate.input_class == av.AnalyticsInputClass.ESTIMATE
    answer = reader.serve(7, av.ServeRequest(100.0, PUT, 100.0))
    assert av.classify_served_answer(answer) == av.AnalyticsInputClass.ESTIMATE


def test_a_refusal_is_a_typed_status_with_nan_numbers(record):
    stale = av.ServeBook()
    # An expiry instant before the record's as_of: every answer is Stale.
    stale.publish_expiry(av.ServeBookEntry(7, record, expiry_ns=record.version.as_of_ns - 1))
    hedge = av.hedge_greeks(stale.reader(), av.HedgeGreeksRequest(7, 100.0, PUT, 100.0,
                                                                  tier=FAMILY))
    assert hedge.status == av.AnalyticsStatus.REFUSED
    assert hedge.serve_status == av.ServeStatus.STALE
    assert math.isnan(hedge.price) and math.isnan(hedge.delta)

    live = av.ServeBook()
    live.publish_expiry(av.ServeBookEntry(7, record))
    refit = av.hedge_greeks(live.reader(), av.HedgeGreeksRequest(
        7, 100.0, PUT, 100.0, convention=av.HedgeConvention.REFITTED_SURFACE, tier=FAMILY))
    # served_analytics.hpp: a refit per bump is the fitter's job, so every Greek is NaN with
    # RequiresRefit while the price is still served.
    assert refit.reason == [av.HedgeGreekReason.REQUIRES_REFIT] * 4
    assert refit.price > 0.0
    assert all(math.isnan(g) for g in (refit.delta, refit.gamma, refit.vega, refit.theta))
    missing = av.hedge_greeks(live.reader(), av.HedgeGreeksRequest(9, 100.0, PUT, 100.0))
    assert missing.status == av.AnalyticsStatus.REFUSED and math.isnan(missing.price)


def test_scenario_cells_are_numpy_grids_in_spot_vol_time_order(book):
    result = av.served_scenarios(book, _portfolio(), _spec())
    assert result.status == av.AnalyticsStatus.OK and result.book_version == 1
    assert (result.n_spot, result.n_vol, result.n_time) == (3, 2, 1)
    assert result.workload["n_cells"] == 6 and result.workload["contracts"] == 2
    cells = result.cells
    for name in ("spot_rel", "taylor_pnl", "exact_pnl", "exact_certificate_usd"):
        assert cells[name].shape == (3, 2, 1) and cells[name].dtype == np.float64, name
    n_exclusions = len(av.ScenarioExclusion.__members__)
    assert cells["taylor_excluded"].shape == (3, 2, 1, n_exclusions)
    assert cells["exact_excluded_usd"].dtype == np.float64
    assert cells["exact_routes"].shape == (3, 2, 1, len(av.ScenarioExactRoute.__members__))
    np.testing.assert_array_equal(cells["spot_rel"][:, 0, 0], [-0.01, 0.0, 0.01])
    np.testing.assert_array_equal(cells["vol_abs"][0, :, 0], [0.0, 0.01])
    assert cells["taylor_pnl"][1, 0, 0] == 0.0 and cells["exact_pnl"][1, 0, 0] == 0.0
    # The Taylor expansion tracks the exact repricing over a 1% box (both certified legs).
    np.testing.assert_allclose(cells["taylor_pnl"], cells["exact_pnl"], rtol=0.06, atol=0.05)
    assert (cells["taylor_excluded"] == 0).all()
    assert [leg.input_class for leg in result.legs] == [av.AnalyticsInputClass.CERTIFIED] * 2
    assert result.legs[0].declaration.model == av.AnalyticsModel.QUOTING_RECORD

    bad = _spec()
    bad.spot_rel = []
    with pytest.raises(av.AtxError) as raised:
        av.served_scenarios(book, _portfolio(), bad)
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT


def test_attribution_explains_a_spot_move_leg_by_leg(book):
    attribution = av.served_attribution(book, _portfolio(), av.ServedMove(0.01, 0.0, 0.0),
                                        _spec())
    assert attribution.status == av.AnalyticsStatus.OK
    assert attribution.attributed_legs == 2 and len(attribution.legs) == 2
    assert attribution.explained == pytest.approx(
        attribution.delta + attribution.gamma + attribution.vega + attribution.theta)
    assert attribution.unexplained == pytest.approx(attribution.actual - attribution.explained)
    assert abs(attribution.unexplained) < 0.01 * abs(attribution.actual)


def test_dated_cash_scenarios_reprice_on_the_discrete_dividend_lattice():
    contract = av.CashDividendContract(1, 100.0, 100.0, 0.5, 0.25, 0.03, side=PUT,
                                       dividends=[(0.25, 1.0)])
    result = av.cash_dividend_scenarios([contract], spot_rel=[-0.01, 0.0, 0.01])
    assert (result.n_spot, result.n_vol, result.n_time) == (3, 1, 1)
    exact = result.cells["exact_pnl"]
    assert exact.shape == (3, 1, 1)
    assert exact[1, 0, 0] == 0.0 and exact[0, 0, 0] > 0.0 > exact[2, 0, 0]
    leg = result.legs[0]
    assert leg.base_price > 0.0
    assert leg.declaration.model == av.AnalyticsModel.DISCRETE_DIVIDEND
    assert result.lattice_solves > 0


def test_every_analytics_enum_names_every_library_value():
    for name in ("AnalyticsModel", "HedgeConvention", "AnalyticsInputClass", "AnalyticsStatus",
                 "HedgeGreekReason", "ScenarioExactRoute", "ScenarioExclusion"):
        enum = getattr(av, name)
        library = av._core._enum_names(name)
        assert set(library) == {int(v) for v in enum.__members__.values()}, name
