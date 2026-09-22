"""Reported EPS through the initialized, migrated warehouse schema."""

from __future__ import annotations

import datetime as dt
import json

import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.fundamental_statements import refresh_fundamental_statement_points
from atx_db.fundamentals import refresh_fundamental_fact_revisions
from atx_db.standardization import (
    FundamentalStandardizationOptions,
    refresh_fundamental_standardized,
)
from atx_db.warehouse import cik_security_id


RELEASE_SOURCE = "SEC 8-K Item 2.02 reported earnings release"
TARGET = dt.date(2025, 3, 31)
RELEASE_AT = dt.datetime(2025, 5, 3, 22)
DIRECT_AT = dt.datetime(2025, 5, 20, 22)


def _issuer(store, security_id: str, symbol: str) -> None:
    store.con.execute(
        "INSERT INTO securities (security_id, primary_symbol, name, source) VALUES (?, ?, ?, 'fixture')",
        [security_id, symbol, symbol],
    )


def _companyfact(
    store,
    security_id: str,
    cik: str,
    start: str,
    end: str,
    value: float,
    at: dt.datetime,
    accession: str,
    *,
    concept: str = "EarningsPerShareDiluted",
) -> None:
    period_end = dt.date.fromisoformat(end)
    store.con.execute(
        """
        INSERT INTO sec_company_facts (
            source, security_id, cik, taxonomy, concept, unit, period_start, period_end,
            filed_date, fiscal_year, fiscal_period, form, accession_number, value,
            available_at, source_url, source_loaded_at
        ) VALUES (
            'SEC companyfacts', ?, ?, 'us-gaap', ?, 'USD/shares', ?, ?, ?, ?, ?,
            '10-Q', ?, ?, ?, 'https://example.invalid/companyfacts', ?
        )
        """,
        [
            security_id, cik, concept, start, period_end, at.date() - dt.timedelta(days=2),
            period_end.year, f"Q{(period_end.month - 1) // 3 + 1}",
            accession, value, at, at,
        ],
    )


def _release(
    store,
    fact_id: str,
    *,
    cik: str = "0000000001",
    period_start: str = "2025-01-01",
    period_end: str = "2025-03-31",
    duration: str = "three_months_explicit",
    value: float = 1.5,
    at: dt.datetime = RELEASE_AT,
    preliminary: bool = True,
    confidence: float = 1.0,
    receipt_outcome: str = "accepted",
) -> None:
    receipt_id = f"receipt-{fact_id}"
    accession = f"accession-{fact_id}"
    document_sha = "a" * 64
    source_security = cik_security_id(cik) if cik.isdecimal() else "SEC-CIK-MALFORMED"
    payload = json.dumps(
        {
            "receipt_id": receipt_id,
            "document_sha256": document_sha,
            "duration_evidence": duration,
            "period_start": period_start,
            "period_start_basis": "explicit_table_header",
            "fiscal_period_evidence": "Q1 2025 table heading",
        },
        sort_keys=True,
    )
    store.con.execute(
        """
        INSERT INTO sec_earnings_release_receipts (
            receipt_id, cik, source_security_id, accession_number, document_name,
            filing_date, report_date, timestamp_zone_status, available_at,
            document_sha256, outcome, rejection_reason, source_url, retrieval_at
        ) VALUES (?, ?, ?, ?, 'ex99.htm', DATE '2025-05-01',
                  DATE '2025-05-01', 'sec_filed_date_plus_46h_v1',
                  ?, ?, ?, ?, 'https://example.invalid/ex99.htm', ?)
        """,
        [
            receipt_id, cik, source_security, accession, at, document_sha,
            receipt_outcome, None if receipt_outcome == "accepted" else "fixture rejection", at,
        ],
    )
    store.con.execute(
        """
        INSERT INTO press_release_facts (
            press_release_fact_id, source, security_id, cik, accession_number, form,
            source_item, source_url, measure_code, fiscal_year, fiscal_period,
            period_end, value, unit, basis, is_preliminary, extraction_confidence,
            filing_date, release_date, as_of_date, available_at, raw_payload_json, run_id
        ) VALUES (
            ?, ?, ?, ?, ?, '8-K', '2.02 / EX-99', 'https://example.invalid/ex99.htm',
            'EPS_DILUTED', 2025, 'Q1', ?, ?, 'USD_PER_SHARE', 'GAAP', ?, ?,
            DATE '2025-05-01', DATE '2025-05-01', ?, ?, ?, 'fixture'
        )
        """,
        [
            fact_id, RELEASE_SOURCE, source_security, cik, accession,
            dt.date.fromisoformat(period_end), value, preliminary, confidence,
            dt.date.fromisoformat(period_end), at, payload,
        ],
    )


