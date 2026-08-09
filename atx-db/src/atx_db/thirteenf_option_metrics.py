"""S20: derived 13F issuer-level option-positioning analytics.

The cached SEC Form 13F holdings feed includes listed option positions via
``put_call``. S16 added manager-level common-share position flow; this module adds
the complementary issuer-period option surface: aggregate call/put share-equivalent
and reported-value exposure, put/call ratios, common-share denominators, and QoQ
net-call changes.

Point-in-time discipline: each row uses only the holdings visible for the report
period/source period, and ``available_at`` is the latest filing availability among
the rows used. No network: pure transform over ``thirteenf_security_positions``.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any

import numpy as np
import pandas as pd

from .asof import thirteenf_option_metrics_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived 13F issuer-level option positioning"
DEFAULT_SOURCE = "derived_thirteenf_option_metrics_v1"

OPTION_METRIC_COLUMNS = [
    "metric_id", "source", "security_id", "symbol", "cusip", "name_of_issuer",
    "report_period", "source_period", "filing_date", "filing_count",
    "option_manager_count", "call_manager_count", "put_manager_count",
    "option_position_count", "call_position_count", "put_position_count",
    "call_share_quantity", "put_share_quantity", "net_call_share_quantity",
    "put_call_share_ratio", "call_value_usd", "put_value_usd",
    "net_call_value_usd", "put_call_value_ratio", "option_value_usd",
    "common_share_quantity", "common_value_usd", "call_to_common_share_pct",
    "put_to_common_share_pct", "option_to_common_value_pct",
    "avg_option_portfolio_weight", "max_option_portfolio_weight",
    "top_call_manager_id", "top_call_manager_value_usd",
    "top_put_manager_id", "top_put_manager_value_usd", "option_bias",
    "prior_report_period", "prior_net_call_share_quantity",
    "net_call_share_change", "net_call_share_change_pct",
    "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class ThirteenFOptionMetricsOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _key_part(value: Any) -> str:
    if value is None or pd.isna(value):
        return ""
    return str(value)


def _metric_id(source: str, security_id: Any, cusip: Any, report_period: Any, source_period: Any) -> str:
    payload = "|".join(_key_part(p) for p in (source, security_id, cusip, report_period, source_period))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _first_nonnull(series: pd.Series):
    values = series.dropna()
    if values.empty:
        return pd.NA
    for value in values:
        if str(value) != "":
            return value
    return pd.NA


def _safe_ratio(numerator: float, denominator: float) -> float:
    if denominator is None or pd.isna(denominator) or denominator <= 0:
        return np.nan
    return numerator / denominator


def _top_manager(option_side: pd.DataFrame) -> tuple[Any, float]:
    if option_side.empty:
        return pd.NA, np.nan
    by_manager = (
        option_side.groupby("manager_id", dropna=False)["value_usd"]
        .sum(min_count=1)
        .reset_index()
    )
    if by_manager["value_usd"].isna().all():
        return pd.NA, np.nan
    idx = by_manager["value_usd"].fillna(-np.inf).idxmax()
    row = by_manager.loc[idx]
    return row["manager_id"], float(row["value_usd"])


def _option_bias(call_shares: float, put_shares: float) -> str:
    if call_shares > put_shares:
        return "CALL_HEAVY"
    if put_shares > call_shares:
        return "PUT_HEAVY"
    if call_shares > 0 or put_shares > 0:
        return "BALANCED"
    return "NO_OPTIONS"


def compute_option_metrics(
    positions: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: 13F positions -> issuer-period option-positioning rows."""
    if positions is None or positions.empty:
        return pd.DataFrame(columns=OPTION_METRIC_COLUMNS)

    df = positions.copy()
    df["report_period"] = pd.to_datetime(df["report_period"], errors="coerce")
    df["filing_date"] = pd.to_datetime(df.get("filing_date"), errors="coerce")
    df["available_at"] = pd.to_datetime(df.get("available_at"), errors="coerce")
    if "put_call" not in df.columns:
        df["put_call"] = ""
    df["put_call"] = df["put_call"].fillna("").astype(str).str.strip().str.upper()
    for col in ("value_usd", "share_quantity", "portfolio_weight"):
        if col not in df.columns:
            df[col] = np.nan
        df[col] = pd.to_numeric(df[col], errors="coerce")
    if "is_common_share" not in df.columns:
        df["is_common_share"] = df["put_call"].eq("")
    if "manager_report_id" not in df.columns:
        df["manager_report_id"] = df.get("accession_number")

    df = df[df["report_period"].notna()].copy()
    if df.empty:
        return pd.DataFrame(columns=OPTION_METRIC_COLUMNS)

    option_mask = df["put_call"].isin(["CALL", "PUT"])
    df = df[option_mask | df["is_common_share"].fillna(False)].copy()
    if df.empty or not df["put_call"].isin(["CALL", "PUT"]).any():
        return pd.DataFrame(columns=OPTION_METRIC_COLUMNS)

    keys = ["security_id", "cusip", "report_period", "source_period"]
    rows: list[dict[str, Any]] = []
    for key, g in df.groupby(keys, dropna=False, sort=True):
        security_id, cusip, report_period, source_period = key
        opts = g[g["put_call"].isin(["CALL", "PUT"])]
        if opts.empty:
            continue
        calls = opts[opts["put_call"].eq("CALL")]
        puts = opts[opts["put_call"].eq("PUT")]
        common = g[g["is_common_share"].fillna(False)]

        call_shares = float(calls["share_quantity"].fillna(0).sum())
        put_shares = float(puts["share_quantity"].fillna(0).sum())
        call_value = float(calls["value_usd"].fillna(0).sum())
        put_value = float(puts["value_usd"].fillna(0).sum())
        common_shares = float(common["share_quantity"].fillna(0).sum())
        common_value = float(common["value_usd"].fillna(0).sum())
        top_call_manager, top_call_value = _top_manager(calls)
        top_put_manager, top_put_value = _top_manager(puts)

        rows.append({
            "source": source,
            "security_id": security_id if pd.notna(security_id) else pd.NA,
            "symbol": _first_nonnull(g.get("symbol", pd.Series(dtype=object))),
            "cusip": cusip if pd.notna(cusip) else pd.NA,
            "name_of_issuer": _first_nonnull(g.get("name_of_issuer", pd.Series(dtype=object))),
            "report_period": report_period,
            "source_period": source_period if pd.notna(source_period) else pd.NA,
            "filing_date": g["filing_date"].max(),
            "filing_count": int(g["manager_report_id"].nunique(dropna=True)),
            "option_manager_count": int(opts["manager_id"].nunique(dropna=True)),
            "call_manager_count": int(calls["manager_id"].nunique(dropna=True)),
            "put_manager_count": int(puts["manager_id"].nunique(dropna=True)),
            "option_position_count": int(len(opts)),
            "call_position_count": int(len(calls)),
            "put_position_count": int(len(puts)),
            "call_share_quantity": call_shares,
            "put_share_quantity": put_shares,
            "net_call_share_quantity": call_shares - put_shares,
            "put_call_share_ratio": _safe_ratio(put_shares, call_shares),
            "call_value_usd": call_value,
            "put_value_usd": put_value,
            "net_call_value_usd": call_value - put_value,
            "put_call_value_ratio": _safe_ratio(put_value, call_value),
            "option_value_usd": call_value + put_value,
            "common_share_quantity": common_shares,
            "common_value_usd": common_value,
            "call_to_common_share_pct": _safe_ratio(call_shares, common_shares),
            "put_to_common_share_pct": _safe_ratio(put_shares, common_shares),
            "option_to_common_value_pct": _safe_ratio(call_value + put_value, common_value),
            "avg_option_portfolio_weight": opts["portfolio_weight"].mean(skipna=True),
            "max_option_portfolio_weight": opts["portfolio_weight"].max(skipna=True),
            "top_call_manager_id": top_call_manager,
            "top_call_manager_value_usd": top_call_value,
            "top_put_manager_id": top_put_manager,
            "top_put_manager_value_usd": top_put_value,
            "option_bias": _option_bias(call_shares, put_shares),
            "available_at": g["available_at"].max(),
        })

    out = pd.DataFrame(rows)
    if out.empty:
        return pd.DataFrame(columns=OPTION_METRIC_COLUMNS)

    out = out.sort_values(["security_id", "cusip", "report_period", "source_period"]).reset_index(drop=True)
    group_key = out["security_id"].fillna("").astype(str) + "|" + out["cusip"].fillna("").astype(str)
    out["prior_report_period"] = out.groupby(group_key)["report_period"].shift(1)
    out["prior_net_call_share_quantity"] = out.groupby(group_key)["net_call_share_quantity"].shift(1)
    out["net_call_share_change"] = out["net_call_share_quantity"] - out["prior_net_call_share_quantity"]
    out["net_call_share_change_pct"] = out["net_call_share_change"] / out["prior_net_call_share_quantity"].where(
        out["prior_net_call_share_quantity"].abs() > 0
    ).abs()
    out["is_latest_revision"] = True
    out["as_of_date"] = out["report_period"].dt.date
    out["run_id"] = run_id
    out["metric_id"] = [
        _metric_id(source, sid, cusip, rp.date() if hasattr(rp, "date") else rp, sp)
        for sid, cusip, rp, sp in zip(out["security_id"], out["cusip"], out["report_period"], out["source_period"])
    ]
    for col in ("report_period", "filing_date", "prior_report_period"):
        out[col] = pd.to_datetime(out[col], errors="coerce").dt.date
    return out[OPTION_METRIC_COLUMNS]


