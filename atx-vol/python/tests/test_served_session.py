"""ServedSession through Python: a PricedSurface served through a ServeBook (lane PY).

The library contract is pinned by atx-vol/tests/served_session_test.cpp
(docs/analytics-contracts.md section 6). This file pins what the binding adds: a bound T is
answered by the book's record (source BOOK, the record's statement verbatim), any other T
by the surface's cold route (source SESSION_COLD, priced like the surface itself), the
chain columns equal the per-contract answers, and the session keeps both the borrowed
surface and the co-owned book alive.
"""

from __future__ import annotations

import gc
import math

import numpy as np
import pytest

import atxvol as av
from test_quote_slice import _wing_chain

PUT = av.Side.PUT
NOW_NS = 1_766_000_000_000_000_000


def _surface():
    curves = av.CurveSurface()
    context = []
    for i, t in enumerate((0.25, 0.5)):
        p = av.EssviParams()
        p.theta = 0.0625 * t / 0.25
        p.phi = 1.2
        p.rho = -0.3
        p.psi = 0.5
        p.p = 0.5
        p.lambda_ = 0.5
        p.T = t
        p.F = 100.0
        p.expiry_id = i
        curves.push_essvi(p, math.exp(-0.03 * t))
        context.append(av.SliceContext(T=t, forward=100.0, borrow=0.0, q_eff=0.0,
                                       n_used=50, n_dropped=0))
    pricing = av.PricingContext()
    pricing.S = 100.0
    pricing.r = 0.03
    pricing.now_ts_ns = NOW_NS
    pricing.method = av.AmericanMethod.ANDERSEN_LAKE
    return av.PricedSurface.create(curves, context, pricing)


@pytest.fixture(scope="module")
def book():
    market, quotes = _wing_chain()
    config = av.QuoteSliceConfig()
    config.floor_policy = av.QuoteSliceFloorPolicy.PRICE_ONLY
    fit = av.fit_quote_slice(market, quotes, config)
    assert fit.fit_status == av.QuoteSliceFitStatus.SUCCESS, fit.fit_error
    served = av.ServeBook()
    served.publish_expiry(av.ServeBookEntry(7, av.build_serve_record(fit)))
    return served


def test_a_bound_expiry_is_the_books_answer_and_others_take_the_cold_route(book):
    session = av.ServedSession.bind(_surface(), book, [av.SessionBookExpiry(0.25, 7)])
    gc.collect()  # the session keeps the borrowed surface alive
    assert session.bound_expiry(0.25) == 7 and session.bound_expiry(0.5) is None
    assert [(b.T, b.expiry_id) for b in session.bindings()] == [(0.25, 7)]
    reader = session.reader()

    bound = session.serve(reader, 100.0, 0.25, PUT, 100.0, NOW_NS)
    assert bound.source == av.SessionServeSource.BOOK
    direct = book.reader().serve(7, av.ServeRequest(100.0, PUT, 100.0, now_ns=NOW_NS))
    assert bound.book.status == direct.status and bound.price == direct.price
    assert bound.declaration.model == av.AnalyticsModel.QUOTING_RECORD

    cold = session.serve(reader, 100.0, 0.5, PUT, 100.0, NOW_NS)
    assert cold.source == av.SessionServeSource.SESSION_COLD
    assert cold.declaration.model == av.AnalyticsModel.SESSION_CURVE
    assert cold.price > 0.0 and cold.delta < 0.0

    refused = session.serve(reader, -1.0, 0.5, PUT, 100.0, NOW_NS)
    assert refused.status == av.AnalyticsStatus.INVALID_ARGUMENT
    assert math.isnan(refused.price)


def test_session_chain_columns_equal_the_per_contract_answers(book):
    session = av.ServedSession.bind(_surface(), book, [av.SessionBookExpiry(0.25, 7)])
    reader = session.reader()
    strikes = np.array([96.0, 100.0, 100.0])
    expiries = np.array([0.25, 0.25, 0.5])
    cols = session.serve_chain(reader, 100.0, NOW_NS, strikes, expiries, PUT)
    for name in ("price", "delta", "gamma", "vega", "theta", "certificate_usd"):
        assert cols[name].dtype == np.float64 and cols[name].shape == (3,), name
    sources = [av.SessionServeSource(int(s)) for s in cols["source"]]
    assert sources == [av.SessionServeSource.BOOK] * 2 + [av.SessionServeSource.SESSION_COLD]
    # Another model's answer is never a certificate claim (H1's rule).
    assert av.TickStatus(int(cols["tick_status"][2])) == av.TickStatus.FALLBACK
    for i in range(3):
        answer = session.serve(reader, strikes[i], expiries[i], PUT, 100.0, NOW_NS)
        assert answer.source == sources[i]
        assert cols["price"][i] == pytest.approx(answer.price, rel=1e-12, abs=1e-12)
    with pytest.raises(ValueError, match="same length"):
        session.serve_chain(reader, 100.0, NOW_NS, strikes, expiries[:2], PUT)


