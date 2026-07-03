"""S8 restatement-lineage tests.

S8-0 is intentionally narrow: additive provenance columns, catalog/index
coverage, and passthrough/backfill of accession + filed-date lineage.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from db.fundamental_ratios import FundamentalRatiosOptions, compute_ratio_rows, refresh_fundamental_ratios
from db.fundamental_statements import SOURCE_NAME as COMPANYFACTS_SOURCE
from db.fundamental_statements import refresh_fundamental_statement_points
from db.migrations import (
    _fundamental_ratio_provenance_indexes,
    _fundamental_ratio_provenance_schema_catalog,
)


def _ts(value: str) -> pd.Timestamp:
    return pd.Timestamp(value)


def test_s8_0_migrations_are_idempotent_cataloged_and_indexed(tmp_store) -> None:
    versions = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert {92, 93}.issubset(versions)

    _fundamental_ratio_provenance_schema_catalog(tmp_store.con)
    _fundamental_ratio_provenance_schema_catalog(tmp_store.con)
    _fundamental_ratio_provenance_indexes(tmp_store.con)
    _fundamental_ratio_provenance_indexes(tmp_store.con)

    expected_fields = {
        ("fundamental_statement_points", "source_accession"),
        ("fundamental_statement_points", "filed_date"),
        ("fundamental_ratios", "source_accession"),
        ("fundamental_ratios", "filed_date"),
    }
    columns = {
        (row[0], row[1])
        for row in tmp_store.con.execute(
            """
            SELECT table_name, column_name
            FROM duckdb_columns()
            WHERE table_name IN ('fundamental_statement_points', 'fundamental_ratios')
              AND column_name IN ('source_accession', 'filed_date')
            """
        ).fetchall()
    }
    catalog = {
        (row[0], row[1])
        for row in tmp_store.con.execute(
            """
            SELECT table_name, field_name
            FROM field_catalog
            WHERE table_name IN ('fundamental_statement_points', 'fundamental_ratios')
              AND field_name IN ('source_accession', 'filed_date')
            """
        ).fetchall()
    }
    indexes = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT index_name
            FROM duckdb_indexes()
            WHERE table_name IN ('fundamental_statement_points', 'fundamental_ratios')
            """
        ).fetchall()
    }

    assert expected_fields.issubset(columns)
    assert catalog == expected_fields
    assert "idx_fundamental_ratios_source_accession" in indexes
    assert "idx_fundamental_statement_points_source_accession" in indexes


