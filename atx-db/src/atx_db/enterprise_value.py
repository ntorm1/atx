"""PF3-S5 S5-1: point-in-time enterprise value surface."""

from __future__ import annotations

import datetime as dt
import hashlib
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import json_dumps, quality_check

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
        [*key_columns, "fundamental_is_available", "period_end", "fundamental_available_at", "fundamental_sort_key"],
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

    # Function-local (not module-level) so `compute_enterprise_value_rows` stays the
    # only place these column lists exist -- keeps the vectorized assembly free of any
    # new public-API surface. Rebuilt per call; the lists are tiny literals.
    component_columns = [
        "market_cap",
        "total_debt",
        "preferred_equity",
        "minority_interest",
        "cash_and_equivalents",
    ]
    available_at_columns = [
        "market_cap_available_at",
        "total_debt_available_at",
        "preferred_equity_available_at",
        "minority_interest_available_at",
        "cash_and_equivalents_available_at",
    ]
    # Columns `_input_lineage_json`/`_component_lineage` read via `row.get(...)`. Selecting
    # exactly these (in this order) lets the batched lineage pass reconstruct per-row dicts
    # from `itertuples()` tuples without ever materializing a full-width per-row Series
    # (which is what makes `DataFrame.iterrows()` slow). Read from `selected` -- the raw,
    # pre-`astype(float)` component values -- not `prepared`, so an int-valued component
    # still renders as a JSON int in the lineage, matching the pre-vectorization output.
    lineage_columns = [
        "market_cap_source",
        "market_cap_id",
        "security_id",
        "trade_date",
        "market_cap",
        "market_cap_available_at",
        "market_cap_input_lineage_json",
        "period_end",
        "total_debt_id",
        "total_debt_source",
        "total_debt",
        "total_debt_available_at",
        "preferred_equity_id",
        "preferred_equity_source",
        "preferred_equity",
        "preferred_equity_available_at",
        "minority_interest_id",
        "minority_interest_source",
        "minority_interest",
        "minority_interest_available_at",
        "cash_and_equivalents_id",
        "cash_and_equivalents_source",
        "cash_and_equivalents",
        "cash_and_equivalents_available_at",
    ]

    def _skip_bad_component_rows(frame: pd.DataFrame) -> pd.Series:
        """PF4-S3 S3-10: skip-and-flag replacement for the old raising check (which
        called `_assert_non_negative_component`-equivalent logic and aborted the whole
        refresh for one dirty security-day). Vectorized equivalent of a
        `for _, row in selected.iterrows()` scan that coerces each of the 5 EV
        components and, on the first None/NaN or negative value in a row (checked in
        `component_columns` order -- market_cap, total_debt, preferred_equity,
        minority_interest, cash_and_equivalents, same order the original per-row scan
        used), records `(security_id, trade_date, component)` to a skipped-rows
        collector and logs it instead of raising. `import logging` is function-local so
        this snapshot-tracked module gains no new module-level symbol.
        """
        import logging

        values = frame.to_numpy()
        is_bad_cell = pd.isna(frame).to_numpy() | (values < 0)
        row_is_bad = is_bad_cell.any(axis=1)

        skipped: list[tuple[Any, Any, str]] = []
        for row_pos, is_bad in enumerate(row_is_bad):
            if not is_bad:
                continue
            column = component_columns[int(is_bad_cell[row_pos].argmax())]
            bad_row = selected.iloc[row_pos]
            skipped.append((bad_row.get("security_id"), bad_row.get("trade_date"), column))

        if skipped:
            logger = logging.getLogger(__name__)
            for security_id, trade_date, column in skipped:
                logger.warning(
                    "Skipping enterprise-value row security_id=%s trade_date=%s: "
                    "%s must be a non-negative enterprise-value component",
                    security_id,
                    trade_date,
                    column,
                )

        return pd.Series(~row_is_bad, index=frame.index)

    components = selected[component_columns].astype(float)
    clean_mask = _skip_bad_component_rows(components)
    if not clean_mask.all():
        selected = selected.loc[clean_mask].reset_index(drop=True)
        components = components.loc[clean_mask].reset_index(drop=True)
        if selected.empty:
            return _empty_enterprise_value_frame()

    prepared = selected.copy()
    prepared["market_cap"] = components["market_cap"]
    prepared["total_debt"] = components["total_debt"]
    prepared["preferred_equity"] = components["preferred_equity"]
    prepared["minority_interest"] = components["minority_interest"]
    prepared["cash_and_equivalents"] = components["cash_and_equivalents"]
    prepared["enterprise_value"] = (
        components["market_cap"]
        + components["total_debt"]
        + components["preferred_equity"]
        + components["minority_interest"]
        - components["cash_and_equivalents"]
    )
    prepared["is_latest_revision"] = True
    prepared["as_of_date"] = selected["trade_date"]
    prepared["available_at"] = selected[available_at_columns].max(axis=1)
    prepared["source"] = source
    prepared["enterprise_value_id"] = [
        _enterprise_value_id(source, str(market_cap_source), str(security_id), trade_date)
        for market_cap_source, security_id, trade_date in zip(
            selected["market_cap_source"],
            selected["security_id"],
            selected["trade_date"],
            strict=True,
        )
    ]
    # `COMPONENT_CODE_MAP` is the same literal for every row; hoisted out of the
    # per-row pass so json_dumps is called once here instead of once per row.
    prepared["input_codes_json"] = json_dumps(COMPONENT_CODE_MAP)
    prepared["formula_version"] = "enterprise_value_v1"
    prepared["run_id"] = run_id

    # Single batched lineage-JSON pass: `itertuples()` (unlike `iterrows()`) reads each
    # column's native array directly instead of materializing a full-width per-row
    # Series, so per-cell values/dtypes (incl. pd.NA/Timestamp/date) are preserved
    # exactly, but building the M-row list stays a single, cheap tuple-unpacking loop.
    prepared["input_lineage_json"] = [
        _input_lineage_json(dict(zip(lineage_columns, values, strict=True)))
        for values in selected[lineage_columns].itertuples(index=False, name=None)
    ]

    return prepared[ENTERPRISE_VALUE_COLUMNS].reset_index(drop=True)


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


