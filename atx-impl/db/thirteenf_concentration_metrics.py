"""S22: derived 13F issuer-level holder-concentration analytics.

This module complements the issuer rollup (``thirteenf_security_ownership``), the
manager-flow surface (``thirteenf_position_metrics``), and option-positioning surface
(``thirteenf_option_metrics``) with a premium ownership-control signal: how
concentrated the visible 13F common-share holder base is for each issuer/report
period.

The transform is point-in-time safe: each row uses only common-share 13F positions
visible for that report period, and ``available_at`` is the latest filing availability
among the rows used. No network access is required.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any

import numpy as np
import pandas as pd

from .asof import thirteenf_concentration_metrics_asof  # noqa: F401  (re-exported)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "Derived 13F issuer-level holder concentration"
DEFAULT_SOURCE = "derived_thirteenf_concentration_metrics_v1"

CONCENTRATION_METRIC_COLUMNS = [
    "metric_id", "source", "security_id", "symbol", "cusip", "name_of_issuer",
    "report_period", "source_period", "filing_date", "filing_count", "holder_count",
    "common_value_usd", "common_share_quantity", "top_holder_manager_id",
    "top_holder_value_usd", "top_holder_share_quantity", "top_holder_value_pct",
    "top_holder_share_pct", "top_holder_portfolio_weight", "top_3_holder_value_usd",
    "top_3_holder_value_pct", "top_5_holder_value_usd", "top_5_holder_value_pct",
    "top_10_holder_value_usd", "top_10_holder_value_pct", "value_hhi", "share_hhi",
    "effective_holder_count_value", "effective_holder_count_share", "avg_holder_value_usd",
    "median_holder_value_usd", "concentration_bucket", "prior_report_period",
    "prior_value_hhi", "value_hhi_change", "prior_holder_count", "holder_count_change",
    "is_latest_revision", "as_of_date", "available_at", "run_id",
]


@dataclass(frozen=True)
class ThirteenFConcentrationMetricsOptions:
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


def _hhi(values: pd.Series) -> float:
    total = float(values.fillna(0).sum())
    if total <= 0:
        return np.nan
    weights = values.fillna(0) / total
    return float((weights * weights).sum())


def _effective_count(hhi: float) -> float:
    if pd.isna(hhi) or hhi <= 0:
        return np.nan
    return 1.0 / hhi


def _concentration_bucket(value_hhi: float, top_5_pct: float) -> str:
    """Coarse ownership concentration label for scanning and quality rules."""
    if (pd.notna(value_hhi) and value_hhi >= 0.25) or (pd.notna(top_5_pct) and top_5_pct >= 0.75):
        return "HIGHLY_CONCENTRATED"
    if (pd.notna(value_hhi) and value_hhi >= 0.15) or (pd.notna(top_5_pct) and top_5_pct >= 0.50):
        return "CONCENTRATED"
    if (pd.notna(value_hhi) and value_hhi >= 0.05) or (pd.notna(top_5_pct) and top_5_pct >= 0.25):
        return "MODERATE"
    return "DISPERSED"


def _top_value(sorted_managers: pd.DataFrame, n: int) -> float:
    return float(sorted_managers.head(n)["common_value_usd"].fillna(0).sum())


def compute_concentration_metrics(
    positions: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: common-share 13F positions -> issuer holder-concentration rows."""
    if positions is None or positions.empty:
        return pd.DataFrame(columns=CONCENTRATION_METRIC_COLUMNS)

    df = positions.copy()
    for optional in (
        "manager_report_id",
        "accession_number",
        "security_id",
        "symbol",
        "cusip",
        "name_of_issuer",
        "source_period",
        "put_call",
        "is_common_share",
    ):
        if optional not in df.columns:
            df[optional] = pd.NA
    for date_col in ("report_period", "filing_date", "available_at"):
        if date_col in df.columns:
            df[date_col] = pd.to_datetime(df[date_col], errors="coerce")
        else:
            df[date_col] = pd.NaT
    for numeric_col in ("value_usd", "share_quantity", "portfolio_weight"):
        if numeric_col not in df.columns:
            df[numeric_col] = np.nan
        df[numeric_col] = pd.to_numeric(df[numeric_col], errors="coerce")

    df["put_call"] = df["put_call"].fillna("").astype(str).str.strip().str.upper()
    common_mask = df["is_common_share"].fillna(False).astype(bool) | df["put_call"].eq("")
    df = df[common_mask & df["manager_id"].notna() & df["report_period"].notna()].copy()
    if df.empty:
        return pd.DataFrame(columns=CONCENTRATION_METRIC_COLUMNS)

    rows: list[dict[str, Any]] = []
    keys = ["security_id", "cusip", "report_period", "source_period"]
    for key, g in df.groupby(keys, dropna=False, sort=True):
        security_id, cusip, report_period, source_period = key
        manager_rows: list[dict[str, Any]] = []
        for manager_id, mg in g.groupby("manager_id", dropna=True, sort=True):
            manager_rows.append({
                "manager_id": manager_id,
                "common_value_usd": float(mg["value_usd"].fillna(0).sum()),
                "common_share_quantity": float(mg["share_quantity"].fillna(0).sum()),
                "portfolio_weight": mg["portfolio_weight"].max(skipna=True),
            })
        managers = pd.DataFrame(manager_rows)
        if managers.empty:
            continue
        managers = managers.sort_values(
            ["common_value_usd", "common_share_quantity", "manager_id"],
            ascending=[False, False, True],
        ).reset_index(drop=True)

        total_value = float(managers["common_value_usd"].fillna(0).sum())
        total_shares = float(managers["common_share_quantity"].fillna(0).sum())
        top = managers.iloc[0]
        top3 = _top_value(managers, 3)
        top5 = _top_value(managers, 5)
        top10 = _top_value(managers, 10)
        value_hhi = _hhi(managers["common_value_usd"])
        share_hhi = _hhi(managers["common_share_quantity"])
        top5_pct = _safe_ratio(top5, total_value)

        id_source = g["manager_report_id"].where(g["manager_report_id"].notna(), g["accession_number"])
        rows.append({
            "source": source,
            "security_id": security_id if pd.notna(security_id) else pd.NA,
            "symbol": _first_nonnull(g["symbol"]),
            "cusip": cusip if pd.notna(cusip) else pd.NA,
            "name_of_issuer": _first_nonnull(g["name_of_issuer"]),
            "report_period": report_period,
            "source_period": source_period if pd.notna(source_period) else pd.NA,
            "filing_date": g["filing_date"].max(),
            "filing_count": int(id_source.nunique(dropna=True)),
            "holder_count": int(managers["manager_id"].nunique(dropna=True)),
            "common_value_usd": total_value,
            "common_share_quantity": total_shares,
            "top_holder_manager_id": top["manager_id"],
            "top_holder_value_usd": float(top["common_value_usd"]),
            "top_holder_share_quantity": float(top["common_share_quantity"]),
            "top_holder_value_pct": _safe_ratio(float(top["common_value_usd"]), total_value),
            "top_holder_share_pct": _safe_ratio(float(top["common_share_quantity"]), total_shares),
            "top_holder_portfolio_weight": top["portfolio_weight"],
            "top_3_holder_value_usd": top3,
            "top_3_holder_value_pct": _safe_ratio(top3, total_value),
            "top_5_holder_value_usd": top5,
            "top_5_holder_value_pct": top5_pct,
            "top_10_holder_value_usd": top10,
            "top_10_holder_value_pct": _safe_ratio(top10, total_value),
            "value_hhi": value_hhi,
            "share_hhi": share_hhi,
            "effective_holder_count_value": _effective_count(value_hhi),
            "effective_holder_count_share": _effective_count(share_hhi),
            "avg_holder_value_usd": managers["common_value_usd"].mean(skipna=True),
            "median_holder_value_usd": managers["common_value_usd"].median(skipna=True),
            "concentration_bucket": _concentration_bucket(value_hhi, top5_pct),
            "available_at": g["available_at"].max(),
        })

    out = pd.DataFrame(rows)
    if out.empty:
        return pd.DataFrame(columns=CONCENTRATION_METRIC_COLUMNS)

    out = out.sort_values(["security_id", "cusip", "report_period", "source_period"]).reset_index(drop=True)
    group_key = out["security_id"].fillna("").astype(str) + "|" + out["cusip"].fillna("").astype(str)
    out["prior_report_period"] = out.groupby(group_key)["report_period"].shift(1)
    out["prior_value_hhi"] = out.groupby(group_key)["value_hhi"].shift(1)
    out["value_hhi_change"] = out["value_hhi"] - out["prior_value_hhi"]
    out["prior_holder_count"] = out.groupby(group_key)["holder_count"].shift(1).astype("Int64")
    out["holder_count_change"] = (out["holder_count"] - out["prior_holder_count"]).astype("Int64")
    out["is_latest_revision"] = True
    out["as_of_date"] = out["report_period"].dt.date
    out["run_id"] = run_id
    out["metric_id"] = [
        _metric_id(source, sid, cusip, rp.date() if hasattr(rp, "date") else rp, sp)
        for sid, cusip, rp, sp in zip(out["security_id"], out["cusip"], out["report_period"], out["source_period"])
    ]
    for col in ("report_period", "filing_date", "prior_report_period"):
        out[col] = pd.to_datetime(out[col], errors="coerce").dt.date
    return out[CONCENTRATION_METRIC_COLUMNS]


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
        p.is_common_share
    FROM thirteenf_security_positions p
    WHERE p.manager_id IS NOT NULL
      AND p.report_period IS NOT NULL
      AND (
            p.is_common_share
         OR (coalesce(p.put_call, '') = '' AND upper(coalesce(p.share_quantity_type, '')) = 'SH')
      )
      {symbol_pred}
