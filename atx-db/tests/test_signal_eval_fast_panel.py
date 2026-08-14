from __future__ import annotations

import datetime as dt
from types import SimpleNamespace

import duckdb

from atx_db.signal_eval import load_panel_for_eval


def test_load_panel_for_eval_filters_base_tables_before_pit_windows() -> None:
    con = duckdb.connect(":memory:")
    try:
        for table in ("fundamental_factor_values", "cross_domain_factor_values"):
            con.execute(
                f"""
                CREATE TABLE {table} (
                    security_id VARCHAR,as_of_date DATE,factor_id VARCHAR,value DOUBLE,
                    available_at TIMESTAMP,source_loaded_at TIMESTAMP,run_id VARCHAR,
                    input_lineage_json VARCHAR,is_latest_revision BOOLEAN
                )
                """
            )
        con.execute(
            """
            CREATE TABLE universe_membership (
                universe_id VARCHAR,security_id VARCHAR,valid_from DATE,valid_to DATE,
                as_of_date DATE,is_member BOOLEAN,is_latest_revision BOOLEAN,
                available_at TIMESTAMP,source_loaded_at TIMESTAMP,source VARCHAR
            )
            """
        )
        for security_id in ("S1", "S2"):
            con.execute(
                "INSERT INTO universe_membership VALUES (?,?,?,?,?,true,true,?,?,?)",
                [
                    "us_common_equity_liquid_v1",
                    security_id,
                    dt.date(2020, 1, 1),
                    None,
                    dt.date(2020, 1, 1),
                    dt.datetime(2020, 1, 1, 20),
                    dt.datetime(2020, 1, 1, 21),
                    "fixture",
                ],
            )
        row_tail = [
            dt.date(2020, 1, 31),
            None,
            1.5,
            dt.datetime(2020, 1, 31, 20),
            dt.datetime(2020, 1, 31, 21),
            "run",
            "{}",
            True,
        ]
        con.execute(
            "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?,?)",
            ["S1", *row_tail[:1], "wanted", *row_tail[2:]],
        )
        con.execute(
            "INSERT INTO cross_domain_factor_values VALUES (?,?,?,?,?,?,?,?,?)",
            ["S2", *row_tail[:1], "other", *row_tail[2:]],
        )

        panel = load_panel_for_eval(SimpleNamespace(con=con), factor_ids=["wanted"])

        assert len(panel) == 1
        row = panel.iloc[0]
        assert row["security_id"] == "S1"
        assert row["as_of_date"].date() == dt.date(2020, 1, 31)
        assert row["factor_id"] == "wanted"
        assert row["value"] == 1.5
        assert row["available_at"].to_pydatetime() == dt.datetime(2020, 1, 31, 20)
    finally:
        con.close()
