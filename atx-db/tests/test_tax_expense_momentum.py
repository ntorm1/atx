from __future__ import annotations

import datetime as dt
import json

import duckdb

from atx_db.asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from atx_db.connection import DuckDBStore
from atx_db.migrations import bodies_0245
from atx_db.tax_expense_momentum import (
    FACTOR_ID,
    SOURCE_NAME,
    TaxExpenseMomentumOptions,
    refresh_tax_expense_momentum_values,
)


def test_tax_expense_momentum_orientation_and_lineage() -> None:
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
        tax_pairs = {"NEG": (0.0, 5.0), "MID": (8.0, 5.0), "POS": (15.0, 5.0)}
        for security_id,(current_tax,prior_tax) in tax_pairs.items():
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [f"parent-{security_id}",ASSET_GROWTH_FACTOR_ID,"parent","investment",
                 security_id,security_id,dt.date(2024,11,29),0.0,0.0,
                 dt.datetime(2024,11,29,22),"[]","{}",True,"parent","fixture"],
            )
            for accession,start,end,tax in (
                (f"{security_id}-2024Q3",dt.date(2024,7,1),dt.date(2024,9,30),current_tax),
                (f"{security_id}-2023Q3",dt.date(2023,7,1),dt.date(2023,9,30),prior_tax),
            ):
                store.con.execute(
                    "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                    [security_id,accession,start,end,"income_tax","USD","duration","10-Q",
                     tax,dt.datetime(end.year,end.month,1,22)+dt.timedelta(days=40),1,
                     f"{accession}-tax"],
                )
            store.con.execute(
                "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                [security_id,f"{security_id}-2023Q3",None,dt.date(2023,9,30),"total_assets",
                 "USD","instant","10-Q",100.0,dt.datetime(2023,11,1,22),1,
                 f"{security_id}-2023Q3-assets"],
            )
        count = refresh_tax_expense_momentum_values(
            store,
            TaxExpenseMomentumOptions(minimum_names_per_date=2,winsor_limit=0.0,run_id="test"),
        )
        rows = store.con.execute(
            """SELECT security_id,raw_value,value,input_lineage_json
               FROM fundamental_factor_values WHERE factor_id=? ORDER BY raw_value""",
            [FACTOR_ID],
        ).fetchall()
        assert count == 3
        assert [row[0] for row in rows] == ["NEG","MID","POS"]
        assert [row[1] for row in rows] == [-0.05,0.03,0.1]
        assert rows[0][2] < rows[1][2] < rows[2][2]
        lineage = json.loads(rows[2][3])
        assert lineage["tax_expense_surprise"] == 10.0
        assert lineage["prior_assets"] == 100.0
        assert lineage["missing_components_imputed"] is False
    finally:
        store.connection.close()
        store.connection = None


def test_tax_expense_momentum_definition_is_governed(monkeypatch) -> None:
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
    monkeypatch.setattr(bodies_0245,"_refresh_schema_contract_v2_pin",lambda _conn: None)
    bodies_0245.MIGRATIONS[0].up(conn)
    assert conn.execute(
        "SELECT expression,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == (
        "(income_tax_q-income_tax_q_minus_4)/assets_q_minus_4",True,SOURCE_NAME,
    )
    dependencies = conn.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_type,dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor",ASSET_GROWTH_FACTOR_ID),("metric","income_tax"),("metric","total_assets")
    ]
    conn.close()
