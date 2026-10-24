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
