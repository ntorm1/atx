"""Reported EPS through the initialized, migrated warehouse schema."""

from __future__ import annotations

import calendar
import datetime as dt
import json

import pytest

from atx_db import press_release
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.fundamental_statements import refresh_fundamental_statement_points
from atx_db.fundamentals import refresh_fundamental_fact_revisions
from atx_db.press_release import SecEarningsReleaseOptions, refresh_sec_earnings_release_facts
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
    measure: str = "EPS_DILUTED",
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
            filing_date, release_date, as_of_date, available_at, raw_payload_json, run_id,
            input_codes_json
        ) VALUES (
            ?, ?, ?, ?, ?, '8-K', '2.02 / EX-99', 'https://example.invalid/ex99.htm',
            ?, 2025, 'Q1', ?, ?, 'USD_PER_SHARE', 'GAAP', ?, ?,
            DATE '2025-05-01', DATE '2025-05-01', ?, ?, ?, 'fixture', '[]'
        )
        """,
        [
            fact_id, RELEASE_SOURCE, source_security, cik, accession, measure,
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


def test_basic_release_projects_as_basic_concept_and_audits_combined_direct_line(tmp_store) -> None:
    """Basic EPS uses its own item; a diluted value is never its counterparty."""

    store = tmp_store
    _issuer(store, "owner-eps", "EPS")
    _companyfact(
        store, "owner-eps", "0000000001", "2024-01-01", "2024-03-31",
        1.0, dt.datetime(2024, 5, 3, 22), "owner-fact",
    )
    # A different diluted value for the same quarter is not a basic conflict.
    _companyfact(
        store, "owner-eps", "0000000001", "2025-01-01", "2025-03-31",
        1.40, RELEASE_AT - dt.timedelta(days=1), "direct-diluted",
    )
    _release(store, "basic-target", value=1.5, measure="EPS_BASIC")
    _release(store, "not-eps", value=9.0, measure="REVENUE")
    _companyfact(
        store, "owner-eps", "0000000001", "2025-01-01", "2025-03-31",
        1.45, DIRECT_AT, "direct-combined", concept="EarningsPerShareBasicAndDiluted",
    )
    refresh_fundamental_fact_revisions(store)
    refresh_fundamental_statement_points(store)
    assert store.con.execute(
        """SELECT concept, canonical_metric, item_id, value FROM fundamental_statement_points
           WHERE source = ? ORDER BY accession_number""",
        [RELEASE_SOURCE],
    ).fetchall() == [("EarningsPerShareBasic", "eps_basic", 1034, 1.5)]
    refresh_fundamental_standardized(store, FundamentalStandardizationOptions(symbols=("EPS",)))

    def basic(at: dt.datetime) -> tuple | None:
        return store.con.execute(
            """SELECT value, available_at, input_codes_json FROM fundamental_standardized
               WHERE security_id = 'owner-eps' AND canonical_code = 'eps_basic__1034'
                 AND basis = 'quarterly' AND period_end = ? AND available_at <= ?
               ORDER BY available_at DESC, standardized_id DESC LIMIT 1""",
            [TARGET, at],
        ).fetchone()

    assert basic(RELEASE_AT - dt.timedelta(microseconds=1)) is None
    assert basic(RELEASE_AT)[:2] == (1.5, RELEASE_AT)
    conflict = basic(DIRECT_AT)
    assert conflict[0] is None and conflict[1] == DIRECT_AT
    assert "direct_accession=direct-combined" in conflict[2]
    assert "release_accession=accession-basic-target" in conflict[2]
    assert store.con.execute(
        """SELECT value FROM fundamental_standardized
           WHERE security_id = 'owner-eps' AND canonical_code = 'eps_diluted'
             AND basis = 'quarterly' AND period_end = ?""",
        [TARGET],
    ).fetchall() == [(1.40,)]


# ---------------------------------------------------------------------------
# Eight quarters of governed basic + diluted Item 2.02 releases, end to end:
# SEC receipt -> press_release_facts -> PIT bridge -> standardized -> growth.

A7_CIK = "0000000777"
A7_OWNER = "owner-a7"
A7_SYMBOL = "ASEV"
A7_CUTOFF = dt.datetime(2025, 5, 15)
A7_AFTER_AMENDMENT = dt.datetime(2025, 7, 1)
# (period end, results title, basic, diluted, prior-year basic, prior-year diluted, filed)
A7_RELEASES = (
    ("2023-03-31", "First Quarter 2023", "0.50", "0.48", "0.45", "0.44", "2023-04-25"),
    ("2023-06-30", "Second Quarter 2023", "(0.40)", "(0.40)", "0.10", "0.10", "2023-07-25"),
    ("2023-09-30", "Third Quarter 2023", "0.00", "0.00", "0.20", "0.19", "2023-10-25"),
    # No Q4 2023 release. The next Q4 release shows a Q4 2023 comparative
    # column, which stays payload evidence and never becomes a quarter.
    # Diluted shares rise ~20% in 2024 while basic shares do not.
    ("2024-03-31", "First Quarter 2024", "0.60", "0.50", "0.50", "0.48", "2024-04-25"),
    ("2024-06-30", "Second Quarter 2024", "0.20", "0.18", "(0.40)", "(0.40)", "2024-07-25"),
    ("2024-09-30", "Third Quarter 2024", "0.30", "0.27", "0.00", "0.00", "2024-10-25"),
    # The fiscal year end moves from December to September: new labels.
    ("2024-12-31", "First Quarter Fiscal 2025", "0.40", "0.36", "0.20", "0.19", "2025-01-27"),
    ("2025-03-31", "Second Quarter Fiscal 2025", "0.72", "0.60", "0.60", "0.50", "2025-04-25"),
)
# (start, end, basic, diluted, filed, accession, fiscal year, fiscal period, form)
A7_COMPANYFACTS = (
    # Nine-month YTD and annual EPS: FY - 9M must never become a Q4 EPS.
    ("2023-01-01", "2023-09-30", 0.10, 0.08, "2023-11-06", "cf-2023q3", 2023, "Q3", "10-Q"),
    ("2023-01-01", "2023-12-31", 0.30, 0.28, "2024-02-20", "cf-2023fy", 2023, "FY", "10-K"),
    # The Q1 2024 10-Q agrees with the release ...
    ("2024-01-01", "2024-03-31", 0.60, 0.50, "2024-05-06", "cf-2024q1", 2024, "Q1", "10-Q"),
    # ... and a 10-Q/A filed after the cutoff restates basic EPS only.
    ("2024-01-01", "2024-03-31", 0.66, 0.50, "2025-06-02", "cf-2024q1-a", 2024, "Q1", "10-Q/A"),
)


class _UrlResponse:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.headers = {"Content-Length": str(len(payload))}

    def raise_for_status(self) -> None:
        return None

    def iter_content(self, chunk_size: int):
        yield self.payload

    def close(self) -> None:
        return None


class _UrlSession:
    def __init__(self, payloads: dict[str, bytes]) -> None:
        self.payloads = payloads

    def get(self, url: str, *, timeout: float, stream: bool) -> _UrlResponse:
        assert timeout > 0 and stream
        return _UrlResponse(self.payloads[url])


def _a7_index(accession: str) -> bytes:
    path = f"/Archives/edgar/data/{int(A7_CIK)}/{accession.replace('-', '')}/ex99.htm"
    return f"""
    <table class="tableFile" summary="Document Format Files">
      <tr><th>Seq</th><th>Description</th><th>Document</th><th>Type</th><th>Size</th></tr>
      <tr><td>1</td><td>EX-99.1</td><td><a href="{path}">ex99.htm</a></td><td>EX-99.1</td><td>100</td></tr>
    </table>
    """.encode()


def _a7_document(end: dt.date, title: str, basic: str, diluted: str, prior_basic: str, prior_diluted: str) -> bytes:
    heading = f"Three Months Ended {calendar.month_name[end.month]} {end.day},"
    return f"""
    <h1>Issuer Reports {title} Results</h1>
    <table>
      <tr><th></th><th colspan="2">{heading}</th></tr>
      <tr><th></th><th>{end.year}</th><th>{end.year - 1}</th></tr>
      <tr><td colspan="3">Earnings per share:</td></tr>
      <tr><td>- Basic</td><td>{basic}</td><td>{prior_basic}</td></tr>
      <tr><td>- Diluted</td><td>{diluted}</td><td>{prior_diluted}</td></tr>
    </table>
    """.encode()


def _a7_companyfact(store, concept: str, start: str, end: str, value: float, filed: str,
                    accession: str, fiscal_year: int, fiscal_period: str, form: str) -> None:
    filed_on = dt.date.fromisoformat(filed)
    at = dt.datetime.combine(filed_on, dt.time()) + dt.timedelta(hours=46)
    store.con.execute(
        """
        INSERT INTO sec_company_facts (
            source, security_id, cik, taxonomy, concept, unit, period_start, period_end,
            filed_date, fiscal_year, fiscal_period, form, accession_number, value,
            available_at, source_url, source_loaded_at
        ) VALUES ('SEC companyfacts', ?, ?, 'us-gaap', ?, 'USD/shares', ?, ?, ?, ?, ?, ?, ?, ?, ?,
                  'https://example.invalid/companyfacts', ?)
        """,
        [A7_OWNER, A7_CIK, concept, dt.date.fromisoformat(start), dt.date.fromisoformat(end),
         filed_on, fiscal_year, fiscal_period, form, accession, value, at, at],
    )


def _a7_seed_and_extract(store, tmp_path) -> dict[str, int]:
    _issuer(store, A7_OWNER, A7_SYMBOL)
    payloads: dict[str, bytes] = {}
    for sequence, (end, title, basic, diluted, prior_basic, prior_diluted, filed) in enumerate(
        A7_RELEASES, start=1
    ):
        period_end, filed_on = dt.date.fromisoformat(end), dt.date.fromisoformat(filed)
        accession = f"{A7_CIK}-{filed_on:%y}-{sequence:06d}"
        store.con.execute(
            """INSERT INTO sec_submissions (
                   security_id, cik, accession_number, filing_date, report_date,
                   acceptance_datetime_raw, form, items, source_url
               ) VALUES (?, ?, ?, ?, ?, ?, '8-K', '2.02;9.01', 'bulk-fixture')""",
            [cik_security_id(A7_CIK), A7_CIK, accession, filed_on, filed_on, f"{filed}T16:30:00-04:00"],
        )
        index_url, directory = press_release._archive_urls(A7_CIK, accession)
        payloads[index_url] = _a7_index(accession)
        payloads[f"{directory}/ex99.htm"] = _a7_document(
            period_end, title, basic, diluted, prior_basic, prior_diluted
        )
    for start, end, basic, diluted, filed, accession, fiscal_year, fiscal_period, form in A7_COMPANYFACTS:
        _a7_companyfact(store, "EarningsPerShareBasic", start, end, basic, filed,
                        accession, fiscal_year, fiscal_period, form)
        _a7_companyfact(store, "EarningsPerShareDiluted", start, end, diluted, filed,
                        accession, fiscal_year, fiscal_period, form)
    return refresh_sec_earnings_release_facts(
        store,
        SecEarningsReleaseOptions(cache_dir=tmp_path / "cache", ciks=(A7_CIK,), run_id="a7-eight-quarters"),
        session=_UrlSession(payloads),
    )


def _a7_growth_state(store, metric: str, period_end: str, at: dt.datetime) -> tuple | None:
    return store.con.execute(
        """
        SELECT value, value_status FROM derived_metric_values
        WHERE security_id = ? AND metric_code = ? AND period_end = ? AND available_at <= ?
        ORDER BY available_at DESC, derived_value_id DESC LIMIT 1
        """,
        [A7_OWNER, metric, dt.date.fromisoformat(period_end), at],
    ).fetchone()


def _a7_growth(store, metric: str, period_end: str, at: dt.datetime) -> float | None:
    state = _a7_growth_state(store, metric, period_end, at)
    return None if state is None else state[0]


def _a7_quarter(store, code: str, period_end: str, at: dt.datetime) -> tuple | None:
    return store.con.execute(
        """
        SELECT value, available_at FROM fundamental_standardized
        WHERE security_id = ? AND canonical_code = ? AND basis = 'quarterly'
          AND period_end = ? AND available_at <= ?
        ORDER BY available_at DESC, standardized_id DESC LIMIT 1
        """,
        [A7_OWNER, code, dt.date.fromisoformat(period_end), at],
    ).fetchone()


def test_eight_quarters_of_basic_and_diluted_releases_end_to_end(tmp_store, tmp_path) -> None:
    store = tmp_store
    counts = _a7_seed_and_extract(store, tmp_path)
    assert (counts["accepted"], counts["rejected"]) == (8, 0)

    facts = store.con.execute(
        """
        SELECT period_end, measure_code, value, fiscal_year, fiscal_period, available_at,
               json_extract_string(raw_payload_json, '$.receipt_id') AS receipt_id,
               json_extract_string(raw_payload_json, '$.row_semantics') AS row_semantics
        FROM press_release_facts WHERE cik = ? ORDER BY period_end, measure_code
        """,
        [A7_CIK],
    ).fetchall()
    assert len(facts) == 16
    for basic, diluted in zip(facts[0::2], facts[1::2], strict=True):
        assert (basic[1], diluted[1]) == ("EPS_BASIC", "EPS_DILUTED")
        # Same document, receipt, clock and period evidence for both measures.
        assert basic[0] == diluted[0] and basic[3:7] == diluted[3:7]
        assert (basic[7], diluted[7]) == ("basic", "diluted")
    by_period = {(row[0].isoformat(), row[1]): row for row in facts}
    assert by_period[("2023-06-30", "EPS_BASIC")][2] == -0.40
    assert by_period[("2024-03-31", "EPS_BASIC")][2:5] == (0.60, 2024, "Q1")
    assert by_period[("2024-03-31", "EPS_DILUTED")][2] == 0.50
    # Fiscal-year-end change: the release's own labels differ from the
    # prior-year quarter's; nothing downstream joins on them.
    assert by_period[("2025-03-31", "EPS_BASIC")][3:5] == (2025, "Q2")
    assert "2023-12-31" not in {period for period, _ in by_period}

    refresh_fundamental_fact_revisions(store)
    refresh_fundamental_statement_points(store)
    refresh_fundamental_standardized(store, FundamentalStandardizationOptions(symbols=(A7_SYMBOL,)))
    refresh_derived_metrics(store, DerivedMetricsOptions(
        metric_codes=("eps_basic_q_growth_yoy", "eps_diluted_q_growth_yoy"), security_ids=(A7_OWNER,),
    ))

    assert store.con.execute(
        """SELECT concept, canonical_metric, item_id, count(*) FROM fundamental_statement_points
           WHERE source = ? AND security_id = ? GROUP BY ALL ORDER BY item_id""",
        [RELEASE_SOURCE, A7_OWNER],
    ).fetchall() == [
        ("EarningsPerShareBasic", "eps_basic", 1034, 8),
        ("EarningsPerShareDiluted", "eps_diluted", 1035, 8),
    ]
    # Missing Q4: no quarterly EPS exists at all, not from FY - 9M and not
    # from the next release's comparative column.
    assert store.con.execute(
        """SELECT count(*) FROM fundamental_standardized
           WHERE security_id = ? AND canonical_code IN ('eps_basic__1034', 'eps_diluted')
             AND basis <> 'annual' AND period_end = DATE '2023-12-31'""",
        [A7_OWNER],
    ).fetchone() == (0,)
    assert store.con.execute(
        """SELECT count(*) FROM fundamental_standardized
           WHERE security_id = ? AND canonical_code = 'eps_basic__1034'
             AND basis = 'annual' AND period_end = DATE '2023-12-31' AND value = 0.30""",
        [A7_OWNER],
    ).fetchone() == (1,)

    # Diluted share-count change: basic and diluted stay independent series.
    assert _a7_quarter(store, "eps_basic__1034", "2024-03-31", A7_CUTOFF)[0] == pytest.approx(0.60)
    assert _a7_quarter(store, "eps_diluted", "2024-03-31", A7_CUTOFF)[0] == pytest.approx(0.50)
    # Each quarter is visible only from its own release clock.
    released = dt.datetime(2024, 4, 26, 22)
    assert _a7_quarter(store, "eps_basic__1034", "2024-03-31", released - dt.timedelta(microseconds=1)) is None
    assert _a7_quarter(store, "eps_basic__1034", "2024-03-31", released)[1] == released

    expected_at_cutoff = {
        # (current - prior) / abs(prior)
        ("eps_basic_q_growth_yoy", "2024-03-31"): 0.20,
        ("eps_diluted_q_growth_yoy", "2024-03-31"): (0.50 - 0.48) / 0.48,
        # Loss base.
        ("eps_basic_q_growth_yoy", "2024-06-30"): 1.50,
        ("eps_diluted_q_growth_yoy", "2024-06-30"): 1.45,
        # Zero base.
        ("eps_basic_q_growth_yoy", "2024-09-30"): None,
        ("eps_diluted_q_growth_yoy", "2024-09-30"): None,
        # Missing Q4 2023 base.
        ("eps_basic_q_growth_yoy", "2024-12-31"): None,
        ("eps_diluted_q_growth_yoy", "2024-12-31"): None,
        # Across the fiscal-year-end change (labels Q2 FY2025 vs Q1 2024).
        ("eps_basic_q_growth_yoy", "2025-03-31"): 0.20,
        ("eps_diluted_q_growth_yoy", "2025-03-31"): 0.20,
    }
    observed = {key: _a7_growth(store, *key, A7_CUTOFF) for key in expected_at_cutoff}
    assert observed == {
        key: (None if value is None else pytest.approx(value)) for key, value in expected_at_cutoff.items()
    }
    # Zero and missing bases are explicit NULL domain states, not absent rows.
    for key, value in expected_at_cutoff.items():
        if value is None:
            assert _a7_growth_state(store, *key, A7_CUTOFF) == (None, "missing_input_or_domain")

    # Post-cutoff 10-Q/A: invisible at the cutoff, then a basic conflict NULL.
    assert _a7_quarter(store, "eps_basic__1034", "2024-03-31", A7_AFTER_AMENDMENT)[0] is None
    assert _a7_quarter(store, "eps_diluted", "2024-03-31", A7_AFTER_AMENDMENT)[0] == pytest.approx(0.50)
    assert _a7_growth(store, "eps_basic_q_growth_yoy", "2024-03-31", A7_AFTER_AMENDMENT) is None
    assert _a7_growth(store, "eps_basic_q_growth_yoy", "2025-03-31", A7_AFTER_AMENDMENT) is None
    assert _a7_growth(store, "eps_diluted_q_growth_yoy", "2024-03-31", A7_AFTER_AMENDMENT) == pytest.approx(
        (0.50 - 0.48) / 0.48
    )
    assert _a7_growth(store, "eps_diluted_q_growth_yoy", "2025-03-31", A7_AFTER_AMENDMENT) == pytest.approx(0.20)
