"""The quote-slice repair / box-certificate config and records through Python (lane PY).

Lane C4 (r6) added an opt-in exact in-box projection rung under the box certificate
(QuoteSliceRepairConfig.inbox_projection; QuoteSliceRepairRecord.inbox). The library
behaviour is pinned by atx-vol/tests/quote_slice_test.cpp and the inbox projection tests.
This file pins the binding: the sub-config is reachable and edits in place, an invalid one
is a typed refusal before any fit, and a fit exposes both rung records with their typed
outcomes (NOT_ATTEMPTED when the certificate passes, per the header contract).
"""

from __future__ import annotations

import pytest

import atxvol as av
from test_quote_slice import _wing_chain


def _config(certificate=av.QuoteSliceCertificate.EXACT, inbox=False):
    config = av.QuoteSliceConfig()
    config.floor_policy = av.QuoteSliceFloorPolicy.PRICE_ONLY
    config.repair.certificate = certificate
    config.repair.inbox_projection = inbox
    return config


def test_the_repair_sub_config_has_its_library_defaults_and_edits_in_place():
    config = av.QuoteSliceConfig()
    repair = config.repair
    assert repair.mode == av.QuoteSliceRepairMode.OFF
    assert repair.certificate == av.QuoteSliceCertificate.EXACT
    assert repair.inbox_projection is False
    assert repair.wing_mult_source == av.QuoteSliceWingMultSource.POLICY
    assert repair.market_distance == av.QuoteSliceMarketDistance.OFF
    assert (repair.max_nodes, repair.joint_max_sweeps) == (16, 20000)
    config.repair.inbox_projection = True
    config.repair.certificate = av.QuoteSliceCertificate.BOX
    assert config.repair.inbox_projection is True
    assert config.repair.certificate == av.QuoteSliceCertificate.BOX
    av.validate_quote_slice_repair(config.repair)


@pytest.mark.parametrize("certificate,inbox", [
    (av.QuoteSliceCertificate.EXACT, False),
    (av.QuoteSliceCertificate.BOX, False),
    (av.QuoteSliceCertificate.BOX, True),
])
def test_a_clean_fit_records_every_rung_as_not_attempted(certificate, inbox):
    market, quotes = _wing_chain()
    fit = av.fit_quote_slice(market, quotes, _config(certificate, inbox))
    assert fit.fit_status == av.QuoteSliceFitStatus.SUCCESS, fit.fit_error
    for record in (fit.repair_record, fit.box_record):
        assert record.outcome == av.QuoteSliceRepairOutcome.NOT_ATTEMPTED
        assert record.nodes == 0 and record.max_price_move == 0.0
        assert set(record.conflict) == {"kind", "slice", "node", "other", "strike", "excess"}
    inbox_record = fit.box_record.inbox
    # C4: NotAttempted when the key is off or the box certificate passed.
    assert inbox_record.outcome == av.QuoteSliceInboxOutcome.NOT_ATTEMPTED
    assert inbox_record.box_refusal == av.QuoteSliceBoxRefusal.NONE
    assert inbox_record.conflict == av.QuoteSliceInboxConflict.NONE


def test_an_invalid_repair_config_is_refused_before_any_fit():
    bad = av.QuoteSliceRepairConfig()
    bad.box_tick = 0.0
    with pytest.raises(av.AtxError) as raised:
        av.validate_quote_slice_repair(bad)
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT
    config = av.QuoteSliceConfig()
    config.repair = bad
    market, quotes = _wing_chain()
    with pytest.raises(av.AtxError) as raised:
        av.fit_quote_slice(market, quotes, config)
    assert raised.value.code == av.ErrorCode.INVALID_ARGUMENT
