"""PF2-S4: point-in-time month snapshot reconstruction.

This module reconstructs the latest visible fundamental fact at an end-of-month
knowledge boundary and tags first-reported versus restated vintages. The core
``compute_pit_snapshot_rows`` function is a pure DataFrame transform.
"""
from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pandas as pd

from .connection import DEFAULT_DB_PATH, DuckDBStore, connect
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check


DEFAULT_SOURCE = "fundamental_pit_snapshot_v1"
SOURCE_NAME = "Fundamental PIT month snapshots"

PIT_SNAPSHOT_COLUMNS = [
    "snapshot_id",
    "source",
    "snapshot_month",
    "upstream_source",
    "security_id",
    "symbol",
    "cik",
    "canonical_metric",
    "item_id",
    "basis",
    "period_start",
    "period_end",
    "value",
    "unit",
    "unit_type",
    "vintage_class",
    "source_accession",
    "filed_date",
    "as_of_date",
    "available_at",
    "is_latest_revision",
    "input_codes_json",
    "run_id",
]


@dataclass(frozen=True)
class PitSnapshotOptions:
    snapshot_month: dt.date
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def month_end(value: dt.date) -> dt.date:
    if value.month == 12:
        return dt.date(value.year, 12, 31)
    return dt.date(value.year, value.month + 1, 1) - dt.timedelta(days=1)


def month_end_ts(value: dt.date) -> dt.datetime:
    end = month_end(value)
    return dt.datetime.combine(end, dt.time(23, 59, 59, 999999))


def _present(value: Any) -> bool:
    try:
        return not pd.isna(value)
    except (TypeError, ValueError):
        return value is not None


def _stable_id(*parts: Any) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _as_date(value: Any) -> dt.date | None:
    if not _present(value):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _classify_vintages(frame: pd.DataFrame) -> pd.DataFrame:
    out = frame.copy()
    out["__filed_sort"] = pd.to_datetime(out["filed_date"], errors="coerce")
    out["__available_sort"] = pd.to_datetime(out["available_at"], errors="coerce")
    group_cols = ["security_id", "canonical_metric", "basis", "period_end"]
    out = out.sort_values(group_cols + ["__filed_sort", "__available_sort", "source_accession"])
    out["__first_rank"] = out.groupby(group_cols, dropna=False).cumcount()
    out = out.sort_values(
        group_cols + ["__filed_sort", "__available_sort", "source_accession"],
        ascending=[True, True, True, True, False, False, False],
    )
    out["__latest_rank"] = out.groupby(group_cols, dropna=False).cumcount()
    out["vintage_class"] = "intermediate_restatement"
    out.loc[out["__first_rank"] == 0, "vintage_class"] = "as_first_reported"
    out.loc[out["__latest_rank"] == 0, "vintage_class"] = "most_recently_restated"
    # If there is only one row, it is both concepts; the latest flag preserves the
    # most-recent query path while the explicit class stays first-reported.
    single = out.groupby(group_cols, dropna=False)["security_id"].transform("size") == 1
    out.loc[single, "vintage_class"] = "as_first_reported"
    out["is_latest_revision"] = out["__latest_rank"] == 0
    return out.drop(columns=["__filed_sort", "__available_sort", "__first_rank", "__latest_rank"])


