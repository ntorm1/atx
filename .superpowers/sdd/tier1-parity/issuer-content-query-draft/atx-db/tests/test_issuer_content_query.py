from __future__ import annotations

import datetime as dt

import duckdb

from atx_db.api.service import WarehouseReadService


def _service(tmp_path) -> WarehouseReadService:
    path = tmp_path / "issuer-content.duckdb"
    with duckdb.connect(str(path)) as conn:
        conn.execute("""
            CREATE TABLE security_identifier_history (
                security_id VARCHAR, id_type VARCHAR, id_value VARCHAR, valid_from DATE,
                valid_to DATE, as_of_date DATE, available_at TIMESTAMP, source VARCHAR,
                source_loaded_at TIMESTAMP
            )
        """)
        conn.execute("CREATE TABLE sec_company_tickers (cik VARCHAR, ticker VARCHAR, title VARCHAR, security_id VARCHAR)")
        conn.execute("""
            CREATE TABLE fundamental_fact_revisions (
                security_id VARCHAR, cik VARCHAR, available_at TIMESTAMP, source_loaded_at TIMESTAMP
            )
        """)
        conn.execute("""
            CREATE TABLE derived_metric_values (
                derived_value_id VARCHAR, revision_group_id VARCHAR, security_id VARCHAR,
                metric_code VARCHAR, metric_window VARCHAR, period_end DATE, value DOUBLE,
                value_status VARCHAR, inputs_hash VARCHAR, as_of_date DATE,
                available_at TIMESTAMP, source_loaded_at TIMESTAMP, run_id VARCHAR
            )
        """)
        conn.executemany(
            "INSERT INTO security_identifier_history VALUES (?,?,?,?,?,?,?,?,?)",
            [
                ("SEC-MARKET", "TICKER", "AAA", dt.date(2026, 9, 20), None, dt.date(2026, 9, 20), dt.datetime(2026, 9, 20, 9), "directory", dt.datetime(2026, 9, 20, 9)),
                ("SEC-MARKET", "CIK", "0000000123", dt.date(2026, 9, 20), None, dt.date(2026, 9, 20), dt.datetime(2026, 9, 20, 9), "directory", dt.datetime(2026, 9, 20, 9)),
            ],
        )
        # The same CIK has two source owners across vintages.  No directory row
        # is inserted: direct CIK content selection must still be usable.
        conn.executemany(
            "INSERT INTO fundamental_fact_revisions VALUES (?,?,?,?)",
            [
                ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000123", "0000000123", dt.datetime(2026, 1, 1), dt.datetime(2026, 1, 1)),
                ("SEC-COMPANYFACTS-RESOLVED-CIK-0000000123", "0000000123", dt.datetime(2026, 7, 1), dt.datetime(2026, 7, 1)),
            ],
        )
        conn.executemany(
            "INSERT INTO derived_metric_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [
                ("old-valid", "eps-q1", "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000123", "eps_diluted_q_growth_yoy", "q", dt.date(2026, 3, 31), 1.0, "valid", "old", dt.date(2026, 3, 31), dt.datetime(2026, 5, 1), dt.datetime(2026, 5, 1), "seed"),
                ("old-unavailable", "eps-q1", "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000123", "eps_diluted_q_growth_yoy", "q", dt.date(2026, 3, 31), None, "missing_input_or_domain", "new", dt.date(2026, 3, 31), dt.datetime(2026, 6, 1), dt.datetime(2026, 6, 1), "seed"),
                ("resolved", "eps-q2", "SEC-COMPANYFACTS-RESOLVED-CIK-0000000123", "eps_diluted_q_growth_yoy", "q", dt.date(2026, 6, 30), 2.0, "valid", "resolved", dt.date(2026, 6, 30), dt.datetime(2026, 8, 1), dt.datetime(2026, 8, 1), "seed"),
            ],
        )
    return WarehouseReadService(path)


def test_issuer_content_discovers_actual_owners_and_keeps_latest_null_state(tmp_path) -> None:
    result = _service(tmp_path).issuer_content_range(
        schema_name="derived-metrics", cik="123", start=dt.date(2026, 1, 1), end=dt.date(2026, 7, 1),
        content_as_of=dt.datetime(2026, 9, 1, tzinfo=dt.UTC), fields=["issuer_owner_id", "cik", "period_end", "value", "value_status"],
    )
    assert result.metadata["issuer_owner_status"] == "ambiguous_multiple_visible_owners"
    assert result.metadata["issuer_owner_ids"] == [
        "SEC-COMPANYFACTS-RESOLVED-CIK-0000000123", "SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000123",
    ]
    assert {(row["issuer_owner_id"], row["value"], row["value_status"]) for row in result.data} == {
        ("SEC-COMPANYFACTS-UNRESOLVED-CIK-0000000123", None, "missing_input_or_domain"),
        ("SEC-COMPANYFACTS-RESOLVED-CIK-0000000123", 2.0, "valid"),
    }


def test_ticker_lookup_uses_co_visible_history_without_current_directory_gate(tmp_path) -> None:
    lookup = _service(tmp_path).resolve_issuer_ticker(
        ticker="aaa", issuer_lookup_as_of=dt.datetime(2026, 9, 20, 12, tzinfo=dt.UTC),
    )
    assert lookup["status"] == "resolved"
    assert lookup["lookup_cik"] == "0000000123"
    assert lookup["current_directory_cross_check"] is False
    assert lookup["directory_valid_from"] == dt.date(2026, 9, 20)

