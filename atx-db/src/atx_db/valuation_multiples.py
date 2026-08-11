"""PF-S6 S6-1: PIT-safe market capitalization.

Market cap is struck as the raw daily close times the latest applicable share
count, then made visible when both the price and selected share vintage are
available. This deliberately uses ``equity_daily_bars.close`` rather than
``adjusted_close``: market capitalization is a same-day level, not a
back-adjusted return series.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check, replace_by_relation


SOURCE_NAME = "Derived market capitalization"
VALUATION_SOURCE_NAME = "Derived point-in-time valuation multiples"
DEFAULT_MARKET_CAP_SOURCE = "derived_market_cap_v1"
DEFAULT_VALUATION_MULTIPLES_SOURCE = "derived_valuation_multiples_v1"
DEFAULT_VALUATION_STALE_GAP_DAYS = 5
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

VALUATION_MULTIPLE_COLUMNS = [
    "valuation_multiple_id",
    "source",
    "market_cap_source",
    "market_cap_id",
    "security_id",
    "symbol",
    "trade_date",
    "formula_code",
    "category",
    "kind",
    "unit",
    "period_start",
    "period_end",
    "fiscal_year",
    "fiscal_period",
    "value",
    "numerator_code",
    "numerator_value",
    "denominator_code",
    "denominator_value",
    "price",
    "market_cap",
    "enterprise_value",
    "is_meaningful",
    "is_latest_revision",
    "as_of_date",
    "available_at",
    "market_cap_available_at",
    "price_available_at",
    "input_codes_json",
    "input_lineage_json",
    "run_id",
]

VALUATION_TTM_INPUTS = {
    "rev": "revenue",
    "ni": "net_income",
    "oi": "operating_income",
    "ocf": "operating_cash_flow",
    "capex": "capital_expenditures",
    "div": "dividends_paid",
    "repurch": "share_repurchases",
}
VALUATION_BALANCE_INPUTS = {
    "assets": "assets",
    "equity": "stockholders_equity",
}
VALUATION_XBRL_INSTANT_INPUTS = {
    "long_term_debt": "long_term_debt",
    "cash_and_equivalents": "cash_and_equivalents",
}
VALUATION_XBRL_DURATION_INPUTS = {
    "depreciation_amortization": "depreciation_amortization",
}

VALUATION_INPUT_CODES = {
    "price": "market_cap.close",
    "market_cap": "market_cap.market_cap",
    "enterprise_value": "valuation_multiples.enterprise_value",
    "rev": "fundamental_ttm_points.revenue",
    "ni": "fundamental_ttm_points.net_income",
    "oi": "fundamental_ttm_points.operating_income",
    "ocf": "fundamental_ttm_points.operating_cash_flow",
    "capex": "fundamental_ttm_points.capital_expenditures",
    "div": "fundamental_ttm_points.dividends_paid",
    "repurch": "fundamental_ttm_points.share_repurchases",
    "assets": "fundamental_statement_points.assets",
    "equity": "fundamental_statement_points.stockholders_equity",
    "long_term_debt": "fundamental_xbrl_metric.long_term_debt",
    "cash_and_equivalents": "fundamental_xbrl_metric.cash_and_equivalents",
    "depreciation_amortization": "fundamental_xbrl_metric.depreciation_amortization",
}


@dataclass(frozen=True)
class ValuationFormulaDef:
    code: str
    category: str
    kind: str
    unit: str
    numerator_code: str
    denominator_code: str
    input_keys: tuple[str, ...]


VALUATION_FORMULA_DEFS: tuple[ValuationFormulaDef, ...] = (
    ValuationFormulaDef(
        "price_to_earnings",
        "valuation",
        "ratio",
        "ratio",
        "market_cap",
        "net_income_ttm",
        ("market_cap", "ni"),
    ),
    ValuationFormulaDef(
        "price_to_book",
        "valuation",
        "ratio",
        "ratio",
        "market_cap",
        "stockholders_equity",
        ("market_cap", "equity"),
    ),
    ValuationFormulaDef(
        "price_to_sales",
        "valuation",
        "ratio",
        "ratio",
        "market_cap",
        "revenue_ttm",
        ("market_cap", "rev"),
    ),
    ValuationFormulaDef(
        "enterprise_value",
        "valuation",
        "difference",
        "currency",
        "market_cap_plus_long_term_debt",
        "cash_and_equivalents",
        ("market_cap", "long_term_debt", "cash_and_equivalents"),
    ),
    ValuationFormulaDef(
        "ev_to_ebitda",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "ebitda",
        ("market_cap", "long_term_debt", "cash_and_equivalents", "oi", "depreciation_amortization"),
    ),
    ValuationFormulaDef(
        "ev_to_sales",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "revenue_ttm",
        ("market_cap", "long_term_debt", "cash_and_equivalents", "rev"),
    ),
    ValuationFormulaDef(
        "fcf_yield",
        "valuation",
        "ratio",
        "ratio",
        "free_cash_flow_ttm",
        "market_cap",
        ("ocf", "capex", "market_cap"),
    ),
    ValuationFormulaDef(
        "earnings_yield",
        "valuation",
        "ratio",
        "ratio",
        "net_income_ttm",
        "market_cap",
        ("ni", "market_cap"),
    ),
    ValuationFormulaDef(
        "dividend_yield",
        "valuation",
        "ratio",
        "ratio",
        "abs_dividends_paid_ttm",
        "market_cap",
        ("div", "market_cap"),
    ),
    ValuationFormulaDef(
        "price_to_cash_flow",
        "valuation",
        "ratio",
        "ratio",
        "market_cap",
        "operating_cash_flow_ttm",
        ("market_cap", "ocf"),
    ),
    ValuationFormulaDef(
        "price_to_free_cash_flow",
        "valuation",
        "ratio",
        "ratio",
        "market_cap",
        "free_cash_flow_ttm",
        ("market_cap", "ocf", "capex"),
    ),
    ValuationFormulaDef(
        "ev_to_ebit",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "operating_income",
        ("enterprise_value", "oi"),
    ),
    ValuationFormulaDef(
        "ev_to_fcf",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "free_cash_flow_ttm",
        ("enterprise_value", "ocf", "capex"),
    ),
    ValuationFormulaDef(
        "ev_to_assets",
        "valuation",
        "ratio",
        "ratio",
        "enterprise_value",
        "assets",
        ("enterprise_value", "assets"),
    ),
    ValuationFormulaDef(
        "buyback_yield",
        "valuation",
        "ratio",
        "ratio",
        "abs_share_repurchases_ttm",
        "market_cap",
        ("repurch", "market_cap"),
    ),
    ValuationFormulaDef(
        "shareholder_yield",
        "valuation",
        "ratio",
        "ratio",
        "abs_dividends_plus_repurchases_ttm",
        "market_cap",
        ("div", "repurch", "market_cap"),
    ),
)


@dataclass(frozen=True)
class MarketCapOptions:
    source: str = DEFAULT_MARKET_CAP_SOURCE
    price_sources: tuple[str, ...] | None = None
    symbols: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    run_id: str | None = None


@dataclass(frozen=True)
class ValuationMultiplesOptions:
    source: str = DEFAULT_VALUATION_MULTIPLES_SOURCE
    market_cap_sources: tuple[str, ...] | None = None
    symbols: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    run_id: str | None = None
    coverage_as_of_ts: dt.datetime | None = None
    stale_price_fundamental_gap_days: int = DEFAULT_VALUATION_STALE_GAP_DAYS


def _market_cap_id(source: str, security_id: str, trade_date) -> str:
    payload = "|".join(str(part) for part in (source, security_id, trade_date))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _valuation_multiple_id(
    source: str,
    market_cap_source: str,
    security_id: str,
    trade_date: Any,
    formula_code: str,
) -> str:
    payload = "|".join(str(part) for part in (source, market_cap_source, security_id, trade_date, formula_code))
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
    out = out[
        (out["close"] > 0)
        & (out["share_count"] > 0)
        & out["close"].map(math.isfinite)
        & out["share_count"].map(math.isfinite)
    ].copy()
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
        for security_id, trade_date in zip(
            out["security_id"], out["trade_date"], strict=True
        )
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
             AND s.share_count > 0
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
    return len(rows)


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


def _safe_timestamp(value: Any) -> pd.Timestamp | None:
    if not _present(value):
        return None
    ts = pd.Timestamp(value)
    return None if pd.isna(ts) else ts


def _safe_json_loads(value: Any) -> Any:
    if not _present(value):
        return None
    if not isinstance(value, str):
        return value
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return value


def _fundamental_av_columns() -> list[str]:
    keys = (
        *VALUATION_TTM_INPUTS,
        *VALUATION_BALANCE_INPUTS,
        *VALUATION_XBRL_INSTANT_INPUTS,
        *VALUATION_XBRL_DURATION_INPUTS,
    )
    return [f"{key}_av" for key in keys]


def _synthesize_enterprise_value_inputs(out: pd.DataFrame) -> pd.DataFrame:
    if "enterprise_value" not in out.columns:
        out["enterprise_value"] = pd.NA
    if "enterprise_value_av" not in out.columns:
        out["enterprise_value_av"] = pd.NaT

    component_columns = ("market_cap", "long_term_debt", "cash_and_equivalents")
    if all(column in out.columns for column in component_columns):
        components_present = out[list(component_columns)].notna().all(axis=1)
        computed = out["market_cap"] + out["long_term_debt"] - out["cash_and_equivalents"]
        out.loc[out["enterprise_value"].isna() & components_present, "enterprise_value"] = computed[components_present]

    av_columns = ("market_cap_available_at", "long_term_debt_av", "cash_and_equivalents_av")
    if all(column in out.columns for column in av_columns):
        av_present = out[list(av_columns)].notna().all(axis=1)
        computed_av = out[list(av_columns)].max(axis=1)
        out.loc[out["enterprise_value"].notna() & out["enterprise_value_av"].isna() & av_present, "enterprise_value_av"] = (
            computed_av[av_present]
        )
    out["enterprise_value"] = pd.to_numeric(out["enterprise_value"], errors="coerce")
    out["enterprise_value_av"] = pd.to_datetime(out["enterprise_value_av"], errors="coerce")
    return out


def _normalize_valuation_inputs(inputs: pd.DataFrame) -> pd.DataFrame:
    out = inputs.copy()
    if "market_cap_source" not in out.columns:
        out["market_cap_source"] = out.get("source", DEFAULT_MARKET_CAP_SOURCE)
    if "price" not in out.columns:
        out["price"] = out.get("close")
    if "market_cap_available_at" not in out.columns:
        out["market_cap_available_at"] = out.get("available_at")
    if "price_available_at" not in out.columns:
        out["price_available_at"] = out.get("market_cap_available_at")
    if "fundamental_period_end" in out.columns and "period_end" not in out.columns:
        out["period_end"] = out["fundamental_period_end"]
    if "fundamental_period_start" in out.columns and "period_start" not in out.columns:
        out["period_start"] = out["fundamental_period_start"]
    if "market_cap_id" not in out.columns:
        out["market_cap_id"] = pd.NA
    if "symbol" not in out.columns:
        out["symbol"] = pd.NA
    if "period_start" not in out.columns:
        out["period_start"] = pd.NaT
    if "fiscal_year" not in out.columns:
        out["fiscal_year"] = pd.NA
    if "fiscal_period" not in out.columns:
        out["fiscal_period"] = pd.NA
    if "fundamental_sort_key" not in out.columns:
        out["fundamental_sort_key"] = ""

    out["trade_date"] = pd.to_datetime(out["trade_date"], errors="coerce").dt.date
    out["period_end"] = pd.to_datetime(out["period_end"], errors="coerce").dt.date
    out["period_start"] = pd.to_datetime(out["period_start"], errors="coerce").dt.date
    for column in (
        "market_cap_available_at",
        "price_available_at",
        "fundamental_available_at",
        "enterprise_value_av",
        *_fundamental_av_columns(),
    ):
        if column in out.columns:
            out[column] = pd.to_datetime(out[column], errors="coerce")
    for column in (
        "price",
        "market_cap",
        "enterprise_value",
        "rev",
        "ni",
        "oi",
        "ocf",
        "capex",
        "div",
        "repurch",
        "assets",
        "equity",
        "long_term_debt",
        "cash_and_equivalents",
        "depreciation_amortization",
    ):
        if column in out.columns:
            out[column] = pd.to_numeric(out[column], errors="coerce")

    out = _synthesize_enterprise_value_inputs(out)

    if "fundamental_available_at" not in out.columns:
        av_columns = [column for column in _fundamental_av_columns() if column in out.columns]
        out["fundamental_available_at"] = out[av_columns].max(axis=1) if av_columns else pd.NaT

    out = out.dropna(
        subset=["security_id", "market_cap_source", "trade_date", "period_end", "price", "market_cap", "market_cap_available_at"]
    )
    if out.empty:
        return out
    return out[out["period_end"] <= out["trade_date"]].copy()


def _select_latest_valuation_inputs(inputs: pd.DataFrame) -> pd.DataFrame:
    out = _normalize_valuation_inputs(inputs)
    if out.empty:
        return out
    key_columns = ["market_cap_source", "security_id", "trade_date"]
    out = out.sort_values(
        [*key_columns, "period_end", "fundamental_available_at", "fundamental_sort_key"],
        ascending=[True, True, True, False, False, False],
        kind="mergesort",
    )
    return out.drop_duplicates(key_columns, keep="first").reset_index(drop=True)


def _input_available_at(rec: dict[str, Any], key: str) -> pd.Timestamp | None:
    if key == "market_cap":
        column = "market_cap_available_at"
    elif key == "enterprise_value":
        column = "enterprise_value_av"
    else:
        column = f"{key}_av"
    return _safe_timestamp(rec.get(column))


def _max_input_available_at(rec: dict[str, Any], keys: tuple[str, ...]) -> pd.Timestamp | None:
    values = [_input_available_at(rec, key) for key in keys]
    if any(value is None for value in values):
        return None
    return max(value for value in values if value is not None)


def _market_cap_components(rec: dict[str, Any]) -> tuple[float, float, float] | None:
    market_cap = _safe_float(rec.get("market_cap"))
    debt = _safe_float(rec.get("long_term_debt"))
    cash = _safe_float(rec.get("cash_and_equivalents"))
    if market_cap is None or debt is None or cash is None:
        return None
    return market_cap, debt, cash


def _enterprise_value(rec: dict[str, Any]) -> float | None:
    components = _market_cap_components(rec)
    if components is None:
        return None
    market_cap, debt, cash = components
    return market_cap + debt - cash


def _ratio_values(
    numerator: float | None,
    denominator: float | None,
    *,
    meaningful_gate: bool = True,
) -> tuple[float, bool] | None:
    if numerator is None or denominator is None:
        return None
    if denominator == 0:
        return None
    return numerator / denominator, bool(meaningful_gate and denominator > 0)


def _formula_values(
    definition: ValuationFormulaDef,
    rec: dict[str, Any],
) -> tuple[float, float, float, float | None, bool] | None:
    market_cap = _safe_float(rec.get("market_cap"))
    if definition.code == "price_to_earnings":
        num, den = market_cap, _safe_float(rec.get("ni"))
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "price_to_book":
        num, den = market_cap, _safe_float(rec.get("equity"))
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "price_to_sales":
        num, den = market_cap, _safe_float(rec.get("rev"))
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "enterprise_value":
        components = _market_cap_components(rec)
        if components is None:
            return None
        market_cap_value, debt, cash = components
        numerator = market_cap_value + debt
        enterprise_value = numerator - cash
        return enterprise_value, numerator, cash, enterprise_value, bool(market_cap_value > 0)
    if definition.code == "ev_to_ebitda":
        enterprise_value = _enterprise_value(rec)
        ebitda = None
        oi = _safe_float(rec.get("oi"))
        depreciation = _safe_float(rec.get("depreciation_amortization"))
        if oi is not None and depreciation is not None:
            ebitda = oi + depreciation
        ratio = _ratio_values(enterprise_value, ebitda, meaningful_gate=bool((market_cap or 0) > 0))
        return None if ratio is None else (ratio[0], enterprise_value, ebitda, enterprise_value, ratio[1])
    if definition.code == "ev_to_sales":
        enterprise_value = _enterprise_value(rec)
        den = _safe_float(rec.get("rev"))
        ratio = _ratio_values(enterprise_value, den, meaningful_gate=bool((market_cap or 0) > 0))
        return None if ratio is None else (ratio[0], enterprise_value, den, enterprise_value, ratio[1])
    if definition.code == "fcf_yield":
        ocf = _safe_float(rec.get("ocf"))
        capex = _safe_float(rec.get("capex"))
        denominator = market_cap
        numerator = None if ocf is None or capex is None else ocf + capex
        ratio = _ratio_values(numerator, denominator)
        return None if ratio is None else (ratio[0], numerator, denominator, None, ratio[1])
    if definition.code == "earnings_yield":
        num, den = _safe_float(rec.get("ni")), market_cap
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "dividend_yield":
        div = _safe_float(rec.get("div"))
        num, den = (None if div is None else abs(div)), market_cap
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "price_to_cash_flow":
        num, den = market_cap, _safe_float(rec.get("ocf"))
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "price_to_free_cash_flow":
        ocf = _safe_float(rec.get("ocf"))
        capex = _safe_float(rec.get("capex"))
        num = market_cap
        den = None if ocf is None or capex is None else ocf + capex
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "ev_to_ebit":
        enterprise_value = _safe_float(rec.get("enterprise_value"))
        den = _safe_float(rec.get("oi"))
        ratio = _ratio_values(enterprise_value, den, meaningful_gate=bool((market_cap or 0) > 0))
        return None if ratio is None else (ratio[0], enterprise_value, den, enterprise_value, ratio[1])
    if definition.code == "ev_to_fcf":
        enterprise_value = _safe_float(rec.get("enterprise_value"))
        ocf = _safe_float(rec.get("ocf"))
        capex = _safe_float(rec.get("capex"))
        den = None if ocf is None or capex is None else ocf + capex
        ratio = _ratio_values(enterprise_value, den, meaningful_gate=bool((market_cap or 0) > 0))
        return None if ratio is None else (ratio[0], enterprise_value, den, enterprise_value, ratio[1])
    if definition.code == "ev_to_assets":
        enterprise_value = _safe_float(rec.get("enterprise_value"))
        den = _safe_float(rec.get("assets"))
        ratio = _ratio_values(enterprise_value, den, meaningful_gate=bool((market_cap or 0) > 0))
        return None if ratio is None else (ratio[0], enterprise_value, den, enterprise_value, ratio[1])
    if definition.code == "buyback_yield":
        repurch = _safe_float(rec.get("repurch"))
        num, den = (None if repurch is None else abs(repurch)), market_cap
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    if definition.code == "shareholder_yield":
        div = _safe_float(rec.get("div"))
        repurch = _safe_float(rec.get("repurch"))
        num = None if div is None or repurch is None else abs(div) + abs(repurch)
        den = market_cap
        ratio = _ratio_values(num, den)
        return None if ratio is None else (ratio[0], num, den, None, ratio[1])
    raise KeyError(f"unknown valuation formula {definition.code!r}")


def _input_codes_json(definition: ValuationFormulaDef) -> str:
    keys = ("price", *definition.input_keys)
    return json_dumps({key: VALUATION_INPUT_CODES[key] for key in keys if key in VALUATION_INPUT_CODES})


def _input_lineage_for_key(rec: dict[str, Any], key: str) -> dict[str, Any]:
    if key == "market_cap":
        return {
            "table": "market_cap",
            "source": rec.get("market_cap_source"),
            "market_cap_id": rec.get("market_cap_id"),
            "security_id": rec.get("security_id"),
            "trade_date": rec.get("trade_date"),
            "available_at": rec.get("market_cap_available_at"),
            "price_available_at": rec.get("price_available_at"),
            "upstream_lineage": _safe_json_loads(rec.get("market_cap_input_lineage_json")),
        }
    if key == "enterprise_value":
        return {
            "table": "derived",
            "formula": "market_cap + long_term_debt - cash_and_equivalents",
            "value": rec.get("enterprise_value"),
            "available_at": rec.get("enterprise_value_av"),
            "components": {
                "market_cap": _input_lineage_for_key(rec, "market_cap"),
                "long_term_debt": _input_lineage_for_key(rec, "long_term_debt"),
                "cash_and_equivalents": _input_lineage_for_key(rec, "cash_and_equivalents"),
            },
        }
    if key in VALUATION_TTM_INPUTS:
        return {
            "table": "fundamental_ttm_points",
            "ttm_point_id": rec.get(f"{key}_id"),
            "source": rec.get(f"{key}_source"),
            "canonical_metric": VALUATION_TTM_INPUTS[key],
            "period_end": rec.get("period_end"),
            "available_at": rec.get(f"{key}_av"),
        }
    if key in VALUATION_BALANCE_INPUTS:
        return {
            "table": "fundamental_statement_points",
            "statement_point_id": rec.get(f"{key}_id"),
            "source": rec.get(f"{key}_source"),
            "canonical_metric": VALUATION_BALANCE_INPUTS[key],
            "period_end": rec.get("period_end"),
            "available_at": rec.get(f"{key}_av"),
        }
    if key in VALUATION_XBRL_INSTANT_INPUTS or key in VALUATION_XBRL_DURATION_INPUTS:
        metric = {**VALUATION_XBRL_INSTANT_INPUTS, **VALUATION_XBRL_DURATION_INPUTS}[key]
        return {
            "table": "fundamental_xbrl_metric",
            "metric_id": rec.get(f"{key}_id"),
            "source": rec.get(f"{key}_source"),
            "canonical_metric": metric,
            "period_end": rec.get("period_end"),
            "available_at": rec.get(f"{key}_av"),
        }
    return {"input_key": key}


def _input_lineage_json(definition: ValuationFormulaDef, rec: dict[str, Any]) -> str:
    return json_dumps(
        {
            "formula_code": definition.code,
            "inputs": {
                key: _input_lineage_for_key(rec, key)
                for key in definition.input_keys
            },
        }
    )


def _valuation_record(
    definition: ValuationFormulaDef,
    rec: dict[str, Any],
    *,
    source: str,
    run_id: str | None,
    value: float,
    numerator: float,
    denominator: float,
    enterprise_value: float | None,
    is_meaningful: bool,
    available_at: pd.Timestamp,
) -> dict[str, Any]:
    trade_date = rec.get("trade_date")
    market_cap_source = rec.get("market_cap_source")
    return {
        "valuation_multiple_id": _valuation_multiple_id(
            source,
            market_cap_source,
            rec.get("security_id"),
            trade_date,
            definition.code,
        ),
        "source": source,
        "market_cap_source": market_cap_source,
        "market_cap_id": rec.get("market_cap_id"),
        "security_id": rec.get("security_id"),
        "symbol": rec.get("symbol"),
        "trade_date": trade_date,
        "formula_code": definition.code,
        "category": definition.category,
        "kind": definition.kind,
        "unit": definition.unit,
        "period_start": rec.get("period_start"),
        "period_end": rec.get("period_end"),
        "fiscal_year": rec.get("fiscal_year"),
        "fiscal_period": rec.get("fiscal_period"),
        "value": value,
        "numerator_code": definition.numerator_code,
        "numerator_value": numerator,
        "denominator_code": definition.denominator_code,
        "denominator_value": denominator,
        "price": rec.get("price"),
        "market_cap": rec.get("market_cap"),
        "enterprise_value": enterprise_value,
        "is_meaningful": is_meaningful,
        "is_latest_revision": True,
        "as_of_date": trade_date,
        "available_at": available_at,
        "market_cap_available_at": rec.get("market_cap_available_at"),
        "price_available_at": rec.get("price_available_at"),
        "input_codes_json": _input_codes_json(definition),
        "input_lineage_json": _input_lineage_json(definition, rec),
        "run_id": run_id,
    }


def compute_valuation_multiple_rows(
    inputs: pd.DataFrame,
    *,
    source: str = DEFAULT_VALUATION_MULTIPLES_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: market-cap/fundamental wide frame -> valuation rows."""

    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=VALUATION_MULTIPLE_COLUMNS)

    latest = _select_latest_valuation_inputs(inputs)
    if latest.empty:
        return pd.DataFrame(columns=VALUATION_MULTIPLE_COLUMNS)

    records: list[dict[str, Any]] = []
    for rec in latest.to_dict("records"):
        for definition in VALUATION_FORMULA_DEFS:
            if not all(_safe_float(rec.get(key)) is not None for key in definition.input_keys):
                continue
            available_at = _max_input_available_at(rec, definition.input_keys)
            if available_at is None:
                continue
            values = _formula_values(definition, rec)
            if values is None:
                continue
            value, numerator, denominator, enterprise_value, is_meaningful = values
            records.append(
                _valuation_record(
                    definition,
                    rec,
                    source=source,
                    run_id=run_id,
                    value=value,
                    numerator=numerator,
                    denominator=denominator,
                    enterprise_value=enterprise_value,
                    is_meaningful=is_meaningful,
                    available_at=available_at,
                )
            )

    if not records:
        return pd.DataFrame(columns=VALUATION_MULTIPLE_COLUMNS)
    return pd.DataFrame(records, columns=VALUATION_MULTIPLE_COLUMNS)