_LOAD_POSITIONS_SQL = """
    SELECT
        p.manager_report_id,
        p.manager_id,
        p.accession_number,
        p.security_id,
        p.symbol,
        p.cusip,
        p.name_of_issuer,
        p.report_period,
        p.filing_date,
        p.source_period,
        p.available_at,
        p.value_usd,
        p.share_quantity,
        p.put_call,
        p.portfolio_weight,
        p.is_common_share,
        p.is_option
    FROM thirteenf_security_positions p
    WHERE p.manager_id IS NOT NULL
      AND p.report_period IS NOT NULL
      AND (p.is_option OR p.is_common_share)
      {symbol_pred}
"""


def load_option_inputs(store: DuckDBStore, options: ThirteenFOptionMetricsOptions) -> pd.DataFrame:
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if symbols:
        store.con.register(
            "tf_option_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_pred = "AND p.symbol IN (SELECT symbol FROM tf_option_symbol_filter)"
    sql = _LOAD_POSITIONS_SQL.format(symbol_pred=symbol_pred)
    try:
        return store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("tf_option_symbol_filter")


def refresh_option_metrics(store: DuckDBStore, options: ThirteenFOptionMetricsOptions) -> int:
    """Recompute and replace the 13F option-positioning rows for ``options.source``."""
    store.initialize()
    inputs = load_option_inputs(store, options)
    rows = compute_option_metrics(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM thirteenf_option_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(store, rows, "thirteenf_option_metrics", "thirteenf_option_metrics_insert")
    return int(len(rows))


class ThirteenFOptionMetricsDataset(Dataset):
    dataset_id = "thirteenf_option_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ThirteenFOptionMetricsOptions) -> DatasetLoadResult:
        rows = refresh_option_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="thirteenf_option_metrics",
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
            details={"grain": "security_id,cusip,report_period,source_period"},
        )
