"""Point-in-time earnings surprise residualized against 12-1 price momentum."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .earnings_surprise import FACTOR_ID as SUE_FACTOR_ID
from .earnings_surprise import SOURCE_NAME as SUE_SOURCE_NAME
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT price-controlled fundamental momentum v1"
FACTOR_ID = "earnings_sue_price_momentum_residual_12_1"
FACTOR_NAME = "PIT SUE residual to 12-1 price momentum"
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
class FundamentalMomentumOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    skip_sessions: int = 21
    lookback_sessions: int = 252
    maximum_reference_staleness_days: int = 7
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None

    def __post_init__(self) -> None:
        if self.skip_sessions < 1:
            raise ValueError("skip_sessions must be positive")
        if self.lookback_sessions <= self.skip_sessions:
            raise ValueError("lookback_sessions must exceed skip_sessions")
        if self.maximum_reference_staleness_days < 0:
            raise ValueError("maximum_reference_staleness_days cannot be negative")
        if self.minimum_names_per_date < 3:
            raise ValueError("minimum_names_per_date must be at least 3")
        if not 0 <= self.winsor_limit < 0.5:
            raise ValueError("winsor_limit must be in [0, 0.5)")


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_fundamental_momentum_inputs(
    store: DuckDBStore,
    options: FundamentalMomentumOptions | None = None,
) -> pd.DataFrame:
    """Load governed SUE and split-adjusted 12-1 momentum on the SUE decision grid."""

    options = options or FundamentalMomentumOptions()
    predicates = [
        "factor_id = ?",
        "source = ?",
        "is_latest_revision",
        "value IS NOT NULL",
        "isfinite(value)",
    ]
    params: list[object] = [SUE_FACTOR_ID, SUE_SOURCE_NAME]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)

    skip = options.skip_sessions
    lookback = options.lookback_sessions
    staleness = options.maximum_reference_staleness_days
    return store.con.execute(
        f"""
        WITH sue_revisions AS (
            SELECT
                factor_value_id AS sue_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                value AS sue_value,
                available_at AS sue_available_at,
                input_lineage_json AS sue_lineage_json,
                row_number() OVER (
                    PARTITION BY security_id, as_of_date
                    ORDER BY available_at DESC, source_loaded_at DESC, factor_value_id DESC
                ) AS revision_rank
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        ),
        sue AS (
            SELECT * EXCLUDE (revision_rank)
            FROM sue_revisions
            WHERE revision_rank = 1
        ),
        relevant_securities AS (
            SELECT DISTINCT security_id FROM sue
        ),
        canonical_bars AS (
            SELECT
                b.security_id,
                b.trade_date,
                b.close,
                CASE
                    WHEN b.split_factor IS NOT NULL
                     AND isfinite(b.split_factor)
                     AND b.split_factor > 0
                    THEN ln(b.split_factor)
                    ELSE 0.0
                END AS log_adjustment,
                b.available_at,
                row_number() OVER (
                    PARTITION BY b.security_id, b.trade_date
                    ORDER BY b.source_loaded_at DESC, b.source DESC
                ) AS bar_revision_rank
            FROM equity_daily_bars b
            JOIN relevant_securities r USING (security_id)
            WHERE b.close IS NOT NULL AND isfinite(b.close) AND b.close > 0
              AND b.available_at IS NOT NULL
        ),
        adjusted_bars AS (
            SELECT
                security_id,
                trade_date,
                close,
                available_at,
                sum(log_adjustment) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                ) AS cumulative_log_adjustment
            FROM canonical_bars
            WHERE bar_revision_rank = 1
        ),
        momentum_lags AS (
            SELECT
                security_id,
                trade_date AS reference_trade_date,
                available_at AS reference_available_at,
                lag(trade_date, {skip}) OVER security_history AS momentum_end_date,
                lag(available_at, {skip}) OVER security_history AS momentum_end_available_at,
                lag(close, {skip}) OVER security_history AS momentum_end_close,
                lag(cumulative_log_adjustment, {skip}) OVER security_history AS momentum_end_adjustment,
                lag(trade_date, {lookback}) OVER security_history AS momentum_start_date,
                lag(available_at, {lookback}) OVER security_history AS momentum_start_available_at,
                lag(close, {lookback}) OVER security_history AS momentum_start_close,
                lag(cumulative_log_adjustment, {lookback}) OVER security_history AS momentum_start_adjustment
            FROM adjusted_bars
            WINDOW security_history AS (PARTITION BY security_id ORDER BY trade_date)
        ),
        momentum AS (
            SELECT
                *,
                momentum_end_close
                    / momentum_start_close
                    / exp(momentum_end_adjustment - momentum_start_adjustment)
                    - 1.0 AS price_momentum_12_1
            FROM momentum_lags
            WHERE momentum_start_close > 0 AND momentum_end_close > 0
        ),
        paired AS (
            SELECT
                s.*,
                m.reference_trade_date,
                m.reference_available_at,
                m.momentum_end_date,
                m.momentum_end_available_at,
                m.momentum_end_close,
                m.momentum_start_date,
                m.momentum_start_available_at,
                m.momentum_start_close,
                m.price_momentum_12_1
            FROM sue s
            ASOF LEFT JOIN momentum m
              ON s.security_id = m.security_id
             AND s.as_of_date >= m.reference_trade_date
        )
        SELECT
            *,
            greatest(
                sue_available_at,
                reference_available_at,
                momentum_end_available_at,
                momentum_start_available_at
            ) AS decision_available_at
        FROM paired
        WHERE reference_trade_date IS NOT NULL
          AND date_diff('day', reference_trade_date, as_of_date) BETWEEN 0 AND {staleness}
          AND reference_available_at <= as_of_date + INTERVAL 1 DAY
          AND price_momentum_12_1 IS NOT NULL
          AND isfinite(price_momentum_12_1)
          AND price_momentum_12_1 > -1.0
        ORDER BY as_of_date, security_id
        """,
        params,
    ).df()


def _decode_json(value: object) -> object:
    if not isinstance(value, str):
        return value
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return value


def _lineage(row: pd.Series, options: FundamentalMomentumOptions) -> str:
    return json_dumps(
        {
            "method": "cross_sectional_rank_ols_residual",
            "response": "standardized_unexpected_eps_rank",
            "control": "split_adjusted_price_momentum_12_1_rank",
            "missing_control_policy": "drop",
            "skip_sessions": options.skip_sessions,
            "lookback_sessions": options.lookback_sessions,
            "sue": {
                "factor_value_id": row["sue_factor_value_id"],
                "value": row["sue_value"],
                "rank": row["sue_rank"],
                "available_at": row["sue_available_at"],
                "lineage": _decode_json(row.get("sue_lineage_json")),
            },
            "price_momentum": {
                "value": row["price_momentum_12_1"],
                "rank": row["momentum_rank"],
                "reference_trade_date": row["reference_trade_date"],
                "reference_available_at": row["reference_available_at"],
                "end_date": row["momentum_end_date"],
                "end_available_at": row["momentum_end_available_at"],
                "end_close": row["momentum_end_close"],
                "start_date": row["momentum_start_date"],
                "start_available_at": row["momentum_start_available_at"],
                "start_close": row["momentum_start_close"],
            },
            "cross_sectional_regression": {
                "intercept": row["regression_intercept"],
                "beta": row["regression_beta"],
                "observations": row["cross_section_names"],
                "residual": row["raw_value"],
            },
        }
    )


def compute_fundamental_momentum_rows(
    inputs: pd.DataFrame,
    options: FundamentalMomentumOptions | None = None,
) -> pd.DataFrame:
    """Residualize SUE rank against contemporaneously observable 12-1 momentum rank."""

    options = options or FundamentalMomentumOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "sue_value",
        "price_momentum_12_1",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"fundamental momentum inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    for column in ("sue_value", "price_momentum_12_1"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "sue_value",
            "price_momentum_12_1",
        ]
    )
    finite = rows["sue_value"].map(math.isfinite) & rows[
        "price_momentum_12_1"
    ].map(math.isfinite)
    rows = rows[finite].copy()
    rows["cross_section_names"] = rows.groupby("as_of_date")[
        "security_id"
    ].transform("nunique")
    rows = rows[
        rows["cross_section_names"] >= options.minimum_names_per_date
    ].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    grouped = rows.groupby("as_of_date", sort=False)
    rows["sue_rank"] = grouped["sue_value"].rank(method="average", pct=True)
    rows["momentum_rank"] = grouped["price_momentum_12_1"].rank(
        method="average", pct=True
    )
    x_mean = rows.groupby("as_of_date")["momentum_rank"].transform("mean")
    y_mean = rows.groupby("as_of_date")["sue_rank"].transform("mean")
    x_centered = rows["momentum_rank"] - x_mean
    y_centered = rows["sue_rank"] - y_mean
    numerator = (x_centered * y_centered).groupby(rows["as_of_date"]).transform("sum")
    denominator = x_centered.pow(2).groupby(rows["as_of_date"]).transform("sum")
    rows["regression_beta"] = numerator / denominator.where(denominator > 1e-12)
    rows["regression_intercept"] = y_mean - rows["regression_beta"] * x_mean
    rows["raw_value"] = rows["sue_rank"] - (
        rows["regression_intercept"]
        + rows["regression_beta"] * rows["momentum_rank"]
    )
    rows = rows[rows["raw_value"].map(math.isfinite)].copy()
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows = winsorize(
        rows,
        value_column="raw_value",
        output_column="raw_value",
        partition_columns=("factor_id", "as_of_date"),
        limits=options.winsor_limit,
    )
    rows = zscore(
        rows,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [f"factor:{SUE_FACTOR_ID}", "market:equity_daily_bars"]
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


def refresh_fundamental_momentum_values(
    store: DuckDBStore,
    options: FundamentalMomentumOptions | None = None,
) -> int:
    """Materialize price-controlled fundamental momentum."""

    options = options or FundamentalMomentumOptions()
    store.initialize()
    rows = compute_fundamental_momentum_rows(
        load_fundamental_momentum_inputs(store, options), options
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
                "fundamental_momentum_insert",
            )
    return len(rows)