def compute_pit_snapshot_rows(
    inputs: pd.DataFrame,
    *,
    snapshot_month: dt.date,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: bitemporal fact rows -> end-of-month visible snapshot rows."""

    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=PIT_SNAPSHOT_COLUMNS)

    cutoff = pd.Timestamp(month_end_ts(snapshot_month))
    visible = inputs.copy()
    visible["available_at"] = pd.to_datetime(visible["available_at"], errors="coerce")
    visible = visible[visible["available_at"].notna() & (visible["available_at"] <= cutoff)].copy()
    visible["__period_end_sort"] = pd.to_datetime(visible["period_end"], errors="coerce")
    visible = visible[
        visible["__period_end_sort"].notna()
        & (visible["__period_end_sort"] <= pd.Timestamp(month_end(snapshot_month)))
    ].copy()
    if visible.empty:
        return pd.DataFrame(columns=PIT_SNAPSHOT_COLUMNS)

    visible["filed_date"] = pd.to_datetime(visible["filed_date"], errors="coerce").dt.date
    classified = _classify_vintages(visible.drop(columns=["__period_end_sort"]))
    sort_cols = [
        "security_id",
        "canonical_metric",
        "basis",
        "period_end",
        "available_at",
        "filed_date",
        "source_accession",
    ]
    latest_visible = (
        classified.sort_values(sort_cols)
        .drop_duplicates(["security_id", "canonical_metric", "basis", "period_end"], keep="last")
        .copy()
    )
    records: list[dict[str, Any]] = []
    snap_month = month_end(snapshot_month)
    for row in latest_visible.to_dict("records"):
        period_end = _as_date(row.get("period_end"))
        records.append(
            {
                "snapshot_id": _stable_id(
                    source,
                    snap_month,
                    row.get("security_id"),
                    row.get("canonical_metric"),
                    row.get("basis"),
                    period_end,
                ),
                "source": source,
                "snapshot_month": snap_month,
                "upstream_source": row.get("upstream_source"),
                "security_id": row.get("security_id"),
                "symbol": row.get("symbol"),
                "cik": row.get("cik"),
                "canonical_metric": row.get("canonical_metric"),
                "item_id": row.get("item_id"),
                "basis": row.get("basis"),
                "period_start": _as_date(row.get("period_start")),
                "period_end": period_end,
                "value": row.get("value"),
                "unit": row.get("unit"),
                "unit_type": row.get("unit_type"),
                "vintage_class": row.get("vintage_class"),
                "source_accession": row.get("source_accession"),
                "filed_date": row.get("filed_date"),
                "as_of_date": period_end,
                "available_at": row.get("available_at"),
                "is_latest_revision": bool(row.get("is_latest_revision")),
                "input_codes_json": json_dumps([row.get("canonical_metric")]),
                "run_id": run_id,
            }
        )
    return pd.DataFrame(records, columns=PIT_SNAPSHOT_COLUMNS)


def load_pit_snapshot_inputs(store: DuckDBStore, options: PitSnapshotOptions) -> pd.DataFrame:
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    sym_join_s = ""
    sym_join_t = ""
    if symbols:
        store.con.register(
            "pit_snapshot_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        sym_join_s = "JOIN pit_snapshot_symbol_filter sf ON sf.symbol = s.symbol"
        sym_join_t = "JOIN pit_snapshot_symbol_filter sf ON sf.symbol = t.symbol"
    sql = f"""
        SELECT
            'fundamental_statement_points' AS upstream_source,
            s.security_id,
            s.symbol,
            s.cik,
            s.canonical_metric,
            s.item_id,
            CASE WHEN s.period_type = 'instant' THEN 'instant' ELSE 'annual' END AS basis,
            s.period_start,
            s.period_end,
            s.value,
            s.unit,
            s.unit_type,
            coalesce(s.source_accession, s.accession_number) AS source_accession,
            coalesce(s.filed_date, s.as_of_date) AS filed_date,
            s.available_at
        FROM fundamental_statement_points s
        {sym_join_s}
        WHERE s.value IS NOT NULL AND s.available_at IS NOT NULL
        UNION ALL
        SELECT
            'fundamental_ttm_points' AS upstream_source,
            t.security_id,
            t.symbol,
            t.cik,
            t.canonical_metric,
            m.item_id,
            'ttm' AS basis,
            t.ttm_start_date AS period_start,
            t.ttm_end_date AS period_end,
            t.ttm_value AS value,
            t.unit,
            t.unit_type,
            t.accession_number AS source_accession,
            t.as_of_date AS filed_date,
            t.available_at
        FROM fundamental_ttm_points t
        LEFT JOIN (
            SELECT canonical_metric, min(item_id) AS item_id
            FROM fundamental_statement_map
            WHERE item_id IS NOT NULL
            GROUP BY canonical_metric
        ) m ON m.canonical_metric = t.canonical_metric
        {sym_join_t}
        WHERE t.ttm_value IS NOT NULL AND t.available_at IS NOT NULL
    """
    try:
        return store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("pit_snapshot_symbol_filter")


def refresh_pit_snapshot(
    store: DuckDBStore,
    options: PitSnapshotOptions,
) -> int:
    store.initialize()
    inputs = load_pit_snapshot_inputs(store, options)
    rows = compute_pit_snapshot_rows(
        inputs,
        snapshot_month=options.snapshot_month,
        source=options.source,
        run_id=options.run_id,
    )
    snap_month = month_end(options.snapshot_month)
    with store.transaction():
        store.con.execute(
            "DELETE FROM fundamental_pit_snapshot WHERE source = ? AND snapshot_month = ?",
            [options.source, snap_month],
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_pit_snapshot", "fundamental_pit_snapshot_insert")
    return int(len(rows))


class PitSnapshotDataset(Dataset):
    dataset_id = "fundamental_pit_snapshot"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: PitSnapshotOptions) -> DatasetLoadResult:
        rows = refresh_pit_snapshot(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_pit_snapshot",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"snapshot_month": month_end(options.snapshot_month).isoformat()},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"snapshot_month": month_end(options.snapshot_month).isoformat()},
        )


PIT_SNAPSHOT_ASOF_SQL = """
WITH params AS (
    SELECT CAST(? AS DATE) AS snapshot_month
)
SELECT s.*
FROM fundamental_pit_snapshot s
{symbol_join}
{metric_join}
CROSS JOIN params p
WHERE s.snapshot_month = p.snapshot_month
ORDER BY s.symbol, s.canonical_metric, s.basis, s.period_end
"""


def pit_snapshot_asof(
    snapshot_month: dt.date,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: DuckDBStore | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    from .asof import _normalize_strings, _normalize_symbols, _register_filter

    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(metrics)
    snap_month = month_end(snapshot_month)

    def _run(active):
        registered: list[str] = []
        try:
            symbol_join = ""
            metric_join = ""
            if _register_filter(active, "pit_snapshot_symbol_filter", "symbol", symbol_values):
                registered.append("pit_snapshot_symbol_filter")
                symbol_join = "JOIN pit_snapshot_symbol_filter sf ON sf.symbol = s.symbol"
            if _register_filter(active, "pit_snapshot_metric_filter", "canonical_metric", metric_values):
                registered.append("pit_snapshot_metric_filter")
                metric_join = "JOIN pit_snapshot_metric_filter mf ON mf.canonical_metric = upper(s.canonical_metric)"
            sql = PIT_SNAPSHOT_ASOF_SQL.format(symbol_join=symbol_join, metric_join=metric_join)
            return active.con.execute(sql, [snap_month]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)