"""


def load_concentration_inputs(store: DuckDBStore, options: ThirteenFConcentrationMetricsOptions) -> pd.DataFrame:
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_pred = ""
    if symbols:
        store.con.register(
            "tf_concentration_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_pred = "AND p.symbol IN (SELECT symbol FROM tf_concentration_symbol_filter)"
    sql = _LOAD_POSITIONS_SQL.format(symbol_pred=symbol_pred)
    try:
        return store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("tf_concentration_symbol_filter")


def refresh_concentration_metrics(store: DuckDBStore, options: ThirteenFConcentrationMetricsOptions) -> int:
    """Recompute and replace the 13F concentration rows for ``options.source``."""
    store.initialize()
    inputs = load_concentration_inputs(store, options)
    rows = compute_concentration_metrics(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        store.con.execute("DELETE FROM thirteenf_concentration_metrics WHERE source = ?", [options.source])
        if not rows.empty:
            insert_frame(
                store,
                rows,
                "thirteenf_concentration_metrics",
                "thirteenf_concentration_metrics_insert",
            )
    return int(len(rows))


class ThirteenFConcentrationMetricsDataset(Dataset):
    dataset_id = "thirteenf_concentration_metrics"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ThirteenFConcentrationMetricsOptions) -> DatasetLoadResult:
        rows = refresh_concentration_metrics(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="thirteenf_concentration_metrics",
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
