"""Point-in-time quarterly return-on-equity profitability factor."""

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
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT q-factor ROE v1"
FACTOR_ID = "profitability_q_factor_roe"
FACTOR_NAME = "PIT q-factor quarterly ROE"
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
class QuarterlyRoeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_earnings_age_days: int = 200
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: QuarterlyRoeOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_quarterly_roe_inputs(
    store: DuckDBStore,
    options: QuarterlyRoeOptions | None = None,
) -> pd.DataFrame:
    """Resolve the latest announced quarter and one-quarter-lagged equity."""

    options = options or QuarterlyRoeOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH quarterly_earnings AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_start,
                period_end,
                arg_max(
                    value,
                    (available_at, revision_sequence, statement_point_id)
                ) AS quarterly_net_income,
                arg_max(
                    statement_point_id,
                    (available_at, revision_sequence, statement_point_id)
                ) AS quarterly_net_income_id,
                max(available_at) AS quarterly_net_income_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric = 'net_income'
              AND unit = 'USD'
              AND period_type = 'duration'
              AND period_start IS NOT NULL
              AND period_end IS NOT NULL
              AND date_diff('day', period_start, period_end) + 1 BETWEEN 70 AND 115
              AND value IS NOT NULL
              AND isfinite(value)
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-Q', '10-Q/A', '10-QT',
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A',
                  '6-K', '6-K/A'
              )
            GROUP BY security_id, accession_number, period_start, period_end
        ),
        quarterly_equity AS (
            SELECT
                security_id,
                accession_number,
                period_end,
                arg_max(
                    value,
                    (available_at, revision_sequence, statement_point_id)
                ) AS stockholders_equity,
                arg_max(
                    statement_point_id,
                    (available_at, revision_sequence, statement_point_id)
                ) AS stockholders_equity_id,
                max(available_at) AS stockholders_equity_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric = 'stockholders_equity'
              AND unit = 'USD'
              AND period_type = 'instant'
              AND period_end IS NOT NULL
              AND value > 0
              AND isfinite(value)
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-Q', '10-Q/A', '10-QT',
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A',
                  '6-K', '6-K/A'
              )
            GROUP BY security_id, accession_number, period_end
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (
                SELECT DISTINCT security_id FROM quarterly_earnings
            )
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
                u.rules_json AS universe_rules_json,
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
        earnings_candidates AS (
            SELECT
                d.*,
                q.fundamental_symbol,
                q.accession_number AS earnings_accession_number,
                q.period_start AS earnings_period_start,
                q.period_end AS earnings_period_end,
                q.quarterly_net_income,
                q.quarterly_net_income_id,
                q.quarterly_net_income_available_at,
                row_number() OVER (
                    PARTITION BY d.security_id, d.trade_date
                    ORDER BY q.period_end DESC,
                             q.quarterly_net_income_available_at DESC,
                             q.accession_number DESC
                ) AS earnings_rank
            FROM governed_rebalances d
            JOIN quarterly_earnings q
              ON q.security_id = d.security_id
             AND q.period_end <= d.trade_date
             AND q.quarterly_net_income_available_at <= d.price_available_at
             AND d.trade_date - q.period_end <= ?
            WHERE d.universe_rank = 1
        ),
        current_earnings AS (
            SELECT * EXCLUDE (earnings_rank)
            FROM earnings_candidates
            WHERE earnings_rank = 1
        ),
        lagged_equity_candidates AS (
            SELECT
                e.*,
                b.accession_number AS equity_accession_number,
                b.period_end AS equity_period_end,
                b.stockholders_equity,
                b.stockholders_equity_id,
                b.stockholders_equity_available_at,
                row_number() OVER (
                    PARTITION BY e.security_id, e.trade_date
                    ORDER BY abs((e.earnings_period_end - b.period_end) - 91),
                             b.period_end DESC,
                             b.stockholders_equity_available_at DESC,
                             b.accession_number DESC
                ) AS equity_rank
            FROM current_earnings e
            JOIN quarterly_equity b
              ON b.security_id = e.security_id
             AND b.period_end < e.earnings_period_end
             AND e.earnings_period_end - b.period_end BETWEEN 60 AND 130
             AND b.stockholders_equity_available_at <= e.price_available_at
        )
        SELECT
            * EXCLUDE (month_rank, universe_rank, equity_rank),
            greatest(price_available_at, universe_available_at)
                AS decision_available_at
        FROM lagged_equity_candidates
        WHERE equity_rank = 1
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [*date_params, options.universe_id, options.maximum_earnings_age_days],
    ).df()


