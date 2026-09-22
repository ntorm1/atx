from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_db.api.service import ApiQueryError, WarehouseReadService


CIK_A = "0000000123"
CIK_B = "0000000456"
OWNER_SAFE = "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000123"
OWNER_COLLIDING = "SEC-COMPANYFACTS-REUSED-OWNER"


def _service(tmp_path) -> WarehouseReadService:
    path = tmp_path / "issuer-content.duckdb"
    with duckdb.connect(str(path)) as conn:
        conn.execute(
            """
            CREATE TABLE security_identifier_history (
                security_id VARCHAR, id_type VARCHAR, id_value VARCHAR, valid_from DATE,
                valid_to DATE, as_of_date DATE, available_at TIMESTAMP, source VARCHAR,
                source_loaded_at TIMESTAMP
            )
            """
        )
        conn.execute(
            "CREATE TABLE sec_company_tickers (cik VARCHAR, ticker VARCHAR, title VARCHAR, security_id VARCHAR)"
        )
        conn.execute(
            """
            CREATE TABLE fundamental_fact_revisions (
                security_id VARCHAR, cik VARCHAR, as_of_date DATE, available_at TIMESTAMP,
                source_loaded_at TIMESTAMP
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE fundamental_statement_points (
                statement_point_id VARCHAR, revision_group_id VARCHAR, security_id VARCHAR, cik VARCHAR,
                canonical_metric VARCHAR, fiscal_period VARCHAR, period_start DATE, period_end DATE,
                value DOUBLE, unit VARCHAR, fact_revision_id VARCHAR, accession_number VARCHAR,
                source_url VARCHAR, as_of_date DATE, available_at TIMESTAMP, source_loaded_at TIMESTAMP
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE shares_outstanding_history (
                share_history_id VARCHAR, security_id VARCHAR, cik VARCHAR, share_count_type VARCHAR,
                effective_date DATE, share_count DOUBLE, accession_number VARCHAR, as_of_date DATE,
                available_at TIMESTAMP, source_loaded_at TIMESTAMP
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE derived_metric_values (
                derived_value_id VARCHAR, revision_group_id VARCHAR, security_id VARCHAR,
                metric_code VARCHAR, metric_window VARCHAR, period_end DATE, value DOUBLE,
                value_status VARCHAR, inputs_hash VARCHAR, as_of_date DATE,
                available_at TIMESTAMP, source_loaded_at TIMESTAMP, run_id VARCHAR
            )
            """
        )
        conn.executemany(
            "INSERT INTO security_identifier_history VALUES (?,?,?,?,?,?,?,?,?)",
            [
                ("SEC-MARKET-A", "TICKER", "AAA", dt.date(2026, 9, 20), None, dt.date(2026, 9, 20), dt.datetime(2026, 9, 20, 9), "directory", dt.datetime(2026, 9, 20, 9)),
                ("SEC-MARKET-A", "CIK", CIK_A, dt.date(2026, 9, 20), None, dt.date(2026, 9, 20), dt.datetime(2026, 9, 20, 9), "directory", dt.datetime(2026, 9, 20, 9)),
                ("SEC-MARKET-B", "TICKER", "AAA", dt.date(2026, 9, 20), None, dt.date(2026, 9, 20), dt.datetime(2026, 9, 20, 10), "directory", dt.datetime(2026, 9, 20, 10)),
                ("SEC-MARKET-B", "CIK", CIK_A, dt.date(2026, 9, 20), None, dt.date(2026, 9, 20), dt.datetime(2026, 9, 20, 10), "directory", dt.datetime(2026, 9, 20, 10)),
            ],
        )
        conn.executemany(
            "INSERT INTO fundamental_fact_revisions VALUES (?,?,?,?,?)",
            [
                (OWNER_SAFE, CIK_A, dt.date(2026, 1, 1), dt.datetime(2026, 1, 1), dt.datetime(2026, 1, 1)),
                (OWNER_COLLIDING, CIK_A, dt.date(2026, 2, 1), dt.datetime(2026, 2, 1), dt.datetime(2026, 2, 1)),
                (OWNER_COLLIDING, CIK_B, dt.date(2026, 3, 1), dt.datetime(2026, 3, 1), dt.datetime(2026, 3, 1)),
            ],
        )
        conn.executemany(
            "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [
                ("statement-old", "income-q1", OWNER_SAFE, CIK_A, "revenue", "q", None, dt.date(2026, 3, 31), 10.0, "USD", "fact-old", "old", "url", dt.date(2026, 3, 31), dt.datetime(2026, 5, 1), dt.datetime(2026, 5, 1)),
                ("statement-new", "income-q1", OWNER_SAFE, CIK_A, "revenue", "q", None, dt.date(2026, 3, 31), 11.0, "USD", "fact-new", "new", "url", dt.date(2026, 3, 31), dt.datetime(2026, 6, 1), dt.datetime(2026, 6, 1)),
                ("statement-a", "income-q2", OWNER_COLLIDING, CIK_A, "revenue", "q", None, dt.date(2026, 6, 30), 20.0, "USD", "fact-a", "a", "url", dt.date(2026, 6, 30), dt.datetime(2026, 7, 1), dt.datetime(2026, 7, 1)),
                ("statement-b", "income-q2", OWNER_COLLIDING, CIK_B, "revenue", "q", None, dt.date(2026, 6, 30), 999.0, "USD", "fact-b", "b", "url", dt.date(2026, 6, 30), dt.datetime(2026, 7, 1), dt.datetime(2026, 7, 1)),
            ],
        )
        conn.executemany(
            "INSERT INTO shares_outstanding_history VALUES (?,?,?,?,?,?,?,?,?,?)",
            [
                ("shares-old", OWNER_SAFE, CIK_A, "dei", dt.date(2026, 3, 31), 100.0, "shares-q1", dt.date(2026, 3, 31), dt.datetime(2026, 5, 1), dt.datetime(2026, 5, 1)),
                ("shares-new", OWNER_SAFE, CIK_A, "dei", dt.date(2026, 3, 31), 110.0, "shares-q1", dt.date(2026, 3, 31), dt.datetime(2026, 6, 1), dt.datetime(2026, 6, 1)),
            ],
        )
        conn.executemany(
            "INSERT INTO derived_metric_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [
                ("old-valid", "eps-q1", OWNER_SAFE, "eps_diluted_q_growth_yoy", "q", dt.date(2026, 3, 31), 1.0, "valid", "old", dt.date(2026, 3, 31), dt.datetime(2026, 5, 1), dt.datetime(2026, 5, 1), "seed"),
                ("new-unavailable", "eps-q1", OWNER_SAFE, "eps_diluted_q_growth_yoy", "q", dt.date(2026, 3, 31), None, "missing_input_or_domain", "new", dt.date(2026, 3, 31), dt.datetime(2026, 6, 1), dt.datetime(2026, 6, 1), "seed"),
                ("cross-cik-leak", "eps-q2", OWNER_COLLIDING, "eps_diluted_q_growth_yoy", "q", dt.date(2026, 6, 30), 999.0, "valid", "b-only", dt.date(2026, 6, 30), dt.datetime(2026, 7, 1), dt.datetime(2026, 7, 1), "seed"),
            ],
        )
    return WarehouseReadService(path)


