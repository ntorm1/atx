from __future__ import annotations

import datetime as dt

import pytest


SECURITY_ID = "TICKER-AAPL"


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
        [SECURITY_ID, SECURITY_ID, "AAPL", "Apple Inc.", "test"],
    )


def _insert_corporate_action(
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
            ?, 'AAPL', ?, ?, NULL, NULL, NULL,
            ?, ?, ?, ?, '{}', ?, 'seed-run'
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


def _seed_actions(tmp_store) -> None:
    _seed_security(tmp_store)
    _insert_corporate_action(
        tmp_store,
        action_type="cash_dividend_inferred",
        ex_date=dt.date(2024, 1, 15),
        cash_amount=0.25,
        adjustment_factor=0.99,
    )
    _insert_corporate_action(
        tmp_store,
        action_type="cash_dividend_inferred",
        ex_date=dt.date(2024, 2, 15),
        cash_amount=0.30,
        adjustment_factor=0.98,
    )
    _insert_corporate_action(
        tmp_store,
        action_type="stock_split",
        ex_date=dt.date(2024, 3, 15),
        split_from=1.0,
        split_to=2.0,
    )


def test_refresh_adjustment_factor_history_materializes_event_factors(tmp_store):
    from atx_db.adjustment_factors import refresh_adjustment_factor_history

    _seed_actions(tmp_store)

    assert refresh_adjustment_factor_history(tmp_store) == 3
    assert refresh_adjustment_factor_history(tmp_store) == 3

    type_rows = tmp_store.con.execute("SELECT count(*) FROM corp_action_type_dim").fetchone()[0]
    assert type_rows >= 5

    rows = tmp_store.con.execute(
        """
        SELECT
            event_type,
            type_code,
            ex_date,
            factor_price,
            factor_shares,
            factor_volume,
            cash_div_amount,
            cumulative_price_factor,
            cumulative_share_factor
        FROM adjustment_factor_history
        ORDER BY ex_date
        """
    ).fetchall()

    assert rows[0][:7] == (
        "CASH_DIV",
        120000,
        dt.date(2024, 1, 15),
        0.99,
        1.0,
        1.0,
        0.25,
    )
    assert rows[1][:7] == (
        "CASH_DIV",
        120000,
        dt.date(2024, 2, 15),
        0.98,
        1.0,
        1.0,
        0.30,
    )
    assert rows[2][:7] == (
        "SPLIT",
        500000,
        dt.date(2024, 3, 15),
        0.5,
        2.0,
        2.0,
        None,
    )
    assert rows[0][7] == pytest.approx(0.99)
    assert rows[1][7] == pytest.approx(0.9702)
    assert rows[2][7] == pytest.approx(0.4851)
    assert rows[0][8] == pytest.approx(1.0)
    assert rows[1][8] == pytest.approx(1.0)
    assert rows[2][8] == pytest.approx(2.0)


def test_adjustment_factor_dataset_records_run(tmp_store):
    from atx_db.adjustment_factors import AdjustmentFactorHistoryDataset, AdjustmentFactorHistoryOptions

    _seed_actions(tmp_store)

    result = AdjustmentFactorHistoryDataset().run(
        tmp_store,
        AdjustmentFactorHistoryOptions(source="test factor source"),
    )

    assert result.dataset_id == "adjustment_factor_history"
    assert result.rows_loaded == 3
    assert result.run_id
    assert (
        tmp_store.con.execute(
            "SELECT status FROM dataset_runs WHERE run_id = ?",
            [result.run_id],
        ).fetchone()[0]
        == "succeeded"
    )


def test_split_like_inferred_cash_artifact_promotes_to_split(tmp_store):
    from atx_db.adjustment_factors import refresh_adjustment_factor_history

    _seed_security(tmp_store)
    _insert_corporate_action(
        tmp_store,
        action_type="cash_dividend_inferred",
        ex_date=dt.date(2024, 8, 13),
        cash_amount=39.41,
        adjustment_factor=0.5,
    )

    assert refresh_adjustment_factor_history(tmp_store) == 1

    row = tmp_store.con.execute(
        """
        SELECT
            event_type,
            type_code,
            classification_reason,
            factor_price,
            factor_shares,
            ratio_numerator,
            ratio_denominator,
            cash_div_amount
        FROM adjustment_factor_history
        """
    ).fetchone()

    assert row == (
        "SPLIT",
        500000,
        "split_like_inferred_cash_artifact",
        0.5,
        2.0,
        2.0,
        1.0,
        None,
    )


def test_split_like_non_inferred_cash_artifact_quarantines_as_other(tmp_store):
    from atx_db.adjustment_factors import refresh_adjustment_factor_history

    _seed_security(tmp_store)
    _insert_corporate_action(
        tmp_store,
        action_type="cash_dividend",
        ex_date=dt.date(2024, 8, 13),
        cash_amount=39.41,
        adjustment_factor=0.5,
    )

    assert refresh_adjustment_factor_history(tmp_store) == 1

    row = tmp_store.con.execute(
        """
        SELECT
            event_type,
            type_code,
            classification_reason,
            factor_price,
            factor_shares,
            ratio_numerator,
            ratio_denominator,
            cash_div_amount
        FROM adjustment_factor_history
        """
    ).fetchone()

    assert row == (
        "OTHER",
        900000,
        "split_like_cash_artifact_quarantined",
        0.5,
        1.0,
        None,
        None,
        None,
    )


def test_adjustment_factors_asof_filters_by_availability_and_event_type(tmp_store):
    from atx_db.adjustment_factors import refresh_adjustment_factor_history
    from atx_db.asof import adjustment_factors_asof

    _seed_actions(tmp_store)
    refresh_adjustment_factor_history(tmp_store)
    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    before_available = adjustment_factors_asof(
        dt.date(2024, 1, 15),
        as_of_ts=dt.datetime(2024, 1, 15, 21, 59),
        db_path=db_path,
        symbols=("AAPL",),
    )
    cash_visible = adjustment_factors_asof(
        dt.date(2024, 2, 20),
        db_path=db_path,
        symbols=("AAPL",),
        event_types=("CASH_DIV",),
    )

    assert before_available.empty
    assert cash_visible["event_type"].tolist() == ["CASH_DIV", "CASH_DIV"]
    assert [value.date() for value in cash_visible["ex_date"].tolist()] == [
        dt.date(2024, 1, 15),
        dt.date(2024, 2, 15),
    ]


def test_adjustment_factors_asof_exposes_promoted_split_artifact(tmp_store):
    from atx_db.adjustment_factors import refresh_adjustment_factor_history
    from atx_db.asof import adjustment_factors_asof

    _seed_security(tmp_store)
    _insert_corporate_action(
        tmp_store,
        action_type="cash_dividend_inferred",
        ex_date=dt.date(2024, 8, 13),
        cash_amount=39.41,
        adjustment_factor=0.5,
    )
    refresh_adjustment_factor_history(tmp_store)
    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    split_visible = adjustment_factors_asof(
        dt.date(2024, 8, 14),
        db_path=db_path,
        symbols=("AAPL",),
        event_types=("SPLIT",),
    )
    cash_visible = adjustment_factors_asof(
        dt.date(2024, 8, 14),
        db_path=db_path,
        symbols=("AAPL",),
        event_types=("CASH_DIV",),
    )

    assert cash_visible.empty
    assert split_visible["event_type"].tolist() == ["SPLIT"]
    assert split_visible["classification_reason"].tolist() == [
        "split_like_inferred_cash_artifact"
    ]
    assert split_visible["factor_price"].tolist() == [0.5]
    assert split_visible["factor_shares"].tolist() == [2.0]


def test_adjustment_factor_quality_checks_and_watermarks_pass_clean_sample(tmp_store):
    from atx_db.adjustment_factors import refresh_adjustment_factor_history
    from atx_db.quality import run_warehouse_quality_checks
    from atx_db.watermarks import refresh_warehouse_watermarks

    _seed_actions(tmp_store)
    refresh_adjustment_factor_history(tmp_store)
    refresh_warehouse_watermarks(tmp_store)

    target_results = [
        result
        for result in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            dataset_ids=("adjustment_factor_history",),
        )
        if result.dataset_id == "adjustment_factor_history"
    ]
    assert {result.check_name for result in target_results} == {
        "bad_corp_action_type_dim_rows",
        "duplicate_adjustment_factor_history",
        "bad_adjustment_factor_history_rows",
        "orphan_adjustment_factor_security_ids",
    }
    assert {result.status for result in target_results} == {"passed"}

    watermarks = tmp_store.con.execute(
        """
        SELECT dataset_id, watermark_name
        FROM dataset_watermarks
        WHERE dataset_id IN ('corp_action_type_dim', 'adjustment_factor_history')
        ORDER BY dataset_id, watermark_name
        """
    ).fetchall()
    assert watermarks == [
        ("adjustment_factor_history", "max_available_at"),
        ("adjustment_factor_history", "max_ex_date"),
        ("corp_action_type_dim", "max_updated_at"),
    ]
