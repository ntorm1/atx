from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from atx_db.connection import DuckDBStore
from atx_db.migrations import bodies_0249
from atx_db.organization_capital import (
    FACTOR_ID,
    SOURCE_NAME,
    OrganizationCapitalOptions,
    refresh_organization_capital_values,
)


def _expected_ratio(sga_values: list[float]) -> float:
    real_investments = [value/100.0 for value in sga_values]
    capital = real_investments[0]/0.25
    for investment in real_investments[1:]:
        capital = 0.85*capital+investment
    return capital/(1000.0/100.0)


def test_organization_capital_formula_orientation_and_lineage() -> None:
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
                security_id VARCHAR,accession_number VARCHAR,period_start DATE,period_end DATE,
                canonical_metric VARCHAR,unit VARCHAR,period_type VARCHAR,form VARCHAR,
                value DOUBLE,available_at TIMESTAMP,revision_sequence INTEGER,
                statement_point_id VARCHAR
            );
            CREATE TABLE macro_observations (
                series_id VARCHAR,observation_date DATE,as_of_date DATE,
                available_at TIMESTAMP,value DOUBLE,source VARCHAR
            );
            """
        )
        for year in range(2020,2025):
            store.con.execute(
                "INSERT INTO macro_observations VALUES (?,?,?,?,?,?)",
                ["CPIAUCSL",dt.date(year,12,1),dt.date(year,12,1),
                 dt.datetime(year,12,1,22),100.0,"fixture"],
            )
        paths = {"LOW": [50.0]*5,"MID": [100.0]*5,"HIGH": [200.0]*5}
        for security_id,sga_values in paths.items():
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [f"parent-{security_id}",ASSET_GROWTH_FACTOR_ID,"parent","investment",
                 security_id,security_id,dt.date(2025,6,30),0.0,0.0,
                 dt.datetime(2025,6,30,22),"[]","{}",True,"parent","fixture"],
            )
            for index,sga in enumerate(sga_values):
                year = 2020+index
                accession = f"{security_id}-{year}K"
                available_at = dt.datetime(year+1,2,15,22)
                store.con.execute(
                    "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                    [security_id,accession,dt.date(year,1,1),dt.date(year,12,31),
                     "sga","USD","duration","10-K",sga,available_at,1,
                     f"{accession}-sga"],
                )
                store.con.execute(
                    "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                    [security_id,accession,None,dt.date(year,12,31),"total_assets","USD",
                     "instant","10-K",1000.0,available_at,1,f"{accession}-assets"],
                )
        count = refresh_organization_capital_values(
            store,
            OrganizationCapitalOptions(
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
        assert [row[1] for row in rows] == pytest.approx(
            [_expected_ratio(paths[key]) for key in ("LOW","MID","HIGH")]
        )
        assert rows[0][2] < rows[1][2] < rows[2][2]
        lineage = json.loads(rows[2][3])
        assert lineage["history_observations"] == 5
        assert lineage["depreciation_rate"] == pytest.approx(0.15)
        assert lineage["sga_values"] == [200.0]*5
        assert lineage["missing_sga_imputed_as_zero"] is False
    finally:
        store.connection.close()
        store.connection = None


def test_organization_capital_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0249,"_refresh_schema_contract_v2_pin",lambda _conn: None)
    bodies_0249.MIGRATIONS[0].up(conn)
    expression = conn.execute(
        "SELECT expression,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone()
    assert expression == (
        "OC_t=(1-0.15)*OC_t_1+SGA_t/CPI_t; OC_0=(SGA_1/CPI_1)/(0.10+0.15); "
        "score=OC_t/(assets_t/CPI_t)",True,SOURCE_NAME,
    )
    dependencies = conn.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_type,dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor",ASSET_GROWTH_FACTOR_ID),("metric","sga"),
        ("metric","total_assets"),("source","CPIAUCSL"),
    ]
    conn.close()
