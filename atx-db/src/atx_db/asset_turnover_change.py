"""Point-in-time annual change in total-asset turnover."""

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

SOURCE_NAME = "atx-db PIT annual asset turnover change v1"
FACTOR_ID = "efficiency_annual_asset_turnover_change"
FACTOR_NAME = "PIT annual change in total-asset turnover"
FACTOR_FAMILY = "fundamental_efficiency"
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
class AssetTurnoverChangeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: AssetTurnoverChangeOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_asset_turnover_change_inputs(
    store: DuckDBStore,
    options: AssetTurnoverChangeOptions | None = None,
) -> pd.DataFrame:
    """Resolve three consecutive annual observations at each governed month end."""

    options = options or AssetTurnoverChangeOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH annual_filings AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_end,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'revenue'
                          AND period_type = 'duration'
                          AND period_start IS NOT NULL
                          AND period_end - period_start BETWEEN 329 AND 399
                    ) AS revenue,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'revenue'
                          AND period_type = 'duration'
                          AND period_start IS NOT NULL
                          AND period_end - period_start BETWEEN 329 AND 399
                    ) AS revenue_id,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'revenue'
                      AND period_type = 'duration'
                      AND period_start IS NOT NULL
                      AND period_end - period_start BETWEEN 329 AND 399
                ) AS revenue_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'total_assets'
                          AND period_type = 'instant'
                    ) AS total_assets,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (
                        WHERE canonical_metric = 'total_assets'
                          AND period_type = 'instant'
                    ) AS total_assets_id,
                max(available_at) FILTER (
                    WHERE canonical_metric = 'total_assets'
                      AND period_type = 'instant'
                ) AS total_assets_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric IN ('revenue', 'total_assets')
              AND unit = 'USD'
              AND period_end IS NOT NULL
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A'
              )
            GROUP BY security_id, accession_number, period_end
        ),
        complete_annual AS (
            SELECT
                *,
                greatest(revenue_available_at, total_assets_available_at)
                    AS annual_available_at
            FROM annual_filings
            WHERE revenue > 0
              AND total_assets > 0
              AND isfinite(revenue)
              AND isfinite(total_assets)
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT DISTINCT security_id FROM complete_annual)
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
                a.fundamental_symbol,
                a.accession_number AS current_accession_number,
                a.period_end AS current_period_end,
                a.revenue AS current_revenue,
                a.revenue_id AS current_revenue_id,
                a.revenue_available_at AS current_revenue_available_at,
                a.total_assets AS current_assets,
                a.total_assets_id AS current_assets_id,
                a.total_assets_available_at AS current_assets_available_at,
                a.annual_available_at AS current_annual_available_at,
                row_number() OVER (
                    PARTITION BY d.security_id, d.trade_date
                    ORDER BY a.period_end DESC,
                             a.annual_available_at DESC,
                             a.accession_number DESC
                ) AS current_rank
            FROM governed_rebalances d
            JOIN complete_annual a
              ON a.security_id = d.security_id
             AND a.period_end <= d.trade_date
             AND a.annual_available_at <= d.price_available_at
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
                p.total_assets AS prior_assets,
                p.total_assets_id AS prior_assets_id,
                p.total_assets_available_at AS prior_assets_available_at,
                p.annual_available_at AS prior_annual_available_at,
                row_number() OVER (
                    PARTITION BY c.security_id, c.trade_date
                    ORDER BY abs((c.current_period_end - p.period_end) - 365),
                             p.period_end DESC,
                             p.annual_available_at DESC,
                             p.accession_number DESC
                ) AS prior_rank
            FROM current_annual c
            JOIN complete_annual p
              ON p.security_id = c.security_id
             AND p.period_end < c.current_period_end
             AND c.current_period_end - p.period_end BETWEEN 300 AND 430
             AND p.annual_available_at <= c.price_available_at
        ),
        current_prior AS (
            SELECT * EXCLUDE (prior_rank)
            FROM prior_candidates
            WHERE prior_rank = 1
        ),
        prior2_candidates AS (
            SELECT
                cp.*,
                a.accession_number AS prior2_accession_number,
                a.period_end AS prior2_period_end,
                a.total_assets AS prior2_assets,
                a.total_assets_id AS prior2_assets_id,
                a.total_assets_available_at AS prior2_assets_available_at,
                row_number() OVER (
                    PARTITION BY cp.security_id, cp.trade_date
                    ORDER BY abs((cp.prior_period_end - a.period_end) - 365),
                             a.period_end DESC,
                             a.total_assets_available_at DESC,
                             a.accession_number DESC
                ) AS prior2_rank
            FROM current_prior cp
            JOIN annual_filings a
              ON a.security_id = cp.security_id
             AND a.period_end < cp.prior_period_end
             AND cp.prior_period_end - a.period_end BETWEEN 300 AND 430
             AND a.total_assets > 0
             AND isfinite(a.total_assets)
             AND a.total_assets_available_at <= cp.price_available_at
        )
        SELECT
            * EXCLUDE (month_rank, universe_rank, prior2_rank),
            greatest(
                price_available_at,
                universe_available_at,
                current_annual_available_at,
                prior_annual_available_at,
                prior2_assets_available_at
            ) AS decision_available_at
        FROM prior2_candidates
        WHERE prior2_rank = 1
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [*date_params, options.universe_id, options.maximum_fundamental_age_days],
    ).df()