def test_a_bad_binding_raises_and_the_source_enum_is_complete(book):
    with pytest.raises(av.AtxError) as raised:
        av.ServedSession.bind(_surface(), book, [av.SessionBookExpiry(-1.0, 7)])
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT
    library = av._core._enum_names("SessionServeSource")
    assert set(library) == {int(v) for v in av.SessionServeSource.__members__.values()}


@pytest.mark.parametrize("side", [av.Side.PUT, av.Side.CALL])
def test_cold_partial_availability_and_provenance_survive_scalar_and_chain(side):
    put = side == av.Side.PUT
    s, k = (772.99, 811.6395) if put else (811.6395, 772.99)
    r, q = (.04496, .00162193) if put else (.00162193, .04496)
    t, sigma = .0682, .12
    p = av.EssviParams()
    p.T, p.F, p.theta = t, s * math.exp((r-q)*t), sigma*sigma*t
    p.phi = p.rho = 0.
    p.psi = p.p = p.lambda_ = .5
    curves = av.CurveSurface()
    curves.push_essvi(p, math.exp(-r*t))
    pricing = av.PricingContext()
    pricing.S, pricing.r = s, r
    surface = av.PricedSurface.create(curves, [av.SliceContext(
        T=t, forward=p.F, borrow=0., q_eff=q, n_used=100, n_dropped=0)], pricing)
    session = av.ServedSession.bind(surface, av.ServeBook(), [])
    reader = session.reader()
    answer = session.serve(reader, k, t, side, s, NOW_NS)
    wanted = 256 | 1 | 2 | 4 | 8
    assert int(answer.requested) == wanted
    assert int(answer.supported) & 256
    assert not int(answer.supported) & 2
    assert math.isfinite(answer.price) and math.isnan(answer.gamma)
    assert answer.cold_reasons[1] == av.AmericanGreekReason.STENCIL_CROSSING
    assert all(answer.cold_reasons[i] == av.AmericanGreekReason.NOT_REQUESTED
               for i in (4, 5, 6, 7))
    assert answer.status == av.AnalyticsStatus.ESTIMATE  # model/price class, not all-risk
    strikes = np.array([k, s, -1.])
    expiries = np.full(3, t)
    columns = session.serve_chain(reader, s, NOW_NS, strikes, expiries, side)
    assert columns["requested"].dtype == columns["supported"].dtype == np.uint32
    assert columns["cold_reasons"].shape == (3, 9)
    assert columns["book_greek_reasons"].shape == (3, 6)
    assert columns["cold_reasons"].dtype == columns["book_greek_reasons"].dtype == np.uint8
    for i in range(3):
        one = session.serve(reader, strikes[i], t, side, s, NOW_NS)
        assert columns["requested"][i] == int(one.requested)
        assert columns["supported"][i] == int(one.supported)
        assert columns["cold_reasons"][i].tolist() == [int(v) for v in one.cold_reasons]
        assert columns["book_greek_reasons"][i].tolist() == [0]*6
    assert columns["supported"][2] == 0 and math.isnan(columns["price"][2])
    assert columns["supported"][1] == wanted
    empty = session.serve_chain(reader, s, NOW_NS, np.array([]), np.array([]), side)
    assert empty["cold_reasons"].shape == (0, 9)
    assert empty["book_greek_reasons"].shape == (0, 6)
    saved = columns["cold_reasons"].copy()
    del reader, session, surface
    gc.collect()
    np.testing.assert_array_equal(columns["cold_reasons"], saved)
    assert columns["cold_reasons"].flags.owndata


def test_book_reason_vocabulary_is_preserved_without_cold_translation(book):
    session = av.ServedSession.bind(_surface(), book, [av.SessionBookExpiry(.25, 7)])
    reader = session.reader()
    strikes = np.array([96., 100., 500.])
    ids = np.full(3, 7, dtype=np.uint64)
    book_columns = reader.serve_chain(100., NOW_NS, ids, strikes, PUT)
    assert book_columns["greek_reasons"].shape == (3, 6)
    assert book_columns["greek_reasons"].dtype == np.uint8
    columns = session.serve_chain(reader, 100., NOW_NS, strikes, np.full(3, .25), PUT)
    np.testing.assert_array_equal(columns["book_greek_reasons"], book_columns["greek_reasons"])
    assert np.all(columns["cold_reasons"] == int(av.AmericanGreekReason.NOT_REQUESTED))
    for i, strike in enumerate(strikes):
        one = session.serve(reader, strike, .25, PUT, 100., NOW_NS)
        assert columns["supported"][i] == int(one.supported)
        assert book_columns["greek_reasons"][i].tolist() == [int(v) for v in one.book.reason]
    assert columns["supported"][2] == 0
