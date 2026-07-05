from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from db.enterprise_value import (
    EnterpriseValueDataset,
    EnterpriseValueOptions,
    compute_enterprise_value_rows,
    refresh_enterprise_value,
)
from db.warehouse import insert_frame


SECURITY_ID = "SEC-EV"


def _market_cap_row(
    *,
    security_id: str = SECURITY_ID,
    symbol: str = "EVT",
    trade_date: dt.date = dt.date(2020, 1, 2),
    available_at: dt.datetime = dt.datetime(2020, 1, 2, 22),
    market_cap: float = 1000.0,
) -> dict[str, object]:
    return {
        "market_cap_id": f"mc-{security_id}-{trade_date}",
        "source": "derived_market_cap_v1",
        "price_source": "fixture_prices",
        "share_source": "fixture_shares",
        "security_id": security_id,
        "symbol": symbol,
        "trade_date": trade_date,
        "close": 10.0,
        "share_count": market_cap / 10.0,
        "share_count_type_used": "shares_diluted_avg",
        "market_cap": market_cap,
        "is_latest_revision": True,
        "as_of_date": trade_date,
        "available_at": available_at,
        "price_available_at": available_at,
        "share_available_at": available_at,
        "price_run_id": "price-run",
        "share_run_id": "share-run",
        "share_history_id": f"share-{security_id}",
        "input_codes_json": '{"market_cap": "fixture"}',
        "input_lineage_json": '{"market_cap": "fixture"}',
        "run_id": "market-run",
    }


def _statement_row(
    metric: str,
    value: float,
    *,
    security_id: str = SECURITY_ID,
    symbol: str = "EVT",
    period_end: dt.date = dt.date(2019, 12, 31),
    available_at: dt.datetime = dt.datetime(2020, 2, 1, 10),
) -> dict[str, object]:
    return {
        "statement_point_id": f"stmt-{security_id}-{metric}",
        "fact_revision_id": f"fact-{security_id}-{metric}",
        "revision_group_id": f"rg-{security_id}-{metric}",
        "source": "fixture_statement",
        "security_id": security_id,
        "symbol": symbol,
        "cik": "0000000001",
        "statement_type": "balance_sheet",
        "statement_section": "valuation",
        "canonical_metric": metric,
        "canonical_label": metric.replace("_", " ").title(),
        "taxonomy": "us-gaap",
        "concept": metric,
        "unit": "USD",
        "unit_type": "monetary",
        "period_type": "instant",
        "normal_balance": "credit",
        "period_start": None,
        "period_end": period_end,
        "as_of_date": period_end,
        "available_at": available_at,
        "fiscal_year": period_end.year,
        "fiscal_period": "FY",
        "form": "10-K",
        "accession_number": f"acc-{security_id}-{period_end:%Y%m%d}",
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "is_value_changed": False,
        "raw_value": value,
        "value": value,
        "previous_raw_value": None,
        "previous_value": None,
        "value_delta": None,
        "value_delta_percent": None,
        "run_id": "statement-run",
        "source_url": "fixture",
        "source_loaded_at": available_at,
    }


def _seed_enterprise_value_inputs(tmp_store, *, cash_available_at: dt.datetime | None = None) -> None:
    cash_av = cash_available_at or dt.datetime(2020, 2, 5, 10)
    insert_frame(
        tmp_store,
        pd.DataFrame([_market_cap_row()]),
        "market_cap",
        "enterprise_value_market_cap_seed",
    )
    insert_frame(
        tmp_store,
        pd.DataFrame(
            [
                _statement_row("total_debt", 200.0, available_at=dt.datetime(2020, 2, 1, 10)),
                _statement_row("pref_stock", 25.0, available_at=dt.datetime(2020, 2, 2, 10)),
                _statement_row("minority_int_bs", 10.0, available_at=dt.datetime(2020, 2, 3, 10)),
                _statement_row("cash_st_inv", 50.0, available_at=cash_av),
            ]
        ),
        "fundamental_statement_points",
        "enterprise_value_statement_seed",
    )


