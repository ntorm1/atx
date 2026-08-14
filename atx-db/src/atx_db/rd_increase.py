"""Point-in-time Eberhart large R&D-increase event factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT large R&D increase v1"
FACTOR_ID = "intangibles_large_rd_increase"
FACTOR_NAME = "PIT large R&D increase event"


@dataclass(frozen=True)
class RdIncreaseOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    intensity_threshold: float = 0.05
    growth_threshold: float = 0.05
    minimum_annual_gap_days: int = 300
    maximum_annual_gap_days: int = 430
    maximum_reporting_age_days: int = 550
    maximum_history_days: int = 1300
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_rd_increase_values(
    store: DuckDBStore,
    options: RdIncreaseOptions | None = None,
) -> int:
    """Materialize the five-criterion large R&D-increase indicator."""

    options = options or RdIncreaseOptions()
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
        [
            f"factor:{ASSET_GROWTH_FACTOR_ID}",
            "metric:rd_expense","metric:revenue","metric:total_assets",
        ]
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
        annual_flows AS (
            SELECT security_id,accession_number,period_start,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='rd_expense') AS rd_expense,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='rd_expense') AS rd_expense_id,
                   max(available_at) FILTER (WHERE canonical_metric='rd_expense')
                       AS rd_available_at,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='revenue') AS revenue,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='revenue') AS revenue_id,
                   max(available_at) FILTER (WHERE canonical_metric='revenue')
                       AS revenue_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric IN ('rd_expense','revenue') AND unit='USD'
              AND period_type='duration' AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 329 AND 399
              AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND value>0 AND isfinite(value)
            GROUP BY security_id,accession_number,period_start,period_end
            HAVING rd_expense>0 AND revenue>0
        ),
        annual_assets AS (
            SELECT security_id,accession_number,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS total_assets,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id)) AS assets_id,
                   max(available_at) AS assets_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='total_assets' AND unit='USD' AND period_type='instant'
              AND accession_number IS NOT NULL AND value>0 AND isfinite(value)
            GROUP BY security_id,accession_number,period_end
        ),
        annual_statement AS (
            SELECT f.*,a.total_assets,a.assets_id,a.assets_available_at,
                   greatest(f.rd_available_at,f.revenue_available_at,a.assets_available_at)
                       AS statement_available_at
            FROM annual_flows f JOIN annual_assets a
              USING (security_id,accession_number,period_end)
        ),
        visible_history AS (
            SELECT p.*,a.* EXCLUDE (security_id),
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date,a.period_end
                       ORDER BY a.statement_available_at DESC,a.accession_number DESC
                   ) AS point_rank
            FROM parent p JOIN annual_statement a ON a.security_id=p.security_id
             AND a.period_end<=p.as_of_date AND p.as_of_date-a.period_end<=?
             AND a.statement_available_at<=p.decision_available_at
        ),
        ranked_history AS (
            SELECT * EXCLUDE (point_rank),
                   row_number() OVER (
                       PARTITION BY security_id,as_of_date ORDER BY period_end DESC
                   ) AS history_rank
            FROM visible_history WHERE point_rank=1
        ),
        summarized AS (
            SELECT parent_factor_value_id,security_id,symbol,as_of_date,
                   decision_available_at,count(*) AS history_observations,
                   max(period_end) FILTER (WHERE history_rank=1) AS current_period_end,
                   max(period_end) FILTER (WHERE history_rank=2) AS prior_period_end,
                   max(period_end) FILTER (WHERE history_rank=3) AS lag2_period_end,
                   max(rd_expense) FILTER (WHERE history_rank=1) AS current_rd,
                   max(rd_expense) FILTER (WHERE history_rank=2) AS prior_rd,
                   max(revenue) FILTER (WHERE history_rank=1) AS current_revenue,
                   max(revenue) FILTER (WHERE history_rank=2) AS prior_revenue,
                   max(total_assets) FILTER (WHERE history_rank=1) AS current_assets,
                   max(total_assets) FILTER (WHERE history_rank=2) AS prior_assets,
                   max(total_assets) FILTER (WHERE history_rank=3) AS lag2_assets,
                   list(rd_expense_id ORDER BY period_end DESC) AS rd_ids,
                   list(revenue_id ORDER BY period_end DESC) AS revenue_ids,
                   list(assets_id ORDER BY period_end DESC) AS assets_ids,
                   list(accession_number ORDER BY period_end DESC) AS accessions,
                   list(statement_available_at ORDER BY period_end DESC)
                       AS statement_available_ats
            FROM ranked_history WHERE history_rank<=3
            GROUP BY parent_factor_value_id,security_id,symbol,as_of_date,
                     decision_available_at
            HAVING count(*)=3
        ),
        measured AS (
            SELECT *,current_rd/current_revenue AS current_rd_to_sales,
                   prior_rd/prior_revenue AS prior_rd_to_sales,
                   current_rd/((current_assets+prior_assets)/2) AS current_rd_to_assets,
                   prior_rd/((prior_assets+lag2_assets)/2) AS prior_rd_to_assets,
                   current_rd/prior_rd-1 AS rd_growth,
                   (current_rd/current_revenue)/(prior_rd/prior_revenue)-1
                       AS rd_to_sales_growth,
                   (current_rd/((current_assets+prior_assets)/2))
                       /(prior_rd/((prior_assets+lag2_assets)/2))-1
                       AS rd_to_assets_growth
            FROM summarized
            WHERE as_of_date-current_period_end<=?
              AND current_period_end-prior_period_end BETWEEN ? AND ?
              AND prior_period_end-lag2_period_end BETWEEN ? AND ?
        ),
        scored AS (
            SELECT *,CASE WHEN current_rd_to_sales>?
                                AND current_rd_to_assets>?
                                AND rd_growth>?
                                AND rd_to_sales_growth>?
                                AND rd_to_assets_growth>?
                           THEN 1.0 ELSE 0.0 END AS raw_value
            FROM measured
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date FROM scored
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
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_intangibles',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'eberhart_large_rd_increase_event_pit',
                formula := 'five_criteria_on_rd_sales_and_average_assets',
                orientation := 'large_rd_increase_event_is_preferred',
                intensity_threshold := ?,growth_threshold := ?,
                maximum_reporting_age_days := ?,maximum_history_days := ?,
                annual_gap_days := list_value(?,?),missing_components_imputed := false,
                return_fitted_parameters := false,
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                current_period_end := current_period_end,prior_period_end := prior_period_end,
                lag2_period_end := lag2_period_end,current_rd := current_rd,
                prior_rd := prior_rd,current_revenue := current_revenue,
                prior_revenue := prior_revenue,current_assets := current_assets,
                prior_assets := prior_assets,lag2_assets := lag2_assets,
                current_rd_to_sales := current_rd_to_sales,
                prior_rd_to_sales := prior_rd_to_sales,
                current_rd_to_assets := current_rd_to_assets,
                prior_rd_to_assets := prior_rd_to_assets,rd_growth := rd_growth,
                rd_to_sales_growth := rd_to_sales_growth,
                rd_to_assets_growth := rd_to_assets_growth,
                event_qualified := raw_value=1.0,rd_ids := rd_ids,
                revenue_ids := revenue_ids,assets_ids := assets_ids,
                accessions := accessions,statement_available_ats := statement_available_ats,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,options.maximum_history_days,
        options.maximum_reporting_age_days,
        options.minimum_annual_gap_days,options.maximum_annual_gap_days,
        options.minimum_annual_gap_days,options.maximum_annual_gap_days,
        options.intensity_threshold,options.intensity_threshold,
        options.growth_threshold,options.growth_threshold,options.growth_threshold,
        options.winsor_limit,1.0-options.winsor_limit,options.minimum_names_per_date,
        options.source,input_ids,
        options.intensity_threshold,options.growth_threshold,
        options.maximum_reporting_age_days,options.maximum_history_days,
        options.minimum_annual_gap_days,options.maximum_annual_gap_days,
        options.run_id,options.source,
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
