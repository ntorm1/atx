"""Tier1-S3 T1: migration 0302 creates the derived-engine tables."""

from __future__ import annotations

import pytest

from atx_db.migrations.registry import MIGRATIONS

_DAILY_METRIC_COLUMNS = (
    "market_cap",
    "enterprise_value",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "ev_ebitda",
    "ev_sales",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "net_payout_yield",
    "total_payout_yield",
    "buyback_yield",
    "book_to_market",
    "rd_to_market_equity",
    "gross_profit_to_ev",
    "cfo_to_ev",
    "ebit_to_ev",
    "sales_to_ev",
    "altman_z",
    "total_return_1m",
    "total_return_3m",
    "total_return_6m",
    "total_return_12m",
    "momentum_12_1",
    "realized_vol_60d",
    "realized_vol_252d",
    "dollar_volume_20d",
)


def _columns(store, table: str) -> list[str]:
    rows = store.con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main' AND table_name = ?
        ORDER BY column_index
        """,
        [table],
    ).fetchall()
    return [str(row[0]) for row in rows]


def test_migration_302_is_registered_exactly_once():
    versions = [migration.version for migration in MIGRATIONS]
    assert versions.count(302) == 1
    assert max(versions) >= 302  # later sprints append 0303+


def test_derived_metric_definitions_shape(tmp_store):
    assert _columns(tmp_store, "derived_metric_definitions") == [
        "metric_code",
        "family",
        "expression",
        "metric_window",
        "inputs_json",
        "requires_market",
        "description",
        "version",
        "topological_rank",
        "seeded_at",
    ]


def test_derived_metric_values_shape(tmp_store):
    assert _columns(tmp_store, "derived_metric_values") == [
        "derived_value_id",
        "source",
        "security_id",
        "metric_code",
        "metric_window",
        "period_end",
        "value",
        "available_at",
        "inputs_hash",
        "as_of_date",
        "is_latest_revision",
        "run_id",
        "source_loaded_at",
    ]


def test_market_daily_metrics_carries_every_published_daily_metric(tmp_store):
    columns = set(_columns(tmp_store, "market_daily_metrics"))
    assert sorted(set(_DAILY_METRIC_COLUMNS) - columns) == []
    for column in (
        "security_id",
        "trade_date",
        "close",
        "adj_close",
        "volume",
        "shares_outstanding",
        "shares_source",
        "shares_reconciliation_ratio",
        "fundamental_available_at",
        "available_at",
        "inputs_hash",
    ):
        assert column in columns


def test_new_tables_are_catalogued(tmp_store):
    rows = tmp_store.con.execute(
        """
        SELECT table_name FROM table_catalog
        WHERE table_name IN (
            'derived_metric_definitions','derived_metric_values','market_daily_metrics'
        )
        ORDER BY table_name
        """
    ).fetchall()
    assert [str(row[0]) for row in rows] == [
        "derived_metric_definitions",
        "derived_metric_values",
        "market_daily_metrics",
    ]


def test_lake_partition_specs_registered(tmp_store):
    rows = tmp_store.con.execute(
        """
        SELECT object_name, watermark_column FROM lake_partition_specs
        WHERE object_name IN ('derived_metric_values','market_daily_metrics')
        ORDER BY object_name
        """
    ).fetchall()
    assert [(str(a), str(b)) for a, b in rows] == [
        ("derived_metric_values", "available_at"),
        ("market_daily_metrics", "available_at"),
    ]


@pytest.mark.parametrize("table", ["derived_metric_values", "market_daily_metrics"])
def test_tables_are_empty_after_bootstrap(tmp_store, table):
    assert tmp_store.con.execute(f"SELECT count(*) FROM {table}").fetchone()[0] == 0
