"""Point-in-time annual conservative asset-growth investment factor."""

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

SOURCE_NAME = "atx-db PIT conservative asset growth v1"
FACTOR_ID = "investment_conservative_asset_growth"
FACTOR_NAME = "PIT conservative annual asset growth"
FACTOR_FAMILY = "fundamental_investment"
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
class AssetGrowthOptions:
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


def _date_filter(options: AssetGrowthOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_asset_growth_inputs(
    store: DuckDBStore,
    options: AssetGrowthOptions | None = None,
) -> pd.DataFrame:
    """Resolve exact consecutive annual asset observations visible each month."""

    options = options or AssetGrowthOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH annual_assets AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_end,
                arg_max(
                    value,
                    (available_at, revision_sequence, statement_point_id)
                ) AS current_assets,
                arg_max(
                    statement_point_id,
                    (available_at, revision_sequence, statement_point_id)
                ) AS current_asset_id,
                max(available_at) AS current_asset_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric = 'total_assets'
              AND unit = 'USD'
              AND period_type = 'instant'
              AND value > 0
              AND isfinite(value)
              AND period_end IS NOT NULL
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A'
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
            WHERE security_id IN (SELECT DISTINCT security_id FROM annual_assets)
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
        current_candidates AS (
            SELECT
                d.*,
                a.fundamental_symbol,
                a.accession_number AS current_accession_number,
                a.period_end AS current_period_end,
                a.current_assets,
                a.current_asset_id,
                a.current_asset_available_at,
                row_number() OVER (
                    PARTITION BY d.security_id, d.trade_date
                    ORDER BY a.period_end DESC,
                             a.current_asset_available_at DESC,
                             a.accession_number DESC
                ) AS current_rank
            FROM governed_rebalances d
            JOIN annual_assets a
              ON a.security_id = d.security_id
             AND a.period_end <= d.trade_date
             AND a.current_asset_available_at <= d.price_available_at
             AND d.trade_date - a.period_end <= ?
            WHERE d.universe_rank = 1
        ),
        current_assets AS (
            SELECT * EXCLUDE (current_rank)
            FROM current_candidates
            WHERE current_rank = 1
        ),
        prior_candidates AS (
            SELECT
                c.*,
                p.accession_number AS prior_accession_number,
                p.period_end AS prior_period_end,
                p.current_assets AS prior_assets,
                p.current_asset_id AS prior_asset_id,
                p.current_asset_available_at AS prior_asset_available_at,
                row_number() OVER (
                    PARTITION BY c.security_id, c.trade_date
                    ORDER BY abs((c.current_period_end - p.period_end) - 365),
                             p.period_end DESC,
                             p.current_asset_available_at DESC,
                             p.accession_number DESC
                ) AS prior_rank
            FROM current_assets c
            JOIN annual_assets p
              ON p.security_id = c.security_id
             AND p.period_end < c.current_period_end
             AND c.current_period_end - p.period_end BETWEEN 300 AND 430
             AND p.current_asset_available_at <= c.price_available_at
        )
        SELECT
            * EXCLUDE (month_rank, universe_rank, prior_rank),
            greatest(price_available_at, universe_available_at)
                AS decision_available_at
        FROM prior_candidates
        WHERE prior_rank = 1
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [*date_params, options.universe_id, options.maximum_fundamental_age_days],
    ).df()


def _lineage(row: pd.Series, options: AssetGrowthOptions) -> str:
    return json_dumps(
        {
            "method": "cooper_gulen_schill_annual_asset_growth_pit",
            "formula": "-((total_assets_t - total_assets_t_minus_1) / total_assets_t_minus_1)",
            "orientation": "higher_is_more_conservative_investment",
            "research_contract": {
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
            "assets": {
                "current": {
                    "statement_point_id": row["current_asset_id"],
                    "accession_number": row["current_accession_number"],
                    "period_end": row["current_period_end"],
                    "value": row["current_assets"],
                    "available_at": row["current_asset_available_at"],
                },
                "prior": {
                    "statement_point_id": row["prior_asset_id"],
                    "accession_number": row["prior_accession_number"],
                    "period_end": row["prior_period_end"],
                    "value": row["prior_assets"],
                    "available_at": row["prior_asset_available_at"],
                },
                "asset_growth": row["asset_growth"],
                "conservative_asset_growth": row["raw_value"],
            },
        }
    )


def compute_asset_growth_rows(
    inputs: pd.DataFrame,
    options: AssetGrowthOptions | None = None,
) -> pd.DataFrame:
    """Winsorize and standardize negative annual total-asset growth."""

    options = options or AssetGrowthOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "decision_available_at",
        "current_assets",
        "prior_assets",
        "current_asset_id",
        "prior_asset_id",
        "current_period_end",
        "prior_period_end",
        "current_asset_available_at",
        "prior_asset_available_at",
        "current_accession_number",
        "prior_accession_number",
        "universe_id",
        "universe_valid_from",
        "universe_valid_to",
        "universe_available_at",
        "universe_source",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Asset-growth inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    rows["current_assets"] = pd.to_numeric(rows["current_assets"], errors="coerce")
    rows["prior_assets"] = pd.to_numeric(rows["prior_assets"], errors="coerce")
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "current_assets",
            "prior_assets",
        ]
    )
    rows = rows[
        (rows["current_assets"] > 0)
        & (rows["prior_assets"] > 0)
        & rows["current_assets"].map(math.isfinite)
        & rows["prior_assets"].map(math.isfinite)
    ].copy()
    rows["asset_growth"] = (
        rows["current_assets"] - rows["prior_assets"]
    ) / rows["prior_assets"]
    rows = rows[rows["asset_growth"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = -rows["asset_growth"]
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
        ["metric:total_assets", f"universe:{options.universe_id}"]
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


def refresh_asset_growth_values(
    store: DuckDBStore,
    options: AssetGrowthOptions | None = None,
) -> int:
    """Materialize point-in-time conservative annual asset growth."""

    options = options or AssetGrowthOptions()
    store.initialize()
    rows = compute_asset_growth_rows(load_asset_growth_inputs(store, options), options)
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
            insert_frame(store, rows, "fundamental_factor_values", "asset_growth_insert")
    return len(rows)
