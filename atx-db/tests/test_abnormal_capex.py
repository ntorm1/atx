from __future__ import annotations

import datetime as dt
import json
import math

import duckdb

from atx_db.abnormal_capex import (
    FACTOR_ID,
    AbnormalCapexOptions,
    refresh_abnormal_capex_values,
)
from atx_db.connection import DuckDBStore


def test_refresh_abnormal_capex_requires_three_pit_lags_and_orients_low_investment() -> None:
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
            CREATE TABLE fundamental_ttm_points (
                ttm_point_id VARCHAR,security_id VARCHAR,canonical_metric VARCHAR,unit VARCHAR,
                ttm_value DOUBLE,ttm_end_date DATE,available_at TIMESTAMP,
                revision_sequence INTEGER,source VARCHAR,
                input_statement_point_ids_json VARCHAR,input_accessions_json VARCHAR
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
        decision_date = dt.date(2020,3,31)
        prior_ends = (dt.date(2018,12,31),dt.date(2017,12,31),dt.date(2016,12,31))
        for security_id,current_capex in (("S1",100.0),("S2",50.0)):
            store.con.execute(
                "INSERT INTO market_cap VALUES (?,?,?,?,?,?,?,true)",
                [
                    f"cap-{security_id}",security_id,security_id,decision_date,1_000.0,
                    dt.datetime(2020,3,31,22),'{"fixture":"market_cap"}',
                ],
            )
            store.con.execute(
                "INSERT INTO universe_membership VALUES (?,?,?,?,?,true,true,?,?,?)",
                [
                    "us_common_equity_liquid_v1",security_id,dt.date(2020,1,1),None,
                    dt.date(2020,1,1),dt.datetime(2020,1,1,20),
                    dt.datetime(2020,1,1,21),"fixture",
                ],
            )
            observations = [(dt.date(2019,12,31),current_capex),*((end,100.0) for end in prior_ends)]
            for index,(period_end,capex) in enumerate(observations):
                store.con.execute(
                    "INSERT INTO fundamental_ttm_points VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    [
                        f"ttm-{security_id}-{index}",security_id,"capital_expenditures","USD",
                        -capex,period_end,dt.datetime(2020,2,15,22),1,"fixture","[]","[]",
                    ],
                )

        count = refresh_abnormal_capex_values(
            store,
            AbnormalCapexOptions(minimum_names_per_date=2,winsor_limit=0.0,run_id="test"),
        )
        rows = store.con.execute(
            """
            SELECT security_id,raw_value,value,input_lineage_json
            FROM fundamental_factor_values WHERE factor_id=? ORDER BY security_id
            """,
            [FACTOR_ID],
        ).fetchall()

        assert count == 2
        assert rows[0][1] == 0.0
        assert rows[1][1] == 0.5
        assert rows[0][2] < rows[1][2]
        assert math.isclose(sum(row[2] for row in rows),0.0,abs_tol=1e-12)
        lineage = json.loads(rows[1][3])
        assert lineage["current_capex"] == 50.0
        assert lineage["baseline_capex"] == 100.0
        assert lineage["prior_3y_id"] == "ttm-S2-3"
    finally:
        store.connection.close()
        store.connection = None
