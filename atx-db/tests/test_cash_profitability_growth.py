from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.cash_profitability import SOURCE_NAME as CASH_SOURCE
from atx_db.cash_profitability_growth import (
    CASH_PROFITABILITY_FACTOR_ID,
    FACTOR_ID,
    SOURCE_NAME,
    CashProfitabilityGrowthOptions,
    refresh_cash_profitability_growth_values,
)
from atx_db.connection import DuckDBStore
from atx_db.migrations import bodies_0254


def _row(
    security_id: str,
    as_of_date: dt.date,
    period_end: dt.date,
    raw_value: float,
    value: float,
) -> list[object]:
    factor_value_id = f"{security_id}-{period_end}-{as_of_date}"
    available_at = dt.datetime.combine(as_of_date, dt.time(22))
    return [
        factor_value_id,
        CASH_PROFITABILITY_FACTOR_ID,
        "cash profitability",
        "input",
        security_id,
        security_id,
        as_of_date,
        raw_value,
        value,
        available_at,
        "[]",
        json.dumps({"current_statement": {"period_end": str(period_end)}}),
        True,
        "fixture",
        CASH_SOURCE,
    ]


def _store() -> DuckDBStore:
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    store._initialized = True
    store.con.execute(
        """CREATE TABLE fundamental_factor_values (
            factor_value_id VARCHAR,factor_id VARCHAR,factor_name VARCHAR,family VARCHAR,
            security_id VARCHAR,symbol VARCHAR,as_of_date DATE,raw_value DOUBLE,
            value DOUBLE,available_at TIMESTAMP,input_ids_json VARCHAR,
            input_lineage_json VARCHAR,is_latest_revision BOOLEAN,run_id VARCHAR,
            source VARCHAR
        )"""
    )
    return store


def test_level_growth_formula_orientation_and_lineage() -> None:
    store = _store()
    try:
        prior_date = dt.date(2024, 2, 29)
        current_date = dt.date(2025, 2, 28)
        for index, security_id in enumerate(("LOW", "MID", "HIGH")):
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                _row(
                    security_id,
                    prior_date,
                    dt.date(2023, 12, 31),
                    0.2 - 0.1 * index,
                    float(index - 1),
                ),
            )
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                _row(
                    security_id,
                    current_date,
                    dt.date(2024, 12, 31),
                    0.1,
                    float(index - 1),
                ),
            )
        count = refresh_cash_profitability_growth_values(
            store,
            CashProfitabilityGrowthOptions(
                start_date=current_date,
                minimum_names_per_date=2,
                winsor_limit=0.0,
                run_id="test",
            ),
        )
        rows = store.con.execute(
            """SELECT security_id,raw_value,value,input_lineage_json
               FROM fundamental_factor_values WHERE factor_id=? ORDER BY value""",
            [FACTOR_ID],
        ).fetchall()
        assert count == 3
        assert [row[0] for row in rows] == ["LOW", "MID", "HIGH"]
        assert rows[0][1] < rows[1][1] < rows[2][1]
        lineage = json.loads(rows[2][3])
        assert lineage["level_weight"] == pytest.approx(0.5)
        assert lineage["growth_weight"] == pytest.approx(0.5)
        assert lineage["growth_raw_value"] == pytest.approx(0.1)
        assert lineage["weights_fitted_to_returns"] is False
        assert lineage["prior_period_end"] == "2023-12-31"
    finally:
        store.connection.close()
        store.connection = None


def test_level_growth_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0254, "_refresh_schema_contract_v2_pin", lambda _conn: None)
    bodies_0254.MIGRATIONS[0].up(conn)
    assert conn.execute(
        "SELECT is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == (True, SOURCE_NAME)
    assert conn.execute(
        "SELECT dependency_factor_id FROM factor_dependency_edges WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == (CASH_PROFITABILITY_FACTOR_ID,)
    conn.close()


def test_level_growth_rejects_fitted_weights() -> None:
    store = DuckDBStore(":memory:")
    with pytest.raises(ValueError, match="sum to one"):
        refresh_cash_profitability_growth_values(
            store,
            CashProfitabilityGrowthOptions(level_weight=0.7, growth_weight=0.7),
        )
