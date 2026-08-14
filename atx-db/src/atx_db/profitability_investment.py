"""Point-in-time joint cash-profitability and conservative-investment factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .asset_growth import SOURCE_NAME as ASSET_GROWTH_SOURCE_NAME
from .cash_profitability import SOURCE_NAME as CASH_PROFITABILITY_SOURCE_NAME
from .connection import DuckDBStore
from .warehouse import json_dumps

CASH_PROFITABILITY_FACTOR_ID = "profitability_cash_operating_profitability"
SOURCE_NAME = "atx-db PIT profitability-investment composite v1"
FACTOR_ID = "composite_cash_profitability_conservative_investment"
FACTOR_NAME = "PIT cash profitability and conservative investment"


@dataclass(frozen=True)
class ProfitabilityInvestmentOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    cash_profitability_weight: float = 0.5
    conservative_investment_weight: float = 0.5
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_profitability_investment_values(
    store: DuckDBStore,
    options: ProfitabilityInvestmentOptions | None = None,
) -> int:
    """Materialize the frozen equal-weight profitability-investment composite."""

    options = options or ProfitabilityInvestmentOptions()
    if options.cash_profitability_weight < 0 or options.conservative_investment_weight < 0:
        raise ValueError("profitability-investment weights must be non-negative")
    weight_sum = (
        options.cash_profitability_weight + options.conservative_investment_weight
    )
    if abs(weight_sum - 1.0) > 1e-12:
        raise ValueError("profitability-investment weights must sum to one")
    store.initialize()
    date_predicates: list[str] = []
    date_params: list[object] = []
    delete_predicates = ["factor_id=?", "source=?"]
    delete_params: list[object] = [FACTOR_ID, options.source]
    if options.start_date is not None:
        date_predicates.append("as_of_date>=?")
        date_params.append(options.start_date)
        delete_predicates.append("as_of_date>=?")
        delete_params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("as_of_date<=?")
        date_params.append(options.end_date)
        delete_predicates.append("as_of_date<=?")
        delete_params.append(options.end_date)
    date_sql = f" AND {' AND '.join(date_predicates)}" if date_predicates else ""
    input_ids = json_dumps(
        [
            f"factor:{CASH_PROFITABILITY_FACTOR_ID}",
            f"factor:{ASSET_GROWTH_FACTOR_ID}",
        ]
    )
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH cash AS (
            SELECT factor_value_id AS cash_factor_value_id,security_id,symbol,
                   as_of_date,value AS cash_profitability_value,
                   raw_value AS cash_profitability_raw_value,
                   available_at AS cash_profitability_available_at
            FROM fundamental_factor_values
            WHERE factor_id=? AND source=? AND is_latest_revision {date_sql}
            QUALIFY row_number() OVER (
                PARTITION BY security_id,as_of_date
                ORDER BY available_at DESC,factor_value_id DESC
            )=1
        ),
        investment AS (
            SELECT factor_value_id AS investment_factor_value_id,security_id,
                   as_of_date,value AS conservative_investment_value,
                   raw_value AS conservative_investment_raw_value,
                   available_at AS conservative_investment_available_at
            FROM fundamental_factor_values
            WHERE factor_id=? AND source=? AND is_latest_revision {date_sql}
            QUALIFY row_number() OVER (
                PARTITION BY security_id,as_of_date
                ORDER BY available_at DESC,factor_value_id DESC
            )=1
        ),
        combined AS (
            SELECT c.*,i.* EXCLUDE (security_id,as_of_date),
                   ?*cash_profitability_value+?*conservative_investment_value
                       AS raw_value,
                   greatest(cash_profitability_available_at,
                            conservative_investment_available_at) AS decision_available_at
            FROM cash c JOIN investment i USING (security_id,as_of_date)
            WHERE isfinite(cash_profitability_value)
              AND isfinite(conservative_investment_value)
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date
            FROM combined
        ),
        bounded AS (
            SELECT *,quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date)
                       AS lower_bound,
                   quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date)
                       AS upper_bound
            FROM breadth WHERE names_on_date>=?
        ),
        moments AS (
            SELECT *,greatest(lower_bound,least(upper_bound,raw_value))
                       AS winsorized_value
            FROM bounded
        ),
        standardized AS (
            SELECT *,avg(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_mean,
                   stddev_samp(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_std
            FROM moments
        )
        SELECT
            sha256(concat_ws('|',?,'{FACTOR_ID}',security_id,cast(as_of_date AS VARCHAR))),
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_composite',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,
            decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'equal_weight_cash_profitability_conservative_investment_pit',
                formula := '0.5*z_cash_profitability+0.5*z_conservative_asset_growth',
                orientation := 'higher_profitability_and_lower_investment_are_preferred',
                cash_profitability_weight := ?,conservative_investment_weight := ?,
                weights_fitted_to_returns := false,missing_components_imputed := false,
                cash_profitability_factor_id := '{CASH_PROFITABILITY_FACTOR_ID}',
                cash_profitability_factor_value_id := cash_factor_value_id,
                cash_profitability_source := '{CASH_PROFITABILITY_SOURCE_NAME}',
                cash_profitability_raw_value := cash_profitability_raw_value,
                cash_profitability_value := cash_profitability_value,
                cash_profitability_available_at := cash_profitability_available_at,
                conservative_investment_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                conservative_investment_factor_value_id := investment_factor_value_id,
                conservative_investment_source := '{ASSET_GROWTH_SOURCE_NAME}',
                conservative_investment_raw_value := conservative_investment_raw_value,
                conservative_investment_value := conservative_investment_value,
                conservative_investment_available_at :=
                    conservative_investment_available_at,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        CASH_PROFITABILITY_FACTOR_ID,
        CASH_PROFITABILITY_SOURCE_NAME,
        *date_params,
        ASSET_GROWTH_FACTOR_ID,
        ASSET_GROWTH_SOURCE_NAME,
        *date_params,
        options.cash_profitability_weight,
        options.conservative_investment_weight,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
        options.cash_profitability_weight,
        options.conservative_investment_weight,
        options.run_id,
        options.source,
    ]
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
            delete_params,
        )
        store.con.execute(sql, params)
    row = store.con.execute(
        f"SELECT count(*) FROM fundamental_factor_values "
        f"WHERE {' AND '.join(delete_predicates)}",
        delete_params,
    ).fetchone()
    return int(row[0])
