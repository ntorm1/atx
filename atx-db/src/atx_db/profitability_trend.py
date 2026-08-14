"""Point-in-time eight-quarter trend in quarterly gross profitability."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .quarterly_gross_profitability import FACTOR_ID as QGP_FACTOR_ID
from .quarterly_gross_profitability import SOURCE_NAME as QGP_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT eight-quarter gross profitability trend v1"
FACTOR_ID = "profitability_quarterly_gross_profitability_trend_8q"
FACTOR_NAME = "PIT eight-quarter gross profitability trend"
FACTOR_FAMILY = "fundamental_profitability"
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
class ProfitabilityTrendOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_quarters: int = 8
    minimum_history_span_days: int = 580
    maximum_history_span_days: int = 1000
    minimum_seasonal_quarters: int = 3
    maximum_absolute_trend: float = 1.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_profitability_trend_inputs(
    store: DuckDBStore,
    options: ProfitabilityTrendOptions | None = None,
) -> pd.DataFrame:
    """Estimate an eight-quarter PIT OLS trend with seasonal-quarter fixed effects."""

    options = options or ProfitabilityTrendOptions()
    if options.minimum_quarters != 8:
        raise ValueError("profitability trend currently requires exactly eight quarters")
    date_predicates: list[str] = []
    date_params: list[object] = []
    if options.start_date is not None:
        date_predicates.append("as_of_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("as_of_date <= ?")
        date_params.append(options.end_date)
    date_sql = " AND " + " AND ".join(date_predicates) if date_predicates else ""
    return store.con.execute(
        f"""
        WITH parents AS (
            SELECT
                factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at,
                raw_value,
                TRY_CAST(
                    json_extract_string(
                        input_lineage_json,'$.quarterly_statement.period_end'
                    ) AS DATE
                ) AS period_end
            FROM fundamental_factor_values
            WHERE factor_id = ?
              AND source = ?
              AND is_latest_revision
              AND raw_value IS NOT NULL
              AND isfinite(raw_value)
        ),
        decisions AS (
            SELECT * FROM parents WHERE period_end IS NOT NULL {date_sql}
        ),
        candidates AS (
            SELECT
                d.factor_value_id AS decision_factor_value_id,
                d.security_id,
                d.symbol,
                d.as_of_date,
                d.available_at AS decision_available_at,
                h.factor_value_id AS history_factor_value_id,
                h.as_of_date AS history_as_of_date,
                h.available_at AS history_available_at,
                h.period_end,
                h.raw_value AS gross_profitability,
                row_number() OVER (
                    PARTITION BY d.factor_value_id,h.period_end
                    ORDER BY h.as_of_date DESC,h.available_at DESC,
                             h.factor_value_id DESC
                ) AS period_choice
            FROM decisions d
            JOIN parents h
              ON h.security_id = d.security_id
             AND h.period_end <= d.as_of_date
             AND h.as_of_date <= d.as_of_date
             AND h.available_at <= d.available_at
        ),
        ranked AS (
            SELECT
                * EXCLUDE (period_choice),
                row_number() OVER (
                    PARTITION BY decision_factor_value_id
                    ORDER BY period_end DESC
                ) AS history_rank
            FROM candidates
            WHERE period_choice = 1
        ),
        history AS (
            SELECT
                *,
                date_diff('day',DATE '1970-01-01',period_end)/91.3125 AS trend_t,
                quarter(period_end) AS seasonal_quarter
            FROM ranked
            WHERE history_rank <= 8
        ),
        demeaned AS (
            SELECT
                *,
                trend_t-avg(trend_t) OVER (
                    PARTITION BY decision_factor_value_id,seasonal_quarter
                ) AS residual_t,
                gross_profitability-avg(gross_profitability) OVER (
                    PARTITION BY decision_factor_value_id,seasonal_quarter
                ) AS residual_profitability
            FROM history
        ),
        estimates AS (
            SELECT
                decision_factor_value_id,
                any_value(security_id) AS security_id,
                any_value(symbol) AS symbol,
                any_value(as_of_date) AS as_of_date,
                any_value(decision_available_at) AS decision_available_at,
                min(period_end) AS oldest_period_end,
                max(period_end) AS latest_period_end,
                date_diff('day',min(period_end),max(period_end)) AS history_span_days,
                count(*) AS quarter_count,
                sum(residual_t*residual_profitability)
                    / nullif(sum(residual_t*residual_t),0) AS profitability_trend,
                to_json(list(struct_pack(
                    factor_value_id := history_factor_value_id,
                    period_end := period_end,
                    raw_value := gross_profitability,
                    as_of_date := history_as_of_date,
                    available_at := history_available_at
                ) ORDER BY history_rank DESC)) AS history_json
            FROM demeaned
            GROUP BY decision_factor_value_id
            HAVING count(*) = 8
               AND count(DISTINCT seasonal_quarter) >= ?
               AND date_diff('day',min(period_end),max(period_end)) BETWEEN ? AND ?
               AND sum(residual_t*residual_t) > 0
        )
        SELECT *
        FROM estimates
        WHERE isfinite(profitability_trend)
          AND abs(profitability_trend) <= ?
        ORDER BY as_of_date,security_id
        """,
        [
            QGP_FACTOR_ID,
            QGP_SOURCE_NAME,
            *date_params,
            options.minimum_seasonal_quarters,
            options.minimum_history_span_days,
            options.maximum_history_span_days,
            options.maximum_absolute_trend,
        ],
    ).df()


def _lineage(row: pd.Series, options: ProfitabilityTrendOptions) -> str:
    history = row.get("history_json")
    if isinstance(history, str):
        history = json.loads(history)
    return json_dumps(
        {
            "method": "akbas_jiang_koch_seasonally_adjusted_eight_quarter_trend_pit",
            "formula": "beta from GPQ=a+beta*elapsed_quarters+calendar-quarter fixed effects",
            "orientation": "higher_profitability_trend_is_preferred",
            "published_method_adaptations": {
                "gross_profitability": (
                    "Uses the governed PIT parent factor's quarterly gross profit divided "
                    "by one-quarter-lagged assets instead of contemporaneous assets."
                ),
                "timing": "Actual PIT availability replaces a fixed post-fiscal-quarter delay.",
                "filing_gaps": (
                    "Uses the latest eight visible distinct quarters within 1,000 days and "
                    "actual elapsed-quarter time rather than requiring a complete panel."
                ),
            },
            "decision": {
                "factor_value_id": row["decision_factor_value_id"],
                "as_of_date": row["as_of_date"],
                "available_at": row["decision_available_at"],
            },
            "history": history,
            "oldest_period_end": row.get("oldest_period_end"),
            "latest_period_end": row.get("latest_period_end"),
            "history_span_days": row.get("history_span_days"),
            "quarter_count": row.get("quarter_count"),
            "profitability_trend": row["raw_value"],
            "research_contract": {
                "minimum_quarters": options.minimum_quarters,
                "history_span_days": [
                    options.minimum_history_span_days,
                    options.maximum_history_span_days,
                ],
                "minimum_seasonal_quarters": options.minimum_seasonal_quarters,
                "maximum_absolute_trend": options.maximum_absolute_trend,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
        }
    )


def compute_profitability_trend_rows(
    inputs: pd.DataFrame,
    options: ProfitabilityTrendOptions | None = None,
) -> pd.DataFrame:
    """Guard, winsorize, and standardize point-in-time profitability trends."""

    options = options or ProfitabilityTrendOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "decision_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "profitability_trend",
        "history_json",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"profitability trend inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    rows["profitability_trend"] = pd.to_numeric(
        rows["profitability_trend"], errors="coerce"
    )
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", "profitability_trend"]
    )
    rows = rows[
        rows["profitability_trend"].map(math.isfinite)
        & (rows["profitability_trend"].abs() <= options.maximum_absolute_trend)
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["profitability_trend"].astype(float)
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
    rows["input_ids_json"] = json_dumps([f"factor:{QGP_FACTOR_ID}"])
    rows["input_lineage_json"] = rows.apply(
        lambda row: _lineage(row, options), axis=1
    )
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(
            rows["security_id"], rows["as_of_date"], strict=True
        )
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_profitability_trend_values(
    store: DuckDBStore,
    options: ProfitabilityTrendOptions | None = None,
) -> int:
    """Materialize the point-in-time eight-quarter profitability trend."""

    options = options or ProfitabilityTrendOptions()
    store.initialize()
    rows = compute_profitability_trend_rows(
        load_profitability_trend_inputs(store, options), options
    )
    predicates = ["source = ?", "factor_id = ?"]
    params: list[object] = [options.source, FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
            params,
        )
        if not rows.empty:
            insert_frame(
                store,
                rows,
                "fundamental_factor_values",
                "profitability_trend_insert",
            )
    return len(rows)
