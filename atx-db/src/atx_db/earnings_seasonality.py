"""Point-in-time five-year earnings seasonality for predicted announcement months."""

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
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT five-year earnings seasonality v1"
FACTOR_ID = "earnings_seasonality_predicted_announcement"
FACTOR_NAME = "PIT five-year earnings seasonality"
FACTOR_FAMILY = "fundamental_earnings"

_OUTPUT_COLUMNS = [
    "factor_value_id", "factor_id", "factor_name", "family", "security_id",
    "symbol", "as_of_date", "raw_value", "value", "available_at",
    "input_ids_json", "input_lineage_json", "is_latest_revision", "run_id", "source",
]


@dataclass(frozen=True)
class EarningsSeasonalityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_market_cap_usd: float = 100_000_000.0
    minimum_adv21_usd: float = 1_000_000.0
    minimum_price_usd: float = 5.0
    history_quarters: int = 20
    same_quarter_observations: int = 5
    minimum_history_span_days: int = 1_550
    maximum_history_span_days: int = 2_100
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None

    def __post_init__(self) -> None:
        if self.history_quarters != 20 or self.same_quarter_observations != 5:
            raise ValueError("earnings seasonality requires the published 20-quarter/5-year history")
        if self.minimum_market_cap_usd <= 0 or self.minimum_adv21_usd <= 0:
            raise ValueError("liquidity thresholds must be positive")
        if self.minimum_price_usd <= 0:
            raise ValueError("minimum_price_usd must be positive")
        if self.minimum_history_span_days <= 0:
            raise ValueError("minimum_history_span_days must be positive")
        if self.maximum_history_span_days < self.minimum_history_span_days:
            raise ValueError("maximum_history_span_days must not precede the minimum")
        if self.minimum_names_per_date < 3:
            raise ValueError("minimum_names_per_date must be at least 3")
        if not 0 <= self.winsor_limit < 0.5:
            raise ValueError("winsor_limit must be in [0, 0.5)")


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_earnings_seasonality_inputs(
    store: DuckDBStore,
    options: EarningsSeasonalityOptions | None = None,
) -> pd.DataFrame:
    """Build published EarnRank using only information at least eleven months old."""

    options = options or EarningsSeasonalityOptions()
    date_predicates: list[str] = []
    date_params: list[object] = []
    if options.start_date is not None:
        date_predicates.append("formation_trade_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("formation_trade_date <= ?")
        date_params.append(options.end_date)
    date_sql = " AND " + " AND ".join(date_predicates) if date_predicates else ""
    return store.con.execute(
        f"""
        WITH quarter_candidates AS (
            SELECT
                statement_point_id,security_id,symbol,period_start,period_end,
                as_of_date AS filed_date,available_at,accession_number,
                fiscal_year,fiscal_period,value AS eps_diluted,
                row_number() OVER (
                    PARTITION BY security_id,period_end
                    ORDER BY available_at,revision_sequence,statement_point_id
                ) AS first_period_rank
            FROM fundamental_statement_points
            WHERE canonical_metric='eps_diluted'
              AND unit_type='per_share'
              AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 70 AND 115
              AND value IS NOT NULL AND isfinite(value)
              AND available_at IS NOT NULL
              AND revision_sequence=1
              AND form IN ('10-Q','10-Q/A','10-K','10-K/A')
        ),
        quarters AS (
            SELECT
                * EXCLUDE (first_period_rank),
                row_number() OVER (
                    PARTITION BY security_id ORDER BY period_end,available_at,statement_point_id
                ) AS quarter_sequence
            FROM quarter_candidates WHERE first_period_rank=1
        ),
        relevant_securities AS (
            SELECT DISTINCT security_id FROM quarters
        ),
        canonical_bars AS (
            SELECT
                b.security_id,b.symbol,b.trade_date,b.close,b.volume,b.market_cap_usd,
                b.available_at,
                CASE
                    WHEN b.split_factor IS NOT NULL AND isfinite(b.split_factor)
                     AND b.split_factor>0 THEN ln(b.split_factor)
                    ELSE 0.0
                END AS log_adjustment,
                row_number() OVER (
                    PARTITION BY b.security_id,b.trade_date
                    ORDER BY b.source_loaded_at DESC,b.source DESC
                ) AS revision_rank
            FROM equity_daily_bars b
            JOIN relevant_securities r USING (security_id)
            WHERE b.close IS NOT NULL AND isfinite(b.close) AND b.close>0
              AND b.volume IS NOT NULL AND b.volume>=0
              AND b.available_at IS NOT NULL
        ),
        price_features AS (
            SELECT
                * EXCLUDE (revision_rank,log_adjustment),
                exp(sum(log_adjustment) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                )) AS split_index,
                avg(close*volume) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                ) AS adv21_usd,
                row_number() OVER (
                    PARTITION BY security_id,year(trade_date),month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM canonical_bars WHERE revision_rank=1
        ),
        month_ends AS (
            SELECT * FROM price_features WHERE month_rank=1
        ),
        formations AS (
            SELECT
                q.*,
                p.trade_date AS formation_trade_date,
                p.available_at AS formation_price_available_at,
                p.close AS formation_close,
                p.market_cap_usd,
                p.adv21_usd,
                p.split_index AS formation_split_index,
                CAST(date_trunc('month',q.available_at+INTERVAL 12 MONTH) AS DATE)
                    AS predicted_announcement_month
            FROM quarters q
            JOIN month_ends p
              ON p.security_id=q.security_id
             AND date_trunc('month',p.trade_date)
                 =date_trunc('month',q.available_at+INTERVAL 11 MONTH)
            WHERE q.available_at<=p.available_at
              AND p.close>=?
              AND p.market_cap_usd>=?
              AND p.adv21_usd>=?
        ),
        history AS (
            SELECT
                f.statement_point_id AS anchor_statement_point_id,
                f.security_id,f.symbol,f.formation_trade_date,
                f.formation_price_available_at,f.formation_close,f.market_cap_usd,
                f.adv21_usd,f.formation_split_index,f.predicted_announcement_month,
                f.period_end AS anchor_period_end,f.available_at AS anchor_available_at,
                f.accession_number AS anchor_accession_number,
                f.fiscal_period AS anchor_fiscal_period,
                h.statement_point_id AS history_statement_point_id,
                h.period_end AS history_period_end,h.available_at AS history_available_at,
                h.accession_number AS history_accession_number,
                h.fiscal_period AS history_fiscal_period,h.eps_diluted,
                f.quarter_sequence-h.quarter_sequence AS quarters_back
            FROM formations f
            JOIN quarters h
              ON h.security_id=f.security_id
             AND h.quarter_sequence BETWEEN f.quarter_sequence-19 AND f.quarter_sequence
             AND h.available_at<=f.anchor_available_at
        ),
        history_with_split AS (
            SELECT h.*,p.split_index AS history_split_index
            FROM history h
            ASOF LEFT JOIN price_features p
              ON h.security_id=p.security_id AND h.history_period_end>=p.trade_date
        ),
        adjusted AS (
            SELECT
                *,
                eps_diluted*formation_split_index/nullif(history_split_index,0)
                    AS split_adjusted_eps,
                coalesce(history_fiscal_period,CAST(quarter(history_period_end) AS VARCHAR))
                    =coalesce(anchor_fiscal_period,CAST(quarter(anchor_period_end) AS VARCHAR))
                    AS is_same_fiscal_quarter
            FROM history_with_split
            WHERE history_split_index IS NOT NULL AND history_split_index>0
        ),
        ranked AS (
            SELECT
                *,
                rank() OVER (
                    PARTITION BY anchor_statement_point_id ORDER BY split_adjusted_eps
                ) AS earnings_rank
            FROM adjusted
        ),
        estimates AS (
            SELECT
                anchor_statement_point_id,
                any_value(security_id) AS security_id,
                any_value(symbol) AS symbol,
                any_value(formation_trade_date) AS formation_trade_date,
                any_value(formation_price_available_at) AS formation_price_available_at,
                any_value(formation_close) AS formation_close,
                any_value(market_cap_usd) AS market_cap_usd,
                any_value(adv21_usd) AS adv21_usd,
                any_value(predicted_announcement_month) AS predicted_announcement_month,
                any_value(anchor_period_end) AS anchor_period_end,
                any_value(anchor_available_at) AS anchor_available_at,
                any_value(anchor_accession_number) AS anchor_accession_number,
                min(history_period_end) AS oldest_period_end,
                max(history_period_end) AS latest_period_end,
                max(history_available_at) AS latest_history_available_at,
                date_diff('day',min(history_period_end),max(history_period_end)) AS history_span_days,
                count(*) AS history_quarters,
                count(*) FILTER (WHERE is_same_fiscal_quarter) AS same_quarter_observations,
                avg(earnings_rank) FILTER (WHERE is_same_fiscal_quarter) AS earn_rank,
                to_json(list(struct_pack(
                    statement_point_id:=history_statement_point_id,
                    period_end:=history_period_end,
                    available_at:=history_available_at,
                    accession_number:=history_accession_number,
                    fiscal_period:=history_fiscal_period,
                    quarters_back:=quarters_back,
                    reported_eps:=eps_diluted,
                    split_index:=history_split_index,
                    split_adjusted_eps:=split_adjusted_eps,
                    earnings_rank:=earnings_rank,
                    is_same_fiscal_quarter:=is_same_fiscal_quarter
                ) ORDER BY quarters_back DESC)) AS history_json
            FROM ranked
            GROUP BY anchor_statement_point_id
            HAVING count(*)=?
               AND count(DISTINCT history_period_end)=?
               AND count(*) FILTER (WHERE is_same_fiscal_quarter)=?
               AND date_diff('day',min(history_period_end),max(history_period_end)) BETWEEN ? AND ?
        ),
        deduped AS (
            SELECT *
            FROM estimates
            QUALIFY row_number() OVER (
                PARTITION BY security_id,formation_trade_date
                ORDER BY anchor_available_at DESC,anchor_period_end DESC,anchor_statement_point_id DESC
            )=1
        )
        SELECT
            *,
            greatest(anchor_available_at,latest_history_available_at,formation_price_available_at)
                AS decision_available_at
        FROM deduped
        WHERE earn_rank IS NOT NULL AND isfinite(earn_rank) {date_sql}
        ORDER BY formation_trade_date,security_id
        """,
        [
            options.minimum_price_usd,
            options.minimum_market_cap_usd,
            options.minimum_adv21_usd,
            options.history_quarters,
            options.history_quarters,
            options.same_quarter_observations,
            options.minimum_history_span_days,
            options.maximum_history_span_days,
            *date_params,
        ],
    ).df()


def _lineage(row: pd.Series, options: EarningsSeasonalityOptions) -> str:
    history = row.get("history_json")
    if isinstance(history, str):
        history = json.loads(history)
    return json_dumps(
        {
            "method": "chang_hartzmark_solomon_soltes_five_year_earnrank_pit",
            "formula": "mean rank of five same-fiscal-quarter EPS observations among 20 quarters",
            "rank_orientation": "1=lowest_split_adjusted_eps,20=highest_split_adjusted_eps",
            "prediction_timing": "month preceding the month twelve months after the anchor filing",
            "anchor": {
                "statement_point_id": row["anchor_statement_point_id"],
                "period_end": row["anchor_period_end"],
                "available_at": row["anchor_available_at"],
                "accession_number": row["anchor_accession_number"],
            },
            "formation": {
                "trade_date": row["as_of_date"],
                "price_available_at": row["formation_price_available_at"],
                "close": row["formation_close"],
                "market_cap_usd": row["market_cap_usd"],
                "adv21_usd": row["adv21_usd"],
                "predicted_announcement_month": row["predicted_announcement_month"],
            },
            "history": history,
            "oldest_period_end": row["oldest_period_end"],
            "latest_period_end": row["latest_period_end"],
            "latest_history_available_at": row["latest_history_available_at"],
            "history_span_days": row["history_span_days"],
            "history_quarters": row["history_quarters"],
            "same_quarter_observations": row["same_quarter_observations"],
            "earn_rank": row["raw_value"],
            "research_contract": {
                "history_quarters": options.history_quarters,
                "same_quarter_observations": options.same_quarter_observations,
                "history_span_days": [
                    options.minimum_history_span_days,
                    options.maximum_history_span_days,
                ],
                "minimum_price_usd": options.minimum_price_usd,
                "minimum_market_cap_usd": options.minimum_market_cap_usd,
                "minimum_adv21_usd": options.minimum_adv21_usd,
                "first_filed_values_only": True,
                "return_fitted_parameters": False,
            },
        }
    )


def compute_earnings_seasonality_rows(
    inputs: pd.DataFrame,
    options: EarningsSeasonalityOptions | None = None,
) -> pd.DataFrame:
    """Validate, standardize, and serialize PIT earnings seasonality rows."""

    options = options or EarningsSeasonalityOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "anchor_statement_point_id", "security_id", "symbol", "formation_trade_date",
        "decision_available_at", "earn_rank", "history_json", "history_quarters",
        "same_quarter_observations",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"earnings seasonality inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["formation_trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    rows["earn_rank"] = pd.to_numeric(rows["earn_rank"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "earn_rank"])
    rows = rows[
        rows["earn_rank"].map(math.isfinite)
        & rows["earn_rank"].between(1.0, float(options.history_quarters))
        & (rows["history_quarters"] == options.history_quarters)
        & (rows["same_quarter_observations"] == options.same_quarter_observations)
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["earn_rank"].astype(float)
    rows = winsorize(
        rows, value_column="raw_value", output_column="raw_value",
        partition_columns=("factor_id", "as_of_date"), limits=options.winsor_limit,
    )
    rows = zscore(
        rows, value_column="raw_value", output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        ["metric:eps_diluted_quarterly", "market:equity_daily_bars"]
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


def refresh_earnings_seasonality_values(
    store: DuckDBStore,
    options: EarningsSeasonalityOptions | None = None,
) -> int:
    """Materialize PIT five-year earnings seasonality."""

    options = options or EarningsSeasonalityOptions()
    store.initialize()
    rows = compute_earnings_seasonality_rows(load_earnings_seasonality_inputs(store, options), options)
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
            insert_frame(store, rows, "fundamental_factor_values", "earnings_seasonality_insert")
    return len(rows)
