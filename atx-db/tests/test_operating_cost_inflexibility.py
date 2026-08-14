from __future__ import annotations

import datetime as dt
import json
import math
import statistics

import duckdb
import pytest

from atx_db.asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from atx_db.connection import DuckDBStore
from atx_db.migrations import bodies_0248
from atx_db.operating_cost_inflexibility import (
    FACTOR_ID,
    SOURCE_NAME,
    OperatingCostInflexibilityOptions,
    refresh_operating_cost_inflexibility_values,
)


def test_operating_cost_inflexibility_formula_orientation_and_lineage() -> None:
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
            """
        )
        paths = {
            "FLEX": [50.0,100.0,200.0,100.0,50.0],
            "MID": [90.0,100.0,110.0,100.0,90.0],
            "STABLE": [100.0,100.0,100.0,100.0,100.0],
        }
        for security_id,costs in paths.items():
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [f"parent-{security_id}",ASSET_GROWTH_FACTOR_ID,"parent","investment",
                 security_id,security_id,dt.date(2025,6,30),0.0,0.0,
                 dt.datetime(2025,6,30,22),"[]","{}",True,"parent","fixture"],
            )
            for index,cost in enumerate(costs):
                year = 2020+index
                accession = f"{security_id}-{year}K"
                for metric,value in (("cogs",cost*0.7),("sga",cost*0.3)):
                    store.con.execute(
                        "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                        [security_id,accession,dt.date(year,1,1),dt.date(year,12,31),
                         metric,"USD","duration","10-K",value,
                         dt.datetime(year+1,2,15,22),1,f"{accession}-{metric}"],
                    )
        count = refresh_operating_cost_inflexibility_values(
            store,
            OperatingCostInflexibilityOptions(
                minimum_names_per_date=2,winsor_limit=0.0,run_id="test"
            ),
        )
        rows = store.con.execute(
            """SELECT security_id,raw_value,value,input_lineage_json
               FROM fundamental_factor_values WHERE factor_id=? ORDER BY raw_value""",
            [FACTOR_ID],
        ).fetchall()
        assert count == 3
        assert [row[0] for row in rows] == ["FLEX","MID","STABLE"]
        expected = {
            security_id: -statistics.stdev(math.log(cost) for cost in costs)
            for security_id,costs in paths.items()
        }
        assert [row[1] for row in rows] == pytest.approx(
            [expected["FLEX"],expected["MID"],expected["STABLE"]]
        )
        assert rows[0][2] < rows[1][2] < rows[2][2]
        lineage = json.loads(rows[2][3])
        assert lineage["observation_count"] == 5
        assert lineage["operating_costs"] == [100.0]*5
        assert lineage["cost_sdoc"] == pytest.approx(0.0)
        assert lineage["missing_components_imputed"] is False
    finally:
        store.connection.close()
        store.connection = None


def test_operating_cost_inflexibility_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0248,"_refresh_schema_contract_v2_pin",lambda _conn: None)
    bodies_0248.MIGRATIONS[0].up(conn)
    assert conn.execute(
        "SELECT expression,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == (
        "-stddev_samp(ln(cogs+sga),five_annual_observations)",True,SOURCE_NAME,
    )
    dependencies = conn.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_type,dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor",ASSET_GROWTH_FACTOR_ID),("metric","cogs"),("metric","sga")
    ]
    conn.close()
