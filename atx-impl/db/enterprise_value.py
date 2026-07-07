"""PF3-S5 S5-1: point-in-time enterprise value surface."""

from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Derived point-in-time enterprise value"
DEFAULT_ENTERPRISE_VALUE_SOURCE = "derived_enterprise_value_v1"
DEFAULT_MARKET_CAP_SOURCE = "derived_market_cap_v1"

COMPONENT_CODE_MAP = {
    "market_cap": "market_cap.market_cap",
    "total_debt": "fundamental_statement_points.total_debt",
    "preferred_equity": "fundamental_statement_points.pref_stock",
    "minority_interest": "fundamental_statement_points.minority_int_bs",
    "cash_and_equivalents": "fundamental_statement_points.cash_st_inv",
}

COMPONENT_METRICS = {
    "total_debt": "total_debt",
    "preferred_equity": "pref_stock",
    "minority_interest": "minority_int_bs",
    "cash_and_equivalents": "cash_st_inv",
}

ENTERPRISE_VALUE_COLUMNS = [
    "enterprise_value_id",
    "source",
    "market_cap_source",
    "market_cap_id",
    "security_id",
    "symbol",
    "trade_date",
    "period_start",
    "period_end",
    "fiscal_year",
    "fiscal_period",
    "price",
    "share_count",
    "share_count_type_used",
    "market_cap",
    "total_debt",
    "preferred_equity",
    "minority_interest",
    "cash_and_equivalents",
    "enterprise_value",
    "is_latest_revision",
    "as_of_date",
    "available_at",
    "market_cap_available_at",
    "price_available_at",
    "share_available_at",
    "total_debt_available_at",
    "preferred_equity_available_at",
    "minority_interest_available_at",
    "cash_and_equivalents_available_at",
    "input_codes_json",
    "input_lineage_json",
    "formula_version",
    "run_id",
]


@dataclass(frozen=True)
class EnterpriseValueOptions:
    source: str = DEFAULT_ENTERPRISE_VALUE_SOURCE
    market_cap_sources: tuple[str, ...] | None = None
    symbols: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    run_id: str | None = None


def _enterprise_value_id(source: str, market_cap_source: str, security_id: str, trade_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, market_cap_source, security_id, trade_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _normalized_symbols(symbols: tuple[str, ...] | list[str] | None) -> list[str]:
    if symbols is None:
        return []
    return sorted({str(symbol).strip().upper() for symbol in symbols if str(symbol).strip()})


def _empty_enterprise_value_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ENTERPRISE_VALUE_COLUMNS)


def _present(value: Any) -> bool:
    try:
        return not pd.isna(value)
    except (TypeError, ValueError):
        return value is not None


def _safe_float(value: Any) -> float | None:
    if not _present(value):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _normalize_enterprise_value_inputs(inputs: pd.DataFrame) -> pd.DataFrame:
    out = inputs.copy()
    if out.empty:
        return out

    if "market_cap_source" not in out.columns:
        out["market_cap_source"] = out.get("source", DEFAULT_MARKET_CAP_SOURCE)
    if "price" not in out.columns:
        out["price"] = out.get("close")
    if "market_cap_available_at" not in out.columns:
        out["market_cap_available_at"] = out.get("available_at")
    if "price_available_at" not in out.columns:
        out["price_available_at"] = out.get("market_cap_available_at")
    if "share_available_at" not in out.columns:
        out["share_available_at"] = out.get("market_cap_available_at")
    if "period_start" not in out.columns:
        out["period_start"] = pd.NaT
    if "fiscal_year" not in out.columns:
        out["fiscal_year"] = pd.NA
    if "fiscal_period" not in out.columns:
        out["fiscal_period"] = pd.NA
    if "fundamental_sort_key" not in out.columns:
        out["fundamental_sort_key"] = ""

    for column in (
        "market_cap_id",
        "symbol",
        "share_count_type_used",
        "market_cap_input_lineage_json",
        "total_debt_id",
        "preferred_equity_id",
        "minority_interest_id",
        "cash_and_equivalents_id",
        "total_debt_source",
        "preferred_equity_source",
        "minority_interest_source",
        "cash_and_equivalents_source",
    ):
        if column not in out.columns:
            out[column] = pd.NA

    out["trade_date"] = pd.to_datetime(out["trade_date"], errors="coerce").dt.date
    out["period_start"] = pd.to_datetime(out["period_start"], errors="coerce").dt.date
    out["period_end"] = pd.to_datetime(out["period_end"], errors="coerce").dt.date
    timestamp_columns = [
        "market_cap_available_at",
        "price_available_at",
        "share_available_at",
        "total_debt_available_at",
        "preferred_equity_available_at",
        "minority_interest_available_at",
        "cash_and_equivalents_available_at",
    ]
    for column in timestamp_columns:
        out[column] = pd.to_datetime(out[column], errors="coerce")
    numeric_columns = [
        "price",
        "share_count",
        "market_cap",
        "total_debt",
        "preferred_equity",
        "minority_interest",
        "cash_and_equivalents",
    ]
    for column in numeric_columns:
        out[column] = pd.to_numeric(out[column], errors="coerce")

    required = [
        "security_id",
        "market_cap_source",
        "trade_date",
        "period_end",
        "market_cap",
        "total_debt",
        "preferred_equity",
        "minority_interest",
        "cash_and_equivalents",
        *timestamp_columns,
    ]
    out = out.dropna(subset=required)
    if out.empty:
        return out
    return out[out["period_end"] <= out["trade_date"]].copy()


