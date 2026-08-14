from __future__ import annotations

import datetime as dt
import json
import math

import duckdb

from atx_db.connection import DuckDBStore
from atx_db.rd_intensity import FACTOR_ID, RdIntensityOptions, refresh_rd_intensity_values


def test_refresh_rd_intensity_is_pit_lineaged_and_standardized() -> None:
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    store._initialized = True
    try:
        store.con.execute(
            """
            CREATE TABLE market_cap (
                market_cap_id VARCHAR,security_id VARCHAR,symbol VARCHAR,trade_date DATE,
                market_cap DOUBLE,available_at TIMESTAMP,input_lineage_json VARCHAR,
                is_latest_revision BOOLEAN
            );
            CREATE TABLE universe_membership (
                universe_id VARCHAR,security_id VARCHAR,valid_from DATE,valid_to DATE,
                as_of_date DATE,is_member BOOLEAN,is_latest_revision BOOLEAN,
                available_at TIMESTAMP,source_loaded_at TIMESTAMP,source VARCHAR
            );
            CREATE TABLE fundamental_statement_points (
                security_id VARCHAR,accession_number VARCHAR,period_start DATE,period_end DATE,
                value DOUBLE,available_at TIMESTAMP,revision_sequence INTEGER,
                statement_point_id VARCHAR,source VARCHAR,canonical_metric VARCHAR,
                unit VARCHAR,form VARCHAR
            );
            CREATE TABLE fundamental_factor_values (
                factor_value_id VARCHAR,factor_id VARCHAR,factor_name VARCHAR,family VARCHAR,
                security_id VARCHAR,symbol VARCHAR,as_of_date DATE,raw_value DOUBLE,
                value DOUBLE,available_at TIMESTAMP,input_ids_json VARCHAR,
                input_lineage_json VARCHAR,is_latest_revision BOOLEAN,run_id VARCHAR,
                source VARCHAR
            )
            """
        )
        decision_date = dt.date(2020, 3, 31)
        for index, security_id in enumerate(("S1", "S2"),start=1):
            store.con.execute(
                "INSERT INTO market_cap VALUES (?,?,?,?,?,?,?,true)",
                [
                    f"cap-{security_id}",
                    security_id,
                    security_id,
                    decision_date,
                    100.0,
                    dt.datetime(2020, 3, 31,22),
                    '{"fixture":"market_cap"}',
                ],
            )
            store.con.execute(
                "INSERT INTO universe_membership VALUES (?,?,?,?,?,true,true,?,?,?)",
                [
                    "us_common_equity_liquid_v1",
                    security_id,
                    dt.date(2020, 1, 1),
                    None,
                    dt.date(2020, 1, 1),
                    dt.datetime(2020, 1, 1,20),
                    dt.datetime(2020, 1, 1,21),
                    "fixture",
                ],
            )
            store.con.execute(
                "INSERT INTO fundamental_statement_points VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                [
                    security_id,
                    f"accession-{security_id}",
                    dt.date(2019, 1, 1),
                    dt.date(2019, 12, 31),
                    10.0 * index,
                    dt.datetime(2020, 2, 15,22),
                    1,
                    f"rd-{security_id}",
                    "fixture",
                    "rd_expense",
                    "USD",
                    "10-K",
                ],
            )

        count = refresh_rd_intensity_values(
            store,
            RdIntensityOptions(
                minimum_names_per_date=2,
                winsor_limit=0.0,
                run_id="test",
            ),
        )
        rows = store.con.execute(
            """
            SELECT security_id,raw_value,value,available_at,input_lineage_json
            FROM fundamental_factor_values
            WHERE factor_id=? ORDER BY security_id
            """,
            [FACTOR_ID],
        ).fetchall()

        assert count == 2
        assert rows[0][1] == 0.1
        assert rows[1][1] == 0.2
        assert rows[0][2] < rows[1][2]
        assert math.isclose(sum(row[2] for row in rows),0.0,abs_tol=1e-12)
        assert all(row[3] == dt.datetime(2020,3,31,22) for row in rows)
        lineage = json.loads(rows[0][4])
        assert lineage["rd_statement_point_id"] == "rd-S1"
        assert lineage["missing_rd_imputed_as_zero"] is False
    finally:
        store.connection.close()
        store.connection = None
