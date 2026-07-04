from __future__ import annotations

import datetime as dt
import json

import pytest

from db.fact_disagreement import (
    FactDisagreementDataset,
    FactDisagreementOptions,
    fact_disagreement_summary,
    refresh_fact_disagreement,
)


PERIOD_END = dt.date(2024, 12, 31)
AVAILABLE_AT = dt.datetime(2025, 2, 15, 10)


def _insert_standardized(
    store,
    *,
    security_id: str = "SEC-A",
    symbol: str = "AAA",
    item_id: int = 1001,
    canonical_code: str = "revenue",
    value: float = 100.0,
    available_at: dt.datetime = AVAILABLE_AT,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, upstream_source, security_id, symbol, cik,
            item_id, canonical_code, basis, period_start, period_end,
            fiscal_year, fiscal_period, value, unit_type, source_accession,
            filed_date, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, is_latest_revision,
            run_id
        )
        VALUES (
            ?, 'standardized_v1', 'fixture', ?, ?, '0000000001',
            ?, ?, 'annual', DATE '2024-01-01', DATE '2024-12-31',
            2024, 'FY', ?, 'USD', 'std-acc',
            DATE '2025-02-15', DATE '2024-12-31', ?, '{}',
            '{}', 'fixture_rule', 'identity', true, 'std-run'
        )
        """,
        [
            f"std-{security_id}-{item_id}",
            security_id,
            symbol,
            item_id,
            canonical_code,
            value,
            available_at,
        ],
    )


def test_fact_disagreement_loads_baseline_and_flags_statuses(tmp_store) -> None:
    _insert_standardized(tmp_store, item_id=1001, canonical_code="revenue", value=100.0)
    _insert_standardized(tmp_store, item_id=1031, canonical_code="net_income_total", value=25.0)

    rows = [
        {
            "vendor": "sharadar",
            "security_id": "SEC-A",
            "canonical_code": "revenue",
            "basis": "annual",
            "period_end": PERIOD_END,
            "value": 100.0,
            "available_at": AVAILABLE_AT,
        },
        {
            "vendor": "sharadar",
            "security_id": "SEC-A",
            "canonical_code": "net_income_total",
            "basis": "annual",
            "period_end": PERIOD_END,
            "value": 20.0,
            "available_at": AVAILABLE_AT,
        },
        {
            "vendor": "sharadar",
            "security_id": "SEC-A",
            "canonical_code": "cash_and_equivalents",
            "item_id": 1104,
            "basis": "annual",
            "period_end": PERIOD_END,
            "value": 5.0,
            "available_at": AVAILABLE_AT,
        },
    ]
    options = FactDisagreementOptions(vendor="SHARADAR", baseline_rows=rows, run_id="recon-run")

    assert refresh_fact_disagreement(tmp_store, options) == 3
    statuses = tmp_store.con.execute(
        """
        SELECT canonical_code, agreement_status, warehouse_value, vendor_value
        FROM fact_disagreement
        ORDER BY canonical_code
        """
    ).fetchall()

    assert statuses == [
        ("cash_and_equivalents", "missing_warehouse", None, 5.0),
        ("net_income_total", "disagrees", 25.0, 20.0),
        ("revenue", "agrees", 100.0, 100.0),
    ]
    summary = fact_disagreement_summary(tmp_store, options)
    assert summary["agreement_ratio"] == pytest.approx(1 / 3)
    assert summary["disagrees"] == 1
    assert summary["missing_warehouse"] == 1


def test_fact_disagreement_dataset_records_agreement_quality(tmp_store) -> None:
    _insert_standardized(tmp_store, item_id=1001, canonical_code="revenue", value=100.0)
    options = FactDisagreementOptions(
        vendor="SIMFIN",
        baseline_rows=[
            {
                "vendor": "simfin",
                "security_id": "SEC-A",
                "canonical_code": "revenue",
                "basis": "annual",
                "period_end": PERIOD_END,
                "value": 100.0,
                "available_at": AVAILABLE_AT,
            }
        ],
        run_id="quality-run",
    )

    result = FactDisagreementDataset().load(tmp_store, options)

    assert result.rows_loaded == 1
    row = tmp_store.con.execute(
        """
        SELECT status, observed_value, details_json
        FROM data_quality_checks
        WHERE dataset_id = 'fact_disagreement'
          AND check_name = 'agreement_ratio'
        """
    ).fetchone()
    assert row[0] == "passed"
    assert row[1] == pytest.approx(1.0)
    assert json.loads(row[2])["threshold"] == pytest.approx(0.99)


def test_fact_disagreement_migration_catalog_present(tmp_store) -> None:
    assert tmp_store.con.execute(
        "SELECT description FROM schema_migrations WHERE version = '0125'"
    ).fetchone()[0] == "fact_disagreement_schema_catalog"
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'vendor_baseline_facts'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'fact_disagreement'"
    ).fetchone()[0] == 1

    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'fact_disagreement'
            """
        ).fetchall()
    }
    assert {"agreement_status", "warehouse_value", "vendor_value", "relative_difference"}.issubset(columns)
