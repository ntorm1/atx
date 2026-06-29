from __future__ import annotations

import datetime as dt

import pytest


SECURITY_ID = "TICKER-TEST"


def _date_value(value) -> dt.date:
    return value.date() if hasattr(value, "date") else value


def _seed_security(tmp_store) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO securities (
            security_id,
            issuer_id,
            primary_symbol,
            name,
            source
        )
        VALUES (?, ?, ?, ?, ?)
        """,
        [SECURITY_ID, SECURITY_ID, "TEST", "Test Corp.", "test"],
    )


def _insert_bar(tmp_store, trade_date: dt.date, close: float, volume: int = 1000) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source,
            security_id,
            vendor_security_id,
            symbol,
            trade_date,
            open,
            high,
            low,
            close,
            adjusted_close,
            volume,
            vwap,
            dividend_amount,
            split_factor,
            is_adjusted,
            available_at,
            run_id
        )
        VALUES (
            'test bars',
            ?, ?, 'TEST', ?,
            NULL, NULL, NULL,
            ?, NULL, ?, NULL, NULL, NULL,
            false, ?, 'bars-run'
        )
        """,
        [
            SECURITY_ID,
            SECURITY_ID,
            trade_date,
            close,
            volume,
            dt.datetime.combine(trade_date, dt.time(22, 0)),
        ],
    )