def _valuation_pivot_case(prefix: str, value_col: str, id_col: str, metric_map: dict[str, str]) -> str:
    parts = []
    for key, metric in metric_map.items():
        parts.extend(
            [
                f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.{value_col} END) AS {key}",
                f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.available_at END) AS {key}_av",
                f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.{id_col} END) AS {key}_id",
                f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.source END) AS {key}_source",
            ]
        )
    return ",\n                ".join(parts)


def _normalized_valuation_symbols(symbols: tuple[str, ...]) -> tuple[str, ...]:
    return tuple(sorted({str(symbol).strip().upper() for symbol in symbols if str(symbol).strip()}))


def _iso_or_none(value: Any) -> str | None:
    if value is None:
        return None
    if isinstance(value, pd.Timestamp):
        return None if pd.isna(value) else value.isoformat()
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return str(value)


def _valuation_scope_predicates(
    options: ValuationMultiplesOptions,
    alias: str = "v",
) -> tuple[list[str], list[object]]:
    predicates = [f"{alias}.source = ?"]
    params: list[object] = [options.source]
    if options.symbols:
        symbols = _normalized_valuation_symbols(options.symbols)
        if not symbols:
            predicates.append("1 = 0")
        else:
            placeholders = ", ".join("?" for _ in symbols)
            predicates.append(f"upper({alias}.symbol) IN ({placeholders})")
            params.extend(symbols)
    if options.start_date is not None:
        predicates.append(f"{alias}.trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append(f"{alias}.trade_date <= ?")
        params.append(options.end_date)
    if options.market_cap_sources:
        placeholders = ", ".join("?" for _ in options.market_cap_sources)
        predicates.append(f"{alias}.market_cap_source IN ({placeholders})")
        params.extend(options.market_cap_sources)
    return predicates, params


def _valuation_quality_as_of_ts(
    store: DuckDBStore,
    options: ValuationMultiplesOptions,
    as_of_ts: dt.datetime | pd.Timestamp | None,
) -> pd.Timestamp | None:
    if as_of_ts is not None:
        return _safe_timestamp(as_of_ts)
    predicates, params = _valuation_scope_predicates(options, "v")
    observed = store.con.execute(
        f"""
        SELECT max(v.available_at)
        FROM valuation_multiples v
        WHERE {' AND '.join(predicates)}
        """,
        params,
    ).fetchone()[0]
    return _safe_timestamp(observed)


def _valuation_visible_predicates(
    options: ValuationMultiplesOptions,
    as_of_ts: pd.Timestamp | None,
    alias: str = "v",
) -> tuple[list[str], list[object]]:
    predicates, params = _valuation_scope_predicates(options, alias)
    if as_of_ts is not None:
        predicates.append(f"{alias}.available_at <= ?")
        params.append(as_of_ts.to_pydatetime())
    return predicates, params


def _valuation_fundamental_denominator_count(
    store: DuckDBStore,
    options: ValuationMultiplesOptions,
    *,
    min_period_end: dt.date | None,
    max_trade_date: dt.date | None,
    as_of_ts: pd.Timestamp | None,
) -> int:
    if min_period_end is None or max_trade_date is None or as_of_ts is None:
        return 0

    predicates = [
        "f.period_end >= ?",
        "f.period_end <= ?",
        "f.available_at <= ?",
    ]
    params: list[object] = [min_period_end, max_trade_date, as_of_ts.to_pydatetime()]
    if options.symbols:
        symbols = _normalized_valuation_symbols(options.symbols)
        if not symbols:
            predicates.append("1 = 0")
        else:
            placeholders = ", ".join("?" for _ in symbols)
            predicates.append(f"upper(f.symbol) IN ({placeholders})")
            params.extend(symbols)

    sql = f"""
        WITH fundamentals AS (
            SELECT security_id, symbol, ttm_end_date AS period_end, available_at
            FROM fundamental_ttm_points
            WHERE is_latest_revision
              AND canonical_metric IN (
                  'revenue',
                  'net_income',
                  'operating_income',
                  'operating_cash_flow',
                  'capital_expenditures',
                  'dividends_paid',
                  'share_repurchases'
              )
            UNION ALL
            SELECT security_id, symbol, period_end, available_at
            FROM fundamental_statement_points
            WHERE is_latest_revision
              AND period_type = 'instant'
              AND canonical_metric IN ('stockholders_equity', 'assets')
            UNION ALL
            SELECT security_id, symbol, period_end, available_at
            FROM fundamental_xbrl_metric
            WHERE is_latest_revision
              AND canonical_metric IN (
                  'long_term_debt',
                  'cash_and_equivalents',
                  'depreciation_amortization'
              )
        )
        SELECT count(DISTINCT f.security_id)
        FROM fundamentals f
        WHERE {' AND '.join(predicates)}
    """
    return int(store.con.execute(sql, params).fetchone()[0] or 0)


def valuation_multiples_overlap_coverage(
    store: DuckDBStore,
    options: ValuationMultiplesOptions | None = None,
    *,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    stale_gap_days: int | None = None,
) -> dict[str, object]:
    """Return PIT-visible valuation coverage over the current output scope.

    Denominator securities are counted from latest-revision, valuation-relevant
    fundamentals whose period_end falls between the first selected fundamental
    period and the last visible valuation trade date. This keeps the report on
    the true price x fundamental overlap window while still counting securities
    that had fundamentals but no valuation multiple in that window.
    """

    options = options or ValuationMultiplesOptions()
    stale_gap_days = int(
        options.stale_price_fundamental_gap_days
        if stale_gap_days is None
        else stale_gap_days
    )
    resolved_as_of_ts = _valuation_quality_as_of_ts(store, options, as_of_ts)
    predicates, params = _valuation_visible_predicates(options, resolved_as_of_ts, "v")
    row = store.con.execute(
        f"""
        SELECT
            count(*)::DOUBLE AS row_count,
            count(DISTINCT v.security_id)::DOUBLE AS security_count,
            min(v.trade_date) AS min_trade_date,
            max(v.trade_date) AS max_trade_date,
            min(v.period_end) AS min_period_end,
            max(v.period_end) AS max_period_end,
            max(v.available_at) AS max_visible_available_at,
            coalesce(sum(
                CASE
                    WHEN date_diff('day', v.period_end, v.trade_date) > ?
                    THEN 1 ELSE 0
                END
            ), 0)::DOUBLE AS stale_row_count,
            max(date_diff('day', v.period_end, v.trade_date)) AS max_gap_days
        FROM valuation_multiples v
        WHERE {' AND '.join(predicates)}
        """,
        [stale_gap_days, *params],
    ).fetchone()

    valuation_row_count = int(row[0] or 0)
    numerator = int(row[1] or 0)
    min_trade_date = row[2]
    max_trade_date = row[3]
    min_period_end = row[4]
    max_period_end = row[5]
    denominator = _valuation_fundamental_denominator_count(
        store,
        options,
        min_period_end=min_period_end,
        max_trade_date=max_trade_date,
        as_of_ts=resolved_as_of_ts,
    )
    coverage_ratio = (numerator / denominator) if denominator else None

    return {
        "source": options.source,
        "market_cap_sources": list(options.market_cap_sources or ()),
        "symbols": list(_normalized_valuation_symbols(options.symbols)) if options.symbols else [],
        "start_date": _iso_or_none(options.start_date),
        "end_date": _iso_or_none(options.end_date),
        "as_of_ts": _iso_or_none(resolved_as_of_ts),
        "max_visible_available_at": _iso_or_none(row[6]),
        "valuation_row_count": valuation_row_count,
        "numerator_security_count": numerator,
        "denominator_security_count": denominator,
        "coverage_ratio": coverage_ratio,
        "min_valuation_trade_date": _iso_or_none(min_trade_date),
        "max_valuation_trade_date": _iso_or_none(max_trade_date),
        "min_valuation_period_end": _iso_or_none(min_period_end),
        "max_valuation_period_end": _iso_or_none(max_period_end),
        "denominator_definition": (
            "distinct securities with latest-revision valuation-relevant fundamentals "
            "available at as_of_ts and period_end between min valuation period_end "
            "and max valuation trade_date"
        ),
        "stale_price_fundamental_gap_days": stale_gap_days,
        "stale_valuation_row_count": int(row[7] or 0),
        "max_price_fundamental_gap_days": None if row[8] is None else int(row[8]),
    }


def _date_from_iso(value: Any) -> dt.date | None:
    if value is None:
        return None
    if isinstance(value, pd.Timestamp):
        return None if pd.isna(value) else value.date()
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    text = str(value).strip()
    if not text:
        return None
    return dt.date.fromisoformat(text[:10])


def _timestamp_from_iso(value: Any) -> dt.datetime | None:
    if value is None:
        return None
    if isinstance(value, pd.Timestamp):
        if pd.isna(value):
            return None
        return value.to_pydatetime().replace(tzinfo=None)
    if isinstance(value, dt.datetime):
        return value.replace(tzinfo=None)
    text = str(value).strip()
    if not text:
        return None
    return dt.datetime.fromisoformat(text).replace(tzinfo=None)


def _overlap_slice_id(options: ValuationMultiplesOptions, details: dict[str, object]) -> str:
    payload = json_dumps(
        {
            "source": options.source,
            "market_cap_sources": list(options.market_cap_sources or ()),
            "symbols": list(_normalized_valuation_symbols(options.symbols)) if options.symbols else [],
            "start_date": _iso_or_none(options.start_date),
            "end_date": _iso_or_none(options.end_date),
            "as_of_ts": details.get("as_of_ts"),
            "run_id": options.run_id or "manual",
        }
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _overlap_slice_row(options: ValuationMultiplesOptions, details: dict[str, object]) -> dict[str, object]:
    available_at = (
        _timestamp_from_iso(details.get("as_of_ts"))
        or _timestamp_from_iso(details.get("max_visible_available_at"))
        or dt.datetime.utcnow()
    )
    as_of_date = (
        _date_from_iso(details.get("max_valuation_trade_date"))
        or _date_from_iso(options.end_date)
        or available_at.date()
    )
    return {
        "overlap_slice_id": _overlap_slice_id(options, details),
        "source": options.source,
        "market_cap_sources_json": json_dumps(list(options.market_cap_sources or ())),
        "symbol_scope_json": json_dumps(list(_normalized_valuation_symbols(options.symbols)) if options.symbols else []),
        "start_date": options.start_date,
        "end_date": options.end_date,
        "as_of_ts": _timestamp_from_iso(details.get("as_of_ts")),
        "max_visible_available_at": _timestamp_from_iso(details.get("max_visible_available_at")),
        "numerator_security_count": int(details.get("numerator_security_count") or 0),
        "denominator_security_count": int(details.get("denominator_security_count") or 0),
        "coverage_ratio": details.get("coverage_ratio"),
        "valuation_row_count": int(details.get("valuation_row_count") or 0),
        "min_valuation_trade_date": _date_from_iso(details.get("min_valuation_trade_date")),
        "max_valuation_trade_date": _date_from_iso(details.get("max_valuation_trade_date")),
        "min_valuation_period_end": _date_from_iso(details.get("min_valuation_period_end")),
        "max_valuation_period_end": _date_from_iso(details.get("max_valuation_period_end")),
        "stale_price_fundamental_gap_days": int(details.get("stale_price_fundamental_gap_days") or 0),
        "stale_valuation_row_count": int(details.get("stale_valuation_row_count") or 0),
        "max_price_fundamental_gap_days": details.get("max_price_fundamental_gap_days"),
        "denominator_definition": str(details.get("denominator_definition") or ""),
        "details_json": json_dumps(details),
        "is_latest_revision": True,
        "as_of_date": as_of_date,
        "available_at": available_at,
        "run_id": options.run_id,
    }


def refresh_valuation_overlap_slice(
    store: DuckDBStore,
    options: ValuationMultiplesOptions | None = None,
    *,
    coverage_details: dict[str, object] | None = None,
) -> dict[str, object]:
    """Persist the current valuation price x fundamental overlap coverage row."""

    options = options or ValuationMultiplesOptions()
    store.initialize()
    details = coverage_details or valuation_multiples_overlap_coverage(
        store,
        options,
        as_of_ts=options.coverage_as_of_ts,
    )
    row = _overlap_slice_row(options, details)
    frame = pd.DataFrame([row])
    with store.transaction():
        store.con.execute("DELETE FROM valuation_overlap_slice WHERE overlap_slice_id = ?", [row["overlap_slice_id"]])
        insert_frame(store, frame, "valuation_overlap_slice", "valuation_overlap_slice_insert")
    return details


def valuation_multiples_stale_gap_details(
    store: DuckDBStore,
    options: ValuationMultiplesOptions | None = None,
    *,
    as_of_ts: dt.datetime | pd.Timestamp | None = None,
    stale_gap_days: int | None = None,
) -> dict[str, object]:
    options = options or ValuationMultiplesOptions()
    stale_gap_days = int(
        options.stale_price_fundamental_gap_days
        if stale_gap_days is None
        else stale_gap_days
    )
    resolved_as_of_ts = _valuation_quality_as_of_ts(store, options, as_of_ts)
    predicates, params = _valuation_visible_predicates(options, resolved_as_of_ts, "v")
    where_sql = " AND ".join(predicates)
    count_row = store.con.execute(
        f"""
        SELECT
            count(*)::DOUBLE,
            max(date_diff('day', v.period_end, v.trade_date))
        FROM valuation_multiples v
        WHERE {where_sql}
          AND date_diff('day', v.period_end, v.trade_date) > ?
        """,
        [*params, stale_gap_days],
    ).fetchone()
    rows_cursor = store.con.execute(
        f"""
        SELECT
            v.security_id,
            v.symbol,
            v.trade_date,
            v.period_end,
            v.formula_code,
            date_diff('day', v.period_end, v.trade_date) AS gap_days,
            v.available_at
        FROM valuation_multiples v
        WHERE {where_sql}
          AND date_diff('day', v.period_end, v.trade_date) > ?
        ORDER BY gap_days DESC, v.security_id, v.formula_code
        LIMIT 20
        """,
        [*params, stale_gap_days],
    )
    columns = [column[0] for column in rows_cursor.description or ()]
    examples = [
        {
            key: (_iso_or_none(value) if isinstance(value, (dt.date, dt.datetime, pd.Timestamp)) else value)
            for key, value in dict(zip(columns, row, strict=True)).items()
        }
        for row in rows_cursor.fetchall()
    ]
    return {
        "source": options.source,
        "market_cap_sources": list(options.market_cap_sources or ()),
        "symbols": list(_normalized_valuation_symbols(options.symbols)) if options.symbols else [],
        "as_of_ts": _iso_or_none(resolved_as_of_ts),
        "stale_price_fundamental_gap_days": stale_gap_days,
        "stale_valuation_row_count": int(count_row[0] or 0),
        "max_price_fundamental_gap_days": None if count_row[1] is None else int(count_row[1]),
        "rows": examples,
    }


def load_valuation_multiple_inputs(
    store: DuckDBStore,
    options: ValuationMultiplesOptions,
) -> pd.DataFrame:
    """Assemble one latest-fundamental wide input row per market-cap row."""

    registered: list[str] = []
    joins: list[str] = []
    params: list[object] = []
    symbols = _normalized_valuation_symbols(options.symbols) if options.symbols else None
    if options.symbols and not symbols:
        return pd.DataFrame()
    if options.market_cap_sources:
        store.con.register(
            "valuation_market_cap_source_filter",
            pd.DataFrame({"market_cap_source": list(options.market_cap_sources)}),
        )
        registered.append("valuation_market_cap_source_filter")
        joins.append("JOIN valuation_market_cap_source_filter mcsf ON mcsf.market_cap_source = m.source")
    if symbols:
        store.con.register("valuation_symbol_filter", pd.DataFrame({"symbol": symbols}))
        registered.append("valuation_symbol_filter")
        joins.append("JOIN valuation_symbol_filter vsf ON vsf.symbol = m.symbol")

    date_predicates = []
    if options.start_date is not None:
        date_predicates.append("m.trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("m.trade_date <= ?")
        params.append(options.end_date)
    where_extra = f" AND {' AND '.join(date_predicates)}" if date_predicates else ""
    join_extra = "\n            ".join(joins)

    ttm_cases = _valuation_pivot_case("t", "ttm_value", "ttm_point_id", VALUATION_TTM_INPUTS)
    balance_cases = _valuation_pivot_case("s", "value", "statement_point_id", VALUATION_BALANCE_INPUTS)
    xbrl_instant_cases = _valuation_pivot_case("x", "value", "metric_id", VALUATION_XBRL_INSTANT_INPUTS)
    xbrl_duration_cases = _valuation_pivot_case("x", "value", "metric_id", VALUATION_XBRL_DURATION_INPUTS)

    av_columns = [
        "ttm.rev_av",
        "ttm.ni_av",
        "ttm.oi_av",
        "ttm.ocf_av",
        "ttm.capex_av",
        "ttm.div_av",
        "ttm.repurch_av",
        "bal.assets_av",
        "bal.equity_av",
        "balx.long_term_debt_av",
        "balx.cash_and_equivalents_av",
        "flowx.depreciation_amortization_av",
    ]
    sort_ids = [
        "ttm.rev_id",
        "ttm.ni_id",
        "ttm.oi_id",
        "ttm.ocf_id",
        "ttm.capex_id",
        "ttm.div_id",
        "ttm.repurch_id",
        "bal.assets_id",
        "bal.equity_id",
        "balx.long_term_debt_id",
        "balx.cash_and_equivalents_id",
        "flowx.depreciation_amortization_id",
    ]
    greatest_av = "greatest(" + ", ".join(f"coalesce({column}, TIMESTAMP '1900-01-01')" for column in av_columns) + ")"
    sort_key = "concat_ws('|', " + ", ".join(f"coalesce({column}, '')" for column in sort_ids) + ")"

    sql = f"""
        WITH market_caps AS (
            SELECT
                m.market_cap_id,
                m.source AS market_cap_source,
                m.price_source,
                m.share_source,
                m.security_id,
                m.symbol,
                m.trade_date,
                m.close AS price,
                m.market_cap,
                m.available_at AS market_cap_available_at,
                m.price_available_at,
                m.input_lineage_json AS market_cap_input_lineage_json
            FROM market_cap m
            {join_extra}
            WHERE m.is_latest_revision
              AND m.security_id IS NOT NULL
              AND m.trade_date IS NOT NULL
              AND m.market_cap IS NOT NULL
              AND m.available_at IS NOT NULL
              {where_extra}
        ),
        ttm_ranked AS (
            SELECT
                t.*,
                row_number() OVER (
                    PARTITION BY t.security_id, t.ttm_end_date, t.canonical_metric
                    ORDER BY
                        t.available_at DESC NULLS LAST,
                        t.revision_sequence DESC,
                        t.source DESC,
                        t.ttm_point_id DESC
                ) AS rn
            FROM fundamental_ttm_points t
            WHERE t.is_latest_revision
              AND t.canonical_metric IN ('revenue', 'net_income', 'operating_income',
                                         'operating_cash_flow', 'capital_expenditures',
                                         'dividends_paid', 'share_repurchases')
        ),
        ttm AS (
            SELECT
                t.security_id,
                any_value(t.symbol) AS fundamental_symbol,
                any_value(t.cik) AS cik,
                t.ttm_end_date AS period_end,
                min(t.ttm_start_date) AS period_start,
                any_value(t.fiscal_year) AS fiscal_year,
                any_value(t.fiscal_period) AS fiscal_period,
                {ttm_cases}
            FROM ttm_ranked t
            WHERE t.rn = 1
            GROUP BY t.security_id, t.ttm_end_date
        ),
        bal_ranked AS (
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
              AND s.canonical_metric IN ('stockholders_equity', 'assets')
        ),
        bal AS (
            SELECT
                s.security_id,
                any_value(s.symbol) AS fundamental_symbol,
                any_value(s.cik) AS cik,
                s.period_end,
                any_value(s.fiscal_year) AS fiscal_year,
                any_value(s.fiscal_period) AS fiscal_period,
                {balance_cases}
            FROM bal_ranked s
            WHERE s.rn = 1
            GROUP BY s.security_id, s.period_end
        ),
        balx_ranked AS (
            SELECT
                x.*,
                row_number() OVER (
                    PARTITION BY x.security_id, x.period_end, x.canonical_metric
                    ORDER BY
                        x.available_at DESC NULLS LAST,
                        x.revision_seq DESC,
                        x.source DESC,
                        x.metric_id DESC
                ) AS rn
            FROM fundamental_xbrl_metric x
            WHERE x.is_latest_revision
              AND x.period_type = 'instant'
              AND x.canonical_metric IN ('long_term_debt', 'cash_and_equivalents')
        ),
        balx AS (
            SELECT
                x.security_id,
                any_value(x.symbol) AS fundamental_symbol,
                any_value(x.cik) AS cik,
                x.period_end,
                any_value(x.fiscal_year) AS fiscal_year,
                any_value(x.fiscal_period) AS fiscal_period,
                {xbrl_instant_cases}
            FROM balx_ranked x
            WHERE x.rn = 1
            GROUP BY x.security_id, x.period_end
        ),
        flowx_ranked AS (
            SELECT
                x.*,
                row_number() OVER (
                    PARTITION BY x.security_id, x.period_end, x.canonical_metric
                    ORDER BY
                        x.period_start DESC NULLS LAST,
                        x.available_at DESC NULLS LAST,
                        x.revision_seq DESC,
                        x.source DESC,
                        x.metric_id DESC
                ) AS rn
            FROM fundamental_xbrl_metric x
            WHERE x.is_latest_revision
              AND x.period_type = 'duration'
              AND x.canonical_metric = 'depreciation_amortization'
        ),
        flowx AS (
            SELECT
                x.security_id,
                any_value(x.symbol) AS fundamental_symbol,
                any_value(x.cik) AS cik,
                x.period_end,
                min(x.period_start) AS period_start,
                any_value(x.fiscal_year) AS fiscal_year,
                any_value(x.fiscal_period) AS fiscal_period,
                {xbrl_duration_cases}
            FROM flowx_ranked x
            WHERE x.rn = 1
            GROUP BY x.security_id, x.period_end
        ),
        periods AS (
            SELECT security_id, period_end FROM ttm
            UNION
            SELECT security_id, period_end FROM bal
            UNION
            SELECT security_id, period_end FROM balx
            UNION
            SELECT security_id, period_end FROM flowx
        ),
        fundamentals AS (
            SELECT
                p.security_id,
                coalesce(ttm.fundamental_symbol, bal.fundamental_symbol, balx.fundamental_symbol, flowx.fundamental_symbol) AS fundamental_symbol,
                coalesce(ttm.cik, bal.cik, balx.cik, flowx.cik) AS cik,
                coalesce(ttm.period_start, flowx.period_start) AS period_start,
                p.period_end,
                coalesce(ttm.fiscal_year, bal.fiscal_year, balx.fiscal_year, flowx.fiscal_year) AS fiscal_year,
                coalesce(ttm.fiscal_period, bal.fiscal_period, balx.fiscal_period, flowx.fiscal_period) AS fiscal_period,
                ttm.rev, ttm.rev_av, ttm.rev_id, ttm.rev_source,
                ttm.ni, ttm.ni_av, ttm.ni_id, ttm.ni_source,
                ttm.oi, ttm.oi_av, ttm.oi_id, ttm.oi_source,
                ttm.ocf, ttm.ocf_av, ttm.ocf_id, ttm.ocf_source,
                ttm.capex, ttm.capex_av, ttm.capex_id, ttm.capex_source,
                ttm.div, ttm.div_av, ttm.div_id, ttm.div_source,
                ttm.repurch, ttm.repurch_av, ttm.repurch_id, ttm.repurch_source,
                bal.assets, bal.assets_av, bal.assets_id, bal.assets_source,
                bal.equity, bal.equity_av, bal.equity_id, bal.equity_source,
                balx.long_term_debt, balx.long_term_debt_av, balx.long_term_debt_id, balx.long_term_debt_source,
                balx.cash_and_equivalents, balx.cash_and_equivalents_av, balx.cash_and_equivalents_id, balx.cash_and_equivalents_source,
                flowx.depreciation_amortization, flowx.depreciation_amortization_av,
                flowx.depreciation_amortization_id, flowx.depreciation_amortization_source,
                {greatest_av} AS fundamental_available_at,
                {sort_key} AS fundamental_sort_key
            FROM periods p
            LEFT JOIN ttm
              ON ttm.security_id = p.security_id
             AND ttm.period_end = p.period_end
            LEFT JOIN bal
              ON bal.security_id = p.security_id
             AND bal.period_end = p.period_end
            LEFT JOIN balx
              ON balx.security_id = p.security_id
             AND balx.period_end = p.period_end
            LEFT JOIN flowx
              ON flowx.security_id = p.security_id
             AND flowx.period_end = p.period_end
        ),
        matched AS (
            SELECT
                mc.*,
                f.period_start,
                f.period_end,
                f.fiscal_year,
                f.fiscal_period,
                f.rev, f.rev_av, f.rev_id, f.rev_source,
                f.ni, f.ni_av, f.ni_id, f.ni_source,
                f.oi, f.oi_av, f.oi_id, f.oi_source,
                f.ocf, f.ocf_av, f.ocf_id, f.ocf_source,
                f.capex, f.capex_av, f.capex_id, f.capex_source,
                f.div, f.div_av, f.div_id, f.div_source,
                f.repurch, f.repurch_av, f.repurch_id, f.repurch_source,
                f.assets, f.assets_av, f.assets_id, f.assets_source,
                f.equity, f.equity_av, f.equity_id, f.equity_source,
                f.long_term_debt, f.long_term_debt_av, f.long_term_debt_id, f.long_term_debt_source,
                f.cash_and_equivalents, f.cash_and_equivalents_av,
                f.cash_and_equivalents_id, f.cash_and_equivalents_source,
                f.depreciation_amortization, f.depreciation_amortization_av,
                f.depreciation_amortization_id, f.depreciation_amortization_source,
                CASE
                    WHEN mc.market_cap IS NOT NULL
                     AND f.long_term_debt IS NOT NULL
                     AND f.cash_and_equivalents IS NOT NULL
                    THEN mc.market_cap + f.long_term_debt - f.cash_and_equivalents
                END AS enterprise_value,
                CASE
                    WHEN mc.market_cap_available_at IS NOT NULL
                     AND f.long_term_debt_av IS NOT NULL
                     AND f.cash_and_equivalents_av IS NOT NULL
                    THEN greatest(mc.market_cap_available_at, f.long_term_debt_av, f.cash_and_equivalents_av)
                END AS enterprise_value_av,
                f.fundamental_available_at,
                f.fundamental_sort_key,
                row_number() OVER (
                    PARTITION BY mc.market_cap_source, mc.security_id, mc.trade_date
                    ORDER BY f.period_end DESC, f.fundamental_available_at DESC, f.fundamental_sort_key DESC
                ) AS valuation_period_rn
            FROM market_caps mc
            JOIN fundamentals f
              ON f.security_id = mc.security_id
             AND f.period_end <= mc.trade_date
        )
        SELECT * EXCLUDE (valuation_period_rn)
        FROM matched
        WHERE valuation_period_rn = 1
    """
    try:
        return store.con.execute(sql, params).df()
    finally:
        for relation in registered:
            store.con.unregister(relation)


def _delete_valuation_multiples_scope(
    store: DuckDBStore,
    options: ValuationMultiplesOptions,
) -> None:
    predicates = ["source = ?"]
    params: list[object] = [options.source]
    if options.symbols:
        symbols = _normalized_valuation_symbols(options.symbols)
        if not symbols:
            return
        placeholders = ", ".join("?" for _ in symbols)
        predicates.append(f"symbol IN ({placeholders})")
        params.extend(symbols)
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    if options.market_cap_sources:
        placeholders = ", ".join("?" for _ in options.market_cap_sources)
        predicates.append(f"market_cap_source IN ({placeholders})")
        params.extend(options.market_cap_sources)
    store.con.execute(f"DELETE FROM valuation_multiples WHERE {' AND '.join(predicates)}", params)


def refresh_valuation_multiples(
    store: DuckDBStore,
    options: ValuationMultiplesOptions | None = None,
) -> int:
    options = options or ValuationMultiplesOptions()
    store.initialize()
    inputs = load_valuation_multiple_inputs(store, options)
    rows = compute_valuation_multiple_rows(inputs, source=options.source, run_id=options.run_id)
    with store.transaction():
        _delete_valuation_multiples_scope(store, options)
        if not rows.empty:
            insert_frame(store, rows, "valuation_multiples", "valuation_multiples_insert")
    return len(rows)


class ValuationMultiplesDataset(Dataset):
    dataset_id = "valuation_multiples"
    source_name = VALUATION_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: ValuationMultiplesOptions) -> DatasetLoadResult:
        rows = refresh_valuation_multiples(store, options)
        coverage_details = valuation_multiples_overlap_coverage(
            store,
            options,
            as_of_ts=options.coverage_as_of_ts,
        )
        refresh_valuation_overlap_slice(store, options, coverage_details=coverage_details)
        coverage_ratio = coverage_details["coverage_ratio"]
        stale_details = valuation_multiples_stale_gap_details(
            store,
            options,
            as_of_ts=options.coverage_as_of_ts,
        )
        stale_count = float(stale_details["stale_valuation_row_count"])
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="valuation_multiples",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="valuation_multiples",
            check_name="overlap_coverage",
            status="passed" if coverage_ratio == 1.0 else "warning",
            observed_value=None if coverage_ratio is None else float(coverage_ratio),
            threshold_value=1.0,
            details=coverage_details,
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="valuation_multiples",
            check_name="stale_price_fundamental_gap_days",
            status="passed" if stale_count == 0.0 else "warning",
            observed_value=stale_count,
            threshold_value=0.0,
            details=stale_details,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={
                "formulas": [definition.code for definition in VALUATION_FORMULA_DEFS],
                "coverage": coverage_details,
                "stale_price_fundamental_gap_days": stale_details,
            },
        )