def _select_latest_enterprise_value_inputs(inputs: pd.DataFrame) -> pd.DataFrame:
    out = _normalize_enterprise_value_inputs(inputs)
    if out.empty:
        return out
    component_av = [
        "market_cap_available_at",
        "total_debt_available_at",
        "preferred_equity_available_at",
        "minority_interest_available_at",
        "cash_and_equivalents_available_at",
    ]
    out["fundamental_available_at"] = out[component_av[1:]].max(axis=1)
    # Prefer periods already visible as of trade_date over a not-yet-filed later
    # period; only fall back to an unavailable period when no available one exists
    # for that trade_date (mirrors the `matched` CTE ranking in load_enterprise_value_inputs).
    out["fundamental_is_available"] = out["fundamental_available_at"] <= out["trade_date"]
    key_columns = ["market_cap_source", "security_id", "trade_date"]
    out = out.sort_values(
        key_columns
        + ["fundamental_is_available", "period_end", "fundamental_available_at", "fundamental_sort_key"],
        ascending=[True, True, True, False, False, False, False],
        kind="mergesort",
    )
    out = out.drop(columns=["fundamental_is_available"])
    return out.drop_duplicates(key_columns, keep="first").reset_index(drop=True)


def _assert_non_negative_component(row: pd.Series, column: str) -> float:
    value = _safe_float(row.get(column))
    if value is None or value < 0:
        raise ValueError(f"{column} must be a non-negative enterprise-value component")
    return value


def _component_lineage(row: pd.Series, component: str) -> dict[str, Any]:
    if component == "market_cap":
        return {
            "table": "market_cap",
            "source": row.get("market_cap_source"),
            "market_cap_id": row.get("market_cap_id"),
            "security_id": row.get("security_id"),
            "trade_date": row.get("trade_date"),
            "value": row.get("market_cap"),
            "available_at": row.get("market_cap_available_at"),
            "sign": "add",
            "upstream_lineage": row.get("market_cap_input_lineage_json"),
        }
    return {
        "table": "fundamental_statement_points",
        "statement_point_id": row.get(f"{component}_id"),
        "source": row.get(f"{component}_source"),
        "canonical_metric": COMPONENT_METRICS[component],
        "period_end": row.get("period_end"),
        "value": row.get(component),
        "available_at": row.get(f"{component}_available_at"),
        "sign": "subtract" if component == "cash_and_equivalents" else "add",
    }


def _input_lineage_json(row: pd.Series) -> str:
    return json_dumps(
        {
            "formula": (
                "enterprise_value = market_cap + total_debt + preferred_equity "
                "+ minority_interest - cash_and_equivalents"
            ),
            "components": {
                component: _component_lineage(row, component)
                for component in (
                    "market_cap",
                    "total_debt",
                    "preferred_equity",
                    "minority_interest",
                    "cash_and_equivalents",
                )
            },
        }
    )


