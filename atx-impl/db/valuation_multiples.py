"""PF-S6 S6-1: PIT-safe market capitalization.

Market cap is struck as the raw daily close times the latest applicable share
count, then made visible when both the price and selected share vintage are
available. This deliberately uses ``equity_daily_bars.close`` rather than
``adjusted_close``: market capitalization is a same-day level, not a
back-adjusted return series.
"""
from __future__ import annotations

import hashlib
import datetime as dt
from dataclasses import dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check, replace_by_relation


SOURCE_NAME = "Derived market capitalization"
DEFAULT_MARKET_CAP_SOURCE = "derived_market_cap_v1"
SHARE_COUNT_PRIORITY = ("shares_outstanding", "shares_diluted_avg")

MARKET_CAP_COLUMNS = [
    "market_cap_id",
    "source",
    "price_source",
    "share_source",
    "security_id",
    "symbol",
    "trade_date",
    "close",
    "share_count",
    "share_count_type_used",
    "market_cap",
    "is_latest_revision",
    "as_of_date",
    "available_at",
    "price_available_at",
    "share_available_at",
    "price_run_id",
    "share_run_id",
    "share_history_id",
    "input_codes_json",
    "input_lineage_json",
    "run_id",
]


@dataclass(frozen=True)
class MarketCapOptions:
    source: str = DEFAULT_MARKET_CAP_SOURCE
    price_sources: tuple[str, ...] | None = None
    symbols: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    run_id: str | None = None


def _market_cap_id(source: str, security_id: str, trade_date) -> str:
    payload = "|".join(str(part) for part in (source, security_id, trade_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _normalize_price_inputs(prices: pd.DataFrame) -> pd.DataFrame:
    out = prices.copy()
    if "price_source" not in out.columns:
        out["price_source"] = out.get("source")
    if "price_available_at" not in out.columns:
        out["price_available_at"] = out.get("available_at")
    if "price_run_id" not in out.columns:
        out["price_run_id"] = out.get("run_id")
    out["trade_date"] = pd.to_datetime(out["trade_date"], errors="coerce").dt.date
    out["price_available_at"] = pd.to_datetime(out["price_available_at"], errors="coerce")
    out["close"] = pd.to_numeric(out["close"], errors="coerce")
    return out.dropna(subset=["security_id", "trade_date", "close", "price_available_at"])


def _normalize_share_inputs(shares: pd.DataFrame) -> pd.DataFrame:
    out = shares.copy()
    if "share_source" not in out.columns:
        out["share_source"] = out.get("source")
    if "share_available_at" not in out.columns:
        out["share_available_at"] = out.get("available_at")
    if "share_run_id" not in out.columns:
        out["share_run_id"] = out.get("run_id")
    out["effective_date"] = pd.to_datetime(out["effective_date"], errors="coerce").dt.date
    out["share_as_of_date"] = pd.to_datetime(out.get("as_of_date"), errors="coerce").dt.date
    out["share_available_at"] = pd.to_datetime(out["share_available_at"], errors="coerce")
    out["share_count"] = pd.to_numeric(out["share_count"], errors="coerce")
    out["share_count_type"] = out["share_count_type"].astype("string")
    if "share_history_id" not in out.columns:
        out["share_history_id"] = pd.NA
    if "revision_sequence" not in out.columns:
        out["revision_sequence"] = 0
    out["revision_sequence"] = pd.to_numeric(out["revision_sequence"], errors="coerce").fillna(0)
    return out.dropna(
        subset=[
            "security_id",
            "share_count_type",
            "effective_date",
            "share_as_of_date",
            "share_available_at",
            "share_count",
        ]
    )


def _select_pit_share_rows(prices: pd.DataFrame, shares: pd.DataFrame) -> pd.DataFrame:
    price_rows = _normalize_price_inputs(prices)
    share_rows = _normalize_share_inputs(shares)
    if price_rows.empty or share_rows.empty:
        return pd.DataFrame()

    results: list[dict[str, object]] = []
    shares_by_security = {
        security_id: group.copy()
        for security_id, group in share_rows.groupby("security_id", sort=False)
    }
    for price in price_rows.to_dict("records"):
        candidates = shares_by_security.get(price["security_id"])
        if candidates is None or candidates.empty:
            continue
        visible = candidates[
            (candidates["effective_date"] <= price["trade_date"])
            & (candidates["share_as_of_date"] <= price["trade_date"])
        ]
        if visible.empty:
            continue
        chosen = None
        for share_type in SHARE_COUNT_PRIORITY:
            typed = visible[visible["share_count_type"] == share_type]
            if typed.empty:
                continue
            chosen = typed.sort_values(
                [
                    "effective_date",
                    "share_as_of_date",
                    "share_available_at",
                    "revision_sequence",
                    "share_history_id",
                ],
                ascending=[False, False, False, False, False],
            ).iloc[0]
            break
        if chosen is None:
            continue
        row = dict(price)
        for column in (
            "share_source",
            "share_history_id",
            "share_count",
            "share_count_type",
            "share_available_at",
            "share_run_id",
            "effective_date",
            "share_as_of_date",
        ):
            row[column] = chosen.get(column)
        results.append(row)
    return pd.DataFrame(results)


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "price": {
                "table": "equity_daily_bars",
                "source": row.get("price_source"),
                "security_id": row.get("security_id"),
                "trade_date": row.get("trade_date"),
                "available_at": row.get("price_available_at"),
                "run_id": row.get("price_run_id"),
                "field": "close",
            },
            "shares": {
                "table": "shares_outstanding_history",
                "source": row.get("share_source"),
                "share_history_id": row.get("share_history_id"),
                "share_count_type": row.get("share_count_type"),
                "effective_date": row.get("effective_date"),
                "as_of_date": row.get("share_as_of_date"),
                "available_at": row.get("share_available_at"),
                "run_id": row.get("share_run_id"),
                "field": "share_count",
            },
        }
    )


