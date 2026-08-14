"""Point-in-time quarterly tax-expense momentum factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .connection import DuckDBStore
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT tax expense momentum v1"
FACTOR_ID = "earnings_tax_expense_momentum"
FACTOR_NAME = "PIT quarterly tax expense momentum"


@dataclass(frozen=True)
class TaxExpenseMomentumOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_reporting_age_days: int = 200
    maximum_absolute_raw_value: float = 1.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_tax_expense_momentum_values(
    store: DuckDBStore,
    options: TaxExpenseMomentumOptions | None = None,
) -> int:
    """Materialize seasonally differenced quarterly tax expense over prior assets."""

    options = options or TaxExpenseMomentumOptions()
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
        [f"factor:{ASSET_GROWTH_FACTOR_ID}", "metric:income_tax", "metric:total_assets"]
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
        quarterly_tax AS (
            SELECT security_id,accession_number,period_start,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS tax_expense,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       AS tax_expense_id,
                   max(available_at) AS tax_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='income_tax' AND unit='USD'
              AND period_type='duration' AND period_start IS NOT NULL
              AND period_end-period_start BETWEEN 69 AND 114
              AND accession_number IS NOT NULL
              AND form IN ('10-Q','10-Q/A','10-QT','10-K','10-K/A','10-KT',
                           '20-F','20-F/A','40-F','40-F/A','6-K','6-K/A')
              AND isfinite(value)
            GROUP BY security_id,accession_number,period_start,period_end
        ),
        quarterly_assets AS (
            SELECT security_id,accession_number,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       AS total_assets,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       AS total_assets_id,
                   max(available_at) AS assets_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric='total_assets' AND unit='USD'
              AND period_type='instant' AND accession_number IS NOT NULL
              AND value>0 AND isfinite(value)
            GROUP BY security_id,accession_number,period_end
        ),
        current_candidates AS (
            SELECT p.*,q.accession_number AS current_accession_number,
                   q.period_start AS current_period_start,q.period_end AS current_period_end,
                   q.tax_expense AS current_tax,q.tax_expense_id AS current_tax_id,
                   q.tax_available_at AS current_tax_available_at,
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date
                       ORDER BY q.period_end DESC,q.tax_available_at DESC,q.accession_number DESC
                   ) AS current_rank
            FROM parent p JOIN quarterly_tax q ON q.security_id=p.security_id
             AND q.period_end<=p.as_of_date AND q.tax_available_at<=p.decision_available_at
             AND p.as_of_date-q.period_end<=?
        ),
        current_rows AS (
            SELECT * EXCLUDE (current_rank) FROM current_candidates WHERE current_rank=1
        ),
        prior_candidates AS (
            SELECT c.*,q.accession_number AS prior_accession_number,
                   q.period_start AS prior_period_start,q.period_end AS prior_period_end,
                   q.tax_expense AS prior_tax,q.tax_expense_id AS prior_tax_id,
                   q.tax_available_at AS prior_tax_available_at,
                   a.total_assets AS prior_assets,a.total_assets_id AS prior_asset_id,
                   a.assets_available_at AS prior_asset_available_at,
                   row_number() OVER (
                       PARTITION BY c.security_id,c.as_of_date
                       ORDER BY abs((c.current_period_end-q.period_end)-365),q.period_end DESC,
                                q.tax_available_at DESC,q.accession_number DESC
                   ) AS prior_rank
            FROM current_rows c
            JOIN quarterly_tax q ON q.security_id=c.security_id
             AND q.period_end<c.current_period_end
             AND c.current_period_end-q.period_end BETWEEN 300 AND 430
             AND q.tax_available_at<=c.decision_available_at
            JOIN quarterly_assets a ON a.security_id=q.security_id
             AND a.accession_number=q.accession_number AND a.period_end=q.period_end
             AND a.assets_available_at<=c.decision_available_at
        ),
        raw AS (
            SELECT *,current_tax-prior_tax AS tax_expense_surprise,
                   (current_tax-prior_tax)/prior_assets AS raw_value
            FROM prior_candidates
            WHERE prior_rank=1 AND prior_assets>0 AND isfinite(prior_assets)
              AND current_tax<>prior_tax
              AND abs((current_tax-prior_tax)/prior_assets)<=?
              AND isfinite((current_tax-prior_tax)/prior_assets)
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
                method := 'thomas_zhang_tax_expense_momentum_pit',
                formula := '(income_tax_q-income_tax_q_minus_4)/assets_q_minus_4',
                orientation := 'higher_tax_expense_surprise_is_preferred',
                zero_surprises_excluded := true,
                maximum_reporting_age_days := ?,maximum_absolute_raw_value := ?,
                missing_components_imputed := false,return_fitted_parameters := false,
                parent_factor_id := '{ASSET_GROWTH_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                current_accession_number := current_accession_number,
                current_period_start := current_period_start,current_period_end := current_period_end,
                current_tax := current_tax,current_tax_id := current_tax_id,
                current_tax_available_at := current_tax_available_at,
                prior_accession_number := prior_accession_number,
                prior_period_start := prior_period_start,prior_period_end := prior_period_end,
                prior_tax := prior_tax,prior_tax_id := prior_tax_id,
                prior_tax_available_at := prior_tax_available_at,
                prior_assets := prior_assets,prior_asset_id := prior_asset_id,
                prior_asset_available_at := prior_asset_available_at,
                tax_expense_surprise := tax_expense_surprise,
                decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,
        options.maximum_reporting_age_days,
        options.maximum_absolute_raw_value,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
        options.maximum_reporting_age_days,
        options.maximum_absolute_raw_value,
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
