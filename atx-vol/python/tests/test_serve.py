"""Serving records and the live serve book through Python (lane PY, 2026-10-23).

The library contracts (certificates, tiers, routes, versioning) are pinned by
atx-vol/tests/slice_serve_record_test.cpp and serve_book_test.cpp. This file pins what the
binding adds: a record built from a Python fit answers with TYPED statuses and routes
(never an exception for a refusal), serve_chain's numpy columns equal the per-contract
answers of the same pinned version, readers outlive the book object, and every serve enum
lists every library value.
"""

from __future__ import annotations

import gc
import math

import numpy as np
import pytest

import atxvol as av
from test_quote_slice import _wing_chain

PUT, CALL = av.Side.PUT, av.Side.CALL


@pytest.fixture(scope="module")
def fit():
    market, quotes = _wing_chain()
    config = av.QuoteSliceConfig()
    config.floor_policy = av.QuoteSliceFloorPolicy.PRICE_ONLY
    result = av.fit_quote_slice(market, quotes, config)
    assert result.fit_status == av.QuoteSliceFitStatus.SUCCESS, result.fit_error
    return result


@pytest.fixture(scope="module")
def record(fit):
    return av.build_serve_record(fit)


def test_a_record_built_from_a_fit_states_its_domain_and_version(fit, record):
    assert (record.spot, record.tau) == (100.0, 0.25)
    assert record.strike_lo == min(fit.node_strikes)
    assert record.strike_hi == max(fit.node_strikes)
    strikes = record.node_strikes
    assert strikes.dtype == np.float64 and strikes.shape == (len(fit.node_strikes),)
    np.testing.assert_array_equal(strikes, np.asarray(fit.node_strikes))
    assert len(record.nodes()) == strikes.size
    version = record.version
    assert len(version.fit_sha256) == len(version.policy_sha256) == 32
    assert record.bytes.total > 0
    assert record.support.reason == av.ServeUnsupportedReason.NONE
    iv = record.curve.iv(np.array([-0.05, 0.0, 0.05]))
    assert iv.dtype == np.float64 and np.isfinite(iv).all()


def test_a_node_answer_is_certified_exact_and_its_price_inverts(record):
    answer = record.serve(av.ServeRequest(100.0, PUT, record.spot, tier=av.ServeTier.FAMILY))
    assert answer.status == av.ServeStatus.OK and answer.route == av.ServeRoute.NONE
    assert answer.tier == av.ServeTier.FAMILY
    assert answer.price_source == av.ServePriceSource.SLICE_NODE
    assert answer.certificate_class == av.ServeBoundClass.EXACT
    assert answer.certificate_usd == 0.0 and not answer.sampled
    assert answer.price > 0.0 and answer.delta < 0.0
    assert answer.reason[:4] == [av.ServeGreekReason.SERVED] * 4

    iv = record.implied_vol(av.ServeIvRequest(100.0, PUT, record.spot, answer.price,
                                              tier=av.ServeTier.FAMILY))
    assert iv.status == av.ServeStatus.OK
    assert iv.acceptance.kind == av.ServeAcceptanceKind.CONFIRMED
    assert 0.05 < iv.sigma < 1.0


def test_a_refusal_is_a_typed_answer_and_a_bad_build_raises(fit, record):
    outside = record.serve(av.ServeRequest(record.strike_hi * 10.0, PUT, record.spot))
    assert outside.status == av.ServeStatus.OUT_OF_DOMAIN
    assert outside.route == av.ServeRoute.REFIT and math.isnan(outside.price)

    policy = av.ServeRecordPolicy()
    policy.sigma_lo_mult = -1.0
    with pytest.raises(av.AtxError) as raised:
        av.validate_serve_record_policy(policy)
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT
    with pytest.raises(av.AtxError) as raised:
        av.build_serve_record(fit, policy)
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT

    market, quotes = _wing_chain()
    config = av.QuoteSliceConfig()
    config.min_quotes = 10_000
    failed = av.fit_quote_slice(market, quotes, config)
    assert failed.fit_status != av.QuoteSliceFitStatus.SUCCESS
    with pytest.raises(av.AtxError) as raised:
        av.build_serve_record(failed)
    assert raised.value.code == av.ErrorCode.UNAVAILABLE