def compute_market_cap_rows(
    prices: pd.DataFrame,
    shares: pd.DataFrame | None = None,
    *,
    source: str = DEFAULT_MARKET_CAP_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: prices + PIT share history -> ``market_cap`` rows.

    If ``shares`` is supplied, the transform performs the PIT share selection
    itself. If ``shares`` is omitted, ``prices`` is treated as an already-matched
    price/share input frame, as returned by :func:`load_market_cap_inputs`.
    """
    matched = _select_pit_share_rows(prices, shares) if shares is not None else prices.copy()
    if matched.empty:
        return pd.DataFrame(columns=MARKET_CAP_COLUMNS)

    out = matched.copy()
    out["trade_date"] = pd.to_datetime(out["trade_date"], errors="coerce").dt.date
    out["price_available_at"] = pd.to_datetime(out["price_available_at"], errors="coerce")
    out["share_available_at"] = pd.to_datetime(out["share_available_at"], errors="coerce")
    out["close"] = pd.to_numeric(out["close"], errors="coerce")
    out["share_count"] = pd.to_numeric(out["share_count"], errors="coerce")
    out = out.dropna(
        subset=["security_id", "trade_date", "close", "share_count", "price_available_at", "share_available_at"]
    )
    if out.empty:
        return pd.DataFrame(columns=MARKET_CAP_COLUMNS)

    out["source"] = source
    out["share_count_type_used"] = out["share_count_type"]
    out["market_cap"] = out["close"] * out["share_count"]
    out["as_of_date"] = out["trade_date"]
    out["available_at"] = out[["price_available_at", "share_available_at"]].max(axis=1)
    out["is_latest_revision"] = True
    out["run_id"] = run_id
    out["market_cap_id"] = [
        _market_cap_id(source, security_id, trade_date)
        for security_id, trade_date in zip(out["security_id"], out["trade_date"])
    ]
    out["input_codes_json"] = out.apply(
        lambda row: json_dumps(
            {
                "price": "equity_daily_bars.close",
                "shares": f"shares_outstanding_history.{row['share_count_type_used']}",
            }
        ),
        axis=1,
    )
    out["input_lineage_json"] = out.apply(_lineage, axis=1)
    if "symbol" not in out.columns:
        out["symbol"] = pd.NA
    for column in ("price_source", "share_source", "price_run_id", "share_run_id", "share_history_id"):
        if column not in out.columns:
            out[column] = pd.NA
    return out[MARKET_CAP_COLUMNS]


def load_market_cap_inputs(store: DuckDBStore, options: MarketCapOptions) -> pd.DataFrame:
    registered: list[str] = []
    filters: list[str] = []
    if options.price_sources:
        store.con.register("market_cap_price_source_filter", pd.DataFrame({"price_source": list(options.price_sources)}))
        registered.append("market_cap_price_source_filter")
        filters.append("JOIN market_cap_price_source_filter psf ON psf.price_source = b.source")
    if options.symbols:
        store.con.register("market_cap_symbol_filter", pd.DataFrame({"symbol": list(options.symbols)}))
        registered.append("market_cap_symbol_filter")
        filters.append("JOIN market_cap_symbol_filter sf ON sf.symbol = b.symbol")
    date_predicates = []
    params: list[object] = []
    if options.start_date is not None:
        date_predicates.append("b.trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("b.trade_date <= ?")
        params.append(options.end_date)
    where_extra = ""
    if date_predicates:
        where_extra = " AND " + " AND ".join(date_predicates)
    join_extra = "\n        ".join(filters)
    sql = f"""
        WITH price_ranked AS (
            SELECT
                b.source AS price_source,
                b.security_id,
                b.symbol,
                b.trade_date,
                b.close,
                b.available_at AS price_available_at,
                b.run_id AS price_run_id,
                row_number() OVER (
                    PARTITION BY b.security_id, b.trade_date
                    ORDER BY b.available_at DESC, b.source DESC
                ) AS price_rn
            FROM equity_daily_bars b
            {join_extra}
            WHERE b.close IS NOT NULL
              AND b.security_id IS NOT NULL
              AND b.trade_date IS NOT NULL
              AND b.available_at IS NOT NULL
              {where_extra}
        ),
        prices AS (
            SELECT * EXCLUDE (price_rn)
            FROM price_ranked
            WHERE price_rn = 1
        ),
        share_candidates AS (
            SELECT
                p.*,
                s.source AS share_source,
                s.share_history_id,
                s.share_count_type,
                s.share_count,
                s.available_at AS share_available_at,
                s.run_id AS share_run_id,
                s.effective_date,
                s.as_of_date AS share_as_of_date,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY
                        CASE s.share_count_type
                            WHEN 'shares_outstanding' THEN 0
                            WHEN 'shares_diluted_avg' THEN 1
                            ELSE 2
                        END,
                        s.effective_date DESC,
                        s.as_of_date DESC,
                        s.available_at DESC,
                        s.revision_sequence DESC,
                        s.share_history_id DESC
                ) AS share_rn
            FROM prices p
            JOIN shares_outstanding_history s
              ON s.security_id = p.security_id
             AND s.share_count_type IN ('shares_outstanding', 'shares_diluted_avg')
             AND s.effective_date <= p.trade_date
             AND s.as_of_date <= p.trade_date
             AND s.share_count IS NOT NULL
             AND s.share_count >= 0
        )
        SELECT * EXCLUDE (share_rn)
        FROM share_candidates
        WHERE share_rn = 1
    """
    try:
        return store.con.execute(sql, params).df()
    finally:
        for relation in registered:
            store.con.unregister(relation)


def _delete_market_cap_scope(
    store: DuckDBStore,
    options: MarketCapOptions,
    rows: pd.DataFrame,
) -> None:
    """Delete only the output keys/scope that this refresh is allowed to replace."""

    has_scope_filter = any(
        (
            options.price_sources,
            options.symbols,
            options.start_date is not None,
            options.end_date is not None,
        )
    )

    if has_scope_filter:
        predicates = ["source = ?"]
        params: list[object] = [options.source]
        if options.symbols:
            placeholders = ", ".join("?" for _ in options.symbols)
            predicates.append(f"symbol IN ({placeholders})")
            params.extend(options.symbols)
        if options.start_date is not None:
            predicates.append("trade_date >= ?")
            params.append(options.start_date)
        if options.end_date is not None:
            predicates.append("trade_date <= ?")
            params.append(options.end_date)
        if options.price_sources:
            placeholders = ", ".join("?" for _ in options.price_sources)
            predicates.append(f"price_source IN ({placeholders})")
            params.extend(options.price_sources)
        store.con.execute(f"DELETE FROM market_cap WHERE {' AND '.join(predicates)}", params)
    else:
        store.con.execute("DELETE FROM market_cap WHERE source = ?", [options.source])

    if rows.empty:
        return

    relation_name = "market_cap_replace_keys"
    store.con.register(relation_name, rows[["source", "security_id", "trade_date"]])
    try:
        replace_by_relation(
            store,
            table="market_cap",
            relation=relation_name,
            key_columns=("source", "security_id", "trade_date"),
        )
    finally:
        store.con.unregister(relation_name)


def refresh_market_cap(store: DuckDBStore, options: MarketCapOptions | None = None) -> int:
    options = options or MarketCapOptions()
    store.initialize()
    inputs = load_market_cap_inputs(store, options)
    rows = compute_market_cap_rows(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        _delete_market_cap_scope(store, options, rows)
        if not rows.empty:
            insert_frame(store, rows, "market_cap", "market_cap_insert")
    return int(len(rows))


class MarketCapDataset(Dataset):
    dataset_id = "market_cap"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: MarketCapOptions) -> DatasetLoadResult:
        rows = refresh_market_cap(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="market_cap",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "security_id,trade_date"},
        )
