from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.migrations import bodies_0251
from atx_db.net_operating_assets import FACTOR_ID as NET_OPERATING_ASSETS_FACTOR_ID
from atx_db.net_operating_assets import SOURCE_NAME as NET_OPERATING_ASSETS_SOURCE_NAME
from atx_db.rsst_accruals import (
    FACTOR_ID,
    SOURCE_NAME,
    RsstAccrualsOptions,
    refresh_rsst_accruals_values,
)


def test_rsst_formula_orientation_and_lineage() -> None:
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
        current_cash = {"LOW": 20.0, "MID": 10.0, "HIGH": 0.0}
        for security_id, cash in current_cash.items():
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [
                    f"parent-{security_id}",
                    NET_OPERATING_ASSETS_FACTOR_ID,
                    "parent",
                    "quality",
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
                    NET_OPERATING_ASSETS_SOURCE_NAME,
                ],
            )
            for period_end, available_at, accession, period_cash in (
                (
                    dt.date(2023, 12, 31),
                    dt.datetime(2024, 2, 15, 22),
                    "prior",
                    10.0,
                ),
                (
                    dt.date(2024, 12, 31),
                    dt.datetime(2025, 2, 15, 22),
                    "current",
                    cash,
                ),
            ):
                values = {
                    "total_assets": 100.0,
                    "total_liabilities": 60.0,
                    "cash_st_inv": period_cash,
                    "st_debt": 10.0,
                    "lt_debt": 20.0,
                }
                for metric, value in values.items():
                    store.con.execute(
                        "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                        [
                            security_id,
                            period_end,
                            metric,
                            "USD",
                            "instant",
                            "10-K",
                            value,
                            available_at,
                            1,
                            f"{security_id}-{accession}-{metric}",
                            f"{security_id}-{accession}",
                        ],
                    )
        count = refresh_rsst_accruals_values(
            store,
            RsstAccrualsOptions(
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
        assert [row[1] for row in rows] == pytest.approx([-0.1, 0.0, 0.1])
        assert rows[0][2] < rows[1][2] < rows[2][2]
        lineage = json.loads(rows[2][3])
        assert lineage["current_net_operating_assets"] == pytest.approx(50.0)
        assert lineage["prior_net_operating_assets"] == pytest.approx(60.0)
        assert lineage["accessions"] == ["LOW-current", "LOW-prior"]
        assert lineage["missing_components_imputed"] is False
    finally:
        store.connection.close()
        store.connection = None


def test_rsst_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0251, "_refresh_schema_contract_v2_pin", lambda _conn: None)
    bodies_0251.MIGRATIONS[0].up(conn)
    definition = conn.execute(
        """SELECT expression,is_point_in_time_safe,source
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition == (
        "-delta(total_assets-cash_st_inv-total_liabilities+st_debt+lt_debt)/average_total_assets",
        True,
        SOURCE_NAME,
    )
    dependencies = conn.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_type,dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", NET_OPERATING_ASSETS_FACTOR_ID),
        ("metric", "cash_st_inv"),
        ("metric", "lt_debt"),
        ("metric", "st_debt"),
        ("metric", "total_assets"),
        ("metric", "total_liabilities"),
    ]
    conn.close()
