"""Tier1-S3 T9: wide panel exports and their manifests.

NOTE (T9 substitution): the brief's fixture referenced a shared
``derived_fixture`` builder in ``tests/test_derived_metrics.py`` that does not
exist there (that file only defines a private, non-fixture ``_insert_fact``
helper and a ``seeded`` fixture scoped to its own tests). This file instead
builds its own self-contained fixture, modeled directly on
``tests/test_market_daily.py``'s ``panel`` fixture (same helper shapes,
quarters and trading-day range), so it seeds both the quarterly fundamentals
needed for ``panel_quarterly`` and the daily bars needed for
``panel_daily_market`` without touching ``data/``.
"""

from __future__ import annotations

import datetime as dt
import json

import pyarrow.parquet as pq
import pytest

from atx_db.api.catalog import DATASETS, get_schema, public_schema
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import seed_derived_metric_definitions
from atx_db.market_daily import END_OF_DAY_HOURS, MarketDailyOptions, refresh_market_daily_metrics
from atx_db.panel_export import (
    PANEL_EXPORT_CONTRACT_VERSION,
    export_panel_daily_market,
    export_panel_quarterly,
)

AS_OF = dt.date(2020, 6, 30)

_QUARTERS = (
    dt.date(2019, 3, 31),
    dt.date(2019, 6, 30),
    dt.date(2019, 9, 30),
    dt.date(2019, 12, 31),
)
_FIRST_TRADE = dt.date(2020, 1, 2)


def _fact(store, security_id, code, basis, period_end, value, available_at, revision=1, is_latest=True):
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence,
            is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', ?, ?)
        """,
        [
            f"{security_id}|{code}|{basis}|{period_end}|{revision}",
            security_id,
            code,
            basis,
            period_end,
            value,
            available_at.date(),
            available_at,
            revision,
            is_latest,
        ],
    )


def _bar(store, security_id, trade_date, close, shares=None):
    store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close,
            adjusted_close, volume, split_factor, is_adjusted, available_at,
            as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('test', ?, 'AAA', ?, ?, ?, ?, ?, ?, 1000, 1.0, false, ?, ?, true, ?)
        """,
        [
            security_id,
            trade_date,
            close,
            close,
            close,
            close,
            close,
            dt.datetime.combine(trade_date, dt.time(END_OF_DAY_HOURS, 0)),
            trade_date,
            shares,
        ],
    )


@pytest.fixture
def exported(tmp_store, tmp_path):
    seed_derived_metric_definitions(tmp_store)
    for index, period_end in enumerate(_QUARTERS):
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21, 0))
        _fact(tmp_store, "S1", "revenue", "quarterly", period_end, 100.0 + 10.0 * index, available_at)
        _fact(tmp_store, "S1", "cost_of_revenue_cogs", "quarterly", period_end, 60.0 + 4.0 * index, available_at)
        _fact(tmp_store, "S1", "net_income_to_common", "quarterly", period_end, 10.0 + index, available_at)
        _fact(tmp_store, "S1", "total_assets", "instant", period_end, 1000.0 + 50.0 * index, available_at)
        _fact(tmp_store, "S1", "stockholders_equity", "instant", period_end, 500.0 + 10.0 * index, available_at)
    for offset in range(300):
        trade_date = _FIRST_TRADE + dt.timedelta(days=offset)
        if trade_date.weekday() >= 5:
            continue
        _bar(tmp_store, "S1", trade_date, 20.0 + 0.01 * offset, shares=1_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    return tmp_store, tmp_path


def test_quarterly_panel_writes_parquet_and_manifest(exported):
    store, out_dir = exported
    result = export_panel_quarterly(
        store, AS_OF, items=("revenue", "total_assets"), metrics=("gross_margin",), out_dir=out_dir
    )
    assert result.parquet_path.exists()
    assert result.manifest_path.exists()
    table = pq.read_table(result.parquet_path)
    assert set(table.column_names) >= {
        "security_id",
        "period_end",
        "revenue",
        "total_assets",
        "gross_margin",
        "panel_available_at",
    }
    assert table.num_rows == result.row_count


def test_quarterly_manifest_has_the_documented_keys(exported):
    store, out_dir = exported
    result = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=("gross_margin",), out_dir=out_dir)
    manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))
    assert set(manifest) == {
        "panel",
        "contract_version",
        "as_of",
        "inputs",
        "query_sha256",
        "schema_sha256",
        "row_count",
        "column_count",
        "parquet_path",
        "parquet_sha256",
    }
    assert manifest["panel"] == "panel_quarterly"
    assert manifest["contract_version"] == PANEL_EXPORT_CONTRACT_VERSION
    assert manifest["as_of"] == AS_OF.isoformat()
    assert manifest["inputs"]["items"] == ["revenue"]
    assert manifest["inputs"]["metrics"] == ["gross_margin"]
    assert len(manifest["query_sha256"]) == 64
    assert len(manifest["schema_sha256"]) == 64
    assert len(manifest["parquet_sha256"]) == 64


def test_repeated_export_is_byte_identical(exported):
    store, out_dir = exported
    first = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    first_bytes = first.parquet_path.read_bytes()
    first_manifest = json.loads(first.manifest_path.read_text(encoding="utf-8"))
    second = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    second_manifest = json.loads(second.manifest_path.read_text(encoding="utf-8"))
    assert second.parquet_path.read_bytes() == first_bytes
    assert second_manifest == first_manifest


