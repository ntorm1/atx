from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_factor.atx_db import load_factor_panel


def test_load_factor_panel_pushes_filter_into_both_factor_tables(tmp_path) -> None:
    path = tmp_path / "warehouse.duckdb"
    con = duckdb.connect(str(path))
    con.execute(
        """
        CREATE TABLE fundamental_factor_values (
            security_id VARCHAR,as_of_date DATE,factor_id VARCHAR,value DOUBLE,
            available_at TIMESTAMP,source_loaded_at TIMESTAMP,
            input_lineage_json VARCHAR,is_latest_revision BOOLEAN
        );
        CREATE TABLE cross_domain_factor_values (
            security_id VARCHAR,as_of_date DATE,factor_id VARCHAR,value DOUBLE,
            available_at TIMESTAMP,source_loaded_at TIMESTAMP,
            input_lineage_json VARCHAR,is_latest_revision BOOLEAN
        );
        CREATE TABLE universe_membership (
            universe_id VARCHAR,security_id VARCHAR,valid_from DATE,valid_to DATE,
            as_of_date DATE,is_member BOOLEAN,is_latest_revision BOOLEAN,
            available_at TIMESTAMP,source_loaded_at TIMESTAMP,source VARCHAR
        );
        CREATE TABLE equity_daily_bars (
            security_id VARCHAR,trade_date DATE,close DOUBLE,split_factor DOUBLE,volume BIGINT
        )
        """
    )
    dates = [dt.date(2020, 1, day) for day in range(2, 7)]
    securities = ("S1", "S2")
    for security_index, security_id in enumerate(securities):
        con.execute(
            "INSERT INTO universe_membership VALUES (?,?,?,?,?,?,?,?,?,?)",
            [
                "us_common_equity_liquid_v1",
                security_id,
                dt.date(2020, 1, 1),
                None,
                dt.date(2020, 1, 1),
                True,
                True,
                dt.datetime(2020, 1, 1, 20),
                dt.datetime(2020, 1, 1, 21),
                "fixture",
            ],
        )
        for date_index, as_of_date in enumerate(dates):
            close = 10.0 * (security_index + 1) + date_index
            con.execute(
                "INSERT INTO equity_daily_bars VALUES (?,?,?,?,?)",
                [security_id, as_of_date, close, 1.0, 1_000],
            )
            if date_index == len(dates) - 1:
                continue
            values = [
                security_id,
                as_of_date,
                None,
                float(date_index + security_index),
                dt.datetime.combine(as_of_date, dt.time(20)),
                dt.datetime.combine(as_of_date, dt.time(21)),
                "{}",
                True,
            ]
            con.execute(
                "INSERT INTO fundamental_factor_values VALUES (?,?,?,?,?,?,?,?)",
                [*values[:2], "fundamental_fixture", *values[3:]],
            )
            con.execute(
                "INSERT INTO cross_domain_factor_values VALUES (?,?,?,?,?,?,?,?)",
                [*values[:2], "cross_fixture", *values[3:]],
            )
    con.close()

    panel = load_factor_panel(
        path,
        ["fundamental_fixture", "cross_fixture"],
        horizon_days=1,
        threads=1,
    )

    assert panel.height == 16
    assert set(panel["signal_id"].to_list()) == {"fundamental_fixture", "cross_fixture"}
    assert panel["date"].n_unique() == 4
    first = panel.filter(
        (panel["signal_id"] == "fundamental_fixture")
        & (panel["asset_id"] == "S1")
        & (panel["date"] == dt.date(2020, 1, 2))
    )
    assert first["forward_return"].item() == pytest.approx(0.1)