def test_refresh_statement_points_populates_source_accession_and_filed_date(tmp_store) -> None:
    sid = "SEC-CIK-0000000001"
    accession = "0000000001-26-000010"
    filed_date = dt.date(2026, 2, 15)
    available_at = dt.datetime(2026, 2, 15, 22, 0)
    tmp_store.con.execute(
        """
        INSERT OR REPLACE INTO securities (
            security_id, primary_symbol, name, source, source_loaded_at
        )
        VALUES (?, 'LINE', 'Lineage Fixture Inc.', 'test', ?)
        """,
        [sid, available_at],
    )
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_fact_revisions (
            fact_revision_id, revision_group_id, source, security_id, cik,
            taxonomy, concept, unit, period_start, period_end, accession_number,
            filed_date, available_at, form, fiscal_year, fiscal_period, value,
            revision_sequence, revision_count, is_latest_revision, is_value_changed,
            source_url, source_loaded_at
        )
        VALUES (
            'fr-s8-0', 'rg-s8-0', ?, ?, '0000000001',
            'us-gaap', 'RevenueFromContractWithCustomerExcludingAssessedTax', 'USD',
            DATE '2025-01-01', DATE '2025-12-31', ?,
            ?, ?, '10-K', 2025, 'FY', 123.45,
            1, 1, true, false,
            'https://example.test/filing', ?
        )
        """,
        [COMPANYFACTS_SOURCE, sid, accession, filed_date, available_at, available_at],
    )

    assert refresh_fundamental_statement_points(tmp_store) == 1
    row = tmp_store.con.execute(
        """
        SELECT accession_number, source_accession, as_of_date, filed_date, value
        FROM fundamental_statement_points
        WHERE security_id = ?
          AND canonical_metric = 'revenue'
        """,
        [sid],
    ).fetchone()

    assert row[0] == accession
    assert row[1] == accession
    assert row[2] == filed_date
    assert row[3] == filed_date
    assert row[4] == pytest.approx(123.45)


def test_compute_ratio_rows_uses_consumed_input_provenance_without_changing_value() -> None:
    wide = pd.DataFrame(
        [
            {
                "security_id": "SEC-CIK-0000000002",
                "symbol": "RATIO",
                "cik": "0000000002",
                "upstream_source": "sec_companyfacts",
                "period_start": dt.date(2025, 1, 1),
                "period_end": dt.date(2025, 12, 31),
                "fiscal_year": 2025,
                "fiscal_period": "FY",
                "rev": 400.0,
                "rev_av": _ts("2026-02-20 22:00"),
                "rev_accession": "REV-ACC",
                "rev_filed_date": dt.date(2026, 2, 20),
                "ni": 100.0,
                "ni_av": _ts("2026-02-21 22:00"),
                "ni_accession": "NI-ACC",
                "ni_filed_date": dt.date(2026, 2, 21),
                "oi": 120.0,
                "oi_av": _ts("2026-03-01 22:00"),
                "oi_accession": "OI-UNRELATED-LATER",
                "oi_filed_date": dt.date(2026, 3, 1),
            }
        ]
    )

    row = compute_ratio_rows(wide).query("ratio_code == 'net_profit_margin'").iloc[0]

    assert row["value"] == pytest.approx(0.25)
    assert row["available_at"] == _ts("2026-02-21 22:00")
    assert row["source_accession"] == "NI-ACC"
    assert row["source_accession"] != "OI-UNRELATED-LATER"
    assert row["filed_date"] == dt.date(2026, 2, 21)


def _insert_ttm(
    store,
    *,
    security_id: str,
    symbol: str,
    metric: str,
    value: float,
    accession: str,
    filed_date: dt.date,
    end: dt.date,
    available_at: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_ttm_points (
            ttm_point_id, ttm_revision_group_id, anchor_statement_point_id,
            source, security_id, symbol, cik, statement_type, statement_section,
            canonical_metric, canonical_label, unit, unit_type, ttm_start_date,
            ttm_end_date, as_of_date, available_at, fiscal_year, fiscal_period,
            form, accession_number, quarter_count, coverage_days,
            input_statement_point_ids_json, input_accessions_json, input_period_ends_json,
            ttm_value, revision_sequence, revision_count, is_latest_revision,
            is_value_changed, calculation_method, source_loaded_at
        )
        VALUES (
            ?, 'ttm-rg-s8-0', ?, 'sec_companyfacts', ?, ?, '0000000002',
            'income_statement', 'profitability', ?, ?, 'USD', 'monetary',
            DATE '2025-01-01', ?, ?, ?, 2025, 'FY', '10-K', ?,
            4, 365, '[]', '[]', '[]', ?, 1, 1, true, false, 'sum_4q', ?
        )
        """,
        [
            f"{security_id}|{metric}|{end}",
            f"anchor|{metric}",
            security_id,
            symbol,
            metric,
            metric,
            end,
            filed_date,
            available_at,
            accession,
            value,
            available_at,
        ],
    )


