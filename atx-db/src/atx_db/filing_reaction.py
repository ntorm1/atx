"""Day-level market-adjusted reaction after an SEC earnings filing.

Companyfacts provides a filed date with a synthetic 22:00 availability timestamp,
not the original intraday EDGAR acceptance time. The conservative event return is
therefore the first complete trading session strictly after the filed date.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .earnings_surprise import FACTOR_ID as SUE_FACTOR_ID
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT SEC filing reaction v1"
FACTOR_ID = "earnings_sec_filing_reaction"
FACTOR_NAME = "PIT market-adjusted SEC filing reaction"
FACTOR_FAMILY = "fundamental_earnings"

_OUTPUT_COLUMNS = [
    "factor_value_id", "factor_id", "factor_name", "family", "security_id",
    "symbol", "as_of_date", "raw_value", "value", "available_at",
    "input_ids_json", "input_lineage_json", "is_latest_revision", "run_id", "source",
]


@dataclass(frozen=True)
class FilingReactionOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    maximum_absolute_reaction: float = 0.50
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_filing_reaction_inputs(
    store: DuckDBStore,
    options: FilingReactionOptions | None = None,
) -> pd.DataFrame:
    """Attach a conservative post-filing session return to monthly SUE rows."""

    options = options or FilingReactionOptions()
    predicates = ["f.factor_id = ?", "f.is_latest_revision"]
    params: list[object] = [SUE_FACTOR_ID]
    if options.start_date is not None:
        predicates.append("f.as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("f.as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                arg_max("close", available_at) AS close_value,
                arg_max(split_factor, available_at) AS split_value,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE "close" > 0 AND trade_date IS NOT NULL AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
        ),
        daily AS (
            SELECT
                *,
                lag(close_value) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                ) AS prior_close,
                close_value / (
                    lag(close_value) OVER (
                        PARTITION BY security_id ORDER BY trade_date
                    ) * coalesce(nullif(split_value, 0), 1)
                ) - 1 AS daily_return
            FROM price_dedup
        ),
        market_return_by_date AS (
            SELECT trade_date, median(daily_return) AS market_return
            FROM daily
            WHERE isfinite(daily_return)
            GROUP BY trade_date
        ),
        sue_rows AS (
            SELECT
                f.*,
                json_extract_string(
                    f.input_lineage_json,
                    '$.current.statement_point_id'
                ) AS statement_point_id
            FROM fundamental_factor_values f
            WHERE {' AND '.join(predicates)}
        ),
        event_rows AS (
            SELECT DISTINCT
                s.statement_point_id,
                s.security_id,
                s.as_of_date AS filed_date,
                s.available_at AS filing_available_at,
                s.accession_number,
                s.form
            FROM (
                SELECT DISTINCT statement_point_id FROM sue_rows
            ) keys
            JOIN fundamental_statement_points s USING (statement_point_id)
        ),
        reaction_candidates AS (
            SELECT
                e.*,
                d.trade_date AS reaction_date,
                d.price_available_at AS reaction_available_at,
                d.prior_close,
                d.close_value AS reaction_close,
                d.split_value AS reaction_adjustment_factor,
                d.daily_return,
                m.market_return,
                d.daily_return - m.market_return AS abnormal_return,
                row_number() OVER (
                    PARTITION BY e.statement_point_id ORDER BY d.trade_date
                ) AS reaction_rank
            FROM event_rows e
            JOIN daily d
              ON d.security_id = e.security_id
             AND d.trade_date > e.filed_date
             AND d.trade_date <= e.filed_date + 7
            JOIN market_return_by_date m USING (trade_date)
            WHERE isfinite(d.daily_return)
        ),
        reactions AS (
            SELECT * EXCLUDE (reaction_rank)
            FROM reaction_candidates
            WHERE reaction_rank = 1
        )
        SELECT
            f.factor_value_id AS sue_factor_value_id,
            f.security_id,
            f.symbol,
            f.as_of_date,
            f.available_at AS decision_available_at,
            f.value AS sue_value,
            f.statement_point_id,
            r.* EXCLUDE (statement_point_id, security_id)
        FROM sue_rows f
        JOIN reactions r USING (statement_point_id, security_id)
        WHERE r.reaction_date <= f.as_of_date
          AND r.reaction_available_at <= f.available_at
          AND abs(r.abnormal_return) <= ?
        ORDER BY f.as_of_date, f.security_id
        """,
        [*params, options.maximum_absolute_reaction],
    ).df()


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "event_semantics": "first_complete_trading_session_strictly_after_sec_filed_date",
            "timestamp_limitation": "companyfacts filed date normalized to synthetic 22:00",
            "sue_factor_value_id": row["sue_factor_value_id"],
            "statement_point_id": row["statement_point_id"],
            "accession_number": row["accession_number"],
            "form": row["form"],
            "filed_date": row["filed_date"],
            "filing_available_at": row["filing_available_at"],
            "reaction": {
                "trade_date": row["reaction_date"],
                "available_at": row["reaction_available_at"],
                "prior_close": row["prior_close"],
                "close": row["reaction_close"],
                "adjustment_factor": row["reaction_adjustment_factor"],
                "security_return": row["daily_return"],
                "cross_sectional_median_return": row["market_return"],
                "abnormal_return": row["abnormal_return"],
            },
        }
    )


def compute_filing_reaction_rows(
    inputs: pd.DataFrame,
    options: FilingReactionOptions | None = None,
) -> pd.DataFrame:
    """Convert post-filing abnormal returns to monthly standardized scores."""

    options = options or FilingReactionOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id", "symbol", "as_of_date", "decision_available_at", "abnormal_return"
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"filing reaction inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    rows["abnormal_return"] = pd.to_numeric(rows["abnormal_return"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "abnormal_return"])
    rows = rows[rows["abnormal_return"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["abnormal_return"].astype(float)
    rows = winsorize(
        rows, value_column="raw_value", output_column="winsorized_value",
        partition_columns=("factor_id", "as_of_date"), limits=options.winsor_limit,
    )
    rows = zscore(
        rows, value_column="winsorized_value", output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [f"factor:{SUE_FACTOR_ID}", "market:post_filing_session_return"]
    )
    rows["input_lineage_json"] = rows.apply(_lineage, axis=1)
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(rows["security_id"], rows["as_of_date"], strict=True)
    ]
    return (
        rows[_OUTPUT_COLUMNS].dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable").reset_index(drop=True)
    )


def refresh_filing_reaction_values(
    store: DuckDBStore,
    options: FilingReactionOptions | None = None,
) -> int:
    """Materialize the conservative post-SEC-filing reaction factor."""

    options = options or FilingReactionOptions()
    store.initialize()
    rows = compute_filing_reaction_rows(load_filing_reaction_inputs(store, options), options)
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
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}", params
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "filing_reaction_values_insert")
    return len(rows)