def _wide_row(**overrides) -> dict[str, object]:
    row = {
        "market_cap_source": "derived_market_cap_v1",
        "market_cap_id": "mc-ev",
        "security_id": SECURITY_ID,
        "symbol": "EVT",
        "trade_date": dt.date(2020, 1, 2),
        "period_end": dt.date(2019, 12, 31),
        "fiscal_year": 2019,
        "fiscal_period": "FY",
        "price": 10.0,
        "share_count": 100.0,
        "share_count_type_used": "shares_diluted_avg",
        "market_cap": 1000.0,
        "market_cap_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "price_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "share_available_at": pd.Timestamp("2020-01-02 22:00:00"),
        "market_cap_input_lineage_json": '{"market_cap": "fixture"}',
        "total_debt": 200.0,
        "total_debt_available_at": pd.Timestamp("2020-02-01 10:00:00"),
        "total_debt_id": "stmt-debt",
        "total_debt_source": "fixture_statement",
        "preferred_equity": 25.0,
        "preferred_equity_available_at": pd.Timestamp("2020-02-02 10:00:00"),
        "preferred_equity_id": "stmt-pref",
        "preferred_equity_source": "fixture_statement",
        "minority_interest": 10.0,
        "minority_interest_available_at": pd.Timestamp("2020-02-03 10:00:00"),
        "minority_interest_id": "stmt-minority",
        "minority_interest_source": "fixture_statement",
        "cash_and_equivalents": 50.0,
        "cash_and_equivalents_available_at": pd.Timestamp("2020-02-05 10:00:00"),
        "cash_and_equivalents_id": "stmt-cash",
        "cash_and_equivalents_source": "fixture_statement",
    }
    row.update(overrides)
    return row


def test_compute_enterprise_value_rows_returns_hand_computed_ev_and_lineage() -> None:
    rows = compute_enterprise_value_rows(pd.DataFrame([_wide_row()]), run_id="ev-run")

    assert len(rows) == 1
    row = rows.iloc[0]
    assert row["enterprise_value"] == pytest.approx(1185.0)
    assert row["available_at"] == pd.Timestamp("2020-02-05 10:00:00")
    assert row["run_id"] == "ev-run"
    lineage = json.loads(row["input_lineage_json"])
    assert lineage["components"]["cash_and_equivalents"]["sign"] == "subtract"
    assert lineage["components"]["total_debt"]["sign"] == "add"


def test_compute_enterprise_value_rejects_negative_cash_component() -> None:
    with pytest.raises(ValueError, match="cash_and_equivalents"):
        compute_enterprise_value_rows(pd.DataFrame([_wide_row(cash_and_equivalents=-50.0)]))


def test_refresh_enterprise_value_is_idempotent_and_asof_visible(tmp_store) -> None:
    from db.asof import enterprise_value_asof

    _seed_enterprise_value_inputs(tmp_store)

    assert refresh_enterprise_value(tmp_store, EnterpriseValueOptions(run_id="run-1")) == 1
    assert refresh_enterprise_value(tmp_store, EnterpriseValueOptions(run_id="run-2")) == 1

    stored = tmp_store.con.execute(
        """
        SELECT symbol, enterprise_value, available_at, run_id
        FROM enterprise_value
        """
    ).fetchall()
    assert stored == [("EVT", 1185.0, dt.datetime(2020, 2, 5, 10), "run-2")]

    early = enterprise_value_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 2, 4, 23),
        store=tmp_store,
        symbols=("EVT",),
    )
    late = enterprise_value_asof(
        dt.date(2020, 1, 2),
        as_of_ts=dt.datetime(2020, 2, 5, 10),
        store=tmp_store,
        symbols=("EVT",),
    )
    assert early.empty
    assert late[["symbol", "enterprise_value"]].to_dict("records") == [
        {"symbol": "EVT", "enterprise_value": 1185.0}
    ]


def test_enterprise_value_dataset_records_quality(tmp_store) -> None:
    _seed_enterprise_value_inputs(tmp_store)

    result = EnterpriseValueDataset().load(tmp_store, EnterpriseValueOptions(run_id="dataset-run"))

    assert result.rows_loaded == 1
    assert tmp_store.con.execute(
        """
        SELECT status, observed_value
        FROM data_quality_checks
        WHERE dataset_id = 'enterprise_value'
          AND check_name = 'rows_materialized'
        """
    ).fetchall() == [("passed", 1.0)]


def test_enterprise_value_migration_catalog_and_view_are_present(tmp_store) -> None:
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'enterprise_value'
            """
        ).fetchall()
    }
    assert {
        "enterprise_value_id",
        "market_cap",
        "total_debt",
        "preferred_equity",
        "minority_interest",
        "cash_and_equivalents",
        "enterprise_value",
        "input_lineage_json",
    }.issubset(columns)
    assert (
        tmp_store.con.execute(
            "SELECT description FROM schema_migrations WHERE version = 145"
        ).fetchone()[0]
        == "pf3_s5_enterprise_value_schema_catalog"
    )
    assert (
        tmp_store.con.execute(
            """
            SELECT count(*)
            FROM information_schema.tables
            WHERE table_name = 'v_enterprise_value_latest'
              AND table_type = 'VIEW'
            """
        ).fetchone()[0]
        == 1
    )
    assert (
        tmp_store.con.execute(
            """
            SELECT count(*)
            FROM field_catalog
            WHERE table_name = 'enterprise_value'
              AND field_name IN (
                'market_cap',
                'total_debt',
                'preferred_equity',
                'minority_interest',
                'cash_and_equivalents',
                'enterprise_value',
                'input_lineage_json'
              )
            """
        ).fetchone()[0]
        == 7
    )
