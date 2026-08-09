from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


# ---------------------------------------------------------------------------
# S5-1 fix: an UNRESOLVED-CUSIP-* (or any other non-existent) target_security_id
# must never be applied into security_identifier_history, even if some future
# decision-maker marks the row "accepted". _apply_accepted_identifiers is the
# consumption point; it must fail loudly (raise) rather than silently insert
# a fake security_id key.
# ---------------------------------------------------------------------------


def _seed_securities(store) -> None:
    store.con.execute(
        """
        INSERT INTO securities (
            security_id, entity_id, issuer_id, primary_symbol, name, asset_class,
            country, currency, active, first_seen_date, last_seen_date, source
        )
        VALUES
            ('SEC-CIK-0000320193', 'CIK-0000320193', 'CIK-0000320193', 'AAPL', 'Apple Inc.', 'EQUITY', 'US', 'USD', true, DATE '2019-05-04', NULL, 'fixture')
        """
    )


def _accepted_decision_frame(target_security_id: str) -> pd.DataFrame:
    as_of = dt.date(2026, 6, 1)
    available_at = dt.datetime(2026, 6, 1, 22, 0, 0)
    return pd.DataFrame(
        [
            {
                "decision_id": "decision-1",
                "candidate_id": "candidate-1",
                "source_dataset_id": "identifiers_figi",
                "source_table": "security_identifier_history",
                "source_period": None,
                "source_key_type": "CUSIP",
                "source_key_value": "037833100",
                "source_security_id": None,
                "target_security_id": target_security_id,
                "target_id_type": "FIGI",
                "target_id_value": "BBG000B9XRY4",
                "match_method": "openfigi_cusip_mapping",
                "confidence": 0.5,
                "candidate_status": "proposed",
                "decision_status": "accepted",
                "decision_method": "manual_override",
                "decided_by": "test:manual",
                "decided_at": dt.datetime(2026, 6, 1, 12, 0, 0),
                "effective_from": as_of,
                "as_of_date": as_of,
                "available_at": available_at,
                "notes_json": None,
                "run_id": None,
            }
        ]
    )


def test_apply_accepted_identifiers_rejects_unresolved_cusip_sentinel(tmp_store):
    """RED: a hand-accepted row carrying the UNRESOLVED-CUSIP-* sentinel as
    target_security_id must not be written into security_identifier_history --
    it must raise instead of silently inserting a fake security_id key."""
    from atx_db.identifier_decisions import (
        IdentifierResolutionDecisionDataset,
        IdentifierResolutionDecisionOptions,
    )

    _seed_securities(tmp_store)
    frame = _accepted_decision_frame("UNRESOLVED-CUSIP-037833100")
    dataset = IdentifierResolutionDecisionDataset()
    options = IdentifierResolutionDecisionOptions(run_id=None)

    with pytest.raises(ValueError):
        dataset._apply_accepted_identifiers(tmp_store, frame, options)

    rows = tmp_store.con.execute(
        "SELECT count(*) FROM security_identifier_history WHERE security_id = 'UNRESOLVED-CUSIP-037833100'"
    ).fetchone()[0]
    assert rows == 0


def test_apply_accepted_identifiers_accepts_real_security_id(tmp_store):
    """Control: a legitimate target_security_id that exists in securities is
    still applied normally -- the guard must not reject real rows."""
    from atx_db.identifier_decisions import (
        IdentifierResolutionDecisionDataset,
        IdentifierResolutionDecisionOptions,
    )

    _seed_securities(tmp_store)
    frame = _accepted_decision_frame("SEC-CIK-0000320193")
    dataset = IdentifierResolutionDecisionDataset()
    options = IdentifierResolutionDecisionOptions(run_id=None)

    applied = dataset._apply_accepted_identifiers(tmp_store, frame, options)
    assert applied == 1

    rows = tmp_store.con.execute(
        "SELECT security_id, id_type, id_value FROM security_identifier_history WHERE source = 'atx-db identifier decision manager'"
    ).fetchall()
    assert rows == [("SEC-CIK-0000320193", "CUSIP", "037833100")]


