"""Point-in-time annual changes in DuPont profit-margin variants."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT annual margin change research v1"
VARIANT_FACTOR_IDS = {
    "net_margin": "profitability_annual_net_margin_change",
    "operating_margin": "profitability_annual_operating_margin_change",
    "gross_margin": "profitability_annual_gross_margin_change",
}
VARIANT_METRICS = {
    "net_margin": "net_income",
    "operating_margin": "operating_income",
    "gross_margin": "gross_profit",
}
VARIANT_NAMES = {
    "net_margin": "PIT annual change in net profit margin",
    "operating_margin": "PIT annual change in operating margin",
    "gross_margin": "PIT annual change in gross margin",
}
_OUTPUT_COLUMNS = [
    "factor_value_id",
    "factor_id",
    "factor_name",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "raw_value",
    "value",
    "available_at",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
]


@dataclass(frozen=True)
class AnnualMarginChangeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(
    source: str,
    factor_id: str,
    security_id: str,
    as_of_date: Any,
) -> str:
    payload = "|".join(
        str(part) for part in (source, factor_id, security_id, as_of_date)
    )
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: AnnualMarginChangeOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def _metric_pivot(metric: str) -> str:
    predicate = (
        f"canonical_metric = '{metric}' "
        "AND period_type = 'duration' "
        "AND period_start IS NOT NULL "
        "AND period_end - period_start BETWEEN 329 AND 399"
    )
    order = "(available_at, revision_sequence, statement_point_id)"
    return ",\n".join(
        [
            f"arg_max(value, {order}) FILTER (WHERE {predicate}) AS {metric}",
            f"arg_max(statement_point_id, {order}) "
            f"FILTER (WHERE {predicate}) AS {metric}_id",
            f"max(available_at) FILTER (WHERE {predicate}) AS {metric}_available_at",
        ]
    )


def load_annual_margin_change_inputs(
    store: DuckDBStore,
    options: AnnualMarginChangeOptions | None = None,
) -> pd.DataFrame:
    """Resolve exact current/prior annual margin observations for all variants."""

    options = options or AnnualMarginChangeOptions()
    date_sql, date_params = _date_filter(options)
    metrics = ("revenue", *VARIANT_METRICS.values())
    metric_filter = ",".join(f"'{metric}'" for metric in metrics)
    pivot_columns = ",\n".join(_metric_pivot(metric) for metric in metrics)
    event_branches = "\nUNION ALL\n".join(
        f"""
        SELECT
            '{variant}' AS variant,
            security_id,
            fundamental_symbol,
            accession_number,
            period_end,
            revenue,
            revenue_id,
            revenue_available_at,
            {metric} AS numerator,
            {metric}_id AS numerator_id,
            {metric}_available_at AS numerator_available_at,
            greatest(revenue_available_at, {metric}_available_at) AS event_available_at
        FROM annual_filings
        WHERE revenue > 0
          AND {metric} IS NOT NULL
          AND isfinite(revenue)
          AND isfinite({metric})
        """
        for variant, metric in VARIANT_METRICS.items()
    )
    sql = f"""
        WITH annual_filings AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_end,
                {pivot_columns}
            FROM fundamental_statement_points
            WHERE canonical_metric IN ({metric_filter})
              AND unit = 'USD'
              AND period_end IS NOT NULL
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A'
              )
            GROUP BY security_id, accession_number, period_end
        ),
        variant_events AS (
            {event_branches}
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT DISTINCT security_id FROM variant_events)
              AND close > 0
              AND trade_date IS NOT NULL
              AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
        ),
        price_months AS (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT * FROM price_months
            WHERE month_rank = 1 {date_sql}
        ),
        governed_rebalances AS (
            SELECT
                p.*,
                u.valid_from AS universe_valid_from,
                u.valid_to AS universe_valid_to,
                u.universe_id,
                u.available_at AS universe_available_at,
                u.source AS universe_source,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY u.valid_from DESC,
                             u.available_at DESC NULLS LAST,
                             u.source_loaded_at DESC,
                             u.source DESC
                ) AS universe_rank
            FROM rebalances p
            JOIN universe_membership u
              ON u.universe_id = ?
             AND u.security_id = p.security_id
             AND u.valid_from <= p.trade_date
             AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
             AND u.as_of_date <= p.trade_date
             AND u.is_member
             AND u.is_latest_revision
             AND (u.available_at IS NULL OR u.available_at <= p.price_available_at)
        ),
        current_candidates AS (
            SELECT
                d.*,
                a.variant,
                a.fundamental_symbol,
                a.accession_number AS current_accession_number,
                a.period_end AS current_period_end,
                a.revenue AS current_revenue,
                a.revenue_id AS current_revenue_id,
                a.revenue_available_at AS current_revenue_available_at,
                a.numerator AS current_numerator,
                a.numerator_id AS current_numerator_id,
                a.numerator_available_at AS current_numerator_available_at,
                a.event_available_at AS current_event_available_at,
                row_number() OVER (
                    PARTITION BY a.variant, d.security_id, d.trade_date
                    ORDER BY a.period_end DESC,
                             a.event_available_at DESC,
                             a.accession_number DESC
                ) AS current_rank
            FROM governed_rebalances d
            JOIN variant_events a
              ON a.security_id = d.security_id
             AND a.period_end <= d.trade_date
             AND a.event_available_at <= d.price_available_at
             AND d.trade_date - a.period_end <= ?
            WHERE d.universe_rank = 1
        ),
        current_annual AS (
            SELECT * EXCLUDE (current_rank)
            FROM current_candidates
            WHERE current_rank = 1
        ),
        prior_candidates AS (
            SELECT
                c.*,
                p.accession_number AS prior_accession_number,
                p.period_end AS prior_period_end,
                p.revenue AS prior_revenue,
                p.revenue_id AS prior_revenue_id,
                p.revenue_available_at AS prior_revenue_available_at,
                p.numerator AS prior_numerator,
                p.numerator_id AS prior_numerator_id,
                p.numerator_available_at AS prior_numerator_available_at,
                p.event_available_at AS prior_event_available_at,
                row_number() OVER (
                    PARTITION BY c.variant, c.security_id, c.trade_date
                    ORDER BY abs((c.current_period_end - p.period_end) - 365),
                             p.period_end DESC,
                             p.event_available_at DESC,
                             p.accession_number DESC
                ) AS prior_rank
            FROM current_annual c
            JOIN variant_events p
              ON p.variant = c.variant
             AND p.security_id = c.security_id
             AND p.period_end < c.current_period_end
             AND c.current_period_end - p.period_end BETWEEN 300 AND 430
             AND p.event_available_at <= c.price_available_at
        )
        SELECT
            * EXCLUDE (month_rank, universe_rank, prior_rank),
            greatest(
                price_available_at,
                universe_available_at,
                current_event_available_at,
                prior_event_available_at
            ) AS decision_available_at
        FROM prior_candidates
        WHERE prior_rank = 1
        ORDER BY variant, trade_date, security_id
    """
    return store.con.execute(
        sql,
        [*date_params, options.universe_id, options.maximum_fundamental_age_days],
    ).df()


def _lineage(row: pd.Series, options: AnnualMarginChangeOptions) -> str:
    metric = VARIANT_METRICS[row["variant"]]
    return json_dumps(
        {
            "method": "annual_profit_margin_change_pit",
            "variant": row["variant"],
            "formula": f"{metric}_t/revenue_t-{metric}_t_1/revenue_t_1",
            "orientation": "higher_margin_change_is_preferred",
            "research_contract": {
                "annual_duration_days_inclusive": [330, 400],
                "annual_period_gap_days": [300, 430],
                "maximum_fundamental_age_days": options.maximum_fundamental_age_days,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "missing_components_imputed": False,
                "return_fitted_parameters": False,
            },
            "decision": {
                "trade_date": row["trade_date"],
                "available_at": row["decision_available_at"],
                "universe_id": row["universe_id"],
                "universe_valid_from": row["universe_valid_from"],
                "universe_valid_to": row["universe_valid_to"],
                "universe_available_at": row["universe_available_at"],
                "universe_source": row["universe_source"],
            },
            "current": {
                "accession_number": row["current_accession_number"],
                "period_end": row["current_period_end"],
                "revenue": {
                    "statement_point_id": row["current_revenue_id"],
                    "value": row["current_revenue"],
                    "available_at": row["current_revenue_available_at"],
                },
                metric: {
                    "statement_point_id": row["current_numerator_id"],
                    "value": row["current_numerator"],
                    "available_at": row["current_numerator_available_at"],
                },
                "margin": row["current_margin"],
            },
            "prior": {
                "accession_number": row["prior_accession_number"],
                "period_end": row["prior_period_end"],
                "revenue": {
                    "statement_point_id": row["prior_revenue_id"],
                    "value": row["prior_revenue"],
                    "available_at": row["prior_revenue_available_at"],
                },
                metric: {
                    "statement_point_id": row["prior_numerator_id"],
                    "value": row["prior_numerator"],
                    "available_at": row["prior_numerator_available_at"],
                },
                "margin": row["prior_margin"],
            },
            "change": row["raw_value"],
        }
    )


def compute_annual_margin_change_rows(
    inputs: pd.DataFrame,
    options: AnnualMarginChangeOptions | None = None,
) -> pd.DataFrame:
    """Compute, winsorize, and standardize every annual margin-change variant."""

    options = options or AnnualMarginChangeOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "variant",
        "security_id",
        "symbol",
        "trade_date",
        "decision_available_at",
        "current_revenue",
        "current_numerator",
        "prior_revenue",
        "prior_numerator",
        "current_revenue_id",
        "current_numerator_id",
        "prior_revenue_id",
        "prior_numerator_id",
        "current_accession_number",
        "prior_accession_number",
        "current_period_end",
        "prior_period_end",
        "current_revenue_available_at",
        "current_numerator_available_at",
        "prior_revenue_available_at",
        "prior_numerator_available_at",
        "universe_id",
        "universe_valid_from",
        "universe_valid_to",
        "universe_available_at",
        "universe_source",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Annual-margin-change inputs missing columns: {missing}")

    rows = inputs.copy()
    unknown_variants = sorted(set(rows["variant"].dropna()) - set(VARIANT_FACTOR_IDS))
    if unknown_variants:
        raise ValueError(f"Unknown annual-margin variants: {unknown_variants}")
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    numeric_columns = (
        "current_revenue",
        "current_numerator",
        "prior_revenue",
        "prior_numerator",
    )
    for column in numeric_columns:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=["variant", "security_id", "as_of_date", "available_at", *numeric_columns]
    )
    rows = rows[
        (rows["current_revenue"] > 0)
        & (rows["prior_revenue"] > 0)
        & rows["current_revenue"].map(math.isfinite)
        & rows["prior_revenue"].map(math.isfinite)
        & rows["current_numerator"].map(math.isfinite)
        & rows["prior_numerator"].map(math.isfinite)
    ].copy()
    rows["current_margin"] = rows["current_numerator"] / rows["current_revenue"]
    rows["prior_margin"] = rows["prior_numerator"] / rows["prior_revenue"]
    rows["raw_value"] = rows["current_margin"] - rows["prior_margin"]
    rows = rows[rows["raw_value"].map(math.isfinite)].copy()
    counts = rows.groupby(["variant", "as_of_date"])["security_id"].transform(
        "nunique"
    )
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = rows["variant"].map(VARIANT_FACTOR_IDS)
    rows["factor_name"] = rows["variant"].map(VARIANT_NAMES)
    rows["family"] = "fundamental_profitability"
    rows = winsorize(
        rows,
        value_column="raw_value",
        output_column="winsorized_value",
        partition_columns=("factor_id", "as_of_date"),
        limits=options.winsor_limit,
    )
    rows = zscore(
        rows,
        value_column="winsorized_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = rows["variant"].map(
        lambda variant: json_dumps(
            [
                "metric:revenue",
                f"metric:{VARIANT_METRICS[variant]}",
                f"universe:{options.universe_id}",
            ]
        )
    )
    rows["input_lineage_json"] = rows.apply(
        lambda row: _lineage(row, options), axis=1
    )
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, factor_id, security_id, as_of_date)
        for factor_id, security_id, as_of_date in zip(
            rows["factor_id"],
            rows["security_id"],
            rows["as_of_date"],
            strict=True,
        )
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["factor_id", "as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )
