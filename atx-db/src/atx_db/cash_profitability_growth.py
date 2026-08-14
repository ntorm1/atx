"""PIT cash-profitability level and first annual change composite."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .cash_profitability import SOURCE_NAME as CASH_PROFITABILITY_SOURCE_NAME
from .connection import DuckDBStore
from .warehouse import json_dumps

CASH_PROFITABILITY_FACTOR_ID = "profitability_cash_operating_profitability"
SOURCE_NAME = "atx-db PIT cash-profitability level-growth composite v1"
FACTOR_ID = "composite_cash_profitability_level_growth"
FACTOR_NAME = "PIT cash profitability level and growth"


@dataclass(frozen=True)
class CashProfitabilityGrowthOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    level_weight: float = 0.5
    growth_weight: float = 0.5
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_cash_profitability_growth_values(
    store: DuckDBStore,
    options: CashProfitabilityGrowthOptions | None = None,
) -> int:
    """Materialize a fixed blend of cash profitability and its first annual change."""

    options = options or CashProfitabilityGrowthOptions()
    if options.level_weight < 0 or options.growth_weight < 0:
        raise ValueError("cash-profitability level-growth weights must be non-negative")
    if abs(options.level_weight + options.growth_weight - 1.0) > 1e-12:
        raise ValueError("cash-profitability level-growth weights must sum to one")
    if not 0.0 <= options.winsor_limit < 0.5:
        raise ValueError("winsor_limit must be in [0, 0.5)")

    store.initialize()
    current_predicates: list[str] = []
    current_params: list[object] = []
    delete_predicates = ["factor_id=?", "source=?"]
    delete_params: list[object] = [FACTOR_ID, options.source]
    if options.start_date is not None:
        current_predicates.append("as_of_date>=?")
        current_params.append(options.start_date)
        delete_predicates.append("as_of_date>=?")
        delete_params.append(options.start_date)
    if options.end_date is not None:
        current_predicates.append("as_of_date<=?")
        current_params.append(options.end_date)
        delete_predicates.append("as_of_date<=?")
        delete_params.append(options.end_date)
    current_sql = (
        f" AND {' AND '.join(current_predicates)}" if current_predicates else ""
    )
    input_ids = json_dumps([f"factor:{CASH_PROFITABILITY_FACTOR_ID}"])
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH cash AS (
            SELECT factor_value_id,security_id,symbol,as_of_date,raw_value,value,
                   available_at,
                   try_cast(json_extract_string(
                       input_lineage_json,'$.current_statement.period_end'
                   ) AS DATE) AS period_end
            FROM fundamental_factor_values
            WHERE factor_id=? AND source=? AND is_latest_revision
        ),
        current_cash AS (
            SELECT * FROM cash WHERE period_end IS NOT NULL {current_sql}
        ),
        paired AS (
            SELECT c.*,p.factor_value_id AS prior_factor_value_id,
                   p.period_end AS prior_period_end,p.raw_value AS prior_raw_value,
                   p.available_at AS prior_available_at,
                   c.raw_value-p.raw_value AS growth_raw_value,
                   row_number() OVER (
                       PARTITION BY c.security_id,c.as_of_date
                       ORDER BY p.period_end DESC,p.as_of_date DESC,
                                p.available_at DESC,p.factor_value_id DESC
                   ) AS prior_rank
            FROM current_cash c
            JOIN cash p
              ON p.security_id=c.security_id
             AND p.period_end<c.period_end
             AND c.period_end-p.period_end BETWEEN 300 AND 430
             AND p.as_of_date<c.as_of_date
             AND p.available_at<=c.available_at
            WHERE isfinite(c.raw_value) AND isfinite(c.value) AND isfinite(p.raw_value)
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date
            FROM paired WHERE prior_rank=1
        ),
        growth_bounds AS (
            SELECT *,
                   quantile_cont(growth_raw_value,?) OVER (PARTITION BY as_of_date)
                       AS growth_lower,
                   quantile_cont(growth_raw_value,?) OVER (PARTITION BY as_of_date)
                       AS growth_upper
            FROM breadth WHERE names_on_date>=?
        ),
        growth_moments AS (
            SELECT *,greatest(growth_lower,least(growth_upper,growth_raw_value))
                       AS growth_winsorized,
                   avg(greatest(growth_lower,least(growth_upper,growth_raw_value)))
                       OVER (PARTITION BY as_of_date) AS growth_mean,
                   stddev_samp(greatest(
                       growth_lower,least(growth_upper,growth_raw_value)
                   )) OVER (PARTITION BY as_of_date) AS growth_std
            FROM growth_bounds
        ),
        blended AS (
            SELECT *,
                   (growth_winsorized-growth_mean)/growth_std AS growth_value,
                   ?*value+?*((growth_winsorized-growth_mean)/growth_std) AS blend_raw
            FROM growth_moments WHERE growth_std>0
        ),
        blend_bounds AS (
            SELECT *,quantile_cont(blend_raw,?) OVER (PARTITION BY as_of_date)
                       AS blend_lower,
                   quantile_cont(blend_raw,?) OVER (PARTITION BY as_of_date)
                       AS blend_upper
            FROM blended
        ),
        standardized AS (
            SELECT *,greatest(blend_lower,least(blend_upper,blend_raw))
                       AS blend_winsorized,
                   avg(greatest(blend_lower,least(blend_upper,blend_raw)))
                       OVER (PARTITION BY as_of_date) AS blend_mean,
                   stddev_samp(greatest(blend_lower,least(blend_upper,blend_raw)))
                       OVER (PARTITION BY as_of_date) AS blend_std
            FROM blend_bounds
        )
        SELECT
            sha256(concat_ws('|',?,'{FACTOR_ID}',security_id,cast(as_of_date AS VARCHAR))),
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_composite',security_id,symbol,
            as_of_date,blend_raw,(blend_winsorized-blend_mean)/blend_std,
            greatest(available_at,prior_available_at),?,
            cast(to_json(struct_pack(
                method := 'fixed_cash_profitability_level_first_annual_change_pit',
                formula := '0.5*z_cash_profitability+0.5*z_first_annual_change',
                level_weight := ?,growth_weight := ?,weights_fitted_to_returns := false,
                missing_components_imputed := false,
                cash_profitability_factor_id := '{CASH_PROFITABILITY_FACTOR_ID}',
                current_factor_value_id := factor_value_id,
                current_period_end := period_end,current_raw_value := raw_value,
                current_value := value,current_available_at := available_at,
                prior_factor_value_id := prior_factor_value_id,
                prior_period_end := prior_period_end,prior_raw_value := prior_raw_value,
                prior_available_at := prior_available_at,
                growth_raw_value := growth_raw_value,growth_value := growth_value,
                decision_available_at := greatest(available_at,prior_available_at)
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE blend_std>0 AND isfinite((blend_winsorized-blend_mean)/blend_std)
    """
    params = [
        CASH_PROFITABILITY_FACTOR_ID,
        CASH_PROFITABILITY_SOURCE_NAME,
        *current_params,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.level_weight,
        options.growth_weight,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.source,
        input_ids,
        options.level_weight,
        options.growth_weight,
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