def _annual_leg(row: pd.Series, prefix: str) -> dict[str, object]:
    return {
        "accession_number": row[f"{prefix}_accession_number"],
        "period_end": row[f"{prefix}_period_end"],
        "revenue": {
            "statement_point_id": row[f"{prefix}_revenue_id"],
            "value": row[f"{prefix}_revenue"],
            "available_at": row[f"{prefix}_revenue_available_at"],
        },
        "total_assets": {
            "statement_point_id": row[f"{prefix}_assets_id"],
            "value": row[f"{prefix}_assets"],
            "available_at": row[f"{prefix}_assets_available_at"],
        },
    }


def _lineage(row: pd.Series, options: AssetTurnoverChangeOptions) -> str:
    return json_dumps(
        {
            "method": "annual_change_in_total_asset_turnover_pit",
            "formula": "revenue_t/assets_t_1-revenue_t_1/assets_t_2",
            "orientation": "higher_efficiency_change_is_preferred",
            "research_contract": {
                "annual_duration_days_inclusive": [330, 400],
                "annual_period_gap_days": [300, 430],
                "maximum_fundamental_age_days": options.maximum_fundamental_age_days,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
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
            "annual": {
                "current": _annual_leg(row, "current"),
                "prior": _annual_leg(row, "prior"),
                "prior2": {
                    "accession_number": row["prior2_accession_number"],
                    "period_end": row["prior2_period_end"],
                    "total_assets": {
                        "statement_point_id": row["prior2_assets_id"],
                        "value": row["prior2_assets"],
                        "available_at": row["prior2_assets_available_at"],
                    },
                },
                "asset_turnover": row["asset_turnover"],
                "prior_asset_turnover": row["prior_asset_turnover"],
                "change": row["raw_value"],
            },
        }
    )


def compute_asset_turnover_change_rows(
    inputs: pd.DataFrame,
    options: AssetTurnoverChangeOptions | None = None,
) -> pd.DataFrame:
    """Compute, winsorize, and standardize annual asset-turnover changes."""

    options = options or AssetTurnoverChangeOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "decision_available_at",
        "current_revenue",
        "prior_revenue",
        "prior_assets",
        "prior2_assets",
        "current_revenue_id",
        "prior_revenue_id",
        "current_assets_id",
        "prior_assets_id",
        "prior2_assets_id",
        "current_accession_number",
        "prior_accession_number",
        "prior2_accession_number",
        "current_period_end",
        "prior_period_end",
        "prior2_period_end",
        "current_revenue_available_at",
        "prior_revenue_available_at",
        "current_assets_available_at",
        "prior_assets_available_at",
        "prior2_assets_available_at",
        "universe_id",
        "universe_valid_from",
        "universe_valid_to",
        "universe_available_at",
        "universe_source",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Asset-turnover-change inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    numeric_columns = (
        "current_revenue",
        "prior_revenue",
        "prior_assets",
        "prior2_assets",
    )
    for column in numeric_columns:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", *numeric_columns]
    )
    valid = pd.Series(True, index=rows.index)
    for column in numeric_columns:
        valid &= (rows[column] > 0) & rows[column].map(math.isfinite)
    rows = rows[valid].copy()
    rows["asset_turnover"] = rows["current_revenue"] / rows["prior_assets"]
    rows["prior_asset_turnover"] = rows["prior_revenue"] / rows["prior2_assets"]
    rows["raw_value"] = rows["asset_turnover"] - rows["prior_asset_turnover"]
    rows = rows[rows["raw_value"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
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
            "metric:revenue",
            "metric:total_assets",
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


def refresh_asset_turnover_change_values(
    store: DuckDBStore,
    options: AssetTurnoverChangeOptions | None = None,
) -> int:
    """Materialize the annual asset-turnover-change feature."""

    options = options or AssetTurnoverChangeOptions()
    store.initialize()
    rows = compute_asset_turnover_change_rows(
        load_asset_turnover_change_inputs(store, options), options
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
                "asset_turnover_change_insert",
            )
    return len(rows)
