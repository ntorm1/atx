from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.connection import DuckDBStore
from atx_db.fundamentals import _unresolved_cik_candidates, resolve_company_facts_identifiers

_CIK_A = "0000000001"
_CIK_B = "0000000002"
_FALLBACK_A = f"SEC-COMPANYFACTS-UNRESOLVED-CIK-{_CIK_A}"
_FALLBACK_B = f"SEC-COMPANYFACTS-UNRESOLVED-CIK-{_CIK_B}"


@pytest.fixture
def identity_store():
    """Real PIT lookup tables, without bootstrapping unrelated warehouse state."""
    store = DuckDBStore(":memory:")
    store.analytical_memory_limit = "64MB"
    store.analytical_threads = 1
    store.reopen()
    store.con.execute("""CREATE TABLE security_identifier_history (
        security_id VARCHAR, id_type VARCHAR, id_value VARCHAR,
        valid_from DATE, valid_to DATE, available_at TIMESTAMP, source_loaded_at TIMESTAMP)""")
    store.con.execute("""CREATE TABLE sec_company_tickers (
        cik VARCHAR, security_id VARCHAR, source_loaded_at TIMESTAMP)""")
    store.con.execute("""CREATE TABLE securities (
        security_id VARCHAR, entity_id VARCHAR, first_seen_date DATE, source_loaded_at TIMESTAMP)""")
    store.con.execute("""INSERT INTO security_identifier_history VALUES
        ('HIST-A', 'CIK', ?, DATE '2020-01-01', NULL, TIMESTAMP '2023-01-01', TIMESTAMP '2026-09-20'),
        ('HIST-A', 'ENTITY_ID', 'ENTITY-A', DATE '2020-01-01', NULL,
         TIMESTAMP '2024-01-01', TIMESTAMP '2026-09-20')""", [_CIK_A])
    store.con.execute("""INSERT INTO sec_company_tickers VALUES
        (?, 'CURRENT-A', TIMESTAMP '2026-09-20'), (?, 'CURRENT-B', TIMESTAMP '2026-09-20')""",
        [_CIK_A, _CIK_B])
    store.con.execute("""INSERT INTO securities VALUES
        ('CURRENT-A', 'CURRENT-ENTITY-A', DATE '2020-01-01', TIMESTAMP '2026-09-20'),
        ('CURRENT-B', 'CURRENT-ENTITY-B', DATE '2020-01-01', TIMESTAMP '2026-09-20')""")
    try:
        yield store
    finally:
        store.close()


def test_summary_preserves_pit_multiplicity_first_representative_and_candidate_counts(identity_store):
    # The first A row resolves only its security. Its older unresolved filing
    # comes later, so choosing min(available_at) would change the candidate.
    facts = pd.DataFrame([
        (_CIK_A, _FALLBACK_A, pd.Timestamp("2023-06-01 22:00"), 11.0),
        (_CIK_B, _FALLBACK_B, pd.Timestamp("2022-06-01 22:00"), 21.0),
        (_CIK_A, _FALLBACK_A, pd.Timestamp("2022-06-01 22:00"), 12.0),
        (_CIK_A, _FALLBACK_A, pd.Timestamp("2023-06-01 22:00"), 11.0),
        (_CIK_B, _FALLBACK_B, pd.Timestamp("2022-06-01 22:00"), 21.0),
        (_CIK_A, _FALLBACK_A, pd.Timestamp("2024-06-01 22:00"), 13.0),
        (_CIK_A, _FALLBACK_A, pd.NaT, 14.0),
    ], columns=["cik", "security_id", "available_at", "value"], index=[8, 2, 5, 1, 3, 9, 7])
    original = facts.copy(deep=True)

    resolved, unresolved = resolve_company_facts_identifiers(
        identity_store, facts, allow_current_fallback=False,
    )

    pd.testing.assert_frame_equal(facts, original)
    pd.testing.assert_frame_equal(resolved[["cik", "available_at", "value"]], original.drop(columns="security_id"))
    assert resolved["security_id"].tolist() == [
        "HIST-A", _FALLBACK_B, _FALLBACK_A, "HIST-A", _FALLBACK_B, "HIST-A", _FALLBACK_A,
    ]
    assert resolved["entity_id"].fillna("missing").tolist() == [
        "missing", "missing", "missing", "missing", "missing", "ENTITY-A", "missing",
    ]
    expected = pd.DataFrame([
        (_CIK_A, "HIST-A", pd.Timestamp("2023-06-01 22:00"), 2, 4, 4),
        (_CIK_B, _FALLBACK_B, pd.Timestamp("2022-06-01 22:00"), 2, 2, 2),
    ], columns=["cik", "security_id", "available_at", "security_unresolved_count",
                "entity_unresolved_count", "fact_count"])
    pd.testing.assert_frame_equal(unresolved, expected)

    candidates = _unresolved_cik_candidates(unresolved, run_id="summary-check", as_of_date=dt.date(2026, 9, 20))
    assert candidates["source_key_value"].tolist() == [_CIK_A, _CIK_B]
    assert candidates["source_security_id"].tolist() == ["HIST-A", _FALLBACK_B]
    assert candidates["candidate_status"].tolist() == ["proposed", "proposed"]
    assert [json.loads(value) for value in candidates["details_json"]] == [
        {"status_reason": "unresolved_cik_or_entity_at_fact_available_at",
         "unresolved_security_fact_rows": 2, "unresolved_entity_fact_rows": 4,
         "fact_available_at": "2023-06-01 22:00:00"},
        {"status_reason": "unresolved_cik_or_entity_at_fact_available_at",
         "unresolved_security_fact_rows": 2, "unresolved_entity_fact_rows": 2,
         "fact_available_at": "2022-06-01 22:00:00"},
    ]