def _refresh(store) -> None:
    refresh_fundamental_fact_revisions(store)
    refresh_fundamental_statement_points(store)
    refresh_fundamental_standardized(
        store, FundamentalStandardizationOptions(symbols=("EPS", "ORD"))
    )
    refresh_derived_metrics(
        store,
        DerivedMetricsOptions(
            metric_codes=("eps_diluted_q_growth_yoy",),
            security_ids=("owner-eps", "owner-ordinary"),
        ),
    )


def _standardized(store, security_id: str, at: dt.datetime) -> tuple:
    return store.con.execute(
        """
        SELECT value, available_at, input_codes_json, is_latest_revision
        FROM fundamental_standardized
        WHERE security_id = ? AND canonical_code = 'eps_diluted'
          AND basis = 'quarterly' AND period_end = ?
          AND available_at <= ?
        ORDER BY available_at DESC, standardized_id DESC LIMIT 1
        """,
        [security_id, TARGET, at],
    ).fetchone()


def _growth(store, security_id: str, at: dt.datetime) -> tuple:
    return store.con.execute(
        """
        SELECT value, value_status, available_at
        FROM derived_metric_values
        WHERE security_id = ? AND metric_code = 'eps_diluted_q_growth_yoy'
          AND period_end = ? AND available_at <= ?
        ORDER BY available_at DESC, derived_value_id DESC LIMIT 1
        """,
        [security_id, TARGET, at],
    ).fetchone()


def _seed_history(store) -> None:
    _issuer(store, "owner-eps", "EPS")
    _issuer(store, "owner-ordinary", "ORD")
    quarters = (
        ("2024-01-01", "2024-03-31", 1.0, "2024-05-03"),
        ("2024-04-01", "2024-06-30", 1.1, "2024-08-03"),
        ("2024-07-01", "2024-09-30", 1.2, "2024-11-03"),
        ("2024-10-01", "2024-12-31", 1.3, "2025-02-03"),
    )
    for index, (start, end, value, clock) in enumerate(quarters):
        at = dt.datetime.fromisoformat(clock + "T22:00:00")
        _companyfact(store, "owner-eps", "0000000001", start, end, value, at, f"eps-{index}")
        _companyfact(
            store, "owner-ordinary", "0000000002", start, end,
            value + 1.0, at, f"ordinary-{index}",
        )
    _companyfact(
        store, "owner-ordinary", "0000000002", "2025-01-01", "2025-03-31",
        2.5, RELEASE_AT, "ordinary-target",
    )


def test_late_direct_conflict_reaches_standardized_and_derived_null(tmp_store) -> None:
    store = tmp_store
    _seed_history(store)
    _release(store, "release-target")
    _refresh(store)
    release = _standardized(store, "owner-eps", RELEASE_AT)
    assert release[:2] == (1.5, RELEASE_AT)
    assert _standardized(store, "owner-eps", RELEASE_AT - dt.timedelta(microseconds=1)) is None
    assert _growth(store, "owner-eps", RELEASE_AT)[:2] == (pytest.approx(0.5), "valid")
    ordinary_before = (
        _standardized(store, "owner-ordinary", DIRECT_AT),
        _growth(store, "owner-ordinary", DIRECT_AT),
    )

    _companyfact(
        store, "owner-eps", "0000000001", "2025-01-01", "2025-03-31",
        1.55, DIRECT_AT, "direct-conflict",
    )
    _refresh(store)
    assert _standardized(store, "owner-eps", RELEASE_AT)[:2] == (1.5, RELEASE_AT)
    conflict = _standardized(store, "owner-eps", DIRECT_AT)
    assert conflict[0] is None and conflict[1] == DIRECT_AT and conflict[3]
    assert "direct_statement_point_id=" in conflict[2]
    assert "release_statement_point_id=" in conflict[2]
    assert "direct_accession=direct-conflict" in conflict[2]
    assert "release_accession=accession-release-target" in conflict[2]
    assert _growth(store, "owner-eps", DIRECT_AT)[0] is None
    assert store.con.execute(
        """
        SELECT count(*) FROM fundamental_standardization_exception
        WHERE security_id = 'owner-eps' AND reason = 'reported_eps_conflict'
          AND period_end = ? AND available_at = ?
        """,
        [TARGET, DIRECT_AT],
    ).fetchone()[0] == 1
    assert ordinary_before == (
        _standardized(store, "owner-ordinary", DIRECT_AT),
        _growth(store, "owner-ordinary", DIRECT_AT),
    )
    snapshot = store.con.execute(
        """
        SELECT standardized_id, value, available_at
        FROM fundamental_standardized
        WHERE security_id = 'owner-eps' AND canonical_code = 'eps_diluted'
        ORDER BY standardized_id
        """
    ).fetchall()
    _refresh(store)
    assert store.con.execute(
        """
        SELECT standardized_id, value, available_at
        FROM fundamental_standardized
        WHERE security_id = 'owner-eps' AND canonical_code = 'eps_diluted'
        ORDER BY standardized_id
        """
    ).fetchall() == snapshot


