from __future__ import annotations

import datetime as dt
import json
import math

import duckdb

from atx_db.connection import DuckDBStore
from atx_db.operating_leverage import (
    FACTOR_ID,
    PARENT_FACTOR_ID,
    OperatingLeverageOptions,
    refresh_operating_leverage_values,
)


def _lineage(*,cogs: float | None,sga: float,revenue: float,gross_profit: float) -> str:
    def item(value: float | None,name: str) -> dict[str,object]:
        return {"value": value,"id": None if value is None else f"{name}-id"}

    return json.dumps(
        {
            "operating_profitability": {
                "cogs": item(cogs,"cogs"),
                "sga": item(sga,"sga"),
                "revenue": item(revenue,"revenue"),
            },
            "gross_profit": {
                **item(gross_profit,"gross-profit"),
                "period_end": "2019-12-31",
            },
            "total_assets": item(100.0,"assets"),
        }
    )


def test_operating_leverage_uses_reported_and_algebraic_cogs_paths() -> None:
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
            )
            """
        )
        parent_rows = (
            ("S1",_lineage(cogs=30.0,sga=10.0,revenue=100.0,gross_profit=70.0)),
            ("S2",_lineage(cogs=None,sga=20.0,revenue=100.0,gross_profit=60.0)),
        )
        for security_id,lineage in parent_rows:
            store.con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                [
                    f"parent-{security_id}",PARENT_FACTOR_ID,"parent","fundamental_profitability",
                    security_id,security_id,dt.date(2020,3,31),0.0,0.0,
                    dt.datetime(2020,3,31,22),"[]",lineage,True,"parent-run","fixture",
                ],
            )

        count = refresh_operating_leverage_values(
            store,
            OperatingLeverageOptions(minimum_names_per_date=2,winsor_limit=0.0,run_id="test"),
        )
        rows = store.con.execute(
            """
            SELECT security_id,raw_value,value,input_lineage_json
            FROM fundamental_factor_values WHERE factor_id=? ORDER BY security_id
            """,
            [FACTOR_ID],
        ).fetchall()

        assert count == 2
        assert rows[0][1] == 0.4
        assert rows[1][1] == 0.6
        assert rows[0][2] < rows[1][2]
        assert math.isclose(sum(row[2] for row in rows),0.0,abs_tol=1e-12)
        assert json.loads(rows[0][3])["cogs_method"] == "reported_cogs"
        assert json.loads(rows[1][3])["cogs_method"] == "revenue_minus_gross_profit"
    finally:
        store.connection.close()
        store.connection = None
