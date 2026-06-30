"""Derived security listing / reference-compliance analytics.

This module consolidates two broad public reference feeds into one typed,
point-in-time security reference surface keyed by ``security_id``:

* ``listing_status_intervals`` — the SEC-CIK-resolved listing spine (venue,
  exchange, active/inactive status, bitemporal validity), and
* ``nasdaq_symbol_directory`` — the daily Nasdaq Trader symbol-directory
  snapshot (market tier, ETF/test-issue flags, round-lot size, and the Nasdaq
  ``financial_status`` listing-compliance code).

The value-add over the raw feeds is the decoded Nasdaq listing-compliance
signal: ``financial_status`` packs deficiency / delinquency / bankruptcy into a
single letter (D/E/Q and the combined G/H/J/K codes); we expand it into typed
boolean flags that quant screens can use as a delisting-risk / financial-distress
indicator across the whole listed tape, not just the handful of securities with
deep fundamentals coverage.

The transform is offline and point-in-time safe: each derived row is grained by
``(source, security_id, as_of_date)`` (the directory snapshot date) and carries
the later of the listing-spine and directory availability timestamps, so a
``financial_status`` flip between snapshots (e.g. ``D`` -> ``N`` recovery) shows
up as a new, later-dated reference row rather than mutating history.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived security listing reference metrics"
DEFAULT_SOURCE = "derived_security_listing_metrics_v1"
DEFAULT_INPUT_LISTING_SOURCE = "atx_listing_status_intervals_v1"

# Nasdaq Trader symbol-directory `Financial Status` field (nasdaqlisted only).
# Single-letter codes pack three independent compliance states; the combined
# codes are unions of the primaries.
FINANCIAL_STATUS_LABELS = {
    "N": "Normal",
    "D": "Deficient",
    "E": "Delinquent",
    "Q": "Bankrupt",
    "G": "Deficient and Bankrupt",
    "H": "Deficient and Delinquent",
    "J": "Delinquent and Bankrupt",
    "K": "Deficient, Delinquent, and Bankrupt",
}
DEFICIENT_CODES = {"D", "G", "H", "K"}
DELINQUENT_CODES = {"E", "H", "J", "K"}
BANKRUPT_CODES = {"Q", "G", "J", "K"}

# Nasdaq market-category letters (nasdaqlisted only).
MARKET_TIER_LABELS = {
    "Q": "NASDAQ Global Select Market",
    "G": "NASDAQ Global Market",
    "S": "NASDAQ Capital Market",
}

# Exchange letters shared by the directory `exchange` column and the listing
# spine `listing_exchange_code`.
EXCHANGE_NAME_LABELS = {
    "A": "NYSE American",
    "N": "New York Stock Exchange",
    "P": "NYSE Arca",
    "Z": "Cboe BZX Exchange",
    "V": "IEX",
    "Q": "NASDAQ",
    "NASDAQ": "NASDAQ",
}

METRIC_COLUMNS = [
    "metric_id",
    "source",
    "input_listing_source",
    "security_id",
    "symbol",
    "as_of_date",
    "directory",
    "listing_status",
    "listing_venue_code",
    "listing_venue_name",
    "listing_exchange_code",
    "listing_exchange_name",
    "market_category",
    "market_tier",
    "security_name",
    "round_lot_size",
    "is_etf",
    "is_test_issue",
    "is_next_shares",
    "financial_status_code",
    "financial_status_label",
    "has_financial_status",
    "is_listing_compliant",
    "is_deficient",
    "is_delinquent",
    "is_bankrupt",
    "is_noncompliant",
    "restatement_seq",
    "is_latest_revision",
    "available_at",
    "run_id",
]


@dataclass(frozen=True)
class SecurityListingMetricsOptions:
    source: str = DEFAULT_SOURCE
    input_listing_source: str = DEFAULT_INPUT_LISTING_SOURCE
    security_ids: tuple[str, ...] | None = None
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _empty_metrics_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=METRIC_COLUMNS)


def _key_part(value: Any) -> str:
    if value is None or (not isinstance(value, str) and pd.isna(value)):
        return ""
    return str(value)


def _metric_id(row: dict[str, Any]) -> str:
    payload = "|".join(
        _key_part(row.get(part))
        for part in ("source", "security_id", "as_of_date", "available_at")
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _clean_code(value: Any) -> str | None:
    if value is None or (not isinstance(value, str) and pd.isna(value)):
        return None
    text = str(value).strip().upper()
    return text or None


def _nullable_bool(value: Any) -> Any:
    if value is None or (not isinstance(value, bool) and pd.isna(value)):
        return None
    if isinstance(value, bool):
        return value
    text = str(value).strip().lower()
    if text in {"1", "true", "t", "yes", "y"}:
        return True
    if text in {"0", "false", "f", "no", "n"}:
        return False
    return None


def _nullable_int(value: Any) -> Any:
    if value is None or pd.isna(value):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _nullable_text(value: Any) -> Any:
    if value is None or (not isinstance(value, str) and pd.isna(value)):
        return None
    text = str(value).strip()
    return text or None


def compute_security_listing_metrics(
    rows: pd.DataFrame,
    *,
    options: SecurityListingMetricsOptions | None = None,
) -> pd.DataFrame:
    """Pure transform: listing-spine x symbol-directory rows -> PIT reference rows."""

    options = options or SecurityListingMetricsOptions()
    if rows is None or rows.empty:
        return _empty_metrics_frame()

    df = rows.copy()
    df["security_id"] = df["security_id"].map(_nullable_text)
    df["symbol"] = df["symbol"].map(_nullable_text)
    df["as_of_date"] = pd.to_datetime(df["as_of_date"], errors="coerce").dt.date
    df["available_at"] = pd.to_datetime(df["available_at"], errors="coerce")
    df = df[df["security_id"].notna() & df["symbol"].notna() & df["as_of_date"].notna() & df["available_at"].notna()].copy()
    if df.empty:
        return _empty_metrics_frame()

    # Enforce the (security_id, as_of_date) grain deterministically (dual-class
    # symbols can resolve to one security_id on a single snapshot).
    df = df.sort_values(["security_id", "as_of_date", "symbol"]).drop_duplicates(
        ["security_id", "as_of_date"], keep="first"
    )

    out_rows: list[dict[str, Any]] = []
    for _, src in df.iterrows():
        code = _clean_code(src.get("financial_status"))
        has_status = code in FINANCIAL_STATUS_LABELS
        market_category = _clean_code(src.get("market_category"))
        listing_exchange_code = _clean_code(src.get("listing_exchange_code"))
        row: dict[str, Any] = {
            "source": options.source,
            "input_listing_source": options.input_listing_source,
            "security_id": src["security_id"],
            "symbol": src["symbol"],
            "as_of_date": src["as_of_date"],
            "directory": _nullable_text(src.get("directory")),
            "listing_status": _nullable_text(src.get("listing_status")),
            "listing_venue_code": _nullable_text(src.get("listing_venue_code")),
            "listing_venue_name": _nullable_text(src.get("listing_venue_name")),
            "listing_exchange_code": listing_exchange_code,
            "listing_exchange_name": EXCHANGE_NAME_LABELS.get(listing_exchange_code) if listing_exchange_code else None,
            "market_category": market_category,
            "market_tier": MARKET_TIER_LABELS.get(market_category) if market_category else None,
            "security_name": _nullable_text(src.get("security_name")),
            "round_lot_size": _nullable_int(src.get("round_lot_size")),
            "is_etf": _nullable_bool(src.get("etf")),
            "is_test_issue": _nullable_bool(src.get("test_issue")),
            "is_next_shares": _nullable_bool(src.get("next_shares")),
            "financial_status_code": code,
            "financial_status_label": FINANCIAL_STATUS_LABELS.get(code) if has_status else None,
            "has_financial_status": bool(has_status),
            "is_listing_compliant": bool(code == "N"),
            "is_deficient": bool(code in DEFICIENT_CODES),
            "is_delinquent": bool(code in DELINQUENT_CODES),
            "is_bankrupt": bool(code in BANKRUPT_CODES),
            "is_noncompliant": bool(has_status and code != "N"),
            "restatement_seq": 0,
            "is_latest_revision": True,
            "available_at": src["available_at"],
            "run_id": options.run_id,
        }
        row["metric_id"] = _metric_id(row)
        out_rows.append(row)

    out = pd.DataFrame(out_rows)
    if out.empty:
        return _empty_metrics_frame()
    return out[METRIC_COLUMNS]


def _load_listing_inputs(store: DuckDBStore, options: SecurityListingMetricsOptions) -> pd.DataFrame:
    dir_filters: list[str] = []
    listing_filters = ["security_id IS NOT NULL"]
    params: list[Any] = []
    registered: list[str] = []
    try:
        if options.security_ids:
            frame = pd.DataFrame({"security_id": [str(v) for v in options.security_ids if str(v)]})
            if not frame.empty:
                store.con.register("listing_metric_security_filter", frame)
                registered.append("listing_metric_security_filter")
                listing_filters.append("security_id IN (SELECT security_id FROM listing_metric_security_filter)")
        if options.symbols:
            frame = pd.DataFrame({"symbol": [str(v).strip().upper() for v in options.symbols if str(v).strip()]})
            if not frame.empty:
                store.con.register("listing_metric_symbol_filter", frame)
                registered.append("listing_metric_symbol_filter")
                dir_filters.append("upper(symbol) IN (SELECT symbol FROM listing_metric_symbol_filter)")
                listing_filters.append("upper(symbol) IN (SELECT symbol FROM listing_metric_symbol_filter)")

        dir_where = ("WHERE " + " AND ".join(dir_filters)) if dir_filters else ""
        listing_where = ("WHERE " + " AND ".join(listing_filters)) if listing_filters else ""
        return store.con.execute(
            f"""
            WITH dir AS (
                SELECT
                    symbol, as_of_date, directory, market_category, exchange,
                    security_name, financial_status, etf, test_issue, next_shares,
                    round_lot_size, source_loaded_at,
                    row_number() OVER (
                        PARTITION BY symbol, as_of_date
                        ORDER BY source_loaded_at DESC
                    ) AS rn
                FROM nasdaq_symbol_directory
                {dir_where}
            ),
            listing AS (
                SELECT
                    security_id, symbol, status, listing_venue_code, listing_venue_name,
                    listing_exchange_code, available_at,
                    row_number() OVER (
                        PARTITION BY symbol
                        ORDER BY valid_from DESC, available_at DESC, listing_status_id
                    ) AS rn
                FROM listing_status_intervals
                {listing_where}
            )
            SELECT
                l.security_id,
                d.symbol,
                d.as_of_date,
                d.directory,
                d.market_category,
                d.exchange,
                d.security_name,
                d.financial_status,
                d.etf,
                d.test_issue,
                d.next_shares,
                d.round_lot_size,
                l.status AS listing_status,
                l.listing_venue_code,
                l.listing_venue_name,
                l.listing_exchange_code,
                -- The reference signal (financial_status, tier, ETF flag) is
                -- knowable at the directory snapshot ingest time; the listing
                -- spine only supplies the stable symbol->security_id identity.
                d.source_loaded_at AS available_at
            FROM dir d
            JOIN listing l ON l.symbol = d.symbol AND l.rn = 1
            WHERE d.rn = 1
            """,
            params,
        ).df()
    finally:
        for relation in registered:
            store.con.unregister(relation)


def _delete_existing_ids(store: DuckDBStore, frame: pd.DataFrame) -> None:
    ids = frame[["metric_id"]].drop_duplicates()
    relation = "security_listing_metrics_delete"
    store.con.register(relation, ids)
    try:
        store.con.execute(
            "DELETE FROM security_listing_metrics AS dst USING security_listing_metrics_delete AS src "
            "WHERE dst.metric_id = src.metric_id"
        )
    finally:
        store.con.unregister(relation)


def _recompute_latest(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        WITH ranked AS (
            SELECT
                metric_id,
                row_number() OVER (
                    PARTITION BY source, security_id, as_of_date
                    ORDER BY available_at DESC, metric_id
                ) AS rn,
                dense_rank() OVER (
                    PARTITION BY source, security_id, as_of_date
                    ORDER BY available_at ASC
                ) - 1 AS seq
            FROM security_listing_metrics
            WHERE source = ?
        )
        UPDATE security_listing_metrics m
        SET is_latest_revision = (ranked.rn = 1),
            restatement_seq = ranked.seq
        FROM ranked
        WHERE ranked.metric_id = m.metric_id
        """,
        [source],
    )


def refresh_security_listing_metrics(
    store: DuckDBStore,
    options: SecurityListingMetricsOptions | None = None,
) -> int:
    store.initialize()
    options = options or SecurityListingMetricsOptions()
    rows = _load_listing_inputs(store, options)
    metrics = compute_security_listing_metrics(rows, options=options)
    if metrics.empty:
        return 0
    with store.transaction():
        _delete_existing_ids(store, metrics)
        insert_frame(store, metrics, "security_listing_metrics", "security_listing_metrics_insert")
        _recompute_latest(store, options.source)
    return int(len(metrics))


class SecurityListingMetricsDataset(Dataset):
    dataset_id = "security_listing_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SecurityListingMetricsOptions) -> DatasetLoadResult:
        rows = refresh_security_listing_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="security_listing_metrics",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "input_listing_source": options.input_listing_source,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "source,security_id,as_of_date"},
        )
