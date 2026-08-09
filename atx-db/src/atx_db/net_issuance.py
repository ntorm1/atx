"""Point-in-time net share issuance factor.

Pontiff and Woodgate define annual issuance as the log change in split-adjusted
shares outstanding.  The investable orientation is the negative of that change:
repurchases score high and net issuance scores low.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT net share issuance v1"
FACTOR_ID = "financing_low_net_share_issuance"
FACTOR_NAME = "PIT low net share issuance"
FACTOR_FAMILY = "fundamental_financing"

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
class NetIssuanceOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_market_cap_usd: float = 100_000_000.0
    minimum_adv21_usd: float = 1_000_000.0
    maximum_observation_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    maximum_absolute_log_change: float = 3.0
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: NetIssuanceOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_net_issuance_inputs(
    store: DuckDBStore,
    options: NetIssuanceOptions | None = None,
) -> pd.DataFrame:
    """Assemble monthly split-adjusted one-year share changes without look-ahead.

    Share observations are paired only within the same taxonomy/concept. The prior
    observation must already have been available when the current observation became
    public. Price-feed adjustment factors at or below 0.8 or at or above 1.25 are treated
    as stock splits; small dividend adjustment factors are deliberately excluded.
    """

    options = options or NetIssuanceOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                arg_max("close", available_at) AS "close",
                arg_max(volume, available_at) AS volume,
                arg_max(split_factor, available_at) AS split_factor,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE "close" > 0
              AND trade_date IS NOT NULL
              AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
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
                    PARTITION BY security_id
                    ORDER BY trade_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                ) AS split_index,
                avg("close" * volume) OVER (
                    PARTITION BY security_id
                    ORDER BY trade_date
                    ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                ) AS adv21_usd,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        share_observations AS (
            SELECT
                share_history_id,
                security_id,
                symbol,
                taxonomy,
                concept,
                effective_date,
                available_at,
                share_count,
                accession_number,
                form,
                revision_sequence
            FROM shares_outstanding_history
            WHERE share_count_type = 'shares_outstanding'
              AND share_count > 0
              AND concept IN (
                  'EntityCommonStockSharesOutstanding',
                  'CommonStockSharesOutstanding'
              )
              AND form IN (
                  '10-Q', '10-Q/A', '10-QT',
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A'
              )
              AND effective_date IS NOT NULL
              AND available_at IS NOT NULL
        ),
        share_with_split_index AS (
            SELECT
                s.*,
                coalesce(p.split_index, 1.0) AS split_index
            FROM share_observations s
            ASOF LEFT JOIN price_features p
              ON s.security_id = p.security_id
             AND s.effective_date >= p.trade_date
        ),
        pair_candidates AS (
            SELECT
                current.share_history_id AS current_share_history_id,
                prior.share_history_id AS prior_share_history_id,
                current.security_id,
                current.symbol,
                current.taxonomy,
                current.concept,
                current.effective_date AS current_effective_date,
                prior.effective_date AS prior_effective_date,
                current.available_at AS signal_available_at,
                prior.available_at AS prior_available_at,
                current.share_count AS current_share_count,
                prior.share_count AS prior_share_count,
                current.split_index AS current_split_index,
                prior.split_index AS prior_split_index,
                current.accession_number AS current_accession_number,
                prior.accession_number AS prior_accession_number,
                row_number() OVER (
                    PARTITION BY current.share_history_id
                    ORDER BY
                        abs((current.effective_date - prior.effective_date) - 365),
                        prior.effective_date DESC,
                        prior.available_at DESC,
                        prior.revision_sequence DESC,
                        prior.share_history_id DESC
                ) AS prior_rank
            FROM share_with_split_index current
            JOIN share_with_split_index prior
              ON prior.security_id = current.security_id
             AND prior.taxonomy = current.taxonomy
             AND prior.concept = current.concept
             AND prior.effective_date < current.effective_date
             AND current.effective_date - prior.effective_date BETWEEN 300 AND 430
             AND prior.available_at <= current.available_at
        ),
        signals AS (
            SELECT
                *,
                ln(
                    (current_share_count * current_split_index)
                    / nullif(prior_share_count * prior_split_index, 0)
                ) AS net_share_issuance
            FROM pair_candidates
            WHERE prior_rank = 1
        ),
        rebalances AS (
            SELECT *
            FROM price_features
            WHERE month_rank = 1
              {date_sql}
        ),
        matched AS (
            SELECT
                p.*,
                s.* EXCLUDE (security_id, symbol),
                s.current_share_count * s.current_split_index
                    / nullif(p.split_index, 0) AS decision_share_count,
                "close" * s.current_share_count * s.current_split_index
                    / nullif(p.split_index, 0) AS market_cap_usd,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY
                        s.current_effective_date DESC,
                        s.signal_available_at DESC,
                        CASE WHEN s.taxonomy = 'dei' THEN 0 ELSE 1 END,
                        s.current_share_history_id DESC
                ) AS signal_rank
            FROM rebalances p
            JOIN signals s
              ON s.security_id = p.security_id
             AND s.current_effective_date <= p.trade_date
             AND s.signal_available_at <= p.price_available_at
        )
        SELECT *
        FROM matched
        WHERE signal_rank = 1
          AND market_cap_usd >= ?
          AND adv21_usd >= ?
          AND trade_date - current_effective_date <= ?
          AND abs(net_share_issuance) <= ?
        ORDER BY trade_date, security_id
    """
    params = [
        *date_params,
        options.minimum_market_cap_usd,
        options.minimum_adv21_usd,
        options.maximum_observation_age_days,
        options.maximum_absolute_log_change,
    ]
    return store.con.execute(sql, params).df()


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "formula": (
                "-ln((current_shares * current_split_index) / "
                "(prior_shares * prior_split_index))"
            ),
            "split_policy": {
                "source": "equity_daily_bars.split_factor",
                "included": "factor <= 0.8 or factor >= 1.25",
                "excluded": "small dividend adjustment factors",
            },
            "current": {
                "share_history_id": row["current_share_history_id"],
                "effective_date": row["current_effective_date"],
                "available_at": row["signal_available_at"],
                "share_count": row["current_share_count"],
                "split_index": row["current_split_index"],
                "accession_number": row["current_accession_number"],
            },
            "prior": {
                "share_history_id": row["prior_share_history_id"],
                "effective_date": row["prior_effective_date"],
                "available_at": row["prior_available_at"],
                "share_count": row["prior_share_count"],
                "split_index": row["prior_split_index"],
                "accession_number": row["prior_accession_number"],
            },
            "decision": {
                "trade_date": row["trade_date"],
                "price_available_at": row["price_available_at"],
                "market_cap_usd": row["market_cap_usd"],
                "adv21_usd": row["adv21_usd"],
            },
        }
    )