def _insert_action(
    tmp_store,
    *,
    action_type: str,
    ex_date: dt.date,
    cash_amount: float | None = None,
    split_from: float | None = None,
    split_to: float | None = None,
    adjustment_factor: float | None = None,
) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO corporate_actions (
            source,
            security_id,
            symbol,
            action_type,
            ex_date,
            declaration_date,
            record_date,
            payable_date,
            cash_amount,
            split_from,
            split_to,
            adjustment_factor,
            details_json,
            available_at,
            run_id
        )
        VALUES (
            'test corporate actions',
            ?, 'TEST', ?, ?, NULL, NULL, NULL,
            ?, ?, ?, ?, '{}', ?, 'actions-run'
        )
        """,
        [
            SECURITY_ID,
            action_type,
            ex_date,
            cash_amount,
            split_from,
            split_to,
            adjustment_factor,
            dt.datetime.combine(ex_date, dt.time(22, 0)),
        ],
    )


def _seed_price_and_actions(tmp_store) -> None:
    from db.adjustment_factors import refresh_adjustment_factor_history

    _seed_security(tmp_store)
    _insert_bar(tmp_store, dt.date(2024, 1, 9), 100.0, 1000)
    _insert_bar(tmp_store, dt.date(2024, 1, 10), 50.0, 2100)
    _insert_bar(tmp_store, dt.date(2024, 1, 14), 52.0, 2200)
    _insert_bar(tmp_store, dt.date(2024, 1, 15), 51.5, 2300)
    _insert_bar(tmp_store, dt.date(2024, 1, 16), 52.0, 2400)
    _insert_action(
        tmp_store,
        action_type="stock_split",
        ex_date=dt.date(2024, 1, 10),
        split_from=1.0,
        split_to=2.0,
    )
    _insert_action(
        tmp_store,
        action_type="cash_dividend_inferred",
        ex_date=dt.date(2024, 1, 15),
        cash_amount=1.0,
        adjustment_factor=0.99,
    )
    refresh_adjustment_factor_history(tmp_store)


def test_refresh_daily_adjustment_factors_materializes_split_and_total_return(tmp_store):
    from db.daily_adjustments import DailyAdjustmentFactorOptions, refresh_daily_adjustment_factors

    _seed_price_and_actions(tmp_store)

    rows = refresh_daily_adjustment_factors(
        tmp_store,
        DailyAdjustmentFactorOptions(as_of_date=dt.date(2024, 1, 20)),
    )
    assert rows == 5

    factors = tmp_store.con.execute(
        """
        SELECT
            trade_date,
            split_price_factor,
            split_share_factor,
            dividend_total_return_factor,
            total_return_price_factor,
            raw_close,
            split_adjusted_close,
            total_return_adjusted_close,
            raw_volume,
            split_adjusted_volume,
            split_event_count,
            cash_div_event_count
        FROM daily_adjustment_factors
        ORDER BY trade_date
        """
    ).fetchall()

    assert factors[0] == (
        dt.date(2024, 1, 9),
        0.5,
        2.0,
        0.99,
        0.495,
        100.0,
        50.0,
        49.5,
        1000,
        2000.0,
        1,
        1,
    )
    assert factors[1][0] == dt.date(2024, 1, 10)
    assert factors[1][1] == pytest.approx(1.0)
    assert factors[1][2] == pytest.approx(1.0)
    assert factors[1][3] == pytest.approx(0.99)
    assert factors[3][0] == dt.date(2024, 1, 15)
    assert factors[3][3] == pytest.approx(1.0)
    assert factors[3][7] == pytest.approx(51.5)


def test_daily_adjustment_factors_asof_uses_latest_visible_snapshot(tmp_store):
    from db.asof import daily_adjustment_factors_asof
    from db.daily_adjustments import DailyAdjustmentFactorOptions, refresh_daily_adjustment_factors

    _seed_price_and_actions(tmp_store)
    refresh_daily_adjustment_factors(
        tmp_store,
        DailyAdjustmentFactorOptions(as_of_date=dt.date(2024, 1, 14)),
    )
    refresh_daily_adjustment_factors(
        tmp_store,
        DailyAdjustmentFactorOptions(as_of_date=dt.date(2024, 1, 20)),
    )
    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    before_dividend = daily_adjustment_factors_asof(
        dt.date(2024, 1, 14),
        db_path=db_path,
        symbols=("TEST",),
    )
    after_dividend = daily_adjustment_factors_asof(
        dt.date(2024, 1, 20),
        db_path=db_path,
        symbols=("TEST",),
    )

    before_dates = before_dividend["trade_date"].map(_date_value)
    after_dates = after_dividend["trade_date"].map(_date_value)
    jan9_before = before_dividend.loc[before_dates == dt.date(2024, 1, 9)].iloc[0]
    jan9_after = after_dividend.loc[after_dates == dt.date(2024, 1, 9)].iloc[0]
    assert jan9_before["dividend_total_return_factor"] == pytest.approx(1.0)
    assert jan9_before["split_adjusted_close"] == pytest.approx(50.0)
    assert jan9_before["total_return_adjusted_close"] == pytest.approx(50.0)
    assert jan9_after["dividend_total_return_factor"] == pytest.approx(0.99)
    assert jan9_after["total_return_adjusted_close"] == pytest.approx(49.5)


def test_daily_panel_asof_includes_explicit_adjusted_return_semantics(tmp_store):
    from db.asof import daily_panel_asof
    from db.daily_adjustments import DailyAdjustmentFactorOptions, refresh_daily_adjustment_factors

    _seed_price_and_actions(tmp_store)
    refresh_daily_adjustment_factors(
        tmp_store,
        DailyAdjustmentFactorOptions(as_of_date=dt.date(2024, 1, 20)),
    )
    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    panel = daily_panel_asof(
        dt.date(2024, 1, 20),
        db_path=db_path,
        symbols=("TEST",),
    )

    panel_dates = panel["trade_date"].map(_date_value)
    split_day = panel.loc[panel_dates == dt.date(2024, 1, 10)].iloc[0]
    assert split_day["close"] == pytest.approx(50.0)
    assert split_day["split_adjusted_close"] == pytest.approx(50.0)
    assert split_day["total_return_adjusted_close"] == pytest.approx(49.5)
    assert split_day["simple_return"] == pytest.approx(-0.5)
    assert split_day["split_adjusted_return"] == pytest.approx(0.0)
    assert split_day["total_return_adjusted_return"] == pytest.approx(0.0)


def test_daily_adjustment_dataset_records_run_quality_and_watermarks(tmp_store):
    from db.daily_adjustments import DailyAdjustmentFactorDataset, DailyAdjustmentFactorOptions
    from db.quality import run_warehouse_quality_checks
    from db.watermarks import refresh_warehouse_watermarks

    _seed_price_and_actions(tmp_store)

    result = DailyAdjustmentFactorDataset().run(
        tmp_store,
        DailyAdjustmentFactorOptions(as_of_date=dt.date(2024, 1, 20)),
    )
    assert result.dataset_id == "daily_adjustment_factors"
    assert result.rows_loaded == 5
    assert result.run_id

    target_results = [
        result
        for result in run_warehouse_quality_checks(tmp_store, record=False)
        if result.dataset_id == "daily_adjustment_factors"
    ]
    assert {result.check_name for result in target_results} == {
        "duplicate_daily_adjustment_factors",
        "bad_daily_adjustment_factor_rows",
        "orphan_daily_adjustment_factor_security_ids",
    }
    assert {result.status for result in target_results} == {"passed"}

    refresh_warehouse_watermarks(tmp_store)
    watermarks = tmp_store.con.execute(
        """
        SELECT watermark_name, watermark_value
        FROM dataset_watermarks
        WHERE dataset_id = 'daily_adjustment_factors'
        ORDER BY watermark_name
        """
    ).fetchall()
    assert watermarks == [
        ("max_as_of_date", "2024-01-20"),
        ("max_available_at", "2024-01-20 22:00:00"),
        ("max_trade_date", "2024-01-16"),
    ]
