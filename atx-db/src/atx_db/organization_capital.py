"""Point-in-time organization-capital-to-assets factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT organization capital v1"
FACTOR_ID = "intangibles_high_organization_capital"
FACTOR_NAME = "PIT organization capital to assets"


@dataclass(frozen=True)
class OrganizationCapitalOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    depreciation_rate: float = 0.15
    initial_growth_rate: float = 0.10
    minimum_history_observations: int = 5
    minimum_annual_gap_days: int = 300
    maximum_annual_gap_days: int = 430
    maximum_reporting_age_days: int = 550
    maximum_history_days: int = 8000
    maximum_raw_value: float = 20.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_organization_capital_values(
    store: DuckDBStore,
    options: OrganizationCapitalOptions | None = None,
) -> int:
    """Materialize CPI-adjusted capitalized SG&A divided by current real assets."""

    options = options or OrganizationCapitalOptions()
    if not 0.0 < options.depreciation_rate < 1.0:
        raise ValueError("depreciation_rate must be in (0,1)")
    if options.initial_growth_rate < 0.0:
        raise ValueError("initial_growth_rate must be nonnegative")
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
            "metric:sga",
            "metric:total_assets",
            "macro:CPIAUCSL",
        ]
    )
    sql = f"""
        INSERT INTO fundamental_factor_values (
            factor_value_id,factor_id,factor_name,family,security_id,symbol,
            as_of_date,raw_value,value,available_at,input_ids_json,
            input_lineage_json,is_latest_revision,run_id,source
        )
        WITH settings AS (
            SELECT ?::DOUBLE AS depreciation_rate,?::DOUBLE AS initial_growth_rate
        ),
        parent AS (
            SELECT factor_value_id AS parent_factor_value_id,security_id,symbol,
                   as_of_date,available_at AS decision_available_at
            FROM fundamental_factor_values
            WHERE {' AND '.join(parent_predicates)}
            QUALIFY row_number() OVER (
                PARTITION BY security_id,as_of_date
                ORDER BY available_at DESC,factor_value_id DESC
            )=1
        ),
        annual_sga AS (
            SELECT security_id,accession_number,period_start,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS sga,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id)) AS sga_id,
                   max(available_at) AS sga_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='sga' AND unit='USD' AND period_type='duration'
              AND period_start IS NOT NULL AND period_end-period_start BETWEEN 329 AND 399
              AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND value>0 AND isfinite(value)
            GROUP BY security_id,accession_number,period_start,period_end
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
            SELECT s.*,a.total_assets,a.assets_id,a.assets_available_at,
                   greatest(s.sga_available_at,a.assets_available_at)
                       AS statement_available_at
            FROM annual_sga s JOIN annual_assets a
              USING (security_id,accession_number,period_end)
        ),
        annual_real AS (
            SELECT a.*,c.observation_date AS cpi_observation_date,
                   c.available_at AS cpi_available_at,c.value AS cpi_value
            FROM annual_statement a
            JOIN LATERAL (
                SELECT observation_date,available_at,value
                FROM macro_observations
                WHERE series_id='CPIAUCSL' AND observation_date<=a.period_end
                  AND value>0 AND isfinite(value)
                ORDER BY observation_date DESC,as_of_date DESC,available_at DESC
                LIMIT 1
            ) c ON true
        ),
        visible_history AS (
            SELECT p.*,a.* EXCLUDE (security_id),
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date,a.period_end
                       ORDER BY a.statement_available_at DESC,a.accession_number DESC
                   ) AS point_rank
            FROM parent p JOIN annual_real a ON a.security_id=p.security_id
             AND a.period_end<=p.as_of_date AND p.as_of_date-a.period_end<=?
             AND a.statement_available_at<=p.decision_available_at
             AND a.cpi_available_at<=p.decision_available_at
        ),
        as_of_history AS (
            SELECT * EXCLUDE (point_rank),
                   row_number() OVER (
                       PARTITION BY security_id,as_of_date ORDER BY period_end
                   ) AS history_index,
                   count(*) OVER (PARTITION BY security_id,as_of_date) AS history_count,
                   lag(period_end) OVER (
                       PARTITION BY security_id,as_of_date ORDER BY period_end
                   ) AS previous_period_end
            FROM visible_history WHERE point_rank=1
        ),
        capitalized AS (
            SELECT h.*,s.depreciation_rate,s.initial_growth_rate,
                   CASE WHEN history_index=1 THEN
                       power(1-s.depreciation_rate,history_count-1)
                       *(h.sga/h.cpi_value)/(s.initial_growth_rate+s.depreciation_rate)
                   ELSE power(1-s.depreciation_rate,history_count-history_index)
                       *(h.sga/h.cpi_value)
                   END AS capital_contribution
            FROM as_of_history h CROSS JOIN settings s
        ),
        summarized AS (
            SELECT parent_factor_value_id,security_id,symbol,as_of_date,
                   decision_available_at,depreciation_rate,initial_growth_rate,
                   count(*) AS history_observations,min(period_end) AS first_period_end,
                   max(period_end) AS last_period_end,sum(capital_contribution)
                       AS organization_capital_real,
                   arg_max(total_assets,period_end) AS current_total_assets,
                   arg_max(assets_id,period_end) AS current_assets_id,
                   arg_max(cpi_value,period_end) AS current_cpi_value,
                   list(period_end ORDER BY period_end) AS sga_period_ends,
                   list(sga ORDER BY period_end) AS sga_values,
                   list(sga_id ORDER BY period_end) AS sga_ids,
                   list(cpi_value ORDER BY period_end) AS cpi_values,
                   list(cpi_observation_date ORDER BY period_end) AS cpi_observation_dates,
                   list(statement_available_at ORDER BY period_end)
                       AS statement_available_ats,
                   min(period_end-previous_period_end)
                       FILTER (WHERE previous_period_end IS NOT NULL) AS minimum_gap_days,
                   max(period_end-previous_period_end)
                       FILTER (WHERE previous_period_end IS NOT NULL) AS maximum_gap_days
            FROM capitalized
            GROUP BY parent_factor_value_id,security_id,symbol,as_of_date,
                     decision_available_at,depreciation_rate,initial_growth_rate
            HAVING count(*)>=? AND minimum_gap_days>=? AND maximum_gap_days<=?
        ),
        raw AS (
            SELECT *,current_total_assets/current_cpi_value AS current_total_assets_real,
                   organization_capital_real/(current_total_assets/current_cpi_value)
                       AS raw_value
            FROM summarized
            WHERE as_of_date-last_period_end<=?
              AND organization_capital_real/(current_total_assets/current_cpi_value)<=?
              AND isfinite(
                  organization_capital_real/(current_total_assets/current_cpi_value)
              )
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
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_intangibles',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'eisfeldt_papanikolaou_organization_capital_pit',
                formula := 'OC_t=(1-delta)*OC_t_1+SGA_t/CPI_t; score=OC_real/assets_real',
                orientation := 'higher_organization_capital_to_assets_is_preferred',
                depreciation_rate := depreciation_rate,
                initial_growth_rate := initial_growth_rate,
                history_observations := history_observations,
                minimum_history_observations := ?,minimum_annual_gap_days := ?,
                maximum_annual_gap_days := ?,maximum_reporting_age_days := ?,
                maximum_history_days := ?,maximum_raw_value := ?,
                missing_sga_imputed_as_zero := false,return_fitted_parameters := false,
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                first_period_end := first_period_end,last_period_end := last_period_end,
                organization_capital_real := organization_capital_real,
                current_total_assets := current_total_assets,
                current_total_assets_real := current_total_assets_real,
                current_assets_id := current_assets_id,current_cpi_value := current_cpi_value,
                sga_period_ends := sga_period_ends,sga_values := sga_values,
                sga_ids := sga_ids,cpi_values := cpi_values,
                cpi_observation_dates := cpi_observation_dates,
                statement_available_ats := statement_available_ats,
                minimum_gap_days := minimum_gap_days,maximum_gap_days := maximum_gap_days,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        options.depreciation_rate,options.initial_growth_rate,
        *parent_params,
        options.maximum_history_days,
        options.minimum_history_observations,
        options.minimum_annual_gap_days,
        options.maximum_annual_gap_days,
        options.maximum_reporting_age_days,
        options.maximum_raw_value,
        options.winsor_limit,1.0-options.winsor_limit,
        options.minimum_names_per_date,
        options.source,input_ids,
        options.minimum_history_observations,
        options.minimum_annual_gap_days,
        options.maximum_annual_gap_days,
        options.maximum_reporting_age_days,
        options.maximum_history_days,
        options.maximum_raw_value,
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
