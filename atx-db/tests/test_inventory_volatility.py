from __future__ import annotations

import datetime as dt
import json
import statistics

import duckdb
import pytest

from atx_db.asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from atx_db.connection import DuckDBStore
from atx_db.inventory_volatility import (
    FACTOR_ID,
    SOURCE_NAME,
    InventoryVolatilityOptions,
    refresh_inventory_volatility_values,
)
from atx_db.migrations import bodies_0247


def test_inventory_volatility_formula_orientation_and_lineage() -> None:
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
                security_id VARCHAR,period_end DATE,canonical_metric VARCHAR,unit VARCHAR,
                period_type VARCHAR,form VARCHAR,value DOUBLE,available_at TIMESTAMP,
                revision_sequence INTEGER,statement_point_id VARCHAR,accession_number VARCHAR
            );
            """
        )
        paths = {
            "LOW": [100.0,100.0,100.0,100.0],
            "MID": [90.0,100.0,110.0,100.0],
            "HIGH": [50.0,100.0,150.0,100.0],
        }
        periods = (
            (dt.date(2024,3,31),dt.datetime(2024,5,1,22)),
            (dt.date(2024,6,30),dt.datetime(2024,8,1,22)),
            (dt.date(2024,9,30),dt.datetime(2024,11,1,22)),
            (dt.date(2024,12,31),dt.datetime(2025,2,15,22)),
        )
        for security_id,values in paths.items():
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [f"parent-{security_id}",ASSET_GROWTH_FACTOR_ID,"parent","investment",
                 security_id,security_id,dt.date(2025,2,28),0.0,0.0,
                 dt.datetime(2025,2,28,22),"[]","{}",True,"parent","fixture"],
            )
            for index,((period_end,available_at),value) in enumerate(
                zip(periods,values,strict=True)
            ):
                store.con.execute(
                    "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    [security_id,period_end,"inventory","USD","instant","10-Q",value,
                     available_at,1,f"{security_id}-inventory-{index}",
                     f"{security_id}-accession-{index}"],
                )
        count = refresh_inventory_volatility_values(
            store,
            InventoryVolatilityOptions(
                minimum_names_per_date=2,winsor_limit=0.0,run_id="test"
            ),
        )
        rows = store.con.execute(
            """SELECT security_id,raw_value,value,input_lineage_json
               FROM fundamental_factor_values WHERE factor_id=? ORDER BY raw_value""",
            [FACTOR_ID],
        ).fetchall()
        assert count == 3
        assert [row[0] for row in rows] == ["LOW","MID","HIGH"]
        expected = [statistics.stdev(values)/statistics.mean(values) for values in paths.values()]
        assert [row[1] for row in rows] == pytest.approx(expected)
        assert rows[0][2] < rows[1][2] < rows[2][2]
        lineage = json.loads(rows[2][3])
        assert lineage["observation_count"] == 4
        assert lineage["inventory_values"] == [50.0,100.0,150.0,100.0]
        assert lineage["inventory_standard_deviation"] == pytest.approx(
            statistics.stdev(paths["HIGH"])
        )
        assert lineage["missing_components_imputed"] is False
    finally:
        store.connection.close()
        store.connection = None


def test_inventory_volatility_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0247,"_refresh_schema_contract_v2_pin",lambda _conn: None)
    bodies_0247.MIGRATIONS[0].up(conn)
    assert conn.execute(
        "SELECT expression,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == (
        "stddev_samp(inventory_q0..q3)/mean(inventory_q0..q3)",True,SOURCE_NAME,
    )
    dependencies = conn.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_type,dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor",ASSET_GROWTH_FACTOR_ID),("metric","inventory")
    ]
    conn.close()