def compute_net_issuance_rows(
    inputs: pd.DataFrame,
    options: NetIssuanceOptions | None = None,
) -> pd.DataFrame:
    """Convert monthly issuance inputs to an investable, standardized factor."""

    options = options or NetIssuanceOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "price_available_at",
        "net_share_issuance",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"net issuance inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["trade_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["price_available_at"] = pd.to_datetime(
        rows["price_available_at"], errors="coerce"
    )
    rows["net_share_issuance"] = pd.to_numeric(
        rows["net_share_issuance"], errors="coerce"
    )
    rows = rows.dropna(
        subset=["security_id", "trade_date", "price_available_at", "net_share_issuance"]
    )
    rows = rows[rows["net_share_issuance"].map(math.isfinite)].copy()
    counts = rows.groupby("trade_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["as_of_date"] = rows["trade_date"]
    rows["raw_value"] = -rows["net_share_issuance"].astype(float)
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
    rows["available_at"] = rows["price_available_at"]
    rows["input_ids_json"] = json_dumps(
        ["metric:shares_outstanding", "market:split_factor"]
    )
    rows["input_lineage_json"] = rows.apply(_lineage, axis=1)
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


def _delete_scope(store: DuckDBStore, options: NetIssuanceOptions) -> None:
    predicates = ["source = ?", "factor_id = ?"]
    params: list[object] = [options.source, FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    store.con.execute(
        f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
        params,
    )


def refresh_net_issuance_values(
    store: DuckDBStore,
    options: NetIssuanceOptions | None = None,
) -> int:
    """Materialize the monthly PIT net share issuance factor."""

    options = options or NetIssuanceOptions()
    store.initialize()
    inputs = load_net_issuance_inputs(store, options)
    rows = compute_net_issuance_rows(inputs, options)
    with store.transaction():
        _delete_scope(store, options)
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "net_issuance_values_insert")
    return len(rows)