def compute_enterprise_value_rows(
    inputs: pd.DataFrame,
    *,
    source: str = DEFAULT_ENTERPRISE_VALUE_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: PIT market cap + balance-sheet components -> EV rows."""

    selected = _select_latest_enterprise_value_inputs(inputs)
    if selected.empty:
        return _empty_enterprise_value_frame()

    records: list[dict[str, Any]] = []
    for _, row in selected.iterrows():
        market_cap = _assert_non_negative_component(row, "market_cap")
        total_debt = _assert_non_negative_component(row, "total_debt")
        preferred_equity = _assert_non_negative_component(row, "preferred_equity")
        minority_interest = _assert_non_negative_component(row, "minority_interest")
        cash_and_equivalents = _assert_non_negative_component(row, "cash_and_equivalents")
        enterprise_value = market_cap + total_debt + preferred_equity + minority_interest - cash_and_equivalents
        available_at = max(
            row["market_cap_available_at"],
            row["total_debt_available_at"],
            row["preferred_equity_available_at"],
            row["minority_interest_available_at"],
            row["cash_and_equivalents_available_at"],
        )
        records.append(
            {
                "enterprise_value_id": _enterprise_value_id(
                    source,
                    str(row["market_cap_source"]),
                    str(row["security_id"]),
                    row["trade_date"],
                ),
                "source": source,
                "market_cap_source": row["market_cap_source"],
                "market_cap_id": row.get("market_cap_id"),
                "security_id": row["security_id"],
                "symbol": row.get("symbol"),
                "trade_date": row["trade_date"],
                "period_start": row.get("period_start"),
                "period_end": row["period_end"],
                "fiscal_year": row.get("fiscal_year"),
                "fiscal_period": row.get("fiscal_period"),
                "price": row.get("price"),
                "share_count": row.get("share_count"),
                "share_count_type_used": row.get("share_count_type_used"),
                "market_cap": market_cap,
                "total_debt": total_debt,
                "preferred_equity": preferred_equity,
                "minority_interest": minority_interest,
                "cash_and_equivalents": cash_and_equivalents,
                "enterprise_value": enterprise_value,
                "is_latest_revision": True,
                "as_of_date": row["trade_date"],
                "available_at": available_at,
                "market_cap_available_at": row["market_cap_available_at"],
                "price_available_at": row["price_available_at"],
                "share_available_at": row["share_available_at"],
                "total_debt_available_at": row["total_debt_available_at"],
                "preferred_equity_available_at": row["preferred_equity_available_at"],
                "minority_interest_available_at": row["minority_interest_available_at"],
                "cash_and_equivalents_available_at": row["cash_and_equivalents_available_at"],
                "input_codes_json": json_dumps(COMPONENT_CODE_MAP),
                "input_lineage_json": _input_lineage_json(row),
                "formula_version": "enterprise_value_v1",
                "run_id": run_id,
            }
        )
    return pd.DataFrame(records, columns=ENTERPRISE_VALUE_COLUMNS)


def load_enterprise_value_inputs(store: DuckDBStore, options: EnterpriseValueOptions) -> pd.DataFrame:
    registered: list[str] = []
    joins: list[str] = []
    params: list[object] = []
    symbols = _normalized_symbols(options.symbols)
    if options.symbols is not None and not symbols:
        return pd.DataFrame()
    if options.market_cap_sources:
        store.con.register(
            "enterprise_value_market_cap_source_filter",
            pd.DataFrame({"market_cap_source": list(options.market_cap_sources)}),
        )
        registered.append("enterprise_value_market_cap_source_filter")
        joins.append("JOIN enterprise_value_market_cap_source_filter mcsf ON mcsf.market_cap_source = m.source")
    if symbols:
        store.con.register("enterprise_value_symbol_filter", pd.DataFrame({"symbol": symbols}))
        registered.append("enterprise_value_symbol_filter")
        joins.append("JOIN enterprise_value_symbol_filter evsf ON evsf.symbol = m.symbol")

    date_predicates = []
    if options.start_date is not None:
        date_predicates.append("m.trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("m.trade_date <= ?")
        params.append(options.end_date)
    where_extra = f" AND {' AND '.join(date_predicates)}" if date_predicates else ""
    join_extra = "\n            ".join(joins)

    sql = f"""
        WITH market_caps AS (
            SELECT
                m.market_cap_id,
                m.source AS market_cap_source,
                m.security_id,
                m.symbol,
                m.trade_date,
                m.close AS price,
                m.share_count,
                m.share_count_type_used,
                m.market_cap,
                m.available_at AS market_cap_available_at,
                m.price_available_at,
                m.share_available_at,
                m.input_lineage_json AS market_cap_input_lineage_json
            FROM market_cap m
            {join_extra}
            WHERE m.is_latest_revision
              AND m.security_id IS NOT NULL
              AND m.trade_date IS NOT NULL
              AND m.market_cap IS NOT NULL
              AND m.market_cap >= 0
              AND m.available_at IS NOT NULL
              {where_extra}
        ),
        ranked AS (
            SELECT
                s.*,
                row_number() OVER (
                    PARTITION BY s.security_id, s.period_end, s.canonical_metric
                    ORDER BY
                        s.available_at DESC NULLS LAST,
                        s.revision_sequence DESC,
                        s.source DESC,
                        s.statement_point_id DESC
                ) AS rn
            FROM fundamental_statement_points s
            WHERE s.is_latest_revision
              AND s.period_type = 'instant'
              AND s.canonical_metric IN ('total_debt', 'pref_stock', 'minority_int_bs', 'cash_st_inv')
              AND s.value IS NOT NULL
              AND s.value >= 0
              AND s.available_at IS NOT NULL
        ),
        fundamentals AS (
            SELECT
                r.security_id,
                max(r.symbol) AS fundamental_symbol,
                r.period_end,
                min(r.period_start) AS period_start,
                max(r.fiscal_year) AS fiscal_year,
                max(r.fiscal_period) AS fiscal_period,
                max(CASE WHEN r.canonical_metric = 'total_debt' THEN r.value END) AS total_debt,
                max(CASE WHEN r.canonical_metric = 'total_debt' THEN r.available_at END) AS total_debt_available_at,
                max(CASE WHEN r.canonical_metric = 'total_debt' THEN r.statement_point_id END) AS total_debt_id,
                max(CASE WHEN r.canonical_metric = 'total_debt' THEN r.source END) AS total_debt_source,
                max(CASE WHEN r.canonical_metric = 'pref_stock' THEN r.value END) AS preferred_equity,
                max(CASE WHEN r.canonical_metric = 'pref_stock' THEN r.available_at END) AS preferred_equity_available_at,
                max(CASE WHEN r.canonical_metric = 'pref_stock' THEN r.statement_point_id END) AS preferred_equity_id,
                max(CASE WHEN r.canonical_metric = 'pref_stock' THEN r.source END) AS preferred_equity_source,
                max(CASE WHEN r.canonical_metric = 'minority_int_bs' THEN r.value END) AS minority_interest,
                max(CASE WHEN r.canonical_metric = 'minority_int_bs' THEN r.available_at END) AS minority_interest_available_at,
                max(CASE WHEN r.canonical_metric = 'minority_int_bs' THEN r.statement_point_id END) AS minority_interest_id,
                max(CASE WHEN r.canonical_metric = 'minority_int_bs' THEN r.source END) AS minority_interest_source,
                max(CASE WHEN r.canonical_metric = 'cash_st_inv' THEN r.value END) AS cash_and_equivalents,
                max(CASE WHEN r.canonical_metric = 'cash_st_inv' THEN r.available_at END) AS cash_and_equivalents_available_at,
                max(CASE WHEN r.canonical_metric = 'cash_st_inv' THEN r.statement_point_id END) AS cash_and_equivalents_id,
                max(CASE WHEN r.canonical_metric = 'cash_st_inv' THEN r.source END) AS cash_and_equivalents_source
            FROM ranked r
            WHERE r.rn = 1
            GROUP BY r.security_id, r.period_end
        ),
        complete_fundamentals AS (
            SELECT
                f.*,
                greatest(
                    coalesce(f.total_debt_available_at, TIMESTAMP '1900-01-01'),
                    coalesce(f.preferred_equity_available_at, TIMESTAMP '1900-01-01'),
                    coalesce(f.minority_interest_available_at, TIMESTAMP '1900-01-01'),
                    coalesce(f.cash_and_equivalents_available_at, TIMESTAMP '1900-01-01')
                ) AS fundamental_available_at,
                concat_ws(
                    '|',
                    coalesce(f.total_debt_id, ''),
                    coalesce(f.preferred_equity_id, ''),
                    coalesce(f.minority_interest_id, ''),
                    coalesce(f.cash_and_equivalents_id, '')
                ) AS fundamental_sort_key
            FROM fundamentals f
            WHERE f.total_debt IS NOT NULL
              AND f.preferred_equity IS NOT NULL
              AND f.minority_interest IS NOT NULL
              AND f.cash_and_equivalents IS NOT NULL
        ),
        matched AS (
            SELECT
                mc.*,
                f.period_start,
                f.period_end,
                f.fiscal_year,
                f.fiscal_period,
                f.total_debt,
                f.total_debt_available_at,
                f.total_debt_id,
                f.total_debt_source,
                f.preferred_equity,
                f.preferred_equity_available_at,
                f.preferred_equity_id,
                f.preferred_equity_source,
                f.minority_interest,
                f.minority_interest_available_at,
                f.minority_interest_id,
                f.minority_interest_source,
                f.cash_and_equivalents,
                f.cash_and_equivalents_available_at,
                f.cash_and_equivalents_id,
                f.cash_and_equivalents_source,
                f.fundamental_available_at,
                f.fundamental_sort_key,
                row_number() OVER (
                    PARTITION BY mc.market_cap_source, mc.security_id, mc.trade_date
                    ORDER BY
                        (f.fundamental_available_at <= mc.trade_date) DESC,
                        f.period_end DESC,
                        f.fundamental_available_at DESC,
                        f.fundamental_sort_key DESC
                ) AS ev_period_rn
            FROM market_caps mc
            JOIN complete_fundamentals f
              ON f.security_id = mc.security_id
             AND f.period_end <= mc.trade_date
        )
        SELECT * EXCLUDE (ev_period_rn)
        FROM matched
        WHERE ev_period_rn = 1
    """
    try:
        return store.con.execute(sql, params).df()
    finally:
        for relation in registered:
            store.con.unregister(relation)


def _delete_enterprise_value_scope(
    store: DuckDBStore,
    options: EnterpriseValueOptions,
) -> None:
    symbols = _normalized_symbols(options.symbols)
    if options.symbols is not None and not symbols:
        return
    predicates = ["source = ?"]
    params: list[object] = [options.source]
    if options.market_cap_sources:
        placeholders = ", ".join("?" for _ in options.market_cap_sources)
        predicates.append(f"market_cap_source IN ({placeholders})")
        params.extend(options.market_cap_sources)
    if symbols:
        placeholders = ", ".join("?" for _ in symbols)
        predicates.append(f"symbol IN ({placeholders})")
        params.extend(symbols)
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    store.con.execute(f"DELETE FROM enterprise_value WHERE {' AND '.join(predicates)}", params)


def refresh_enterprise_value(store: DuckDBStore, options: EnterpriseValueOptions | None = None) -> int:
    options = options or EnterpriseValueOptions()
    store.initialize()
    inputs = load_enterprise_value_inputs(store, options)
    rows = compute_enterprise_value_rows(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        _delete_enterprise_value_scope(store, options)
        if not rows.empty:
            insert_frame(store, rows, "enterprise_value", "enterprise_value_insert")
    return int(len(rows))


class EnterpriseValueDataset(Dataset):
    dataset_id = "enterprise_value"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EnterpriseValueOptions) -> DatasetLoadResult:
        rows = refresh_enterprise_value(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="enterprise_value",
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
            details={"formula": "market_cap + total_debt + preferred_equity + minority_interest - cash"},
        )