def test_migration_backfills_ratio_provenance_from_consumed_input(tmp_store) -> None:
    sid = "SEC-CIK-0000000002"
    symbol = "RATIO"
    period_end = dt.date(2025, 12, 31)
    _insert_ttm(
        tmp_store,
        security_id=sid,
        symbol=symbol,
        metric="revenue",
        value=400.0,
        accession="REV-ACC",
        filed_date=dt.date(2026, 2, 20),
        end=period_end,
        available_at=dt.datetime(2026, 2, 20, 22, 0),
    )
    _insert_ttm(
        tmp_store,
        security_id=sid,
        symbol=symbol,
        metric="net_income",
        value=100.0,
        accession="NI-ACC",
        filed_date=dt.date(2026, 2, 21),
        end=period_end,
        available_at=dt.datetime(2026, 2, 21, 22, 0),
    )
    _insert_ttm(
        tmp_store,
        security_id=sid,
        symbol=symbol,
        metric="operating_income",
        value=120.0,
        accession="OI-UNRELATED-LATER",
        filed_date=dt.date(2026, 3, 1),
        end=period_end,
        available_at=dt.datetime(2026, 3, 1, 22, 0),
    )
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_ratios (
            ratio_id, source, upstream_source, security_id, symbol, cik,
            ratio_code, ratio_category, ratio_kind, basis, unit,
            period_start, period_end, fiscal_year, fiscal_period,
            value, numerator_code, numerator_value, denominator_code,
            denominator_value, is_meaningful, is_latest_revision,
            as_of_date, available_at, input_codes_json
        )
        VALUES (
            'ratio-mixed', 'derived_fundamental_ratios_v1', 'sec_companyfacts',
            ?, ?, '0000000002', 'net_profit_margin', 'profitability',
            'ratio', 'ttm', 'ratio', DATE '2025-01-01', ?, 2025, 'FY',
            0.25, 'net_income', 100.0, 'revenue', 400.0, true, true,
            ?, TIMESTAMP '2026-02-21 22:00:00', '["ni", "rev"]'
        )
        """,
        [sid, symbol, period_end, period_end],
    )

    _fundamental_ratio_provenance_schema_catalog(tmp_store.con)

    row = tmp_store.con.execute(
        """
        SELECT source_accession, filed_date
        FROM fundamental_ratios
        WHERE ratio_id = 'ratio-mixed'
        """
    ).fetchone()
    assert row[0] == "NI-ACC"
    assert row[0] != "OI-UNRELATED-LATER"
    assert row[1] == dt.date(2026, 2, 21)


def test_refresh_fundamental_ratios_emits_provenance_and_preserves_values(tmp_store) -> None:
    sid = "SEC-CIK-0000000002"
    symbol = "RATIO"
    period_end = dt.date(2025, 12, 31)
    _insert_ttm(
        tmp_store,
        security_id=sid,
        symbol=symbol,
        metric="revenue",
        value=400.0,
        accession="REV-ACC",
        filed_date=dt.date(2026, 2, 20),
        end=period_end,
        available_at=dt.datetime(2026, 2, 20, 22, 0),
    )
    _insert_ttm(
        tmp_store,
        security_id=sid,
        symbol=symbol,
        metric="net_income",
        value=100.0,
        accession="NI-ACC",
        filed_date=dt.date(2026, 2, 21),
        end=period_end,
        available_at=dt.datetime(2026, 2, 21, 22, 0),
    )
    _insert_ttm(
        tmp_store,
        security_id=sid,
        symbol=symbol,
        metric="operating_income",
        value=120.0,
        accession="OI-UNRELATED-LATER",
        filed_date=dt.date(2026, 3, 1),
        end=period_end,
        available_at=dt.datetime(2026, 3, 1, 22, 0),
    )

    assert refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions()) > 0
    rows = tmp_store.con.execute(
        """
        SELECT ratio_code, value, available_at, source_accession, filed_date
        FROM fundamental_ratios
        ORDER BY ratio_code
        """
    ).df()
    margin = rows.loc[rows["ratio_code"] == "net_profit_margin"].iloc[0]
    operating = rows.loc[rows["ratio_code"] == "operating_margin"].iloc[0]

    assert margin["value"] == pytest.approx(0.25)
    assert pd.Timestamp(margin["available_at"]) == _ts("2026-02-21 22:00")
    assert margin["source_accession"] == "NI-ACC"
    assert margin["source_accession"] != "OI-UNRELATED-LATER"
    assert pd.Timestamp(margin["filed_date"]).date() == dt.date(2026, 2, 21)
    assert operating["source_accession"] == "OI-UNRELATED-LATER"
