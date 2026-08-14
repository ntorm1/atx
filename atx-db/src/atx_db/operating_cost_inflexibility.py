"""Point-in-time five-year operating-cost inflexibility factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT operating cost inflexibility v1"
FACTOR_ID = "risk_operating_cost_inflexibility"
FACTOR_NAME = "PIT operating cost inflexibility"


@dataclass(frozen=True)
class OperatingCostInflexibilityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_reporting_age_days: int = 550
    maximum_history_days: int = 2200
    minimum_five_year_span_days: int = 1300
    maximum_five_year_span_days: int = 1650
    maximum_sdoc: float = 5.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_operating_cost_inflexibility_values(
    store: DuckDBStore,
    options: OperatingCostInflexibilityOptions | None = None,
) -> int:
    """Materialize negative five-year volatility of log annual operating costs."""

    options = options or OperatingCostInflexibilityOptions()
    store.initialize()
    parent_predicates = ["factor_id=?", "is_latest_revision"]
    parent_params: list[object] = [ASSET_GROWTH_FACTOR_ID]
    delete_predicates = ["factor_id=?", "source=?"]
    delete_params: list[object] = [FACTOR_ID,options.source]
    if options.start_date is not None:
        parent_predicates.append("as_of_date>=?")
        parent_params.append(options.start_date)
        delete_predicates.append("as_of_date>=?")
        delete_params.append(options.start_date)
    if options.end_date is not None:
        parent_predicates.append("as_of_date<=?")
        parent_params.append(options.end_date)
        delete_predicates.append("as_of_date<=?")
        delete_params.append(options.end_date)
    input_ids = json_dumps(
        [f"factor:{ASSET_GROWTH_FACTOR_ID}","metric:cogs","metric:sga"]
    )
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH parent AS (
            SELECT factor_value_id AS parent_factor_value_id,security_id,symbol,
                   as_of_date,available_at AS decision_available_at
            FROM fundamental_factor_values
            WHERE {' AND '.join(parent_predicates)}
            QUALIFY row_number() OVER (
                PARTITION BY security_id,as_of_date
                ORDER BY available_at DESC,factor_value_id DESC
            )=1
        ),
        annual_cogs AS (
            SELECT security_id,accession_number,period_start,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS cogs,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       AS cogs_id,
                   max(available_at) AS cogs_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='cogs' AND unit='USD'
              AND period_type='duration' AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 329 AND 399
              AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND value>=0 AND isfinite(value)
            GROUP BY security_id,accession_number,period_start,period_end
        ),
        annual_sga AS (
            SELECT security_id,accession_number,period_start,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS sga,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       AS sga_id,
                   max(available_at) AS sga_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='sga' AND unit='USD'
              AND period_type='duration' AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 329 AND 399
              AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND value>=0 AND isfinite(value)
            GROUP BY security_id,accession_number,period_start,period_end
        ),
        annual_cost AS (
            SELECT c.security_id,c.accession_number,c.period_start,c.period_end,
                   c.cogs,c.cogs_id,c.cogs_available_at,
                   s.sga,s.sga_id,s.sga_available_at,c.cogs+s.sga AS operating_cost,
                   greatest(c.cogs_available_at,s.sga_available_at) AS cost_available_at
            FROM annual_cogs c
            JOIN annual_sga s USING (security_id,accession_number,period_start,period_end)
            WHERE c.cogs+s.sga>0 AND isfinite(c.cogs+s.sga)
        ),
        visible_cost AS (
            SELECT p.*,a.* EXCLUDE (security_id),
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date,a.period_end
                       ORDER BY a.cost_available_at DESC,a.accession_number DESC
                   ) AS point_rank
            FROM parent p JOIN annual_cost a ON a.security_id=p.security_id
             AND a.period_end<=p.as_of_date AND p.as_of_date-a.period_end<=?
             AND a.cost_available_at<=p.decision_available_at
        ),
        as_of_costs AS (
            SELECT * EXCLUDE (point_rank),
                   row_number() OVER (
                       PARTITION BY security_id,as_of_date ORDER BY period_end DESC
                   ) AS history_rank
            FROM visible_cost WHERE point_rank=1
        ),
        summarized AS (
            SELECT parent_factor_value_id,security_id,symbol,as_of_date,
                   decision_available_at,count(*) AS observation_count,
                   min(period_end) AS first_period_end,max(period_end) AS last_period_end,
                   stddev_samp(ln(operating_cost)) AS cost_sdoc,
                   list(period_end ORDER BY period_end) AS cost_period_ends,
                   list(operating_cost ORDER BY period_end) AS operating_costs,
                   list(cogs ORDER BY period_end) AS cogs_values,
                   list(sga ORDER BY period_end) AS sga_values,
                   list(cogs_id ORDER BY period_end) AS cogs_ids,
                   list(sga_id ORDER BY period_end) AS sga_ids,
                   list(accession_number ORDER BY period_end) AS cost_accessions,
                   list(cost_available_at ORDER BY period_end) AS cost_available_ats
            FROM as_of_costs WHERE history_rank<=5
            GROUP BY parent_factor_value_id,security_id,symbol,as_of_date,
                     decision_available_at
            HAVING count(*)=5
        ),
        raw AS (
            SELECT *,-cost_sdoc AS raw_value
            FROM summarized
            WHERE as_of_date-last_period_end<=?
              AND last_period_end-first_period_end BETWEEN ? AND ?
              AND cost_sdoc<=? AND isfinite(cost_sdoc)
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date FROM raw
        ),
        bounded AS (
            SELECT *,quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date) AS lower_bound,
                   quantile_cont(raw_value,?) OVER (PARTITION BY as_of_date) AS upper_bound
            FROM breadth WHERE names_on_date>=?
        ),
        moments AS (
            SELECT *,greatest(lower_bound,least(upper_bound,raw_value)) AS winsorized_value
            FROM bounded
        ),
        standardized AS (
            SELECT *,avg(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_mean,
                   stddev_samp(winsorized_value) OVER (PARTITION BY as_of_date) AS cs_std
            FROM moments
        )
        SELECT
            sha256(concat_ws('|',?,'{FACTOR_ID}',security_id,cast(as_of_date AS VARCHAR))),
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_risk',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'taussig_five_year_operating_cost_sdoc_pit',
                formula := '-stddev_samp(ln(cogs+sga),five_annual_observations)',
                orientation := 'lower_cost_sdoc_inflexibility_is_preferred',
                observation_count := observation_count,
                maximum_reporting_age_days := ?,maximum_history_days := ?,
                minimum_five_year_span_days := ?,maximum_five_year_span_days := ?,
                maximum_sdoc := ?,missing_components_imputed := false,
                return_fitted_parameters := false,
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                first_period_end := first_period_end,last_period_end := last_period_end,
                cost_sdoc := cost_sdoc,cost_period_ends := cost_period_ends,
                operating_costs := operating_costs,cogs_values := cogs_values,
                sga_values := sga_values,cogs_ids := cogs_ids,sga_ids := sga_ids,
                cost_accessions := cost_accessions,
                cost_available_ats := cost_available_ats,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,
        options.maximum_history_days,
        options.maximum_reporting_age_days,
        options.minimum_five_year_span_days,
        options.maximum_five_year_span_days,
        options.maximum_sdoc,
        options.winsor_limit,
        1.0-options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
        options.maximum_reporting_age_days,
        options.maximum_history_days,
        options.minimum_five_year_span_days,
        options.maximum_five_year_span_days,
        options.maximum_sdoc,
        options.run_id,
        options.source,
    ]
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
            delete_params,
        )
        store.con.execute(sql,params)
    row = store.con.execute(
        f"SELECT count(*) FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
        delete_params,
    ).fetchone()
    return int(row[0])
