"""Point-in-time price-deflated quarterly earnings acceleration."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .earnings_surprise import FACTOR_ID as SUE_FACTOR_ID
from .earnings_surprise import SOURCE_NAME as SUE_SOURCE
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT price-deflated earnings acceleration v1"
FACTOR_ID = "earnings_quarterly_acceleration"
FACTOR_NAME = "PIT quarterly earnings acceleration"
FACTOR_FAMILY = "fundamental_earnings"
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
class EarningsAccelerationOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_signal_age_days: int = 150
    maximum_absolute_acceleration: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_earnings_acceleration_inputs(
    store: DuckDBStore,
    options: EarningsAccelerationOptions | None = None,
) -> pd.DataFrame:
    """Build first-filed quarterly acceleration on the governed SUE decision grid."""

    options = options or EarningsAccelerationOptions()
    predicates = ["factor_id = ?", "source = ?", "is_latest_revision"]
    params: list[object] = [SUE_FACTOR_ID, SUE_SOURCE]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH quarter_candidates AS (
            SELECT
                statement_point_id,
                security_id,
                symbol,
                period_start,
                period_end,
                available_at,
                accession_number,
                value AS eps_diluted,
                row_number() OVER (
                    PARTITION BY security_id,period_end
                    ORDER BY available_at,revision_sequence,statement_point_id
                ) AS first_period_rank
            FROM fundamental_statement_points
            WHERE canonical_metric = 'eps_diluted'
              AND unit_type = 'per_share'
              AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 70 AND 115
              AND value IS NOT NULL
              AND isfinite(value)
              AND available_at IS NOT NULL
              AND revision_sequence = 1
              AND form IN ('10-Q','10-Q/A','10-K','10-K/A')
        ),
        price_dedup AS (
            SELECT
                security_id,
                trade_date,
                arg_max("close",available_at) AS "close",
                arg_max(split_factor,available_at) AS split_factor,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (
                SELECT DISTINCT security_id FROM quarter_candidates
            )
              AND "close" > 0
              AND trade_date IS NOT NULL
              AND available_at IS NOT NULL
            GROUP BY security_id,trade_date
        ),
        price_features AS (
            SELECT
                *,
                product(
                    CASE
                        WHEN split_factor > 0
                         AND (split_factor <= 0.8 OR split_factor >= 1.25)
                        THEN split_factor
                        ELSE 1.0
                    END
                ) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                ) AS split_index
            FROM price_dedup
        ),
        quarters_with_market AS (
            SELECT
                q.* EXCLUDE (first_period_rank),
                p.trade_date AS period_price_date,
                p.price_available_at AS period_price_available_at,
                p."close" AS period_end_price,
                p.split_index,
                q.eps_diluted/nullif(p.split_index,0) AS adjusted_eps,
                p."close"/nullif(p.split_index,0) AS adjusted_period_end_price
            FROM quarter_candidates q
            ASOF JOIN price_features p
              ON q.security_id = p.security_id
             AND q.period_end >= p.trade_date
            WHERE q.first_period_rank = 1
        ),
        sequenced AS (
            SELECT
                *,
                lag(statement_point_id,1) OVER quarter_window AS lag1_statement_point_id,
                lag(statement_point_id,2) OVER quarter_window AS lag2_statement_point_id,
                lag(statement_point_id,4) OVER quarter_window AS lag4_statement_point_id,
                lag(statement_point_id,5) OVER quarter_window AS lag5_statement_point_id,
                lag(period_end,1) OVER quarter_window AS lag1_period_end,
                lag(period_end,2) OVER quarter_window AS lag2_period_end,
                lag(period_end,4) OVER quarter_window AS lag4_period_end,
                lag(period_end,5) OVER quarter_window AS lag5_period_end,
                lag(available_at,1) OVER quarter_window AS lag1_available_at,
                lag(available_at,4) OVER quarter_window AS lag4_available_at,
                lag(available_at,5) OVER quarter_window AS lag5_available_at,
                lag(accession_number,1) OVER quarter_window AS lag1_accession_number,
                lag(accession_number,4) OVER quarter_window AS lag4_accession_number,
                lag(accession_number,5) OVER quarter_window AS lag5_accession_number,
                lag(eps_diluted,1) OVER quarter_window AS lag1_eps_diluted,
                lag(eps_diluted,4) OVER quarter_window AS lag4_eps_diluted,
                lag(eps_diluted,5) OVER quarter_window AS lag5_eps_diluted,
                lag(adjusted_eps,1) OVER quarter_window AS lag1_adjusted_eps,
                lag(adjusted_eps,4) OVER quarter_window AS lag4_adjusted_eps,
                lag(adjusted_eps,5) OVER quarter_window AS lag5_adjusted_eps,
                lag(split_index,1) OVER quarter_window AS lag1_split_index,
                lag(split_index,4) OVER quarter_window AS lag4_split_index,
                lag(split_index,5) OVER quarter_window AS lag5_split_index,
                lag(period_price_date,1) OVER quarter_window AS lag1_price_date,
                lag(period_price_date,2) OVER quarter_window AS lag2_price_date,
                lag(period_price_available_at,1) OVER quarter_window
                    AS lag1_price_available_at,
                lag(period_price_available_at,2) OVER quarter_window
                    AS lag2_price_available_at,
                lag(period_end_price,1) OVER quarter_window AS lag1_period_end_price,
                lag(period_end_price,2) OVER quarter_window AS lag2_period_end_price,
                lag(adjusted_period_end_price,1) OVER quarter_window
                    AS lag1_adjusted_period_end_price,
                lag(adjusted_period_end_price,2) OVER quarter_window
                    AS lag2_adjusted_period_end_price
            FROM quarters_with_market
            WINDOW quarter_window AS (
                PARTITION BY security_id ORDER BY period_end,available_at,statement_point_id
            )
        ),
        acceleration_signals AS (
            SELECT
                *,
                (adjusted_eps-lag4_adjusted_eps)
                    /nullif(lag1_adjusted_period_end_price,0) AS current_earnings_growth,
                (lag1_adjusted_eps-lag5_adjusted_eps)
                    /nullif(lag2_adjusted_period_end_price,0) AS prior_earnings_growth,
                current_earnings_growth-prior_earnings_growth
                    AS earnings_acceleration
            FROM sequenced
            WHERE period_end-lag1_period_end BETWEEN 60 AND 130
              AND period_end-lag4_period_end BETWEEN 350 AND 380
              AND lag1_period_end-lag5_period_end BETWEEN 350 AND 380
              AND lag1_available_at <= available_at
              AND lag4_available_at <= available_at
              AND lag5_available_at <= available_at
              AND lag1_price_available_at <= available_at
              AND lag2_price_available_at <= available_at
        ),
        decisions AS (
            SELECT
                factor_value_id AS sue_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS decision_available_at
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        ),
        matched AS (
            SELECT
                d.*,
                a.* EXCLUDE (security_id,symbol),
                row_number() OVER (
                    PARTITION BY d.sue_factor_value_id
                    ORDER BY a.available_at DESC,a.period_end DESC,a.statement_point_id DESC
                ) AS signal_rank
            FROM decisions d
            JOIN acceleration_signals a
              ON a.security_id = d.security_id
             AND a.period_end <= d.as_of_date
             AND a.available_at <= d.decision_available_at
             AND d.as_of_date-a.period_end <= ?
            WHERE isfinite(a.earnings_acceleration)
              AND abs(a.earnings_acceleration) <= ?
        )
        SELECT * EXCLUDE (signal_rank)
        FROM matched
        WHERE signal_rank = 1
        ORDER BY as_of_date,security_id
        """,
        [
            *params,
            options.maximum_signal_age_days,
            options.maximum_absolute_acceleration,
        ],
    ).df()