def _enterprise_value_stage_sql(options: EnterpriseValueOptions) -> tuple[str, list[object]]:
    """Build a fully set-based, component-lineaged EV refresh query."""

    symbols = _normalized_symbols(options.symbols)
    if options.symbols is not None and not symbols:
        return "", []

    predicates = [
        "m.is_latest_revision",
        "m.security_id IS NOT NULL",
        "m.trade_date IS NOT NULL",
        "m.market_cap IS NOT NULL",
        "m.market_cap >= 0",
        "isfinite(m.market_cap)",
        "m.available_at IS NOT NULL",
    ]
    params: list[object] = [options.source, options.run_id]
    if options.market_cap_sources:
        placeholders = ", ".join("?" for _ in options.market_cap_sources)
        predicates.append(f"m.source IN ({placeholders})")
        params.extend(options.market_cap_sources)
    if symbols:
        placeholders = ", ".join("?" for _ in symbols)
        predicates.append(f"upper(m.symbol) IN ({placeholders})")
        params.extend(symbols)
    if options.start_date is not None:
        predicates.append("m.trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("m.trade_date <= ?")
        params.append(options.end_date)
    market_cap_where = "\n              AND ".join(predicates)

    raw_concepts = (
        ("total_debt_reported", "TotalDebt"),
        ("debt_current", "DebtCurrent"),
        ("long_term_debt_current", "LongTermDebtCurrent"),
        ("short_term_borrowings", "ShortTermBorrowings"),
        ("commercial_paper", "CommercialPaper"),
        ("long_term_debt_noncurrent", "LongTermDebtNoncurrent"),
        ("preferred_equity", "PreferredStockValue"),
        ("minority_interest", "MinorityInterest"),
        ("cash_and_equivalents", "CashAndCashEquivalentsAtCarryingValue"),
    )
    fact_projection: list[str] = []
    for component, concept in raw_concepts:
        fact_projection.extend(
            [
                f"max(CASE WHEN l.concept = '{concept}' THEN l.fact.value END) AS {component}",
                f"max(CASE WHEN l.concept = '{concept}' THEN l.fact.available_at END) "
                f"AS {component}_available_at",
                f"max(CASE WHEN l.concept = '{concept}' THEN l.fact.fact_revision_id END) "
                f"AS {component}_id",
                f"max(CASE WHEN l.concept = '{concept}' THEN l.fact.source END) "
                f"AS {component}_source",
            ]
        )
    fact_columns = ",\n                ".join(fact_projection)

    sql = f"""
        CREATE OR REPLACE TEMP TABLE enterprise_value_refresh_stage AS
        WITH config AS (
            SELECT CAST(? AS VARCHAR) AS source, CAST(? AS VARCHAR) AS run_id
        ),
        market_caps AS (
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
            WHERE {market_cap_where}
        ),
        component_source AS (
            SELECT
                r.security_id,
                r.period_start,
                r.period_end,
                r.concept,
                r.fiscal_year,
                r.fiscal_period,
                r.value,
                r.available_at,
                r.fact_revision_id,
                r.accession_number,
                r.source,
                r.revision_sequence,
                1 AS source_priority
            FROM fundamental_fact_revisions r
            WHERE r.is_latest_revision
              AND r.period_start IS NULL
              AND r.unit = 'USD'
              AND r.concept IN (
                    'DebtCurrent',
                    'LongTermDebtCurrent',
                    'ShortTermBorrowings',
                    'CommercialPaper',
                    'LongTermDebtNoncurrent',
                    'PreferredStockValue',
                    'MinorityInterest',
                    'CashAndCashEquivalentsAtCarryingValue'
              )
              AND r.value IS NOT NULL
              AND r.value >= 0
              AND isfinite(r.value)
              AND r.available_at IS NOT NULL

            UNION ALL

            SELECT
                s.security_id,
                s.period_start,
                s.period_end,
                CASE s.canonical_metric
                    WHEN 'total_debt' THEN 'TotalDebt'
                    WHEN 'pref_stock' THEN 'PreferredStockValue'
                    WHEN 'minority_int_bs' THEN 'MinorityInterest'
                    WHEN 'cash_st_inv' THEN 'CashAndCashEquivalentsAtCarryingValue'
                END AS concept,
                s.fiscal_year,
                s.fiscal_period,
                s.value,
                s.available_at,
                s.statement_point_id AS fact_revision_id,
                s.accession_number,
                s.source,
                s.revision_sequence,
                0 AS source_priority
            FROM fundamental_statement_points s
            WHERE s.is_latest_revision
              AND s.period_type = 'instant'
              AND s.canonical_metric IN ('total_debt', 'pref_stock', 'minority_int_bs', 'cash_st_inv')
              AND s.value IS NOT NULL
              AND s.value >= 0
              AND isfinite(s.value)
              AND s.available_at IS NOT NULL
        ),
        latest_components AS (
            SELECT
                x.security_id,
                x.period_end,
                x.concept,
                arg_max(
                    struct_pack(
                        period_start := x.period_start,
                        fiscal_year := x.fiscal_year,
                        fiscal_period := x.fiscal_period,
                        value := x.value,
                        available_at := x.available_at,
                        fact_revision_id := x.fact_revision_id,
                        accession_number := x.accession_number,
                        source := x.source
                    ),
                    struct_pack(
                        source_priority := x.source_priority,
                        available_at := x.available_at,
                        revision_sequence := coalesce(x.revision_sequence, 0),
                        source := x.source,
                        fact_revision_id := x.fact_revision_id
                    )
                ) AS fact
            FROM component_source x
            GROUP BY x.security_id, x.period_end, x.concept
        ),
        fundamentals AS (
            SELECT
                l.security_id,
                l.period_end,
                min(l.fact.period_start) AS period_start,
                max(l.fact.fiscal_year) AS fiscal_year,
                max(l.fact.fiscal_period) AS fiscal_period,
                {fact_columns}
            FROM latest_components l
            GROUP BY l.security_id, l.period_end
        ),
        derived_fundamentals AS (
            SELECT
                f.*,
                coalesce(
                    f.total_debt_reported,
                    coalesce(
                        f.debt_current,
                        coalesce(f.long_term_debt_current, 0)
                            + coalesce(f.short_term_borrowings, f.commercial_paper, 0)
                    ) + coalesce(f.long_term_debt_noncurrent, 0)
                ) AS total_debt,
                CASE
                    WHEN f.total_debt_reported IS NOT NULL THEN f.total_debt_reported_available_at
                    ELSE greatest(
                    CASE
                        WHEN f.debt_current IS NOT NULL THEN f.debt_current_available_at
                        ELSE greatest(
                            f.long_term_debt_current_available_at,
                            coalesce(
                                f.short_term_borrowings_available_at,
                                f.commercial_paper_available_at
                            )
                        )
                    END,
                    f.long_term_debt_noncurrent_available_at
                    )
                END AS total_debt_available_at,
                coalesce(
                    f.total_debt_reported_id,
                    sha256(concat_ws(
                        '|',
                        'derived_total_debt_v1',
                        f.security_id,
                        CAST(f.period_end AS VARCHAR),
                        f.debt_current_id,
                        f.long_term_debt_current_id,
                        f.short_term_borrowings_id,
                        f.commercial_paper_id,
                        f.long_term_debt_noncurrent_id
                    ))
                ) AS total_debt_id,
                coalesce(
                    f.total_debt_reported_source,
                    'derived_sec_companyfacts_debt_v1'
                ) AS total_debt_source,
                CAST(json_object(
                    'formula', 'coalesce(DebtCurrent, LongTermDebtCurrent + coalesce(ShortTermBorrowings, CommercialPaper, 0)) + LongTermDebtNoncurrent',
                    'reported_total_debt', json_object(
                        'fact_revision_id', f.total_debt_reported_id,
                        'source', f.total_debt_reported_source,
                        'value', f.total_debt_reported,
                        'available_at', f.total_debt_reported_available_at
                    ),
                    'debt_current', json_object(
                        'fact_revision_id', f.debt_current_id,
                        'source', f.debt_current_source,
                        'value', f.debt_current,
                        'available_at', f.debt_current_available_at
                    ),
                    'long_term_debt_current', json_object(
                        'fact_revision_id', f.long_term_debt_current_id,
                        'source', f.long_term_debt_current_source,
                        'value', f.long_term_debt_current,
                        'available_at', f.long_term_debt_current_available_at
                    ),
                    'short_term_borrowings', json_object(
                        'fact_revision_id', f.short_term_borrowings_id,
                        'source', f.short_term_borrowings_source,
                        'value', f.short_term_borrowings,
                        'available_at', f.short_term_borrowings_available_at
                    ),
                    'commercial_paper', json_object(
                        'fact_revision_id', f.commercial_paper_id,
                        'source', f.commercial_paper_source,
                        'value', f.commercial_paper,
                        'available_at', f.commercial_paper_available_at
                    ),
                    'long_term_debt_noncurrent', json_object(
                        'fact_revision_id', f.long_term_debt_noncurrent_id,
                        'source', f.long_term_debt_noncurrent_source,
                        'value', f.long_term_debt_noncurrent,
                        'available_at', f.long_term_debt_noncurrent_available_at
                    )
                ) AS VARCHAR) AS total_debt_input_lineage_json
            FROM fundamentals f
            WHERE f.total_debt_reported IS NOT NULL
               OR f.debt_current IS NOT NULL
               OR f.long_term_debt_current IS NOT NULL
               OR f.short_term_borrowings IS NOT NULL
               OR f.commercial_paper IS NOT NULL
               OR f.long_term_debt_noncurrent IS NOT NULL
        ),
        complete_fundamentals AS (
            SELECT
                f.*,
                greatest(
                    f.total_debt_available_at,
                    f.preferred_equity_available_at,
                    f.minority_interest_available_at,
                    f.cash_and_equivalents_available_at
                ) AS fundamental_available_at,
                concat_ws(
                    '|',
                    f.total_debt_id,
                    f.preferred_equity_id,
                    f.minority_interest_id,
                    f.cash_and_equivalents_id
                ) AS fundamental_sort_key
            FROM derived_fundamentals f
            WHERE f.preferred_equity IS NOT NULL
              AND f.minority_interest IS NOT NULL
              AND f.cash_and_equivalents IS NOT NULL
        ),
        matched_groups AS (
            SELECT
                mc.market_cap_id,
                mc.market_cap_source,
                mc.security_id,
                mc.symbol,
                mc.trade_date,
                mc.price,
                mc.share_count,
                mc.share_count_type_used,
                mc.market_cap,
                mc.market_cap_available_at,
                mc.price_available_at,
                mc.share_available_at,
                mc.market_cap_input_lineage_json,
                arg_max(
                    struct_pack(
                        period_start := f.period_start,
                        period_end := f.period_end,
                        fiscal_year := f.fiscal_year,
                        fiscal_period := f.fiscal_period,
                        total_debt := f.total_debt,
                        total_debt_available_at := f.total_debt_available_at,
                        total_debt_id := f.total_debt_id,
                        total_debt_source := f.total_debt_source,
                        total_debt_input_lineage_json := f.total_debt_input_lineage_json,
                        preferred_equity := f.preferred_equity,
                        preferred_equity_available_at := f.preferred_equity_available_at,
                        preferred_equity_id := f.preferred_equity_id,
                        preferred_equity_source := f.preferred_equity_source,
                        minority_interest := f.minority_interest,
                        minority_interest_available_at := f.minority_interest_available_at,
                        minority_interest_id := f.minority_interest_id,
                        minority_interest_source := f.minority_interest_source,
                        cash_and_equivalents := f.cash_and_equivalents,
                        cash_and_equivalents_available_at := f.cash_and_equivalents_available_at,
                        cash_and_equivalents_id := f.cash_and_equivalents_id,
                        cash_and_equivalents_source := f.cash_and_equivalents_source
                    ),
                    struct_pack(
                        available_priority := f.fundamental_available_at <= CAST(mc.trade_date AS TIMESTAMP),
                        period_end := f.period_end,
                        fundamental_available_at := f.fundamental_available_at,
                        fundamental_sort_key := f.fundamental_sort_key
                    )
                ) AS fundamental
            FROM market_caps mc
            JOIN complete_fundamentals f
              ON f.security_id = mc.security_id
             AND f.period_end <= mc.trade_date
            GROUP BY
                mc.market_cap_id,
                mc.market_cap_source,
                mc.security_id,
                mc.symbol,
                mc.trade_date,
                mc.price,
                mc.share_count,
                mc.share_count_type_used,
                mc.market_cap,
                mc.market_cap_available_at,
                mc.price_available_at,
                mc.share_available_at,
                mc.market_cap_input_lineage_json
        ),
        matched AS (
            SELECT
                g.* EXCLUDE (fundamental),
                unnest(g.fundamental)
            FROM matched_groups g
        )
        SELECT
            sha256(concat_ws(
                '|', c.source, m.market_cap_source, m.security_id, CAST(m.trade_date AS VARCHAR)
            )) AS enterprise_value_id,
            c.source,
            m.market_cap_source,
            m.market_cap_id,
            m.security_id,
            m.symbol,
            m.trade_date,
            m.period_start,
            m.period_end,
            m.fiscal_year,
            m.fiscal_period,
            m.price,
            m.share_count,
            m.share_count_type_used,
            m.market_cap,
            m.total_debt,
            m.preferred_equity,
            m.minority_interest,
            m.cash_and_equivalents,
            m.market_cap + m.total_debt + m.preferred_equity
                + m.minority_interest - m.cash_and_equivalents AS enterprise_value,
            true AS is_latest_revision,
            m.trade_date AS as_of_date,
            greatest(
                m.market_cap_available_at,
                m.total_debt_available_at,
                m.preferred_equity_available_at,
                m.minority_interest_available_at,
                m.cash_and_equivalents_available_at
            ) AS available_at,
            m.market_cap_available_at,
            m.price_available_at,
            m.share_available_at,
            m.total_debt_available_at,
            m.preferred_equity_available_at,
            m.minority_interest_available_at,
            m.cash_and_equivalents_available_at,
            CAST(json_object(
                'market_cap', 'market_cap.market_cap',
                'total_debt', 'derived:SEC debt facts',
                'preferred_equity', 'fundamental_fact_revisions.PreferredStockValue',
                'minority_interest', 'fundamental_fact_revisions.MinorityInterest',
                'cash_and_equivalents', 'fundamental_fact_revisions.CashAndCashEquivalentsAtCarryingValue'
            ) AS VARCHAR) AS input_codes_json,
            CAST(json_object(
                'formula', 'enterprise_value = market_cap + total_debt + preferred_equity + minority_interest - cash_and_equivalents',
                'components', json_object(
                    'market_cap', json_object(
                        'table', 'market_cap',
                        'source', m.market_cap_source,
                        'market_cap_id', m.market_cap_id,
                        'security_id', m.security_id,
                        'trade_date', m.trade_date,
                        'value', m.market_cap,
                        'available_at', m.market_cap_available_at,
                        'sign', 'add',
                        'upstream_lineage', m.market_cap_input_lineage_json
                    ),
                    'total_debt', json_object(
                        'table', 'derived',
                        'input_id', m.total_debt_id,
                        'source', m.total_debt_source,
                        'canonical_metric', 'total_debt',
                        'period_end', m.period_end,
                        'value', m.total_debt,
                        'available_at', m.total_debt_available_at,
                        'sign', 'add',
                        'upstream_lineage', m.total_debt_input_lineage_json
                    ),
                    'preferred_equity', json_object(
                        'table', 'fundamental_fact_revisions',
                        'fact_revision_id', m.preferred_equity_id,
                        'source', m.preferred_equity_source,
                        'canonical_metric', 'pref_stock',
                        'period_end', m.period_end,
                        'value', m.preferred_equity,
                        'available_at', m.preferred_equity_available_at,
                        'sign', 'add'
                    ),
                    'minority_interest', json_object(
                        'table', 'fundamental_fact_revisions',
                        'fact_revision_id', m.minority_interest_id,
                        'source', m.minority_interest_source,
                        'canonical_metric', 'minority_int_bs',
                        'period_end', m.period_end,
                        'value', m.minority_interest,
                        'available_at', m.minority_interest_available_at,
                        'sign', 'add'
                    ),
                    'cash_and_equivalents', json_object(
                        'table', 'fundamental_fact_revisions',
                        'fact_revision_id', m.cash_and_equivalents_id,
                        'source', m.cash_and_equivalents_source,
                        'canonical_metric', 'cash_st_inv',
                        'period_end', m.period_end,
                        'value', m.cash_and_equivalents,
                        'available_at', m.cash_and_equivalents_available_at,
                        'sign', 'subtract'
                    )
                )
            ) AS VARCHAR) AS input_lineage_json,
            'enterprise_value_v1' AS formula_version,
            c.run_id
        FROM matched m
        CROSS JOIN config c
    """
    return sql, params


def refresh_enterprise_value(store: DuckDBStore, options: EnterpriseValueOptions | None = None) -> int:
    """Atomically refresh EV without materializing the fact surface in pandas."""

    options = options or EnterpriseValueOptions()
    store.initialize()
    stage_sql, params = _enterprise_value_stage_sql(options)
    if not stage_sql:
        return 0
    try:
        store.con.execute(stage_sql, params)
        row_count = int(store.con.execute("SELECT count(*) FROM enterprise_value_refresh_stage").fetchone()[0])
        with store.transaction():
            _delete_enterprise_value_scope(store, options)
            if row_count:
                columns = ", ".join(ENTERPRISE_VALUE_COLUMNS)
                store.con.execute(
                    f"INSERT INTO enterprise_value ({columns}) "
                    f"SELECT {columns} FROM enterprise_value_refresh_stage"
                )
        return row_count
    finally:
        store.con.execute("DROP TABLE IF EXISTS enterprise_value_refresh_stage")


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