def _lineage(row: pd.Series, options: QuarterlyRoeOptions) -> str:
    return json_dumps(
        {
            "method": "hou_xue_zhang_quarterly_roe_pit",
            "formula": "quarterly_net_income / one_quarter_lagged_stockholders_equity",
            "orientation": "higher_is_more_profitable",
            "research_contract": {
                "quarter_duration_days": [70, 115],
                "lagged_equity_gap_days": [60, 130],
                "maximum_earnings_age_days": options.maximum_earnings_age_days,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
            "published_method_adaptations": {
                "numerator": (
                    "Reported net income replaces Compustat income before extraordinary items."
                ),
                "denominator": (
                    "Reported stockholders' equity replaces the full Compustat quarterly "
                    "book-equity reconstruction."
                ),
                "financial_firm_exclusion": (
                    "Not applied because historical point-in-time industry coverage is absent."
                ),
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
            "earnings": {
                "statement_point_id": row["quarterly_net_income_id"],
                "accession_number": row["earnings_accession_number"],
                "period_start": row["earnings_period_start"],
                "period_end": row["earnings_period_end"],
                "value": row["quarterly_net_income"],
                "available_at": row["quarterly_net_income_available_at"],
            },
            "lagged_book_equity": {
                "statement_point_id": row["stockholders_equity_id"],
                "accession_number": row["equity_accession_number"],
                "period_end": row["equity_period_end"],
                "value": row["stockholders_equity"],
                "available_at": row["stockholders_equity_available_at"],
            },
            "quarterly_roe": row["raw_value"],
        }
    )


def compute_quarterly_roe_rows(
    inputs: pd.DataFrame,
    options: QuarterlyRoeOptions | None = None,
) -> pd.DataFrame:
    """Winsorize and standardize q-factor-style quarterly ROE."""

    options = options or QuarterlyRoeOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "decision_available_at",
        "quarterly_net_income",
        "quarterly_net_income_id",
        "quarterly_net_income_available_at",
        "earnings_accession_number",
        "earnings_period_start",
        "earnings_period_end",
        "stockholders_equity",
        "stockholders_equity_id",
        "stockholders_equity_available_at",
        "equity_accession_number",
        "equity_period_end",
        "universe_id",
        "universe_valid_from",
        "universe_valid_to",
        "universe_available_at",
        "universe_source",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Quarterly ROE inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    rows["quarterly_net_income"] = pd.to_numeric(
        rows["quarterly_net_income"], errors="coerce"
    )
    rows["stockholders_equity"] = pd.to_numeric(
        rows["stockholders_equity"], errors="coerce"
    )
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "quarterly_net_income",
            "stockholders_equity",
        ]
    )
    rows = rows[
        (rows["stockholders_equity"] > 0)
        & rows["quarterly_net_income"].map(math.isfinite)
        & rows["stockholders_equity"].map(math.isfinite)
    ].copy()
    rows["quarterly_roe"] = (
        rows["quarterly_net_income"] / rows["stockholders_equity"]
    )
    rows = rows[rows["quarterly_roe"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["quarterly_roe"]
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
        [
            "metric:net_income",
            "metric:stockholders_equity",
            f"universe:{options.universe_id}",
        ]
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


def refresh_quarterly_roe_values(
    store: DuckDBStore,
    options: QuarterlyRoeOptions | None = None,
) -> int:
    """Materialize point-in-time q-factor-style quarterly ROE."""

    options = options or QuarterlyRoeOptions()
    store.initialize()
    rows = compute_quarterly_roe_rows(load_quarterly_roe_inputs(store, options), options)
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
            insert_frame(store, rows, "fundamental_factor_values", "quarterly_roe_insert")
    return len(rows)
