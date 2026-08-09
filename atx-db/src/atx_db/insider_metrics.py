"""Derived Section 16 insider transaction analytics.

This module turns normalized Form 3/4/5 ownership XML rows into issuer-day
signals used by insider-trading screens: open-market purchases, 10b5-1 plan
sale contamination, discretionary sale pressure, and trailing-window cluster
buy diagnostics. The transform is offline and point-in-time safe: each derived
row carries the latest availability timestamp of the transactions used.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .insider_ownership import SOURCE_NAME as INSIDER_SOURCE_NAME
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Derived SEC insider transaction metrics"
DEFAULT_SOURCE = "derived_insider_transaction_metrics_v1"

METRIC_COLUMNS = [
    "metric_id",
    "source",
    "input_source",
    "security_id",
    "issuer_cik",
    "issuer_name",
    "issuer_trading_symbol",
    "signal_date",
    "window_days",
    "cluster_min_buyers",
    "cluster_min_purchase_value",
    "transaction_count",
    "open_market_purchase_count",
    "open_market_sale_count",
    "discretionary_sale_count",
    "plan_sale_count",
    "grant_count",
    "exercise_count",
    "tax_withholding_count",
    "unique_insider_count",
    "buyer_count",
    "seller_count",
    "director_count",
    "officer_count",
    "ten_percent_owner_count",
    "gross_purchase_shares",
    "gross_sale_shares",
    "net_purchase_shares",
    "gross_purchase_value",
    "gross_sale_value",
    "discretionary_sale_value",
    "plan_sale_value",
    "net_purchase_value",
    "cluster_purchase_count",
    "cluster_buyer_count",
    "cluster_purchase_value",
    "cluster_sale_count",
    "cluster_seller_count",
    "cluster_sale_value",
    "plan_sale_value_ratio",
    "is_cluster_buy",
    "is_discretionary_sell_pressure",
    "is_10b5_1_heavy_sale",
    "source_transaction_ids_json",
    "restatement_seq",
    "is_latest_revision",
    "as_of_date",
    "available_at",
    "run_id",
]


@dataclass(frozen=True)
class InsiderTransactionMetricsOptions:
    source: str = DEFAULT_SOURCE
    input_source: str = INSIDER_SOURCE_NAME
    window_days: int = 30
    cluster_min_buyers: int = 2
    cluster_min_purchase_value: float = 25_000.0
    security_ids: tuple[str, ...] | None = None
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _empty_metrics_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=METRIC_COLUMNS)


def _key_part(value: Any) -> str:
    if value is None or pd.isna(value):
        return ""
    return str(value)


def _metric_id(row: pd.Series) -> str:
    payload = "|".join(
        _key_part(row.get(part))
        for part in ("source", "security_id", "signal_date", "window_days", "available_at")
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _first_nonnull(series: pd.Series) -> Any:
    for value in series:
        if pd.notna(value) and str(value) != "":
            return value
    return pd.NA


def _sum(series: pd.Series) -> float:
    return float(pd.to_numeric(series, errors="coerce").fillna(0).sum())


def _bool_nullable(series: pd.Series) -> pd.Series:
    def parse(value: Any) -> Any:
        if pd.isna(value):
            return pd.NA
        if isinstance(value, bool):
            return value
        text = str(value).strip().lower()
        if text in {"1", "true", "t", "yes", "y"}:
            return True
        if text in {"0", "false", "f", "no", "n"}:
            return False
        return pd.NA

    return series.map(parse).astype("boolean")


def _unique_count(frame: pd.DataFrame, mask: pd.Series, column: str = "insider_id") -> int:
    if frame.empty:
        return 0
    return int(frame.loc[mask.fillna(False), column].dropna().nunique())


def _load_transactions(store: DuckDBStore, options: InsiderTransactionMetricsOptions) -> pd.DataFrame:
    params: list[Any] = [options.input_source]
    filters = ["t.source = ?", "coalesce(t.transaction_date, t.as_of_date) IS NOT NULL", "t.security_id IS NOT NULL"]
    registered: list[str] = []
    try:
        if options.security_ids:
            frame = pd.DataFrame({"security_id": [str(value) for value in options.security_ids if str(value)]})
            if not frame.empty:
                store.con.register("insider_metric_security_filter", frame)
                registered.append("insider_metric_security_filter")
                filters.append("t.security_id IN (SELECT security_id FROM insider_metric_security_filter)")
        if options.symbols:
            frame = pd.DataFrame({"symbol": [str(value).strip().upper() for value in options.symbols if str(value).strip()]})
            if not frame.empty:
                store.con.register("insider_metric_symbol_filter", frame)
                registered.append("insider_metric_symbol_filter")
                filters.append("upper(coalesce(t.issuer_trading_symbol, '')) IN (SELECT symbol FROM insider_metric_symbol_filter)")

        return store.con.execute(
            f"""
            WITH rel AS (
                SELECT
                    insider_id,
                    security_id,
                    max(CASE WHEN is_director THEN 1 ELSE 0 END) AS is_director,
                    max(CASE WHEN is_officer THEN 1 ELSE 0 END) AS is_officer,
                    max(CASE WHEN is_ten_percent_owner THEN 1 ELSE 0 END) AS is_ten_percent_owner
                FROM insider_relationship
                GROUP BY insider_id, security_id
            )
            SELECT
                t.transaction_id,
                t.source AS input_source,
                t.security_id,
                t.issuer_cik,
                t.issuer_name,
                t.issuer_trading_symbol,
                t.insider_id,
                coalesce(t.transaction_date, t.as_of_date) AS signal_date,
                t.transaction_code,
                t.acquired_disposed,
                t.transaction_shares,
                t.transaction_price,
                t.rule_10b5_1_indicator,
                t.available_at,
                coalesce(rel.is_director, 0) AS is_director,
                coalesce(rel.is_officer, 0) AS is_officer,
                coalesce(rel.is_ten_percent_owner, 0) AS is_ten_percent_owner
            FROM insider_transaction t
            LEFT JOIN rel
              ON rel.insider_id = t.insider_id
             AND rel.security_id = t.security_id
            WHERE {" AND ".join(filters)}
            """,
            params,
        ).df()
    finally:
        for relation in registered:
            store.con.unregister(relation)


def compute_insider_transaction_metrics(
    transactions: pd.DataFrame,
    *,
    options: InsiderTransactionMetricsOptions | None = None,
) -> pd.DataFrame:
    """Pure transform: insider transactions -> PIT issuer-day signal rows."""

    options = options or InsiderTransactionMetricsOptions()
    if transactions is None or transactions.empty:
        return _empty_metrics_frame()
    if options.window_days < 1:
        raise ValueError("window_days must be positive")
    if options.cluster_min_buyers < 1:
        raise ValueError("cluster_min_buyers must be positive")

    tx = transactions.copy()
    tx["source"] = options.source
    tx["signal_date"] = pd.to_datetime(tx["signal_date"], errors="coerce").dt.date
    tx["available_at"] = pd.to_datetime(tx["available_at"], errors="coerce")
    tx = tx[tx["security_id"].notna() & tx["signal_date"].notna() & tx["available_at"].notna()].copy()
    if tx.empty:
        return _empty_metrics_frame()

    tx["transaction_code"] = tx["transaction_code"].fillna("").astype(str).str.upper()
    tx["acquired_disposed"] = tx["acquired_disposed"].fillna("").astype(str).str.upper()
    tx["transaction_shares"] = pd.to_numeric(tx["transaction_shares"], errors="coerce")
    tx["transaction_price"] = pd.to_numeric(tx["transaction_price"], errors="coerce")
    tx["transaction_value"] = (tx["transaction_shares"] * tx["transaction_price"]).where(
        tx["transaction_shares"].notna() & tx["transaction_price"].notna()
    )
    tx["rule_10b5_1_indicator"] = _bool_nullable(tx["rule_10b5_1_indicator"])
    tx["is_purchase"] = tx["transaction_code"].eq("P") & tx["acquired_disposed"].ne("D")
    tx["is_sale"] = tx["transaction_code"].eq("S") & tx["acquired_disposed"].ne("A")
    tx["is_plan_sale"] = tx["is_sale"] & tx["rule_10b5_1_indicator"].fillna(False)
    tx["is_discretionary_sale"] = tx["is_sale"] & tx["rule_10b5_1_indicator"].eq(False)
    tx["is_grant"] = tx["transaction_code"].eq("A")
    tx["is_exercise"] = tx["transaction_code"].eq("M")
    tx["is_tax_withholding"] = tx["transaction_code"].eq("F")
    for col in ("is_director", "is_officer", "is_ten_percent_owner"):
        tx[col] = pd.to_numeric(tx[col], errors="coerce").fillna(0).astype(bool)

    rows: list[dict[str, Any]] = []
    tx = tx.sort_values(["source", "security_id", "signal_date", "available_at", "transaction_id"])
    for (source, security_id, signal_date), daily in tx.groupby(["source", "security_id", "signal_date"], sort=True):
        window_start = signal_date - pd.Timedelta(days=options.window_days - 1)
        same_security = tx[(tx["source"] == source) & (tx["security_id"] == security_id)]
        window = same_security[
            (same_security["signal_date"] >= window_start)
            & (same_security["signal_date"] <= signal_date)
        ].copy()

        purchase = daily["is_purchase"]
        sale = daily["is_sale"]
        plan_sale = daily["is_plan_sale"]
        discretionary_sale = daily["is_discretionary_sale"]
        window_purchase = window["is_purchase"]
        window_sale = window["is_sale"]

        gross_purchase_value = _sum(daily.loc[purchase, "transaction_value"])
        gross_sale_value = _sum(daily.loc[sale, "transaction_value"])
        plan_sale_value = _sum(daily.loc[plan_sale, "transaction_value"])
        discretionary_sale_value = _sum(daily.loc[discretionary_sale, "transaction_value"])
        cluster_purchase_value = _sum(window.loc[window_purchase, "transaction_value"])
        cluster_sale_value = _sum(window.loc[window_sale, "transaction_value"])
        cluster_buyer_count = _unique_count(window, window_purchase)
        cluster_seller_count = _unique_count(window, window_sale)
        plan_sale_value_ratio = plan_sale_value / gross_sale_value if gross_sale_value > 0 else pd.NA

        rows.append(
            {
                "source": options.source,
                "input_source": _first_nonnull(daily["input_source"]),
                "security_id": security_id,
                "issuer_cik": _first_nonnull(daily["issuer_cik"]),
                "issuer_name": _first_nonnull(daily["issuer_name"]),
                "issuer_trading_symbol": _first_nonnull(daily["issuer_trading_symbol"]),
                "signal_date": signal_date,
                "window_days": options.window_days,
                "cluster_min_buyers": options.cluster_min_buyers,
                "cluster_min_purchase_value": float(options.cluster_min_purchase_value),
                "transaction_count": int(len(daily)),
                "open_market_purchase_count": int(purchase.sum()),
                "open_market_sale_count": int(sale.sum()),
                "discretionary_sale_count": int(discretionary_sale.sum()),
                "plan_sale_count": int(plan_sale.sum()),
                "grant_count": int(daily["is_grant"].sum()),
                "exercise_count": int(daily["is_exercise"].sum()),
                "tax_withholding_count": int(daily["is_tax_withholding"].sum()),
                "unique_insider_count": int(daily["insider_id"].dropna().nunique()),
                "buyer_count": _unique_count(daily, purchase),
                "seller_count": _unique_count(daily, sale),
                "director_count": _unique_count(daily, daily["is_director"]),
                "officer_count": _unique_count(daily, daily["is_officer"]),
                "ten_percent_owner_count": _unique_count(daily, daily["is_ten_percent_owner"]),
                "gross_purchase_shares": _sum(daily.loc[purchase, "transaction_shares"]),
                "gross_sale_shares": _sum(daily.loc[sale, "transaction_shares"]),
                "net_purchase_shares": _sum(daily.loc[purchase, "transaction_shares"]) - _sum(daily.loc[sale, "transaction_shares"]),
                "gross_purchase_value": gross_purchase_value,
                "gross_sale_value": gross_sale_value,
                "discretionary_sale_value": discretionary_sale_value,
                "plan_sale_value": plan_sale_value,
                "net_purchase_value": gross_purchase_value - gross_sale_value,
                "cluster_purchase_count": int(window_purchase.sum()),
                "cluster_buyer_count": cluster_buyer_count,
                "cluster_purchase_value": cluster_purchase_value,
                "cluster_sale_count": int(window_sale.sum()),
                "cluster_seller_count": cluster_seller_count,
                "cluster_sale_value": cluster_sale_value,
                "plan_sale_value_ratio": plan_sale_value_ratio,
                "is_cluster_buy": bool(
                    cluster_buyer_count >= options.cluster_min_buyers
                    and cluster_purchase_value >= options.cluster_min_purchase_value
                ),
                "is_discretionary_sell_pressure": bool(
                    _unique_count(window, window["is_discretionary_sale"]) >= options.cluster_min_buyers
                    and _sum(window.loc[window["is_discretionary_sale"], "transaction_value"])
                    >= options.cluster_min_purchase_value
                    and _sum(window.loc[window["is_discretionary_sale"], "transaction_value"]) > cluster_purchase_value
                ),
                "is_10b5_1_heavy_sale": bool(gross_sale_value > 0 and pd.notna(plan_sale_value_ratio) and plan_sale_value_ratio >= 0.5),
                "source_transaction_ids_json": json_dumps(sorted(str(value) for value in window["transaction_id"].dropna().unique())),
                "restatement_seq": 0,
                "is_latest_revision": True,
                "as_of_date": signal_date,
                "available_at": window["available_at"].max(),
                "run_id": options.run_id,
            }
        )

    out = pd.DataFrame(rows)
    if out.empty:
        return _empty_metrics_frame()
    out["metric_id"] = out.apply(_metric_id, axis=1)
    return out[METRIC_COLUMNS]


def _delete_existing_ids(store: DuckDBStore, frame: pd.DataFrame) -> None:
    ids = frame[["metric_id"]].drop_duplicates()
    relation = "insider_transaction_metrics_delete"
    store.con.register(relation, ids)
    try:
        store.con.execute(
            "DELETE FROM insider_transaction_metrics AS dst USING insider_transaction_metrics_delete AS src WHERE dst.metric_id = src.metric_id"
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
                    PARTITION BY source, security_id, signal_date, window_days
                    ORDER BY available_at DESC, metric_id
                ) AS rn,
                dense_rank() OVER (
                    PARTITION BY source, security_id, signal_date, window_days
                    ORDER BY available_at ASC
                ) - 1 AS seq
            FROM insider_transaction_metrics
            WHERE source = ?
        )
        UPDATE insider_transaction_metrics m
        SET is_latest_revision = (ranked.rn = 1),
            restatement_seq = ranked.seq
        FROM ranked
        WHERE ranked.metric_id = m.metric_id
        """,
        [source],
    )


def refresh_insider_transaction_metrics(
    store: DuckDBStore,
    options: InsiderTransactionMetricsOptions | None = None,
) -> int:
    store.initialize()
    options = options or InsiderTransactionMetricsOptions()
    transactions = _load_transactions(store, options)
    metrics = compute_insider_transaction_metrics(transactions, options=options)
    if metrics.empty:
        return 0
    with store.transaction():
        _delete_existing_ids(store, metrics)
        insert_frame(store, metrics, "insider_transaction_metrics", "insider_transaction_metrics_insert")
        _recompute_latest(store, options.source)
    return int(len(metrics))


class InsiderTransactionMetricsDataset(Dataset):
    dataset_id = "insider_transaction_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: InsiderTransactionMetricsOptions) -> DatasetLoadResult:
        rows = refresh_insider_transaction_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="insider_transaction_metrics",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "input_source": options.input_source,
                "window_days": options.window_days,
                "cluster_min_buyers": options.cluster_min_buyers,
                "cluster_min_purchase_value": options.cluster_min_purchase_value,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "source,security_id,signal_date,window_days"},
        )