def test_query_hash_changes_with_the_requested_columns(exported):
    store, out_dir = exported
    one = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    two = export_panel_quarterly(store, AS_OF, items=("revenue", "total_assets"), metrics=(), out_dir=out_dir)
    assert one.query_sha256 != two.query_sha256
    # Different selections at the same as_of must not clobber each other's files
    # on disk (Minor #3, task-9-review.md): each filename carries a short hash
    # of the query text, so both exports' Parquet/manifest files coexist.
    assert one.parquet_path != two.parquet_path
    assert one.manifest_path != two.manifest_path
    assert one.parquet_path.exists()
    assert two.parquet_path.exists()
    assert one.manifest_path.exists()
    assert two.manifest_path.exists()


def test_quarterly_panel_uses_the_latest_revision_visible_at_as_of(exported):
    # Important #2 (task-9-review.md): fundamental_standardized keeps
    # superseded revisions as separate rows, and is_latest_revision is a
    # today-relative flag (revision_sequence == revision_count), not an
    # as_of-relative one. A restatement filed after the requested as_of must
    # not blank out the value that was actually visible on that date -- the
    # export has to select by available_at <= as_of and take the latest
    # revision among those, not filter on is_latest_revision at all.
    store, out_dir = exported
    period_end = _QUARTERS[0]
    revision_1_available = dt.datetime.combine(period_end + dt.timedelta(days=10), dt.time(21, 0))
    revision_2_available = dt.datetime.combine(period_end + dt.timedelta(days=60), dt.time(21, 0))
    _fact(store, "S2", "revenue", "quarterly", period_end, 111.0, revision_1_available, revision=1, is_latest=False)
    _fact(store, "S2", "revenue", "quarterly", period_end, 222.0, revision_2_available, revision=2, is_latest=True)

    between = export_panel_quarterly(
        store, period_end + dt.timedelta(days=30), items=("revenue",), metrics=(), out_dir=out_dir
    )
    row_between = pq.read_table(between.parquet_path).to_pandas().set_index("security_id").loc["S2"]
    assert row_between["revenue"] == pytest.approx(111.0)

    after = export_panel_quarterly(
        store, period_end + dt.timedelta(days=90), items=("revenue",), metrics=(), out_dir=out_dir
    )
    row_after = pq.read_table(after.parquet_path).to_pandas().set_index("security_id").loc["S2"]
    assert row_after["revenue"] == pytest.approx(222.0)


def test_quarterly_panel_never_exposes_a_row_available_after_the_as_of(exported):
    store, out_dir = exported
    result = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    table = pq.read_table(result.parquet_path).to_pandas()
    if not table.empty:
        assert table["panel_available_at"].max() <= dt.datetime.combine(AS_OF, dt.time(23, 59, 59))


def test_daily_market_panel_exports_the_requested_metrics(exported):
    store, out_dir = exported
    result = export_panel_daily_market(
        store, AS_OF, metrics=("market_cap", "pe_ttm", "total_return_1m"), out_dir=out_dir
    )
    table = pq.read_table(result.parquet_path)
    assert set(table.column_names) >= {
        "security_id",
        "trade_date",
        "market_cap",
        "pe_ttm",
        "total_return_1m",
        "available_at",
    }
    manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))
    assert manifest["panel"] == "panel_daily_market"
    assert manifest["inputs"]["metrics"] == ["market_cap", "pe_ttm", "total_return_1m"]


def test_daily_market_panel_rejects_an_unknown_metric(exported):
    store, out_dir = exported
    with pytest.raises(ValueError) as excinfo:
        export_panel_daily_market(store, AS_OF, metrics=("not_a_metric",), out_dir=out_dir)
    assert "not_a_metric" in str(excinfo.value)


def test_quarterly_panel_rejects_an_unknown_item(exported):
    store, out_dir = exported
    with pytest.raises(ValueError):
        export_panel_quarterly(store, AS_OF, items=("nope",), metrics=(), out_dir=out_dir)


def test_derived_metrics_schema_is_registered():
    schema = get_schema("ATX.US.FUNDAMENTALS", "derived-metrics")
    assert schema.source_table == "derived_metric_values"
    assert schema.time_column == "period_end"
    assert schema.item_column == "metric_code"
    assert schema.natural_key == ("revision_group_id",)
    assert schema.version == "2.1.0"
    assert schema.field("value").nullable
    assert "inputs_hash" in schema.field_names
    payload = public_schema(schema)
    assert len(payload["schema_sha256"]) == 64


def test_market_daily_schema_is_registered():
    schema = get_schema("ATX.US.EQUITIES", "market-daily-1d")
    assert schema.source_table == "market_daily_metrics"
    assert schema.time_column == "trade_date"
    assert schema.natural_key == ("security_id", "trade_date")
    for name in ("market_cap", "enterprise_value", "pe_ttm", "shares_source", "momentum_12_1"):
        assert name in schema.field_names


def test_catalog_still_exposes_both_datasets():
    codes = {dataset.code for dataset in DATASETS}
    assert codes == {"ATX.US.FUNDAMENTALS", "ATX.US.EQUITIES"}