@pytest.mark.parametrize("allow_current_fallback", [False, True])
def test_missing_first_clock_stays_missing_and_existing_fallback_policy_is_preserved(
        identity_store, allow_current_fallback):
    facts = pd.DataFrame([
        (_CIK_B, _FALLBACK_B, pd.NaT),
        (_CIK_A, _FALLBACK_A, pd.Timestamp("2023-06-01 22:00")),
        (_CIK_B, _FALLBACK_B, pd.Timestamp("2024-06-01 22:00")),
    ], columns=["cik", "security_id", "available_at"])
    resolved, unresolved = resolve_company_facts_identifiers(
        identity_store, facts, allow_current_fallback=allow_current_fallback,
    )
    assert resolved.loc[0, "security_id"] == _FALLBACK_B
    assert pd.isna(resolved.loc[0, "entity_id"])
    assert unresolved.loc[0, "cik"] == _CIK_B
    assert unresolved.loc[0, "security_id"] == _FALLBACK_B
    assert pd.isna(unresolved.loc[0, "available_at"])
    if allow_current_fallback:
        assert resolved.loc[2, "security_id"] == "CURRENT-B"
        assert resolved.loc[2, "entity_id"] == "CURRENT-ENTITY-B"
        # Existing non-archive behavior does not propose an entity-only failure.
        assert unresolved["cik"].tolist() == [_CIK_B]
        assert unresolved.loc[0, "fact_count"] == 1
    else:
        assert resolved.loc[2, "security_id"] == _FALLBACK_B
        assert pd.isna(resolved.loc[2, "entity_id"])
        assert unresolved["cik"].tolist() == [_CIK_B, _CIK_A]
        assert unresolved.loc[0, "fact_count"] == 2
        assert unresolved.loc[1, "security_unresolved_count"] == 0
        assert unresolved.loc[1, "entity_unresolved_count"] == 1


def test_fully_resolved_facts_keep_the_empty_summary_contract(identity_store):
    facts = pd.DataFrame([{
        "cik": _CIK_A, "security_id": _FALLBACK_A, "available_at": pd.Timestamp("2024-06-01 22:00"),
    }])
    resolved, unresolved = resolve_company_facts_identifiers(
        identity_store, facts, allow_current_fallback=False,
    )
    assert resolved.loc[0, "security_id"] == "HIST-A"
    assert resolved.loc[0, "entity_id"] == "ENTITY-A"
    assert unresolved.empty
    assert unresolved.columns.tolist() == ["cik", "security_id", "available_at"]


def test_empty_facts_keep_the_existing_empty_contract(identity_store):
    facts = pd.DataFrame(columns=["cik", "security_id", "available_at", "value"])
    resolved, unresolved = resolve_company_facts_identifiers(
        identity_store, facts, allow_current_fallback=False,
    )
    pd.testing.assert_frame_equal(resolved, facts)
    assert unresolved.empty
    assert unresolved.columns.tolist() == ["cik", "security_id", "available_at"]
