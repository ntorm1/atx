"""Point-in-time Richardson-Sloan-Soliman-Tuna total accruals factor."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .connection import DuckDBStore
from .net_operating_assets import FACTOR_ID as NET_OPERATING_ASSETS_FACTOR_ID
from .net_operating_assets import SOURCE_NAME as NET_OPERATING_ASSETS_SOURCE_NAME
from .warehouse import json_dumps

SOURCE_NAME = "atx-db PIT low RSST accruals v1"
FACTOR_ID = "quality_low_rsst_accruals"
FACTOR_NAME = "PIT low RSST total accruals"

_METRICS = (
    "total_assets",
    "total_liabilities",
    "cash_st_inv",
    "st_debt",
    "lt_debt",
)


@dataclass(frozen=True)
class RsstAccrualsOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_annual_gap_days: int = 300
    maximum_annual_gap_days: int = 430
    maximum_reporting_age_days: int = 550
    maximum_history_days: int = 1000
    maximum_absolute_accrual: float = 5.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def refresh_rsst_accruals_values(
    store: DuckDBStore,
    options: RsstAccrualsOptions | None = None,
) -> int:
    """Materialize negative annual change in comprehensive NOA over average assets."""

    options = options or RsstAccrualsOptions()
    store.initialize()
    parent_predicates = ["factor_id=?", "source=?", "is_latest_revision"]
    parent_params: list[object] = [
        NET_OPERATING_ASSETS_FACTOR_ID,
        NET_OPERATING_ASSETS_SOURCE_NAME,
    ]
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
        [
            f"factor:{NET_OPERATING_ASSETS_FACTOR_ID}",
            *(f"metric:{metric}" for metric in _METRICS),
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
        annual AS (
            SELECT security_id,accession_number,period_end,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_assets') AS total_assets,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_assets') AS total_assets_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_liabilities')
                       AS total_liabilities,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='total_liabilities')
                       AS total_liabilities_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='cash_st_inv') AS cash_st_inv,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='cash_st_inv') AS cash_st_inv_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='st_debt') AS st_debt,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='st_debt') AS st_debt_id,
                   arg_max(value,(available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='lt_debt') AS lt_debt,
                   arg_max(statement_point_id,
                           (available_at,revision_sequence,statement_point_id))
                       FILTER (WHERE canonical_metric='lt_debt') AS lt_debt_id,
                   max(available_at) AS annual_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric IN (
                    'total_assets','total_liabilities','cash_st_inv','st_debt','lt_debt'
                  )
              AND unit='USD' AND period_type='instant'
              AND accession_number IS NOT NULL AND period_end IS NOT NULL
              AND form IN ('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')
            GROUP BY security_id,accession_number,period_end
            HAVING total_assets>0 AND total_liabilities>=0 AND cash_st_inv>=0
               AND st_debt>=0 AND lt_debt>=0
               AND isfinite(total_assets) AND isfinite(total_liabilities)
               AND isfinite(cash_st_inv) AND isfinite(st_debt) AND isfinite(lt_debt)
        ),
        visible AS (
            SELECT p.*,a.* EXCLUDE (security_id),
                   a.total_assets-a.cash_st_inv-a.total_liabilities
                       +a.st_debt+a.lt_debt AS net_operating_assets,
                   row_number() OVER (
                       PARTITION BY p.security_id,p.as_of_date,a.period_end
                       ORDER BY a.annual_available_at DESC,a.accession_number DESC
                   ) AS amendment_rank
            FROM parent p JOIN annual a ON a.security_id=p.security_id
             AND a.period_end<=p.as_of_date
             AND p.as_of_date-a.period_end<=?
             AND a.annual_available_at<=p.decision_available_at
        ),
        ranked AS (
            SELECT * EXCLUDE (amendment_rank),
                   row_number() OVER (
                       PARTITION BY security_id,as_of_date ORDER BY period_end DESC
                   ) AS annual_rank
            FROM visible WHERE amendment_rank=1
        ),
        summarized AS (
            SELECT parent_factor_value_id,security_id,symbol,as_of_date,
                   decision_available_at,count(*) AS annual_observations,
                   max(period_end) FILTER (WHERE annual_rank=1) AS current_period_end,
                   max(period_end) FILTER (WHERE annual_rank=2) AS prior_period_end,
                   max(net_operating_assets) FILTER (WHERE annual_rank=1)
                       AS current_net_operating_assets,
                   max(net_operating_assets) FILTER (WHERE annual_rank=2)
                       AS prior_net_operating_assets,
                   max(total_assets) FILTER (WHERE annual_rank=1) AS current_assets,
                   max(total_assets) FILTER (WHERE annual_rank=2) AS prior_assets,
                   list(accession_number ORDER BY period_end DESC) AS accessions,
                   list(annual_available_at ORDER BY period_end DESC)
                       AS annual_available_ats,
                   list(total_assets ORDER BY period_end DESC) AS total_assets_values,
                   list(total_liabilities ORDER BY period_end DESC)
                       AS total_liabilities_values,
                   list(cash_st_inv ORDER BY period_end DESC) AS cash_st_inv_values,
                   list(st_debt ORDER BY period_end DESC) AS st_debt_values,
                   list(lt_debt ORDER BY period_end DESC) AS lt_debt_values,
                   list(total_assets_id ORDER BY period_end DESC) AS total_assets_ids,
                   list(total_liabilities_id ORDER BY period_end DESC)
                       AS total_liabilities_ids,
                   list(cash_st_inv_id ORDER BY period_end DESC) AS cash_st_inv_ids,
                   list(st_debt_id ORDER BY period_end DESC) AS st_debt_ids,
                   list(lt_debt_id ORDER BY period_end DESC) AS lt_debt_ids
            FROM ranked WHERE annual_rank<=2
            GROUP BY parent_factor_value_id,security_id,symbol,as_of_date,
                     decision_available_at
            HAVING count(*)=2
        ),
        measured AS (
            SELECT *,-(
                       (current_net_operating_assets-prior_net_operating_assets)
                       /((current_assets+prior_assets)/2.0)
                   ) AS raw_value
            FROM summarized
            WHERE as_of_date-current_period_end<=?
              AND current_period_end-prior_period_end BETWEEN ? AND ?
              AND current_assets>0 AND prior_assets>0
        ),
        eligible AS (
            SELECT * FROM measured
            WHERE abs(raw_value)<=? AND isfinite(raw_value)
        ),
        breadth AS (
            SELECT *,count(*) OVER (PARTITION BY as_of_date) AS names_on_date
            FROM eligible
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
            '{FACTOR_ID}','{FACTOR_NAME}','fundamental_quality',security_id,symbol,
            as_of_date,raw_value,(winsorized_value-cs_mean)/cs_std,
            decision_available_at,?,
            cast(to_json(struct_pack(
                method := 'rsst_change_in_net_operating_assets_pit',
                formula := '-delta(net_operating_assets)/average_total_assets',
                net_operating_assets_formula :=
                    'total_assets-cash_st_inv-total_liabilities+st_debt+lt_debt',
                orientation := 'lower_total_accruals_are_higher_quality',
                annual_observations := annual_observations,
                maximum_reporting_age_days := ?,maximum_history_days := ?,
                annual_gap_days := list_value(?,?),maximum_absolute_accrual := ?,
                missing_components_imputed := false,return_fitted_parameters := false,
                parent_factor_id := '{NET_OPERATING_ASSETS_FACTOR_ID}',
                parent_factor_value_id := parent_factor_value_id,
                current_period_end := current_period_end,prior_period_end := prior_period_end,
                current_net_operating_assets := current_net_operating_assets,
                prior_net_operating_assets := prior_net_operating_assets,
                current_assets := current_assets,prior_assets := prior_assets,
                accessions := accessions,annual_available_ats := annual_available_ats,
                total_assets_values := total_assets_values,
                total_liabilities_values := total_liabilities_values,
                cash_st_inv_values := cash_st_inv_values,
                st_debt_values := st_debt_values,lt_debt_values := lt_debt_values,
                total_assets_ids := total_assets_ids,
                total_liabilities_ids := total_liabilities_ids,
                cash_st_inv_ids := cash_st_inv_ids,st_debt_ids := st_debt_ids,
                lt_debt_ids := lt_debt_ids,decision_available_at := decision_available_at
            )) AS VARCHAR),true,?,?
        FROM standardized
        WHERE cs_std>0 AND isfinite((winsorized_value-cs_mean)/cs_std)
    """
    params = [
        *parent_params,
        options.maximum_history_days,
        options.maximum_reporting_age_days,
        options.minimum_annual_gap_days,
        options.maximum_annual_gap_days,
        options.maximum_absolute_accrual,
        options.winsor_limit,
        1.0 - options.winsor_limit,
        options.minimum_names_per_date,
        options.source,
        input_ids,
        options.maximum_reporting_age_days,
        options.maximum_history_days,
        options.minimum_annual_gap_days,
        options.maximum_annual_gap_days,
        options.maximum_absolute_accrual,
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