def test_equal_clock_conflict_and_qualified_source_rows(tmp_store) -> None:
    store = tmp_store
    _seed_history(store)
    _release(store, "equal")
    _companyfact(
        store, "owner-eps", "0000000001", "2025-01-01", "2025-03-31",
        1.51, RELEASE_AT, "direct-equal",
    )
    _refresh(store)
    assert _standardized(store, "owner-eps", RELEASE_AT)[0] is None
    assert _growth(store, "owner-eps", RELEASE_AT)[0] is None
    assert store.con.execute(
        "SELECT count(*) FROM fundamental_statement_points WHERE source = ? AND accession_number = ?",
        [RELEASE_SOURCE, "accession-equal"],
    ).fetchone()[0] == 1


@pytest.mark.parametrize(
    ("fact_id", "cik", "start", "end", "duration", "preliminary", "confidence", "outcome", "accepted"),
    [
        ("13w", "0000000001", "2025-01-01", "2025-04-01", "thirteen_weeks_explicit", True, 1.0, "accepted", True),
        ("14w", "0000000001", "2025-01-01", "2025-04-08", "fourteen_weeks_explicit", True, 1.0, "accepted", True),
        ("wrong-span", "0000000001", "2025-01-02", "2025-04-01", "thirteen_weeks_explicit", True, 1.0, "accepted", False),
        ("malformed-cik", "00000000001", "2025-01-01", "2025-04-01", "thirteen_weeks_explicit", True, 1.0, "accepted", False),
        ("cross-cik", "0000000003", "2025-01-01", "2025-04-01", "thirteen_weeks_explicit", True, 1.0, "accepted", False),
        ("nonpreliminary", "0000000001", "2025-01-01", "2025-04-01", "thirteen_weeks_explicit", False, 1.0, "accepted", False),
        ("weak", "0000000001", "2025-01-01", "2025-04-01", "thirteen_weeks_explicit", True, 0.9, "accepted", False),
        ("rejected", "0000000001", "2025-01-01", "2025-04-01", "thirteen_weeks_explicit", True, 1.0, "rejected", False),
    ],
)
def test_admission_uses_migrated_tables_and_exact_duration(
    tmp_store, fact_id, cik, start, end, duration, preliminary, confidence, outcome, accepted
) -> None:
    store = tmp_store
    _issuer(store, "owner-eps", "EPS")
    _companyfact(
        store, "owner-eps", "0000000001", "2024-01-01", "2024-03-31",
        1.0, dt.datetime(2024, 5, 3, 22), "owner-fact",
    )
    _release(
        store, fact_id, cik=cik, period_start=start, period_end=end,
        duration=duration, preliminary=preliminary, confidence=confidence,
        receipt_outcome=outcome,
    )
    refresh_fundamental_fact_revisions(store)
    refresh_fundamental_statement_points(store)
    rows = store.con.execute(
        "SELECT period_start, period_end FROM fundamental_statement_points "
        "WHERE source = ? AND accession_number = ?",
        [RELEASE_SOURCE, f"accession-{fact_id}"],
    ).fetchall()
    assert rows == (
        [(dt.date.fromisoformat(start), dt.date.fromisoformat(end))] if accepted else []
    )