# ---------------------------------------------------------------------------
# S5-4 hardening (folded from S5-1 review): _apply_accepted_13f_holdings
# writes target_security_id directly into thirteenf_holdings.security_id with
# NO sentinel/existence check -- the same corruption exposure S5-1 closed for
# _apply_accepted_identifiers via _reject_unresolvable_targets. Apply the same
# guard to this sink too: reject the UNRESOLVED-CUSIP-* sentinel (and any
# other target_security_id absent from securities.security_id) before the
# UPDATE, rather than silently corrupting thirteenf_holdings.security_id.
# ---------------------------------------------------------------------------


def _seed_13f_holding(store, *, cusip: str, source_period: str) -> None:
    from atx_db.thirteenf import ThirteenFDataSet

    ThirteenFDataSet().ensure_schema(store)
    store.con.execute(
        """
        INSERT INTO thirteenf_holdings (
            accession_number, security_id, infotable_sk, name_of_issuer,
            title_of_class, cusip, source_period
        )
        VALUES ('0000320193-26-000001', NULL, 1, 'Apple Inc.', 'Common Stock', ?, ?)
        """,
        [cusip, source_period],
    )


def _accepted_13f_decision_frame(target_security_id: str) -> pd.DataFrame:
    as_of = dt.date(2026, 6, 1)
    available_at = dt.datetime(2026, 6, 1, 22, 0, 0)
    return pd.DataFrame(
        [
            {
                "decision_id": "decision-13f-1",
                "candidate_id": "candidate-13f-1",
                "source_dataset_id": "sec_13f",
                "source_table": "thirteenf_holdings",
                "source_period": "2026Q1",
                "source_key_type": "CUSIP",
                "source_key_value": "037833100",
                "source_security_id": None,
                "target_security_id": target_security_id,
                "target_id_type": "FIGI",
                "target_id_value": "BBG000B9XRY4",
                "match_method": "openfigi_cusip_mapping",
                "confidence": 0.5,
                "candidate_status": "proposed",
                "decision_status": "accepted",
                "decision_method": "manual_override",
                "decided_by": "test:manual",
                "decided_at": dt.datetime(2026, 6, 1, 12, 0, 0),
                "effective_from": as_of,
                "as_of_date": as_of,
                "available_at": available_at,
                "notes_json": None,
                "run_id": None,
            }
        ]
    )


def test_apply_accepted_13f_holdings_rejects_unresolved_cusip_sentinel(tmp_store):
    """RED: a hand-accepted row carrying the UNRESOLVED-CUSIP-* sentinel as
    target_security_id must not be written into thirteenf_holdings.security_id
    -- it must raise instead of silently corrupting the holding's security_id
    with a fake key."""
    from atx_db.identifier_decisions import IdentifierResolutionDecisionDataset

    _seed_securities(tmp_store)
    _seed_13f_holding(tmp_store, cusip="037833100", source_period="2026Q1")
    frame = _accepted_13f_decision_frame("UNRESOLVED-CUSIP-037833100")
    dataset = IdentifierResolutionDecisionDataset()

    with pytest.raises(ValueError):
        dataset._apply_accepted_13f_holdings(tmp_store, frame)

    row = tmp_store.con.execute(
        "SELECT security_id FROM thirteenf_holdings WHERE cusip = '037833100' AND source_period = '2026Q1'"
    ).fetchone()
    assert row[0] is None


def test_apply_accepted_13f_holdings_accepts_real_security_id(tmp_store):
    """Control: a legitimate target_security_id that exists in securities is
    still applied normally -- the guard must not reject real rows."""
    from atx_db.identifier_decisions import IdentifierResolutionDecisionDataset

    _seed_securities(tmp_store)
    _seed_13f_holding(tmp_store, cusip="037833100", source_period="2026Q1")
    frame = _accepted_13f_decision_frame("SEC-CIK-0000320193")
    dataset = IdentifierResolutionDecisionDataset()

    updated = dataset._apply_accepted_13f_holdings(tmp_store, frame)
    assert updated == 1

    row = tmp_store.con.execute(
        "SELECT security_id FROM thirteenf_holdings WHERE cusip = '037833100' AND source_period = '2026Q1'"
    ).fetchone()
    assert row[0] == "SEC-CIK-0000320193"
