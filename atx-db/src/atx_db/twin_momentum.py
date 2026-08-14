"""Point-in-time confirmation between profitability trend and 12-1 momentum."""

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
from .profitability_trend import FACTOR_ID as TREND_FACTOR_ID
from .profitability_trend import SOURCE_NAME as TREND_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT twin profitability-price momentum v1"
FACTOR_ID = "momentum_twin_profitability_trend_price_12_1"
FACTOR_NAME = "PIT twin profitability-trend and price momentum"
FACTOR_FAMILY = "fundamental_momentum"

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
class TwinMomentumOptions:
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


def load_twin_momentum_inputs(
    store: DuckDBStore,
    options: TwinMomentumOptions | None = None,
) -> pd.DataFrame:
    """Load the governed profitability trend and split-adjusted 12-1 momentum."""

    options = options or TwinMomentumOptions()
    predicates = [
        "factor_id = ?",
        "source = ?",
        "is_latest_revision",
        "value IS NOT NULL",
        "isfinite(value)",
    ]
    params: list[object] = [TREND_FACTOR_ID, TREND_SOURCE_NAME]
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
        WITH parent_revisions AS (
            SELECT
                factor_value_id AS trend_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                value AS trend_value,
                available_at AS trend_available_at,
                input_lineage_json AS trend_lineage_json,
                row_number() OVER (
                    PARTITION BY security_id, as_of_date
                    ORDER BY available_at DESC, source_loaded_at DESC, factor_value_id DESC
                ) AS revision_rank
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        ),
        parents AS (
            SELECT * EXCLUDE (revision_rank)
            FROM parent_revisions
            WHERE revision_rank = 1
        ),
        relevant_securities AS (
            SELECT DISTINCT security_id FROM parents
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
                p.*,
                m.reference_trade_date,
                m.reference_available_at,
                m.momentum_end_date,
                m.momentum_end_available_at,
                m.momentum_end_close,
                m.momentum_start_date,
                m.momentum_start_available_at,
                m.momentum_start_close,
                m.price_momentum_12_1
            FROM parents p
            ASOF LEFT JOIN momentum m
              ON p.security_id = m.security_id
             AND p.as_of_date >= m.reference_trade_date
        )
        SELECT
            *,
            greatest(
                trend_available_at,
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


def _lineage(row: pd.Series, options: TwinMomentumOptions) -> str:
    return json_dumps(
        {
            "method": "same_direction_rank_confirmation_minimum_strength",
            "formula": "sign(F)*min(abs(F),abs(P)) when sign(F)=sign(P), otherwise 0",
            "published_method_adaptations": {
                "fundamental_leg": (
                    "Uses the governed PIT eight-quarter quarterly gross-profitability trend "
                    "as a parsimonious, out-of-sample-fitted-parameter-free proxy for the "
                    "paper's fundamental implied-return forecast."
                ),
                "portfolio_encoding": (
                    "Continuous same-direction confirmation preserves the paper's top/top and "
                    "bottom/bottom twin-sort tails while allowing governed construction search."
                ),
            },
            "skip_sessions": options.skip_sessions,
            "lookback_sessions": options.lookback_sessions,
            "profitability_trend": {
                "factor_value_id": row["trend_factor_value_id"],
                "value": row["trend_value"],
                "rank": row["fundamental_rank"],
                "centered_rank": row["fundamental_score"],
                "available_at": row["trend_available_at"],
                "lineage": _decode_json(row.get("trend_lineage_json")),
            },
            "price_momentum": {
                "value": row["price_momentum_12_1"],
                "rank": row["momentum_rank"],
                "centered_rank": row["momentum_score"],
                "reference_trade_date": row["reference_trade_date"],
                "reference_available_at": row["reference_available_at"],
                "end_date": row["momentum_end_date"],
                "end_available_at": row["momentum_end_available_at"],
                "end_close": row["momentum_end_close"],
                "start_date": row["momentum_start_date"],
                "start_available_at": row["momentum_start_available_at"],
                "start_close": row["momentum_start_close"],
            },
            "cross_section_names": row["cross_section_names"],
            "confirmation_value": row["raw_value"],
        }
    )


def compute_twin_momentum_rows(
    inputs: pd.DataFrame,
    options: TwinMomentumOptions | None = None,
) -> pd.DataFrame:
    """Encode agreement between the fundamental and price-momentum ranks."""

    options = options or TwinMomentumOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "trend_value",
        "price_momentum_12_1",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"twin momentum inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    for column in ("trend_value", "price_momentum_12_1"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", "trend_value", "price_momentum_12_1"]
    )
    rows = rows[
        rows["trend_value"].map(math.isfinite)
        & rows["price_momentum_12_1"].map(math.isfinite)
    ].copy()
    rows["cross_section_names"] = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[rows["cross_section_names"] >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    grouped = rows.groupby("as_of_date", sort=False)
    rows["fundamental_rank"] = grouped["trend_value"].rank(method="average", pct=True)
    rows["momentum_rank"] = grouped["price_momentum_12_1"].rank(method="average", pct=True)
    rows["fundamental_score"] = 2.0 * rows["fundamental_rank"] - 1.0
    rows["momentum_score"] = 2.0 * rows["momentum_rank"] - 1.0
    agrees = rows["fundamental_score"] * rows["momentum_score"] > 0
    strength = pd.concat(
        [rows["fundamental_score"].abs(), rows["momentum_score"].abs()], axis=1
    ).min(axis=1)
    rows["raw_value"] = 0.0
    rows.loc[agrees, "raw_value"] = (
        rows.loc[agrees, "fundamental_score"].map(lambda value: math.copysign(1.0, value))
        * strength.loc[agrees]
    )
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
        [f"factor:{TREND_FACTOR_ID}", "market:equity_daily_bars"]
    )
    rows["input_lineage_json"] = rows.apply(lambda row: _lineage(row, options), axis=1)
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(rows["security_id"], rows["as_of_date"], strict=True)
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_twin_momentum_values(
    store: DuckDBStore,
    options: TwinMomentumOptions | None = None,
) -> int:
    """Materialize PIT twin momentum."""

    options = options or TwinMomentumOptions()
    store.initialize()
    rows = compute_twin_momentum_rows(load_twin_momentum_inputs(store, options), options)
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
            insert_frame(store, rows, "fundamental_factor_values", "twin_momentum_insert")
    return len(rows)
