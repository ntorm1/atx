"""Point-in-time financing-side net operating assets sustainability factor."""

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

SOURCE_NAME = "atx-db PIT financing-side net operating assets v1"
FACTOR_ID = "quality_net_operating_assets"
FACTOR_NAME = "PIT low net operating assets"
FACTOR_FAMILY = "fundamental_quality"
_METRICS = (
    "total_assets",
    "total_liabilities",
    "cash_st_inv",
    "st_debt",
    "lt_debt",
)
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
class NetOperatingAssetsOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_fundamental_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _date_filter(options: NetOperatingAssetsOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_net_operating_assets_inputs(
    store: DuckDBStore,
    options: NetOperatingAssetsOptions | None = None,
) -> pd.DataFrame:
    """Resolve same-accession NOA components and prior assets visible monthly."""

    options = options or NetOperatingAssetsOptions()
    date_sql, date_params = _date_filter(options)
    metric_selects: list[str] = []
    for metric in _METRICS:
        metric_selects.extend(
            [
                f"arg_max(value,(available_at,revision_sequence,statement_point_id)) "
                f"FILTER (WHERE canonical_metric='{metric}') AS {metric}",
                f"arg_max(statement_point_id,(available_at,revision_sequence,statement_point_id)) "
                f"FILTER (WHERE canonical_metric='{metric}') AS {metric}_id",
                f"max(available_at) FILTER (WHERE canonical_metric='{metric}') "
                f"AS {metric}_available_at",
            ]
        )
    select_sql = ",\n                ".join(metric_selects)
    metric_sql = ",".join(f"'{metric}'" for metric in _METRICS)
    sql = f"""
        WITH annual_points AS (
            SELECT
                security_id,any_value(symbol) AS fundamental_symbol,
                accession_number,period_end,
                {select_sql}
            FROM fundamental_statement_points
            WHERE canonical_metric IN ({metric_sql})
              AND unit='USD' AND period_type='instant'
              AND period_end IS NOT NULL AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
            GROUP BY security_id,accession_number,period_end
        ),
        complete_current AS (
            SELECT *,
                total_assets-cash_st_inv-total_liabilities+st_debt+lt_debt
                    AS net_operating_assets,
                greatest(
                    total_assets_available_at,total_liabilities_available_at,
                    cash_st_inv_available_at,st_debt_available_at,lt_debt_available_at
                ) AS annual_available_at
            FROM annual_points
            WHERE total_assets>0 AND total_liabilities>=0 AND cash_st_inv>=0
              AND st_debt>=0 AND lt_debt>=0
              AND isfinite(total_assets) AND isfinite(total_liabilities)
              AND isfinite(cash_st_inv) AND isfinite(st_debt) AND isfinite(lt_debt)
        ),
        price_dedup AS (
            SELECT security_id,any_value(symbol) AS symbol,trade_date,
                   max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT DISTINCT security_id FROM complete_current)
              AND close>0 AND trade_date IS NOT NULL AND available_at IS NOT NULL
            GROUP BY security_id,trade_date
        ),
        price_months AS (
            SELECT *,row_number() OVER (
                PARTITION BY security_id,year(trade_date),month(trade_date)
                ORDER BY trade_date DESC
            ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT * FROM price_months WHERE month_rank=1 {date_sql}
        ),
        governed AS (
            SELECT p.*,u.valid_from AS universe_valid_from,
                   u.valid_to AS universe_valid_to,u.universe_id,
                   u.available_at AS universe_available_at,u.source AS universe_source,
                   row_number() OVER (
                       PARTITION BY p.security_id,p.trade_date
                       ORDER BY u.valid_from DESC,u.available_at DESC NULLS LAST,
                                u.source_loaded_at DESC,u.source DESC
                   ) AS universe_rank
            FROM rebalances p
            JOIN universe_membership u
              ON u.universe_id=? AND u.security_id=p.security_id
             AND u.valid_from<=p.trade_date
             AND (u.valid_to IS NULL OR u.valid_to>=p.trade_date)
             AND u.as_of_date<=p.trade_date AND u.is_member AND u.is_latest_revision
             AND (u.available_at IS NULL OR u.available_at<=p.price_available_at)
        ),
        current_candidates AS (
            SELECT d.*,a.* EXCLUDE (security_id),row_number() OVER (
                PARTITION BY d.security_id,d.trade_date
                ORDER BY a.period_end DESC,a.annual_available_at DESC,a.accession_number DESC
            ) AS current_rank
            FROM governed d
            JOIN complete_current a ON a.security_id=d.security_id
             AND a.period_end<=d.trade_date
             AND a.annual_available_at<=d.price_available_at
             AND d.trade_date-a.period_end<=?
            WHERE d.universe_rank=1
        ),
        current_rows AS (
            SELECT * EXCLUDE (current_rank) FROM current_candidates WHERE current_rank=1
        ),
        prior_candidates AS (
            SELECT c.*,p.accession_number AS prior_accession_number,
                   p.period_end AS prior_period_end,p.total_assets AS prior_assets,
                   p.total_assets_id AS prior_asset_id,
                   p.total_assets_available_at AS prior_asset_available_at,
                   row_number() OVER (
                       PARTITION BY c.security_id,c.trade_date
                       ORDER BY abs((c.period_end-p.period_end)-365),p.period_end DESC,
                                p.total_assets_available_at DESC,p.accession_number DESC
                   ) AS prior_rank
            FROM current_rows c
            JOIN annual_points p ON p.security_id=c.security_id
             AND p.period_end<c.period_end
             AND c.period_end-p.period_end BETWEEN 300 AND 430
             AND p.total_assets>0 AND isfinite(p.total_assets)
             AND p.total_assets_available_at<=c.price_available_at
        )
        SELECT * EXCLUDE (month_rank,universe_rank,prior_rank),
               greatest(
                   price_available_at,universe_available_at,
                   annual_available_at,prior_asset_available_at
               ) AS decision_available_at
        FROM prior_candidates WHERE prior_rank=1
        ORDER BY trade_date,security_id
    """
    return store.con.execute(
        sql,
        [*date_params, options.universe_id, options.maximum_fundamental_age_days],
    ).df()


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _metric(row: pd.Series, metric: str) -> dict[str, object]:
    return {
        "statement_point_id": row[f"{metric}_id"],
        "value": row[metric],
        "available_at": row[f"{metric}_available_at"],
    }


def _lineage(row: pd.Series, options: NetOperatingAssetsOptions) -> str:
    return json_dumps(
        {
            "method": "hirshleifer_financing_side_noa_pit",
            "formula": (
                "-((total_assets-cash_st_inv-total_liabilities+st_debt+lt_debt)"
                "/prior_total_assets)"
            ),
            "orientation": "lower_net_operating_assets_is_preferred",
            "short_term_debt_policy": (
                "warehouse canonical st_debt uses prioritized current-debt concepts; "
                "missing components are not imputed"
            ),
            "research_contract": {
                "annual_period_gap_days": [300, 430],
                "maximum_fundamental_age_days": options.maximum_fundamental_age_days,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "missing_components_imputed": False,
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
            "current_statement": {
                "accession_number": row["accession_number"],
                "period_end": row["period_end"],
                **{metric: _metric(row, metric) for metric in _METRICS},
                "net_operating_assets": row["net_operating_assets"],
            },
            "prior_assets": {
                "statement_point_id": row["prior_asset_id"],
                "accession_number": row["prior_accession_number"],
                "period_end": row["prior_period_end"],
                "value": row["prior_assets"],
                "available_at": row["prior_asset_available_at"],
            },
            "normalized_noa": -row["raw_value"],
        }
    )


def compute_net_operating_assets_rows(
    inputs: pd.DataFrame,
    options: NetOperatingAssetsOptions | None = None,
) -> pd.DataFrame:
    """Compute, winsorize, and standardize low-NOA scores."""

    options = options or NetOperatingAssetsOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "decision_available_at",
        "accession_number",
        "period_end",
        "prior_accession_number",
        "prior_period_end",
        "prior_assets",
        "prior_asset_id",
        "prior_asset_available_at",
        "universe_id",
        "universe_valid_from",
        "universe_valid_to",
        "universe_available_at",
        "universe_source",
    }
    for metric in _METRICS:
        required.update({metric, f"{metric}_id", f"{metric}_available_at"})
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"net operating assets inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    numeric = (*_METRICS, "prior_assets")
    for column in numeric:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", *numeric])
    valid = rows["total_assets"].gt(0) & rows["prior_assets"].gt(0)
    for column in ("total_liabilities", "cash_st_inv", "st_debt", "lt_debt"):
        valid &= rows[column].ge(0)
    for column in numeric:
        valid &= rows[column].map(math.isfinite)
    rows = rows[valid].copy()
    rows["net_operating_assets"] = (
        rows["total_assets"]
        - rows["cash_st_inv"]
        - rows["total_liabilities"]
        + rows["st_debt"]
        + rows["lt_debt"]
    )
    rows["raw_value"] = -(rows["net_operating_assets"] / rows["prior_assets"])
    rows = rows[rows["raw_value"].map(math.isfinite)].copy()
    breadth = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[breadth >= options.minimum_names_per_date].copy()
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
        [*(f"metric:{metric}" for metric in _METRICS), f"universe:{options.universe_id}"]
    )
    rows["input_lineage_json"] = rows.apply(lambda row: _lineage(row, options), axis=1)
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


def refresh_net_operating_assets_values(
    store: DuckDBStore,
    options: NetOperatingAssetsOptions | None = None,
) -> int:
    """Materialize the governed low-NOA factor partition."""

    options = options or NetOperatingAssetsOptions()
    store.initialize()
    rows = compute_net_operating_assets_rows(
        load_net_operating_assets_inputs(store, options), options
    )
    predicates = ["factor_id=?", "source=?"]
    params: list[object] = [FACTOR_ID, options.source]
    if options.start_date is not None:
        predicates.append("as_of_date>=?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date<=?")
        params.append(options.end_date)
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
            params,
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "net_operating_assets_insert")
    return len(rows)