def _range(service: WarehouseReadService, schema_name: str, *, cik: str = "123", **kwargs):
    return service.issuer_content_range(
        schema_name=schema_name,
        cik=cik,
        start=dt.date(2026, 1, 1),
        end=dt.date(2026, 7, 1),
        content_as_of=dt.datetime(2026, 9, 1, tzinfo=dt.UTC),
        **kwargs,
    )


def test_issuer_content_excludes_cross_cik_derived_owner_and_keeps_null_state(tmp_path) -> None:
    result = _range(
        _service(tmp_path),
        "derived-metrics",
        fields=["issuer_owner_id", "cik", "period_end", "value", "value_status"],
    )

    assert result.metadata["issuer_owner_status"] == "ambiguous_owner_cik_collision"
    assert result.metadata["issuer_owner_ciks"][OWNER_COLLIDING] == (CIK_A, CIK_B)
    assert result.metadata["excluded_derived_owner_ids"] == [OWNER_COLLIDING]
    assert result.metadata["derived_issuer_owner_ids"] == [OWNER_SAFE]
    assert result.data == [
        {
            "issuer_owner_id": OWNER_SAFE,
            "cik": CIK_A,
            "period_end": dt.date(2026, 3, 31),
            "value": None,
            "value_status": "missing_input_or_domain",
        }
    ]


def test_issuer_content_exact_cik_filter_and_vintage_clocks(tmp_path) -> None:
    service = _service(tmp_path)
    latest = _range(service, "statements", fields=["issuer_owner_id", "period_end", "value"])
    first = _range(
        service,
        "statements",
        vintage="first_reported",
        fields=["issuer_owner_id", "period_end", "value"],
    )
    before_revision = service.issuer_content_range(
        schema_name="statements",
        cik="123",
        start=dt.date(2026, 1, 1),
        end=dt.date(2026, 7, 1),
        content_as_of=dt.datetime(2026, 5, 15, tzinfo=dt.UTC),
        fields=["issuer_owner_id", "period_end", "value"],
    )

    assert {row["value"] for row in latest.data} == {11.0, 20.0}
    assert {row["value"] for row in first.data} == {10.0, 20.0}
    assert {row["value"] for row in before_revision.data} == {10.0}
    assert all(row["value"] != 999.0 for row in latest.data)


def test_issuer_shares_first_reported_uses_the_same_vintage_direction(tmp_path) -> None:
    service = _service(tmp_path)
    latest = _range(service, "shares", fields=["period_end", "value"])
    first = _range(service, "shares", vintage="first_reported", fields=["period_end", "value"])

    assert latest.data == [{"period_end": dt.date(2026, 3, 31), "value": 110.0}]
    assert first.data == [{"period_end": dt.date(2026, 3, 31), "value": 100.0}]


def test_ticker_lookup_preserves_multiple_directory_candidates_as_unqualified(tmp_path) -> None:
    lookup = _service(tmp_path).resolve_issuer_ticker(
        ticker="aaa", issuer_lookup_as_of=dt.datetime(2026, 9, 20, 12, tzinfo=dt.UTC)
    )

    association = lookup["issuer_market_association"]
    assert lookup["status"] == "resolved"
    assert lookup["lookup_cik"] == CIK_A
    assert lookup["directory_candidate_status"] == "ambiguous_multiple_candidates"
    assert lookup["directory_candidate_count"] == 2
    assert lookup["directory_security_id"] is None
    assert association == {
        "market_security_id": None,
        "association_method": "ticker_cik_history_co_visible",
        "association_scope": "query_asof_current_cik_directory",
        "historical_security_qualified": False,
        "unavailable_reason": "multiple_current_directory_security_candidates",
    }


def test_issuer_content_rejects_oversized_cik_without_truncation(tmp_path) -> None:
    with pytest.raises(ApiQueryError, match="at most 10 digits"):
        _range(_service(tmp_path), "statements", cik="12345678901")