def _quarter_lineage(row: pd.Series, prefix: str) -> dict[str, Any]:
    if prefix == "current":
        return {
            "statement_point_id": row["statement_point_id"],
            "period_end": row["period_end"],
            "available_at": row["available_at"],
            "accession_number": row["accession_number"],
            "eps_diluted": row["eps_diluted"],
            "split_index": row["split_index"],
            "adjusted_eps": row["adjusted_eps"],
        }
    return {
        "statement_point_id": row[f"{prefix}_statement_point_id"],
        "period_end": row[f"{prefix}_period_end"],
        "available_at": row.get(f"{prefix}_available_at"),
        "accession_number": row.get(f"{prefix}_accession_number"),
        "eps_diluted": row.get(f"{prefix}_eps_diluted"),
        "split_index": row.get(f"{prefix}_split_index"),
        "adjusted_eps": row.get(f"{prefix}_adjusted_eps"),
    }


def _lineage(row: pd.Series, options: EarningsAccelerationOptions) -> str:
    return json_dumps(
        {
            "method": "he_narayanamoorthy_price_deflated_earnings_acceleration_pit",
            "formula": (
                "((adjusted_eps_t-adjusted_eps_t_4)/adjusted_price_t_1)"
                "-((adjusted_eps_t_1-adjusted_eps_t_5)/adjusted_price_t_2)"
            ),
            "orientation": "higher_acceleration_is_preferred",
            "revision_policy": "first_filed_period_value_only",
            "split_policy": {
                "normalization": "eps_and_price_divided_by_cumulative_split_index",
                "included_factors": "split_factor <= 0.8 or >= 1.25",
                "excluded": "small dividend adjustment factors",
            },
            "quarters": {
                "current": _quarter_lineage(row, "current"),
                "lag1": _quarter_lineage(row, "lag1"),
                "lag4": _quarter_lineage(row, "lag4"),
                "lag5": _quarter_lineage(row, "lag5"),
            },
            "deflators": {
                "lag1": {
                    "price_date": row["lag1_price_date"],
                    "available_at": row["lag1_price_available_at"],
                    "raw_price": row["lag1_period_end_price"],
                    "adjusted_price": row["lag1_adjusted_period_end_price"],
                },
                "lag2": {
                    "price_date": row["lag2_price_date"],
                    "available_at": row["lag2_price_available_at"],
                    "raw_price": row["lag2_period_end_price"],
                    "adjusted_price": row["lag2_adjusted_period_end_price"],
                },
            },
            "current_earnings_growth": row["current_earnings_growth"],
            "prior_earnings_growth": row["prior_earnings_growth"],
            "earnings_acceleration": row["earnings_acceleration"],
            "decision": {
                "sue_factor_id": SUE_FACTOR_ID,
                "sue_factor_value_id": row["sue_factor_value_id"],
                "as_of_date": row["as_of_date"],
                "available_at": row["decision_available_at"],
            },
            "research_contract": {
                "maximum_signal_age_days": options.maximum_signal_age_days,
                "maximum_absolute_acceleration": options.maximum_absolute_acceleration,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
        }
    )


def compute_earnings_acceleration_rows(
    inputs: pd.DataFrame,
    options: EarningsAccelerationOptions | None = None,
) -> pd.DataFrame:
    """Winsorize and standardize point-in-time earnings acceleration."""

    options = options or EarningsAccelerationOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "sue_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "earnings_acceleration",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"earnings acceleration inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    rows["earnings_acceleration"] = pd.to_numeric(
        rows["earnings_acceleration"], errors="coerce"
    )
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", "earnings_acceleration"]
    )
    rows = rows[
        rows["earnings_acceleration"].map(math.isfinite)
        & (rows["earnings_acceleration"].abs() <= options.maximum_absolute_acceleration)
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["earnings_acceleration"].astype(float)
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
    rows["input_ids_json"] = json_dumps(
        [f"factor:{SUE_FACTOR_ID}", "metric:eps_diluted_quarterly", "market:close"]
    )
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


def refresh_earnings_acceleration_values(
    store: DuckDBStore,
    options: EarningsAccelerationOptions | None = None,
) -> int:
    """Materialize the point-in-time earnings-acceleration factor."""

    options = options or EarningsAccelerationOptions()
    store.initialize()
    rows = compute_earnings_acceleration_rows(
        load_earnings_acceleration_inputs(store, options), options
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
                "earnings_acceleration_insert",
            )
    return len(rows)
