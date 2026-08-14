from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from atx_db.asset_growth import SOURCE_NAME as ASSET_GROWTH_SOURCE_NAME
from atx_db.cash_profitability import SOURCE_NAME as CASH_PROFITABILITY_SOURCE_NAME
from atx_db.connection import DuckDBStore
from atx_db.migrations import bodies_0253
from atx_db.profitability_investment import (
    CASH_PROFITABILITY_FACTOR_ID,
    FACTOR_ID,
    SOURCE_NAME,
    ProfitabilityInvestmentOptions,
    refresh_profitability_investment_values,
)


def _factor_row(
    factor_value_id: str,
    factor_id: str,
    source: str,
    security_id: str,
    value: float,
    available_at: dt.datetime,
) -> list[object]:
    return [
        factor_value_id,
        factor_id,
        factor_id,
        "input",
        security_id,
        security_id,
        dt.date(2025, 2, 28),
        value * 10,
        value,
        available_at,
        "[]",
        "{}",
        True,
        "fixture",
        source,
    ]


def test_profitability_investment_formula_orientation_and_lineage() -> None:
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    store._initialized = True
    try:
        store.con.execute(
            """CREATE TABLE fundamental_factor_values (
                factor_value_id VARCHAR,factor_id VARCHAR,factor_name VARCHAR,family VARCHAR,
                security_id VARCHAR,symbol VARCHAR,as_of_date DATE,raw_value DOUBLE,
                value DOUBLE,available_at TIMESTAMP,input_ids_json VARCHAR,
                input_lineage_json VARCHAR,is_latest_revision BOOLEAN,run_id VARCHAR,
                source VARCHAR
            )"""
        )
        for index, security_id in enumerate(("LOW", "MID", "HIGH")):
            value = float(index - 1)
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                _factor_row(
                    f"cash-{security_id}",
                    CASH_PROFITABILITY_FACTOR_ID,
                    CASH_PROFITABILITY_SOURCE_NAME,
                    security_id,
                    value,
                    dt.datetime(2025, 2, 27, 22),
                ),
            )
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                _factor_row(
                    f"investment-{security_id}",
                    ASSET_GROWTH_FACTOR_ID,
                    ASSET_GROWTH_SOURCE_NAME,
                    security_id,
                    value,
                    dt.datetime(2025, 2, 28, 22),
                ),
            )
        count = refresh_profitability_investment_values(
            store,
            ProfitabilityInvestmentOptions(
                minimum_names_per_date=2, winsor_limit=0.0, run_id="test"
            ),
        )
        rows = store.con.execute(
            """SELECT security_id,raw_value,value,available_at,input_lineage_json
               FROM fundamental_factor_values WHERE factor_id=? ORDER BY raw_value""",
            [FACTOR_ID],
        ).fetchall()
        assert count == 3
        assert [row[0] for row in rows] == ["LOW", "MID", "HIGH"]
        assert [row[1] for row in rows] == pytest.approx([-1.0, 0.0, 1.0])
        assert rows[0][2] < rows[1][2] < rows[2][2]
        assert rows[2][3] == dt.datetime(2025, 2, 28, 22)
        lineage = json.loads(rows[2][4])
        assert lineage["cash_profitability_weight"] == pytest.approx(0.5)
        assert lineage["conservative_investment_weight"] == pytest.approx(0.5)
        assert lineage["weights_fitted_to_returns"] is False
        assert lineage["cash_profitability_factor_value_id"] == "cash-HIGH"
        assert lineage["conservative_investment_factor_value_id"] == "investment-HIGH"
    finally:
        store.connection.close()
        store.connection = None


def test_profitability_investment_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0253, "_refresh_schema_contract_v2_pin", lambda _conn: None)
    bodies_0253.MIGRATIONS[0].up(conn)
    assert conn.execute(
        """SELECT is_point_in_time_safe,source FROM factor_definition
           WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone() == (True, SOURCE_NAME)
    dependencies = conn.execute(
        """SELECT dependency_factor_id FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_factor_id""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        (ASSET_GROWTH_FACTOR_ID,),
        (CASH_PROFITABILITY_FACTOR_ID,),
    ]
    conn.close()


def test_profitability_investment_rejects_fitted_weights() -> None:
    store = DuckDBStore(":memory:")
    with pytest.raises(ValueError, match="sum to one"):
        refresh_profitability_investment_values(
            store,
            ProfitabilityInvestmentOptions(
                cash_profitability_weight=0.7,
                conservative_investment_weight=0.7,
            ),
        )