def test_serve_chain_columns_equal_the_per_contract_answers_of_one_version(record):
    book = av.ServeBook()
    assert book.publish_expiry(av.ServeBookEntry(7, record)) == 1
    reader = book.reader()
    del book
    gc.collect()  # the reader shares the book's state; it outlives the Python object

    ids = np.array([7, 7, 7, 9], dtype=np.uint64)
    strikes = np.array([96.0, 100.0, 101.0, 100.0])
    cols = reader.serve_chain(100.0, 0, ids, strikes, PUT)
    assert cols["version"] == reader.pinned_version == 1
    for name in ("price", "delta", "gamma", "vega", "theta", "certificate_usd", "iv_lower"):
        assert cols[name].dtype == np.float64 and cols[name].shape == (4,), name
    for name in ("status", "route", "tier", "certificate_class", "tick_status"):
        assert cols[name].dtype == np.uint8, name
    assert cols["sampled"].dtype == np.bool_

    for i in range(4):
        answer = reader.serve(int(ids[i]), av.ServeRequest(
            strikes[i], PUT, 100.0, tier=av.ServeTier.LADDER, tier_max=av.ServeTier.FAMILY,
            greeks=av.SERVE_GREEKS_FOUR, family_bound=False))
        assert av.ServeStatus(int(cols["status"][i])) == answer.status
        assert av.ServeRoute(int(cols["route"][i])) == answer.route
        assert av.ServeTier(int(cols["tier"][i])) == answer.tier
        assert av.ServeBoundClass(int(cols["certificate_class"][i])) == answer.certificate_class
        assert bool(cols["sampled"][i]) == answer.sampled
        assert (cols["price"][i] == answer.price) or (
            math.isnan(cols["price"][i]) and math.isnan(answer.price))

    ticks = [av.TickStatus(int(t)) for t in cols["tick_status"]]
    # A sampled ladder answer is an estimate (Fallback); an unknown expiry is a refusal.
    assert ticks[0] == (av.TickStatus.FALLBACK if cols["sampled"][0] else av.TickStatus.OK)
    assert ticks[3] == av.TickStatus.UNPRICEABLE
    assert av.ServeStatus(int(cols["status"][3])) == av.ServeStatus.TIER_UNAVAILABLE

    with pytest.raises(ValueError, match="same length"):
        reader.serve_chain(100.0, 0, ids, strikes[:2], PUT)


def test_serve_build_publishes_two_phases_and_transactions_retire(fit):
    book = av.ServeBook(av.ServeBookPolicy(primary_tier_max=av.ServeTier.FAMILY))
    stage = av.serve_build(book, av.ServeBookEntry(3), fit)
    assert (stage.first_version, stage.final_version) == (1, 2)
    assert stage.record is not None and book.version == 2
    assert book.current().find(3).record is not None
    assert book.request_rebuild(3) == av.ServeEnqueue.NO_REBUILD
    tx = book.transaction()
    tx.retire(3)
    assert tx.commit() == 3
    assert book.current().entries == [] and book.current().find(3) is None
    assert book.counters().published == 3


def test_every_serve_enum_names_every_library_value():
    for name in ("ServeTier", "ServeStatus", "ServeRoute", "ServeBoundClass",
                 "ServeGreekReason", "ServeUnsupportedReason", "ServeRebuildReason",
                 "ServeEnqueue"):
        enum = getattr(av, name)
        library = av._core._enum_names(name)
        assert set(library) == {int(v) for v in enum.__members__.values()}, name
