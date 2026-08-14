"""Point-in-time Lev-Nissim tax-to-book income fundamental."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT tax-to-book income v1"
FACTOR_ID = "earnings_tax_to_book_income"
FACTOR_NAME = "PIT tax-to-book income ratio"


@dataclass(frozen=True)
class TaxToBookIncomeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_reporting_age_days: int = 550
    maximum_raw_value: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_tax_to_book_income_values(
    store: DuckDBStore,
    options: TaxToBookIncomeOptions | None = None,
) -> int:
    """Materialize after-tax estimated taxable income divided by book income."""

    options = options or TaxToBookIncomeOptions()
    store.initialize()
    parent_predicates = ["factor_id=?", "is_latest_revision"]
    parent_params: list[object] = [ASSET_GROWTH_FACTOR_ID]
    delete_predicates = ["factor_id=?", "source=?"]
    delete_params: list[object] = [FACTOR_ID, options.source]
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
        [f"factor:{ASSET_GROWTH_FACTOR_ID}", "metric:current_tax", "metric:net_income"]
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
        annual_tax AS (
            SELECT security_id,accession_number,period_start,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS current_tax,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       AS current_tax_id,
                   max(available_at) AS current_tax_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='current_tax' AND unit='USD'
              AND period_type='duration' AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 329 AND 399
              AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND isfinite(value)
            GROUP BY security_id,accession_number,period_start,period_end
        ),
        annual_book_income AS (
            SELECT security_id,accession_number,period_start,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS net_income,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       AS net_income_id,
                   max(available_at) AS net_income_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='net_income' AND unit='USD'
              AND period_type='duration' AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 329 AND 399
              AND accession_number IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
              AND isfinite(value)
            GROUP BY security_id,accession_number,period_start,period_end
        ),
        candidates AS (
            SELECT p.*,t.accession_number,t.period_start,t.period_end,
                   t.current_tax,t.current_tax_id,t.current_tax_available_at,
                   b.net_income,b.net_income_id,b.net_income_available_at,
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date
                       ORDER BY t.period_end DESC,
                                greatest(t.current_tax_available_at,
                                         b.net_income_available_at) DESC,
                                t.accession_number DESC
                   ) AS candidate_rank
            FROM parent p
            JOIN annual_tax t ON t.security_id=p.security_id
             AND t.period_end<=p.as_of_date
             AND t.current_tax_available_at<=p.decision_available_at
             AND p.as_of_date-t.period_end<=?
            JOIN annual_book_income b ON b.security_id=t.security_id
             AND b.accession_number=t.accession_number
             AND b.period_start=t.period_start AND b.period_end=t.period_end
             AND b.net_income_available_at<=p.decision_available_at
        ),
        measured AS (
            SELECT * EXCLUDE (candidate_rank),
                   CASE WHEN period_end<DATE '2018-01-01' THEN 0.35 ELSE 0.21 END
                       AS statutory_rate
            FROM candidates
            WHERE candidate_rank=1 AND current_tax>0 AND net_income>0
        ),
        raw AS (
            SELECT *,current_tax*(1-statutory_rate)/statutory_rate
                       AS estimated_after_tax_taxable_income,
                   (current_tax*(1-statutory_rate)/statutory_rate)/net_income
                       AS raw_value
            FROM measured
            WHERE (current_tax*(1-statutory_rate)/statutory_rate)/net_income<=?
              AND isfinite((current_tax*(1-statutory_rate)/statutory_rate)/net_income)
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
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_earnings',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'lev_nissim_tax_to_book_income_pit',
                formula := '(current_tax/statutory_rate)*(1-statutory_rate)/net_income',
                orientation := 'higher_tax_to_book_income_is_preferred',
                statutory_rate_policy := '35pct_through_2017_then_21pct',
                statutory_rate := statutory_rate,
                positive_current_tax_required := true,positive_net_income_required := true,
                maximum_reporting_age_days := ?,maximum_raw_value := ?,
                missing_components_imputed := false,return_fitted_parameters := false,
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                accession_number := accession_number,period_start := period_start,
                period_end := period_end,current_tax := current_tax,
                current_tax_id := current_tax_id,
                current_tax_available_at := current_tax_available_at,
                net_income := net_income,net_income_id := net_income_id,
                net_income_available_at := net_income_available_at,
                estimated_after_tax_taxable_income := estimated_after_tax_taxable_income,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,
        options.maximum_reporting_age_days,
        options.maximum_raw_value,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
        options.maximum_reporting_age_days,
        options.maximum_raw_value,
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
        f"SELECT count(*) FROM fundamental_factor_values WHERE {' AND '.join(delete_predicates)}",
        delete_params,
    ).fetchone()
    return int(row[0])
