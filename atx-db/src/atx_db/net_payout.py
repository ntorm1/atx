"""Point-in-time cash-flow net payout yield.

Net payout is common dividends plus common-share repurchases less common-stock
issuance, divided by decision-date market capitalization. All three TTM components
must be explicitly reported for the same filing and period; missing tags are never
silently converted to zero.
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

SOURCE_NAME = "atx-db PIT cash-flow net payout yield v1"
FACTOR_ID = "financing_net_payout_yield"
FACTOR_NAME = "PIT cash-flow net payout yield"
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
class NetPayoutOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_market_cap_usd: float = 100_000_000.0
    minimum_adv21_usd: float = 1_000_000.0
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.025
    maximum_absolute_raw_yield: float = 5.0
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: NetPayoutOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_net_payout_inputs(
    store: DuckDBStore,
    options: NetPayoutOptions | None = None,
) -> pd.DataFrame:
    """Assemble strict same-filing TTM payout components at monthly closes."""

    options = options or NetPayoutOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH complete_ttm AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                ttm_end_date,
                arg_max(ttm_value, (available_at, revision_sequence, ttm_point_id))
                    FILTER (WHERE canonical_metric = 'common_div_paid') AS common_div_paid,
                arg_max(ttm_point_id, (available_at, revision_sequence, ttm_point_id))
                    FILTER (WHERE canonical_metric = 'common_div_paid') AS common_div_paid_id,
                max(available_at) FILTER (WHERE canonical_metric = 'common_div_paid')
                    AS common_div_paid_available_at,
                arg_max(ttm_value, (available_at, revision_sequence, ttm_point_id))
                    FILTER (WHERE canonical_metric = 'share_repurchases') AS share_repurchases,
                arg_max(ttm_point_id, (available_at, revision_sequence, ttm_point_id))
                    FILTER (WHERE canonical_metric = 'share_repurchases') AS share_repurchases_id,
                max(available_at) FILTER (WHERE canonical_metric = 'share_repurchases')
                    AS share_repurchases_available_at,
                arg_max(ttm_value, (available_at, revision_sequence, ttm_point_id))
                    FILTER (WHERE canonical_metric = 'stock_issuance') AS stock_issuance,
                arg_max(ttm_point_id, (available_at, revision_sequence, ttm_point_id))
                    FILTER (WHERE canonical_metric = 'stock_issuance') AS stock_issuance_id,
                max(available_at) FILTER (WHERE canonical_metric = 'stock_issuance')
                    AS stock_issuance_available_at
            FROM fundamental_ttm_points
            WHERE canonical_metric IN (
                'common_div_paid', 'share_repurchases', 'stock_issuance'
            )
              AND unit_type = 'monetary'
              AND quarter_count = 4
              AND coverage_days BETWEEN 330 AND 400
            GROUP BY security_id, accession_number, ttm_end_date
            HAVING common_div_paid IS NOT NULL
               AND share_repurchases IS NOT NULL
               AND stock_issuance IS NOT NULL
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                arg_max("close", available_at) AS "close",
                arg_max(volume, available_at) AS volume,
                arg_max(split_factor, available_at) AS split_factor,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT DISTINCT security_id FROM complete_ttm)
              AND "close" > 0
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
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                ) AS split_index,
                avg("close" * volume) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                ) AS adv21_usd,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT * FROM price_features
            WHERE month_rank = 1
              {date_sql}
        ),
        ttm_at_rebalance AS (
            SELECT
                p.*,
                t.* EXCLUDE (security_id),
                greatest(
                    t.common_div_paid_available_at,
                    t.share_repurchases_available_at,
                    t.stock_issuance_available_at
                ) AS fundamental_available_at,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY t.ttm_end_date DESC,
                             fundamental_available_at DESC,
                             t.accession_number DESC
                ) AS ttm_rank
            FROM rebalances p
            JOIN complete_ttm t
              ON t.security_id = p.security_id
             AND t.ttm_end_date <= p.trade_date
             AND t.common_div_paid_available_at <= p.price_available_at
             AND t.share_repurchases_available_at <= p.price_available_at
             AND t.stock_issuance_available_at <= p.price_available_at
        ),
        share_observations AS (
            SELECT
                share_history_id,
                security_id,
                taxonomy,
                concept,
                effective_date,
                available_at,
                share_count,
                accession_number AS share_accession_number,
                revision_sequence
            FROM shares_outstanding_history
            WHERE share_count_type = 'shares_outstanding'
              AND share_count > 0
              AND concept IN (
                  'EntityCommonStockSharesOutstanding',
                  'CommonStockSharesOutstanding'
              )
              AND effective_date IS NOT NULL
              AND available_at IS NOT NULL
        ),
        share_with_split_index AS (
            SELECT s.*, coalesce(p.split_index, 1.0) AS share_split_index
            FROM share_observations s
            ASOF LEFT JOIN price_features p
              ON s.security_id = p.security_id
             AND s.effective_date >= p.trade_date
        ),
        shares_at_rebalance AS (
            SELECT
                p.security_id,
                p.trade_date,
                s.* EXCLUDE (security_id),
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY s.effective_date DESC,
                             s.available_at DESC,
                             CASE WHEN s.taxonomy = 'dei' THEN 0 ELSE 1 END,
                             s.revision_sequence DESC,
                             s.share_history_id DESC
                ) AS share_rank
            FROM rebalances p
            JOIN share_with_split_index s
              ON s.security_id = p.security_id
             AND s.effective_date <= p.trade_date
             AND s.available_at <= p.price_available_at
        ),
        combined AS (
            SELECT
                t.* EXCLUDE (ttm_rank),
                s.share_history_id,
                s.taxonomy AS share_taxonomy,
                s.concept AS share_concept,
                s.effective_date AS share_effective_date,
                s.available_at AS share_available_at,
                s.share_count,
                s.share_split_index,
                s.share_accession_number,
                s.share_count * s.share_split_index / nullif(t.split_index, 0)
                    AS decision_share_count
            FROM ttm_at_rebalance t
            JOIN shares_at_rebalance s USING (security_id, trade_date)
            WHERE t.ttm_rank = 1 AND s.share_rank = 1
        )
        SELECT
            *,
            "close" * decision_share_count AS market_cap_usd,
            (-common_div_paid - share_repurchases - stock_issuance)
                / nullif("close" * decision_share_count, 0) AS net_payout_yield
        FROM combined
        WHERE "close" * decision_share_count >= ?
          AND adv21_usd >= ?
          AND trade_date - ttm_end_date <= ?
          AND abs(
              (-common_div_paid - share_repurchases - stock_issuance)
              / nullif("close" * decision_share_count, 0)
          ) <= ?
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [
            *date_params,
            options.minimum_market_cap_usd,
            options.minimum_adv21_usd,
            options.maximum_fundamental_age_days,
            options.maximum_absolute_raw_yield,
        ],
    ).df()


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "formula": (
                "(-common_div_paid - share_repurchases - stock_issuance) / market_cap"
            ),
            "missing_component_policy": "require_same_filing_complete_case",
            "payout_statement": {
                "accession_number": row["accession_number"],
                "ttm_end_date": row["ttm_end_date"],
                "common_div_paid": {
                    "ttm_point_id": row["common_div_paid_id"],
                    "value": row["common_div_paid"],
                    "available_at": row["common_div_paid_available_at"],
                },
                "share_repurchases": {
                    "ttm_point_id": row["share_repurchases_id"],
                    "value": row["share_repurchases"],
                    "available_at": row["share_repurchases_available_at"],
                },
                "stock_issuance": {
                    "ttm_point_id": row["stock_issuance_id"],
                    "value": row["stock_issuance"],
                    "available_at": row["stock_issuance_available_at"],
                },
            },
            "market_cap": {
                "share_history_id": row["share_history_id"],
                "share_effective_date": row["share_effective_date"],
                "share_available_at": row["share_available_at"],
                "share_split_index": row["share_split_index"],
                "decision_split_index": row["split_index"],
                "decision_share_count": row["decision_share_count"],
                "close": row["close"],
                "market_cap_usd": row["market_cap_usd"],
            },
        }
    )


def compute_net_payout_rows(
    inputs: pd.DataFrame,
    options: NetPayoutOptions | None = None,
) -> pd.DataFrame:
    """Convert complete payout inputs to a winsorized, standardized factor."""

    options = options or NetPayoutOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "price_available_at",
        "net_payout_yield",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"net payout inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["price_available_at"], errors="coerce")
    rows["net_payout_yield"] = pd.to_numeric(rows["net_payout_yield"], errors="coerce")
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", "net_payout_yield"]
    )
    rows = rows[rows["net_payout_yield"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["net_payout_yield"].astype(float)
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
            "metric:common_div_paid_ttm",
            "metric:share_repurchases_ttm",
            "metric:stock_issuance_ttm",
            "metric:market_cap",
        ]
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


def _delete_scope(store: DuckDBStore, options: NetPayoutOptions) -> None:
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


def refresh_net_payout_values(
    store: DuckDBStore,
    options: NetPayoutOptions | None = None,
) -> int:
    """Materialize strict cash-flow net payout yield at monthly decision dates."""

    options = options or NetPayoutOptions()
    store.initialize()
    inputs = load_net_payout_inputs(store, options)
    rows = compute_net_payout_rows(inputs, options)
    with store.transaction():
        _delete_scope(store, options)
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "net_payout_values_insert")
    return len(rows)
