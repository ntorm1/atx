from __future__ import annotations

import datetime as dt
import json
import math

import duckdb

from atx_db.asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from atx_db.connection import DuckDBStore
from atx_db.external_financing import (
    FACTOR_ID,
    SOURCE_NAME,
    ExternalFinancingOptions,
    refresh_external_financing_values,
)
from atx_db.migrations import bodies_0243


def _parent_lineage(security_id: str) -> str:
    return json.dumps(
        {
            "assets": {
                "current": {
                    "accession_number": f"{security_id}-2024",
                    "period_end": "2024-12-31",
                },
                "prior": {
                    "statement_point_id": f"{security_id}-assets-2023",
                    "period_end": "2023-12-31",
                    "value": 100.0,
                    "available_at": "2024-02-20T22:00:00",
                },
            }
        }
    )


def test_external_financing_orientation_and_lineage() -> None:
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
        for security_id, financing in (("RAISE", 20.0), ("NEUTRAL", 0.0), ("DISTRIBUTE", -10.0)):
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [
                    f"parent-{security_id}",
                    ASSET_GROWTH_FACTOR_ID,
                    "parent",
                    "fundamental_investment",
                    security_id,
                    security_id,
                    dt.date(2025, 3, 31),
                    0.0,
                    0.0,
                    dt.datetime(2025, 3, 31, 22),
                    "[]",
                    _parent_lineage(security_id),
                    True,
                    "parent",
                    "fixture",
                ],
            )
            store.con.execute(
                "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                [
                    security_id,
                    f"{security_id}-2024",
                    dt.date(2024, 1, 1),
                    dt.date(2024, 12, 31),
                    "financing_cash_flow",
                    "USD",
                    "duration",
                    "10-K",
                    financing,
                    dt.datetime(2025, 2, 20, 22),
                    1,
                    f"{security_id}-financing",
                ],
            )
        count = refresh_external_financing_values(
            store,
            ExternalFinancingOptions(
                minimum_names_per_date=2,
                winsor_limit=0.0,
                run_id="test",
            ),
        )
        rows = store.con.execute(
            """
            SELECT security_id,raw_value,value,input_lineage_json
            FROM fundamental_factor_values WHERE factor_id=? ORDER BY raw_value
            """,
            [FACTOR_ID],
        ).fetchall()
        assert count == 3
        assert [row[0] for row in rows] == ["RAISE", "NEUTRAL", "DISTRIBUTE"]
        assert [row[1] for row in rows] == [-0.2, 0.0, 0.1]
        assert rows[0][2] < rows[1][2] < rows[2][2]
        assert math.isclose(sum(row[2] for row in rows), 0.0, abs_tol=1e-12)
        lineage = json.loads(rows[2][3])
        assert lineage["formula"] == "-financing_cash_flow/prior_total_assets"
        assert lineage["missing_components_imputed"] is False
        assert lineage["parent_factor_id"] == ASSET_GROWTH_FACTOR_ID
    finally:
        store.connection.close()
        store.connection = None


def test_external_financing_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0243, "_refresh_schema_contract_v2_pin", lambda _conn: None)
    bodies_0243.MIGRATIONS[0].up(conn)
    definition = conn.execute(
        "SELECT expression,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone()
    assert definition == ("-financing_cash_flow/prior_total_assets", True, SOURCE_NAME)
    dependencies = conn.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_type,dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", ASSET_GROWTH_FACTOR_ID),
        ("metric", "financing_cash_flow"),
    ]
    conn.close()
