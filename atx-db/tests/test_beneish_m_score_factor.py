from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from atx_db.beneish_m_score import (
    FACTOR_ID,
    SOURCE_NAME,
    BeneishMScoreOptions,
    refresh_beneish_m_score_values,
)
from atx_db.connection import DuckDBStore
from atx_db.formula_library import beneish_m_score
from atx_db.migrations import bodies_0252


def _expected_m_score(receivables: float) -> float:
    return float(
        beneish_m_score(
            {
                "rev": 110.0,
                "rev_prior": 100.0,
                "assets": 110.0,
                "assets_prior": 100.0,
                "property_plant_equipment_net": 33.0,
                "property_plant_equipment_net_prior": 30.0,
                "depreciation_amortization": 11.0,
                "depreciation_amortization_prior": 10.0,
                "selling_general_and_administrative_expense": 11.0,
                "selling_general_and_administrative_expense_prior": 10.0,
                "liabilities": 55.0,
                "liabilities_prior": 50.0,
                "accounts_receivable": receivables,
                "accounts_receivable_prior": 10.0,
                "cost_of_revenue": 66.0,
                "cost_of_revenue_prior": 60.0,
                "current_assets": 44.0,
                "current_assets_prior": 40.0,
                "ni": 5.0,
                "ocf": 6.0,
            }
        )
    )


def test_beneish_formula_orientation_and_lineage() -> None:
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    store._initialized = True
    try:
        store.con.execute(
            """
            CREATE TABLE fundamental_factor_values (
                factor_value_id VARCHAR,factor_id VARCHAR,factor_name VARCHAR,family VARCHAR,
                security_id VARCHAR,symbol VARCHAR,as_of_date DATE,raw_value DOUBLE,
                value DOUBLE,available_at TIMESTAMP,input_ids_json VARCHAR,
                input_lineage_json VARCHAR,is_latest_revision BOOLEAN,run_id VARCHAR,
                source VARCHAR
            );
            CREATE TABLE fundamental_statement_points (
                security_id VARCHAR,period_start DATE,period_end DATE,
                canonical_metric VARCHAR,unit VARCHAR,period_type VARCHAR,form VARCHAR,
                value DOUBLE,available_at TIMESTAMP,revision_sequence INTEGER,
                statement_point_id VARCHAR,accession_number VARCHAR
            );
            """
        )
        current_receivables = {"LOW": 11.0, "MID": 22.0, "HIGH": 44.0}
        for security_id, receivables in current_receivables.items():
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [
                    f"parent-{security_id}",
                    ASSET_GROWTH_FACTOR_ID,
                    "parent",
                    "investment",
                    security_id,
                    security_id,
                    dt.date(2025, 2, 28),
                    0.0,
                    0.0,
                    dt.datetime(2025, 2, 28, 22),
                    "[]",
                    "{}",
                    True,
                    "parent",
                    "fixture",
                ],
            )
            periods = (
                (
                    dt.date(2023, 1, 1),
                    dt.date(2023, 12, 31),
                    dt.datetime(2024, 2, 15, 22),
                    "prior",
                    {
                        "revenue": 100.0,
                        "ar": 10.0,
                        "cogs": 60.0,
                        "total_assets": 100.0,
                        "current_assets": 40.0,
                        "ppe_net": 30.0,
                        "da_cf": 10.0,
                        "sga": 10.0,
                        "total_liabilities": 50.0,
                        "net_income": 5.0,
                        "operating_cash_flow": 6.0,
                    },
                ),
                (
                    dt.date(2024, 1, 1),
                    dt.date(2024, 12, 31),
                    dt.datetime(2025, 2, 15, 22),
                    "current",
                    {
                        "revenue": 110.0,
                        "ar": receivables,
                        "cogs": 66.0,
                        "total_assets": 110.0,
                        "current_assets": 44.0,
                        "ppe_net": 33.0,
                        "da_cf": 11.0,
                        "sga": 11.0,
                        "total_liabilities": 55.0,
                        "net_income": 5.0,
                        "operating_cash_flow": 6.0,
                    },
                ),
            )
            for period_start, period_end, available_at, accession, values in periods:
                for metric, value in values.items():
                    is_instant = metric in {
                        "ar",
                        "total_assets",
                        "current_assets",
                        "ppe_net",
                        "total_liabilities",
                    }
                    store.con.execute(
                        "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                        [
                            security_id,
                            None if is_instant else period_start,
                            period_end,
                            metric,
                            "USD",
                            "instant" if is_instant else "duration",
                            "10-K",
                            value,
                            available_at,
                            1,
                            f"{security_id}-{accession}-{metric}",
                            f"{security_id}-{accession}",
                        ],
                    )
        count = refresh_beneish_m_score_values(
            store,
            BeneishMScoreOptions(
                minimum_names_per_date=2, winsor_limit=0.0, run_id="test"
            ),
        )
        rows = store.con.execute(
            """SELECT security_id,raw_value,value,input_lineage_json
               FROM fundamental_factor_values WHERE factor_id=? ORDER BY raw_value""",
            [FACTOR_ID],
        ).fetchall()
        assert count == 3
        assert [row[0] for row in rows] == ["HIGH", "MID", "LOW"]
        assert [row[1] for row in rows] == pytest.approx(
            [-_expected_m_score(value) for value in (44.0, 22.0, 11.0)]
        )
        assert rows[0][2] < rows[1][2] < rows[2][2]
        lineage = json.loads(rows[0][3])
        assert lineage["m_score"] == pytest.approx(_expected_m_score(44.0))
        assert lineage["dsri"] == pytest.approx(4.0)
        assert lineage["accessions"] == ["HIGH-current", "HIGH-prior"]
        assert lineage["missing_components_imputed"] is False
    finally:
        store.connection.close()
        store.connection = None


def test_beneish_definition_is_governed(monkeypatch) -> None:
    conn = duckdb.connect(":memory:")
    conn.execute(
        """
        CREATE TABLE factor_definition (
            factor_id VARCHAR PRIMARY KEY,factor_name VARCHAR,family VARCHAR,
            description VARCHAR,expression VARCHAR,input_ids_json VARCHAR,
            direction INTEGER,lookback_days INTEGER,neutralization_spec_json VARCHAR,
            unit VARCHAR,sign VARCHAR,scale VARCHAR,is_point_in_time_safe BOOLEAN,
            available_at_policy VARCHAR,declared_in VARCHAR,owner VARCHAR,source VARCHAR,
            standardization_spec_json VARCHAR,valid_from DATE,valid_to DATE
        );
        CREATE TABLE factor_dependency_edges (
            dependency_id VARCHAR PRIMARY KEY,factor_id VARCHAR,dependency_type VARCHAR,
            dependency_name VARCHAR,dependency_factor_id VARCHAR,dependency_metric_id VARCHAR,
            dependency_source_id VARCHAR,dependency_depth INTEGER,expression VARCHAR,
            lookback_days INTEGER,is_direct BOOLEAN,source VARCHAR
        );
        """
    )
    monkeypatch.setattr(bodies_0252, "_refresh_schema_contract_v2_pin", lambda _conn: None)
    bodies_0252.MIGRATIONS[0].up(conn)
    definition = conn.execute(
        """SELECT is_point_in_time_safe,source FROM factor_definition
           WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition == (True, SOURCE_NAME)
    assert conn.execute(
        "SELECT count(*) FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID]
    ).fetchone() == (14,)
    conn.close()
